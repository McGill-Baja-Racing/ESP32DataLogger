//! `string.h` and `stdlib.h`: parsing numbers and splitting strings.

use core::{
    ffi::{CStr, c_char, c_int, c_long, c_ulong},
    ptr,
};

use macros::export;
use sys::ERANGE;

use super::set_errno;

/// The sign, the value (`None` if it overflows), and the bytes used, with none used if there
/// are no digits.
#[expect(clippy::panic, reason = "other bases are a firmware bug")]
fn number(text: &[u8], base: c_int) -> (bool, Option<u32>, usize) {
    let radix = match base {
        10 => 10,
        16 => 16,
        _ => panic!("the kernel supports only base 10 and 16"),
    };
    let skipped = text
        .iter()
        .take_while(|byte| byte.is_ascii_whitespace() || **byte == 0x0b)
        .count();
    let rest = text.get(skipped..).unwrap_or_default();
    let negative = rest.first() == Some(&b'-');
    let signed = usize::from(negative || rest.first() == Some(&b'+'));
    let rest = rest.get(signed..).unwrap_or_default();
    let prefix = if radix == 16
        && matches!(rest, [b'0', b'x' | b'X', digit, ..] if digit.is_ascii_hexdigit())
    {
        2
    } else {
        0
    };
    let digits = rest.get(prefix..).unwrap_or_default();
    let mut value = Some(0_u32);
    let mut count = 0;
    for digit in digits
        .iter()
        .map_while(|&byte| char::from(byte).to_digit(radix))
    {
        value = value.and_then(|value| value.checked_mul(radix)?.checked_add(digit));
        count += 1;
    }
    if count == 0 {
        return (false, Some(0), 0);
    }
    (negative, value, skipped + signed + prefix + count)
}

/// Runs `number` on the C string and stores where it stopped.
unsafe fn parse(
    text: *const c_char,
    end: Option<&mut *mut c_char>,
    base: c_int,
) -> (bool, Option<u32>) {
    // SAFETY: C passes a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    let (negative, value, used) = number(bytes, base);
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
