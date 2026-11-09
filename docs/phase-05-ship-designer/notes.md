# Phase 5 — notes

Findings and gotchas as tasks land. One section per task, titled by box number.

## Standing context at phase start (2026-11-09)
- Phase 4 shipped concept-free sim core: 60 Hz integer ticks (#45a), dquat attitude (#45b),
  rails/gravity/wells, rigid integrator (midpoint RK2 angular), replay hashes, assists,
  ghost preview. Diagonal-only inertia was Phase 4 by design — **the full tensor lands in
  5.2/5.3** (#45b: "full-tensor derivation from mounted parts is Phase 5's").
- `game/` exists as an empty skeleton (version.cpp only). Attribute-key strings (thrust.force,
  power.draw, ...) belong there / in cooked data, never in `core/` (AGENTS rule 1).
- Cooklet today: PNG/glTF(TinyGLTF-free hand json parse)/font/shader; manifest content-IDs;
  kCookFormat=3; DEF0 fourcc reserved but no producer yet. **No TOML anywhere in the repo yet**
  — 5.7 introduces the hand-rolled parser (no new deps allowed).
- PoC fixture `docs/tests/blender/test_ship.glb` (decision #14 PoC) HAS hp_* empties with
  `extras.socket_type = engine|wingtip` — PoC-era naming; decision #21 ratified
  **main/rcs/gun/utility**, so 5.6 re-authors the fixture into tests/cook/fixtures/.
- No .blend->CI dependency: fixtures are committed blobs; Blender runs offline only
  (docs/tests/blender/make_test_ship.py regenerates when art changes; PoC proved
  byte-identical exports).

### 5.1 — loadout data model (00304b1)
- ONE build model (#24a): Loadout = hull_info + socket table + validated mounts; stations get it for free by being big hulls (5.11 smoke proves it, not a type hierarchy).
- Concept-free split is executable: type ids/attr keys are opaque std::strings here; category match is pure enum equality; the enum values (main_/rcs_/gun_/utility_) are hull STRUCTURE (#21), affordances live game-side.
- assemble() is total + FIXED check order -> deterministic loadout_error for multi-broken input (cooklet reports per-entry; runtime reports first violation, enough to reject).
- Loadout is immutable: mount/unmount builds a NEW one. Cheap at tens of entries; keeps sim-hash identity simple (no delta semantics to replay).
- Mount attr values are double (feed fp64 inertia/thrust 5.2/5.5); #42 suffix coercion happens at COOK (5.7), loadout only ever sees SI.
- Test hygiene trap hit: expected::error() on a value-holding expected is UB — tests use err_of() which REQUIREs failure first, so a validation rule that regresses fails as an assertion, not silent UB.

### 5.2 — full-tensor inertia (879b24d)
- mat3 follows the Phase-1 mat4 contract: default-construct = IDENTITY. Footgun found while testing: mass_point{...,{}} gives an IDENTITY local tensor, not zero - point masses must pass dmat3::zero(). Kept the convention (matches mat4) rather than special-casing mat3.
- Jacobi: fixed 12 sweeps x fixed pair order -> bitwise-deterministic diagonalization (pinned: same input twice, bitwise). Ascending eigenvalues + largest-|component| positive axes + RH fixup (flip axis of SMALLEST principal value if det<0) - all choices fixed, no data-dependent branching beyond zero-pivot skip.
- Real bug caught: the eigenvector accumulator V must receive the rotation for ALL rows including p/q (the A update excludes those rows - easy to copy wrong). Reconstruction pin (axes*diag*axes^T == input) is what caught it; an eigenvalue-only test would have passed with garbage axes.
- Translation invariance of the tensor is exact-arithmetic only: fp rounding of com differs per origin, so compare with tight Approx, never bitwise.

### 5.3 — rigid body tensor path (2f4a2e1)
- Tensor branch replaces (not refines) the diagonal path: when inertia_tensor is set, inertia_diag is UNUSED. Diagonal arithmetic stays bitwise-frozen for every Phase-4 hash pin; adding `= std::nullopt` default keeps Phase-4 aggregate brace-init compiling under -Werror=missing-field-initializers (no call-site churn).
- Closed-form anchor that makes the tensor branch trustworthy: torque-free axisymmetric top with the symmetry axis TILTED 30 deg in body axes -> body-frame omega precesses about the symmetry axis at Omega = w3*(I3-I1)/I1, Rodrigues closed form (tilting about the SYMMETRY axis itself would be a no-op tensor - test authoring trap).
- Positive-definiteness gate = Sylvester leading principal minors (m00>0, 2x2 minor>0, det>0). Non-symmetric tensors are NOT rejected (physics would change); authorship guarantee: only 5.2's parallel-axis output feeds the tensor, and that is exactly symmetric.
- I^-1 recomputed per integrate call (adjugate, 9 mults; tens of bodies) - caching it would add a staleness class for no measurable win. Revisit only if profiling says otherwise.
