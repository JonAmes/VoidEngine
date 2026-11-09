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
