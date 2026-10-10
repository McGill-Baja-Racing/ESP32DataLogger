#![no_std]

use core::panic::PanicInfo;

mod brake_position;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
