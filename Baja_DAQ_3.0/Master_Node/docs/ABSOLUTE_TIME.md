# GPS absolute time

CSV downloads retain `timestamp_ms` and add `absolute_time_utc`, for example
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

The existing `gps_receiver` owns UART1 and forwards each complete NMEA sentence
for time synchronization before logging speed/latitude/longitude samples. GPS data
continues to work even when its UTC date is rejected. There is no second UART reader.
For a bench with only GPS attached, build/upload `MasterNoCAN`; select GPS speed
in Live Data while recording. `MasterStable` retains normal CAN support.
Full CSV and CVT input CSV include the new column; Paired RPM CSV retains its
specialized RPM schema. GPS-only logs have no RPM pairs to export.
