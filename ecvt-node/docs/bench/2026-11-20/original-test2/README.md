# Original Test 2 comparison

The user selected BAJA_ECVT `run_test2`: velocity command +1000 rpm for 3000 ms, zero velocity for 1000 ms, then controlword 0000 disable. They confirmed the motor was unloaded/clear and the servo power cycle had cleared its red alarm.

## Source fidelity

The selected `run_test2` function, `elec_motor_driver.c` and `.h` were byte-for-byte unchanged from the user's current BAJA_ECVT source. Test-function SHA256: `12bb2bc63581c80e88eba03a717cb2281426fe8b69beab509927115ae5b9077e`.

Changes outside that test:

- Board target changed to ESP32-P4 r3, with compatible chip-revision/CPU configuration, for the production P4 board. Built with the currently installed ESP-IDF 5.5.6.261115 toolchain, not a recovered historical toolchain.
- SD mount failure allowed serial-only operation; SD file tasks skipped when no card mounted. Original test/driver timing calls and CAN operations were retained; this does not reproduce SD task load.
- Startup selected original Test 2 behind an explicit `2` start gate, instead of automatically running the source's currently selected Test 5. Reboots therefore do not automatically repeat the test.

See [startup patch](comparison.patch), [original file hashes](original-source-sha256.json), [staged source archive](comparison-source.tar.gz), and the recorded PlatformIO/SDK configurations. The isolated build directory is `/private/tmp/ecvt-original-test2-20261120`.

## Preflight and result

[Preflight trace](preflight.jsonl): 603F error 0, 1001 error register 0, 1003 history empty, 6062 demand 0, 6064 position 0, actual mode 1, DC link raw 47900.

[Run trace](run.jsonl) recorded the original sequence:

| Device log time | Event |
| --- | --- |
| 12962 ms | RPDO1 controlword 0006; ready status 0231 followed |
| 13972 ms | Controlword 0007; switched-on status 0233 followed |
| 14982 ms | Controlword 000F; operation-enabled status 0237 followed |
| 16012 ms | Original test set operating mode 3 (after enabling, as written) |
| 16082 ms | RPDO2 payload 0A 8B 02 00: raw velocity 166666, original 1000 rpm conversion |
| 19092 ms | RPDO2 velocity zero |
| 19912 ms | EMCY 0xFF2F, register 1; then fault status 1218 with operation disabled |
| 20092 ms | Controlword 0000; original test printed TEST 2 done |

The user reported the brake did not release. The same 0xFF2F fault observed in the migrated runner also occurred with the original Test 2 and original driver. This does not establish whether the cause is brake power/configuration, another hardware/drive setting, shared command assumptions, or an interaction with retained drive parameters. The statusword printed after the final disable was an older queued sample, so that line does not independently confirm the final drive state or brake engagement. No SD motion trace was available.

After completion, the unchanged test's infinite busy loop starved IDLE0 and produced task-watchdog diagnostics. That happened after the test and is distinct from the servo EMCY. The agent closed serial control and reset only the P4 to return to its idle start gate; no second test was started.

## State-only code the user identified

`BAJA_ECVT/src/main.c:run_test1` calls `elec_motor_enable_operation_sequence` and sends no position/velocity/torque target. That helper sends 0006, 0007, 000F and waits 1000 ms after each, with status printing. Test 2 calls the same helper before its velocity target. The position-specific helper instead sends 0000, 0006, 0007, 000F with 200 ms between commands; the position test then waits another 1000 ms before its target.

Standalone original Test 1 has **not** been run in this comparison. At completion of this comparison, its image selected Test 2 on `2` and did not implement the modern terminal commands. In a subsequent user-requested single-step session, the agent restored the guarded terminal firmware and added `servo cw`; the first isolated 0006 request was acknowledged with status 0231. Do not infer mechanical brake release solely from operation-enabled status.
