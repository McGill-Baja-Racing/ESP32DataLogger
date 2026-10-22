//! Generates ESP-IDF's C types, constants and function declarations with bindgen, from the same
//! headers the firmware compiles against. Needs libclang.

use std::{env, fs, path::PathBuf};

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets it"));
    let platformio = match env::var_os("PLATFORMIO_CORE_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => env::home_dir().expect("a home folder").join(".platformio"),
    };
    let idf = platformio.join("packages/framework-espidf/components");
    let newlib = platformio.join("packages/toolchain-riscv32-esp/riscv32-esp-elf/include");
    let chip = if env::var("TARGET").is_ok_and(|target| target.starts_with("riscv32imafc")) {
        "esp32p4"
    } else {
        "esp32c3"
    };
    println!("cargo::rerun-if-changed=../pio/kernel.ini");
    println!("cargo::rerun-if-env-changed=PLATFORMIO_CORE_DIR");
    let ini = fs::read_to_string("../pio/kernel.ini").expect("kernel/pio/kernel.ini exists");

    let mut args = vec![
        "-DESP_PLATFORM".into(),
        format!("-I../include/{chip}"),
        format!("-isystem{}", newlib.display()),
    ];
    let mut section = "";
    for line in ini.lines().map(str::trim) {
        if line.starts_with('[') {
            section = line;
        } else if let Some(dir) = line.strip_prefix("-I${kernel.custom_idf}/")
            && (section == "[kernel]" || section == format!("[kernel_{chip}]"))
        {
            args.push(format!("-I{}", idf.join(dir).display()));
        }
    }

    bindgen::Builder::default()
        .header("wrapper.h")
        .clang_args(args)
        .use_core()
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_file(".*/framework-espidf/components.*")
        .clang_macro_fallback()
        .clang_macro_fallback_build_dir(&out)
        .default_macro_constant_type(bindgen::MacroTypeVariation::Signed)
        .default_enum_style(bindgen::EnumVariation::NewType {
            is_bitfield: false,
            is_global: false,
        })
        .generate()
        .expect("bindgen can read the headers")
        .write_to_file(out.join("bindings.rs"))
        .expect("OUT_DIR is writable");
}
