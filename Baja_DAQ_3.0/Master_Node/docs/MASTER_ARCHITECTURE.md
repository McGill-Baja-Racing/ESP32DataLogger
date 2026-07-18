# DAQ 3.0 Minimal Master

The ESP32-P4 master has one purpose: collect synchronized CAN sensor samples
and store them safely on an SD card.

## Included features

- FAT SD card mount at `/sdcard`
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
| `time/time_beacon.c` | Periodic master-clock broadcast |
| `console/serial_console.c` | Serial command parsing |

See `src/README.md` for dependencies and maintenance guidance.

## Fixed sensors

| Node | Measurement | CAN ID | Rate |
|---|---|---:|---:|
| 1 | Front brake pressure | `0x0B1` | 100 Hz |
| 1 | Rear brake pressure | `0x0B2` | 100 Hz |
| 4 | Bearing encoder | `0x0B9` | 50 Hz |
| 5 | Generic ADC voltage | `0x0BA` | 100 Hz |
| 5 | Engine RPM placeholder | `0x0BB` | 50 Hz |

The accepted IDs are compiled into the master. The corresponding sensor type,
rate, CAN ID, and GPIO configuration is compiled into each sensor node. There
is no runtime configuration protocol.

The generic ADC value is calibrated millivolts. Engine RPM is reserved in the
protocol and log decoder but currently reports zero until the analog tach peak
detection is implemented and validated on hardware.

Nodes report transitions on `0x0C0 + node ID`. The master records the latest
state for nodes 1, 4, and 5, prints transitions to serial, and includes their
active/idle/unknown state in the `status` output. State reports are control
information and are not written to the sensor log.

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
```
