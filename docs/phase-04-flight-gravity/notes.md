# Phase 4 — Notes

Running design record for the arcade flight & gravity core. Process rules live in AGENTS.md; task state lives in `checklist.md`.

## Where Phase 3 left off (inputs)
- `core/math` (header-only): fp32 `vec*`/`mat4f`/`quatf` + fp64 `dvec3`; conventions pinned by tests — RH, Y-up, −Z fwd (#34), column-major mats, `wrap_angle`, approximate-equal semantics; `quat::integrate_angular` (fp32); `coords.h` floating-origin recenter; off-axis projection builders (gfx-side consumers, not needed here).
- Everything gfx loads by content ID through the manifest — Phase 4 needs **no** cooked inputs: bodies/ships are hand-authored C++ structs in tests (pure Tier 1; world-def TOML schema rides the first phase that actually cooks a world def).
- `ctest` gate: `./tools/ci_local.sh`, ASan+UBSan on, no `$DISPLAY`. All Phase 4 suites are Tier 1 CPU-only.

## Known gaps this phase closes
- `core/` has zero sim code — `core/sim/` (include/`ve/core/sim/`) opens here: rails, well field, home-well/crossfade, rigid body, integrator, replay hash, assists, ghost sim.
- No fp64 attitude math (`quatf` is fp32 only) → task 4.2.
- No command/replay/hash machinery → 4.7; Phase 6's scrubber and Phase 8's warp determinism both consume its shape.

## Conventions this phase sets
- Sim time = integer tick count; dt = 1/60 s canonical (Q1). No float-seconds state, no wall clock anywhere in `core/sim`.
- fp64 for all sim state (#3); fp32 only at render boundaries (none exist in Phase 4 yet).
- Ordered iteration: `std::vector` / `std::map` in sim logic; unordered containers never feed sim effects (#5).
- Hash = FNV-1a 64 over raw bit patterns (Q5) — stable across rebuilds and platforms (two's-complement IEEE754 assumption stated, pinned by rebuild-determinism test at 4.12).
- "Down" is a query, not a force: the home-well selection decides framing/assists; the accel is the crossfaded field sum. (Keeps "stand on the moon" and free-fall coherent.)

## Findings / gotchas (fill as tasks land)
- (pending 4.2+)

## Open threads for later phases (do NOT build here)
- Safe-speed bubbles + warp multiplier (#22) → Phase 8, rides this well math.
- Swept contacts → Phase 7 per assumption (PLAN contradiction logged in `checklist.md`; awaiting user ruling).
- Thrust/torque numbers as attribute keys → Phase 5/6 declare them; Phase 4 keeps hand-authored gains (Q6).
