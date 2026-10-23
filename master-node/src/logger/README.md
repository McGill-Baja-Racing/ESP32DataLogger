# Recording and logger implementation

Read the [master source map](../README.md) and its concurrency vocabulary first. This guide explains ownership; [Log format](../../../docs/LOG_FORMAT.md) owns the stored representation and export rules.

## Session lifecycle

`data_logger` owns filenames, file handles, the file mutex, the 256-entry queue, drop count, and writer task. `app_control` serializes callers and decides when to broadcast commands; the logger itself does not control nodes.

Starting from `LOGGER_IDLE` validates/selects a name and exclusively creates the binary and UTC files before entering `LOGGER_RUNNING`. Companion-open failure prevents a successful session start. Only after that does application policy enable recording beacons and send START.

Stop changes the logger to `LOGGER_STOPPING`, rejecting further enqueue attempts. Application policy ends live viewing, clears beacon/monitoring state, and sends STOP. The writer asynchronously drains accepted samples, writes the partial block, closes both files, and sets `LOGGER_IDLE`. A successful stop request is not yet permission to remove power; wait for idle.

## Buffering and failure behavior

Each queued item contains a CAN-style sample and its UTC value computed at enqueue time. Samples received outside `LOGGER_RUNNING` are ignored. A full log queue increments the drop counter rather than blocking acquisition indefinitely.

The writer batches 100 records. At approximately 15-second intervals during recording, it writes any partial block and checkpoints by closing/reopening files. Written blocks are flushed, but this is not a guarantee against all power-loss/card failures.

UTC entries track successfully written complete binary records. Companion write/flush failure closes the companion and leaves later absolute time unavailable. A partial binary write disables further companion writing. SD failures are reported; the queue-drop counter is not a comprehensive file-integrity indicator.

## Making changes

Preserve record/companion index alignment, asynchronous stop semantics, duplicate-name protection, and mutex ownership. Keep exporter rules in the web module and wire contracts in protocol headers. Review `data_logger.h` first, then start/enqueue/stop functions and the writer task.

Build the master after changes. On hardware verify repeated start/stop, file integrity, name collisions, companion handling, and sustained collection using [Testing](../../../docs/TESTING.md). Export tests validate selected conversion functions, not this entire writer lifecycle.

Next: [Log format](../../../docs/LOG_FORMAT.md) and [Master architecture](../../docs/MASTER_ARCHITECTURE.md).
