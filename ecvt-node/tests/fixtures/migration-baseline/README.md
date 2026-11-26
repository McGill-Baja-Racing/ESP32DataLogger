# Preserved migration reference

These immutable files are test data from
`https://github.com/McGillBajaRacing/ESP32_ECVT`, commit `6e033d5c100ef7d887510a19da2e0f2c0f31b65d`.
They preserve the working-tree baseline previously checkpointed there; the
controller source hash remains `be9b851e561c2ea9d6aad5adcecee3e3a1458d7a83418c288b189dea9084391b`.

Only files read by the extraction, copy and differential calculation checks are
included. They are not compiled into firmware or connected to the DAQ runtime.
`SHA256SUMS.json` records their original bytes; do not edit fixtures to make a
behavior change pass. Record intentional changes in the migration manifest and
add behavior checks instead. Older full projects remain in the source repository.
