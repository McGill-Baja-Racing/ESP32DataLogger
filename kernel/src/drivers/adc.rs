//! ESP-IDF's one-shot ADC driver and curve-fitting calibration, on esp-hal's SAR ADC.
//!
//! ESP-IDF picks channels by number at run time while esp-hal's `Adc` wants typed pins, so
//! conversions run through esp-hal's register access by channel number, the way ESP-IDF's HAL does.

use alloc::boxed::Box;
use core::{cell::Cell, ffi::c_int, mem::MaybeUninit};

use critical_section::Mutex;
use esp_hal::{
    Blocking,
    analog::adc::{
        Adc, AdcCalBasic, AdcCalCurve, AdcCalScheme as _, AdcChannel, AdcConfig, Attenuation,
        RegisterAccess as _,
    },
    gpio::{AnalogPin, AnyPin},
    peripherals::{ADC1, GPIO0, GPIO1, GPIO2, GPIO3, GPIO4},
};
use macros::export;
use sys::{
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_SUPPORTED, adc_atten_t, adc_bitwidth_t,
    adc_cali_curve_fitting_config_t, adc_channel_t, adc_oneshot_chan_cfg_t,
    adc_oneshot_unit_init_cfg_t, adc_unit_t, esp_err_t,
};

use crate::drivers;

/// ADC1 until `adc_oneshot_new_unit` takes it.
static ADC: Mutex<Cell<Option<ADC1<'static>>>> = Mutex::new(Cell::new(None));

pub fn init(adc: ADC1<'static>) {
    critical_section::with(|cs| ADC.borrow(cs).set(Some(adc)));
}

/// A configured channel's attenuation and the calibration init code for it.
type ChannelSetup = Option<(Attenuation, u16)>;

/// What a C `adc_oneshot_unit_handle_t` points to.
struct Unit {
    /// Keeps the SAR ADC clocked; conversions bypass its typed API.
    _adc: Adc<'static, ADC1<'static>, Blocking>,
    channels: [Mutex<Cell<ChannelSetup>>; 5],
}

// C shares one handle between tasks.
impl sys::CPointer for Unit
where
    Self: Sync,
{
    type Shared = sys::adc_oneshot_unit_handle_t;
    type Unique = sys::adc_oneshot_unit_handle_t;
}

/// What a C `adc_cali_handle_t` points to.
struct Calibration(AdcCalCurve<ADC1<'static>>);

// C shares one handle between tasks.
impl sys::CPointer for Calibration
where
    Self: Sync,
{
    type Shared = sys::adc_cali_handle_t;
    type Unique = sys::adc_cali_handle_t;
}

const fn attenuation(atten: adc_atten_t) -> Result<Attenuation, esp_err_t> {
    match atten {
        adc_atten_t::ADC_ATTEN_DB_0 => Ok(Attenuation::_0dB),
        adc_atten_t::ADC_ATTEN_DB_2_5 => Ok(Attenuation::_2p5dB),
        adc_atten_t::ADC_ATTEN_DB_6 => Ok(Attenuation::_6dB),
        adc_atten_t::ADC_ATTEN_DB_12 => Ok(Attenuation::_11dB),
        _ => Err(ESP_ERR_INVALID_ARG),
    }
}

/// The C3's ADC1 converts 12 bits; ESP-IDF accepts the default or 12.
const fn check_bitwidth(bitwidth: adc_bitwidth_t) -> Result<(), esp_err_t> {
    match bitwidth {
        adc_bitwidth_t::ADC_BITWIDTH_DEFAULT | adc_bitwidth_t::ADC_BITWIDTH_12 => Ok(()),
        _ => Err(ESP_ERR_INVALID_ARG),
    }
}

/// Claims the GPIO behind an ADC1 channel and switches it to analog input. `enable_pin` is
/// esp-hal's only public way to do that, so its `AdcConfig` is thrown away.
fn set_analog(channel: u8, attenuation: Attenuation) -> Result<(), esp_err_t> {
    fn analog<P>(pin: AnyPin<'static>, attenuation: Attenuation) -> Result<(), esp_err_t>
    where
        P: AdcChannel + AnalogPin + TryFrom<AnyPin<'static>>,
    {
        let typed = P::try_from(pin).map_err(|_other_pin| ESP_ERR_INVALID_ARG)?;
        AdcConfig::<ADC1<'static>>::new().enable_pin(typed, attenuation);
        Ok(())
    }
    let pin = drivers::claim(channel)?;
    match channel {
        0 => analog::<GPIO0<'static>>(pin, attenuation),
        1 => analog::<GPIO1<'static>>(pin, attenuation),
        2 => analog::<GPIO2<'static>>(pin, attenuation),
        3 => analog::<GPIO3<'static>>(pin, attenuation),
        4 => analog::<GPIO4<'static>>(pin, attenuation),
        _ => Err(ESP_ERR_INVALID_ARG),
    }
}

impl Unit {
    fn new(config: &adc_oneshot_unit_init_cfg_t) -> Result<Self, esp_err_t> {
        if config.unit_id != adc_unit_t::ADC_UNIT_1 {
            return Err(ESP_ERR_INVALID_ARG);
        }
        let adc =
            critical_section::with(|cs| ADC.borrow(cs).take()).ok_or(ESP_ERR_INVALID_STATE)?;
        Ok(Self {
            _adc: Adc::new(adc, AdcConfig::new()),
            channels: [const { Mutex::new(Cell::new(None)) }; 5],
        })
    }

    /// The channel's register number and its entry in `channels`.
    fn slot(&self, chan: adc_channel_t) -> Result<(u8, &Mutex<Cell<ChannelSetup>>), esp_err_t> {
        let channel = u8::try_from(chan.0).map_err(|_too_big| ESP_ERR_INVALID_ARG)?;
        let slot = self
            .channels
            .get(usize::from(channel))
            .ok_or(ESP_ERR_INVALID_ARG)?;
        Ok((channel, slot))
    }

    fn configure(
        &self,
        chan: adc_channel_t,
        config: &adc_oneshot_chan_cfg_t,
    ) -> Result<(), esp_err_t> {
        let (channel, slot) = self.slot(chan)?;
        let atten = attenuation(config.atten)?;
        check_bitwidth(config.bitwidth)?;
        if critical_section::with(|cs| slot.borrow(cs).get().is_none()) {
            set_analog(channel, atten)?;
        }
        // Without eFuse calibration, esp-hal measures the init code with conversions, which
        // must not interleave with a `read`; ESP-IDF also measures it in a critical section.
        critical_section::with(|cs| {
            let init_code = AdcCalBasic::<ADC1<'static>>::new_cal(atten).adc_cal();
            slot.borrow(cs).set(Some((atten, init_code)));
        });
        Ok(())
    }

    /// One conversion; like ESP-IDF, it first loads the init code for the channel's attenuation.
    fn read(&self, chan: adc_channel_t) -> Result<u16, esp_err_t> {
        type Regs = ADC1<'static>;
        let (channel, slot) = self.slot(chan)?;
        critical_section::with(|cs| {
            let (atten, init_code) = slot.borrow(cs).get().ok_or(ESP_ERR_INVALID_STATE)?;
            Regs::calibration_init();
            Regs::set_init_code(init_code);
            #[expect(
                clippy::as_conversions,
                reason = "the enum's values are the register's"
            )]
            Regs::config_onetime_sample(channel, atten as u8);
            Regs::start_onetime_sample();
            while !Regs::is_done() {}
            let raw = Regs::read_data();
            Regs::reset();
            Ok(raw)
        })
    }
}

impl Calibration {
    fn new(config: &adc_cali_curve_fitting_config_t) -> Result<Self, esp_err_t> {
        if config.unit_id != adc_unit_t::ADC_UNIT_1 {
            return Err(ESP_ERR_INVALID_ARG);
        }
        let atten = attenuation(config.atten)?;
        check_bitwidth(config.bitwidth)?;
        // ESP-IDF refuses without eFuse calibration; esp-hal would guess from a 1.1 V reference.
        if esp_hal::efuse::rtc_calib_version() != 1 {
            return Err(ESP_ERR_NOT_SUPPORTED);
        }
        Ok(Self(AdcCalCurve::new_cal(atten)))
    }
}

#[export]
fn adc_oneshot_new_unit(
    config: &adc_oneshot_unit_init_cfg_t,
    unit: &mut MaybeUninit<Option<&'static Unit>>,
) -> Result<(), esp_err_t> {
    unit.write(Some(Box::leak(Box::new(Unit::new(config)?))));
    Ok(())
}

#[export]
fn adc_oneshot_config_channel(
    unit: &Unit,
    chan: adc_channel_t,
    config: &adc_oneshot_chan_cfg_t,
) -> Result<(), esp_err_t> {
    unit.configure(chan, config)
}

#[export]
fn adc_oneshot_read(
    unit: &Unit,
    chan: adc_channel_t,
    raw: &mut MaybeUninit<c_int>,
) -> Result<(), esp_err_t> {
    raw.write(unit.read(chan)?.into());
    Ok(())
}

#[export]
fn adc_cali_create_scheme_curve_fitting(
    config: &adc_cali_curve_fitting_config_t,
    calibration: &mut MaybeUninit<Option<&'static Calibration>>,
) -> Result<(), esp_err_t> {
    calibration.write(Some(Box::leak(Box::new(Calibration::new(config)?))));
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
