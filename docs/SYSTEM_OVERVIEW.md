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

GPS is connected directly to the master. Valid receiver messages provide location and speed. A valid date/time can also anchor elapsed master time to **UTC**, the common calendar time standard used in exported recordings. Before that anchor exists, absolute-time fields are blank. Later GPS messages keep that anchor corrected gradually, so absolute time never jumps backwards; see [absolute time](../master-node/docs/ABSOLUTE_TIME.md) for accuracy limits.

## Storage and live display serve different purposes

The SD log stores accepted samples at their source rates. The browser's live graphs show the latest values at a limited refresh rate to keep networking and drawing manageable. A slow graph refresh does not reduce the sensor's logging rate. Queues can still overflow if collection or writing cannot keep up, so drop counters matter.

Connect to the master's `BajaDAQ` Wi-Fi and open `http://192.168.4.1` to control sessions and download completed logs. Live viewing must be enabled separately during each recording. Simulators help test sensor inputs on a bench; they generate electrical signals, not CAN data.

## Follow one brake reading

A voltage becomes pressure, a timestamped CAN message, then a saved CSV row.

The numbers below are illustrative, not calibration evidence. Assume a recording is active, queues have room, and the front brake ADC reports **1250 mV** at an elapsed timestamp of **1000 ms**.

1. The periodic sampler assigns the synchronized timestamp immediately **before** calling the driver. This approximates acquisition time; it is not a timestamp taken after the ADC transaction.
2. [The front brake driver](../sensor-node/src/sensors/front_brake.c) applies the configured divider and pressure conversion: `((1250 - 500 × (0.5)) / (4000 × (0.5))) × 3000`, exactly 1500 psi for these inputs. Positive fractional results are truncated to integer psi; this result is **1500 psi**. Negative calculated pressure is clamped to zero.
3. [The sampler](../sensor-node/src/sampler/sampler.c) queues the value and timestamp. Its sender packs them into the eight-byte payload for CAN ID `0x0B1`. See the [worked wire example](PROTOCOL.md#worked-front-brake-example).
4. [The master callback](../master-node/src/main.c) recognizes the signal, attributes it to brake node 1, and offers it to the logger and live-value cache. Live viewing keeps the latest value when enabled; it does not own acquisition.
5. [The logger](../master-node/src/logger/data_logger.c) queues the record and its UTC entry. The writer saves them asynchronously. Without a UTC anchor, the companion entry is zero.
6. The browser's full CSV identifies the signal as `front_brake_pressure`, with value `1500`, units `psi`, and timestamp `1000`. See the [exact stored bytes and CSV row](LOG_FORMAT.md#worked-front-brake-record).

The driver descriptor requests one read every 10,000 microseconds (100 Hz). That is a nominal schedule: hardware latency, scheduling, and queue losses can affect the records actually received. Serial display is throttled and cannot establish acquisition rate.

## Check your understanding

- Does a slower graph refresh reduce the logging rate? **No.** The graph reads latest values independently; queue pressure can still cause losses.
- Can a sample have a valid elapsed timestamp and blank UTC? **Yes.** Clock synchronization between boards is separate from obtaining a GPS calendar-time anchor.

Continue with the [master introduction](../master-node/README.md), [sensor introduction](../sensor-node/README.md), [protocol specification](PROTOCOL.md), or [log-format specification](LOG_FORMAT.md) depending on your task.
