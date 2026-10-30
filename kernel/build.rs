//! Turns on `cfg(esp32c3)` or `cfg(esp32p4)`: the chip `sys` reads the headers for.

fn main() -> Result<(), std::env::VarError> {
    println!("cargo::rustc-check-cfg=cfg(esp32c3, esp32p4)");
    println!("cargo::rustc-cfg={}", std::env::var("DEP_IDF_CHIP")?);
    Ok(())
}
