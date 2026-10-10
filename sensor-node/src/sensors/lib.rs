#![no_std]

use core::ffi::c_char;
use core::fmt::Write;
use core::panic::PanicInfo;

mod brake_position;

extern "C" {
    fn esp_system_abort(details: *const c_char) -> !;
}

struct Buffer {
    bytes: [u8; 96],
    len: usize,
}

impl Write for Buffer {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        // Truncate instead of failing; the last byte stays 0 for C.
        let n = s.len().min(self.bytes.len() - 1 - self.len);
        self.bytes[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let mut message = Buffer { bytes: [0; 96], len: 0 };
    let _ = write!(message, "{info}");
    unsafe { esp_system_abort(message.bytes.as_ptr() as *const c_char) }
}
