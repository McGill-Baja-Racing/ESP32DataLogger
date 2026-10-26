//! The SD host controller, which serves the master's SD card (slot 0, in `storage`) and the
//! esp-hosted coprocessor (slot 1, in `network`), and the SD power control.

use alloc::boxed::Box;
use core::cell::Cell;

use critical_section::Mutex;
use esp_hal::{
    peripherals::SDHOST,
    sdmmc::{Config, SdHostController},
};
use macros::export;
use sys::{ESP_FAIL, esp_err_t, sd_pwr_ctrl_ldo_config_t};

static CONTROLLER: Mutex<Cell<Option<&'static SdHostController<'static>>>> =
    Mutex::new(Cell::new(None));

/// Creates the controller, which powers the SD card's supply. Not thread safe: the master's first
/// call is `sd_pwr_ctrl_new_on_chip_ldo`, before any other task touches the card or Wi-Fi.
pub fn controller() -> Result<&'static SdHostController<'static>, esp_err_t> {
    if let Some(controller) = critical_section::with(|cs| CONTROLLER.borrow(cs).get()) {
        return Ok(controller);
    }
    // SAFETY: the SD host is used only through this controller.
    let controller = SdHostController::new(unsafe { SDHOST::steal() }, Config::default())
        .map_err(|_config| ESP_FAIL)?;
    let controller = &*Box::leak(Box::new(controller));
    critical_section::with(|cs| CONTROLLER.borrow(cs).set(Some(controller)));
    Ok(controller)
}

/// The controller's own supply is LDO channel 4, which esp-hal powers when it creates the
/// controller.
struct Ldo;

sys::c_handle!(Ldo, sd_pwr_ctrl_handle_t);

static LDO: Ldo = Ldo;

#[export]
fn sd_pwr_ctrl_new_on_chip_ldo(
    configs: &sd_pwr_ctrl_ldo_config_t,
    ret_drv: &mut core::mem::MaybeUninit<&'static Ldo>,
) -> Result<(), esp_err_t> {
    assert!(
        configs.ldo_chan_id == 4,
        "the kernel supports only LDO channel 4"
    );
    controller()?;
    ret_drv.write(&LDO);
    Ok(())
}
