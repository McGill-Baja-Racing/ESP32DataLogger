//! ESP-IDF's on-chip TWAI (CAN) node driver, on esp-hal's `twai`. Only what the firmware uses: one
//! node sending standard data frames at 1 Mbit/s.
//!
//! esp-hal's controller retries a failed frame, where ESP-IDF's driver with the firmware's zeroed
//! `fail_retry_cnt` sends each frame once.

use alloc::boxed::Box;
use core::{
    cell::{Cell, RefCell},
    ffi::{c_int, c_void},
    mem::MaybeUninit,
    ptr, slice,
};

use critical_section::Mutex;
use esp_hal::{
    Blocking,
    gpio::AnyPin,
    handler,
    peripherals::TWAI0,
    twai::{BaudRate, EspTwaiFrame, Id, StandardId, Twai, TwaiConfiguration, TwaiMode},
};
use esp_radio_rtos_driver::{
    self as rtos,
    semaphore::{SemaphoreHandle, SemaphoreKind},
};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, esp_err_t,
    twai_error_state_t, twai_event_callbacks_t, twai_frame_t, twai_node_handle_t,
    twai_node_record_t, twai_node_status_t, twai_onchip_node_config_t, twai_rx_done_event_data_t,
    twai_state_change_event_data_t,
};

/// The bit position `ERR_CODE_CAP` reports for an error in the ACK slot.
const ACK_SLOT: u8 = 25;

/// The pointer C registered with its callbacks.
#[derive(Clone, Copy)]
struct Context(*mut c_void);

// SAFETY: the kernel never dereferences it; it only passes it back to C's callbacks.
unsafe impl Send for Context {}

struct Inner {
    twai: Twai<'static, Blocking>,
    state: twai_error_state_t,
    bus_errors: u32,
    /// The frame `twai_node_receive_from_isr` reads, while `on_rx_done` runs.
    rx: Option<EspTwaiFrame>,
    callbacks: Option<(twai_event_callbacks_t, Context)>,
}

struct Node {
    inner: Mutex<RefCell<Inner>>,
    /// Given by the interrupt when the TX buffer frees up or the node goes bus-off.
    idle: SemaphoreHandle,
}

sys::c_handle!(Node, twai_node_handle_t);

/// The node, for the interrupt; it also keeps a second node from taking TWAI0.
static NODE: Mutex<Cell<Option<&'static Node>>> = Mutex::new(Cell::new(None));

fn state(twai: &Twai<'_, Blocking>) -> twai_error_state_t {
    if twai.is_bus_off() {
        twai_error_state_t::TWAI_ERROR_BUS_OFF
    } else if twai.transmit_error_count() >= 128 || twai.receive_error_count() >= 128 {
        twai_error_state_t::TWAI_ERROR_PASSIVE
    } else if TWAI0::regs().status().read().err_st().bit_is_set() {
        twai_error_state_t::TWAI_ERROR_WARNING
    } else {
        twai_error_state_t::TWAI_ERROR_ACTIVE
    }
}

impl Node {
    /// Also wakes `wait_free` itself, in case the abort raises no TX interrupt; waiters recheck
    /// the buffer, so an early wake is harmless.
    fn abort_transmission(&self) {
        TWAI0::regs().cmd().write(|w| w.abort_tx().set_bit());
        self.idle.try_give_from_isr(None);
    }

    fn handle(&self) -> twai_node_handle_t {
        ptr::from_ref(self).cast_mut().cast()
    }

    /// Waits until the TX buffer is free; `-1` waits forever, like ESP-IDF.
    fn wait_free(&self, timeout_ms: c_int) -> Result<(), esp_err_t> {
        let deadline = u64::try_from(timeout_ms)
            .ok()
            .map(|ms| rtos::now() + ms * 1000);
        loop {
            let status = TWAI0::regs().status().read();
            if status.bus_off_st().bit_is_set() {
                return Err(ESP_ERR_INVALID_STATE);
            }
            if status.tx_buf_st().bit_is_set() {
                return Ok(());
            }
            if !self.idle.take_with_deadline(deadline) {
                return Err(ESP_ERR_TIMEOUT);
            }
        }
    }

    /// Error-passive stops the TEC from rising on ACK errors, so the controller would retry a
    /// frame nobody acknowledges forever; this gives up on it instead.
    fn bus_error(&self) {
        // Reading the capture also re-arms it.
        let error = TWAI0::regs().err_code_cap().read();
        let passive = critical_section::with(|cs| {
            let mut inner = self.inner.borrow_ref_mut(cs);
            inner.bus_errors += 1;
            inner.twai.transmit_error_count() >= 128
        });
        let unacknowledged =
            error.ecc_segment().bits() == ACK_SLOT && error.ecc_direction().bit_is_clear();
        if passive && unacknowledged {
            self.abort_transmission();
        }
    }

    fn state_change(&self) {
        let changed = critical_section::with(|cs| {
            let mut inner = self.inner.borrow_ref_mut(cs);
            let new_sta = state(&inner.twai);
            let old_sta = core::mem::replace(&mut inner.state, new_sta);
            let event = twai_state_change_event_data_t { old_sta, new_sta };
            (old_sta != new_sta).then_some((event, inner.callbacks))
        });
        let Some((event, callbacks)) = changed else {
            return;
        };
        if event.new_sta == twai_error_state_t::TWAI_ERROR_BUS_OFF {
            self.abort_transmission();
        }
        if let Some((callbacks, context)) = callbacks
            && let Some(callback) = callbacks.on_state_change
        {
            // SAFETY: C registered `callback` with `context` for this node, to run in its interrupt.
            unsafe { callback(self.handle(), &raw const event, context.0) };
        }
    }

    fn receive(&self) {
        loop {
            let (received, callbacks) = critical_section::with(|cs| {
                let mut inner = self.inner.borrow_ref_mut(cs);
                inner.rx = inner.twai.receive().ok();
                (inner.rx.is_some(), inner.callbacks)
            });
            if !received {
                break;
            }
            if let Some((callbacks, context)) = callbacks
                && let Some(callback) = callbacks.on_rx_done
            {
                // SAFETY: as in `state_change`.
                unsafe { callback(self.handle(), &twai_rx_done_event_data_t {}, context.0) };
            }
        }
        // esp-hal's `receive` reports an overrun until it's cleared, which its
        // `clear_receive_fifo` doesn't do.
        if TWAI0::regs().status().read().miss_st().bit_is_set() {
            critical_section::with(|cs| self.inner.borrow_ref(cs).twai.clear_receive_fifo());
            TWAI0::regs().cmd().write(|w| w.clr_overrun().set_bit());
        }
    }
}

#[handler]
fn interrupt() {
    let Some(node) = critical_section::with(|cs| NODE.borrow(cs).get()) else {
        return;
    };
    // Reading clears the flags.
    let events = TWAI0::regs().int_raw().read();
    if events.bus_err_int_st().bit_is_set() {
        node.bus_error();
    }
    if events.err_warn_int_st().bit_is_set() || events.err_passive_int_st().bit_is_set() {
        node.state_change();
    }
    if events.rx_int_st().bit_is_set() {
        node.receive();
    }
    if events.tx_int_st().bit_is_set() {
        node.idle.try_give_from_isr(None);
    }
}

/// `tx_queue_depth` is ignored: there is one TX buffer, which `twai_node_transmit` waits for.
/// ESP-IDF would route the zeroed `quanta_clk_out` and `bus_off_indicator` to GPIO0; this doesn't.
#[export]
fn twai_new_node_onchip(
    config: &twai_onchip_node_config_t,
    node: &mut MaybeUninit<&'static Node>,
) -> Result<(), esp_err_t> {
    assert!(
        config.bit_timing.bitrate == 1_000_000,
        "the kernel supports only 1 Mbit/s"
    );
    let pin = |number: i32| u8::try_from(number).map_err(|_negative| ESP_ERR_INVALID_ARG);
    let (tx, rx) = (pin(config.io_cfg.tx.0)?, pin(config.io_cfg.rx.0)?);
    critical_section::with(|cs| {
        let slot = NODE.borrow(cs);
        if slot.get().is_some() {
            return Err(ESP_ERR_NOT_FOUND);
        }
        // SAFETY: `NODE` lets this run once, and nothing else uses TWAI0. C assigns pins, as in
        // ESP-IDF; the firmware gives each GPIO to one driver.
        let (twai, rx, tx) = unsafe { (TWAI0::steal(), AnyPin::steal(rx), AnyPin::steal(tx)) };
        let mut setup = TwaiConfiguration::new(twai, rx, tx, BaudRate::B1000K, TwaiMode::Normal);
        setup.set_interrupt_handler(interrupt);
        let new = Box::leak(Box::new(Node {
            inner: Mutex::new(RefCell::new(Inner {
                twai: setup.start(),
                state: twai_error_state_t::TWAI_ERROR_ACTIVE,
                bus_errors: 0,
                rx: None,
                callbacks: None,
            })),
            idle: SemaphoreHandle::new(SemaphoreKind::Counting { max: 1, initial: 0 }),
        }));
        slot.set(Some(new));
        node.write(new);
        Ok(())
    })
}

#[export]
fn twai_node_register_event_callbacks(
    node: &Node,
    callbacks: &twai_event_callbacks_t,
    context: *mut c_void,
) -> Result<(), esp_err_t> {
    critical_section::with(|cs| {
        node.inner.borrow_ref_mut(cs).callbacks = Some((*callbacks, Context(context)));
    });
    Ok(())
}

/// The interrupt, and so every callback, starts here.
#[export]
fn twai_node_enable(_node: &Node) -> Result<(), esp_err_t> {
    TWAI0::regs().int_ena().write(|w| {
        w.rx_int_ena().set_bit();
        w.tx_int_ena().set_bit();
        w.err_warn_int_ena().set_bit();
        w.err_passive_int_ena().set_bit();
        w.bus_err_int_ena().set_bit()
    });
    Ok(())
}

#[export]
fn twai_node_transmit(
    node: &Node,
    frame: &twai_frame_t,
    timeout_ms: c_int,
) -> Result<(), esp_err_t> {
    let format = frame.header.__bindgen_anon_1;
    assert!(
        format.ide() == 0 && format.rtr() == 0,
        "the kernel supports only standard data frames"
    );
    let payload = if frame.buffer_len == 0 {
        &[]
    } else if frame.buffer.is_null() {
        return Err(ESP_ERR_INVALID_ARG);
    } else {
        // SAFETY: C passes `buffer_len` bytes at `buffer`.
        unsafe { slice::from_raw_parts(frame.buffer, frame.buffer_len) }
    };
    let id = u16::try_from(frame.header.id)
        .ok()
        .and_then(StandardId::new)
        .ok_or(ESP_ERR_INVALID_ARG)?;
    let frame = EspTwaiFrame::new(id, payload).ok_or(ESP_ERR_INVALID_ARG)?;
    node.wait_free(timeout_ms)?;
    critical_section::with(|cs| node.inner.borrow_ref_mut(cs).twai.transmit(&frame))
        .map_err(|_bus_off| ESP_ERR_INVALID_STATE)
}

#[export]
fn twai_node_transmit_wait_all_done(node: &Node, timeout_ms: c_int) -> Result<(), esp_err_t> {
    node.wait_free(timeout_ms)
}

/// Only valid in `on_rx_done`, as in ESP-IDF.
#[export]
fn twai_node_receive_from_isr(node: &Node, frame: &mut twai_frame_t) -> Result<(), esp_err_t> {
    let buffer = if frame.buffer_len == 0 {
        &mut []
    } else if frame.buffer.is_null() {
        return Err(ESP_ERR_INVALID_ARG);
    } else {
        // SAFETY: C gives room for `buffer_len` bytes at `buffer`.
        unsafe { slice::from_raw_parts_mut(frame.buffer, frame.buffer_len) }
    };
    let received =
        critical_section::with(|cs| node.inner.borrow_ref(cs).rx).ok_or(ESP_ERR_INVALID_STATE)?;
    let (id, extended) = match received.identifier() {
        Id::Standard(id) => (u32::from(id.as_raw()), false),
        Id::Extended(id) => (id.as_raw(), true),
    };
    frame.header.id = id;
    frame.header.dlc = u16::try_from(received.data_length_code()).unwrap_or_default();
    frame.header.__bindgen_anon_1.set_ide(extended.into());
    frame
        .header
        .__bindgen_anon_1
        .set_rtr(received.is_remote_request().into());
    buffer
        .iter_mut()
        .zip(received.data())
        .for_each(|(to, from)| *to = *from);
    Ok(())
}

#[export]
fn twai_node_get_info(
    node: &Node,
    status: Option<&mut MaybeUninit<twai_node_status_t>>,
    record: Option<&mut MaybeUninit<twai_node_record_t>>,
) -> Result<(), esp_err_t> {
    critical_section::with(|cs| {
        let inner = node.inner.borrow_ref(cs);
        if let Some(status) = status {
            status.write(twai_node_status_t {
                state: inner.state,
                tx_error_count: inner.twai.transmit_error_count().into(),
                rx_error_count: inner.twai.receive_error_count().into(),
                tx_queue_remaining: TWAI0::regs().status().read().tx_buf_st().bit().into(),
            });
        }
        if let Some(record) = record {
            record.write(twai_node_record_t {
                bus_err_num: inner.bus_errors,
            });
        }
    });
    Ok(())
}

/// Recovery finishes once the bus has been idle long enough; the state change reports it.
#[export]
fn twai_node_recover(node: &Node) -> Result<(), esp_err_t> {
    critical_section::with(|cs| {
        let mut inner = node.inner.borrow_ref_mut(cs);
        if !inner.twai.is_bus_off() {
            return Err(ESP_ERR_INVALID_STATE);
        }
        inner.twai.initiate_recovery();
        inner.bus_errors = 0;
        Ok(())
    })
}
