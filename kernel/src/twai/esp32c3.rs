//! The C3's TWAI controller, on esp-hal's `twai` and its registers.

use esp_hal::{
    gpio::{AnyPin, Flex, InputConfig, Pull},
    interrupt::InterruptHandler,
    peripherals::TWAI0,
    twai::{BaudRate, TwaiConfiguration, TwaiMode},
};

use super::{Events, Status};

/// Leaves the node in reset mode until `enable`.
///
/// # Safety
///
/// Nothing else may use TWAI0 or the pins.
pub unsafe fn start(tx: u8, rx: u8, handler: InterruptHandler) {
    // SAFETY: the caller has TWAI0 and the pins to itself.
    let (twai, rx_pin, tx_pin) = unsafe { (TWAI0::steal(), AnyPin::steal(rx), AnyPin::steal(tx)) };
    let mut setup =
        TwaiConfiguration::new(twai, rx_pin, tx_pin, BaudRate::B1000K, TwaiMode::Normal);
    setup.set_interrupt_handler(handler);
    // esp-hal's `start` also sets the acceptance filter and mode, which only reset mode allows; the
    // driver stays alive for good to keep the peripheral's clock on.
    core::mem::forget(setup.start());
    TWAI0::regs().mode().modify(|_, w| w.reset_mode().set_bit());
    // esp-hal leaves RX floating, where ESP-IDF pulls it up against an unplugged transceiver.
    // SAFETY: as above.
    let mut rx = Flex::new(unsafe { AnyPin::steal(rx) });
    rx.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
    rx.set_input_enable(true);
}

pub fn tx_errors() -> u8 {
    TWAI0::regs().tx_err_cnt().read().tx_err_cnt().bits()
}

pub fn rx_errors() -> u8 {
    TWAI0::regs().rx_err_cnt().read().rx_err_cnt().bits()
}

/// The controller recovers once it has seen 128 sequences of 11 idle bits.
pub fn recover() {
    // Bus-off leaves it in reset mode; leaving that mode starts the recovery.
    let mode = TWAI0::regs().mode();
    mode.modify(|_, w| w.reset_mode().set_bit());
    mode.modify(|_, w| w.reset_mode().clear_bit());
}

pub fn status() -> Status {
    let status = TWAI0::regs().status().read();
    Status {
        bus_off: status.bus_off_st().bit_is_set(),
        tx_free: status.tx_buf_st().bit_is_set(),
        rx_pending: status.rx_buf_st().bit_is_set(),
        missed: status.miss_st().bit_is_set(),
    }
}

pub fn request_transmit() {
    TWAI0::regs().cmd().write(|w| w.tx_req().set_bit());
}

pub fn release_frame() {
    TWAI0::regs().cmd().write(|w| w.release_buf().set_bit());
}

/// Whether an error counter is at or over the warning limit.
pub fn warning() -> bool {
    TWAI0::regs().status().read().err_st().bit_is_set()
}

pub fn abort_tx() {
    TWAI0::regs().cmd().write(|w| w.abort_tx().set_bit());
}

/// The type and bit position of the last bus error. Reading the capture also re-arms it.
pub fn error_capture() -> (u8, u8) {
    let error = TWAI0::regs().err_code_cap().read();
    (error.ecc_type().bits(), error.ecc_segment().bits())
}

/// Whether the node is in reset mode, where the acceptance filter can change.
pub fn stopped() -> bool {
    TWAI0::regs().mode().read().reset_mode().bit_is_set()
}

/// Sets one filter over the frame's first 32 bits. A set `mask` bit must match `code`.
pub fn set_filter(code: u32, mask: u32) {
    let twai = TWAI0::regs();
    twai.mode().modify(|_, w| w.rx_filter_mode().set_bit());
    // The hardware's mask marks the bits it ignores.
    let bytes = code.to_be_bytes().into_iter().chain((!mask).to_be_bytes());
    for (index, byte) in bytes.enumerate() {
        // SAFETY: in reset mode the data registers hold the acceptance code and mask.
        twai.data(index)
            .write(|w| unsafe { w.tx_byte().bits(byte) });
    }
}

pub fn clear_overrun() {
    TWAI0::regs().cmd().write(|w| w.clr_overrun().set_bit());
}

/// Reading clears the flags, and reading the capture re-arms the arbitration-lost interrupt.
pub fn events() -> Events {
    let events = TWAI0::regs().int_raw().read();
    let _ = TWAI0::regs().arb_lost_cap().read();
    Events {
        bus_error: events.bus_err_int_st().bit_is_set(),
        arbitration_lost: events.arb_lost_int_st().bit_is_set(),
        state_change: events.err_warn_int_st().bit_is_set()
            || events.err_passive_int_st().bit_is_set(),
        receive: events.rx_int_st().bit_is_set(),
        transmit: events.tx_int_st().bit_is_set(),
    }
}

/// Leaves reset mode, with the interrupts on.
pub fn enable() {
    let twai = TWAI0::regs();
    // Reading clears the flags.
    let _ = twai.int_raw().read();
    twai.int_ena().write(|w| {
        w.rx_int_ena().set_bit();
        w.tx_int_ena().set_bit();
        w.err_warn_int_ena().set_bit();
        w.err_passive_int_ena().set_bit();
        w.arb_lost_int_ena().set_bit();
        w.bus_err_int_ena().set_bit()
    });
    twai.mode().modify(|_, w| w.reset_mode().clear_bit());
}
