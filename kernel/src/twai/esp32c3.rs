//! The C3's TWAI controller, on esp-hal's `twai` and its registers.

use esp_hal::{
    Blocking,
    gpio::{AnyPin, Flex, InputConfig, Pull},
    interrupt::InterruptHandler,
    peripherals::TWAI0,
    twai::{BaudRate, Twai, TwaiConfiguration, TwaiMode},
};

use super::{Events, Status};

/// The bit position `ERR_CODE_CAP` reports for an error in the ACK slot.
const ACK_SLOT: u8 = 25;

pub struct Controller(Twai<'static, Blocking>);

impl Controller {
    /// Starts the node at 1 Mbit/s on the given pins, with `handler` for its interrupt.
    ///
    /// # Safety
    ///
    /// Nothing else may use TWAI0 or the pins.
    pub unsafe fn start(tx: u8, rx: u8, handler: InterruptHandler) -> Self {
        // SAFETY: the caller has TWAI0 and the pins to itself.
        let (twai, rx_pin, tx_pin) =
            unsafe { (TWAI0::steal(), AnyPin::steal(rx), AnyPin::steal(tx)) };
        let mut setup =
            TwaiConfiguration::new(twai, rx_pin, tx_pin, BaudRate::B1000K, TwaiMode::Normal);
        setup.set_interrupt_handler(handler);
        let twai = setup.start();
        // esp-hal leaves RX floating, where ESP-IDF pulls it up against an unplugged transceiver.
        // SAFETY: as above.
        let mut rx = Flex::new(unsafe { AnyPin::steal(rx) });
        rx.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
        rx.set_input_enable(true);
        Self(twai)
    }

    pub fn is_bus_off(&self) -> bool {
        self.0.is_bus_off()
    }

    pub fn tx_errors(&self) -> u8 {
        self.0.transmit_error_count()
    }

    pub fn rx_errors(&self) -> u8 {
        self.0.receive_error_count()
    }

    pub fn initiate_recovery(&mut self) {
        self.0.initiate_recovery();
    }
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

/// Whether the last bus error was a transmitted frame nobody acknowledged. Reading the capture
/// also re-arms it.
pub fn ack_error() -> bool {
    let error = TWAI0::regs().err_code_cap().read();
    error.ecc_segment().bits() == ACK_SLOT && error.ecc_direction().bit_is_clear()
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

pub fn enable_interrupts() {
    TWAI0::regs().int_ena().write(|w| {
        w.rx_int_ena().set_bit();
        w.tx_int_ena().set_bit();
        w.err_warn_int_ena().set_bit();
        w.err_passive_int_ena().set_bit();
        w.arb_lost_int_ena().set_bit();
        w.bus_err_int_ena().set_bit()
    });
}
