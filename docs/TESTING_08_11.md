# testing-08-11 integration

Created from `main` at `9e2a54b94e2cf3596fc1ebc0cf4a79cf9dee19da`.
The existing `testing` branch is unchanged. The IR sensor PR (#142) is excluded.

## Merge order and source revisions

Each PR has one merge commit on the branch's first-parent history. Conflict
resolutions and integration fixes belong to the corresponding merge commit.

| Order | PR | Source branch | Source revision |
|---|---|---|---|
| 1 | #143 | `BajaRemy/aim-evo4s-rpm` | `a88f6ddeeb5407609d0994b156b737e9bacac8d6` |
| 2 | #144 | `Frank1304/lin-pots-parallel` | `22e101ebfda13e1ca107f80facf89b209c0292e9` |
| 3 | #138 | `yann/node-health` | `54fde3744525fbb32ae37c4fdf00bc8b1e213664` |

## Integration fixes

- AiM keeps its reception timestamps, independent RPM channel, and recording
  lock spanning CAN dequeue and admission. START prepares files, clears both
  queues, and then admits samples. Node health uses this same recording API;
  health admission opens only at START and closes before the final STOP drain.
- Both shock profiles use normal CAN operation. Driver initialization owns the
  shared ADC, including the existing brake and generic ADC drivers. The sampling
  deadline keeps its original 64-bit uptime calculation.
- Node capacity includes IDs 7 and 8. Their four pot channels have live,
  status, CSV, offline-decoder, and health metadata. The original rear second
  pot ID `0x0C1` collided with the brake node state report; it now uses `0x0C9`.
  The other pot IDs remain `0x0BD`, `0x0BE`, and `0x0BF`.
- Health slot lookup includes all four pot IDs. Offline event subjects retain
  bits 1–7 for nodes 1–7 and use the previously unused bit 0 for node 8, which
  otherwise would be truncated by the eight-bit field. Both decoders agree.
- Compact health subjects preserve AiM wire ID `0x500` and normalized RPM ID
  `0x703` in drop/overflow diagnostics. Full CSV gains the health PR's `detail`
  column; Powertrain CSV retains its original schema and engine RPM source.

Flash matching master and node firmware from this branch together. The master
expects shock nodes 7 and 8; adjust `MASTER_EXPECTED_NODE_MASK` for a smaller
bench configuration. See [Protocol](PROTOCOL.md), [sensor profiles](../sensor-node/README.md),
and [AiM setup](../master-node/docs/AIM_EVO4S.md).

## Validation

| Integration stage | Host tests | Master profiles | Sensor profiles |
|---|---|---|---|
| AiM | 18 passed | All 3 passed | No sensor code changed |
| AiM + linear pots | 20 passed | All 3 passed | All 8 passed |
| AiM + linear pots + health | 27 passed | All 3 passed | All 8 passed |

Both unchanged RPM simulator profiles also built successfully: 13 firmware
profiles in total. The dashboard JavaScript parsed successfully in V8; both
AiM and health elements remain present. All four projects retain the same
PlatformIO platform pin. Staged whitespace and conflict-marker checks passed.

Host checks compile production C for AiM decoding/routing, START/STOP with CAN
enabled and disabled, health admission/draining across two sessions, pot
conversion and ADC failures, CAN/control separation, health slots and offline
masks, health event packing/sequences, full CSV/decoder parity, Powertrain CSV,
and absolute time. These checks do not establish hardware behavior.

Reproduce from the repository root with Python 3, a C compiler, and PlatformIO:

```sh
python3 -m unittest discover -s master-node/tests
pio run -d master-node
pio run -d sensor-node
pio run -d simulators/engine-rpm
pio run -d simulators/wheel-rpm
```

Local host tests used
`SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk` because the
default SDK is incompatible with the installed linker.

No hardware was flashed. Physical CAN behavior, AiM configuration and timing,
pot calibration, sensor disconnection/reconnection, SD recording, and browser
operation on a real master still need bench validation. No independent human
validation was performed for this combined branch.

## Reverting

List the three merge commits with:

```sh
git log --first-parent --oneline main..testing-08-11
```

Revert a merge with `git revert -m 1 <merge-commit>`. Revert newest merges first
when removing multiple PRs; the health integration builds on the earlier merges.
