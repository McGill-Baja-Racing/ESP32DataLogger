# Sensor source map

Use this map after the [sensor introduction](../README.md). It explains ownership and a useful code-reading order. [Node architecture](../docs/NODE_ARCHITECTURE.md) covers runtime details.

## Concurrency vocabulary

FreeRTOS **tasks** are independently scheduled functions. The sampler's **queue** holds samples awaiting transmission, so reading sensors does not directly wait for CAN. **Callbacks** let CAN request application actions without owning sensor drivers. Hardware interrupts run an **ISR**, a short handler that captures edges; **critical sections** protect state shared with tasks.

A slow read can delay other periodic sensors. Interrupt handlers must use interrupt-safe operations, not ordinary blocking calls. Learn these concepts before changing scheduling or shared state.

## Read in this order

1. `main.c` for startup and callback wiring.
2. `sensors/sensor.h` and `sensors/sensor_registry.c` for the driver interface and build selection.
3. Your driver and `sampler/sampler.c` for acquisition/output.
4. `can/can_node.c` and `time/time_sync.c` for transport and timestamps.

| Module | Responsibility |
|---|---|
| `main.c` | Startup and connecting CAN callbacks to sampler/time sync |
| `sensors/` | Hardware state, conversion, build-selected descriptors; [driver guide](sensors/README.md) |
| `sampler/sampler.*` | Start/stop, periodic schedules, engine event consumption, output queue |
| `can/can_node.*` | TWAI tasks, command dispatch, state acknowledgements, recovery |
| `time/time_sync.*` | Protected master-to-local clock offset |
| `protocol/app_protocol.h` | Shared identifiers and encodings |
| `CMakeLists.txt` | Compiled source registration |
| `../platformio.ini` | Board role, node ID, configuration flags, bench mode |

## Ownership rules

Drivers own their hardware and mutable `context`. The sampler owns `next_sample_us`, its active state, and pending output. CAN owns transport and reported node state. Time sync owns the clock offset. `main.c` connects public APIs; it should not contain calibration or CAN-driver logic.

```text
CAN START/STOP/beacon → main callbacks → sampler / time_sync
registry → selected drivers → sampler queue → CAN send task
engine interrupt → driver event queue → sampler queue → CAN send task
```

The engine build has a dedicated event path; the generic loop is for positive-period polling descriptors. Changing a driver's registry selection may also require updating shared ADC initialization conditions and master mappings.

Next: [Sensor drivers](sensors/README.md), [Adding sensors/nodes](../docs/ADDING_SENSORS_AND_NODES.md), [Protocol](../../docs/PROTOCOL.md), and [Testing](../../docs/TESTING.md).
