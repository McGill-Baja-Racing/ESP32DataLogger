//! Layer 2: errors, logging and the clock.

use alloc::boxed::Box;
use core::{
    cell::Cell,
    ffi::{c_char, c_int},
};

use critical_section::Mutex;
use esp_hal::time::Instant;
use esp_radio_rtos_driver::semaphore::{SemaphoreHandle, SemaphoreKind};
use macros::export;
use sys::{esp_err_t, esp_log_config_t};

#[export]
#[expect(clippy::panic, reason = "ESP-IDF aborts here too")]
fn _esp_error_check_failed(
    code: esp_err_t,
    file: &str,
    line: c_int,
    _function: &str,
    expression: &str,
) -> ! {
    panic!("ESP_ERROR_CHECK({expression}) failed: {code:#x} at {file}:{line}");
}

/// Keeps each task's log line whole with interrupts on, like ESP-IDF's stdout lock.
static LOG_LOCK: Mutex<Cell<Option<&'static SemaphoreHandle>>> = Mutex::new(Cell::new(None));

pub fn init() {
    let lock = Box::leak(Box::new(SemaphoreHandle::new(SemaphoreKind::Mutex)));
    critical_section::with(|cs| LOG_LOCK.borrow(cs).set(Some(lock)));
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
    let lock = critical_section::with(|cs| LOG_LOCK.borrow(cs).get());
    if let Some(lock) = lock {
        lock.take(None);
    }
    esp_println::print!("{line}");
    if let Some(lock) = lock {
        lock.give();
    }
}

#[export]
#[expect(
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
