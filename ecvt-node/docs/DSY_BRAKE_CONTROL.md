# DSY-C holding brake findings

Historical design/investigation record imported from ESP32_ECVT. For the
current standalone-first scope, see [Development](DEVELOPMENT.md); for executable
behavior, see [Terminal](../TERMINAL.md). Later bench results are in the
[November 25 report](bench/2026-11-25/README.md).

Source: STEPPERONLINE DSY-C Series User Manual V1.0, vendor-authored manual reproduced on ManualsLib. These findings describe the documented interface, not the actual wiring or configured parameters on the team's drive.

- [PDF page 18, printed page 15](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=18): drive connection table includes BRK+ / BRK- brake output.
- [PDF page 47, printed page 44](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=47): the motor brake is a holding mechanism for stopped motion; brake release is energized. It describes separate 24 V brake power and a drive BK signal.
- [PDF page 48, printed page 45](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=48): configure the second digital output as `FunOUT.11: BK`, with appropriate output logic. Inactive BK disconnects brake power and locks; active BK powers/releases the brake.
- [PDF page 53, printed page 50](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=53): brake-related parameters P00.16/P00.17 have no effect unless function BK is assigned. Mechanical action delays must come from motor specifications.

Separate brake power/wiring does not imply the ESP32 must directly switch it. The drive has a brake output and sequencing associated with drive state. The intended software path is ESP32 CANopen control -> servo drive -> BK brake power control. Normal servo enable/disable is already controlled over CAN in the source.

This confirms a drive-managed brake interface. It does not yet establish a dedicated CAN object for independent forced brake engage/release while leaving servo state unchanged. Do not invent such an object or assume a Halt command applies the brake. Verify the drive's configured BK function, stop option and timing; hardware-test standstill, brake operation and restart sequencing before declaring lock successful. Do not add ESP32 brake GPIO hardware on the assumption that separate brake wiring requires it.

User-approved policy remains controlled stop then lock, with explicit restart and no automatic return-to-zero. Torque cancellation and fault/sensor-loss behavior still require their own definition. Drive-managed brake timing is the proposed implementation mechanism; no brake actuation code has been added during this documentation check.

## Independent brake test investigation, 2026-11-19

The user confirmed `servo ping` succeeds with mode 1 and the motor is uncoupled/unloaded. They request brake release and engagement without a motion command.

The vendor-authored DSY-C manual lists candidate forced-output functions:

- [Pages 142–143](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=143): P11.10 enables forced DI/DO control (2 means forced DO), and P11.12 sets the forced DO output mask (range 0–0x001F).
- [Page 134](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=134): P02.22 selects the DO2 brake function and P02.32 controls output logic inversion.

These parameter numbers are not verified CANopen index/subindex addresses. The DO2 bit in the forced-output mask, interaction with BK sequencing, and availability of these parameters through CANopen remain unverified. Do not infer the CAN addresses from P-group numbers or toggle output polarity to simulate a brake command. Forced DO control may affect other outputs as well; any implementation must preserve/reconcile their state and restore normal drive control afterward.

The product page provides `DSY-C.EDS`, the vendor object dictionary. Its download returned HTTP 403 to the execution environment; the user subsequently supplied `/Users/remylaurendeau/Downloads/DSY-C.EDS`, which was inspected as data.

### Supplied EDS findings and selected test behavior

The user explicitly selected **brake release with motor torque off**, rather than repeating the old drive enable/disable cycle. The old `elec_motor_enable_operation_sequence()` sends controlword 0x0006, 0x0007, then 0x000F: this enables operation and therefore does not establish torque-off brake release.

The supplied EDS declares these PDO-mappable, read/write UNSIGNED32 entries:

| Object | EDS name |
| --- | --- |
| 0x60FE:01 | Digital outputs / Physical |
| 0x60FE:02 | Digital outputs / Bit mask |

It provides no bit descriptions, brake-specific object, or mapping of P11.10/P11.12 to CANopen addresses; `ManufacturerObjects` lists zero supported objects. EDS default values are not readings of the connected drive's current configuration.

Other manufacturers implement manual brake control through bit 0 of 0x60FE, but their implementation does not verify DSY-C support, polarity, or interaction with the drive's BK sequencing. Do not turn that inference into a DSY brake-release command.

The next evidence needed is a DSY-200C vendor-confirmed procedure for independent release while operation is disabled: the object/subindex and bit definition, any required output mask/configuration, and the procedure to reapply the brake and restore automatic BK control. Specifically ask whether 0x60FE:01/:02 bit 0 implements this function on the installed firmware. No brake override commands have been added and no hardware commands were sent.

The user subsequently authorized a bench command test. `servo inspect` now provides the first diagnostic step: read 0x6041:00, 0x606C:00, 0x60FE:01 and 0x60FE:02 without writing controlwords or outputs. It reports raw output/mask values without interpreting undocumented brake bits. Save the full result before selecting a write experiment; accepting SDO reads alone does not establish that a brake override is supported. Firmware build and six host tests passed; physical testing remains with the user.

### Observed baseline and experimental probe

The user supplied hardware read results: statusword 0x0250 (Switch on disabled), velocity 0, 0x60FE:01 = 0 and 0x60FE:02 = 0. No fault or operation-enabled flag was set. This confirms reads are accepted, not the semantics of writes.

`servo brake-test` now implements a restricted experiment of the bit-0 interpretation documented in the [item CANopen manual](https://cdn.item24.com/product-assets/DOK_BEDA_CANopen_%23SEN_%23AIN_%23V1.pdf): physical bit 0 = 1 requests apply, 0 requests release, and mask bit 0 permits control. This remains an inference for DSY-C and is explicitly labeled experimental. It only runs in idle manual mode before any motion this boot, on fresh disabled/zero-speed readings and the exact 0/0 output baseline. Every write is acknowledged and read back. Candidate release lasts nominally two seconds with state/velocity checks, then restores the original registers. Cleanup is attempted on cancellation/errors too. Failed restoration latches a motion/settings block and exposes `servo brake-restore` for retry. No NMT, enable, target, or persistent-store commands are sent. See [terminal instructions](../TERMINAL.md#experimental-unloaded-brake-probe).

Seven host tests cover the real probe's disabled-state checks, baseline rejection, cancellation, uncertain accepted writes, failed restoration and retry, alongside the existing migration/transport/parser checks. Firmware builds successfully. Actual brake release/engagement and the candidate bit mapping remain unverified until the user's physical test.

### First physical probe result

The user ran the experiment: candidate writes/readbacks and restoration to 0/0 completed, but the probe returned ESP_ERR_INVALID_STATE, both before the override on some attempts and after entering the release window on others. They report no audible click or felt release. This does not validate bit 0 as the DSY brake control. The initial implementation omitted the failing guard's values, so the exact cause cannot be inferred from that log. Diagnostics now print phase, statusword, signed velocity, baseline values and write/readback errors without relaxing any guard. Serial input also gained buffered UART reception and received-line/invalid-byte reporting to investigate intermittent parse failures. No further physical command was sent by the agent.

Follow-up diagnostics identified a leading 0xFF byte before an otherwise correct `servo inspect` command, and exact-zero velocity failures at raw values +1 and -4, including a release-window abort. The source of the stray byte and physical velocity units/noise are not established; no input byte is silently discarded and no velocity threshold was relaxed. The user switched focus to a motion test and explicitly approved +3 motor-shaft revolutions from the current position, at 60 rpm in the drive-positive direction, still uncoupled/unloaded. The new preset uses normal enable/brake sequencing and does not rely on the unverified output bit. See terminal instructions for scaling checks, boundaries and stop behavior. Actual motion remains to be tested by the user.

## Live commissioning update, 2026-11-20

See the [bench report and raw traces](bench/2026-11-20/README.md). Agent-run +3-revolution tests did not complete. Restoring the old one-second active-enable wait still produced 0xFF2F and three red flashes; a hold-only test subsequently produced 0xFF2B and solid red. The user heard a click and loud/stuck behavior. Holding-brake coil voltage was not measured: the attempted measurement was at the distinct braking resistor, and the actual brake connection was inaccessible. A post-fault target/demand discrepancy also remains unresolved. Actuation was stopped, the serial port released, and a tested demand-mismatch guard flashed without further physical enable tests. No conclusion that the holding brake is the sole cause is established.
