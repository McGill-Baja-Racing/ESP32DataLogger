# GPS absolute time

Read the [system overview](../../docs/SYSTEM_OVERVIEW.md) and [master source map](../src/README.md) first. This reference owns UTC synchronization and accuracy limits; [Hardware](../../docs/HARDWARE.md) owns wiring and [Log format](../../docs/LOG_FORMAT.md) owns companion/export representation.

Full CSV downloads retain `timestamp_ms` and add `absolute_time_utc`, for example
`2026-09-10T18:42:03.125Z`. Z means UTC. The UTC field is blank before GPS
synchronization, and for old logs without UTC metadata. No internet is required.

The receiver connection is documented in [Hardware](../../docs/HARDWARE.md). The first checksum-valid active RMC
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

Samples retain the UTC value available when enqueued. Unsynchronized samples stay blank in absolute-time export, including after later synchronization. Storage alignment and missing-companion behavior are specified in [Log format](../../docs/LOG_FORMAT.md).

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

Next: [Master architecture](MASTER_ARCHITECTURE.md), [Testing](../../docs/TESTING.md), or the [master introduction](../README.md).
