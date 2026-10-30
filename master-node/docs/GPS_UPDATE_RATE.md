# GPS update rate

Read [GPS absolute time](ABSOLUTE_TIME.md) first. This page explains how the master gets
GPS position, speed and UTC five times a second instead of once, and what to check on
the bench. [Hardware](../../docs/HARDWARE.md) owns the wiring.

## What changes for users

| | Before | Now |
|---|---|---|
| Speed, latitude, longitude samples | 1 per second | 5 per second |
| NMEA sentences sent by the receiver | GGA, GLL, GSA, GSV, RMC, VTG | RMC only |
| Serial baud rate | 9600 | 9600 (unchanged) |
| `Fix:` log line on the serial console | every fix | at most once per second |

Logs, live data and CSV exports need no change: GPS samples simply arrive more often.
In the Powertrain CSV the GPS columns hold the latest reading, so they are now at most
200 ms old instead of 1 s.

## How it works

The GY-GPS6MV2 carries a u-blox **NEO-6M**. It sends 1 Hz NMEA by default, can measure
up to 5 Hz, and is configured with u-blox binary (UBX) commands. The setup only changes the
receiver's RAM settings and is not saved, so the master sends it on every boot, from
the GPS task before it starts reading sentences (`configure_update_rate()` in
`src/gps/gps_receiver.c`):

1. Five **UBX-CFG-MSG** commands turn off GGA, GLL, GSA, GSV and VTG. The firmware only
   parses RMC, and at 9600 baud all six default sentences take about 500 ms to send,
   so they could not fit five times a second.
2. One **UBX-CFG-RATE** command sets the measurement period to 200 ms, aligned to GPS
   time, so whole UTC seconds still land on a measurement (`.000`, `.200`, … `.800`).
   It is sent only if all five sentences were confirmed off: with even one extra
   sentence, 5 Hz bursts would run into each other, so the receiver stays at 1 Hz.

The bytes are built in `src/gps/ubx_config.c`, which has no ESP-IDF calls so the host
tests can compare them with the reference frames from the u-blox 6 protocol
specification. After each command the master waits up to 250 ms for the receiver's
**UBX-ACK-ACK**, skipping the NMEA text that keeps arriving meanwhile, and retries once.
It then empties the UART receive buffer so the acknowledgements cannot be mistaken for
the start of a burst.

### Why stay at 9600 baud

RMC alone takes about 75 ms at 9600 baud, leaving about 125 ms of silence before the
next burst. That is enough for the 50 ms gap that marks a new burst
([absolute time](ABSOLUTE_TIME.md)). Keeping the baud rate means a failed setup is
harmless: the receiver keeps sending 1 Hz NMEA at 9600 and everything works as before.
A faster link would need the master and receiver to agree on a new baud rate, including
after an ESP32 reset while the receiver stays powered, for no gain at this rate.

### Effect on absolute time

Each RMC now names a fractional time (`hhmmss.200`), which the parser already
accepted. Every burst is paired with its own measurement, so the clock is observed
five times a second. The correction of each error is still spread over 32 s, so the
clock reacts as quickly as before but averages five times more readings. The rule that
re-anchors the clock after persistent disagreement now needs both 5 rejections in
a row and 5 s since the first of them; before, 5 rejections alone were enough, which
would have meant only 1 s at 5 Hz.

## Failure behaviour

| Situation | Result |
|---|---|
| Receiver acknowledges all 6 commands | `Receiver set to RMC every 200 ms`, 5 Hz |
| Master → receiver wire (GPIO32 → RX) missing, or another receiver model | `Receiver acknowledged 0 of 6 rate commands; staying at its 1 Hz default`, everything else unchanged |
| One sentence could not be turned off | Rate command not sent; 1 Hz with the remaining sentences |
| No receiver connected | Same warning after about 2.5 s, then the task waits for sentences as before |
| Receiver powered after the master | Setup is missed; 1 Hz until the next master boot |

Setup delays the first GPS sample by well under a second when the receiver answers,
and by at most 2.5 s when it does not.

## Bench check

1. Flash `MasterNoCAN` with only the GPS attached and open the serial monitor.
2. At boot, expect `Receiver set to RMC every 200 ms`.
3. Outdoors with a fix, record for a minute and export the full CSV: GPS speed rows
   should be about 200 ms apart (`timestamp_ms` steps of roughly 200).
4. Still open: measure `GPS_UTC_OUTPUT_DELAY_US` (the delay between a measurement and
   the start of its burst) with a PPS-capable reference.

Next: [GPS absolute time](ABSOLUTE_TIME.md), [Testing](../../docs/TESTING.md).
