//! The build script of every firmware package. It compiles the package's `src/` with clang, once
//! per binary with that binary's defines, and links each binary to its objects and esp-hal's
//! memory layout. The package depends on `sys` for the clang arguments.

use std::{
    env,
    error::Error,
    fs, io, iter,
    path::{Path, PathBuf},
    process::Command,
    time::SystemTime,
};

/// Frame pointers let a Rust panic's backtrace continue through the C callers. Without atomic
/// instructions every atomic is a call, so the alignment warning is noise.
const FLAGS: &str = "-std=gnu17 -Oz -ggdb -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
    -ffunction-sections -fdata-sections -Wall -Wno-atomic-alignment";

/// A package that uses this build script.
struct Firmware {
    package: &'static str,
    chip: &'static str,
    /// Each binary, and the defines its C is built with.
    binaries: &'static [(&'static str, &'static str)],
}

const FIRMWARE: [Firmware; 4] = [
    Firmware {
        package: "sensor-node",
        chip: "esp32c3",
        binaries: &[
            (
                "brake",
                "-DNODE_ID=1 -DSENSOR_SERIAL_TEST=0 -DNODE_FIXED_BRAKE_CONFIG=1",
            ),
            (
                "mpu",
                "-DNODE_ID=3 -DSENSOR_SERIAL_TEST=0 -DNODE_FIXED_MPU_CONFIG=1",
            ),
            (
                "encoder",
                "-DNODE_ID=4 -DSENSOR_SERIAL_TEST=0 -DNODE_FIXED_ENCODER_CONFIG=1",
            ),
            (
                "engine",
                "-DNODE_ID=5 -DSENSOR_SERIAL_TEST=0 -DNODE_FIXED_ENGINE_CONFIG=1",
            ),
            (
                "engine-bench",
                "-DNODE_ID=5 -DSENSOR_SERIAL_TEST=1 -DNODE_FIXED_ENGINE_CONFIG=1",
            ),
            (
                "adc",
                "-DNODE_ID=6 -DSENSOR_SERIAL_TEST=0 -DNODE_FIXED_ADC_CONFIG=1",
            ),
        ],
    },
    Firmware {
        package: "engine-rpm-sim",
        chip: "esp32c3",
        binaries: &[("engine-rpm-sim", "")],
    },
    Firmware {
        package: "wheel-rpm-sim",
        chip: "esp32c3",
        binaries: &[("wheel-rpm-sim", "")],
    },
    Firmware {
        package: "master-node",
        chip: "esp32p4",
        binaries: &[("master", ""), ("master-no-can", "-DMASTER_CAN_ENABLED=0")],
    },
];

fn main() -> Result<(), Box<dyn Error>> {
    let package = env::var("CARGO_PKG_NAME")?;
    let Firmware { chip, binaries, .. } = FIRMWARE
        .into_iter()
        .find(|firmware| firmware.package == package)
        .ok_or("FIRMWARE lists every package that uses this build script")?;
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
    let firmware = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("no CARGO_MANIFEST_DIR")?);
    let out = PathBuf::from(env::var_os("OUT_DIR").ok_or("no OUT_DIR")?);
    let kernel = PathBuf::from(env::var_os("DEP_IDF_KERNEL").ok_or("no DEP_IDF_KERNEL")?);
    let repo = kernel.parent().ok_or("kernel/ is in the repository")?;
    if env::var("DEP_IDF_CHIP")? != chip {
        // `cargo check` and rust-analyzer check every package for one target, so only the link
        // fails. lld reports the missing `app_main` before any ASSERT unless it's a warning.
        let folder = firmware.strip_prefix(repo)?.display();
        let script = out.join("wrong_folder.x");
        let message = format!("wrong folder: build {package} inside {folder}/");
        fs::write(&script, format!("ASSERT(0, \"{message}\");\n"))?;
        println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
        println!("cargo::rustc-link-arg-bins=-T{}", script.display());
        return Ok(());
    }
    let Some(clang) = clang() else {
        println!(
            "cargo::error=no clang with RISC-V support found; install LLVM and put its clang on the PATH or in CLANG_PATH"
        );
        return Ok(());
    };
    let src = firmware.join("src");
    // The C3 has no hardware floats, so Rust's `riscv32imc` ABI is soft-float. The P4's must
    // match Rust's `riscv32imafc`, or the linker refuses to mix them.
    let arch = if chip == "esp32p4" {
        "--target=riscv32-unknown-elf -march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
    } else {
        "--target=riscv32-unknown-elf -march=rv32imc_zicsr_zifencei -mabi=ilp32"
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
        format!("-I{}", firmware.display()),
        format!("-I{}", repo.display()),
    ]);
    let mut entries = Vec::new();
    for (index, &(binary, defines)) in binaries.iter().enumerate() {
        let defines: Vec<_> = defines.split_whitespace().map(String::from).collect();
        let commands = compile(
            &clang,
            &[&flags[..], &defines].concat(),
            &src,
            &out.join(binary),
            binary,
        )?;
        // clangd reads the first binary's commands.
        if index == 0 {
            entries = commands;
        }
    }
    println!("cargo::rerun-if-changed={}", src.display());
    // In the target folder, where the firmware's .clangd looks.
    let target_dir = env::var_os("DEP_IDF_TARGET_DIR").ok_or("no DEP_IDF_TARGET_DIR")?;
    let database = Path::new(&target_dir).join(&package);
    fs::create_dir_all(&database)?;
    let json = format!("[\n{}\n]\n", entries.join(",\n"));
    fs::write(database.join("compile_commands.json"), json)?;
    Ok(())
}

/// Compiles the C in `src` with `flags` into `out` and links the objects into `binary`. Returns
/// the compile commands, as `compile_commands.json` entries.
fn compile(
    clang: &str,
    flags: &[String],
    src: &Path,
    out: &Path,
    binary: &str,
) -> Result<Vec<String>, Box<dyn Error>> {
    let kernel = PathBuf::from(env::var_os("DEP_IDF_KERNEL").ok_or("no DEP_IDF_KERNEL")?);
    let unsupported = [
        "-include".to_owned(),
        kernel.join("include/unsupported.h").display().to_string(),
    ];
    let mut files = Vec::new();
    c_files(src, &mut files)?;
    files.sort();
    let mut entries = Vec::new();
    fs::create_dir_all(out)?;
    let stamp = out.join("flags");
    let current = format!("{clang} {flags:?}");
    let flags_changed = fs::read_to_string(&stamp).ok().as_deref() != Some(&current);
    for file in &files {
        let object = out.join(file.strip_prefix(src)?).with_extension("o");
        let rule = object.with_extension("d");
        let [file_arg, object_arg] = [file, &object].map(|path| path.display().to_string());
        // -MD doesn't list the files an assembly file's `.incbin` reads. C headers have no
        // business in assembly.
        let assembly = file.extension().is_some_and(|extension| extension == "S");
        let args = [
            flags,
            if assembly { &[] } else { &unsupported },
            &["-MD".into(), "-c".into(), file_arg, "-o".into(), object_arg],
        ]
        .concat();
        if flags_changed || assembly || stale(&object, &dependencies(&rule)) {
            fs::create_dir_all(object.parent().ok_or("an object has a folder")?)?;
            // A failed compile must not leave the old object to be linked next time.
            fs::remove_file(&object).ok();
            let output = Command::new(clang).args(&args).output()?;
            let level = if output.status.success() {
                "warning"
            } else {
                "error"
            };
            for line in String::from_utf8_lossy(&output.stderr).lines() {
                println!("cargo::{level}={line}");
            }
        }
        for header in dependencies(&rule) {
            println!("cargo::rerun-if-changed={}", header.display());
        }
        println!("cargo::rustc-link-arg-bin={binary}={}", object.display());
        // Debug quoting matches JSON's.
        let arguments: Vec<_> = iter::once(clang)
            .chain(args.iter().map(String::as_str))
            .collect();
        entries.push(format!(
            "{{\"file\":{:?},\"directory\":{:?},\"arguments\":{arguments:?}}}",
            file.display().to_string(),
            src.display().to_string(),
        ));
    }
    fs::write(&stamp, current)?;
    Ok(entries)
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
