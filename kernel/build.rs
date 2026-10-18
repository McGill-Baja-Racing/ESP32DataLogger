//! Adds the firmware's C code (libapp.a) and esp-hal's memory layout to the kernel.

use std::{env, fs, io, path::PathBuf};

fn main() -> io::Result<()> {
    println!("cargo::rerun-if-env-changed=BAJA_APP_ARCHIVE");
    if let Ok(archive) = env::var("BAJA_APP_ARCHIVE") {
        println!("cargo::rerun-if-changed={archive}");
        // Nothing in Rust calls most of the C, so without this the linker would drop it.
        println!("cargo::rustc-link-arg-bins=--whole-archive");
        println!("cargo::rustc-link-arg-bins={archive}");
        println!("cargo::rustc-link-arg-bins=--no-whole-archive");
    } else {
        // A plain `cargo build` has no C, so `app_main` is missing. lld reports that before any
        // ASSERT in a linker script, so only here it is downgraded to a warning, and the
        // ASSERT's message becomes the error.
        #[expect(
            clippy::expect_used,
            reason = "cargo sets OUT_DIR for every build script"
        )]
        let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets it"));
        fs::write(
            out.join("no_c.x"),
            "ASSERT(0, \"no firmware C: build with `pio run` in a node's project folder\");\n",
        )?;
        println!("cargo::rustc-link-search={}", out.display());
        println!("cargo::rustc-link-arg-bins=--warn-unresolved-symbols");
        println!("cargo::rustc-link-arg-bins=-Tno_c.x");
    }
    println!("cargo::rustc-link-arg-bins=-Tlinkall.x");
    Ok(())
}
