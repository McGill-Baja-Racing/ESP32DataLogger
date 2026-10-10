//! Root of the Rust sensor drivers. Cargo builds this crate into a static
//! library that the C firmware links; each sensor is a `mod` below and exports
//! one `sensor_t` descriptor that `sensor_registry.c` declares `extern`.
//!
//! The crate has no standard library (`no_std`), so it must supply its own
//! panic handler. On a panic it hands the message to ESP-IDF, which prints it
//! and reboots the chip.

// This declaration turns off the Rust standard (std) library since it needs an operating system and a heap.
// Only `core`, the minimal part of Rust, is available.
#![no_std]

use core::ffi::{c_char, c_void}; // Import C types necessary for interoperability
use core::fmt::Write; // lets `write!` send formatted text to our `Console`
use core::panic::PanicInfo; // holds the location and message of a panic (a crash in Rust)

mod brake_position; // pulls in brake_position.rs (each new Rust sensor adds another `mod` line)

// Functions that live in C (ESP-IDF), not in Rust. The compiler cannot check
// C code, so it trusts these signatures and makes every call to them `unsafe`.
extern "C" {
    // Sends one byte to the serial console
    fn esp_rom_output_tx_one_char(byte: u8) -> i32;
    // Prints the "abort() was called" message and reboots, so it never returns (`-> !`).
    fn abort() -> !;
}

// Sends text straight to the serial console
struct Console;

// Teaches `write!` to print each piece of text it produces.
impl Write for Console {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for byte in s.bytes() {
            // SAFETY: takes a plain byte and has no other requirements.
            unsafe { esp_rom_output_tx_one_char(byte) };
        }
        Ok(())
    }
}

// Rust calls this on any panic, e.g. `.expect()` on `None` or an index out of
// range. It must never return (`-> !`): here it reboots the chip.
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    // Prints "panicked at <file>:<line>:<col>:\n<message>".
    // The result is ignored: a failed print must not cause a second panic.
    let _ = core::write!(Console, "{info}");
    // SAFETY: `abort` takes no arguments and does not return.
    unsafe { abort() }
}

/// The sensor struct just like the C version
#[repr(C)]
struct Sensor {
    /// C `const char *`: `c_char` is `char` on the target (signedness varies).
    name: *const c_char,
    /// C `uint32_t`: fixed-width, so exactly `u32`.
    can_id: u32,
    /// C `uint32_t`: fixed-width, so exactly `u32`.
    period_us: u32,
    /// C `int64_t`: fixed-width, so exactly `i64`.
    next_sample_us: i64,
    /// C `esp_err_t (*)(sensor_t *)`: `esp_err_t` is `int`, so `i32`. `Option`
    /// because C allows NULL; `None` is NULL.
    init: Option<unsafe extern "C" fn(sensor: &mut Sensor) -> i32>,
    /// C `void (*)(sensor_t *)`: `void` return is no `->`. `Option` for NULL.
    start: Option<unsafe extern "C" fn(sensor: &mut Sensor)>,
    /// C `esp_err_t (*)(sensor_t *, int32_t *)`: returns `i32`; `int32_t *` is
    /// `*mut i32`. `Option` for NULL.
    read: Option<unsafe extern "C" fn(sensor: &mut Sensor, value: *mut i32) -> i32>,
    /// C `void *`: Rust has no `void`, so `*mut c_void`. `mut` because the C
    /// pointer is non-const.
    context: *mut c_void,
}

// SAFETY: descriptors are read-only statics; the registry copies them before
// the sampler task writes `next_sample_us`.
unsafe impl Sync for Sensor {}
