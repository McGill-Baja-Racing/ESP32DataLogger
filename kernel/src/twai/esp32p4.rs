//! The P4's TWAI controller. esp-hal has no driver for it, so this sets TWAI0 up as esp-hal does
//! for the C6, whose controller is the same and also runs from the 40 MHz crystal.
//!
//! Only standard data frames at 1 Mbit/s in normal mode, with every frame accepted.

use esp_hal::{
    gpio::{AnyPin, Flex, InputConfig, InputSignal, Level, OutputConfig, OutputSignal, Pull},
    interrupt::{self, InterruptHandler},
    peripherals::{HP_SYS_CLKRST, Interrupt, TWAI0},
};

use super::{ACK_SLOT, Events, Status};

/// Leaves the node in reset mode until `enable`.
///
/// # Safety
///
/// Nothing else may use TWAI0 or the pins.
pub unsafe fn start(tx: u8, rx: u8, handler: InterruptHandler) {
    let clock = HP_SYS_CLKRST::regs();
    clock
        .soc_clk_ctrl2()
        .modify(|_, w| w.twai0_apb_clk_en().set_bit());
    clock.peri_clk_ctrl115().modify(|_, w| {
        w.twai0_clk_en().set_bit();
        w.twai0_clk_src_sel().clear_bit()
    });
    clock.hp_rst_en1().modify(|_, w| w.rst_en_can0().set_bit());
    clock
        .hp_rst_en1()
        .modify(|_, w| w.rst_en_can0().clear_bit());

    let twai = TWAI0::regs();
    twai.mode().write(|w| {
        w.reset_mode().set_bit();
        w.acceptance_filter_mode().set_bit()
    });
    // In reset mode the data registers hold the acceptance code, then the mask: accept all.
    for index in 0..8 {
        // SAFETY: the registers take any byte.
        twai.data(index)
            .write(|w| unsafe { w.tx_byte().bits(if index < 4 { 0 } else { 0xff }) });
    }
    // SAFETY: the caller has the pins to itself.
    let (tx, rx) = unsafe { (AnyPin::steal(tx), AnyPin::steal(rx)) };
    let mut tx = Flex::new(tx);
    tx.apply_output_config(&OutputConfig::default());
    // Until the controller's signal is routed the pin is a plain GPIO, and low is dominant.
    tx.set_level(Level::High);
    tx.set_output_enable(true);
    OutputSignal::TWAI0_TX.connect_to(&tx.into_peripheral_output());
    let mut rx = Flex::new(rx);
    rx.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
    InputSignal::TWAI0_RX.connect_to(&rx.peripheral_input());

    // SAFETY: the registers take any value. Each holds its value minus one: from 40 MHz, a
    // prescaler of 2, a jump width of 3 and segments of 15 and 4 quanta give 1 Mbit/s.
    unsafe {
        twai.tx_err_cnt().write(|w| w.tx_err_cnt().bits(0));
        twai.rx_err_cnt().write(|w| w.rx_err_cnt().bits(0));
        twai.err_warning_limit()
            .write(|w| w.err_warning_limit().bits(96));
        twai.bus_timing_0().modify(|_, w| {
            w.baud_presc().bits(0);
            w.sync_jump_width().bits(2)
        });
        twai.bus_timing_1().modify(|_, w| {
            w.time_segment1().bits(14);
            w.time_segment2().bits(3);
            w.time_sampling().clear_bit()
        });
    }
    twai.clock_divider().modify(|_, w| w.clock_off().set_bit());

    interrupt::bind_handler(Interrupt::TWAI0, handler);
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
        bus_off: status.node_bus_off().bit_is_set(),
        tx_free: status.transmit_buffer().bit_is_set(),
        rx_pending: status.receive_buffer().bit_is_set(),
        missed: status.miss().bit_is_set(),
    }
}

pub fn request_transmit() {
    TWAI0::regs().cmd().write(|w| w.tx_request().set_bit());
}

pub fn release_frame() {
    TWAI0::regs().cmd().write(|w| w.release_buffer().set_bit());
}

/// Whether an error counter is at or over the warning limit.
pub fn warning() -> bool {
    TWAI0::regs().status().read().err().bit_is_set()
}

pub fn abort_tx() {
    TWAI0::regs().cmd().write(|w| w.abort_tx().set_bit());
}

/// Whether the last bus error was a transmitted frame nobody acknowledged. Reading the capture
/// also re-arms it.
pub fn ack_error() -> bool {
    let error = TWAI0::regs().err_code_cap().read();
    error.err_capture_code_segment().bits() == ACK_SLOT
        && error.err_capture_code_direction().bit_is_clear()
}

pub fn clear_overrun() {
    TWAI0::regs()
        .cmd()
        .write(|w| w.clear_data_overrun().set_bit());
}

/// Reading clears the flags, and reading the capture re-arms the arbitration-lost interrupt.
pub fn events() -> Events {
    let events = TWAI0::regs().interrupt().read();
    let _ = TWAI0::regs().arb_lost_cap().read();
    Events {
        bus_error: events.bus_err_int_st().bit_is_set(),
        arbitration_lost: events.arbitration_lost_int_st().bit_is_set(),
        state_change: events.err_warning_int_st().bit_is_set()
            || events.err_passive_int_st().bit_is_set(),
        receive: events.receive_int_st().bit_is_set(),
        transmit: events.transmit_int_st().bit_is_set(),
    }
}

/// Leaves reset mode, with the interrupts on.
pub fn enable() {
    let twai = TWAI0::regs();
    // Reading clears the flags.
    let _ = twai.interrupt().read();
    twai.interrupt_enable().write(|w| {
        w.ext_receive_int_ena().set_bit();
        w.ext_transmit_int_ena().set_bit();
        w.ext_err_warning_int_ena().set_bit();
        w.err_passive_int_ena().set_bit();
        w.arbitration_lost_int_ena().set_bit();
        w.bus_err_int_ena().set_bit()
    });
    twai.mode().modify(|_, w| w.reset_mode().clear_bit());
}
