# SKF wheel-bearing RPM simulator

Generates the two-channel quadrature waveform of the SKF
`BMB-6202/032S2/UB108A` bearing: 32 pulses per revolution and 128 decoded
quadrature transitions per revolution.

Connect simulator GPIO6 → Encoder Node GPIO6, GPIO7 → GPIO7, and connect the
ESP32 grounds. The simulator drives 3.3 V push-pull outputs; do not add any
5 V pull-up.

It starts at +500 RPM and accepts:

- `rpm <value>` — signed fixed speed from -3000 to +3000 RPM
- `stop` — stop and cancel a sweep
- `sweep <start> <end> <step> <dwell_ms>` — run a signed-speed sweep
- `cancel` — cancel a sweep while retaining the current RPM
- `status` — show RPM, direction, pins, and sweep state
- `help` — show commands
