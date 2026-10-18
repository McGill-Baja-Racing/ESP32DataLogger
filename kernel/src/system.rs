//! Layer 2: errors, logging and the clock.

use alloc::boxed::Box;
use core::{
    cell::Cell,
    ffi::{c_char, c_int},
};

use critical_section::Mutex;
use esp_hal::{rtc_cntl::SocResetReason, time::Instant};
use esp_radio_rtos_driver::semaphore::{SemaphoreHandle, SemaphoreKind};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND,
    ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK, esp_err_t, esp_log_config_t,
    esp_reset_reason_t,
};

/// Only the codes the kernel and firmware return.
#[export]
fn esp_err_to_name(code: esp_err_t) -> *const c_char {
    match code {
        ESP_OK => c"ESP_OK",
        ESP_FAIL => c"ESP_FAIL",
        ESP_ERR_NO_MEM => c"ESP_ERR_NO_MEM",
        ESP_ERR_INVALID_ARG => c"ESP_ERR_INVALID_ARG",
        ESP_ERR_INVALID_STATE => c"ESP_ERR_INVALID_STATE",
        ESP_ERR_NOT_FOUND => c"ESP_ERR_NOT_FOUND",
        ESP_ERR_NOT_SUPPORTED => c"ESP_ERR_NOT_SUPPORTED",
        ESP_ERR_TIMEOUT => c"ESP_ERR_TIMEOUT",
        _ => c"UNKNOWN ERROR",
    }
    .as_ptr()
}

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

/// Set by a panic so the reset after it reads as one; esp-hal zeroes it at power-on.
#[esp_hal::ram(unstable(rtc_fast, persistent))]
static mut PANICKED: u8 = 0;

static RESET_REASON: Mutex<Cell<esp_reset_reason_t>> =
    Mutex::new(Cell::new(esp_reset_reason_t::ESP_RST_UNKNOWN));

pub fn mark_panic() {
    // SAFETY: the panic path and `init` are the only writers, with nothing else running.
    unsafe { PANICKED = 1 };
}

pub fn init() {
    let lock = Box::leak(Box::new(SemaphoreHandle::new(SemaphoreKind::Mutex)));
    // SAFETY: read and cleared once at boot, before any task runs.
    let panicked = unsafe { core::ptr::replace(&raw mut PANICKED, 0) } != 0;
    let reason = reset_reason(esp_hal::system::reset_reason(), panicked);
    critical_section::with(|cs| {
        LOG_LOCK.borrow(cs).set(Some(lock));
        RESET_REASON.borrow(cs).set(reason);
    });
}

/// ESP-IDF's table for the C3, which counts an eFuse CRC reset as power-on.
fn reset_reason(hardware: Option<SocResetReason>, panicked: bool) -> esp_reset_reason_t {
    use SocResetReason::{
        ChipPowerOn, CoreDeepSleep, CoreEfuseCrc, CoreMwdt0, CoreMwdt1, CoreRtcWdt, CoreSw,
        CoreUsbJtag, CoreUsbUart, Cpu0Mwdt0, Cpu0Mwdt1, Cpu0RtcWdt, Cpu0Sw, SysBrownOut, SysRtcWdt,
        SysSuperWdt,
    };
    match hardware {
        Some(ChipPowerOn | CoreEfuseCrc) => esp_reset_reason_t::ESP_RST_POWERON,
        Some(CoreSw | Cpu0Sw) if panicked => esp_reset_reason_t::ESP_RST_PANIC,
        Some(CoreSw | Cpu0Sw) => esp_reset_reason_t::ESP_RST_SW,
        Some(CoreDeepSleep) => esp_reset_reason_t::ESP_RST_DEEPSLEEP,
        Some(CoreMwdt0) => esp_reset_reason_t::ESP_RST_TASK_WDT,
        Some(CoreMwdt1) => esp_reset_reason_t::ESP_RST_INT_WDT,
        Some(CoreRtcWdt | SysRtcWdt | SysSuperWdt | Cpu0RtcWdt | Cpu0Mwdt0 | Cpu0Mwdt1) => {
            esp_reset_reason_t::ESP_RST_WDT
        }
        Some(SysBrownOut) => esp_reset_reason_t::ESP_RST_BROWNOUT,
        Some(CoreUsbUart | CoreUsbJtag) => esp_reset_reason_t::ESP_RST_USB,
        _ => esp_reset_reason_t::ESP_RST_UNKNOWN,
    }
}

#[export]
fn esp_reset_reason() -> esp_reset_reason_t {
    critical_section::with(|cs| RESET_REASON.borrow(cs).get())
}

/// GCC calls this for C's `atomic_fetch_add`: the C3 has no atomic instructions. ESP-IDF's
/// libatomic does the same in a critical section on a single core.
#[unsafe(no_mangle)]
unsafe extern "C" fn __atomic_fetch_add_4(pointer: *mut u32, value: u32, _order: c_int) -> u32 {
    // SAFETY: C passes an aligned `atomic_uint`, and interrupts are off.
    critical_section::with(|_| unsafe {
        let old = pointer.read_volatile();
        pointer.write_volatile(old.wrapping_add(value));
        old
    })
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
