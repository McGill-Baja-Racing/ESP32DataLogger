# First bench validation

This procedure validates the current standalone terminal firmware. It does not validate DAQ integration, the sensor-loss failsafe, or mechanical brake locking. Start with manual position operation and advance only after each observation matches the expected result.

## 1. Build the correct project

Open `ecvt-node/` as the PlatformIO project, environment `esp32-p4`. From the repository root:

```sh
pio run -d ecvt-node -e esp32-p4
```

Expected: `SUCCESS`. Record the build result. If `pio` is unavailable in your normal shell, use the PlatformIO terminal in VS Code. Do not flash another project by mistake.

For the reported `esp32p4-eco7-20260109` board, the `esp32-p4` environment must select `board = esp32-p4_r3`, with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` disabled. A pre-v3 build produced the repeated ROM-only `CHIP_LP_WDT_RESET` startup reported on 2026-11-19. After taking this fix, clean the project and use a full PlatformIO upload so both bootloader and app are replaced. The previous bootloader entry was `0x4ff29ed0`; the corrected locally verified bootloader entry is `0x4ffac2c0`. Hardware boot confirmation is still required. See [Espressif's revision compatibility guidance](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.html).

## 2. Check the existing bench setup with power removed

Use the previously working servo bench setup, with the motor secured and preferably uncoupled from the eCVT mechanism for initial validation. Keep an independent motor-power cutoff accessible; terminal stop cannot stop a motor through a failed CAN link.

Check against actual board wiring:

- MCU CAN TX GPIO 5 -> transceiver TXD; transceiver RXD -> MCU GPIO 4. MCU GPIOs are not CAN-H/CAN-L.
- Existing servo CAN bus: 500 kbit/s, servo CANopen node ID 1, correct termination and signal reference according to the transceiver/drive wiring.
- Keep the current DAQ 1 Mbit/s bus disconnected from this legacy single-channel firmware.
- Verify motor power, encoder and brake connections using the drive documentation. A stopped servo holding with torque does not demonstrate mechanical brake engagement.
- If local recording is wanted, insert the SD card before boot. SDMMC pins in `src/storage/sd_card.cpp` must match this particular board. Missing SD does not prevent terminal operation.

## 3. Upload with motor power off

Power the P4 over USB with drive/motor power off. In PlatformIO select Upload, or run:

```sh
pio run -d ecvt-node -e esp32-p4 -t upload
pio device monitor -d ecvt-node -e esp32-p4
```

If needed, list ports with `pio device list` and specify `--upload-port PORT` for upload and `--port PORT` for monitor. Monitor baud is 115200; enter commands followed by Enter. Opening the monitor may reset the P4, so establish the monitor before powering the drive.

Expected: an idle startup message and command help, without CAN servo setup or movement commands. An SD mount error is acceptable for a terminal-only test; record it if logging is intended.

## 4. Check idle and non-motion commands

Enter one line at a time, waiting for each response:

```text
help
status
mode manual
test list
status
```

Expected: commands recognized; initial manual/idle state; `busy=0` after handling the commands. `brake=UNVERIFIED` is expected. Feedback age `-1` or initially zero position before servo setup means no measurement has been received, not a verified physical zero. No movement should occur.

With the monitor open and firmware idle, power the drive and observe again without issuing motion. Any unexpected movement fails this checkpoint. Boot idle describes what the P4 commands, not a guarantee about a drive left running by another controller.

To test communication before movement, enter `servo ping`. Expected: `Servo replied: node=1 object=0x6061:00 mode=N`, with N reporting the current mode. Repeat it to confirm replies remain available. This sends only an SDO read; it does not prepare/enable the servo or send movement commands. A failure needs investigation of drive power, node ID, bitrate, CAN wiring/transceiver and drive communication state. `help` tests the P4 terminal; `servo ping` tests the P4-to-drive CAN round trip. Keep the DAQ bus disconnected. Once diagnostic reads work, test `stop` and `servo cw 0x0000` separately while the drive is already disabled. Each should send 0000 and confirm the disabled state. A missing reply must report unconfirmed disable, not idle/no command.

## 5. Establish position, units and the allowed test envelope

Before the first `move`, run `servo inspect` to read user actual position `6064h`, encoder resolution, electronic gearing and position factors. Keep the drive stationary. `servo ping` reads operating mode only, while `move` performs setup and movement together. The separately approved unloaded `test run three-revolutions` preset reads and checks its configured conversion ratio and measured endpoints before enabling; see [terminal instructions](TERMINAL.md#approved-three-revolution-bench-test).

The code assumes 10000 counts per motor revolution. With that drive scaling confirmed, at a fresh P4 boot its initial logical position is `6064h counts / 10000`; the software zero offset initially is zero. No mechanical homing is performed. Check which shaft you observe: the BOM's 50:1 gearbox changes output motion relative to motor motion, and the terminal adds no gearbox conversion.

Choose minimum/maximum logical positions, low speed, small displacement, and acceleration/deceleration for the actual fixture. Limits must include the current position as well as both requested endpoints. Enter actual numeric values, not the placeholders below:

```text
limits MIN_REV MAX_REV MAX_RPM
profile ACCEL_RPM_PER_S DECEL_RPM_PER_S
```

There are no universal safe values for an attached mechanism. If current position/gearing/travel is unknown, stop here and establish it before sending a target. Bounds are a software command envelope, not hardware limit switches or guaranteed stopping distance. They reset on reboot.

## 6. Make and measure one small move

Calculate `target = current logical position + chosen small displacement`, then enter the computed numeric target and chosen speed:

```text
move TARGET_REV LOW_RPM
status
```

Observe the complete setup, enable and motion sequence. Expected: a small move in the intended direction, followed by `Drive disable confirmed` and `busy=0`. Confirm physical stopping and brake action separately. Once configured, repeated `status` should show recent feedback (normally below 200 ms), zero raw velocity at rest and no drive-fault bit (statusword bit 3). The firmware reads back position mode itself.

Compare final `position_counts / 10000` with the expected position and physical movement. `position_counts` is drive user-position feedback; after an automatic-state zero recapture, logical position additionally has a software offset. Keep automatic mode out of this first test. Returning to the starting position requires another explicit absolute target; it is not automatically zero.

Unexpected displacement/direction, drive faults, setup errors, or `STOP UNCONFIRMED` fail this checkpoint. Preserve the console output. Do not widen limits merely to bypass a rejection.

## 7. Test cancellation and mode ownership

Choose another fully permitted target and speed giving enough time to type a command while the motor moves. Enter `move TARGET_REV LOW_RPM`, then `stop` on the next line. Expected: an explicit 6040=0000 write followed by Switch on disabled confirmation, with no return to zero or automatic restart. Observe the physical stop and brake sequence; the statusword alone does not establish them.

During an active move, `mode auto` should be rejected as busy; it must not queue an automatic start. After stopping, `mode auto` and `mode manual` should only select modes and must not move the motor. Do not enter `controller start` yet.

Repeat a small manual move to verify an explicit new command is required and accepted. If stop is unconfirmed, record the feedback ages, velocity, statusword, error and physical behavior; use the independent cutoff if motion continues. Retrying `stop` is supported, but reset/power-cycle is not proof that stopping worked. Do not intentionally create a CAN failure during this first moving test.

## 8. Validate two separate recordings

With SD available, enter `log start`. Wait for the asynchronous `Recording started` and successful start-result messages. Make a previously validated small move, then enter `log stop`; wait for `Recording closed`. Repeat the sequence and check that a new file is created without rebooting. `status` reports cumulative `log_dropped_records`; zero is the target for these small tests. File opening failure, short-write reports, or losses need investigation.

After closure and powering down the setup, copy the files from SD. From the repo root:

```sh
python3 ecvt-node/tools/decode_log.py /path/to/log_0001.bin /path/to/log_0001.csv
```

Check that both recordings decode, contain controller-command and servo position/velocity/current records, and show the movement actually observed. The legacy CSV label `command_speed_rev_s` currently contains the rpm-equivalent command value; feedback velocity is still in raw drive units. Do not compare those two columns directly. This format is not yet the full command/input provenance or master telemetry format planned for DAQ integration.

## 9. Verify the brake separately, before claiming lock

Use the DSY-C manual's BK output configuration and timing diagrams with the actual brake wiring. Confirm the assigned output (`FunOUT.11: BK`), output logic, brake supply and mechanical engage/release delays. The current terminal stop sends 6040=0000 and confirms the disabled drive state. It does not command a separate mechanical lock or prove BK engagement.

Do not infer brake engagement from zero speed, a stationary shaft, or holding torque while the servo is enabled. Do not force a shaft or deliberately remove supporting torque from a loaded mechanism to test it. Establish the supported drive-managed stop/lock/restart sequence on an unloaded fixture; use its verified configuration to implement and validate brake integration next. The brake test now uses ordinary enable/disable, not forced output bits.

Sources: [DSY-C holding brake description and wiring](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=47), [BK output configuration](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=48).

## Later checkpoints

Only after small moves and stop behavior pass, review the larger preset targets with the fixture owner: position-step is 100 rev at 1000 rpm; position-profile is 75 rev at 2000 rpm; step-response is +10/-10 rev at 2000 rpm. Each requires appropriate limits. Do not use these as initial wiring/scaling tests.

Automatic control comes after an explicit review of the retained ADC/CSV input path, thresholds and zero recapture. `controller start` currently starts that legacy bench path, not the future DAQ sensors. The new sensor-loss policy and separate DAQ CAN interface remain outstanding.

For each checkpoint record: command, limits/profile, observed motion, before/after status, pass/fail, and the console/log file. The first handoff is idle startup output plus confirmed drive position/gearing and proposed small-move limits; those establish the initial movement commands without guessing.
