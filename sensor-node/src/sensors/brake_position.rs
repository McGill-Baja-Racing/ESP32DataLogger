#[no_mangle]
pub extern "C" fn brake_position_normalize(raw_mv: i32) -> i32 {
    let _ = raw_mv;
    0
}

#[no_mangle]
pub extern "C" fn brake_position_hello() -> *const u8 {
    b"Hello from Rust\0".as_ptr()
}
