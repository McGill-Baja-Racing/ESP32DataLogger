//! The C library functions the firmware calls, on the console. Only what the firmware uses.

use alloc::{boxed::Box, vec::Vec};
use core::{
    cell::RefCell,
    ffi::{CStr, c_char, c_int},
    ptr, slice,
};

use critical_section::Mutex;
use esp_hal::peripherals::{UART0, USB_DEVICE};
use esp_println::Printer;
use macros::export;

use crate::system;

#[cfg(feature = "esp32p4")]
mod master;
mod string;
#[cfg(feature = "esp32p4")]
pub mod time;

pub enum Stream {
    Stdin,
    Stdout,
    #[cfg(feature = "esp32p4")]
    File(crate::files::Handle),
}

#[cfg(feature = "esp32p4")]
pub const EOF: c_int = -1;

sys::c_handle!(Stream, FilePtr, owned);

static STDIN: Stream = Stream::Stdin;
static STDOUT: Stream = Stream::Stdout;

struct Reent(sys::_reent);

// SAFETY: it holds only pointers to the streams above, which C never writes through.
unsafe impl Sync for Reent {}

/// `stdin` and `stdout` are `__getreent()->_stdin` and `_stdout`. Nothing else in it is used.
static REENT: Reent = Reent(sys::_reent {
    _stdin: ptr::from_ref(&STDIN).cast_mut().cast(),
    _stdout: ptr::from_ref(&STDOUT).cast_mut().cast(),
    // SAFETY: all-zero is a valid `_reent`: null pointers, no callbacks and zero counts.
    ..unsafe { core::mem::zeroed() }
});

#[export]
fn __getreent() -> *mut sys::_reent {
    ptr::from_ref(&REENT.0).cast_mut()
}

/// newlib's table for the C locale: one entry per byte after a leading one for EOF.
#[unsafe(export_name = "_ctype_")]
static CTYPE: [u8; 257] = ctype();

#[expect(clippy::indexing_slicing, reason = "`byte` is below 128")]
const fn ctype() -> [u8; 257] {
    const UPPER: u8 = 0o1;
    const LOWER: u8 = 0o2;
    const DIGIT: u8 = 0o4;
    const SPACE: u8 = 0o10;
    const PUNCT: u8 = 0o20;
    const CONTROL: u8 = 0o40;
    const HEX: u8 = 0o100;
    const BLANK: u8 = 0o200;
    let mut table = [0; 257];
    let mut byte = 0_u8;
    while byte < 128 {
        table[byte as usize + 1] = match byte {
            b' ' => SPACE | BLANK,
            b'\t'..=b'\r' => CONTROL | SPACE,
            0..=31 | 127 => CONTROL,
            b'0'..=b'9' => DIGIT,
            b'A'..=b'F' => UPPER | HEX,
            b'G'..=b'Z' => UPPER,
            b'a'..=b'f' => LOWER | HEX,
            b'g'..=b'z' => LOWER,
            _ => PUNCT,
        };
        byte += 1;
    }
    table
}

/// Each task's `errno`, boxed so that the address C holds stays put as tasks come and go.
static ERRNO: Mutex<RefCell<Vec<TaskErrno>>> = Mutex::new(RefCell::new(Vec::new()));

type TaskErrno = (usize, Box<c_int>);

fn task() -> usize {
    esp_radio_rtos_driver::current_task().as_ptr().addr()
}

#[export]
fn __errno() -> *mut c_int {
    let task = task();
    critical_section::with(|cs| {
        let mut errnos = ERRNO.borrow_ref_mut(cs);
        if !errnos.iter().any(|(owner, _)| *owner == task) {
            errnos.push((task, Box::new(0)));
        }
        errnos
            .iter_mut()
            .find(|(owner, _)| *owner == task)
            .map_or(ptr::null_mut(), |(_, errno)| ptr::from_mut(&mut **errno))
    })
}

pub fn set_errno(error: c_int) {
    // SAFETY: `__errno` returns the calling task's own `errno`.
    unsafe { *__errno() = error };
}

pub fn forget_task() {
    let task = task();
    critical_section::with(|cs| ERRNO.borrow_ref_mut(cs).retain(|(owner, _)| *owner != task));
}

#[export]
unsafe fn printf(format: *const c_char, args: ...) -> c_int {
    // SAFETY: `format` and `args` are the printf format and arguments C passed.
    let text = unsafe { printf_compat::output::display(format, args) };
    system::locked(|| esp_println::print!("{text}"));
    text.bytes_written()
}

#[export]
unsafe fn puts(text: *const c_char) -> c_int {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    system::locked(|| {
        Printer::write_bytes(bytes);
        Printer::write_bytes(b"\n");
    });
    0
}

#[export]
unsafe fn fputs(text: *const c_char, stream: &Stream) -> c_int {
    assert!(
        matches!(stream, Stream::Stdout),
        "the kernel supports only stdout for output"
    );
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    system::locked(|| Printer::write_bytes(bytes));
    0
}

#[export]
fn putchar(character: c_int) -> c_int {
    system::locked(|| Printer::write_bytes(&[character.to_le_bytes()[0]]));
    character
}

/// Nothing is buffered, so only a file has anything to report.
#[export]
#[expect(clippy::panic, reason = "flushing stdin is a firmware bug")]
fn fflush(stream: &Stream) -> c_int {
    match stream {
        Stream::Stdin => panic!("stdin is not for output"),
        Stream::Stdout => 0,
        #[cfg(feature = "esp32p4")]
        Stream::File(file) if file.failed() => EOF,
        #[cfg(feature = "esp32p4")]
        Stream::File(_) => 0,
    }
}

/// Mapping CR to LF is ESP-IDF's default for the console, and the monitor sends CR.
fn read_byte() -> Option<u8> {
    let usb = USB_DEVICE::regs();
    let uart = UART0::regs();
    let byte = if usb
        .ep1_conf()
        .read()
        .serial_out_ep_data_avail()
        .bit_is_set()
    {
        usb.ep1().read().rdwr_byte().bits()
    } else if uart.status().read().rxfifo_cnt().bits() > 0 {
        uart.fifo().read().rxfifo_rd_byte().bits()
    } else {
        return None;
    };
    Some(if byte == b'\r' { b'\n' } else { byte })
}

/// Waits for a whole line. Polling sleeps so that lower-priority tasks, like the watchdog's
/// feeder, still run.
#[export]
unsafe fn fgets(buffer: *mut c_char, size: c_int, stream: &Stream) -> *mut c_char {
    assert!(
        matches!(stream, Stream::Stdin),
        "the kernel supports only stdin for input"
    );
    let Some(room) = usize::try_from(size)
        .ok()
        .and_then(|size| size.checked_sub(1))
    else {
        return ptr::null_mut();
    };
    // SAFETY: C gives room for `size` bytes, and the text is at most `size - 1`.
    let text = unsafe { slice::from_raw_parts_mut(buffer.cast::<u8>(), room) };
    let mut length = 0;
    while let Some(slot) = text.get_mut(length) {
        let Some(byte) = read_byte() else {
            esp_radio_rtos_driver::usleep(10_000);
            continue;
        };
        *slot = byte;
        length += 1;
        if byte == b'\n' {
            break;
        }
    }
    // SAFETY: `length` is at most `room`, which leaves the byte for the NUL.
    unsafe { buffer.add(length).write(0) };
    buffer
}

/// stdin never fails.
#[export]
fn clearerr(_stream: &Stream) {}
