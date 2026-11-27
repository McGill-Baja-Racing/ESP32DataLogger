# Bare-core bench validation

This revision has not been flashed. Earlier successful unloaded-motor checks are
preserved in [the November 25 report](docs/bench/2026-11-25/README.md). Repeat the
basic checks after this source cleanup before developing the interface.

1. Build/upload `ecvt-node`, environment `esp32-p4`, with servo power off.
   Confirm the selected project is in the DAQ checkout on
   `BajaRemy/ecvt-standalone`. The isolated servo bus remains TX5/RX4,
   500 kbit/s, drive node 1. Keep the vehicle DAQ bus disconnected.
2. Open the 115200-baud monitor. Confirm idle startup and command help. No SD,
   sensor tasks, automatic controller or motion should start. Save the boot log.
3. Power the drive. Run `servo ping`, `servo inspect`, `servo errors`. Save all
   output. Check actual gearing/position and error-free disabled state.
4. Run `stop`, then `servo cw 0x0000` separately while disabled. Each must attempt
   0000 and confirm disabled; a missing reply must report unconfirmed disable.
5. From a fresh idle boot, run `servo brake-test`. Observe release during the
   ten-second enabled window and engagement after 0000. Save its entire output.
   Report unexpected motion, vibration or heat; firmware status alone cannot
   establish physical brake behavior. The brake test enables motor torque.
6. Choose small absolute targets, low RPM, profile and bounds appropriate for
   the actual fixture. Use [the terminal guide](TERMINAL.md) for command syntax
   and the 10000-count motor-revolution scaling. Run one move to completion.
   Compare actual final position and observed motion with the requested target.
7. Start a second permitted small move, then enter `stop` during motion. Confirm
   a 0000 write and disabled status, with no resumed movement. Retry with
   `servo cw 0x0000` as the stop command. Observe physical stopping/brake action.
8. Finish with `servo inspect`, `servo errors`, `status`. Return the complete
   terminal transcript, physical observations and whether each step passed.

No tuning changes are required for these checks. Export the current working
configuration before future tuning; preserve separate bare-motor and loaded
profiles. Larger scripts, network control, tuning UI and DAQ connection are later
stages in [the development sequence](docs/DEVELOPMENT.md).
