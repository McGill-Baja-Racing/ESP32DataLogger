// Placeholder: will map the raw ADC voltage (mV) to pedal position, 0-100 %.
// Declared for C in brake_position.h.
#[no_mangle] // keep the exact name so the linker can match the call in C
pub extern "C" fn brake_position_normalize(raw_mv: i32) -> i32 { // `extern "C"`: C calling convention
    let _ = raw_mv; // unused for now
    0
}

#[no_mangle]
pub extern "C" fn brake_position_hello() -> *const u8 {
    // A constant string stored in flash. The `\0` is how C knows where it stops.
    // Creating a raw pointer is safe in Rust; only reading through it is not,
    // and the C caller does that.
    b"Hello from Rust\0".as_ptr()
}
