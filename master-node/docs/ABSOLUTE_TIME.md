# GPS absolute time

Full CSV downloads retain `timestamp_ms` and add `absolute_time_utc`, for example
`2026-09-10T18:42:03.125Z`. Z means UTC. The UTC field is blank before GPS
synchronization, and for old logs without UTC metadata. No internet is required.

The GY-GPS6MV2 connects to UART1 at 9600 baud: GPS TX to master GPIO33,
GPS RX to master GPIO32, with common ground. The first checksum-valid active RMC
sentence with a valid date (2020–2099) anchors UTC to the ESP32 monotonic clock.
The receiver needs satellite reception. Verify its reported date against a known
clock during commissioning; older receivers can report GPS week rollover dates.

Each RMC UTC is paired with the time the first byte of that second's NMEA burst
arrived (the first byte after at least 50 ms of silence), not the end of the RMC
line: at 9600 baud a sentence takes about 70 ms to arrive. The UART delivers every
byte immediately so the burst start is not held in the receive FIFO. Only the first
valid RMC of a burst is used.

Later RMC sentences keep the clock disciplined (`time/clock_discipline.c`). Each
error between GPS UTC and the current estimate is spread over the next 32 s
instead of being applied at once, so absolute time never steps or runs backwards
and receiver jitter is averaged. Errors above 200 ms are ignored as bad
observations; after 5 in a row the clock is re-anchored (a logged step). If GPS
reception is lost, time keeps advancing at the ESP32 oscillator rate and drift
accumulates until reception returns. The host tests simulate 30 ppm drift with
±20 ms of jitter per sentence and check the estimate stays within 15 ms of the
GPS reference.

The receiver still outputs a burst some time after the UTC second it describes.
`GPS_UTC_OUTPUT_DELAY_US` in `gps/gps_receiver.c` (or a `-D` build flag) subtracts
that delay; it is 0 until measured on the target receiver, so absolute time is
currently late by that delay. Millisecond formatting is not millisecond UTC
accuracy. PPS synchronization is not implemented. Leap-second sentences with second
60 are rejected. Existing CAN time synchronization and millisecond sample values
are unchanged.
Samples must be within half the 32-bit millisecond wrap period (~24.8 days) of receipt.

Each new `log_NNNN.bin` has a `log_NNNN.bin.utc` companion on SD: one little-endian
signed 64-bit Unix millisecond value per 16-byte binary sample, zero if unsynchronized
when enqueued. This preserves UTC across reboots without changing the binary record
format. Keep both files together when copying SD logs. Binary web downloads contain
only the original records; use CSV downloads to export UTC. A missing/truncated
companion produces blank UTC cells for unavailable values. UTC adds 8 bytes per sample.

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
For a bench with only GPS attached, build/upload `MasterNoCAN`; select GPS speed
in Live Data while recording. `MasterStable` retains normal CAN support.
Full CSV and Powertrain CSV include absolute UTC time. Powertrain rows use the
UTC companion entry aligned with the engine sample. GPS-only logs have no
engine/bearing pairs to export.
