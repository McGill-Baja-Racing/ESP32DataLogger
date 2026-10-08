# How the DAQ system works

This guide explains the system before the implementation. No prior electronics knowledge is required. Start with the [root README](../README.md), or continue afterward to [Getting started](GETTING_STARTED.md).

## Measurements become samples

A sensor converts a physical quantity, such as pressure or rotation, into an electrical signal. A sensor node reads that signal and turns it into a **sample**: a measurement value and the time it was measured. The gearbox bearing provides a raw rotation measurement used later to calculate wheel and secondary RPM using drivetrain ratios. The firmware records that bearing RPM without doing those conversions.

Most sensors are read periodically; engine speed uses the time between spark-input edges instead.

Sensor nodes are ESP32-C3 microcontrollers. Different firmware build profiles select different sensors. Hardware selection happens when compiling, not through the browser at runtime.

## Nodes share a connection and a clock

**CAN** is a shared wired communication bus used by the vehicle boards. Each sensor message has an identifier that tells the master which measurement it contains. A CAN transceiver converts a microcontroller's transmit/receive signals into the electrical bus signals.

Every board has its own clock. The master repeatedly broadcasts its time so nodes can express measurements on the same time base. This makes samples from different boards comparable even when they arrive at different times. The time is elapsed master-clock time, not automatically a calendar date.

## A recording has a lifecycle

The ESP32-P4 master opens a new SD log before telling nodes to start. During recording it accepts samples into a queue, a temporary waiting line, and writes them in groups. It also monitors whether configured nodes are still sending data.

On Stop, the master stops accepting new log samples and tells nodes to stop. Its writer finishes queued samples and closes the files. Wait for the logger to become idle before removing power or the card. The master attempts automatic recording five seconds after initialization; an unnamed start can wait briefly for GPS time to choose a date-based filename.

The periodic time message also carries the recording state. A node that reboots can rejoin a recording on its next beacon.

## GPS and calendar time

GPS is connected directly to the master. Valid receiver messages provide location and speed. A valid date/time can also anchor elapsed master time to **UTC**, the common calendar time standard used in exported recordings. Before that anchor exists, absolute-time fields are blank. GPS time is not continuously corrected in this implementation; see [absolute time](../master-node/docs/ABSOLUTE_TIME.md) for accuracy limits.

## Storage and live display serve different purposes

The SD log stores accepted samples at their source rates. The browser's live graphs show the latest values at a limited refresh rate to keep networking and drawing manageable. A slow graph refresh does not reduce the sensor's logging rate. Queues can still overflow if collection or writing cannot keep up, so drop counters matter.

Connect to the master's `BajaDAQ` Wi-Fi and open `http://192.168.4.1` to control sessions and download completed logs. Live viewing must be enabled separately during each recording. Simulators help test sensor inputs on a bench; they generate electrical signals, not CAN data.

## Follow one brake reading

1. The front pressure sensor produces a voltage.
2. The brake node reads it, applies the configured conversion, and attaches a synchronized timestamp.
3. The node sends a CAN message identified as front brake pressure.
4. The master attributes it to the brake node and offers it to the logger and live-value cache.
5. The logger writes a binary record and an aligned UTC entry, or zero if UTC is unavailable.
6. A full CSV download turns the record into a named signal, value, units, and timestamps.

Continue with a [component introduction](README.md), [protocol specification](PROTOCOL.md), or [log-format specification](LOG_FORMAT.md) depending on your task.
