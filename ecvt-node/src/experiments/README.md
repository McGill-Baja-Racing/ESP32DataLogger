# Manual operations

`motor_tests.cpp` executes one requested absolute position move with the configured
profile, waits for fresh target completion, and supports cancellation. The motor
owner disables on completion, error or cancellation.

`brake_probe.cpp` runs the ten-second zero-velocity state sequence and the shared
disable/confirmation operation. The old fixed position presets and automatic
experiments are removed; Git history retains them.
