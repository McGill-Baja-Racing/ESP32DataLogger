# Your first contribution: clarify an instruction

This walkthrough takes a useful documentation change from a question to a draft pull request (PR). No board or C knowledge is needed. Start with [Getting started](GETTING_STARTED.md); the general [contribution guide](../CONTRIBUTING.md) covers other task types.

Before starting, ask the **Electronics Lead** to grant your GitHub account access to the team repository and confirm that access.

## 1. Find a real point of confusion

While following setup, note one instruction that made you pause. Record what you expected, what happened, and the sentence you would have needed. For example, an instruction might name a folder without explaining how to find it. This is an example of a task, not a request to change a sentence that is already clear.

Check the team's [issue board](https://github.com/orgs/McGill-Baja-Racing/projects/4/views/1) for the same problem. Link the existing issue or create one describing the confusing instruction and proposed clarification. Ask a teammate to confirm the intended behavior if you cannot verify it yourself. Your goal is one instruction that a future reader can follow without guessing.

## 2. Prepare your branch

Complete [Git identity and access setup](../CONTRIBUTING.md#prepare-git-and-repository-access), then follow [Work on a branch](../CONTRIBUTING.md#work-on-a-branch) in order. Use a name such as `YOUR_GITHUB_USERNAME/clarify-setup`, replacing the placeholder with your GitHub username; branches follow `username/short-task-description`. Confirm `git status` names your task branch before editing.

## 3. Make and check the clarification

Open the affected Markdown file in VS Code and change only the confusing instruction. Explain the action and its expected outcome. Preserve the existing technical meaning; do not guess about wiring, calibration, or firmware behavior. Preview the file using **Markdown: Open Preview** in the Command Palette.

Follow the revised instruction yourself. Open any links you changed and verify that their targets exist. From the repository root, run:

```bash
git diff --check
git diff
```

The first command should print no whitespace errors. The second should show only your intended edit. A wording-only change needs these documentation checks; it does not require flashing a board. If you changed a command, verify that command when feasible and describe any part you could not run.

## 4. Save and publish the change

Use the [stage, commit, and push steps](../CONTRIBUTING.md#validate-and-review-your-changes). Replace the example path with the file you actually edited, such as `docs/GETTING_STARTED.md`, and use a message describing the result, such as `Clarify how to find the workspace file`. Inspect the staged diff before committing. If Git reports an unknown author, return to identity setup; if push is denied, check authentication and repository access.

## 5. Open a draft PR

Follow [Set up the pull request](../CONTRIBUTING.md#set-up-the-pull-request), selecting the team repository's `main` as the base. Review the **Files changed** tab: it should contain only your clarification.

Use a description like this, replacing the bracketed details with your actual observations:

```text
Problem: [Which instruction was confusing, and why?]
Change and system effects: [What can the next reader now do? Is firmware behavior unchanged?]
Test prerequisites and steps: [What does a reviewer need, and what should they do?]
Expected result: [What should they observe?]
Actual result and evidence: [What you followed/checked; attach relevant screenshot or output.]
Limits: [Failed or unrun checks; state if no system test was run for this doc-only change.]
Related to #[actual issue number]
```

Choose **Create draft pull request** and request feedback. Success means your reviewer can see the original problem, the small proposed fix, and evidence for it. Continue with the [review workflow](../CONTRIBUTING.md#prepare-a-reviewable-pull-request); merging requires the team's review and checks. Large changes or changes to key system behavior also require an independent test run by a second person who did not author the change, with results recorded in the PR.
