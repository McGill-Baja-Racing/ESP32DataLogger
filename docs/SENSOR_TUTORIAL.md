# Worked tutorial: follow and extend a periodic measurement

First trace a working sensor, then plan an illustrative scalar signal. This tutorial teaches where changes belong; it does not implement a new hardware sensor. You need basic C and a successful sensor build. Start with the [sensor introduction](../sensor-node/README.md) and keep [Adding sensors and nodes](../sensor-node/docs/ADDING_SENSORS_AND_NODES.md) open as the authoritative extension checklist.

Before changing code, confirm [repository access](../CONTRIBUTING.md#get-repository-access-first) and create a [task branch](../CONTRIBUTING.md#work-on-a-branch).

## 1. Follow the existing front-brake driver

Open these files in order:

| File | What to find and why |
|---|---|
| [`sensor-node/platformio.ini`](../sensor-node/platformio.ini) | `NodeBrake` selects node 1 and the brake configuration at build time |
| [`sensor-node/src/sensors/sensor_registry.c`](../sensor-node/src/sensors/sensor_registry.c) | The brake branch copies front/rear descriptors into the selected array |
| [`sensor-node/src/sensors/front_brake.c`](../sensor-node/src/sensors/front_brake.c) | `read_pressure()` reads ADC millivolts, converts to integer psi, and clamps negative results to zero |
| [`sensor-node/src/sensors/sensor.h`](../sensor-node/src/sensors/sensor.h) | The descriptor contract: period, callbacks, value output, and driver context |
| [`sensor-node/src/sampler/sampler.c`](../sensor-node/src/sampler/sampler.c) | Initializes shared ADC support, assigns timestamps before reads, queues successful values, and sends them separately |

The front descriptor requests 10,000 microseconds between reads, or 100 Hz. Its conversion belongs in the driver; changing pressure calibration should not require a sampler change. A failed ADC read returns an error and does not generate a fabricated zero measurement. Follow the [worked brake reading](SYSTEM_OVERVIEW.md#follow-one-brake-reading) through CAN, master logging, and CSV to connect this code to a recording.

Before continuing, explain which file changes for a different pressure span, a different nominal rate, or a different physical pin. Then identify which corresponding protocol or hardware reference also needs updating.

## 2. Specify an illustrative scalar signal

Suppose a future device should report an integer angle in milli-degrees at 50 Hz on an existing node. Specify the physical input, legal range, signedness, conversion, failure behavior, and test input before writing hardware code. Choose an unused signal ID only after checking the shared protocol header and command/state ranges; `0x0BD` is an illustrative candidate, not a permanent reservation.

A hardware-free practice stub could return a fixed known integer through a periodic `read` callback. Label its name and PR as a stub: it validates plumbing, not angle measurement or calibration. Do not claim that a constant value implements a sensor. Follow Workflow A in [Adding sensors and nodes](../sensor-node/docs/ADDING_SENSORS_AND_NODES.md#workflow-a-add-a-sensor-to-an-existing-configuration) for the descriptor skeleton and actual edits rather than copying a second implementation here.

## 3. Make a complete change inventory

| Area | Required change and reason |
|---|---|
| New driver and `sensor_registry.c` | Define the descriptor and select it for the intended build; use a positive 20,000 µs period and bounded `read` |
| `sensor-node/src/CMakeLists.txt` | Add the new source to explicit `idf_component_register(SRCS ...)`; files are not automatically discovered here |
| Root `protocol/app_protocol.h` | Define the ID and source-node mapping once for both firmwares; acceptance follows the mapping |
| `master-node/src/live/live_data.h` and `.c` | Expand `LIVE_DATA_SIGNAL_COUNT` and add names, units, nominal rate, and node metadata |
| `master-node/tools/decode_log.py` | Add matching offline metadata |
| Protocol, hardware, and affected profile documentation | Let the next contributor reproduce the meaning, wiring, and build |

Read a reused driver's preprocessor guards: compiling its file does not guarantee its descriptor exists in your selected configuration. New ADC configurations must also extend shared ADC initialization in `sampler_init()`. Keep hardware state in the driver; avoid adding sensor-specific conversion to the generic loop.

A new physical node adds more work than an additional signal: follow Workflow B for node identity, registry capacity, expected-node mask, application status, HTTP status, and browser display. The current node-7 example explicitly needs those extensions. Two concurrent physical sources cannot share a signal ID: the data payload has no separate source-node field. Powertrain CSV inclusion is a separate exporter design; full CSV does not need a new per-signal column.

## 4. Validate each boundary

Build the affected profile, replacing `PROFILE` with its actual environment name:

```bash
pio run -d sensor-node -e PROFILE -t buildprog
pio run -d master-node -e MasterStable -t buildprog
```

For shared sensor/protocol edits, use the broader matrix in [Testing](TESTING.md). Build success checks integration, not electrical correctness. Use the extension guide's isolated serial bench procedure to confirm a known input, sign, units, range, and failure behavior. A stub must report exactly its declared fixed value. Serial output is throttled and cannot establish sampling rate.

With normal CAN firmware, verify idle/START/STOP behavior, correct node attribution, and known values in browser full CSV. Check source timestamp intervals and counts over a known duration against the nominal rate, allowing for documented scheduling and drop behavior. Decode a copied binary file with [the offline tool](LOG_FORMAT.md#offline-decoding) and confirm names, units, and values agree; that tool does not read UTC companions. For a new node, test disconnection and reboot/rejoin status too.

Record profiles, input values, duration, drop counters, exports, and remaining failures in the PR. Without hardware, state which build/code checks ran and leave bench and CAN acceptance pending. Event-driven sensors such as engine spark capture require a different capture/queue/sampler design; this periodic tutorial is not their extension procedure.
