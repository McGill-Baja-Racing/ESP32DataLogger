# Master node

The ESP32-P4 master collects vehicle samples and saves recording sessions. Read the [system overview](../docs/SYSTEM_OVERVIEW.md) first if CAN, firmware, or timestamps are new to you.

## Responsibilities

The master receives sensor readings over CAN and GPS messages over a serial connection. It broadcasts recording commands and clock beacons, monitors configured nodes, buffers samples to SD, and hosts the browser interface through the board's ESP32-C6 Wi-Fi coprocessor.

After initialization it attempts to start recording after five seconds. An unnamed start waits up to three additional seconds for GPS UTC before selecting a filename; without it, a numbered filename is used. Existing recordings are not overwritten. Serial `start`, `stop`, and `status` commands and browser controls provide manual operation.

Connect to the open `BajaDAQ` network and open `http://192.168.4.1` for controls, session naming, completed-log rename/download, and up to four live graphs. Live viewing is enabled separately while recording and ends on stop or viewer lease expiry. SD collection continues independently of viewing.

## Build profiles

| Environment | Use |
|---|---|
| `MasterStable` | Normal master firmware with CAN |
| `Master` | Same current configuration as `MasterStable`; base environment |
| `MasterNoCAN` | GPS/SD/web operation without initializing CAN or monitoring remote nodes |

With PlatformIO installed, from the repository root and without a board:

```bash
pio run -d master-node -e MasterStable -t buildprog
```

Expect `SUCCESS`. Initial downloads can be substantial. For uploads and expected observations, use [Flashing](../docs/FLASHING.md).

## Find your work area

| Task | Start here |
|---|---|
| Recording and SD buffering | [Logger guide](src/logger/README.md) |
| Browser controls, graphs, exports | [Web guide](src/web/README.md) |
| GPS, time, CAN, node status, lifecycle | [Source map](src/README.md) and [architecture](docs/MASTER_ARCHITECTURE.md) |
| Interpret recorded data | [Log format](../docs/LOG_FORMAT.md) |

Wi-Fi startup failure is reported on serial while collection can continue. SD mount failure prevents normal startup; automatic formatting is disabled in the current build configuration. See [Troubleshooting](../docs/TROUBLESHOOTING.md).

Next: [Source map](src/README.md). Shared references: [protocol](../docs/PROTOCOL.md), [hardware](../docs/HARDWARE.md), [testing](../docs/TESTING.md), and [contribution workflow](../CONTRIBUTING.md).
