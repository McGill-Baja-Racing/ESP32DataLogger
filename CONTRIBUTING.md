# Making your first contribution

Use this guide after [Getting started](docs/GETTING_STARTED.md). You can begin without hardware. The [documentation index](docs/README.md) links to deeper references.

## Choose a focused task

Ask the team which current issue needs help, or propose a small improvement with a clear expected result. Read the relevant component introduction before changing code.

| Starting area | Example task | Read next | Initial validation |
|---|---|---|---|
| Documentation | Clarify a confusing setup instruction | [Getting started](docs/GETTING_STARTED.md) | Follow the instruction and check links |
| Browser interface | Improve a label or graph interaction | [Web source guide](master-node/src/web/README.md) | Master build; browser behavior needs a board |
| Parsing or exports | Add a case for malformed GPS input or missing data | [Master source guide](master-node/src/README.md) | Relevant host checks and master build |
| Sensors | Correct conversion or extend a node | [Sensor driver guide](sensor-node/src/sensors/README.md) | Affected builds, then bench validation |

Hardware-free work is useful, but passing a build does not prove wiring, timing, or calibration.

## Create or pick an issue

An issue records the problem or proposed improvement before implementation. Check the repository's GitHub Issues for an existing report so discussion stays in one place. For a new issue, include a clear title, the current behavior, the desired result, reproduction steps or an example, and relevant logs/screenshots. For a feature, describe who needs it and how you will know it works.

Discuss scope and expected validation with the team, especially for hardware or protocol changes. Link an existing issue if you are taking it on, and comment that you are working on it to avoid duplicated effort. Never include credentials in logs.

## Work on a branch

From the repository root, inspect the current checkout before starting:

```bash
git status
git switch -c YOUR_GITHUB_USERNAME/short-task-description
```

Use the naming convention **GitHub username / descriptive branch title**, for example `BajaRemy/documentation-onboarding`. Replace `YOUR_GITHUB_USERNAME` with your username and use a short hyphen-separated title. Start new work from an up-to-date `main`; when your checkout is clean, use `git switch main` and `git pull --ff-only` before creating the branch. Preserve any work already in progress before switching.

The branch isolates your work from the shared main branch. Keep unrelated changes out of the task; do not discard files you did not create. A normal Git commit records a local change, while a pull request proposes it for team review.

## Follow the code's ownership

Keep hardware conversion in its sensor driver, sampling in the sampler, CAN transport in its CAN module, and master recording policy in `app_control`. `main.c` connects these modules. See the [master](master-node/src/README.md) and [sensor](sensor-node/src/README.md) source maps.

The projects have separate copies of their protocol header. A protocol change must keep both compatible and update consumers, signal metadata, decoding, and documentation. Read [Protocol](docs/PROTOCOL.md) before allocating IDs or changing units.

Edit intentional SDK defaults/build flags rather than committing generated `sdkconfig.<environment>` files. Avoid build artifacts, local port settings, and unrelated dependency updates. Documentation changes should describe behavior that exists; label assumptions requiring hardware validation explicitly.

## Prototype a small change

Define one observable result from the issue and implement the smallest change that demonstrates it. Use the hardware-free build and host-test routes where possible, then validate on the bench when the change depends on physical inputs. Capture observations and uncertainties rather than assuming compilation proves operation.

A draft PR is useful for early feedback. Explain what the prototype demonstrates and what remains before it is ready. Temporary test modes and exploratory changes must be removed or made intentional before final review.

## Validate and review your changes

Use [Testing](docs/TESTING.md) to select checks appropriate to the change. Review your diff from the repository root:

```bash
git diff --check
git diff
```

Expect no whitespace errors and only intended changes. Update the authoritative guide when behavior changes; link to it from introductions rather than repeating specifications.

From the repository root, stage the specific files you changed, inspect the staged diff, then commit and publish the branch:

```bash
git add path/to/changed-file.md
git diff --cached --check
git diff --cached
git commit -m "Describe the resulting change"
git push -u origin HEAD
```

Replace the example file path and commit message. `origin` must point to a repository you can push to; contributors without write access should push to their fork and open a PR into the team repository. Expect a successful push and an upstream branch to be set. Subsequent commits can be published with `git push`.

## Set up the pull request

On GitHub, open **Pull requests → New pull request**. Select the team repository's `main` as the base and your username-prefixed branch as the compare branch. For a fork, select your fork as the head repository. Review the file list and commits to ensure the PR contains only your task.

Use a title describing the outcome and link the issue in the description. Use `Closes #ISSUE_NUMBER` only when merging this PR should fully resolve that issue; otherwise use `Related to #ISSUE_NUMBER`. Replace the placeholder with the actual number. Choose **Create draft pull request** while prototyping; mark it ready for review after validation and cleanup.

## Prepare a reviewable pull request

Describe the problem, resulting behavior, checks you ran, and any hardware checks still outstanding. Include screenshots for visible interface changes when available, or a sample input/output for data changes. Mention affected firmware profiles and any protocol or hardware assumptions.

The repository's [CODEOWNERS](.github/CODEOWNERS) requires review by `@McGill-Baja-Racing/pr-reviewers` for changes. Request review and resolve feedback before merging; opening a PR is not approval to flash vehicle firmware.

After opening the PR, inspect its checks, request the appropriate reviewer, and address feedback with new commits on the same branch. Re-run affected checks after changes and update the description with final behavior and validation. Do not report an unrun or failing check as passing. If hardware validation is still required, identify it explicitly for reviewers.

Merge only after required review/checks and the team's merge policy are satisfied. Verify the linked issue is resolved, update your local `main`, and delete the completed branch through GitHub when it is no longer needed. Do not merge your own PR to bypass required review.

Next: use the [task-based documentation index](docs/README.md) to find the procedure for your change.
