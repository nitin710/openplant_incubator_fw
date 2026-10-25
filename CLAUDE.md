# CLAUDE.md

Firmware for the OpenPlant plant incubator (ESP32 local control, Raspberry Pi gateway). See `README.md` for scope and architecture.

GitHub repo: `nitin710/openplant_incubator_fw`, default branch `main`.

## Workflow for every problem

Every change, whether a bug fix, feature or docs update, follows these steps. Don't commit directly to `main`.

1. **Create an issue** that describes the problem or task:
   ```sh
   gh issue create --title "<short summary>" --body "<problem, expected behavior, acceptance criteria>"
   ```
   Note the issue number (`#N`). If an issue already exists, use it instead of opening a duplicate.

2. **Create a PR** from a branch dedicated to that issue:
   ```sh
   git checkout -b <type>/<N>-<short-slug>      # e.g. fix/12-cooler-on-sensor-fail
   # make changes, commit
   git push -u origin HEAD
   gh pr create --title "<summary>" --body "<what changed and why>"
   ```

3. **Link the issue(s) in the PR.** Put a closing keyword in the PR description, one line per issue:
   ```
   Closes #N
   ```
   Use `Closes`, `Fixes` or `Resolves`. A bare `#N` mention does not close the issue.

4. **Merge the PR**, which closes the linked issues:
   ```sh
   gh pr merge <PR#> --squash --delete-branch
   ```
   Afterwards, confirm the issues are closed (`gh issue view N`).
