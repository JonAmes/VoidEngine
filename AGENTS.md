# Agent Rules — Void Engine

Rules for any AI agent working in this repo. **PLAN.md is the source of truth for WHAT to build and in what order; this file is the source of truth for HOW work is done.** When they conflict, PLAN.md wins on scope, this file wins on process.

## What this repo is
A from-scratch C++23 game engine + arcade space sim, Linux-only, headless-test-first, VR-capable via OpenXR. Work on the **current phase** in PLAN.md unless the user directs otherwise.

- **Standalone, absolutely.** Do not read, copy, reference, or "draw inspiration from" any other project on this machine or elsewhere — especially anything named "Void Empire" (a Unity project) or OpenKSP. The name collision is meaningless. Only this repo.

## Hard architectural boundaries (violating one = rejected work)
1. `core/` includes **no** gfx, windowing, or XR headers, and **no game concept names** (no "Piloting", "Laser", "reputation" — attributes/modifiers/tags/effects only). Game names live in `game/` or cooked data.
2. XR API calls live **only** in `platform/xr/`. Everything else talks to the Presenter/action interfaces. If `core/` or `gfx/` needs XR data, it arrives through those interfaces.
3. Nothing renders to "the backbuffer." Passes write **render targets**; a Presenter presents. No code may assume one camera, fixed aspect, or a window existing.
4. Sim positions/velocities are **fp64** (`dvec3` etc.). fp32 is for rendering math and stats values only.
5. Sim is **deterministic**: fixed timestep, ordered containers in sim logic (never iterate an unordered map for sim effects), no wall-clock/time-of-day reads in `core/`, no thread-order assumptions. Replay-hash tests are the enforcement.
6. New external dependencies: **never without explicit user approval.** Allowed set (PLAN.md): GLFW, Volk, OpenXR-SDK loader, stb, doctest, miniaudio (#32a), Tracy (approved 2026-11-07). Any cheat-dependency gets a line in `docs/CHEATS.md` with why.

## Build & test contract
- `ctest` must be green with **no `$DISPLAY`, no compositor, no headset**. Never add a test that needs a display; gfx tests use the surfaceless/lavapipe path (Tier 2) and XR logic uses `xr_mock` (Tier 3).
- Every behavior ships with its test **in the same commit**. No "tests later."
- Run `tools/ci_local.sh` (once it exists) before declaring any task done, and report its result.
- Sanitizers (ASan/UBSan) on all test builds; TSan once threading exists. Do not mark green with sanitizer errors.
- Exact build commands are defined in Phase 0 — record them here once known, and keep this section updated.

## Step checklists — `docs/`, one folder per phase (process spine)
Every phase runs through a written checklist. No implementation code for a phase exists before its checklist exists.

**Layout** (created at phase start):
```
docs/
  phase-00-toolchain/
    checklist.md        # the spine — see template below
    notes.md            # design notes, findings, gotchas worth keeping
  phase-01-math/
    ...
```
Folder names: `phase-NN-<short-name>` matching PLAN.md phase numbers.

**Checklist template** (copy exactly):
```markdown
# Phase NN — <name>
Status: not started | in progress | DONE (YYYY-MM-DD)

## Tasks — execute top to bottom, one commit each
- [ ] NN.1 <task> — test: <expected test name(s)> — commit: —
- [ ] NN.2 <task> — test: ... — commit: —

## Done-when (every PLAN.md "Done when" bullet linked to a task; all checked = phase DONE)
- [ ] (a) <bullet> ← NN.x
```

**Rules:**
1. **Before starting a phase:** create its folder + `checklist.md` decomposing the phase into ordered tasks (each = one commit, each names its test up front), and commit the checklist itself. Present it to the user; that decomposition is the phase's first task.
2. **One task in flight at a time.** Check a box only when: the task's test passes, `tools/ci_local.sh` is green, and the commit exists — then fill the box with `[x]` and the commit hash. Only then start the next box. Never batch-check boxes after the fact.
3. If work reveals a missing subtask, **insert it as a new numbered box** before continuing — never silently fold extra work into the current box.
4. A phase is **DONE** only when every box (tasks + Done-when) is checked with a commit hash, the status line says DONE, and the report goes to the user: tasks done, test evidence, deviations from the plan, next-phase note.
5. Checklists are living documents: update status whenever work happens. A user reading `checklist.md` must be able to tell exactly where the project stands without asking.

## Done discipline
- A phase is done **only** when every bullet in its "Done when" list has demonstrated evidence (test names + passing output), not "should work," **and** every box in `docs/phase-NN-*/checklist.md` is checked with its commit hash.
- Do not start the next phase's work without the user's go-ahead. Phase reordering (e.g. jumping to Phase 4 when Vulkan stalls) is the user's call, not the agent's.
- If a design decision is needed that PLAN.md doesn't answer: **stop and ask the user.** After the user decides, record it in PLAN.md tagged `(user decision)` and update any affected later phases' cross-references.

## C++ conventions (defaults — override via PLAN.md/user, don't freelance per-file)
- C++23; compile wall: `-Wall -Wextra -Wpedantic -Werror` — never weaken it to pass a build; fix the code.
- Naming: types `CamelCase`, functions/variables `snake_case`, members trailing `_`, constants `kCamelCase`. Files `snake_case.{h,cpp}`. Headers: `#pragma once`.
- Exceptions: allowed in `tools/` and tests, **not in `core/` or `gfx/` hot paths** — propagate errors with `std::expected` / status types there.
- No RTTI in `core/` (no `dynamic_cast`); component dispatch via tags/indices.
- Hand-rolled only what PLAN.md says hand-rolled (math, ECS, contacts). Do not vendor a library to "just get it working."
- No global mutable state in `core/`; systems get explicit context.
- Comments: explain WHY (derived constants, invariants, determinism requirements). Math in physics/stats cites its derivation in the comment.

## Git discipline — branches: `testing` (working) and `main` (user-approved only)
- No repo yet; `git init` + remote setup happen in Phase 0, **only when the user provides the remote.** Never reuse or point at another project's remote (this machine hosts repos with remotes — do not confuse them; see standalone rule).
- All work commits to **`testing`**. One task per commit; tests in the same commit. Message: `Phase N: <what changed>`.
- Push `testing` to the remote at every phase DONE, and whenever the user asks.
- **`main` is user-gated:** never commit, merge, or push to `main` without the user's explicit request in that session. When asked: merge the approved `testing` state into `main`, strip process docs per the README rule below, push, and report what was promoted.
- No force-push to either branch without explicit permission.

## Version numbering — format `xx.xx.xx` (two digits per part)
- Format: `xx.xx.xx` (major.minor.hotfix), always two digits, e.g. `01.02.03`. Source of truth: a single top-level `VERSION` file, read by CMake — never hardcode the version elsewhere.
- Increments:
  - **Major:** `+01.00.00` — engine milestones / breaking changes to the engine's public interfaces (`core/`, `gfx/`, `platform/` APIs consumed by `game/`).
  - **Minor:** `+00.01.00` — additive features; a phase's deliverables landing counts as a minor.
  - **Hotfix:** `+00.00.01` — bug fixes only, no interface changes.
- Bump on promotion to `main` (the promotion is user-gated, so versioning rides that same gate); tag the promoted commit `vxx.xx.xx`. Bumps come from the user's instruction — agents never decide version numbers on their own.
- Project starts at `00.00.00` (pre-release); the first promotion sets the first real version.

## README.md — the only documentation on `main`
- Keep `README.md` current as work lands on `testing` (commit updates with the work): what the project is, phase/status snapshot, how to build + test.
- **No agent instructions in README** — no process rules, checklists, or AGENTS.md content. It documents the engine for a human reader.
- On `main`, README is the *only* documentation: PLAN.md, AGENTS.md, and `docs/` live on `testing` only and get stripped from the main tree at promotion. README must therefore stand alone on main (overview + current status + build/run/test instructions).

## VR-specific rules
- Cameras are rigs-of-eyes from Phase 1; projection builders must accept off-axis (L/R/T/B tan) parameters even while only symmetric desktop use exists.
- Any new render pass must output: per-eye view list compatibility, a render target, and (once motion vectors start) per-pixel motion — design passes so this stays cheap.

## Working style
- Small, reviewable steps; if a task balloons past one phase-task's scope, stop and split it.
- Report to the user: what was done, test evidence (counts/output), what's blocked, what's next. No claim of "done" without the done-when bullets checked off.
- Update PLAN.md's machine-check / build-command / phase-status notes when facts change — PLAN.md stays accurate as a run-sheet.
