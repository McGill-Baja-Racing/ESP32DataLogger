//! Adds the firmware's C code (libapp.a) and esp-hal's memory layout to the kernel.

fn main() {
    println!("cargo::rerun-if-env-changed=BAJA_APP_ARCHIVE"); // pio/cargo.py sets this per env
    if let Ok(archive) = std::env::var("BAJA_APP_ARCHIVE") {
        println!("cargo::rerun-if-changed={archive}"); // rebuild when the C code changes
        println!("cargo::rustc-link-arg-bins=--whole-archive"); // keep every C file, like PlatformIO does
        println!("cargo::rustc-link-arg-bins={archive}"); // put the C code into the kernel
        println!("cargo::rustc-link-arg-bins=--no-whole-archive"); // back to normal for what follows
    }
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x"); // esp-hal's memory layout for the chip
}
