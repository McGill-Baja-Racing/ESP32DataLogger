//! The ESP32-P4 master: its heap, PSRAM check and network error codes. It reads no reset reason.

use core::ffi::CStr;

use esp_hal::{
    peripherals::PSRAM,
    psram::{Psram, PsramConfig},
};
use sys::{
    ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED,
    ESP_ERR_HTTPD_HANDLER_EXISTS, ESP_ERR_HTTPD_HANDLERS_FULL, ESP_ERR_HTTPD_RESP_SEND,
    ESP_ERR_HTTPD_RESULT_TRUNC, esp_err_t,
};

/// Everything the firmware leaves of the SRAM but the main stack, which the linker checks.
pub const HEAP: usize = 372 * 1024;

/// The master board's PSRAM. esp-hal reports a missing or unrecognised one only in a log line.
const PSRAM_SIZE: usize = 32 * 1024 * 1024;

/// The network and HTTP codes the master returns.
pub fn error_name(code: esp_err_t) -> Option<&'static CStr> {
    Some(match code {
        ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED => c"ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED",
        ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED => c"ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED",
        ESP_ERR_HTTPD_HANDLERS_FULL => c"ESP_ERR_HTTPD_HANDLERS_FULL",
        ESP_ERR_HTTPD_HANDLER_EXISTS => c"ESP_ERR_HTTPD_HANDLER_EXISTS",
        ESP_ERR_HTTPD_RESULT_TRUNC => c"ESP_ERR_HTTPD_RESULT_TRUNC",
        ESP_ERR_HTTPD_RESP_SEND => c"ESP_ERR_HTTPD_RESP_SEND",
        _ => return None,
    })
}

/// A wrong or missing PSRAM is reported and boot goes on.
pub fn init() {
    // SAFETY: nothing else uses the PSRAM peripheral.
    let psram = Psram::new(unsafe { PSRAM::steal() }, PsramConfig::default());
    let (_start, size) = psram.raw_parts();
    if size != PSRAM_SIZE {
        crate::system::locked(|| {
            esp_println::println!("PSRAM is {size} bytes, not the master's {PSRAM_SIZE}");
        });
    }
}
