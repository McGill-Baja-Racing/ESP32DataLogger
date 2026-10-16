//! Links the firmware's C archive and esp-hal's linker script into the kernel binary.

fn main() {
    println!("cargo::rerun-if-env-changed=BAJA_APP_ARCHIVE");
    // Set by the PlatformIO hook to the firmware's C archive, built for the same chip ABI.
    if let Ok(archive) = std::env::var("BAJA_APP_ARCHIVE") {
        println!("cargo::rerun-if-changed={archive}");
        println!("cargo::rustc-link-arg-bins={archive}");
    }
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
}
