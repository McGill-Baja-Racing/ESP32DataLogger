# GPS absolute time

Read the [system overview](../../docs/SYSTEM_OVERVIEW.md) and [master source map](../src/README.md) first. This reference owns UTC synchronization and accuracy limits; [Hardware](../../docs/HARDWARE.md) owns wiring and [Log format](../../docs/LOG_FORMAT.md) owns companion/export representation.

Full CSV downloads retain `timestamp_ms` and add `absolute_time_utc`, for example
`2026-09-10T18:42:03.125Z`. Z means UTC. The UTC field is blank before GPS
synchronization, and for old logs without UTC metadata. No internet is required.

## Establishing the UTC anchor

The receiver connection is documented in [Hardware](../../docs/HARDWARE.md). The first checksum-valid active RMC
sentence with a valid date (2020–2099) anchors UTC to the ESP32 monotonic clock.
The receiver needs satellite reception. Verify its reported date against a known
clock during commissioning; older receivers can report GPS week rollover dates.

At boot the master sets the receiver to send only RMC, five times a second (see
[GPS update rate](GPS_UPDATE_RATE.md)). Each RMC UTC is paired with the time the
first byte of that measurement's NMEA burst arrived (the first byte after at least
50 ms of silence), not the end of the RMC line: at 9600 baud a sentence takes about
75 ms to arrive. The UART delivers every
byte immediately so the burst start is not held in the receive FIFO. Only the first
valid RMC of a burst is used.

## Keeping the clock locked to GPS

Later RMC sentences keep the clock disciplined (`time/clock_discipline.c`). Each
error between GPS UTC and the current estimate is spread over the next 32 s
instead of being applied at once, so absolute time never steps or runs backwards
and receiver jitter is averaged. Errors above 200 ms are ignored as bad
observations; after 5 in a row spanning at least 5 s the clock is re-anchored (a
logged step). The duration keeps this the same at 5 Hz and at the receiver's 1 Hz
default. If GPS
reception is lost, time keeps advancing at the ESP32 oscillator rate and drift
accumulates until reception returns. The correction time constant is also
independent of the rate: at 5 Hz each sentence moves the clock five times less,
but there are five times more. The host tests simulate 30 ppm drift with ±20 ms
of jitter per sentence, at 1 Hz and 5 Hz, and check the estimate stays within
15 ms of the GPS reference.

## Accuracy and clock limits

The receiver still outputs a burst some time after the measurement it describes.
`GPS_UTC_OUTPUT_DELAY_US` in `gps/gps_receiver.c` (or a `-D` build flag) subtracts
that delay; it is 0 until measured on the target receiver, so absolute time is
currently late by that delay. Millisecond formatting is not millisecond UTC
accuracy. PPS synchronization is not implemented. Leap-second sentences with second
60 are rejected. Existing CAN time synchronization and millisecond sample values
are unchanged.
Samples must be within half the 32-bit millisecond wrap period (~24.8 days) of receipt.

Samples retain the UTC value available when enqueued. Unsynchronized samples stay blank in absolute-time export, including after later synchronization. Storage alignment and missing-companion behavior are specified in [Log format](../../docs/LOG_FORMAT.md).

## GPS parsing and accepted data

The existing `gps_receiver` owns UART1 and parses each complete RMC sentence once
with `gps/rmc_parser.c`. That parser validates the checksum, complete fields,
coordinate ranges and hemispheres, and speed before passing decoded UTC to the
clock and logging speed/latitude/longitude samples. Numeric suffixes, non-finite
values, negative speeds, and speeds above the uint16 km/h ×100 range are rejected.
Missing speed or position suppresses the telemetry group rather than logging a
fabricated zero. Zero speed and coordinates on the equator/prime meridian are valid.
GPS telemetry continues when only its UTC date/time is rejected; valid UTC can
still synchronize the clock when telemetry fields are empty. Truncated, overlong,
and malformed telemetry sentences do not synchronize the clock or emit samples.
There is no second UART reader or second checksum/field parser.

## Bench observation and exports

For a bench with only GPS attached, build/upload `MasterNoCAN`; select GPS speed
in Live Data while recording. `MasterStable` retains normal CAN support.
Full CSV and Powertrain CSV include absolute UTC time. Powertrain rows use the
UTC companion entry aligned with the engine sample. GPS-only logs have no
engine/bearing pairs to export.

Next: [Master architecture](MASTER_ARCHITECTURE.md), [Testing](../../docs/TESTING.md), or the [master introduction](../README.md).
