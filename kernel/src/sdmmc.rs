//! The SD host controller, which serves the master's SD card (slot 0, in `storage`) and the
//! esp-hosted coprocessor (slot 1, in `network`), and the SD power control.

use alloc::boxed::Box;
use core::{ffi::c_int, mem::MaybeUninit};

use esp_hal::{
    peripherals::SDHOST,
    sdmmc::{Config, SdHostController},
};
use macros::export;
use sys::{ESP_FAIL, esp_err_t, sd_pwr_ctrl_ldo_config_t};

use crate::rtos::Global;

static CONTROLLER: Global<SdHostController<'static>> = Global::new();

/// Creates the controller, which powers the SD card's supply. Not thread safe: the master's first
/// call is `sd_pwr_ctrl_new_on_chip_ldo`, before any other task touches the card or Wi-Fi.
pub fn controller() -> Result<&'static SdHostController<'static>, esp_err_t> {
    if let Some(controller) = CONTROLLER.get() {
        return Ok(controller);
    }
    // SAFETY: the SD host is used only through this controller.
    let controller = SdHostController::new(unsafe { SDHOST::steal() }, Config::default())
        .map_err(|_config| ESP_FAIL)?;
    let controller = &*Box::leak(Box::new(controller));
    CONTROLLER.set(controller);
    Ok(controller)
}

/// The LDO channel of the controller's own supply, which esp-hal powers when it creates the
/// controller.
const LDO_CHANNEL: c_int = 4;

struct Ldo;

sys::c_handle!(Ldo, sys::sd_pwr_ctrl_handle_t);

static LDO: Ldo = Ldo;

#[export]
fn sd_pwr_ctrl_new_on_chip_ldo(
    configs: &sd_pwr_ctrl_ldo_config_t,
    ret_drv: &mut MaybeUninit<&'static Ldo>,
) -> Result<(), esp_err_t> {
    assert!(
        configs.ldo_chan_id == LDO_CHANNEL,
        "the kernel supports only LDO channel {LDO_CHANNEL}"
    );
    controller()?;
    ret_drv.write(&LDO);
    Ok(())
}
