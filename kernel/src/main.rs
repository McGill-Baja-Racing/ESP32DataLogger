//! Rust implementation of the ESP-IDF, FreeRTOS and C library APIs the firmware links against.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use core::ffi::c_void;

use esp_backtrace as _;
use esp_hal::rtc_cntl::{Rtc, Rwdt, RwdtStage};

#[cfg(esp32c3)]
mod adc;
#[cfg_attr(esp32c3, path = "chip/esp32c3.rs")]
#[cfg_attr(esp32p4, path = "chip/esp32p4.rs")]
mod chip;
#[cfg(esp32p4)]
mod files;
#[cfg(esp32c3)]
mod gpio;
#[cfg(esp32c3)]
mod gptimer;
#[cfg(esp32p4)]
mod http;
#[cfg(esp32c3)]
mod i2c;
mod libc;
#[cfg(esp32p4)]
mod network;
mod rtos;
#[cfg(esp32p4)]
mod sdmmc;
#[cfg(esp32p4)]
mod storage;
mod system;
mod twai;
#[cfg(esp32p4)]
mod uart;

esp_bootloader_esp_idf::esp_app_desc!();

unsafe extern "C" {
    fn app_main();
}

// esp-backtrace's `custom-halt` hook: reset after printing the panic, like ESP-IDF.
#[unsafe(no_mangle)]
extern "Rust" fn custom_halt() -> ! {
    esp_hal::system::software_reset()
}

const WATCHDOG_STACK: usize = 2048;

/// Below every C task's priority, so a task that never blocks resets the chip.
extern "C" fn feed_watchdog(rwdt: *mut c_void) {
    // SAFETY: `main` leaked this `Rwdt` for this task alone.
    let rwdt = unsafe { &mut *rwdt.cast::<Rwdt>() };
    loop {
        rwdt.feed();
        esp_radio_rtos_driver::usleep(500_000);
    }
}

#[esp_hal::main]
fn main() -> ! {
    // ESP-IDF's default is the maximum clock; esp-hal's is lower.
    let config = esp_hal::Config::default().with_cpu_clock(esp_hal::clock::CpuClock::max());
    let peripherals = esp_hal::init(config);
    esp_alloc::heap_allocator!(size: chip::HEAP);
    let timg0 = esp_hal::timer::timg::TimerGroup::new(peripherals.TIMG0);
    esp_rtos::start(timg0.timer0, peripherals.FROM_CPU_INTR0);
    system::init();
    #[cfg(esp32p4)]
    chip::init();
    let mut rwdt = Rtc::new(peripherals.RTC_TIMER).rwdt;
    rwdt.set_timeout(RwdtStage::Stage0, esp_hal::time::Duration::from_secs(5));
    rwdt.enable();
    let rwdt = Box::into_raw(Box::new(rwdt)).cast();
    // SAFETY: the task is the only user of `rwdt`.
    unsafe {
        esp_radio_rtos_driver::task_create(
            "watchdog",
            feed_watchdog,
            rwdt,
            0,
            None,
            WATCHDOG_STACK + rtos::ISR_STACK,
        );
    }
    // ESP-IDF runs `app_main` at priority 1; esp-rtos starts the main task at 0.
    esp_rtos::CurrentThreadHandle::get().set_priority(1);
    // SAFETY: `app_main` is the firmware's entry point, takes no arguments and runs once.
    unsafe { app_main() };
    // ESP-IDF deletes the main task when `app_main` returns; esp-rtos can't, so it sleeps forever.
    loop {
        esp_radio_rtos_driver::usleep_until(u64::MAX);
    }
}
