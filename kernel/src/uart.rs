//! ESP-IDF's UART driver on esp-hal's `Uart`. Only what the firmware uses: UART1 receiving 8N1
//! without flow control into a byte queue.

use alloc::sync::Arc;
use core::{
    cell::RefCell,
    ffi::{c_int, c_void},
    ops::BitOr,
};

use critical_section::Mutex;
use esp_hal::{
    Blocking,
    gpio::AnyPin,
    handler,
    peripherals::UART1,
    uart::{Config, Uart, UartInterrupt},
};
use esp_radio_rtos_driver::queue::QueueHandle;
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_FAIL, QueueHandle_t, SOC_UART_FIFO_LEN,
    TickType_t, UART_PIN_NO_CHANGE, esp_err_t, uart_config_t, uart_port_t, uart_stop_bits_t,
    uart_word_length_t,
};

use crate::{
    rtos,
    system::{self, c_slice_mut},
};

struct Port {
    uart: Uart<'static, Blocking>,
    /// One byte per item. A task that is reading holds its own reference, so deleting the driver
    /// can't free the queue under it.
    queue: Arc<QueueHandle>,
}

static PORT: Mutex<RefCell<Option<Port>>> = Mutex::new(RefCell::new(None));

/// The interrupts that report received bytes.
fn rx_interrupts() -> <UartInterrupt as BitOr>::Output {
    UartInterrupt::RxFifoFull | UartInterrupt::RxTimeout
}

/// Unlike ESP-IDF, which pauses reception until the reader catches up, drops bytes that don't fit.
#[handler]
fn interrupt() {
    critical_section::with(|cs| {
        let mut port = PORT.borrow_ref_mut(cs);
        let Some(Port { uart, queue }) = port.as_mut() else {
            return;
        };
        let mut bytes = [0_u8; 64];
        loop {
            // An error is reported once, and the next read returns the bytes.
            match uart.read_buffered(&mut bytes) {
                Ok(0) => break,
                Ok(count) => {
                    for byte in bytes.iter().take(count) {
                        // SAFETY: `byte` is one item of the queue's one-byte items.
                        unsafe { queue.try_send_to_back_from_isr(byte, None) };
                    }
                }
                Err(_overflow) => {}
            }
        }
        uart.clear_interrupts(rx_interrupts());
    });
}

#[export]
fn uart_driver_install(
    uart_num: uart_port_t,
    rx_buffer_size: c_int,
    tx_buffer_size: c_int,
    queue_size: c_int,
    uart_queue: *mut QueueHandle_t,
    intr_alloc_flags: c_int,
) -> Result<(), esp_err_t> {
    assert!(
        uart_num == uart_port_t::UART_NUM_1
            && tx_buffer_size == 0
            && queue_size == 0
            && uart_queue.is_null()
            && intr_alloc_flags == 0,
        "the kernel supports only UART1 with no TX buffer and no event queue"
    );
    let capacity = usize::try_from(rx_buffer_size)
        .ok()
        .filter(|&capacity| capacity > SOC_UART_FIFO_LEN as usize)
        .ok_or(ESP_FAIL)?;
    critical_section::with(|cs| {
        let mut port = PORT.borrow_ref_mut(cs);
        if port.is_some() {
            return Err(ESP_FAIL);
        }
        // SAFETY: `PORT` lets this run once at a time, and nothing else uses UART1.
        let mut uart = Uart::new(unsafe { UART1::steal() }, Config::default())
            .map_err(|_config| ESP_ERR_INVALID_ARG)?;
        uart.set_interrupt_handler(interrupt);
        uart.listen(rx_interrupts());
        let queue = Arc::new(QueueHandle::new(capacity, 1));
        *port = Some(Port { uart, queue });
        Ok(())
    })
}

#[export]
fn uart_param_config(uart_num: uart_port_t, uart_config: &uart_config_t) -> Result<(), esp_err_t> {
    assert!(
        uart_num == uart_port_t::UART_NUM_1
            && uart_config.data_bits == uart_word_length_t::UART_DATA_8_BITS
            && uart_config.stop_bits == uart_stop_bits_t::UART_STOP_BITS_1,
        "the kernel supports only UART1 at 8N1 without flow control"
    );
    let baudrate = u32::try_from(uart_config.baud_rate).map_err(|_negative| ESP_ERR_INVALID_ARG)?;
    critical_section::with(|cs| {
        let mut port = PORT.borrow_ref_mut(cs);
        let port = port.as_mut().ok_or(ESP_ERR_INVALID_STATE)?;
        port.uart
            .apply_config(&Config::default().with_baudrate(baudrate))
            .map_err(|_config| ESP_ERR_INVALID_ARG)
    })
}

#[export]
fn uart_set_pin(
    uart_num: uart_port_t,
    tx_io_num: c_int,
    rx_io_num: c_int,
    rts_io_num: c_int,
    cts_io_num: c_int,
) -> Result<(), esp_err_t> {
    assert!(
        uart_num == uart_port_t::UART_NUM_1
            && rts_io_num == UART_PIN_NO_CHANGE
            && cts_io_num == UART_PIN_NO_CHANGE,
        "the kernel supports only UART1's TX and RX pins"
    );
    let (tx, rx) = (
        system::gpio_number(tx_io_num)?,
        system::gpio_number(rx_io_num)?,
    );
    critical_section::with(|cs| {
        let mut port = PORT.borrow_ref_mut(cs);
        let Port { uart, queue } = port.take().ok_or(ESP_ERR_INVALID_STATE)?;
        // SAFETY: C assigns pins, as in ESP-IDF; the firmware gives each GPIO to one driver.
        let uart = unsafe { uart.with_tx(AnyPin::steal(tx)).with_rx(AnyPin::steal(rx)) };
        *port = Some(Port { uart, queue });
        Ok(())
    })
}

/// Returns how many bytes came before the timeout, or -1 if the driver isn't installed.
#[export]
unsafe fn uart_read_bytes(
    uart_num: uart_port_t,
    buf: *mut c_void,
    length: u32,
    ticks_to_wait: TickType_t,
) -> c_int {
    assert!(
        uart_num == uart_port_t::UART_NUM_1,
        "the kernel supports only UART1"
    );
    // SAFETY: C gives room for `length` bytes at `buf`.
    let Some(buffer) = (unsafe { c_slice_mut(buf.cast::<u8>(), length as usize) }) else {
        return -1;
    };
    let Some(queue) = critical_section::with(|cs| {
        PORT.borrow_ref(cs)
            .as_ref()
            .map(|port| Arc::clone(&port.queue))
    }) else {
        return -1;
    };
    let deadline = rtos::deadline(ticks_to_wait);
    let count = buffer
        .iter_mut()
        // SAFETY: `byte` is one item of the queue's one-byte items.
        .map_while(|byte| unsafe { queue.receive_with_deadline(byte, deadline) }.then_some(()))
        .count();
    c_int::try_from(count).unwrap_or(c_int::MAX)
}

#[export]
fn uart_driver_delete(uart_num: uart_port_t) -> Result<(), esp_err_t> {
    assert!(
        uart_num == uart_port_t::UART_NUM_1,
        "the kernel supports only UART1"
    );
    // The interrupt is level-triggered, so it must be off before `PORT` is empty.
    critical_section::with(|cs| {
        if let Some(mut port) = PORT.borrow_ref_mut(cs).take() {
            port.uart.unlisten(rx_interrupts());
            port.uart.clear_interrupts(rx_interrupts());
        }
    });
    Ok(())
}
