# Vehicle firmware flashing

Use the following PlatformIO environments. All vehicle sensor profiles use
CAN at 1 Mbit/s and `SENSOR_SERIAL_TEST=0`.

| Board | Project | Environment | Sensor inputs |
| --- | --- | --- | --- |
| Master ESP32-P4 | Master_Node | MasterStable | GPS UART1: RX33, TX32, 9600 baud |
| Brake ESP32-C3, node 1 | Sensor_Node | NodeBrake | Front GPIO1, rear GPIO2 |
| MPU ESP32-C3, node 3 | Sensor_Node | NodeMPU | I2C SDA4, SCL5; address 0x68 or 0x69 |
| Wheel ESP32-C3, node 4 | Sensor_Node | NodeEncoder | Encoder A GPIO6, B GPIO7 |
| Engine ESP32-C3, node 5 | Sensor_Node | NodeEngine | Rising-edge pulse input GPIO3 |
| Optional ADC ESP32-C3, node 6 | Sensor_Node | NodeADC | ADC GPIO1 |

CAN transceiver pins: master TX20/RX21; all sensor nodes TX21/RX20.
`NodeEngineBench` disables CAN. `MasterNoCAN` collects only local GPS.
GPS is attached to the master and has no separate firmware profile here.

From the relevant project directory, select the environment in PlatformIO
and upload, or run `pio run -e ENVIRONMENT -t upload --upload-port PORT`.
Select the port belonging to the board being flashed. Build without uploading
with `pio run -e ENVIRONMENT -t buildprog`.

The engine calculation assumes one revolution per pulse and accepts
1000–6000 rpm. The wheel encoder uses 32 pulses/revolution with four-edge
quadrature decoding. Brake scaling assumes 0.5–4.5 V sensors, the existing
2.33/4.33 divider, and spans of 3000 psi front / 1600 psi rear.
These settings must match the fitted sensors and wiring.

After all boards are powered, the master starts its recording flow after five
seconds, waiting at most three more seconds for GPS time before using a numbered filename.
Connect to `BajaDAQ` and open `http://192.168.4.1` to confirm the active
date/time filename. Check that nodes 1, 3, 4, and 5 become active, and that CAN/log drop counts
stay at zero. Open live data and exercise each sensor; check both brake
channels, all MPU axes, wheel direction, and engine RPM. GPS samples appear
when checksum-valid active RMC position fixes arrive. Stop recording before
removing power, wait for the logger to become idle, and verify the downloaded
CSV contains the expected channels. A successful build does not verify the
physical bus, sensor calibration, or GPS reception.
