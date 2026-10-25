//! Fetches the ESP-IDF and newlib headers the firmware compiles against, generates their Rust
//! bindings with bindgen, and passes the same clang arguments to `kernel/build.rs`. Needs git
//! and libclang.

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

const IDF: &str = "esp-idf-v5.5.5";
const IDF_COMMIT: &str = "b774170ff46c393eeb5e495ea37936038d3f4f4f";
/// The newlib of ESP-IDF v5.5's toolchain, esp-4.3.0.
const NEWLIB_COMMIT: &str = "1cc5829f04ef0bb1edc913e55ac7c47d58a46646";

/// The ESP-IDF header folders the firmware needs, on every chip (`""`) or on one.
const IDF_INCLUDES: [(&str, &str); 24] = [
    ("", "components/newlib/platform_include"),
    ("", "components/freertos/config/include"),
    ("", "components/freertos/config/include/freertos"),
    ("", "components/freertos/config/riscv/include"),
    ("", "components/freertos/FreeRTOS-Kernel/include"),
    (
        "",
        "components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos",
    ),
    ("", "components/freertos/esp_additions/include"),
    ("", "components/esp_hw_support/include"),
    ("", "components/heap/include"),
    ("", "components/log/include"),
    ("", "components/hal/platform_port/include"),
    ("", "components/hal/include"),
    ("", "components/esp_rom/include"),
    ("", "components/esp_common/include"),
    ("", "components/esp_system/include"),
    ("", "components/riscv/include"),
    ("", "components/esp_adc/include"),
    ("", "components/esp_driver_gpio/include"),
    ("", "components/esp_driver_i2c/include"),
    ("", "components/esp_driver_twai/include"),
    ("", "components/esp_timer/include"),
    ("esp32c3", "components/soc/esp32c3/include"),
    ("esp32c3", "components/soc/esp32c3/register"),
    ("esp32c3", "components/esp_adc/esp32c3/include"),
];

/// In the order newlib's install overlays them.
const NEWLIB_INCLUDES: [&str; 3] = [
    "newlib/libc/sys/esp/include",
    "newlib/libc/machine/riscv",
    "newlib/libc/include",
];

/// The headers newlib's configure generates, as ESP-IDF v5.5's toolchain configures it.
const NEWLIB_H: &str = "#pragma once
#include <_newlib_version.h>
#define _ATEXIT_DYNAMIC_ALLOC 1
#define _FSEEK_OPTIMIZATION 1
#define _FVWRITE_IN_STREAMIO 1
#define _HAVE_CC_INHIBIT_LOOP_TO_LIBCALL 1
#define _HAVE_INITFINI_ARRAY 1
#define _HAVE_LONG_DOUBLE 1
#define _ICONV_ENABLED 1
#define _MB_LEN_MAX 1
#define _NANO_MALLOC 1
#define _REENT_CHECK_VERIFY 1
#define _RETARGETABLE_LOCKING 1
#define _UNBUF_STREAM_OPT 1
#define _WANT_IO_C99_FORMATS 1
#define _WANT_IO_LONG_LONG 1
#define _WANT_IO_POS_ARGS 1
#define _WANT_REENT_BACKWARD_BINARY_COMPAT 1
#define _WANT_REENT_SMALL 1
#define _WANT_USE_GDTOA 1
";
const NEWLIB_VERSION_H: &str = "#pragma once
#define _NEWLIB_VERSION \"4.3.0\"
#define __NEWLIB__ 4
#define __NEWLIB_MINOR__ 3
#define __NEWLIB_PATCHLEVEL__ 0
";

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets it"));
    // OUT_DIR is <target>/<triple>/<profile>/build/<package>/out; `[build] target` sets <triple>.
    let target_dir = out
        .ancestors()
        .nth(5)
        .expect("OUT_DIR is in the target folder");
    let idf = fetch(
        target_dir,
        IDF,
        "https://github.com/espressif/esp-idf.git",
        IDF_COMMIT,
        &IDF_INCLUDES.map(|(_, dir)| dir),
    );
    let newlib = fetch(
        target_dir,
        &format!("newlib-{}", &NEWLIB_COMMIT[..8]),
        "https://github.com/espressif/newlib-esp32.git",
        NEWLIB_COMMIT,
        &NEWLIB_INCLUDES,
    );
    let generated = out.join("libc");
    fs::create_dir_all(&generated).expect("OUT_DIR is writable");
    for (name, contents) in [
        ("newlib.h", NEWLIB_H),
        ("_newlib_version.h", NEWLIB_VERSION_H),
    ] {
        let path = generated.join(name);
        // A newer file would make cargo rebuild all the C.
        if fs::read_to_string(&path).ok().as_deref() != Some(contents) {
            fs::write(path, contents).expect("OUT_DIR is writable");
        }
    }

    let chip = if env::var("TARGET").is_ok_and(|target| target.starts_with("riscv32imafc")) {
        "esp32p4"
    } else {
        "esp32c3"
    };
    // The C library is the newlib below, never one clang finds on the machine.
    let mut args = vec!["-nostdlibinc".to_owned(), "-DESP_PLATFORM".to_owned()];
    for (only, dir) in IDF_INCLUDES {
        if only.is_empty() || only == chip {
            args.push(format!("-I{}", idf.join(dir).display()));
        }
    }
    let kernel = concat!(env!("CARGO_MANIFEST_DIR"), "/..");
    args.push(format!("-I{kernel}/include/{chip}"));
    // -idirafter puts the C library after the compiler's headers, as GCC does.
    args.push(format!("-idirafter{}", generated.display()));
    for dir in NEWLIB_INCLUDES {
        args.push(format!("-idirafter{}", newlib.join(dir).display()));
    }
    // A path list, so one metadata line carries every argument.
    let joined = env::join_paths(&args).expect("no argument contains the path separator");
    println!("cargo::metadata=args={}", joined.display());

    bindgen::Builder::default()
        .header("wrapper.h")
        .clang_args(&args)
        .use_core()
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_file(format!(".*{IDF}.*"))
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

/// Checks out only `paths` of `commit` into `cache/name`, once: a shallow, partial, sparse clone.
fn fetch(cache: &Path, name: &str, url: &str, commit: &str, paths: &[&str]) -> PathBuf {
    let dir = cache.join(name);
    if !dir.exists() {
        let tmp = cache.join(format!("{name}.tmp"));
        fs::remove_dir_all(&tmp).ok();
        git(cache, &["init", &format!("{name}.tmp")]);
        let shallow = ["fetch", "--depth=1", "--filter=blob:none", url, commit];
        git(&tmp, &shallow);
        git(&tmp, &[&["sparse-checkout", "set"], paths].concat());
        git(&tmp, &["checkout", "FETCH_HEAD"]);
        let head = git(&tmp, &["rev-parse", "HEAD"]);
        assert_eq!(head.trim(), commit, "{url} checked out");
        fs::rename(&tmp, &dir).expect("the target folder is writable");
    }
    dir
}

fn git(dir: &Path, args: &[&str]) -> String {
    let output = Command::new("git")
        .current_dir(dir)
        .args(args)
        .output()
        .expect("git is installed");
    assert!(
        output.status.success(),
        "git {}: {}",
        args.join(" "),
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8(output.stdout).expect("git prints UTF-8")
}
