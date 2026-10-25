//! Compiles the C firmware of the node chosen by a cargo feature, and links it and esp-hal's
//! memory layout into the kernel.

use std::{
    collections::BTreeSet,
    env,
    error::Error,
    fs, io, iter,
    path::{Path, PathBuf},
    process::Command,
    time::SystemTime,
};

/// Each node's feature, `NODE_ID`, sensor configuration and `SENSOR_SERIAL_TEST`.
const NODES: [(&str, u8, &str, u8); 6] = [
    ("BRAKE", 1, "BRAKE", 0),
    ("MPU", 3, "MPU", 0),
    ("ENCODER", 4, "ENCODER", 0),
    ("ENGINE", 5, "ENGINE", 0),
    ("ENGINE_BENCH", 5, "ENGINE", 1),
    ("ADC", 6, "ADC", 0),
];
const CONFIGS: [&str; 5] = ["BRAKE", "ENCODER", "ENGINE", "ADC", "MPU"];

/// Every node is a C3. Frame pointers let a Rust panic's backtrace continue through the C callers.
/// Without atomic instructions every atomic is a call, so the alignment warning is noise.
const FLAGS: &str = "--target=riscv32-unknown-elf -march=rv32imc_zicsr_zifencei -mabi=ilp32 \
    -std=gnu17 -Oz -ggdb -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -ffunction-sections \
    -fdata-sections -Wall -Wno-atomic-alignment";

fn main() -> Result<(), Box<dyn Error>> {
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
    let nodes: Vec<_> = NODES
        .iter()
        .filter(|(feature, ..)| env::var_os(format!("CARGO_FEATURE_{feature}")).is_some())
        .collect();
    match nodes[..] {
        [] => {
            println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
            println!("cargo::rustc-link-arg-bins=-Tno_c.x");
        }
        [&(_, id, config, serial_test)] => {
            let mut defines = vec![
                "-DNODE_CAN_TX_GPIO=21".to_owned(),
                "-DNODE_CAN_RX_GPIO=20".to_owned(),
                format!("-DNODE_ID={id}"),
                format!("-DSENSOR_SERIAL_TEST={serial_test}"),
            ];
            for other in CONFIGS {
                let on = u8::from(other == config);
                defines.push(format!("-DNODE_FIXED_{other}_CONFIG={on}"));
            }
            compile(&defines)?;
        }
        _ => println!("cargo::error=enable one node feature, not several"),
    }
    Ok(())
}

fn compile(defines: &[String]) -> Result<(), Box<dyn Error>> {
    let Some(clang) = clang() else {
        let install = match env::consts::OS {
            "macos" => "brew install llvm",
            "windows" => "winget install LLVM.LLVM",
            _ => "apt install clang libclang-dev",
        };
        println!("cargo::error=no clang with RISC-V support found; install it: {install}");
        return Ok(());
    };
    let kernel = Path::new(env!("CARGO_MANIFEST_DIR"));
    let repo = kernel.parent().ok_or("kernel/ is in the repository")?;
    let src = repo.join("sensor-node/src");
    let out = PathBuf::from(env::var_os("OUT_DIR").ok_or("no OUT_DIR")?);
    let mut flags: Vec<_> = FLAGS.split_whitespace().map(String::from).collect();
    let idf = env::var_os("DEP_IDF_ARGS").ok_or("no DEP_IDF_ARGS")?;
    flags.extend(env::split_paths(&idf).map(|arg| arg.display().to_string()));
    flags.extend([
        format!("-I{}", src.display()),
        format!("-I{}", repo.display()),
        "-include".to_owned(),
        kernel.join("include/unsupported.h").display().to_string(),
    ]);
    flags.extend_from_slice(defines);

    let stamp = out.join("flags");
    let flags_changed = fs::read_to_string(&stamp).ok() != Some(format!("{clang} {flags:?}"));
    let mut files = Vec::new();
    c_files(&src, &mut files)?;
    files.sort();
    let mut headers = BTreeSet::new();
    let mut entries = Vec::new();
    for file in files {
        let object = out.join(file.strip_prefix(&src)?).with_extension("o");
        // -MD writes `rule` next to the object.
        let rule = object.with_extension("d");
        let [file_arg, object_arg] = [&file, &object].map(|path| path.display().to_string());
        let args = [
            &flags[..],
            &["-MD".into(), "-c".into(), file_arg, "-o".into(), object_arg],
        ]
        .concat();
        if flags_changed || stale(&object, &dependencies(&rule)) {
            fs::create_dir_all(object.parent().ok_or("an object has a folder")?)?;
            let output = Command::new(&clang).args(&args).output()?;
            let level = if output.status.success() {
                "warning"
            } else {
                "error"
            };
            for line in String::from_utf8_lossy(&output.stderr).lines() {
                println!("cargo::{level}={line}");
            }
        }
        headers.extend(dependencies(&rule));
        println!("cargo::rustc-link-arg-bins={}", object.display());
        // Debug quoting is JSON's for these strings.
        let arguments: Vec<_> = iter::once(&clang).chain(&args).collect();
        entries.push(format!(
            "{{\"directory\":{:?},\"file\":{:?},\"arguments\":{arguments:?}}}",
            kernel.display().to_string(),
            file.display().to_string(),
        ));
    }
    fs::write(&stamp, format!("{clang} {flags:?}"))?;
    println!("cargo::rerun-if-changed={}", src.display());
    for header in headers {
        println!("cargo::rerun-if-changed={}", header.display());
    }
    // In the target folder, where sensor-node/.clangd looks; OUT_DIR is
    // <target>/<triple>/<profile>/build/<package>/out.
    let target_dir = out
        .ancestors()
        .nth(5)
        .ok_or("OUT_DIR is in the target folder")?;
    let json = format!("[\n{}\n]\n", entries.join(",\n"));
    fs::write(target_dir.join("compile_commands.json"), json)?;
    Ok(())
}

/// Upstream clang: Apple's has no RISC-V backend, and Homebrew's LLVM isn't on the PATH.
fn clang() -> Option<String> {
    [
        "/opt/homebrew/opt/llvm/bin/clang",
        "/usr/local/opt/llvm/bin/clang",
        r"C:\Program Files\LLVM\bin\clang.exe",
        "clang",
    ]
    .into_iter()
    .find(|clang| {
        Command::new(clang)
            .arg("-print-targets")
            .output()
            .is_ok_and(|output| String::from_utf8_lossy(&output.stdout).contains("riscv32"))
    })
    .map(String::from)
}

fn c_files(dir: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            c_files(&path, files)?;
        } else if path.extension().is_some_and(|extension| extension == "c") {
            files.push(path);
        }
    }
    Ok(())
}

/// The files after the target in the Makefile rule that clang's -MD writes.
fn dependencies(rule: &Path) -> Vec<PathBuf> {
    let rule = fs::read_to_string(rule).unwrap_or_default();
    let Some((_, files)) = rule.split_once(": ") else {
        return Vec::new();
    };
    // `\ ` is a space in a path; a lone `\` continues the line.
    let files = files.replace("\\ ", "\0");
    files
        .split_whitespace()
        .filter(|file| *file != "\\")
        .map(|file| PathBuf::from(file.replace('\0', " ")))
        .collect()
}

/// Whether `object` is missing or older than a file it was built from.
fn stale(object: &Path, dependencies: &[PathBuf]) -> bool {
    let Some(built) = modified(object) else {
        return true;
    };
    dependencies.is_empty()
        || dependencies
            .iter()
            .any(|file| modified(file).is_none_or(|time| time > built))
}

fn modified(path: &Path) -> Option<SystemTime> {
    fs::metadata(path)
        .and_then(|metadata| metadata.modified())
        .ok()
}
