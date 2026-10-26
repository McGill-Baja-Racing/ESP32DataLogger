//! Compiles the C firmware chosen by a cargo feature, and links it and esp-hal's memory layout
//! into the kernel.

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
/// Each simulator's feature and source folder.
const SIMULATORS: [(&str, &str); 2] = [
    ("ENGINE_RPM_SIM", "simulators/engine-rpm/src"),
    ("WHEEL_RPM_SIM", "simulators/wheel-rpm/src"),
];
const CONFIGS: [&str; 5] = ["BRAKE", "ENCODER", "ENGINE", "ADC", "MPU"];
/// The master's features and defines: `Master` and `MasterNoCAN`'s `build_flags` in `platformio.ini`.
const MASTERS: [(&str, &[&str]); 2] = [
    ("MASTER", &["-DSD_FORMAT_IF_MOUNT_FAILED=0"]),
    (
        "MASTER_NO_CAN",
        &["-DSD_FORMAT_IF_MOUNT_FAILED=0", "-DMASTER_CAN_ENABLED=0"],
    ),
];

/// Frame pointers let a Rust panic's backtrace continue through the C callers. Without atomic
/// instructions every atomic is a call, so the alignment warning is noise.
const FLAGS: &str = "-std=gnu17 -Oz -ggdb -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
    -ffunction-sections -fdata-sections -Wall -Wno-atomic-alignment";
/// The C3 has no hardware floats, so Rust's `riscv32imc` ABI is soft-float. The P4's must match
/// Rust's `riscv32imafc`, or the linker refuses to mix them.
const C3_FLAGS: &str = "--target=riscv32-unknown-elf -march=rv32imc_zicsr_zifencei -mabi=ilp32";
const P4_FLAGS: &str = "--target=riscv32-unknown-elf -march=rv32imafc_zicsr_zifencei -mabi=ilp32f";

fn main() -> Result<(), Box<dyn Error>> {
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
    let enabled = |feature: &str| env::var_os(format!("CARGO_FEATURE_{feature}")).is_some();
    let mut firmware = Vec::new();
    for (feature, id, config, serial_test) in NODES {
        if enabled(feature) {
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
            firmware.push(("esp32c3", "sensor-node/src", defines));
        }
    }
    for (feature, src) in SIMULATORS {
        if enabled(feature) {
            firmware.push(("esp32c3", src, Vec::new()));
        }
    }
    for (feature, defines) in MASTERS {
        if enabled(feature) {
            let defines = defines.iter().map(|define| (*define).to_owned()).collect();
            firmware.push(("esp32p4", "master-node/src", defines));
        }
    }
    let target = env::var("TARGET")?;
    let target_chip = if target.starts_with("riscv32imafc") {
        "esp32p4"
    } else {
        "esp32c3"
    };
    if enabled("ESP32C3") && enabled("ESP32P4") {
        println!("cargo::error=the master needs --no-default-features, which includes the C3");
    }
    match &firmware[..] {
        [] => {
            println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
            println!("cargo::rustc-link-arg-bins=-Tno_c.x");
        }
        [(chip, ..)] if *chip != target_chip => println!(
            "cargo::error=this firmware is for the {chip}; build it with {}",
            if *chip == "esp32p4" {
                "--target riscv32imafc-unknown-none-elf"
            } else {
                "--target riscv32imc-unknown-none-elf"
            }
        ),
        [(chip, src, defines)] => compile(chip, src, defines)?,
        _ => println!("cargo::error=enable one firmware feature, not several"),
    }
    Ok(())
}

fn compile(chip: &str, src: &str, defines: &[String]) -> Result<(), Box<dyn Error>> {
    let Some(clang) = clang() else {
        println!(
            "cargo::error=no clang with RISC-V support found; install LLVM and put its clang on the PATH or in CLANG_PATH"
        );
        return Ok(());
    };
    let kernel = Path::new(env!("CARGO_MANIFEST_DIR"));
    let repo = kernel.parent().ok_or("kernel/ is in the repository")?;
    let src = repo.join(src);
    let out = PathBuf::from(env::var_os("OUT_DIR").ok_or("no OUT_DIR")?);
    let arch = if chip == "esp32p4" {
        P4_FLAGS
    } else {
        C3_FLAGS
    };
    let mut flags: Vec<_> = [arch, FLAGS]
        .iter()
        .flat_map(|flags| flags.split_whitespace().map(String::from))
        .collect();
    let idf = env::var_os("DEP_IDF_ARGS").ok_or("no DEP_IDF_ARGS")?;
    flags.extend(env::split_paths(&idf).map(|arg| arg.display().to_string()));
    flags.extend([
        format!("-I{}", src.display()),
        // For the master's `.incbin "src/web/index.html"`.
        format!("-I{}", src.join("..").display()),
        format!("-I{}", repo.display()),
    ]);
    flags.extend_from_slice(defines);
    let unsupported = [
        "-include".to_owned(),
        kernel.join("include/unsupported.h").display().to_string(),
    ];

    let stamp = out.join("flags");
    let current = format!("{clang} {flags:?}");
    let flags_changed = fs::read_to_string(&stamp).ok().as_deref() != Some(&current);
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
        // -MD doesn't list the files an assembly file's `.incbin` reads. C headers have no
        // business in assembly.
        let assembly = file.extension().is_some_and(|extension| extension == "S");
        let args = [
            &flags[..],
            if assembly { &[] } else { &unsupported },
            &["-MD".into(), "-c".into(), file_arg, "-o".into(), object_arg],
        ]
        .concat();
        if flags_changed || assembly || stale(&object, &dependencies(&rule)) {
            fs::create_dir_all(object.parent().ok_or("an object has a folder")?)?;
            // A failed compile must not leave the old object to be linked next time.
            fs::remove_file(&object).ok();
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
        // Debug quoting matches JSON's for paths and flags.
        let arguments: Vec<_> = iter::once(&clang).chain(&args).collect();
        entries.push(format!(
            "{{\"directory\":{:?},\"file\":{:?},\"arguments\":{arguments:?}}}",
            kernel.display().to_string(),
            file.display().to_string(),
        ));
    }
    fs::write(&stamp, current)?;
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

/// `CLANG_PATH` is the variable bindgen's clang-sys also reads. Apple's clang has no RISC-V backend.
fn clang() -> Option<String> {
    println!("cargo::rerun-if-env-changed=CLANG_PATH");
    let clang = env::var("CLANG_PATH").unwrap_or_else(|_| "clang".into());
    Command::new(&clang)
        .arg("-print-targets")
        .output()
        .is_ok_and(|output| String::from_utf8_lossy(&output.stdout).contains("riscv32"))
        .then_some(clang)
}

fn c_files(dir: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            c_files(&path, files)?;
        } else if path
            .extension()
            .is_some_and(|extension| extension == "c" || extension == "S")
        {
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
