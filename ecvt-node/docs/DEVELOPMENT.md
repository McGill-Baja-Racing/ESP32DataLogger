# eCVT development sequence

## Current scope: one repository, standalone runtime

User direction, 2026-11-26: first place the validated eCVT node on its own branch
inside the DAQ repository. Develop manual control, motor tuning and a web
interface before connecting to the DAQ sensors, master, recording lifecycle or
shared protocol. Earlier integration-first work orders are superseded by this
sequence. Moving files is not evidence of DAQ compatibility or deployment.

The branch is `BajaRemy/ecvt-standalone`, based on DAQ main
`5fa2917e4d311e9a419ffd37b93ba92420df3806`. The imported eCVT source checkpoint is
`6e033d5c100ef7d887510a19da2e0f2c0f31b65d`. The runtime was subsequently reduced to the standalone motor core.

## Development stages

1. **Standalone migration:** portable build/tests, existing terminal behavior,
   source provenance, bench evidence and an editor workspace entry. No shared
   protocol or master/sensor firmware changes.
2. **Manual control and tuning interface:** a browser interface for status,
   explicit start/stop, bounded position/velocity tests, parameter readback and
   deliberate tuning changes. Keep the existing single motor owner; UI/API
   handlers submit operations rather than writing to the drive concurrently.
3. **Repeatable test scripts and measurements:** user-editable sequences of
   named motor actions, waits and logging, with explicit run/cancel, execution
   status and a saved result linked to the parameter configuration used. Define
   the script format and supported actions before implementing an interpreter.
4. **Loaded-system validation:** retune and validate for the gearbox and intended
   eCVT mechanism; retain separate bare-motor and loaded parameter snapshots.
5. **DAQ connection:** only after the standalone workflow is accepted, design
   the second CAN bus, input conversions/freshness, acquisition demand, telemetry,
   health and recovery. The [older migration plan](ECVT_MIGRATION_PLAN.md) and
   [CAN budget](ECVT_CAN_BUDGET.md) are background for this later stage.

## Drive-control checkpoint

The [drive-control core](MOTOR_CORE.md) now provides the shared operation, status/result and parameter-access API. The terminal is an adapter. Legacy controller, acquisition, replay and recording implementations and their service hooks have been removed. Web and script frontends can use this API without duplicating drive ownership. No DAQ runtime connection or web server was added.

## Decisions for the interface stage

- Implement the device-hosted interface using the board's network companion.
  The user confirmed it is the same board type as the master, on a newer revision.
  Network setup and the web server are not implemented in this firmware yet.
- Select parameter groups and engineering units. Read the current drive values,
  display saved/current differences, and distinguish temporary writes from
  persistent storage. No blanket factory reset or guessed motor/encoder settings.
- Define the manual operations and test-script actions the user needs first.
  No general script engine or tuning UI is implemented.
- Choose plots and exported results: target/actual position and velocity,
  current/torque, state/faults, parameter snapshot, timestamps and loss counts.
- Keep stop/0000 accessible during an active test; opening a page or reconnecting
  must not start motion. Clarify completion behavior for each new test action.

The master's interface may inform layout and reusable web tooling. This stage
must not depend on the master being present, on recording START/STOP, or on
sensor traffic. It is a standalone motor development tool.

## Migration validation

Run eCVT and existing DAQ host checks from the repository root:

```sh
python3 -m unittest discover -s ecvt-node/tests -v
python3 -m unittest discover -s master-node/tests -v
pio run -d ecvt-node -e esp32-p4
```

CI includes the standalone project and its host checks. Its toolchain pin is
checked separately from the unchanged DAQ projects. The import provenance
records hashes of the initial import, not the current refactored source. Legacy
comparison fixtures were removed with the old runtime. No hardware upload or actuation is part of this migration.
