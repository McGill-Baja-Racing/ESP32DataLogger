//! ESP-IDF's one-shot ADC driver and curve-fitting calibration, on esp-hal's SAR ADC. Only what
//! the firmware uses: ADC1 at 12 dB and 12 bits.
//!
//! ESP-IDF picks channels by number at run time while esp-hal's `Adc` wants typed pins, so
//! conversions run through esp-hal's register access by channel number, the way ESP-IDF's HAL does.

use alloc::boxed::Box;
use core::{cell::Cell, ffi::c_int, mem::MaybeUninit};

use critical_section::Mutex;

use esp_hal::{
    Blocking,
    analog::adc::{
        Adc, AdcCalBasic, AdcCalCurve, AdcCalScheme as _, AdcConfig, Attenuation,
        RegisterAccess as _,
    },
    gpio::{AnyPin, Flex, InputConfig},
    peripherals::ADC1,
};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, adc_atten_t, adc_bitwidth_t,
    adc_cali_curve_fitting_config_t, adc_channel_t, adc_oneshot_chan_cfg_t,
    adc_oneshot_unit_init_cfg_t, adc_unit_t, esp_err_t,
};

type Regs = ADC1<'static>;

const ATTENUATION: Attenuation = Attenuation::_11dB;

/// Whether `adc_oneshot_new_unit` has taken ADC1.
static TAKEN: Mutex<Cell<bool>> = Mutex::new(Cell::new(false));

/// What a C `adc_oneshot_unit_handle_t` points to.
struct Unit {
    /// Keeps the SAR ADC clocked; conversions bypass its typed API.
    _adc: Adc<'static, Regs, Blocking>,
    init_code: u16,
}

sys::c_handle!(Unit, adc_oneshot_unit_handle_t);

/// What a C `adc_cali_handle_t` points to.
struct Calibration(AdcCalCurve<Regs>);

sys::c_handle!(Calibration, adc_cali_handle_t);

/// `unsupported.h` rejects other settings at compile time; this catches values computed at run time.
fn check(unit: adc_unit_t, atten: adc_atten_t, bitwidth: adc_bitwidth_t) {
    assert!(
        unit == adc_unit_t::ADC_UNIT_1
            && atten == adc_atten_t::ADC_ATTEN_DB_12
            && matches!(
                bitwidth,
                adc_bitwidth_t::ADC_BITWIDTH_DEFAULT | adc_bitwidth_t::ADC_BITWIDTH_12
            ),
        "the kernel supports only ADC1 at 12 dB and 12 bits"
    );
}

/// ADC1 channel `n` is GPIO`n` on the C3.
fn channel(chan: adc_channel_t) -> Result<u8, esp_err_t> {
    u8::try_from(chan.0)
        .ok()
        .filter(|&number| number <= 4)
        .ok_or(ESP_ERR_INVALID_ARG)
}

#[export]
fn adc_oneshot_new_unit(
    config: &adc_oneshot_unit_init_cfg_t,
    unit: &mut MaybeUninit<Option<&'static Unit>>,
) -> Result<(), esp_err_t> {
    check(
        config.unit_id,
        adc_atten_t::ADC_ATTEN_DB_12,
        adc_bitwidth_t::ADC_BITWIDTH_DEFAULT,
    );
    if critical_section::with(|cs| TAKEN.borrow(cs).replace(true)) {
        return Err(ESP_ERR_NOT_FOUND);
    }
    // SAFETY: `TAKEN` lets this run once, and nothing else uses ADC1.
    let adc = Adc::new(unsafe { Regs::steal() }, AdcConfig::new());
    // Without eFuse calibration, esp-hal measures the init code with conversions.
    let init_code = AdcCalBasic::<Regs>::new_cal(ATTENUATION).adc_cal();
    unit.write(Some(Box::leak(Box::new(Unit {
        _adc: adc,
        init_code,
    }))));
    Ok(())
}

/// esp-hal's `AnalogPin` setup on RISC-V is a GPIO with its input buffer and pulls off, which
/// `Flex::new` and the default `InputConfig` do too.
#[export]
fn adc_oneshot_config_channel(
    _unit: &Unit,
    chan: adc_channel_t,
    config: &adc_oneshot_chan_cfg_t,
) -> Result<(), esp_err_t> {
    check(adc_unit_t::ADC_UNIT_1, config.atten, config.bitwidth);
    // SAFETY: C assigns pins, as in ESP-IDF; the firmware gives each GPIO to one driver.
    let mut pin = Flex::new(unsafe { AnyPin::steal(channel(chan)?) });
    pin.apply_input_config(&InputConfig::default());
    Ok(())
}

#[export]
fn adc_oneshot_read(
    unit: &Unit,
    chan: adc_channel_t,
    raw: &mut MaybeUninit<c_int>,
) -> Result<(), esp_err_t> {
    let channel = channel(chan)?;
    critical_section::with(|_| {
        Regs::calibration_init();
        Regs::set_init_code(unit.init_code);
        Regs::config_onetime_sample(channel, ATTENUATION as u8);
        Regs::start_onetime_sample();
        while !Regs::is_done() {}
        raw.write(Regs::read_data().into());
        Regs::reset();
    });
    Ok(())
}

#[export]
fn adc_cali_create_scheme_curve_fitting(
    config: &adc_cali_curve_fitting_config_t,
    calibration: &mut MaybeUninit<Option<&'static Calibration>>,
) -> Result<(), esp_err_t> {
    check(config.unit_id, config.atten, config.bitwidth);
    // ESP-IDF refuses without eFuse calibration; esp-hal would guess from a 1.1 V reference.
    if esp_hal::efuse::rtc_calib_version() != 1 {
        return Err(ESP_ERR_NOT_SUPPORTED);
    }
    calibration.write(Some(Box::leak(Box::new(Calibration(
        AdcCalCurve::new_cal(ATTENUATION),
    )))));
    Ok(())
}

#[export]
fn adc_cali_raw_to_voltage(
    calibration: &Calibration,
    raw: c_int,
    voltage: &mut MaybeUninit<c_int>,
) -> Result<(), esp_err_t> {
    let reading = u16::try_from(raw).map_err(|_negative| ESP_ERR_INVALID_ARG)?;
    voltage.write(calibration.0.adc_val(reading).into());
    Ok(())
}
