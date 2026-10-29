# Build and flash a board

Use this procedure after [Getting started](GETTING_STARTED.md). Uploading requires a powered board, a USB data cable, PlatformIO, and an identified serial port. Read [Hardware](HARDWARE.md) before connecting sensors. Return to the [documentation index](README.md).

## Select the firmware

Each firmware project defines its available environments in its `platformio.ini`. Open the configuration for the board's role:

- [Master firmware](../master-node/platformio.ini)
- [Sensor-node firmware](../sensor-node/platformio.ini)
- [Engine signal simulator](../simulators/engine-rpm/platformio.ini)
- [Bearing signal simulator](../simulators/wheel-rpm/platformio.ini)

Look for sections named `[env:NAME]`. `NAME` is the environment selected in PlatformIO or passed to `-e` on the command line. Sections without the `env:` prefix can hold shared settings and are not selectable environments.

Read the selected section and any section named by `extends`: board settings identify the hardware target, while build flags select sensors, node identity, and operating mode. Follow inherited `${...}` settings as well. In sensor profiles, `NODE_FIXED_*_CONFIG`, `NODE_ID`, and `SENSOR_SERIAL_TEST` distinguish the intended configuration and CAN versus isolated serial testing. Ask a teammate if you cannot match the configuration to the physical board before uploading.

The configuration files are the current environment list; this guide does not duplicate that list. GPS is connected to the master and has no separate firmware project.

## Build first, without uploading

The commands below use the brake sensor as an example. From the repository root:

```bash
pio run -d sensor-node -e NodeBrake -t buildprog
```

Expect `SUCCESS`. Replace `sensor-node` and `NodeBrake` together with the project directory and environment chosen from its configuration. Use the same selection for upload below. Explicit `buildprog` prevents the simulators' default upload/monitor targets from running.

## Identify the port and upload

With the board disconnected, run this from the repository root and note the listed ports:

```bash
pio device list
```

Connect the board with a USB data cable and run `pio device list` again. The newly appearing entry is a candidate for that board. For example, Linux might add `/dev/ttyUSB0`, Windows `COM3`, or macOS `/dev/cu.usbmodem1101` (some adapters use `/dev/cu.usbserial-...`). These are examples, not fixed board addresses; use the port actually listed on your computer. If no new entry appears, check the cable and power before continuing.

Close other monitors using that port. Replace the literal word `PORT` in **both** commands below with your port, such as `/dev/ttyUSB0` or `COM3`. Do not type angle brackets around it; shells can interpret them as redirection:

```bash
pio run -d sensor-node -e NodeBrake -t upload --upload-port PORT
pio device monitor --port PORT -b 115200
```

Expect a successful upload, reset, and startup messages. Sensor nodes pause three seconds before initialization. In VS Code the equivalent is the selected environment's Upload task followed by Monitor. Do not infer board role from the USB port name alone.

## Confirm physical operation

Check serial logs for the expected sensors and role. For the full vehicle setup, power the master with an SD card and the correctly configured nodes. The master attempts auto-recording after five seconds; unnamed starts may wait three more seconds for GPS UTC.

Connect to `BajaDAQ`, open `http://192.168.4.1`, and check recording/node status. Exercise the sensors, inspect live values, stop, wait for idle, and download a full CSV. Detailed acceptance criteria and sustained-run checks are in [Testing](TESTING.md). Calibration and pin assumptions are in [Hardware](HARDWARE.md).

A successful upload establishes that firmware reached a board, not that the physical bus or sensor readings are correct. If expected startup or measurements are missing, use [Troubleshooting](TROUBLESHOOTING.md).
