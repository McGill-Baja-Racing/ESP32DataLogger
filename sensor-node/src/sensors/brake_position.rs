use core::ptr::null_mut;

use crate::Sensor;

const ESP_OK: i32 = 0;

// Placeholder: will map the raw ADC voltage (mV) to pedal position, 0-100 %.
const fn brake_position_normalize(_raw_mv: i32) -> i32 {
    0
}

// SAFETY (caller): the sampler passes a valid pointer to an `int32_t`.
unsafe extern "C" fn read_position(_sensor: &mut Sensor, value: *mut i32) -> i32 {
    // SAFETY: `value` is valid per the caller contract above.
    unsafe { *value = brake_position_normalize(0) };
    ESP_OK
}

/// `can_id` is left 0 here: `sensor_registry.c` sets it from `CAN_ID_BRAKE_POSITION`.
#[unsafe(no_mangle)]
pub static brake_position_sensor: Sensor = Sensor {
    name: c"brake_position".as_ptr(),
    can_id: 0,
    period_us: 40000, // 25 Hz
    next_sample_us: 0,
    init: None,
    start: None,
    read: Some(read_position),
    context: null_mut(),
};
