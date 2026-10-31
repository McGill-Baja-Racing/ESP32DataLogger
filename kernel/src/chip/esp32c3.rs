//! The ESP32-C3 sensor nodes: their heap, and the reset reason and atomics that ESP-IDF's C3 port
//! gives their firmware.

use core::ffi::{CStr, c_int};

use esp_hal::rtc_cntl::SocResetReason;
use macros::export;
use portable_atomic::{AtomicU32, Ordering};
use sys::{esp_err_t, esp_reset_reason_t};

/// Everything the firmware leaves of the SRAM but the main stack, which the linker checks.
pub const HEAP: usize = 296 * 1024;

/// The sensor nodes return no codes besides the common ones.
pub fn error_name(_code: esp_err_t) -> Option<&'static CStr> {
    None
}

/// ESP-IDF's table for the C3, which counts an eFuse CRC reset as power-on. Only a panic resets
/// in software.
#[export]
fn esp_reset_reason() -> esp_reset_reason_t {
    use SocResetReason::{
        ChipPowerOn, CoreDeepSleep, CoreEfuseCrc, CoreMwdt0, CoreMwdt1, CoreRtcWdt, CoreSw,
        CoreUsbJtag, CoreUsbUart, Cpu0Mwdt0, Cpu0Mwdt1, Cpu0RtcWdt, Cpu0Sw, SysBrownOut, SysRtcWdt,
        SysSuperWdt,
    };
    match esp_hal::system::reset_reason() {
        Some(ChipPowerOn | CoreEfuseCrc) => esp_reset_reason_t::ESP_RST_POWERON,
        Some(CoreSw | Cpu0Sw) => esp_reset_reason_t::ESP_RST_PANIC,
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

/// Clang calls this for C's `atomic_fetch_add` because the C3 has no atomic instructions.
#[unsafe(no_mangle)]
unsafe extern "C" fn __atomic_fetch_add_4(pointer: *mut u32, value: u32, _order: c_int) -> u32 {
    // SAFETY: C passes an aligned `atomic_uint` that it only accesses atomically.
    unsafe { AtomicU32::from_ptr(pointer) }.fetch_add(value, Ordering::SeqCst)
}

/// Clang calls this for C's `atomic_load` because the C3 has no atomic instructions.
#[unsafe(no_mangle)]
unsafe extern "C" fn __atomic_load_4(pointer: *mut u32, _order: c_int) -> u32 {
    // SAFETY: C passes an aligned `atomic_uint` that it only accesses atomically.
    unsafe { AtomicU32::from_ptr(pointer) }.load(Ordering::SeqCst)
}
