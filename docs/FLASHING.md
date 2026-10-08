# Build and flash a board

Use this procedure after [Getting started](GETTING_STARTED.md). Uploading requires a powered board, a USB data cable, PlatformIO, and an identified serial port. Read [Hardware](HARDWARE.md) before connecting sensors. Return to the [documentation index](README.md).

## Select the firmware

| Board / role | Project | Environment |
|---|---|---|
| Master ESP32-P4, vehicle CAN | `master-node` | `MasterStable` |
| Master without CAN, GPS bench | `master-node` | `MasterNoCAN` |
| Brake ESP32-C3, node 1 | `sensor-node` | `NodeBrake` |
| MPU ESP32-C3, node 3 | `sensor-node` | `NodeMPU` |
| Bearing ESP32-C3, node 4 | `sensor-node` | `NodeEncoder` |
| Engine ESP32-C3, node 5 | `sensor-node` | `NodeEngine` |
| Optional ADC ESP32-C3, node 6 | `sensor-node` | `NodeADC` |
| Engine serial bench | `sensor-node` | `NodeEngineBench` |
| Engine signal generator | `simulators/engine-rpm` | `EngineRPMSimulator` |
| Wheel signal generator | `simulators/wheel-rpm` | `WheelRPM_SKF_Simulator` |

Vehicle sensor profiles have serial-test mode disabled. GPS is attached to the master; there is no separate GPS firmware project.

## Build first, without uploading

From the repository root:

```bash
pio run -d sensor-node -e NodeBrake -t buildprog
```

Expect `SUCCESS`. Change project and environment together using the table. Explicit `buildprog` prevents the simulators' default upload/monitor targets from running.

## Identify the port and upload

Connect the board with a USB data cable. From the repository root:

```bash
pio device list
```

Compare before/after connecting if several ports appear. Close other monitors using that port. Replace `PORT` below with the actual device path or COM port:

```bash
pio run -d sensor-node -e NodeBrake -t upload --upload-port PORT
pio device monitor --port PORT -b 115200
```

Expect a successful upload, reset, and startup messages. Sensor nodes pause three seconds before initialization. In VS Code the equivalent is the selected environment's Upload task followed by Monitor. Do not infer board role from the USB port name alone.

## Confirm physical operation

Check serial logs for the expected sensors and role. For the full vehicle setup, power the master with an SD card and the correctly configured nodes. The master attempts auto-recording after five seconds; unnamed starts may wait three more seconds for GPS UTC.

Connect to `BajaDAQ`, open `http://192.168.4.1`, and check recording/node status. Exercise the sensors, inspect live values, stop, wait for idle, and download a full CSV. Detailed acceptance criteria and sustained-run checks are in [Testing](TESTING.md). Calibration and pin assumptions are in [Hardware](HARDWARE.md).

A successful upload establishes that firmware reached a board, not that the physical bus or sensor readings are correct. If expected startup or measurements are missing, use [Troubleshooting](TROUBLESHOOTING.md).
