# Hardware reference

This reference describes wiring and assumptions encoded in the current firmware. Choose the relevant component introduction from the [documentation index](README.md) first. It is not a record of successful vehicle commissioning: verify the actual boards, harness, sensor models, and calibration before use.

## Boards and shared communication

Sensor firmware targets ESP32-C3 devkit boards; master firmware targets the Waveshare ESP32-P4-WIFI6 arrangement with an ESP32-C6 Wi-Fi coprocessor. The C6 must run ESP-Hosted slave firmware compatible with the master's locked dependencies.

| Connection | Master ESP32-P4 | Sensor ESP32-C3 |
|---|---|---|
| CAN controller TX | GPIO20 | GPIO21 |
| CAN controller RX | GPIO21 | GPIO20 |

These GPIOs connect to a suitable CAN transceiver, not directly to CAN-H/CAN-L. Use common signal ground, matched 1 Mbit/s settings, and proper bus termination at both ends (normally 120 Ω per end). Check transceiver supply and logic compatibility against its datasheet. The firmware does not specify the team's exact transceiver or harness assembly.

## Sensor inputs and configured assumptions

| Measurement | Connection | Current conversion/setup |
|---|---|---|
| Front pressure | GPIO0 | 0.5–4.5 V sensor, 3000 psi span, divider scale 0.5 |
| Rear pressure | GPIO1 | 0.5–4.5 V sensor, 1600 psi span, divider scale 0.5 |
| Generic ADC | GPIO1 | Voltage in mV; calibrated ADC when available, approximate fallback otherwise |
| Engine spark | GPIO3 | Conditioned digital rising edges, one revolution per spark, accepted 1000–6000 RPM intervals, 100 ms stopped timeout |
| Bearing A/B | GPIO6/GPIO7 | SKF BMB-6202/032S2/UB108A, 32 pulses/revolution, four-edge decoding (128 counts/revolution) |
| CVT thermistor / thermopile | GPIO3/GPIO2 | 100 kΩ NTC (β 3960) under a 3.3 V divider with 100 kΩ fixed; ZTP-148SR through INA126 (gain 247) |
| MPU SDA/SCL | GPIO4/GPIO5 | I2C acceleration/gyro; addresses 0x68 or 0x69, ±8 g and ±2000 degrees/second |

The [front](../sensor-node/src/sensors/front_brake.c) and [rear](../sensor-node/src/sensors/rear_brake.c) drivers define these pins and divider scales. Inspect the fitted harness and divider before flashing or rewiring. These are code-derived assumptions, not measured calibration results.

ESP32 GPIO inputs must stay within 3.3 V logic limits. The pressure sensor's higher voltage needs the documented divider; do not attach its full output directly. The engine input needs appropriate ignition signal conditioning; never connect raw ignition voltage to GPIO3. Match conversion constants to the installed sensors.

This bearing is installed inside the gearbox. Its raw rotation measurement is converted later to wheel and secondary RPM; the firmware does not apply drivetrain ratios.

The bearing has NPN open-collector outputs: use its regulated 5 V supply, common ground, and separate external pull-ups from A/B to **3.3 V**. Use **4.7 kΩ** for each external pull-up; this is the recommended value for the team’s bearing interface. Internal pull-ups are a bench fallback; vehicle wiring needs suitable filtering and transient protection. Direction depends on channel order and the configured sign. Its RPM average spans nominally 100 ms, with a 100 ms no-edge timeout.

The current motion-sensor hardware target is the **MPU-6500**. For this breakout, use 3.3 V power and ground, SDA4/SCL5, AD0 at ground or 3.3 V to choose the address, and CS/NCS high to disable SPI. INT/FSYNC are unused. The [driver](../sensor-node/src/sensors/mpu6500.c) reads acceleration and angular velocity only; it does not implement magnetometer readings. The MPU-6500 provides acceleration and angular velocity, with no magnetometer. A change to another sensor model must include the corresponding code and documentation updates. Use external 2.2–4.7 kΩ I2C pull-ups to 3.3 V unless already fitted.

## Master GPS and SD

The GY-GPS6MV2 GPS uses UART1 at 9600 baud: receiver TX → master GPIO33, receiver RX → master GPIO32, common ground. Verify power requirements for your specific receiver breakout. Satellite reception is needed for a valid position fix.

The master uses four-bit SDMMC: CLK43, CMD44, D0=39, D1=40, D2=41, D3=42. Storage mounts at `/sdcard` as FAT. `SD_FORMAT_IF_MOUNT_FAILED=0` is the current default; a failed mount does not automatically erase the card. Setting it to `1` permits formatting on mount failure and can destroy existing data.

## Bench connections

Use a separate simulator board and sensor board. Engine simulator GPIO8 connects to engine GPIO3. Wheel simulator GPIO6/7 connects to encoder GPIO6/7. Share grounds. Simulator outputs are 3.3 V push-pull; do not add 5 V pull-ups. See [simulator instructions](../simulators/README.md).

Next: [Flashing](FLASHING.md), [Testing](TESTING.md), or [Troubleshooting](TROUBLESHOOTING.md).
