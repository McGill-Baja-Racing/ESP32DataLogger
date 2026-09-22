# GPS absolute time

Full CSV downloads retain `timestamp_ms` and add `absolute_time_utc`, for example
`2026-09-10T18:42:03.125Z`. Z means UTC. The UTC field is blank before GPS
synchronization, and for old logs without UTC metadata. No internet is required.

The GY-GPS6MV2 connects to UART1 at 9600 baud: GPS TX to master GPIO33,
GPS RX to master GPIO32, with common ground. The first checksum-valid active RMC
sentence with a valid date (2020–2099) anchors UTC to the ESP32 monotonic clock.
The receiver needs satellite reception. Verify its reported date against a known
clock during commissioning; older receivers can report GPS week rollover dates.

The anchor is fixed until reboot, preventing serial jitter from stepping timestamps.
The ESP32 keeps advancing time if GPS reception is lost, but oscillator drift
accumulates; this version does not discipline the clock continuously. UART latency
also offsets absolute time. Millisecond formatting is not millisecond UTC accuracy.
PPS synchronization is not implemented. Leap-second sentences with second 60 are
rejected. Existing CAN time synchronization and millisecond sample values are unchanged.
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
Full CSV includes the new column; Powertrain CSV retains its
specialized RPM schema. GPS-only logs have no RPM pairs to export.
