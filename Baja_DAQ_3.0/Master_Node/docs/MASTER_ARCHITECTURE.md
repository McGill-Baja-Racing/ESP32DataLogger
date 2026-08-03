# DAQ 3.0 Minimal Master

The ESP32-P4 master has one purpose: collect synchronized CAN sensor samples
and store them safely on an SD card.

## Included features

- FAT SD card mount at `/sdcard`
- optional FAT formatting when an inserted card cannot be mounted, controlled
  by the `SD_FORMAT_IF_MOUNT_FAILED` build flag
- 1 Mbit/s TWAI/CAN on TX GPIO 20 and RX GPIO 21
- fixed master-side list of accepted sensor CAN IDs
- global node start and stop commands
- 64-bit master microsecond time beacon every 100 ms
- queued, buffered binary SD logging
- automatic CAN bus-off detection and controller recovery
- automatic logging 500 ms after boot
- graceful stop, queue drain, flush, and file close
- serial `start`, `stop`, and `status` commands at 115200 baud
- registration of node boot, start, stop, and recovery states
- open `BajaDAQ` SoftAP and HTTP controls at `http://192.168.4.1`
- binary and streamed CSV downloads for completed log sessions
- `tools/decode_log.py` to convert a log to CSV

## Source organization

`main.c` initializes and connects cohesive modules:

| Path | Responsibility |
|---|---|
| `protocol/app_protocol.h` | CAN IDs shared conceptually with sensor nodes |
| `can/can_master.c` | TWAI transport, receive dispatch, and recovery |
| `logger/data_logger.c` | Log state, queue, binary blocks, and file writes |
| `storage/sd_card.c` | SDMMC hardware and FAT mount |
| `node_state/node_registry.c` | Latest node state acknowledgements |
| `diagnostics/diagnostic_registry.c` | Deduplicated active/recent node faults |
| `time/time_beacon.c` | Periodic master-clock broadcast |
| `console/serial_console.c` | Serial command parsing |
| `app/app_control.c` | Serialized logging lifecycle shared by serial and HTTP |
| `web/web_server.c` | ESP-Hosted SoftAP, HTTP API, UI, and downloads |

See `src/README.md` for dependencies and maintenance guidance.

## Fixed sensors

| Node | Measurement | CAN ID | Rate |
|---|---|---:|---:|
| 1 | Front brake pressure | `0x0B1` | 100 Hz |
| 1 | Rear brake pressure | `0x0B2` | 100 Hz |
| 4 | Signed bearing RPM | `0x0B9` | 50 Hz |
| 5 | Engine RPM placeholder | `0x0BB` | 50 Hz |
| 6 | Generic ADC voltage | `0x0BA` | 100 Hz |

The accepted IDs are compiled into the master. The corresponding sensor type,
rate, CAN ID, and GPIO configuration is compiled into each sensor node. There
is no runtime configuration protocol.

The generic ADC value is calibrated millivolts. Engine RPM is reserved in the
protocol and log decoder but currently reports zero until the analog tach peak
detection is implemented and validated on hardware.

Nodes report boot, start, stop, beacon synchronization, and recovery transitions
on `0x0C0 + node ID`. The high bit of each 100 ms time beacon carries the
Master's recording state; the remaining 63 bits carry master microseconds. A
node that reboots during a recording therefore resumes sampling on its next
beacon without requiring the Master to repeat the original START command.
While recording, the master uses normal sensor frames as proof that configured
nodes are active. The expected-node mask in `node_registry.h` currently enables
nodes 4 and 5. The `status` output shows waiting until the first frame,
active while frames arrive, and offline after three seconds without sensor data.
Unconfigured nodes appear as disabled. When the logger is stopped, configured
node status is off and no liveness traffic is sent. State reports are control
information and are not written to the sensor log.

## Sensor diagnostics

Sensor nodes send diagnostic transitions on CAN ID 0x0D0 + node ID. Payload
bytes 0-1 are a stable code; byte 2 contains active, severity, data-degraded,
and timestamp-valid flags; byte 3 is a saturating occurrence count; and bytes
4-7 are synchronized timestamp_ms.

Each transition is sent three times. The Master deduplicates it, keeps bounded
active/recent state, prints it, exposes it at /api/diagnostics, and logs one
logical event while recording. It never treats diagnostics as sensor liveness.
V1 codes are: 0201 TX failure, 0202 RX overflow, 0203 warning, 0204 passive,
0205 bus-off, 0206 recovery-start failure, 0301 stale time, 0401 missed period,
0402 sample queue replacement, 0403 diagnostic queue overflow, 1101 invalid
quadrature burst, 1102 bearing RPM limit, 1201 rejected engine-pulse burst, and
1202 engine RPM limit. They are report-only and never alter sampling.

## Time synchronization

The master broadcasts CAN ID `0x0A2` every 100 ms. Its eight-byte payload is
the master's little-endian `esp_timer_get_time()` value in microseconds.

Each sensor node calculates:

```text
clock_offset = master_time_us - node_local_time_us
sample_time  = node_local_sample_time_us + clock_offset
```

The node stores `sample_time / 1000` in its sensor frame. Samples from every
node therefore use the master's millisecond timestamp base.

## Logging pipeline

```text
CAN callback -> receive queue -> dispatch task -> log queue -> SD writer
```

Each log record is 16 bytes:

```text
int64 little-endian: CAN ID
int64 little-endian: packed sensor payload
    low 32 bits: value
    high 32 bits: synchronized timestamp_ms
```

The SD writer flushes blocks of 100 records. A stop command prevents new
records, stops the nodes, drains the log queue, writes the partial block, and
closes the file.

## Build and decode

```bash
pio run -e MasterStable
python3 tools/decode_log.py log_0001.bin
python3 tools/check_log.py log_0001.bin
```

`check_log.py` compares consecutive timestamps independently for every known
CAN ID. It uses a 50% timing tolerance, estimates missing messages from larger
gaps, requires sensors belonging to the configured Master node mask, and writes
a detailed `_gaps.csv` report. Its findings identify that a loss occurred
somewhere in the acquisition pipeline, not which component caused it.
Diagnostic records are summarized separately and excluded from sampling-loss
and unknown-ID calculations.
