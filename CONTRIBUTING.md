# Making your first contribution

Use this guide after [Getting started](docs/GETTING_STARTED.md). You can begin without hardware. The [documentation index](docs/README.md) links to deeper references. For a guided documentation-only example, follow [Your first contribution](docs/FIRST_CONTRIBUTION.md).

## Get repository access first

Before starting work, ask the **Electronics Lead** to grant your GitHub account access to the team repository. Confirm access before creating an issue or task branch. All work and pull requests use this repository.

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

An issue records the problem or proposed improvement before implementation. Check the repository's [GitHub Issues](https://github.com/orgs/McGill-Baja-Racing/projects/4/views/1) for an existing report so discussion stays in one place. For a new issue, include a clear title, the current behavior, the desired result, reproduction steps or an example, and relevant logs/screenshots. For a feature, describe who needs it and how you will know it works.

Discuss scope and expected validation with the team, especially for hardware or protocol changes. Link an existing issue if you are taking it on, and comment that you are working on it to avoid duplicated effort. Never include credentials in logs.

## Prepare Git and repository access

Before your first commit, follow [GitHub's Git setup guide](https://docs.github.com/en/get-started/git-basics/set-up-git) to configure your author name and commit email and authenticate with GitHub. Your commit identity and GitHub sign-in are separate settings. Choose an email you are comfortable publishing, including GitHub's private email option.

Confirm that `origin` points to `McGill-Baja-Racing/ESP32DataLogger` with `git remote -v`. If push is denied, check GitHub authentication and ask the Electronics Lead to confirm your access before continuing.

## Work on a branch

Open a [terminal at the repository root](docs/GETTING_STARTED.md#open-a-terminal-at-the-repository-root), then inspect your local copy:

```bash
git status
```

Continue only when the working tree is clean. If changes are listed, preserve them and ask the team if you are unsure which task they belong to; do not discard them to follow this guide. Then update `main` before creating your task branch:

```bash
git switch main
git pull --ff-only
git switch -c YOUR_GITHUB_USERNAME/short-task-description
git status
```

Use **username/short-task-description**, for example `BajaRemy/documentation-onboarding` or `YOUR_GITHUB_USERNAME/add-sensor`. Replace `YOUR_GITHUB_USERNAME` with your GitHub username and use a short hyphen-separated description of your task. The final status should name your new branch and show a clean working tree. If switching or pulling fails, resolve that error with the team before continuing.

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

Replace the example file path and commit message. `origin` must point to the team repository, with access granted by the Electronics Lead. Expect a successful push and an upstream branch to be set. Subsequent commits can be published with `git push`.

## Set up the pull request

On GitHub, open **Pull requests → New pull request**. Select the team repository's `main` as the base and your task branch as the compare branch. Review the file list and commits to ensure the PR contains only your task.

Use a title describing the outcome and link the issue in the description. Use `Closes #ISSUE_NUMBER` only when merging this PR should fully resolve that issue; otherwise use `Related to #ISSUE_NUMBER`. Replace the placeholder with the actual number. Choose **Create draft pull request** while prototyping; mark it ready for review after validation and cleanup.

## Prepare a reviewable pull request

Use the [PR template](.github/pull_request_template.md) to keep the description concise and reproducible:

- Explain the problem, changes, and effects on the system, including affected firmware profiles and interfaces.
- Give test prerequisites, exact steps or commands, and expected results so another person can repeat them.
- Record actual results and attach evidence from the system: screenshots, serial logs, or recorded/exported data as appropriate. A successful build alone does not establish physical operation.
- State failed or unrun checks and remaining limits. For documentation-only changes, report the instructions and links checked and state when no system test was run.

For a large change or one affecting key system behavior (such as acquisition, CAN, recording, or data interpretation), a **second person who did not author the change must follow the test steps independently before merge**. Record their name, tested revision, steps, results, and evidence in the PR. Resolve failures and repeat affected steps after corrections. If unsure whether this applies, agree with the Electronics Lead during scoping.

The repository's [CODEOWNERS](.github/CODEOWNERS) requires review by `@McGill-Baja-Racing/pr-reviewers` for changes. Request review and resolve feedback before merging; opening a PR is not approval to flash vehicle firmware.

After opening the PR, inspect its checks, request the appropriate reviewer, and address feedback with new commits on the same branch. Re-run affected checks after changes and update the description with final behavior and validation. Do not report an unrun or failing check as passing. If hardware validation is still required, identify it explicitly for reviewers.

Merge only after required review/checks and the team's merge policy are satisfied. Verify the linked issue is resolved, update your local `main`, and delete the completed branch through GitHub when it is no longer needed. Do not merge your own PR to bypass required review.

Next: use the [task-based documentation index](docs/README.md) to find the procedure for your change.
