# Recording and logger implementation

Read the [master source map](../README.md) and its concurrency vocabulary first. This guide explains ownership; [Log format](../../../docs/LOG_FORMAT.md) owns the stored representation and export rules.

## Session lifecycle

`data_logger` owns filenames, file handles, the file mutex, the 256-entry queue, drop count, and writer task. `app_control` serializes callers and decides when to broadcast commands; the logger itself does not control nodes.

Starting from `LOGGER_IDLE` validates/selects a name and exclusively creates the binary and UTC files before entering `LOGGER_RUNNING`. Companion-open failure prevents a successful session start. Only after that does application policy enable recording beacons and send START.

Stop changes the logger to `LOGGER_STOPPING`, rejecting samples while accepting late health frames during the grace period. Application policy ends live viewing, clears beacon/monitoring state, and sends STOP. The writer atomically closes health admission before its final drain, writes the partial block, closes both files, and sets `LOGGER_IDLE`. A successful stop request is not yet permission to remove power; wait for idle.

## Buffering and failure behavior

Each queued item contains a CAN-style sample and its UTC value computed at enqueue time. Samples received outside `LOGGER_RUNNING` are ignored. A full log queue increments the drop counter rather than blocking acquisition indefinitely.

The writer batches 100 records and syncs each block to the card. While recording it also writes any partial block once per second. A power cut loses at most that second and the queued samples.

Each block is synced to the companion before the binary, so every saved binary record has its UTC entry. After one write error FatFs rejects all later writes to that file until it is reopened. On any write error the writer reopens both files, cuts them back to the last saved record and retries the block once. A block that fails again is cut off and counted in the drop counter, together with queue overflows.

## Making changes

Preserve record/companion index alignment, asynchronous stop semantics, duplicate-name protection, and mutex ownership. Keep exporter rules in the web module and wire contracts in the shared protocol header. Review `data_logger.h` first, then start/enqueue/stop functions and the writer task.

Build the master after changes. On hardware verify repeated start/stop, file integrity, name collisions, companion handling, and sustained collection using [Testing](../../../docs/TESTING.md). Export tests validate selected conversion functions, not this entire writer lifecycle.

Next: [Log format](../../../docs/LOG_FORMAT.md) and [Master architecture](../../docs/MASTER_ARCHITECTURE.md).
