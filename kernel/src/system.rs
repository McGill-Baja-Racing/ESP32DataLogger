//! Errors, logging and the clock.

use alloc::boxed::Box;
use core::{
    ffi::{CStr, c_char, c_int},
    slice,
};

use esp_hal::rng::Rng;
use esp_radio_rtos_driver as rtos;
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_RESPONSE, ESP_ERR_INVALID_SIZE,
    ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED,
    ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK, SOC_GPIO_PIN_COUNT, esp_err_t, esp_log_config_t,
};

use crate::{
    chip,
    rtos::{Global, Locked},
};

/// Only the codes the kernel and firmware return; the chip's own are `chip::error_name`.
fn error_name(code: esp_err_t) -> &'static CStr {
    match code {
        ESP_OK => c"ESP_OK",
        ESP_FAIL => c"ESP_FAIL",
        ESP_ERR_NO_MEM => c"ESP_ERR_NO_MEM",
        ESP_ERR_INVALID_ARG => c"ESP_ERR_INVALID_ARG",
        ESP_ERR_INVALID_STATE => c"ESP_ERR_INVALID_STATE",
        ESP_ERR_INVALID_SIZE => c"ESP_ERR_INVALID_SIZE",
        ESP_ERR_INVALID_CRC => c"ESP_ERR_INVALID_CRC",
        ESP_ERR_INVALID_RESPONSE => c"ESP_ERR_INVALID_RESPONSE",
        ESP_ERR_NOT_FOUND => c"ESP_ERR_NOT_FOUND",
        ESP_ERR_NOT_SUPPORTED => c"ESP_ERR_NOT_SUPPORTED",
        ESP_ERR_TIMEOUT => c"ESP_ERR_TIMEOUT",
        _ => chip::error_name(code).unwrap_or(c"UNKNOWN ERROR"),
    }
}

#[export]
fn esp_err_to_name(code: esp_err_t) -> *const c_char {
    error_name(code).as_ptr()
}

#[export]
#[expect(clippy::panic, reason = "ESP-IDF aborts here too")]
fn _esp_error_check_failed(
    code: esp_err_t,
    file: &str,
    line: c_int,
    function: &str,
    expression: &str,
) -> ! {
    let name = error_name(code).to_str().unwrap_or("?");
    panic!(
        "ESP_ERROR_CHECK({expression}) failed: {code:#x} ({name}) in {function} at {file}:{line}"
    );
}

#[export]
fn _esp_error_check_failed_without_abort(
    code: esp_err_t,
    file: &str,
    line: c_int,
    function: &str,
    expression: &str,
) {
    let name = error_name(code).to_str().unwrap_or("?");
    locked(|| {
        esp_println::println!(
            "ESP_ERROR_CHECK_WITHOUT_ABORT({expression}) failed: {code:#x} ({name}) in {function} at {file}:{line}"
        );
    });
}

#[export]
fn esp_random() -> u32 {
    Rng::new().random()
}

/// Keeps each task's log line whole with interrupts on, like ESP-IDF's stdout lock.
static LOG_LOCK: Global<Locked<()>> = Global::new();

pub fn init() {
    LOG_LOCK.set(Box::leak(Box::new(Locked::new(()))));
}

pub fn locked(print: impl FnOnce()) {
    match LOG_LOCK.get() {
        Some(lock) => lock.with(|()| print()),
        None => print(),
    }
}

/// The pin number if the chip has that GPIO, as the drivers' `GPIO_IS_VALID_GPIO` checks.
pub fn gpio_number(gpio: c_int) -> Result<u8, esp_err_t> {
    u8::try_from(gpio)
        .ok()
        .filter(|&number| i32::from(number) < SOC_GPIO_PIN_COUNT)
        .ok_or(ESP_ERR_INVALID_ARG)
}

/// The slice C describes with a pointer and a length, where a length of 0 allows any pointer and
/// otherwise NULL is `None`.
///
/// # Safety
///
/// Unless `len` is 0, `ptr` is NULL or points to `len` initialised `T`s that nothing writes for
/// `'a`.
pub unsafe fn c_slice<'a, T>(ptr: *const T, len: usize) -> Option<&'a [T]> {
    if len == 0 {
        Some(&[])
    } else if ptr.is_null() {
        None
    } else {
        // SAFETY: as the caller promises.
        Some(unsafe { slice::from_raw_parts(ptr, len) })
    }
}

/// `c_slice` for a buffer C lets the kernel write.
///
/// # Safety
///
/// Unless `len` is 0, `ptr` is NULL or points to `len` initialised `T`s that nothing else uses
/// for `'a`.
pub unsafe fn c_slice_mut<'a, T>(ptr: *mut T, len: usize) -> Option<&'a mut [T]> {
    if len == 0 {
        Some(&mut [])
    } else if ptr.is_null() {
        None
    } else {
        // SAFETY: as the caller promises.
        Some(unsafe { slice::from_raw_parts_mut(ptr, len) })
    }
}

/// `ESP_LOGx`'s C macros build the whole line's format and already drop levels above
/// `CONFIG_LOG_MAXIMUM_LEVEL`, which equals the default level; this prints it.
#[export]
unsafe fn esp_log(
    _config: esp_log_config_t,
    _tag: *const c_char,
    format: *const c_char,
    args: ...
) {
    // SAFETY: `format` and `args` are the printf format and arguments the macro passed.
    let line = unsafe { printf_compat::output::display(format, args) };
    locked(|| {
        esp_println::print!("{line}");
        // A format error stops the line short of its newline.
        if line.bytes_written() < 0 {
            esp_println::println!();
        }
    });
}

#[export]
#[expect(
    clippy::cast_possible_truncation,
    reason = "wraps like ESP-IDF's counter"
)]
fn esp_log_timestamp() -> u32 {
    (rtos::now() / 1000) as u32
}

#[export]
fn esp_timer_get_time() -> i64 {
    rtos::now().cast_signed()
}
