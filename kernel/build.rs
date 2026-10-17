//! Adds the firmware's C code (libapp.a) and esp-hal's memory layout to the kernel.

fn main() {
    println!("cargo::rerun-if-env-changed=BAJA_APP_ARCHIVE");
    if let Ok(archive) = std::env::var("BAJA_APP_ARCHIVE") {
        println!("cargo::rerun-if-changed={archive}");
        // Nothing in Rust calls most of the C, so without this the linker would drop it.
        println!("cargo::rustc-link-arg-bins=--whole-archive");
        println!("cargo::rustc-link-arg-bins={archive}");
        println!("cargo::rustc-link-arg-bins=--no-whole-archive");
    }
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
}
