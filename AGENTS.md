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

- Group related operations, separate distinct responsibilities and avoid
  unnecessary navigation. Extend a suitable existing module before creating a
  file or abstraction; ownership does not require a separate file.
- Split when responsibilities, dependencies, reuse, readability or testability
  justify a useful boundary. A command, function name or test alone does not
  require its own module. Avoid arbitrary file-size limits or mandatory patterns;
  explain a non-obvious structural choice when it helps the next maintainer.
- Use names that describe responsibilities. Avoid near-synonyms that imply
  separate subsystems where there is only one. Keep current directory maps in
  component READMEs; those maps describe the code and may evolve with it.
- Keep `main` for startup and wiring, and interface handlers for parsing input,
  submitting operations and presenting results. Keep hardware access, conversion
  and protocol encoding in their owning modules rather than duplicating them.
- Reuse repository libraries when their behavior and dependencies fit. Do not
  import an unrelated subsystem merely to reuse a small helper.
- Keep public APIs small and dependencies explicit. Make required inputs and
  ownership clear without forcing callers to inspect unrelated globals. Keep
  implementation helpers private and avoid broad internal headers.
- When moving code, update callers, build lists, tests and authoritative docs;
  remove obsolete files and wrappers instead of leaving parallel implementations.

## Write understandable code

- Follow existing local conventions for language, formatting, naming and error
  handling before introducing new ones. Prefer readable control flow over dense
  expressions, macro tricks or clever shortcuts.
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
- Avoid leaving dead code or unfinished scaffolding.

## Document purpose without clutter

- Aim for an experienced programmer new to this code to understand what a file
  or substantial function does, where it fits and why its non-obvious choices
  exist without needing the original conversation.
- Start source and header files with a short description of their purpose and
  responsibility, usually one to three lines. Mention an important ownership or
  dependency boundary when it helps explain the design. Do not merely restate the
  filename or include a full change history.
- Add a brief docstring or documentation comment for substantial, public or
  non-obvious functions. Explain the operation's purpose and any relevant caller
  requirements, units, side effects, ownership, completion/error semantics or
  cancellation behavior. Document only the details a caller or maintainer needs;
  do not require a template or parameter-by-parameter narration for every helper.
- Explain surprising implementation choices near the relevant code: why an
  ordering, constant, workaround or state transition is required. Include the
  supporting device/manual reference when applicable. Prefer explaining intent
  and constraints over translating statements into English.
- Keep names and structure as the first explanation. Skip comments for obvious
  getters, assignments, loops and self-explanatory helpers. Put shared API
  contracts at their declarations and implementation rationale at the definition;
  do not duplicate the same description in both places.
- Keep comments accurate when behavior changes. Apply this standard to code being
  added or meaningfully changed; avoid an unrelated repository-wide comment pass
  unless that is the task. A description must match the actual implementation,
  not an intended future behavior.

## Respect component boundaries

- Read the relevant component source map for current ownership and dependencies.
  Preserve useful boundaries without treating the current file layout as fixed.
- Shared DAQ messages belong in `protocol/app_protocol.h`. A protocol change
  needs corresponding consumers, metadata, decoding and documentation updates.
- Keep the eCVT project standalone until DAQ integration is explicitly requested.
  Do not reconnect legacy automatic control, sensor inputs, replay or SD logging
  as part of a refactor. Web control, tuning and scripts are separate development
  steps; do not claim that they exist before implementing them.
- The eCVT actuator is one servo motor controlled through its servo drive. The
  ESP32 communicates with that drive; `motor` and `servo` are not separate
  hardware subsystems. Make that relationship clear in naming and documentation.
- Preserve the single motor-operation owner. New interfaces submit requests;
  they must not issue concurrent drive transactions around that owner.
- Preserve behavior during structural refactors. When the task calls for a
  behavior change, implement it explicitly, describe its effect separately from
  file moves and validate the affected behavior. In particular, retain zero
  target before velocity-mode enable and the unconditional stop/0000 path during
  cleanup; do not introduce mode/velocity prerequisites to disable.

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
