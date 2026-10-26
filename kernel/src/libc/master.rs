//! The C library functions only the master calls.

use core::{
    ffi::{CStr, c_char, c_double, c_int, c_uint, c_void},
    fmt, ptr, slice,
};

use macros::export;
use sys::ERANGE;

use super::set_errno;

/// Copies what fits and counts the rest.
pub(super) struct Truncated<'a> {
    pub(super) buffer: &'a mut [u8],
    pub(super) length: usize,
}

impl fmt::Write for Truncated<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        let room = self.buffer.get_mut(self.length..).unwrap_or_default();
        room.iter_mut()
            .zip(text.bytes())
            .for_each(|(to, from)| *to = from);
        self.length += text.len();
        Ok(())
    }
}

/// Returns the length the whole text has, as C99 does.
#[export]
unsafe fn snprintf(buffer: *mut c_char, size: c_uint, format: *const c_char, args: ...) -> c_int {
    // SAFETY: `format` and `args` are the printf format and arguments C passed.
    let text = unsafe { printf_compat::output::display(format, args) };
    let room = (size as usize).saturating_sub(1);
    let bytes = if size == 0 {
        &mut []
    } else {
        // SAFETY: C gives room for `size` bytes at `buffer`.
        unsafe { slice::from_raw_parts_mut(buffer.cast::<u8>(), room) }
    };
    let mut out = Truncated {
        buffer: bytes,
        length: 0,
    };
    fmt::Write::write_fmt(&mut out, format_args!("{text}")).ok();
    if size > 0 {
        // SAFETY: `buffer` has `room` bytes and then one more for the NUL.
        unsafe { buffer.add(out.length.min(room)).write(0) };
    }
    text.bytes_written()
}

/// Only `digits[.digits]`, which the GPS parser checks before it converts.
#[export]
unsafe fn strtod(text: *const c_char, end: Option<&mut *mut c_char>) -> c_double {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    let digits = |from: usize| {
        bytes.get(from..).map_or(0, |rest| {
            rest.iter().take_while(|byte| byte.is_ascii_digit()).count()
        })
    };
    let whole = digits(0);
    assert!(whole > 0, "the kernel supports only digits[.digits]");
    let fraction = if bytes.get(whole) == Some(&b'.') {
        digits(whole + 1)
    } else {
        0
    };
    let used = if fraction > 0 {
        whole + 1 + fraction
    } else {
        whole
    };
    if let Some(end) = end {
        // SAFETY: `used` is at most the string's length.
        *end = unsafe { text.add(used) }.cast_mut();
    }
    // Exact for the GPS fields: up to 15 digits, and at most 22 of them after the point.
    let value = bytes
        .iter()
        .take(used)
        .filter(|&&byte| byte != b'.')
        .fold(0.0, |value, &byte| {
            value * 10.0 + c_double::from(byte - b'0')
        })
        / (0..fraction).fold(1.0, |scale, _| scale * 10.0);
    if value.is_infinite() {
        set_errno(ERANGE);
    }
    value
}

/// The ROM of the ESP32-P4 lacks `strchr`, `strrchr` and `strcspn`.
#[export]
unsafe fn strchr(text: *const c_char, character: c_int) -> *mut c_char {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes_with_nul();
    let wanted = character.to_le_bytes()[0];
    bytes
        .iter()
        .position(|&byte| byte == wanted)
        .map_or(ptr::null_mut(), |i| {
            // SAFETY: `i` is within the string and its NUL.
            unsafe { text.add(i) }.cast_mut()
        })
}

#[export]
unsafe fn strrchr(text: *const c_char, character: c_int) -> *mut c_char {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes_with_nul();
    let wanted = character.to_le_bytes()[0];
    bytes
        .iter()
        .rposition(|&byte| byte == wanted)
        .map_or(ptr::null_mut(), |i| {
            // SAFETY: `i` is within the string and its NUL.
            unsafe { text.add(i) }.cast_mut()
        })
}

#[export]
unsafe fn strcspn(text: *const c_char, reject: &str) -> c_uint {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    let length = bytes
        .iter()
        .take_while(|byte| !reject.as_bytes().contains(byte))
        .count();
    c_uint::try_from(length).unwrap_or(c_uint::MAX)
}

/// Insertion sort: it needs no memory, and the firmware sorts a few hundred elements.
#[export]
#[expect(clippy::panic, reason = "a NULL comparator is a firmware bug")]
unsafe fn qsort(base: *mut c_void, count: usize, size: usize, compare: sys::__compar_fn_t) {
    let (Some(length), Some(compare)) = (count.checked_mul(size), compare) else {
        panic!("qsort needs a comparator and an array that fits in memory");
    };
    if size == 0 {
        return;
    }
    // SAFETY: C passes `count` elements of `size` bytes at `base`.
    let array = unsafe { slice::from_raw_parts_mut(base.cast::<u8>(), length) };
    for end in 1..count {
        for at in (1..=end).rev() {
            let (head, tail) = array.split_at_mut(at * size);
            let (Some(left), Some(right)) = (
                head.rchunks_exact_mut(size).next(),
                tail.chunks_exact_mut(size).next(),
            ) else {
                return;
            };
            // SAFETY: `compare` is C's comparator, which takes pointers to two elements.
            if unsafe { compare(left.as_ptr().cast(), right.as_ptr().cast()) } <= 0 {
                break;
            }
            left.swap_with_slice(right);
        }
    }
}
