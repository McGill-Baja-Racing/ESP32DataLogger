# Documentation index and glossary

Find a guide by what you want to do. No background is required for the introductions; technical references assume you have read the relevant component README. Return to the [project introduction](../README.md).

## Learning path and task routes

| I want to… | Read |
|---|---|
| Understand what this project is | [Project introduction](../README.md) → [System overview](SYSTEM_OVERVIEW.md) |
| Set up my computer without boards | [Getting started](GETTING_STARTED.md) |
| Make my first contribution | [Contributing](../CONTRIBUTING.md) |
| Work on master behavior | [Master introduction](../master-node/README.md) → [Source map](../master-node/src/README.md) → [Architecture](../master-node/docs/MASTER_ARCHITECTURE.md) |
| Add or change a sensor | [Sensor introduction](../sensor-node/README.md) → [Driver guide](../sensor-node/src/sensors/README.md) → [Extension procedure](../sensor-node/docs/ADDING_SENSORS_AND_NODES.md) |
| Understand node scheduling and recovery | [Node architecture](../sensor-node/docs/NODE_ARCHITECTURE.md) |
| Work on the dashboard or downloads | [Web source guide](../master-node/src/web/README.md) |
| Change recording/storage | [Logger source guide](../master-node/src/logger/README.md) |
| Understand IDs and timestamps | [Protocol](PROTOCOL.md) → [GPS absolute time](../master-node/docs/ABSOLUTE_TIME.md) |
| Decode or analyze recordings | [Log format and exports](LOG_FORMAT.md) |
| Connect or flash boards | [Hardware reference](HARDWARE.md) → [Flashing procedure](FLASHING.md) |
| Test a change | [Testing](TESTING.md) → [RPM simulators](../simulators/README.md) |
| Diagnose a problem | [Troubleshooting](TROUBLESHOOTING.md) |

Introductions teach concepts; procedures describe actions and expected outcomes; specifications define exact contracts. Protocol, hardware, and log-format details belong in their specifications rather than repeated tables in every README.

## Glossary

| Term | Meaning |
|---|---|
| ADC | Analog-to-digital converter: reads a voltage as a number |
| API / callback | A module's public interface / a function passed to another module to call later |
| CAN / TWAI | Shared vehicle communication bus / Espressif's CAN controller interface |
| CSV | Comma-separated text table for analysis tools |
| ESP-IDF | Espressif's firmware framework |
| FreeRTOS | Operating system scheduling firmware tasks |
| GPIO | General-purpose input/output pin |
| Hz / period | Measurements per second / time between measurements |
| I2C | Two-wire connection used for the motion sensor |
| ISR | Interrupt service routine responding to a hardware event |
| Little-endian | Integer byte order with the least significant byte first |
| MPU | The motion sensor family providing acceleration and angular velocity |
| Mutex / critical section | Mechanisms protecting shared state from concurrent access |
| NMEA / RMC | GPS text-message format / the sentence parsed by this firmware |
| Queue / task | Temporary message buffer / independently scheduled firmware work |
| RPM | Revolutions per minute |
| SD / SDMMC | Storage card / interface used by the master to access it |
| UART | Serial connection used by GPS and serial consoles |
| UTC | Calendar time standard used for absolute timestamps |

Next: follow a task route above rather than reading every reference first.
