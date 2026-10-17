//! Rust implementation of the ESP-IDF, FreeRTOS and C library APIs the firmware links against.

#![no_std]
#![no_main]

extern crate alloc;

use esp_backtrace as _;

mod drivers;
mod rtos;
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
    let peripherals = esp_hal::init(config);
    esp_println::logger::init_logger(log::LevelFilter::Info);
    esp_alloc::heap_allocator!(size: 64 * 1024);
    let timg0 = esp_hal::timer::timg::TimerGroup::new(peripherals.TIMG0);
    esp_rtos::start(timg0.timer0, peripherals.FROM_CPU_INTR0);
    system::init();
    // ESP-IDF runs `app_main` at priority 1; esp-rtos starts the main task at 0.
    esp_rtos::CurrentThreadHandle::get().set_priority(1);
    // SAFETY: `app_main` is the firmware's entry point, takes no arguments and runs once.
    unsafe { app_main() };
    // ESP-IDF deletes the main task when `app_main` returns; esp-rtos can't, so it sleeps forever.
    loop {
        esp_radio_rtos_driver::usleep_until(u64::MAX);
    }
}
