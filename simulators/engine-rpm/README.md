# Engine RPM simulator

Generates one positive pulse per revolution on GPIO8 for connection to GPIO3
of the Engine Sensor Node. Connect the two ESP32 grounds together.

The simulator starts at 1000 RPM and accepts:

- `rpm <0-4000>` — set a fixed speed
- `stop` — stop the output
- `sweep` — run 0 → 4000 → 0 RPM in 250 RPM steps
- `status` — show the configured speed, period, and output pin
- `help` — show commands

The output is a 200 µs high pulse. Do not connect either GPIO to voltage above
3.3 V.
