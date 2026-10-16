//! Layer 2: errors, logging and the clock.

use core::ffi::{c_char, c_int};

use esp_hal::time::Instant;
use macros::export;
use sys::{CONFIG_LOG_DEFAULT_LEVEL, esp_err_t, esp_log_config_t};

#[export]
fn _esp_error_check_failed(
    code: esp_err_t,
    file: &str,
    line: c_int,
    function: &str,
    expression: &str,
) -> ! {
    #[expect(clippy::panic, reason = "ESP-IDF aborts here too")]
    {
        panic!(
            "ESP_ERROR_CHECK failed: esp_err_t {code:#x}\nfile: \"{file}\" line {line}\nfunc: {function}\nexpression: {expression}"
        );
    }
}

/// `ESP_LOGx`'s C macros build the whole line's format; this prints it.
#[export]
unsafe fn esp_log(config: esp_log_config_t, _tag: *const c_char, format: *const c_char, args: ...) {
    // SAFETY: every field of the union is a view of the same 32 bits.
    let level = unsafe { config.__bindgen_anon_1.opts }.log_level().0;
    if level > CONFIG_LOG_DEFAULT_LEVEL.unsigned_abs() {
        return;
    }
    // SAFETY: `format` and `args` are the printf format and arguments the macro passed.
    let line = unsafe { printf_compat::output::display(format, args) };
    esp_println::print!("{line}");
}

#[export]
#[expect(
    clippy::as_conversions,
    clippy::cast_possible_truncation,
    reason = "wraps like ESP-IDF's counter"
)]
fn esp_log_timestamp() -> u32 {
    Instant::now().duration_since_epoch().as_millis() as u32
}

#[export]
fn esp_timer_get_time() -> i64 {
    i64::try_from(Instant::now().duration_since_epoch().as_micros()).unwrap_or(i64::MAX)
}
