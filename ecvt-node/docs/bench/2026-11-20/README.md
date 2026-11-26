# DSY-200C live commissioning, 2026-11-20

Status: **actuation stopped; commissioning incomplete**. No agent-run test achieved the requested three revolutions. The user separately reported an earlier run where the shaft did rotate. Do not repeat enable/spin attempts until the holding-brake circuit and command/demand discrepancy below have been resolved.

The user subsequently authorized an isolated comparison with original Test 2 at 1000 rpm for 3 s. It also emitted 0xFF2F; see the [comparison report, source archive and trace](original-test2/README.md). The user then identified the original state-only Test 1, which has not yet been run by the agent.

### Later manual state steps

At the user's individual requests, single acknowledged SDO writes 6040=0006 and 0007 returned statuswords 0231 and 0233. An initial 000F request was refused by the helper's then velocity-only guard. Reads before enable showed mode 1, error register 0, target/demand velocity 0, position target 607A=75, position demand 6062=0, actual position 6064=-4. The user explicitly selected preserving position mode 1 and sending only 000F; the helper was updated and host-tested accordingly, with no target/mode writes.

The subsequent single 000F was acknowledged and initially returned 0237 (enabled, no fault). The user then reported blocked behavior and solid red. Read-only follow-up returned 603F=FF2B, history FF2B, statusword 2218 (fault, operation disabled), demand 6062=75 and actual position 6064=-36. Thus a state-only enable did not guarantee a stationary brake-release test: the observed demand changed toward the already stored 75-count target despite no new target write. This remains distinct from proving why the drive could not track it. No further state/reset/target write was sent in that follow-up. See [initial steps](state-steps-0006-0007.jsonl) and [position-mode enable/fault snapshot](position-mode-000f-snapshot.jsonl).

### Zero-velocity state-step comparison

The user then selected velocity mode to remove the existing position target from this test and confirmed a physical servo power cycle, alarm off, motor unloaded/clear. The new `servo velocity-zero` command requires disabled/alarm-free state, writes 60FF=0 before 6060=3, and verifies actual mode 3, target velocity 0, demand velocity 0, status 0250, error register 0. It sends no controlword. Build and nine host tests passed, including order, refusals, cancellation, timeout, partial failures and absence of enable writes.

After the user authorized repeating the state sequence, isolated acknowledged writes 0006, 0007 and 000F returned 0231, 0233 and initially 0237. The next diagnostic check found error FF2F, register 1/history FF2F, status 1218 (fault, operation disabled), actual velocity 0 and DC-link raw 47900. Follow-up reads still showed mode 3, target velocity 0, velocity demand 0 and actual current 0 after the fault. No nonzero velocity, position or torque target was sent in this comparison. Thus retaining a position target is not the sole explanation for all observed faults. The holding-brake state and motor/drive/encoder configuration remain unresolved; no threshold/current/gain adjustment was made. See [zero-velocity state-step trace](velocity-zero-state-steps.jsonl). No further state change or reset followed these diagnostic reads.

The user authorized testing on an uncoupled/unloaded motor, limited to +3 motor-shaft revolutions at 60 rpm in the drive-positive direction. They later requested the old enable/wait/move/disable sequence and a stationary brake-voltage measurement. The agent flashed successive diagnostic builds and controlled the P4 through its serial console. Original projects were not modified in this session.

## Confirmed communication and configuration

Serial device: `/dev/cu.usbmodem5CF71544971`, 115200 baud, USB bridge VID:PID 1A86:55D3. P4 revision v3.2. CANopen drive node 1, 500 kbit/s. Multiple SDO reads succeeded. Every newly connected/restarted console in these traces had a leading 0xFF byte on the first command; the second command worked. Host TX logs contain no such byte. Its source remains unresolved; invalid lines are rejected rather than silently edited.

| Object | Observed value |
| --- | --- |
| 6061:00 actual mode | 1 |
| 608F:01 / :02 encoder resolution | 131072 / 1 |
| 6091:01 / :02 gearing | 1 / 10000 |
| 6093:01 / :02 position factor | 131072 / 10000 |
| 1400:01 RPDO1 COB-ID | 0x201 |
| 1400:02 RPDO1 type | 255 |
| 1600:00 / :01 RPDO1 mapping | 1 / 0x60400010 |
| 1401:01 / :02 RPDO2 | 0x301 / 255 |
| 607A / 6081 after an attempted move | requested target / 10000 |
| 6073 max current, raw | 3000 |
| 6075 rated current, raw | 5200 |
| 6076 rated torque, raw | 640 |
| 6065 maximal following error, raw | 0xFFFFFFFF |

These are actual reads, not EDS defaults. The driver accepted target/speed PDO writes; there is no evidence that the RPDO configuration alone caused the failed motion. The installed drive returns 1003:00 as expedited **u16**, although its supplied EDS advertises u8. The diagnostic command now tries u16, with an explicit u8 fallback; general SDO reads still enforce their requested width.

The user supplied the drive from a bench supply set to 48 V; its current limit was unknown. DC-link object 6079 usually read 47900, consistent with millivolts and that supply setting, but this is not an independent voltage measurement at the brake.

## Sequence and outcomes

| Trace | Action and outcome |
| --- | --- |
| [Session 2](session2.jsonl), [session 3](session3.jsonl) | Read-only inspection: statusword 0x1218, error 0xFF15, error register 1. User saw one red flash per cycle. |
| [Session 4](session4.jsonl) | Explicit 6040=0080 then 0000 reset changed statusword to 0x1250, but error 0xFF15/register 1 and the red LED remained. An attempted move stayed disabled; cancelled. |
| [Session 5](session5.jsonl) | Read live PDO settings; correct IDs/mapping/type. State-verified attempt stopped at 0006 because Ready to switch on was not reached. User then power-cycled the servo supply: error/register cleared and position reset to zero. |
| [Session 6](session6.jsonl) | After restarting the P4, PDO enable transitions 0231 → 0233 → 0237 were confirmed. Three-turn target 30000 was sent. Actual position plateaued around 42–43 counts. First voltage sample was 43300, later near 47900. Drive faulted; code was not captured before the next user restart. |
| [Session 7](session7.jsonl) | User explicitly requested another try after restarting. Start -3, target 29997; position plateaued near 25–26 counts, then faulted. No successful three-turn travel. |
| [Session 8](session8.jsonl) | Added old test's 1000 ms wait after entering active hold (000F). Then target 30000; position plateaued around 47 counts (about 0.0047 motor revolution). Captured fault 0xFF2F, register 1, history 0xFF2F. User saw three red flashes and heard a click during the active-hold wait. |
| [Session 9](session9.jsonl) | After user power cycle, ran a hold-only measurement. No three-turn target was sent. Two hold samples showed position 33, current raw 3001 then 707, voltage raw 48200 then 49000, outputs 0. The drive faulted with 0xFF2B and solid red. The requested ten-second measurement therefore ended early. |

The [manufacturer fault table](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=126) lists Er_21 undervoltage, Er_47 motor overload and Er_43 excessive position deviation. Interpreting the low byte of the observed 0xFFxx codes as those numbers is consistent with the LED patterns and behavior, but a full vendor-specific hexadecimal-code mapping was not found. The [LED table](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=163) groups one flash as voltage, three as overload/stall, and solid red as remaining faults.

An initial statement that the CAN reset had cleared the alarm was corrected after checking the retained code/register and physical LED. Error 0xFF15 and register 1 only cleared after a physical servo power cycle. Neither an ESP reset nor a cleared statusword fault bit alone proved recovery.

## Holding brake and demand remain unresolved

The user hears a release-like click but also loud/stuck behavior. A click is not proof that the holding brake fully releases. The user reports the drive supplies brake power and can identify its connection, but cannot access it for measurement. The proposed meter reading was actually at the braking resistor, so **no holding-brake coil voltage was measured**. The braking resistor and holding-brake coil are different circuits; see [brake wiring](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=48) and [braking resistor](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html?page=49). Disconnecting an unpowered spring-applied brake does not release it. Do not treat this as an instruction to disconnect the brake and retry motion.

After the hold-only fault, sequential reads gave **607A target = 0**, **6062 demand = 29255**, and **6065 = 0xFFFFFFFF**. These are post-fault, non-atomic observations; they do not prove exactly when the demand diverged or why. Possible retained trajectory/setpoint semantics and the meaning/scaling of the demand register require investigation. Setpoint acknowledgement alone did not establish that the controller was demanding a stationary hold.

The final source/build adds a commissioning guard: require the demand to match the requested hold target within one command count before clearing Halt, during the one-second active wait, and during hold-only measurement. On mismatch, stop the experiment without issuing a three-turn target. This guard passed host tests but was **not physically retested**; it is not a demonstrated fix for the fault or proof of brake release.

No protection threshold, current limit or gain was increased. The exact-zero stop criterion was retained, and STOP UNCONFIRMED was not reported as successful brake lock. Failed move/hold runs did not establish that the intended normal stop/disable/brake-engagement sequence completed.

## Handoff

- The user was asked to switch the servo supply output off and leave the brake connected after reporting loud/stuck behavior.
- Serial control was closed after preserving the logs; no further enable/motion test was performed after the hold-only failure.
- Review the manufacturer procedure for clearing/latching the current position demand before operation, plus motor/drive/encoder matching and holding-brake wiring/power. A properly accessible brake test point or breakout would allow loaded coil-voltage measurement.
- Resolve the bench supply current-limit setting and assess the observed voltage sag without increasing drive protection limits to bypass faults.
- Firmware build and eight host tests passed, including a real-runner hold-only test covering no rotation commands, duration, cancellation, fault, stale feedback, read failure and the observed demand mismatch.

`tools/bench_serial.py` records timestamped TX/RX bytes in JSONL. Its default allowlist is diagnostic; `--allow-three-revolutions` additionally permits the bounded three-turn preset, stationary hold test and explicit fault reset. It does not authorize unbounded raw writes or run anything on connection. Opening/closing this serial bridge was nevertheless associated with P4 boot messages in these sessions, so do not assume that connecting a monitor preserves MCU state.
