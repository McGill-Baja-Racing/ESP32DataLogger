//! ESP-IDF's I2C master driver, on esp-hal's blocking `I2c`. Only what the firmware uses: one bus
//! on port 0 with 7-bit devices and synchronous transfers.

use alloc::boxed::Box;
use core::{
    cell::{Cell, RefCell},
    ffi::c_int,
    mem::MaybeUninit,
    slice,
};

use esp_hal::{
    Blocking,
    gpio::AnyPin,
    i2c::master::{Config, Error, I2c, SoftwareTimeout},
    peripherals::I2C0,
    time::{Duration, Rate},
};
use esp_radio_rtos_driver::semaphore::{SemaphoreHandle, SemaphoreKind};
use macros::export;
use portable_atomic::{AtomicBool, Ordering};
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, esp_err_t,
    i2c_device_config_t, i2c_master_bus_config_t,
};

use crate::rtos;

/// What ESP-IDF's probe uses.
const PROBE_HZ: u32 = 100_000;

struct Bus {
    /// Taken for each transfer, so a second user waits its turn, as in ESP-IDF.
    lock: SemaphoreHandle,
    i2c: RefCell<I2c<'static, Blocking>>,
    /// The speed and timeout `i2c` is set up with now; zero Hz is the default configuration.
    config: Cell<(u32, c_int)>,
}

// SAFETY: `i2c` and `config` are used only while holding `lock`.
unsafe impl Sync for Bus {}

sys::c_handle!(Bus, i2c_master_bus_handle_t);

struct Device {
    bus: &'static Bus,
    address: u8,
    speed_hz: u32,
}

sys::c_handle!(Device, i2c_master_dev_handle_t);

static TAKEN: AtomicBool = AtomicBool::new(false);

impl Bus {
    /// Runs `transfer` at `speed_hz`. `timeout_ms` bounds the wait for the bus and, separately,
    /// the transfer, as in ESP-IDF; `-1` waits forever.
    fn run(
        &self,
        speed_hz: u32,
        timeout_ms: c_int,
        transfer: impl FnOnce(&mut I2c<'static, Blocking>) -> Result<(), esp_err_t>,
    ) -> Result<(), esp_err_t> {
        if !self.lock.take_with_deadline(rtos::deadline_ms(timeout_ms)) {
            return Err(ESP_ERR_TIMEOUT);
        }
        let result = self
            .set_up(speed_hz, timeout_ms)
            .and_then(|()| transfer(&mut self.i2c.borrow_mut()));
        self.lock.give();
        result
    }

    fn set_up(&self, speed_hz: u32, timeout_ms: c_int) -> Result<(), esp_err_t> {
        if self.config.get() != (speed_hz, timeout_ms) {
            let timeout = u64::try_from(timeout_ms).map_or(SoftwareTimeout::None, |ms| {
                SoftwareTimeout::Transaction(Duration::from_millis(ms))
            });
            let config = Config::default()
                .with_frequency(Rate::from_hz(speed_hz))
                .with_software_timeout(timeout);
            let applied = self.i2c.borrow_mut().apply_config(&config);
            applied.map_err(|_invalid| ESP_ERR_INVALID_ARG)?;
            self.config.set((speed_hz, timeout_ms));
        }
        Ok(())
    }
}

/// ESP-IDF reports every failed transfer this way.
fn failed(_error: Error) -> esp_err_t {
    ESP_ERR_INVALID_STATE
}

fn address(raw: u16) -> Result<u8, esp_err_t> {
    u8::try_from(raw)
        .ok()
        .filter(|&address| address <= 0x7f)
        .ok_or(ESP_ERR_INVALID_ARG)
}

/// ESP-IDF requires every buffer to be non-empty.
fn check(buffer: *const u8, length: usize) -> Result<(), esp_err_t> {
    if buffer.is_null() || length == 0 {
        return Err(ESP_ERR_INVALID_ARG);
    }
    Ok(())
}

/// The glitch filter is esp-hal's fixed 7, and esp-hal pulls the pins up as
/// `enable_internal_pullup` asks.
#[export]
fn i2c_new_master_bus(
    config: &i2c_master_bus_config_t,
    bus: &mut MaybeUninit<&'static Bus>,
) -> Result<(), esp_err_t> {
    assert!(
        config.i2c_port == 0
            && config.glitch_ignore_cnt == 7
            && config.flags.enable_internal_pullup() == 1,
        "the kernel supports only port 0, a glitch filter of 7 and pull-ups"
    );
    let (sda, scl) = (
        rtos::gpio_number(config.sda_io_num.0)?,
        rtos::gpio_number(config.scl_io_num.0)?,
    );
    if TAKEN.swap(true, Ordering::Relaxed) {
        return Err(ESP_ERR_INVALID_STATE);
    }
    // SAFETY: `TAKEN` lets this run once, and nothing else uses I2C0. C assigns pins, as in
    // ESP-IDF; the firmware gives each GPIO to one driver.
    let (i2c, sda, scl) = unsafe { (I2C0::steal(), AnyPin::steal(sda), AnyPin::steal(scl)) };
    let config = Config::default();
    let i2c = I2c::new(i2c, config)
        .map_err(|_invalid| ESP_ERR_INVALID_ARG)?
        .with_sda(sda)
        .with_scl(scl);
    bus.write(Box::leak(Box::new(Bus {
        lock: SemaphoreHandle::new(SemaphoreKind::Mutex),
        i2c: RefCell::new(i2c),
        config: Cell::new((0, 0)),
    })));
    Ok(())
}

#[export]
fn i2c_master_bus_add_device(
    bus: &'static Bus,
    config: &i2c_device_config_t,
    device: &mut MaybeUninit<&'static Device>,
) -> Result<(), esp_err_t> {
    let address = address(config.device_address)?;
    if config.scl_speed_hz == 0 {
        return Err(ESP_ERR_INVALID_ARG);
    }
    device.write(Box::leak(Box::new(Device {
        bus,
        address,
        speed_hz: config.scl_speed_hz,
    })));
    Ok(())
}

/// A missing device is `ESP_ERR_NOT_FOUND`, not the `ESP_ERR_INVALID_STATE` of other failures,
/// which ESP-IDF's probe counts as a success.
#[export]
fn i2c_master_probe(bus: &Bus, device: u16, timeout_ms: c_int) -> Result<(), esp_err_t> {
    let address = address(device)?;
    bus.run(PROBE_HZ, timeout_ms, |i2c| {
        i2c.write(address, &[]).map_err(|error| match error {
            Error::AcknowledgeCheckFailed(_) => ESP_ERR_NOT_FOUND,
            Error::Timeout => ESP_ERR_TIMEOUT,
            _ => ESP_ERR_INVALID_STATE,
        })
    })
}

#[export]
fn i2c_master_transmit(
    device: &Device,
    write_buffer: *const u8,
    write_size: usize,
    timeout_ms: c_int,
) -> Result<(), esp_err_t> {
    check(write_buffer, write_size)?;
    // SAFETY: C passes `write_size` bytes at `write_buffer`.
    let write = unsafe { slice::from_raw_parts(write_buffer, write_size) };
    device.bus.run(device.speed_hz, timeout_ms, |i2c| {
        i2c.write(device.address, write).map_err(failed)
    })
}

#[export]
fn i2c_master_transmit_receive(
    device: &Device,
    write_buffer: *const u8,
    write_size: usize,
    read_buffer: *mut u8,
    read_size: usize,
    timeout_ms: c_int,
) -> Result<(), esp_err_t> {
    check(write_buffer, write_size)?;
    check(read_buffer, read_size)?;
    // SAFETY: C passes `write_size` bytes at `write_buffer`.
    let write = unsafe { slice::from_raw_parts(write_buffer, write_size) };
    // SAFETY: C gives room for `read_size` bytes at `read_buffer`.
    let read = unsafe { slice::from_raw_parts_mut(read_buffer, read_size) };
    device.bus.run(device.speed_hz, timeout_ms, |i2c| {
        i2c.write_read(device.address, write, read).map_err(failed)
    })
}
