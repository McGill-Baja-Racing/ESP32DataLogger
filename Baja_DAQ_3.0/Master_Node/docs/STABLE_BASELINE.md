# Master Stable Baseline

This copy is intentionally reduced to the reliable logger foundation.

For the complete master architecture, configuration precedence, task
priorities, boot workflow, operating procedure, log format, and upgrade
roadmap, see [DAQ 3.0 Master Node Architecture and Operation](MASTER_ARCHITECTURE.md).

## Included

- ESP32-P4 master firmware
- SD card mount and binary log writing
- TWAI/CAN receive, node start/stop/config, and CAN recovery
- Serial commands: `start`, `stop`, `status`, `files`, `download <filename>`, `log <mode>`
- Built-in node/sensor fallback configuration
- Optional master-local GPS/RPM samples, disabled in the baseline build
- `tools/decode_log.py` for converting downloaded `.bin` logs

## Removed

- WiFi / ESP-Hosted / ESP WiFi Remote
- MQTT telemetry and command bridge
- HTTP/cloud upload
- Streamlit dashboards and local MQTT tooling
- Generated build folders and old sdkconfig snapshots

## Build

```bash
pio run -e MasterStable
```

`MasterStable` is the baseline target. It builds SD + CAN + serial only:

```ini
MASTER_GPS_ENABLED=0
MASTER_RPM_ENABLED=0
```

Optional test targets:

```bash
pio run -e MasterStableGPS
pio run -e MasterStableRPM
pio run -e MasterStableSensors
```

Feature flags:

- `MASTER_GPS_ENABLED=1` compiles and starts GPS.
- `MASTER_RPM_ENABLED=1` compiles and starts RPM sampling.
- `MasterStableSensors` enables both GPS and RPM.

## Stable Test Gate

Before adding a feature back, this baseline should pass:

- Boot and mount SD card repeatedly.
- Start logging automatically.
- Create a valid `/sdcard/log_XXXX.bin`.
- Stop/start from serial without corrupting the file.
- Decode the file with `tools/decode_log.py`.
- Run on CAN for at least 30 minutes without reset.

After the baseline passes, repeat the same gate with `MasterStableGPS`, then
`MasterStableRPM`, then `MasterStableSensors`.
