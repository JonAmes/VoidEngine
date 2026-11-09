# Phase 4 — Notes

Running design record for the arcade flight & gravity core. Process rules live in AGENTS.md; task state lives in `checklist.md`.

## Where Phase 3 left off (inputs)
- `core/math` (header-only): fp32 `vec*`/`mat4f`/`quatf` + fp64 `dvec3`; conventions pinned by tests — RH, Y-up, −Z fwd (#34), column-major mats, `wrap_angle`, approximate-equal semantics; `quat::integrate_angular` (fp32); `coords.h` floating-origin recenter; off-axis projection builders (gfx-side consumers, not needed here).
- Everything gfx loads by content ID through the manifest — Phase 4 needs **no** cooked inputs: bodies/ships are hand-authored C++ structs in tests (pure Tier 1; world-def TOML schema rides the first phase that actually cooks a world def).
- `ctest` gate: `./tools/ci_local.sh`, ASan+UBSan on, no `$DISPLAY`. All Phase 4 suites are Tier 1 CPU-only.

## Known gaps this phase closes
- `core/` has zero sim code — `core/sim/` (include/`ve/core/sim/`) opens here: rails, well field, home-well/crossfade, rigid body, integrator, replay hash, assists, ghost sim.
- No test coverage on fp64 attitude math (`quat<T>`/`mat4<T>` templates give `dquat`/`dmat4` but Phase 1 never pinned them) → task 4.2.
- No command/replay/hash machinery → 4.7; Phase 6's scrubber and Phase 8's warp determinism both consume its shape.

## Conventions this phase sets
- Sim time = integer tick count; dt = 1/60 s canonical (Q1). No float-seconds state, no wall clock anywhere in `core/sim`.
- fp64 for all sim state (#3); fp32 only at render boundaries (none exist in Phase 4 yet).
- Ordered iteration: `std::vector` / `std::map` in sim logic; unordered containers never feed sim effects (#5).
- Hash = FNV-1a 64 over raw bit patterns (Q5) — stable across rebuilds and platforms (two's-complement IEEE754 assumption stated, pinned by rebuild-determinism test at 4.12).
- "Down" is a query, not a force: the home-well selection decides framing/assists; the accel is the crossfaded field sum. (Keeps "stand on the moon" and free-fall coherent.)

## Findings / gotchas (fill as tasks land)
### 4.2 — fp64 attitude surface (f48f593)
- `quat<T>`/`mat4<T>` were templated from Phase 1, so `dquat`/`dmat4` needed **zero header changes** — the gap was purely test coverage (27 asserts). No fp64-only surprises: template code behaves identically at double; epsilons simply move to the 1e-12..1e-15 band.
- `integrate_angular` normalize-every-step keeps |q|−1 < 1e-12 after 10k steps at 1/60 s (closed-form yaw match at 1e-11) — this bound is the replay-hash stability argument for 4.6/4.7.
- `operator==` on `quat`/doubles is the bitwise-determinism contract used by the pin — 4.7's FNV hash will ride the same raw-bit assumption.
- `rigid_frame<PosT>` ("rig frame") keeps its **fp32** rotation (render/stats width, documented in `quat.h`) — untouched deliberately. Sim attitude lives in the new `core/sim` rigid body (4.6) as `dquat`; conversion to `drig`/fp32 happens only at render boundaries.
- `dmat3` NOT added (#45b diagonal principal-axis inertia: ω̇ = I⁻¹(τ − ω×(Iω)) is pure dvec3 algebra). Full tensors get their own math + tests when Phase 5 derives them from mounted parts.

### 4.3 — kinematic rails open core/sim (d9d618b)
- `sim/time.h`: `tick_to_seconds` **divides by 60.0** instead of multiplying by the non-representable 1/60 — division is correctly rounded, so tick-aligned durations (600 ticks = 10.0 s) are exact and rail phase hits hand-pins at exact tick multiples (t=300 of a 1200-tick period lands quarter-orbit to ~1e-13, not ~1e-6).
- `rail_world` invariant: body id == index, and `add_body` only accepts `parent < id` — parent cycles impossible by construction, lookup is O(chain depth), iteration is vector order (#5 discipline). Every reject is validated BEFORE push_back so ids never shift.
- Rail offset = `cos/sin(phase + tau·elapsed_periods)`, coplanar XZ, +X→−Z (positive about +Y, #34). Sense pinned **across two independent code paths**: rail cos/sin == `rotate(quat_yaw(θ),(R,0,0))` at θ=π/4. No pre-wrapping of the angle — libm argument reduction is exact for finite doubles and skipping the wrap keeps results bit-identical across calls.
- Purity contract pinned as bitwise `==` (same tick twice; interleaved other-tick calls cannot perturb — there is no state). Nested pin: moon world == planet world + moon local is **bit-exact**, since `position()` sums the same two offsets with IEEE-commutative adds.
- Well params (well_radius/surface_g/taper_start) ride `body_def` now with shape sanity at insert; the field math itself is 4.4.

### 4.4 — well field math (bb28a80)
- Profile: constant `surface_g` core → C1 ramp `g = surface_g·(1-u)²(1+2u)`, u=(r−taper)/(well−taper) (≡ 1−smoothstep; factored form avoids cancellation near the edge); exactly 0 at/outside the edge. The quadratic-in-(1−u) edge falloff is what makes the finite-difference slope →0 at the boundary: crossing the well edge is force-continuous, so no teleport-assist needs and no integrator artifacts at the shell.
- `well_accel_at` returns the **exact zero vector** outside/at-center (early return on g==0) — a scaled `0·ĝ` would leak −0.0 and NaN (r==0) into replay hashes; the zero-guard is a hash-stability requirement, not cosmetics.
- Rail sampling is an identity, not a second path: `well_accel(world,id,pos,t) == well_accel_at(def, position(id,t), pos)` bitwise, pinned.
- Test gotcha (fixed before landing): C1 finite-difference pins need small δ — a ±1 m central diff across the taper join measures ~3.7e-6, which looks like a kink but is the honest slope of a cubic 1 m off the join. Use δ ~ 1e-3·range/1000 (δ=1e-3 m pins ~1.5e-8 there) and pin one mid-ramp slope analytically (central diff of a cubic = exact derivative at midpoint) to prove the sampler isn't just always-small.
- Hand-authored well bodies everywhere (#45d): `add_body` sanity (well>taper>surface, all finite) front-loads def validity so field math stays branch-light.

## Open threads for later phases (do NOT build here)
- Safe-speed bubbles + warp multiplier (#22) → Phase 8, rides this well math.
- Swept contacts → Phase 7 (user ruling 2026-11-09, decision #45e; PLAN corrected).
- Thrust/torque numbers as attribute keys → Phase 5/6 declare them; Phase 4 keeps hand-authored gains (Q6).
