# Master runtime architecture

Read the [master introduction](../README.md) and [source map](../src/README.md) first. This guide owns runtime behavior; [Protocol](../../docs/PROTOCOL.md), [Hardware](../../docs/HARDWARE.md), and [Log format](../../docs/LOG_FORMAT.md) own exact external contracts.

## Startup and composition

`main.c` mounts SD and initializes the logger, live cache, registry, CAN (when enabled), GPS receiver, and application-control mutex. Mount/init failures guarded by `ESP_ERROR_CHECK` abort normal startup. The master sends initial STOP copies before exposing manual controls or auto-start.

Serial control and time beacons start, and an auto-start task waits five seconds before requesting recording. Web startup is attempted separately: networking failure is reported without aborting the collection path. `MasterNoCAN` removes CAN initialization, commands, beacons, and node monitoring while retaining local GPS, logging, and web/serial operation.

## Collection tasks and queues

CAN receive callbacks copy frames into the CAN queue. A dispatch task invokes the application callback. State reports update the registry; recognized eight-byte sensor frames update traffic-based liveness and enter logger/live APIs. The main callback rejects engine/spark timestamps with the sign bit set. GPS owns UART and parser work and reports locally generated samples through a second application callback into the same logger/live pipeline.

The logger has a separate writer task and bounded queue. Live data stores latest values rather than an independent full-rate history. Neither live viewing nor web download should become the owner of sensor acquisition.

## Session policy

`app_control` serializes serial and HTTP lifecycle requests with a mutex. Start is accepted only while idle. The logger opens a unique session first; application policy then enables monitoring/recording beacons and broadcasts START. Failed command transmission is reported without rolling back the already-open logger.

Stop is accepted only while running. It ends live viewing and requests logger stop, then clears recording/monitoring state and broadcasts STOP. The writer drains already accepted data asynchronously before becoming idle. Repeated API start/stop requests conflict with the current lifecycle state, even though node-level commands are idempotent.

Default naming can wait up to three seconds for GPS UTC. [Logger guide](../src/logger/README.md) explains buffering, checkpoints, and companion failure behavior.

## Time and liveness

Time beacons broadcast master elapsed time and recording state periodically. Node timestamps are translated to this clock before transmission. GPS calendar synchronization is a separate concern; see [Absolute time](ABSOLUTE_TIME.md).

The registry receives explicit state transitions, but active-recording liveness comes from sensor traffic. Its expected-node mask selects monitored nodes. Idle operation disables traffic monitoring. Missing data can indicate a sensor/read problem as well as a bus or power failure.

## Recovery and limits

CAN owns controller error reporting, bus-off detection, and recovery. Bounded receive/logger queues can drop data; check both counters. SD write errors are logged and require file-integrity checks. Network failure can leave serial control useful; SD mount failure does not produce a functioning logger. An active/closed file boundary protects downloads and renames.

Next: [Web guide](../src/web/README.md), [Testing](../../docs/TESTING.md), and [Troubleshooting](../../docs/TROUBLESHOOTING.md).
