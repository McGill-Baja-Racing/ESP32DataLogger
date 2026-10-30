//! `time.h`: UTC conversion, and the clock that `settimeofday` sets.

use core::{
    cell::Cell,
    ffi::{c_char, c_int},
    fmt::Write,
    mem::MaybeUninit,
};

use critical_section::Mutex;
use esp_radio_rtos_driver as rtos;
use macros::export;
use sys::{time_t, timeval, timezone, tm};

use super::string::Truncated;
use crate::system::c_slice_mut;

/// The microseconds to add to esp-rtos's time to get UTC.
static OFFSET: Mutex<Cell<i64>> = Mutex::new(Cell::new(0));

/// UTC in seconds, which counts from boot until `settimeofday`.
#[cfg(esp32p4)]
pub fn now() -> time_t {
    let offset = critical_section::with(|cs| OFFSET.borrow(cs).get());
    (rtos::now().cast_signed() + offset).div_euclid(1_000_000)
}

/// `seconds` since the epoch, split into calendar fields; `None` beyond what `tm` holds.
pub fn civil(seconds: time_t) -> Option<tm> {
    let days = c_int::try_from(seconds.div_euclid(86_400)).ok()?;
    let time = c_int::try_from(seconds.rem_euclid(86_400)).ok()?;
    // Days from 1 March of year 0, so that a leap day ends the year.
    let z = days.checked_add(719_468)?;
    let era = z.div_euclid(146_097);
    let day_of_era = z.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month = (5 * day_of_year + 2) / 153;
    let year = year_of_era + era * 400 + c_int::from(month >= 10);
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    Some(tm {
        tm_sec: time % 60,
        tm_min: time / 60 % 60,
        tm_hour: time / 3600,
        tm_mday: day_of_year - (153 * month + 2) / 5 + 1,
        tm_mon: if month < 10 { month + 2 } else { month - 10 },
        tm_year: year - 1900,
        tm_wday: (days + 4).rem_euclid(7),
        // The year starts on day 306 of the March-based one.
        tm_yday: if day_of_year >= 306 {
            day_of_year - 306
        } else {
            day_of_year + 59 + c_int::from(leap)
        },
        tm_isdst: 0,
    })
}

#[export]
fn gmtime_r<'call>(time: &time_t, result: &'call mut MaybeUninit<tm>) -> Option<&'call mut tm> {
    civil(*time).map(|fields| result.write(fields))
}

/// Only the conversions the firmware formats its timestamps with.
#[export]
#[expect(clippy::panic, reason = "other conversions are a firmware bug")]
unsafe fn strftime(buffer: *mut c_char, size: usize, format: &str, time: &tm) -> usize {
    // SAFETY: C gives room for `size` bytes at `buffer`.
    let Some(room) = (unsafe { c_slice_mut(buffer.cast::<u8>(), size) }) else {
        return 0;
    };
    let mut out = Truncated::new(room);
    let mut characters = format.chars();
    while let Some(character) = characters.next() {
        if character != '%' {
            out.write_char(character).ok();
            continue;
        }
        let (value, width) = match characters.next() {
            Some('Y') => (time.tm_year + 1900, 4),
            Some('m') => (time.tm_mon + 1, 2),
            Some('d') => (time.tm_mday, 2),
            Some('H') => (time.tm_hour, 2),
            Some('M') => (time.tm_min, 2),
            Some('S') => (time.tm_sec, 2),
            Some('%') => {
                out.write_char('%').ok();
                continue;
            }
            _ => panic!("the kernel supports only %Y %m %d %H %M %S and %%"),
        };
        write!(out, "{value:0width$}").ok();
    }
    if out.length >= size {
        return 0;
    }
    // SAFETY: the text fit, and `length < size` leaves the byte for the NUL.
    unsafe { buffer.add(out.length).write(0) };
    out.length
}

/// The time zone is always UTC.
#[export]
fn settimeofday(time: &timeval, zone: Option<&timezone>) -> c_int {
    assert!(zone.is_none(), "the kernel supports only UTC");
    let utc = time.tv_sec * 1_000_000 + i64::from(time.tv_usec);
    let offset = utc - rtos::now().cast_signed();
    critical_section::with(|cs| OFFSET.borrow(cs).set(offset));
    0
}
