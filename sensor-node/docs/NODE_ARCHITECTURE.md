# Sensor node runtime architecture

Read the [sensor introduction](../README.md) and [source map](../src/README.md) first. This guide owns initialization, scheduling, and recovery behavior; [Protocol](../../docs/PROTOCOL.md) and [Hardware](../../docs/HARDWARE.md) own external contracts.

## Startup and modes

`main.c` waits three seconds after reset so USB monitoring can reconnect. The sampler obtains the build-selected registry, initializes shared ADC support where needed, initializes selected drivers, and creates output/sampling tasks. Sensor initialization happens before CAN can issue START.

Normal mode initializes CAN with callbacks for start, stop, time synchronization, and bus-off queue discard, then reports boot/idle state. Serial-test mode skips CAN and starts sampling locally. Build/profile selection, including MPU and engine bench roles, is documented in the [sensor introduction](../README.md).

## Commands and clock ownership

CAN's receive callback copies messages into a queue; a dispatch task interprets commands and beacons. START/STOP transitions call the application sampler callbacks, while repeated copies send acknowledgements without restarting sampling. A beacon updates the protected clock offset and reconciles active/idle state with the master.

`time_sync` converts local capture times to master-clock milliseconds. Engine uses captured edge time rather than transmission time. Other periodic samples take a synchronized timestamp before the read callback. Serial mode has no master synchronization.

## Sampling and output

On start, the sampler clears its output queue, invokes optional driver start callbacks, arms schedules, and becomes active. Periodic drivers are visited when due. Failed reads are omitted; missed schedule periods are advanced rather than emitted as fabricated catch-up samples. The task always yields at least one FreeRTOS tick, so real cadence depends on task scheduling and read duration.

The engine build instead consumes the driver's interrupt event queue. It publishes detected sparks and valid RPM intervals, plus timeout zeros. This special path does not support arbitrary zero-period descriptors. See [Sensor driver guide](../src/sensors/README.md).

The 64-entry output queue separates acquisition from CAN sending. Periodic overflow discards an older queued sample in favor of a current one. Engine CAN output reserves room for each complete event; insufficient space loses the event. Capture overflow is also reported. The send task packs values/timestamps or prints throttled serial values.

On stop, sampling becomes inactive and pending output is cleared. Drivers reset session-specific state on the next start; the engine clears its capture queue then. A frame already being transmitted can still be in flight.

## CAN recovery

CAN owns its transport mutex, queues, controller state, error counters, and recovery task. Bus-off preserves active/idle state but asks the sampler to discard pending output; the engine discard callback also clears captured events. Recovery reports a transition and subsequent beacons can reconcile recording state. Ordinary sensor traffic provides master liveness; no heartbeat task is used.

Next: [Adding sensors/nodes](ADDING_SENSORS_AND_NODES.md), [Testing](../../docs/TESTING.md), and [Troubleshooting](../../docs/TROUBLESHOOTING.md).
