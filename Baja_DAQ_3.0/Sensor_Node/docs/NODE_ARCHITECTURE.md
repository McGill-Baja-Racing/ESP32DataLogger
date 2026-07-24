# DAQ 3.0 Fixed Sensor Nodes

Each node owns its sensor configuration at compile time. The master does not
send runtime configuration.

## Builds

| Build | Node | Sensors |
|---|---:|---|
| `NodeBrake` | 1 | Front brake `0x0B1` at 100 Hz on GPIO1; rear brake `0x0B2` at 100 Hz on GPIO2 |
| `NodeEncoder` | 4 | Signed bearing RPM `0x0B9` at 50 Hz on GPIO6/GPIO7 |
| `NodeEngine` | 5 | Engine RPM `0x0BB` at 50 Hz on GPIO3 |
| `NodeADC` | 6 | Generic ADC `0x0BA` at 100 Hz on GPIO1 |

All builds use 1 Mbit/s CAN with TX GPIO21 and RX GPIO20.
Each node monitors its CAN controller and automatically initiates recovery after
a bus-off condition while preserving its current started/stopped state.

The SKF BMB-6202/032S2/UB108A bearing has two NPN open-collector quadrature
outputs. Power it from regulated 5 V with a shared ground, and pull GPIO6 and
GPIO7 up to 3.3 V through separate 4.7 kΩ resistors. Do not pull either ESP32
input above 3.3 V. The firmware's weak internal pull-ups are only a bench-test
fallback; vehicle wiring also requires suitable filtering and transient
protection.

## Source layout

`main.c` only initializes the application and connects the module callbacks.
The implementation is organized by responsibility:

| Path | Responsibility |
|---|---|
| `protocol/app_protocol.h` | Shared CAN identifiers, node states, and reason codes |
| `can/can_node.c` | TWAI setup, command dispatch, state reports, transmission, and recovery |
| `time/time_sync.c` | Master-clock offset and synchronized timestamps |
| `sampler/sampler.c` | Sensor scheduling, sample queue, and transmission task |
| `sensors/sensor_registry.c` | Sensors selected for each build profile |
| `sensors/front_brake.c` | GPIO1 front pressure conversion |
| `sensors/rear_brake.c` | GPIO2 rear pressure conversion |
| `sensors/bearing_encoder.c` | GPIO6/GPIO7 quadrature decoding and signed RPM conversion |
| `sensors/adc_input.c` | ADC one-shot and calibration shared by analog sensors |
| `sensors/generic_adc.c` | Calibrated GPIO1 voltage reported in millivolts |
| `sensors/engine_rpm.c` | Placeholder for future raw-voltage peak detection |

The bare-bones node has no periodic health frame. It retains a small state
report for boot, start, stop, and CAN recovery.

## Protocol

| CAN ID | Direction | Purpose |
|---:|---|---|
| `0x0A0` | Master to all nodes | Stop sampling |
| `0x0A1` | Master to all nodes | Start sampling |
| `0x0A2` | Master to all nodes | 64-bit microsecond time beacon |
| `0x0C0 + node ID` | Node to master | State and transition reason |

START and STOP have no payload and apply to every node.

Nodes calculate their clock offset from each time beacon and timestamp samples
on the master's clock. A sensor payload contains the 32-bit value in bytes 0-3
and the synchronized millisecond timestamp in bytes 4-7.

The state payload contains state, transition reason, and boot reset reason in
bytes 0-2.

## Build commands

```bash
pio run -e NodeBrake
pio run -e NodeEncoder
pio run -e NodeEngine
pio run -e NodeADC
```

The RPM driver measures rising edges on GPIO3 and assumes one spark per
revolution. Its input conditioning, pulse rejection window, expected RPM range,
and stopped-engine timeout require validation on the vehicle.
