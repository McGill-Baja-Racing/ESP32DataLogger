//! ESP-IDF's GPIO driver and its per-pin interrupt service, on esp-hal's `Flex`. Only what the
//! firmware uses: input and output pins, pull-up, and rising or either-edge interrupts.
//!
//! A configured pin keeps its `Flex` for good: `Flex::new` resets the pad.

use core::{
    cell::{Cell, RefCell},
    ffi::{c_int, c_void},
};

use critical_section::Mutex;
use esp_hal::{
    gpio::{AnyPin, Event, Flex, InputConfig, Io, Level, OutputConfig, Pull},
    handler,
    peripherals::IO_MUX,
};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, SOC_GPIO_PIN_COUNT, esp_err_t, gpio_config_t,
    gpio_int_type_t, gpio_isr_t, gpio_mode_t, gpio_num_t, gpio_pullup_t,
};

use crate::rtos::{self, Context};

struct Pin {
    flex: Flex<'static>,
    event: Option<Event>,
    isr: Option<(unsafe extern "C" fn(*mut c_void), Context)>,
}

impl Pin {
    fn listen(&mut self) {
        match self.event {
            Some(event) => self.flex.listen(event),
            None => self.flex.unlisten(),
        }
    }
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "the chip has fewer than 256 pins"
)]
const PIN_COUNT: u8 = SOC_GPIO_PIN_COUNT as u8;

static PINS: Mutex<RefCell<[Option<Pin>; PIN_COUNT as usize]>> =
    Mutex::new(RefCell::new([const { None }; PIN_COUNT as usize]));

static INSTALLED: Mutex<Cell<bool>> = Mutex::new(Cell::new(false));

/// Runs `f` on a pin `gpio_config` or `gpio_reset_pin` has set up.
#[expect(clippy::panic, reason = "an unconfigured pin is a firmware bug")]
fn with_pin<T>(gpio_num: gpio_num_t, f: impl FnOnce(&mut Pin) -> T) -> T {
    critical_section::with(|cs| {
        let mut pins = PINS.borrow_ref_mut(cs);
        let Some(pin) = (usize::try_from(gpio_num.0).ok())
            .and_then(|number| pins.get_mut(number))
            .and_then(Option::as_mut)
        else {
            panic!("configure GPIO{} with gpio_config first", gpio_num.0);
        };
        f(pin)
    })
}

/// Starts the pin's interrupt here, so none can fire while it is missing from `PINS`.
fn insert(number: u8, flex: Flex<'static>, event: Option<Event>) {
    critical_section::with(|cs| {
        if let Some(slot) = PINS.borrow_ref_mut(cs).get_mut(usize::from(number)) {
            let isr = slot.take().and_then(|old| old.isr);
            slot.insert(Pin { flex, event, isr }).listen();
        }
    });
}

#[export]
fn gpio_config(config: &gpio_config_t) -> Result<(), esp_err_t> {
    let mask = config.pin_bit_mask;
    if mask == 0 || mask >> SOC_GPIO_PIN_COUNT != 0 {
        return Err(ESP_ERR_INVALID_ARG);
    }
    // `unsupported.h` rejects the other modes, pulls and interrupt types by name.
    let output = config.mode == gpio_mode_t::GPIO_MODE_OUTPUT;
    let event = match config.intr_type {
        gpio_int_type_t::GPIO_INTR_DISABLE => None,
        gpio_int_type_t::GPIO_INTR_POSEDGE => Some(Event::RisingEdge),
        _ => Some(Event::AnyEdge),
    };
    let pull = if config.pull_up_en == gpio_pullup_t::GPIO_PULLUP_ENABLE {
        Pull::Up
    } else {
        Pull::None
    };
    for number in (0..PIN_COUNT).filter(|number| mask >> number & 1 != 0) {
        // SAFETY: C assigns pins, as in ESP-IDF; the firmware gives each GPIO to one driver.
        let mut flex = Flex::new(unsafe { AnyPin::steal(number) });
        if output {
            flex.apply_output_config(&OutputConfig::default().with_pull(pull));
            flex.set_output_enable(true);
        } else {
            flex.apply_input_config(&InputConfig::default().with_pull(pull));
            flex.set_input_enable(true);
        }
        // ESP-IDF enables the interrupt here, and the encoder never calls `gpio_intr_enable`.
        insert(number, flex, event);
    }
    Ok(())
}

/// Leaves the pad as ESP-IDF does: input and output off, pull-up on, no interrupt.
#[export]
fn gpio_reset_pin(gpio_num: gpio_num_t) -> Result<(), esp_err_t> {
    let number = rtos::gpio_number(gpio_num.0)?;
    // SAFETY: as in `gpio_config`.
    let mut flex = Flex::new(unsafe { AnyPin::steal(number) });
    flex.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
    insert(number, flex, None);
    Ok(())
}

/// Dispatches like ESP-IDF's `gpio_isr_loop`: an edge during a handler is not lost, because
/// the status is cleared first. `PINS` is not borrowed while C runs, since its handlers read pins.
#[handler]
fn interrupt() {
    let fired = critical_section::with(|cs| {
        PINS.borrow_ref_mut(cs).each_mut().map(|pin| {
            let pin = pin.as_mut().filter(|pin| pin.flex.is_interrupt_set())?;
            pin.flex.clear_interrupt();
            pin.isr
        })
    });
    for (handler, Context(argument)) in fired.into_iter().flatten() {
        // SAFETY: C registered `handler` with `argument` for this pin, to run in this interrupt.
        unsafe { handler(argument) };
    }
}

#[export]
fn gpio_install_isr_service(flags: c_int) -> Result<(), esp_err_t> {
    assert!(
        flags == 0,
        "the kernel supports only default interrupt flags"
    );
    if critical_section::with(|cs| INSTALLED.borrow(cs).replace(true)) {
        return Err(ESP_ERR_INVALID_STATE);
    }
    // SAFETY: `INSTALLED` lets this run once, and nothing else uses the IO MUX interrupt.
    Io::new(unsafe { IO_MUX::steal() }).set_interrupt_handler(interrupt);
    Ok(())
}

#[export]
fn gpio_isr_handler_add(
    gpio_num: gpio_num_t,
    isr_handler: gpio_isr_t,
    args: *mut c_void,
) -> Result<(), esp_err_t> {
    let handler = isr_handler.ok_or(ESP_ERR_INVALID_ARG)?;
    if !critical_section::with(|cs| INSTALLED.borrow(cs).get()) {
        return Err(ESP_ERR_INVALID_STATE);
    }
    with_pin(gpio_num, |pin| {
        pin.isr = Some((handler, Context(args)));
        pin.listen();
    });
    Ok(())
}

#[export]
fn gpio_intr_enable(gpio_num: gpio_num_t) -> Result<(), esp_err_t> {
    with_pin(gpio_num, Pin::listen);
    Ok(())
}

#[export]
fn gpio_get_level(gpio_num: gpio_num_t) -> c_int {
    with_pin(gpio_num, |pin| c_int::from(pin.flex.is_high()))
}

#[export]
fn gpio_set_level(gpio_num: gpio_num_t, level: u32) -> Result<(), esp_err_t> {
    with_pin(gpio_num, |pin| pin.flex.set_level(Level::from(level != 0)));
    Ok(())
}
