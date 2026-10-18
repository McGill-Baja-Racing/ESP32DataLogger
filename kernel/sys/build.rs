//! Generates ESP-IDF's C types, constants and function declarations with bindgen, from the same
//! headers the firmware compiles against. Needs libclang.

use std::{env, fs, path::PathBuf};

fn main() {
    let kernel =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets it")).join("..");
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets it"));
    let platformio = env::var_os("PLATFORMIO_CORE_DIR").map_or_else(
        || env::home_dir().expect("the home folder is known").join(".platformio"),
        PathBuf::from,
    );
    let idf = platformio.join("packages/framework-espidf/components");
    let newlib = platformio.join("packages/toolchain-riscv32-esp/riscv32-esp-elf/include");
    let (chip, march, mabi) =
        if env::var("TARGET").is_ok_and(|target| target.starts_with("riscv32imafc")) {
            ("esp32p4", "rv32imafc_zicsr_zifencei", "ilp32f")
        } else {
            ("esp32c3", "rv32imc_zicsr_zifencei", "ilp32")
        };
    let ini = kernel.join("pio/kernel.ini");
    let sdkconfig = kernel.join("include").join(chip);
    println!("cargo::rerun-if-changed={}", ini.display());
    println!("cargo::rerun-if-env-changed=PLATFORMIO_CORE_DIR");
    let ini = fs::read_to_string(ini).expect("kernel/pio/kernel.ini exists");

    let mut args = vec![
        "--target=riscv32-unknown-none-elf".into(),
        "-std=gnu17".into(),
        "-DESP_PLATFORM".into(),
        format!("-march={march}"),
        format!("-mabi={mabi}"),
        format!("-I{}", sdkconfig.display()),
        format!("-isystem{}", newlib.display()),
    ];
    args.extend(include_dirs(&ini, chip).map(|dir| format!("-I{}", idf.join(dir).display())));

    let builder = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_args(args)
        .use_core()
        // clang's own `max_align_t` fails its layout test on this target; nothing uses it.
        .blocklist_type("max_align_t")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_file(".*/framework-espidf/components.*")
        .generate_comments(false)
        .clang_macro_fallback()
        .clang_macro_fallback_build_dir(&out)
        .default_macro_constant_type(bindgen::MacroTypeVariation::Signed)
        .default_enum_style(bindgen::EnumVariation::NewType {
            is_bitfield: false,
            is_global: false,
        });
    builder
        .generate()
        .expect("bindgen can read the headers")
        .write_to_file(out.join("bindings.rs"))
        .expect("OUT_DIR is writable");
}

/// The ESP-IDF header folders `kernel.ini` gives the C firmware for `chip`, under `custom_idf`.
fn include_dirs<'ini>(ini: &'ini str, chip: &str) -> impl Iterator<Item = &'ini str> {
    let mut section = "";
    ini.lines().map(str::trim).filter_map(move |line| {
        if let Some(name) = line
            .strip_prefix('[')
            .and_then(|rest| rest.strip_suffix(']'))
        {
            section = name;
            return None;
        }
        let ours = section == "kernel" || section == format!("kernel_{chip}");
        line.strip_prefix("-I${kernel.custom_idf}/")
            .filter(|_| ours)
    })
}
