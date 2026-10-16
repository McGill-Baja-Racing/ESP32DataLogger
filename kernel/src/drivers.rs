//! Layer 3: ESP-IDF's driver APIs on top of esp-hal.

pub mod adc;

use core::cell::Cell;

use critical_section::Mutex;
use esp_hal::gpio::{AnyPin, Pin as _};
use sys::{ESP_ERR_INVALID_ARG, esp_err_t};

pub const GPIO_COUNT: usize = 22;

/// The GPIOs no driver has claimed yet, by number. C names pins by number, so esp-hal's one
/// owner per pin is enforced here at run time; ESP-IDF would let two drivers share a pin.
static GPIOS: [Mutex<Cell<Option<AnyPin<'static>>>>; GPIO_COUNT] =
    [const { Mutex::new(Cell::new(None)) }; GPIO_COUNT];

pub fn init(gpios: [AnyPin<'static>; GPIO_COUNT]) {
    critical_section::with(|cs| {
        for pin in gpios {
            if let Some(slot) = GPIOS.get(usize::from(pin.number())) {
                slot.borrow(cs).set(Some(pin));
            }
        }
    });
}

/// Claimed GPIOs are never given back.
#[expect(
    clippy::panic,
    clippy::panic_in_result_fn,
    reason = "two drivers on one pin is a firmware bug, and the C usually ignores errors"
)]
pub fn claim(number: u8) -> Result<AnyPin<'static>, esp_err_t> {
    let slot = GPIOS.get(usize::from(number)).ok_or(ESP_ERR_INVALID_ARG)?;
    let pin = critical_section::with(|cs| slot.borrow(cs).take());
    Ok(pin.unwrap_or_else(|| panic!("GPIO{number} is claimed by two drivers")))
}
