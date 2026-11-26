# Standalone import validation, 2026-11-26

Destination branch: `BajaRemy/ecvt-standalone` in
`McGill-Baja-Racing/ESP32DataLogger`.
DAQ base: `5fa2917e4d311e9a419ffd37b93ba92420df3806`.
Source: `McGillBajaRacing/ESP32_ECVT` commit
`6e033d5c100ef7d887510a19da2e0f2c0f31b65d`.

## Changes

- Added `ecvt-node/` as its own PlatformIO project without modifying its 47
  runtime/build source files. Their source hashes are in
  [import provenance](IMPORT_PROVENANCE.json).
- Included vendor references, prior bench reports and submitted traces inside
  the project; rebased their local documentation links.
- Replaced external legacy-repository test dependencies with 11 immutable
  fixture files, original checksums and source provenance. Reference fixtures
  are test data only and are not compiled into the firmware.
- Pinned the same pioarduino 55.03.313 release previously resolved through
  `stable`; master/sensor/simulator projects retain 55.03.312-1. No compiler or
  ESP-IDF upgrade was introduced for the node. CI checks both explicit policies.
- Saved the prior nondefault SDK settings in `sdkconfig.defaults`; generated
  `sdkconfig.esp32-p4` and build products stay ignored. Compared the generated
  SDK configuration with the source build: all operational symbols match.
  The only differing symbol is `IDF_INIT_VERSION`, which records the version
  in which a configuration was originally created (5.5.1 vs 5.5.6).
- Added an editor workspace entry, repository introduction/index links and CI
  build/host checks for the standalone project.
- Recorded the user's standalone-first web-tuning/script development sequence.

The master, sensors, shared protocol and simulator firmware are unchanged.
No DAQ input adapter, telemetry, acquisition demand, network server, web UI or
script interpreter is introduced. Current runtime control remains terminal-based.

## Results

From the DAQ root:

```sh
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk python3 -m unittest discover -s ecvt-node/tests -v
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk python3 -m unittest discover -s master-node/tests -v
pio run -d ecvt-node -e esp32-p4
```

- eCVT host tests: **11 passed**, including 76,403 differential calculation cases
  and source-fixture checksum verification.
- DAQ host tests: **35 passed**, with CSV exporter checks also reporting success.
- Clean relocated eCVT firmware build: **SUCCESS**, using ESP-IDF 5.5.6 and
  `esp32-p4_r3`. Reported RAM 21,364 bytes, application flash 426,966 bytes.
- All 47 runtime/build source files match the imported source commit byte for byte.
- Local Markdown file links resolve; both CI platform-pin policies pass.
- Full existing DAQ firmware matrix was not rebuilt: its runtime/configuration
  files were unchanged. CI includes all prior projects plus the added node.

The original firmware was physically tested as documented in the
[November 25 bench report](bench/2026-11-25/README.md). The relocated build has
not been flashed or actuated during migration. Existing ADC deprecation warnings
and the generic board's flash-capacity reporting remain known baseline issues.
Runtime and immutable fixture bytes are intentionally preserved, including any
inherited whitespace; new documentation/integration edits are checked separately.

## Next development

Open [the project introduction](../README.md) and
[development sequence](DEVELOPMENT.md). Begin by choosing the browser connection
method and the first manual-control/parameter/script features. DAQ runtime
connection remains a later stage.
