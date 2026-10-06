# Baja DAQ

This repository contains the firmware for McGill Baja Racing's **DAQ 3.0** data acquisition system. Data acquisition means measuring what the vehicle is doing and saving those measurements so the team can investigate performance and diagnose problems.

Small computers called **sensor nodes** measure brake pressure, motion, and engine or bearing speed. A **master node** collects their readings, adds GPS data, and saves recording sessions to an SD card. A phone or laptop can connect to the master's Wi-Fi network to control recording, view live graphs, and download data.

```mermaid
flowchart LR
    Sensors[Vehicle sensors] --> Nodes[Sensor nodes]
    Nodes -->|CAN: shared vehicle data connection| Master[Master node]
    GPS[GPS receiver] --> Master
    Master --> SD[SD card recordings]
    Master <-->|Wi-Fi| Browser[Browser controls and graphs]
    Simulators[Bench signal simulators] --> Nodes
```

## Start here

You do not need a board or embedded programming experience to begin.

1. Follow [Getting started](docs/GETTING_STARTED.md) to learn the vocabulary, install tools, and build firmware without hardware.
2. Read [Contributing](CONTRIBUTING.md) to choose a first task and prepare a pull request.
3. Open the component README for your task, then its source guide for implementation details.

Read the [system overview](docs/SYSTEM_OVERVIEW.md) if you want a guided explanation of how a recording works. The [documentation index and glossary](docs/README.md) help you find a specific topic.

## Repository map

| Folder | What it contains | Start reading |
|---|---|---|
| `master-node/` | ESP32-P4 firmware: recording, GPS, SD storage, browser interface | [Master introduction](master-node/README.md) |
| `sensor-node/` | ESP32-C3 firmware: sensor measurements and communication | [Sensor introduction](sensor-node/README.md) |
| `simulators/` | ESP32-C3 bench firmware generating test signals for RPM inputs | [Simulator introduction](simulators/README.md) |
| `docs/` | Shared learning guides, procedures, and specifications | [Find a guide](docs/README.md) |

Each firmware project has its own PlatformIO configuration. The workspace file [baja-daq.code-workspace](baja-daq.code-workspace) opens them together in VS Code. **Firmware** is the program that runs on a microcontroller, a small computer built into hardware.

This documentation describes DAQ 3.0. Older V1/V2 firmware is kept on the `archive/v1-v2` branch.
