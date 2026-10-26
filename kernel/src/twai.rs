//! ESP-IDF's on-chip TWAI (CAN) node driver, on the chip's controller. Only what the firmware
//! uses: one node sending standard data frames at 1 Mbit/s.
//!
//! The controller retries a failed frame, where ESP-IDF's driver with the firmware's zeroed
//! `fail_retry_cnt` sends each frame once; `Node::bus_error` gives up on unacknowledged ones. The
//! bit timing has 20 quanta, a jump width of 3 and an 80 % sample point, where ESP-IDF uses 75 %.

use alloc::boxed::Box;
use core::{
    cell::{Cell, RefCell},
    ffi::{c_int, c_void},
    mem::MaybeUninit,
    ptr, slice,
};

use critical_section::Mutex;
use esp_hal::{handler, peripherals::TWAI0};
use esp_radio_rtos_driver::semaphore::{SemaphoreHandle, SemaphoreKind};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, esp_err_t,
    twai_error_state_t, twai_event_callbacks_t, twai_frame_t, twai_node_handle_t,
    twai_node_record_t, twai_node_status_t, twai_onchip_node_config_t, twai_rx_done_event_data_t,
    twai_state_change_event_data_t,
};

use crate::rtos::{self, Context};

#[cfg_attr(feature = "esp32c3", path = "twai/esp32c3.rs")]
#[cfg_attr(feature = "esp32p4", path = "twai/esp32p4.rs")]
mod chip;

#[expect(
    clippy::struct_excessive_bools,
    reason = "one flag per status bit the driver reads"
)]
struct Status {
    bus_off: bool,
    tx_free: bool,
    /// The receive FIFO holds a frame.
    rx_pending: bool,
    /// That frame was cut short by a full FIFO.
    missed: bool,
}

/// The 13 bytes of the data registers: the frame information, the identifier, then the payload.
#[derive(Clone, Copy)]
struct Frame([u8; 13]);

impl Frame {
    fn standard(id: u16, payload: &[u8]) -> Option<Self> {
        let mut bytes = [0; 13];
        let [info, high, low, data @ ..] = &mut bytes;
        *info = u8::try_from(payload.len()).ok().filter(|&dlc| dlc <= 8)?;
        [*high, *low] = id.checked_mul(32)?.to_be_bytes();
        data.get_mut(..payload.len())?.copy_from_slice(payload);
        Some(Self(bytes))
    }

    /// The frame at the head of the receive FIFO.
    fn read() -> Self {
        Self(core::array::from_fn(|index| {
            TWAI0::regs().data(index).read().tx_byte().bits()
        }))
    }

    fn write(&self) {
        for (index, byte) in self.0.iter().enumerate() {
            // SAFETY: the registers take any byte.
            TWAI0::regs()
                .data(index)
                .write(|w| unsafe { w.tx_byte().bits(*byte) });
        }
    }

    fn is_extended(&self) -> bool {
        self.0[0] & 0x80 != 0
    }

    /// The identifier, and whether it is an extended one.
    fn id(&self) -> (u32, bool) {
        let [_, a, b, c, d, ..] = self.0.map(u32::from);
        if self.is_extended() {
            (a << 21 | b << 13 | c << 5 | d >> 3, true)
        } else {
            (a << 3 | b >> 5, false)
        }
    }

    fn dlc(&self) -> u16 {
        u16::from(self.0[0] & 0x0f)
    }

    fn is_remote(&self) -> bool {
        self.0[0] & 0x40 != 0
    }

    fn data(&self) -> &[u8] {
        let start = if self.is_extended() { 5 } else { 3 };
        let length = if self.is_remote() {
            0
        } else {
            usize::from(self.dlc().min(8))
        };
        self.0.get(start..start + length).unwrap_or_default()
    }
}

#[expect(
    clippy::struct_excessive_bools,
    reason = "one flag per interrupt the controller reports"
)]
struct Events {
    bus_error: bool,
    arbitration_lost: bool,
    /// An error counter crossed the warning or passive limit, or the bus-off state changed.
    state_change: bool,
    receive: bool,
    transmit: bool,
}

struct Inner {
    twai: chip::Controller,
    state: twai_error_state_t,
    bus_errors: u32,
    /// The frame `twai_node_receive_from_isr` reads, while `on_rx_done` runs.
    rx: Option<Frame>,
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

fn state(twai: &chip::Controller) -> twai_error_state_t {
    if twai.is_bus_off() {
        twai_error_state_t::TWAI_ERROR_BUS_OFF
    } else if twai.tx_errors() >= 128 || twai.rx_errors() >= 128 {
        twai_error_state_t::TWAI_ERROR_PASSIVE
    } else if chip::warning() {
        twai_error_state_t::TWAI_ERROR_WARNING
    } else {
        twai_error_state_t::TWAI_ERROR_ACTIVE
    }
}

impl Node {
    /// Also wakes `wait_free` itself, in case the abort raises no TX interrupt; waiters recheck
    /// the buffer, so an early wake is harmless.
    fn abort_transmission(&self) {
        chip::abort_tx();
        self.idle.try_give_from_isr(None);
    }

    fn handle(&self) -> twai_node_handle_t {
        ptr::from_ref(self).cast_mut().cast()
    }

    /// Waits until the TX buffer is free; `-1` waits forever, like ESP-IDF.
    fn wait_free(&self, timeout_ms: c_int) -> Result<(), esp_err_t> {
        let deadline = rtos::deadline_ms(timeout_ms);
        loop {
            let status = chip::status();
            if status.bus_off {
                return Err(ESP_ERR_INVALID_STATE);
            }
            if status.tx_free {
                return Ok(());
            }
            if !self.idle.take_with_deadline(deadline) {
                return Err(ESP_ERR_TIMEOUT);
            }
        }
    }

    /// Counts like ESP-IDF's `bus_err_num`. Error-passive stops the TEC from rising on ACK errors,
    /// so the controller would retry a frame nobody acknowledges forever; this gives up on it.
    fn bus_error(&self, events: &Events) {
        let unacknowledged = events.bus_error && chip::ack_error();
        let passive = critical_section::with(|cs| {
            let mut inner = self.inner.borrow_ref_mut(cs);
            inner.bus_errors = inner.bus_errors.wrapping_add(1);
            inner.twai.tx_errors() >= 128
        });
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
                inner.rx = next_frame();
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
        chip::clear_overrun();
    }
}

/// A frame the FIFO missed is released alone, as in ESP-IDF, so the frames behind it still arrive.
fn next_frame() -> Option<Frame> {
    loop {
        let status = chip::status();
        if status.bus_off || !status.rx_pending {
            return None;
        }
        let frame = (!status.missed).then(Frame::read);
        chip::release_frame();
        if frame.is_some() {
            return frame;
        }
    }
}

#[handler]
fn interrupt() {
    let Some(node) = critical_section::with(|cs| NODE.borrow(cs).get()) else {
        return;
    };
    let events = chip::events();
    if events.bus_error || events.arbitration_lost {
        node.bus_error(&events);
    }
    if events.state_change {
        node.state_change();
    }
    if events.receive {
        node.receive();
    }
    if events.transmit {
        node.idle.try_give_from_isr(None);
    }
}

/// `tx_queue_depth` only has to be non-zero: there is one TX buffer, which `twai_node_transmit`
/// waits for. ESP-IDF would route the zeroed `quanta_clk_out` and `bus_off_indicator` to GPIO0; this doesn't.
#[export]
fn twai_new_node_onchip(
    config: &twai_onchip_node_config_t,
    node: &mut MaybeUninit<&'static Node>,
) -> Result<(), esp_err_t> {
    assert!(
        config.bit_timing.bitrate == 1_000_000,
        "the kernel supports only 1 Mbit/s"
    );
    if config.tx_queue_depth == 0 {
        return Err(ESP_ERR_INVALID_ARG);
    }
    let (tx, rx) = (
        rtos::gpio_number(config.io_cfg.tx.0)?,
        rtos::gpio_number(config.io_cfg.rx.0)?,
    );
    critical_section::with(|cs| {
        let slot = NODE.borrow(cs);
        if slot.get().is_some() {
            return Err(ESP_ERR_NOT_FOUND);
        }
        // SAFETY: `NODE` lets this run once, and nothing else uses TWAI0. C assigns pins, as in
        // ESP-IDF; the firmware gives each GPIO to one driver.
        let twai = unsafe { chip::Controller::start(tx, rx, interrupt) };
        let new = Box::leak(Box::new(Node {
            inner: Mutex::new(RefCell::new(Inner {
                twai,
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
    chip::enable_interrupts();
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
        format.ide() == 0 && format.rtr() == 0 && frame.header.dlc == 0,
        "the kernel supports only standard data frames sized by `buffer_len`"
    );
    let payload = match frame.buffer_len {
        0 => &[],
        1..=8 if !frame.buffer.is_null() => {
            // SAFETY: C passes `buffer_len` bytes at `buffer`.
            unsafe { slice::from_raw_parts(frame.buffer, frame.buffer_len) }
        }
        _ => return Err(ESP_ERR_INVALID_ARG),
    };
    let frame = u16::try_from(frame.header.id)
        .ok()
        .and_then(|id| Frame::standard(id, payload))
        .ok_or(ESP_ERR_INVALID_ARG)?;
    node.wait_free(timeout_ms)?;
    critical_section::with(|_| {
        let status = chip::status();
        if status.bus_off || !status.tx_free {
            return Err(ESP_ERR_INVALID_STATE);
        }
        frame.write();
        chip::request_transmit();
        Ok(())
    })
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
    let (id, extended) = received.id();
    frame.header.id = id;
    frame.header.dlc = received.dlc();
    frame.header.__bindgen_anon_1.set_ide(extended.into());
    frame
        .header
        .__bindgen_anon_1
        .set_rtr(received.is_remote().into());
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
                tx_error_count: inner.twai.tx_errors().into(),
                rx_error_count: inner.twai.rx_errors().into(),
                tx_queue_remaining: chip::status().tx_free.into(),
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
