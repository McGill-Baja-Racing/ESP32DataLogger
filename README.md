# Baja DAQ

Firmware for the McGill Baja data acquisition system (DAQ 3.0). ESP32-C3
**sensor nodes** read sensors and send timestamped samples over a 1 Mbit/s CAN
bus. The ESP32-P4 **master node** keeps their clocks in sync, logs every sample
to an SD card, and serves controls, live graphs and log downloads over its
`BajaDAQ` Wi-Fi network.

## What is in this repository

| Folder | Contents |
|---|---|
| [`master-node/`](master-node/) | ESP32-P4 master firmware |
| [`sensor-node/`](sensor-node/) | ESP32-C3 sensor node firmware, one environment per node |
| [`simulators/`](simulators/) | Bench tools that generate engine and wheel RPM signals |

Each folder is a separate PlatformIO project. The V1 and V2 firmware is kept in
the [`archive/v1-v2`](https://github.com/McGill-Baja-Racing/ESP32DataLogger/tree/archive/v1-v2)
branch.

## Set up your computer (once)

1. Install [VS Code](https://code.visualstudio.com/) and its **PlatformIO IDE**
   extension.
2. Clone this repository.
3. In VS Code, choose **File > Open Workspace from File...** and open
   `baja-daq.code-workspace`. Every project appears in the same window.

The first build downloads the ESP32 toolchain and takes several minutes.

## Flash a board

1. Connect the board over USB.
2. In the status bar at the bottom of VS Code, click the PlatformIO environment
   and pick the one for your board. The [flashing guide](master-node/docs/FLASHING.md)
   lists the environment for each board.
3. Click the **→** (Upload and Monitor) button in the status bar.

The same thing from a terminal, at the repository root:

```bash
pio run -d sensor-node -e NodeBrake -t upload -t monitor
```

## Documentation

- [Flashing guide](master-node/docs/FLASHING.md): environment for each board and
  the checks to run on the vehicle
- Master node: [overview](master-node/README.md),
  [architecture](master-node/docs/MASTER_ARCHITECTURE.md),
  [GPS absolute time](master-node/docs/ABSOLUTE_TIME.md)
- Sensor node: [overview](sensor-node/README.md),
  [architecture](sensor-node/docs/NODE_ARCHITECTURE.md),
  [adding sensors and nodes](sensor-node/docs/ADDING_SENSORS_AND_NODES.md)
- [RPM simulators](simulators/README.md)
