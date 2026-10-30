//! The C library functions the firmware calls, on the console. Only what the firmware uses.

use alloc::{boxed::Box, collections::BTreeMap};
use core::{
    cell::RefCell,
    ffi::{CStr, c_char, c_int},
    ptr,
};

use critical_section::Mutex;
use esp_hal::peripherals::{UART0, USB_DEVICE};
use esp_println::Printer;
use macros::export;

use crate::system::{self, c_slice_mut};

pub mod string;
pub mod time;

pub enum Stream {
    Stdin,
    Stdout,
    #[cfg(esp32p4)]
    File(crate::files::Handle),
}

sys::c_handle!(Stream, *mut sys::FILE, owned);

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
static ERRNO: Mutex<RefCell<BTreeMap<usize, Box<c_int>>>> =
    Mutex::new(RefCell::new(BTreeMap::new()));

fn task() -> usize {
    esp_radio_rtos_driver::current_task().as_ptr().addr()
}

#[export]
fn __errno() -> *mut c_int {
    let task = task();
    critical_section::with(|cs| {
        ptr::from_mut(
            &mut **ERRNO
                .borrow_ref_mut(cs)
                .entry(task)
                .or_insert_with(|| Box::new(0)),
        )
    })
}

pub fn set_errno(error: c_int) {
    // SAFETY: `__errno` returns the calling task's own `errno`.
    unsafe { *__errno() = error };
}

pub fn forget_task() {
    let task = task();
    critical_section::with(|cs| ERRNO.borrow_ref_mut(cs).remove(&task));
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

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "C converts to unsigned char"
)]
fn unsigned_char(character: c_int) -> u8 {
    character as u8
}

#[export]
fn putchar(character: c_int) -> c_int {
    system::locked(|| Printer::write_bytes(&[unsigned_char(character)]));
    character
}

/// Nothing is buffered, so only a file has anything to report.
#[export]
#[expect(clippy::panic, reason = "flushing stdin is a firmware bug")]
fn fflush(stream: &Stream) -> c_int {
    match stream {
        Stream::Stdin => panic!("stdin is not for output"),
        Stream::Stdout => 0,
        #[cfg(esp32p4)]
        Stream::File(file) if file.failed() => sys::EOF,
        #[cfg(esp32p4)]
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

/// Sleeps between polls so that lower-priority tasks, like the watchdog's feeder, still run.
fn wait_byte() -> u8 {
    loop {
        if let Some(byte) = read_byte() {
            return byte;
        }
        esp_radio_rtos_driver::usleep(10_000);
    }
}

/// Waits for a whole line.
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
    let Some(text) = (unsafe { c_slice_mut(buffer.cast::<u8>(), room) }) else {
        return ptr::null_mut();
    };
    let mut length = 0;
    for slot in text {
        *slot = wait_byte();
        length += 1;
        if *slot == b'\n' {
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
