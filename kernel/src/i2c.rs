//! ESP-IDF's I2C master driver, on esp-hal's blocking `I2c`. Only what the firmware uses: one bus
//! on port 0 with 7-bit devices and synchronous transfers.

use alloc::boxed::Box;
use core::{ffi::c_int, mem::MaybeUninit};

use esp_hal::{
    Blocking,
    gpio::AnyPin,
    i2c::master::{Config, Error, I2c, SoftwareTimeout},
    peripherals::I2C0,
    time::{Duration, Rate},
};
use macros::export;
use portable_atomic::{AtomicBool, Ordering};
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, esp_err_t,
    i2c_device_config_t, i2c_master_bus_config_t,
};

use crate::{
    rtos::{self, Locked},
    system::{self, c_slice, c_slice_mut},
};

/// What ESP-IDF's probe uses.
const PROBE_HZ: u32 = 100_000;

struct BusInner {
    i2c: I2c<'static, Blocking>,
    /// The speed and timeout `i2c` is set up with now; `None` is esp-hal's default.
    config: Option<(u32, c_int)>,
}

/// Locked for each transfer, so a second user waits its turn, as in ESP-IDF.
type Bus = Locked<BusInner>;

sys::c_handle!(Bus, sys::i2c_master_bus_handle_t);

struct Device {
    bus: &'static Bus,
    address: u8,
    speed_hz: u32,
}

sys::c_handle!(Device, sys::i2c_master_dev_handle_t);

static TAKEN: AtomicBool = AtomicBool::new(false);

impl BusInner {
    fn set_up(&mut self, speed_hz: u32, timeout_ms: c_int) -> Result<(), esp_err_t> {
        if self.config != Some((speed_hz, timeout_ms)) {
            let timeout = u64::try_from(timeout_ms).map_or(SoftwareTimeout::None, |ms| {
                SoftwareTimeout::Transaction(Duration::from_millis(ms))
            });
            let config = Config::default()
                .with_frequency(Rate::from_hz(speed_hz))
                .with_software_timeout(timeout);
            self.i2c
                .apply_config(&config)
                .map_err(|_invalid| ESP_ERR_INVALID_ARG)?;
            self.config = Some((speed_hz, timeout_ms));
        }
        Ok(())
    }
}

/// Runs `transfer` at `speed_hz`. `timeout_ms` bounds the wait for the bus and, separately, the
/// transfer, as in ESP-IDF; `-1` waits forever.
fn run(
    bus: &Bus,
    speed_hz: u32,
    timeout_ms: c_int,
    transfer: impl FnOnce(&mut I2c<'static, Blocking>) -> Result<(), esp_err_t>,
) -> Result<(), esp_err_t> {
    bus.with_deadline(rtos::deadline_ms(timeout_ms), |bus| {
        bus.set_up(speed_hz, timeout_ms)?;
        transfer(&mut bus.i2c)
    })
    .unwrap_or(Err(ESP_ERR_TIMEOUT))
}

fn address(raw: u16) -> Result<u8, esp_err_t> {
    u8::try_from(raw)
        .ok()
        .filter(|&address| address <= 0x7f)
        .ok_or(ESP_ERR_INVALID_ARG)
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
        system::gpio_number(config.sda_io_num.0)?,
        system::gpio_number(config.scl_io_num.0)?,
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
    bus.write(Box::leak(Box::new(Locked::new(BusInner {
        i2c,
        config: None,
    }))));
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
    run(bus, PROBE_HZ, timeout_ms, |i2c| {
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
    // SAFETY: C passes `write_size` bytes at `write_buffer`.
    let write = unsafe { c_slice(write_buffer, write_size) }
        .filter(|write| !write.is_empty())
        .ok_or(ESP_ERR_INVALID_ARG)?;
    run(device.bus, device.speed_hz, timeout_ms, |i2c| {
        i2c.write(device.address, write)
            .map_err(|_failed| ESP_ERR_INVALID_STATE)
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
    // SAFETY: C passes `write_size` bytes at `write_buffer`.
    let write = unsafe { c_slice(write_buffer, write_size) }
        .filter(|write| !write.is_empty())
        .ok_or(ESP_ERR_INVALID_ARG)?;
    // SAFETY: C gives room for `read_size` bytes at `read_buffer`.
    let read = unsafe { c_slice_mut(read_buffer, read_size) }
        .filter(|read| !read.is_empty())
        .ok_or(ESP_ERR_INVALID_ARG)?;
    run(device.bus, device.speed_hz, timeout_ms, |i2c| {
        i2c.write_read(device.address, write, read)
            .map_err(|_failed| ESP_ERR_INVALID_STATE)
    })
}
