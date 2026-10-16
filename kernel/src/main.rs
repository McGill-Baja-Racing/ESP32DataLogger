//! Rust implementation of the ESP-IDF, FreeRTOS and C library APIs the firmware links against.

#![no_std]
#![no_main]

use esp_backtrace as _;

mod drivers;
mod rtos;
mod services;
mod system;

esp_bootloader_esp_idf::esp_app_desc!();

unsafe extern "C" {
    fn app_main();
}

// esp-backtrace's `custom-halt` hook: reset after printing the panic, like ESP-IDF.
#[unsafe(no_mangle)]
extern "Rust" fn custom_halt() -> ! {
    esp_hal::system::software_reset()
}

#[esp_hal::main]
fn main() -> ! {
    // ESP-IDF's default is the maximum clock; esp-hal's is lower.
    let config = esp_hal::Config::default().with_cpu_clock(esp_hal::clock::CpuClock::max());
    let _peripherals = esp_hal::init(config);
    // SAFETY: `app_main` is the firmware's entry point, takes no arguments and runs once.
    unsafe { app_main() };
    loop {
        core::hint::spin_loop();
    }
}
