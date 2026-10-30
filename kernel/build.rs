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

struct Node {
    feature: &'static str,
    id: u8,
    /// The `NODE_FIXED_<config>_CONFIG` it turns on.
    config: &'static str,
    serial_test: bool,
}

const NODES: [Node; 6] = [
    Node {
        feature: "BRAKE",
        id: 1,
        config: "BRAKE",
        serial_test: false,
    },
    Node {
        feature: "MPU",
        id: 3,
        config: "MPU",
        serial_test: false,
    },
    Node {
        feature: "ENCODER",
        id: 4,
        config: "ENCODER",
        serial_test: false,
    },
    Node {
        feature: "ENGINE",
        id: 5,
        config: "ENGINE",
        serial_test: false,
    },
    Node {
        feature: "ENGINE_BENCH",
        id: 5,
        config: "ENGINE",
        serial_test: true,
    },
    Node {
        feature: "ADC",
        id: 6,
        config: "ADC",
        serial_test: false,
    },
];
/// Each simulator's feature and source folder.
const SIMULATORS: [(&str, &str); 2] = [
    ("ENGINE_RPM_SIM", "simulators/engine-rpm/src"),
    ("WHEEL_RPM_SIM", "simulators/wheel-rpm/src"),
];
/// Each master's feature and defines.
const MASTERS: [(&str, &str); 2] = [("MASTER", ""), ("MASTER_NO_CAN", "-DMASTER_CAN_ENABLED=0")];

#[derive(Clone, Copy, PartialEq)]
enum Chip {
    Esp32c3,
    Esp32p4,
}

impl Chip {
    fn name(self) -> &'static str {
        match self {
            Self::Esp32c3 => "esp32c3",
            Self::Esp32p4 => "esp32p4",
        }
    }

    /// The C3 has no hardware floats, so Rust's `riscv32imc` ABI is soft-float. The P4's must
    /// match Rust's `riscv32imafc`, or the linker refuses to mix them.
    fn arch(self) -> &'static str {
        match self {
            Self::Esp32c3 => {
                "--target=riscv32-unknown-elf -march=rv32imc_zicsr_zifencei -mabi=ilp32"
            }
            Self::Esp32p4 => {
                "--target=riscv32-unknown-elf -march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
            }
        }
    }
}

/// Frame pointers let a Rust panic's backtrace continue through the C callers. Without atomic
/// instructions every atomic is a call, so the alignment warning is noise.
const FLAGS: &str = "-std=gnu17 -Oz -ggdb -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
    -ffunction-sections -fdata-sections -Wall -Wno-atomic-alignment";

fn main() -> Result<(), Box<dyn Error>> {
    println!("cargo::rustc-link-search={}", env!("CARGO_MANIFEST_DIR"));
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
    let enabled = |feature: &str| env::var_os(format!("CARGO_FEATURE_{feature}")).is_some();
    let mut firmware = Vec::new();
    for node in NODES.iter().filter(|node| enabled(node.feature)) {
        let Node {
            id,
            config,
            serial_test,
            ..
        } = node;
        let defines = format!(
            "-DNODE_ID={id} -DSENSOR_SERIAL_TEST={} -DNODE_FIXED_{config}_CONFIG=1",
            u8::from(*serial_test)
        );
        firmware.push((Chip::Esp32c3, "sensor-node/src", defines));
    }
    for (feature, src) in SIMULATORS {
        if enabled(feature) {
            firmware.push((Chip::Esp32c3, src, String::new()));
        }
    }
    for (feature, defines) in MASTERS {
        if enabled(feature) {
            firmware.push((Chip::Esp32p4, "master-node/src", defines.to_owned()));
        }
    }
    let target_chip = match env::var("DEP_IDF_CHIP")?.as_str() {
        "esp32p4" => Chip::Esp32p4,
        _ => Chip::Esp32c3,
    };
    println!("cargo::rustc-check-cfg=cfg(esp32c3)");
    println!("cargo::rustc-check-cfg=cfg(esp32p4)");
    println!("cargo::rustc-cfg={}", target_chip.name());
    match &firmware[..] {
        [] => {
            println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
            println!("cargo::rustc-link-arg-bins=-Tno_c.x");
        }
        [(chip, ..)] if *chip != target_chip => println!(
            "cargo::error={}",
            if *chip == Chip::Esp32p4 {
                "wrong folder: build the master inside master-node/"
            } else {
                "wrong folder: build this inside sensor-node/ or simulators/"
            }
        ),
        [(chip, src, defines)] => compile(*chip, src, defines)?,
        _ => println!("cargo::error=choose one firmware, not several"),
    }
    Ok(())
}

fn compile(chip: Chip, src: &str, defines: &str) -> Result<(), Box<dyn Error>> {
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
    let mut flags: Vec<_> = [chip.arch(), FLAGS]
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
    flags.extend(defines.split_whitespace().map(String::from));
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
    // In the target folder, where sensor-node/.clangd looks.
    let target_dir = env::var_os("DEP_IDF_TARGET_DIR").ok_or("no DEP_IDF_TARGET_DIR")?;
    let json = format!("[\n{}\n]\n", entries.join(",\n"));
    fs::write(Path::new(&target_dir).join("compile_commands.json"), json)?;
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
