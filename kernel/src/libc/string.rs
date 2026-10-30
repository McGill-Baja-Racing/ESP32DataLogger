//! `string.h` and `stdlib.h`, and `snprintf`.

use core::{
    ffi::{CStr, c_char, c_double, c_int, c_long, c_uint, c_ulong, c_void},
    fmt::{self, Write},
    ptr,
};

use macros::export;
use sys::ERANGE;

use super::{set_errno, unsigned_char};
use crate::system::c_slice_mut;

/// Copies what fits and counts the rest.
pub struct Truncated<'a> {
    buffer: &'a mut [u8],
    pub length: usize,
}

impl<'a> Truncated<'a> {
    pub fn new(buffer: &'a mut [u8]) -> Self {
        Self { buffer, length: 0 }
    }
}

impl Write for Truncated<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        let room = self.buffer.get_mut(self.length..).unwrap_or_default();
        let length = room.len().min(text.len());
        if let Some((to, _)) = room.split_at_mut_checked(length) {
            to.copy_from_slice(text.as_bytes().split_at(length).0);
        }
        self.length += text.len();
        Ok(())
    }
}

struct Number {
    negative: bool,
    /// `None` if it overflows.
    value: Option<u32>,
    /// The bytes used, none if there are no digits.
    used: usize,
}

#[expect(clippy::panic, reason = "other bases are a firmware bug")]
fn number(text: &[u8], base: c_int) -> Number {
    let radix = match base {
        10 => 10,
        16 => 16,
        _ => panic!("the kernel supports only base 10 and 16"),
    };
    let blanks = text
        .iter()
        .take_while(|byte| byte.is_ascii_whitespace() || **byte == b'\x0b')
        .count();
    let rest = text.get(blanks..).unwrap_or_default();
    let (negative, rest) = match rest {
        [b'-', rest @ ..] => (true, rest),
        [b'+', rest @ ..] | rest => (false, rest),
    };
    let digits = match rest {
        [b'0', b'x' | b'X', after @ ..]
            if radix == 16 && after.first().is_some_and(u8::is_ascii_hexdigit) =>
        {
            after
        }
        _ => rest,
    };
    let values = || {
        digits
            .iter()
            .map_while(|&byte| char::from(byte).to_digit(radix))
    };
    let count = values().count();
    if count == 0 {
        return Number {
            negative: false,
            value: Some(0),
            used: 0,
        };
    }
    Number {
        negative,
        value: values().try_fold(0_u32, |value, digit| {
            value.checked_mul(radix)?.checked_add(digit)
        }),
        used: text.len() - digits.len() + count,
    }
}

unsafe fn parse(
    text: *const c_char,
    end: Option<&mut *mut c_char>,
    base: c_int,
) -> (bool, Option<u32>) {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    let Number {
        negative,
        value,
        used,
    } = number(bytes, base);
    if let Some(end) = end {
        // SAFETY: `used` is at most the string's length.
        *end = unsafe { text.add(used) }.cast_mut();
    }
    (negative, value)
}

#[export]
unsafe fn strtoul(text: *const c_char, end: Option<&mut *mut c_char>, base: c_int) -> c_ulong {
    // SAFETY: as in `parse`.
    match unsafe { parse(text, end, base) } {
        (true, Some(value)) => value.wrapping_neg(),
        (false, Some(value)) => value,
        (_, None) => {
            set_errno(ERANGE);
            c_ulong::MAX
        }
    }
}

#[export]
unsafe fn strtol(text: *const c_char, end: Option<&mut *mut c_char>, base: c_int) -> c_long {
    // SAFETY: as in `parse`.
    let (negative, value) = unsafe { parse(text, end, base) };
    let magnitude = value.map_or(i64::MAX, i64::from);
    let signed = if negative { -magnitude } else { magnitude };
    c_long::try_from(signed).unwrap_or_else(|_out_of_range| {
        set_errno(ERANGE);
        if negative { c_long::MIN } else { c_long::MAX }
    })
}

#[export]
unsafe fn strtok_r(text: *mut c_char, delimiters: &str, save: &mut *mut c_char) -> *mut c_char {
    let start = if text.is_null() { *save } else { text };
    if start.is_null() {
        return ptr::null_mut();
    }
    let delimiters = delimiters.as_bytes();
    // SAFETY: C passes a NUL-terminated string, here or in the earlier call that set `save`.
    let bytes = unsafe { CStr::from_ptr(start) }.to_bytes();
    let skipped = bytes
        .iter()
        .take_while(|byte| delimiters.contains(byte))
        .count();
    let length = bytes
        .iter()
        .skip(skipped)
        .take_while(|byte| !delimiters.contains(byte))
        .count();
    // SAFETY: `skipped + length` is at most the string's length, so `end` is at or before the NUL.
    unsafe {
        let token = start.add(skipped);
        let end = token.add(length);
        if length == 0 {
            *save = end;
            return ptr::null_mut();
        }
        if end.read() == 0 {
            *save = end;
        } else {
            end.write(0);
            *save = end.add(1);
        }
        token
    }
}

#[cfg(esp32p4)]
/// Copies `text` and a NUL to `to`, cutting the text short to fit, and returns the length copied.
pub fn copy_c_str(to: &mut [c_char], text: &[u8]) -> usize {
    let length = text.len().min(to.len().saturating_sub(1));
    let Some((nul, head)) = to.get_mut(..=length).and_then(<[_]>::split_last_mut) else {
        return 0;
    };
    head.copy_from_slice(text.split_at(length).0);
    *nul = 0;
    length
}

/// Returns the length the whole text has, as C99 does.
#[export]
unsafe fn snprintf(buffer: *mut c_char, size: c_uint, format: *const c_char, args: ...) -> c_int {
    // SAFETY: `format` and `args` are the printf format and arguments C passed.
    let text = unsafe { printf_compat::output::display(format, args) };
    // SAFETY: C gives room for `size` bytes at `buffer`.
    let buffer = unsafe { c_slice_mut(buffer.cast::<u8>(), size as usize) }.unwrap_or_default();
    let room = buffer.len().saturating_sub(1);
    let (text_room, _) = buffer.split_at_mut(room);
    let mut out = Truncated::new(text_room);
    write!(out, "{text}").ok();
    let end = out.length.min(room);
    if let Some(nul) = buffer.get_mut(end) {
        *nul = 0;
    }
    text.bytes_written()
}

/// Only `digits[.digits]`, which the GPS parser checks before it converts; anything else
/// converts nothing.
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
    let fraction = if whole > 0 && bytes.get(whole) == Some(&b'.') {
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
    let wanted = unsigned_char(character);
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
    let wanted = unsigned_char(character);
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
    if size == 0 || count < 2 {
        return;
    }
    // SAFETY: C passes `count` elements of `size` bytes at `base`.
    let Some(array) = (unsafe { c_slice_mut(base.cast::<u8>(), length) }) else {
        return;
    };
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
