# RPM sensor simulators

These ESP32-C3 projects generate electrical test signals for the DAQ sensor
nodes. They are bench tools and do not transmit CAN frames.

- `EngineRPMSimulator` generates one rising-edge pulse per engine revolution.
- `WheelRPM_SKF_Simulator` generates the two-channel quadrature waveform used
  by the SKF BMB-6202/032S2/UB108A bearing.

Each simulator accepts commands over the 115200-baud serial monitor. Open the
project directory in PlatformIO and use its named environment to build, upload,
and monitor.
