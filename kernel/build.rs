//! Adds the firmware's C code (libapp.a) and esp-hal's memory layout to the kernel.

use std::env;

fn main() {
    println!("cargo::rerun-if-env-changed=BAJA_APP_ARCHIVE");
    if let Ok(archive) = env::var("BAJA_APP_ARCHIVE") {
        println!("cargo::rerun-if-changed={archive}");
        println!("cargo::rustc-link-arg-bins={archive}");
    } else {
        println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
        println!("cargo::rustc-link-arg-bins=-Tno_c.x");
    }
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
}
