# Guidance for AI coding agents

These instructions apply throughout this repository. Read any more specific
`AGENTS.md` in the area being changed, plus `CONTRIBUTING.md` and the relevant
component README. Explicit user instructions take precedence over this guidance.

## Understand the task before changing code

- Check the current branch and working tree. Preserve unrelated work.
- Read the existing implementation, its callers and current documentation before
  proposing a new structure. Historical plans are background, not proof of what
  the current firmware implements.
- Make the smallest coherent change that completes the requested task. Avoid
  unrelated cleanup, new dependencies or speculative future features.
- State material assumptions. Ask for clarification when a missing requirement
  affects behavior or scope; resolve ordinary implementation choices yourself.

## Organize code by responsibility

- Give each module a clear responsibility and owner. Extend the appropriate
  existing module before creating another file, service, class or abstraction.
- Group closely related operations together. Do not create a `.cpp`/`.h` pair
  merely because one function has a name. Split when responsibilities, reuse,
  dependencies or readability justify an independent module.
- Keep modules focused; do not turn an existing file into a collection of
  unrelated helpers. There is no arbitrary file-count or line-count target.
- Keep `main` for startup and wiring. Keep terminal, web and other interface
  handlers thin: parse/validate input, submit operations and present results.
- Keep hardware access, unit conversion and protocol encoding in their owning
  drivers/modules. Do not duplicate them in interfaces or application logic.
- Reuse an existing repository library when its behavior and dependencies fit.
  Do not import an unrelated subsystem merely to reuse a small helper.
- Expose only the declarations callers need. Keep implementation helpers private
  to their module. Avoid broad internal headers and mutable shared globals.
- When moving code, update callers, build lists, tests and authoritative docs;
  remove obsolete files and wrappers instead of leaving parallel implementations.

## Write understandable code

- Follow the surrounding language and formatting conventions. Prefer explicit,
  readable control flow over dense expressions, macro tricks or clever shortcuts.
- Use descriptive domain names. Include units in names where ambiguity matters,
  such as `position_counts`, `speed_rpm`, `timeout_ms` and `received_us`.
- Name and explain protocol constants, masks, conversions and timing values when
  their meaning is not clear locally. Link a manual section or existing reference
  for device-specific behavior; do not invent register meanings or conversions.
- Make state transitions, ownership, side effects and error paths easy to follow.
  Distinguish command acceptance, acknowledgement and verified completion.
- Handle failures explicitly. Do not silently substitute success, stale feedback
  or a zero measurement for an unsuccessful read. Keep waits bounded and long
  operations cancellable where the existing control flow requires cancellation.
- Comment intent, units and non-obvious constraints. Avoid comments that merely
  repeat the code and avoid leaving dead code or unfinished scaffolding.

## Respect component boundaries

- Follow the master and sensor source maps. Sensor conversion belongs in its
  driver, sampling in the sampler, CAN transport in its CAN module, and master
  recording policy in `app_control`.
- Shared DAQ messages belong in `protocol/app_protocol.h`. A protocol change
  needs corresponding consumers, metadata, decoding and documentation updates.
- Keep the eCVT project standalone until DAQ integration is explicitly requested.
  Do not reconnect legacy automatic control, sensor inputs, replay or SD logging
  as part of a refactor. Web control, tuning and scripts are separate development
  steps; do not claim that they exist before implementing them.
- In eCVT, `motor/` owns operations, mode preparation and drive sequencing;
  `servo/` owns CANopen encoding and feedback/drive-state primitives; `can/`
  owns transport; `app/` and `console/` adapt user commands to the motor API.
- Preserve the single motor-operation owner. New interfaces submit requests;
  they must not issue concurrent drive transactions around that owner.
- Preserve validated motor behavior during structural changes, including zero
  target before velocity-mode enable and the unconditional stop/0000 path.
  Do not add mode/velocity prerequisites to disable. Treat behavior changes as
  explicit task decisions and describe their effect separately from file moves.

## Validate and report accurately

- Run checks appropriate to the changed behavior. Use meaningful host tests for
  protocol/state/ownership changes and build affected firmware. For file moves,
  run existing relevant checks before inventing new tests.
- Update tests to exercise the production implementation at its new location;
  do not replace the moved logic with a test-only copy or weaken assertions to
  make a refactor pass.
- Check the final diff for unintended changes and whitespace errors. Keep
  generated SDK configuration, build output and machine-specific settings out of
  commits; edit intentional defaults and build configuration instead.
- Update the current usage/source guide when behavior or module ownership changes.
  Mark historical evidence as historical rather than implying it validates a new
  firmware revision.
- Report what changed, which checks actually ran, their results and remaining
  limitations. Builds and host simulations do not prove physical motor behavior.
- Do not flash, actuate hardware or change persistent drive parameters unless
  the user has authorized that action. Follow repository review requirements
  before merging; do not merge your own changes to bypass review.
