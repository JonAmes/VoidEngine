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

### 5.4 — attribute keys + #42 units (153674c)
- Keys live in game/ (AGENTS rule 1); core keeps treating them as opaque. The kPhase5_attr_keys array is the registry: adding a key without registering it fails the completeness check; duplicates/uniqueness caught in-suite.
- units.h is the ONLY unit-math home outside the cooklet itself (cooklet will call it in 5.7). Suffix set is exactly #42-ratified and case-sensitive: m km AU s day rad deg deg/day. Bare suffix ("") is its own error kind so cook errors can distinguish 'forgot the unit' from 'invented a unit'.
- AU pinned to the IAU-2012 nominal value 149597870700 m (exact integer in double).
- engine_tests already linked ve_game (since early gfx-era CMake), so header-only game constants needed zero link changes - test registered, include dir came free.

### 5.5 — thruster force-at-point (903e72d)
- Concept-free split: the attr-key READING (`thrust.force` -> max_force, type-id -> axis policy, gimbal cone sizing) is game-side glue; core thruster.h never names a component. 5.13 does the glue inline in the harness (game/ module for it can land with Phase 6).
- thruster.main vs thruster.rcs = SAME machinery. RCS translation + torque couple are just summed point forces; no pod-cluster abstraction needed yet (pods authored as individual thruster_statics).
- Gimbal semantics pinned: (pitch,yaw) ANGLE pair scaled into the cone FIRST, then fixed-order pitch-about-mount-X then yaw-about-mount-Y. Composition of two axes makes the composed angle match the cone half-angle only to O(theta^3) (pin loosened accordingly for the 2-axis case); single-axis deflection is exact.
- limit==0 means FIXED MOUNT and the gimbal input is ignored (not 'cone of zero angle that would deflect by whatever' - first implementation got this wrong, test caught it).
- applied-force record skips zero-throttle and degenerate-axis thrusters; impulse = force_local * kSim_tick_dt at drain time (Phase 6).

### 5.6 — cooklet hardpoints (4ffb7ea)

- One .glb source now yields TWO cooked units: MESH (unchanged, socket
  records stay verbatim/debug) + SOCK unit id `<glb id>.sock` (authoritative
  hardpoint table: mesh content-ID link, ratified u8 cats main|rcs|gun|
  utility, fp64 TRS-baked world transforms, node-index order). Cook format
  v4. Manifest edge sock -> mesh = load closure pulls the mesh for free.
- Cook GATE: missing/unknown socket_type fails loudly (byte-patched-fixture
  tests prove both variants). #21 categories ratified at the cook boundary,
  not at runtime.
- Golden: SOCK payload bytes pinned (tests/cook/fixtures/test_ship.sock.
  golden, VE_GOLDEN_UPDATE=1 regenerates — gfx goldens convention reused).
- gotcha (fixture surgery): when splicing a JSON chunk into a GLB in python,
  the BIN chunk slice must START AT the old 8-byte BIN header — counting the
  header twice silently corrupts every accessor past the chunk end
  (symptom: bogus "index 65534 >= vertex_count"). Verify total length ==
  header total_length after rewriting.
- The checked-in fixture was re-authored by direct JSON-chunk edit (no
  Blender here); make_test_ship.py now carries the same SOCKET_TYPES map —
  keep in sync if regenerated.

### 5.7 — TOML subset parser (354249a)

- tools/cook/src/toml.cpp + include/ve/cook/toml.h. Subset documented in the
  header; NOT supported (yet): multi-line strings, raw strings, datetimes,
  inline tables. ve_cooklet now links ve_game (#42 table used at cook time —
  unit math stays out of core/runtime, #42).
- DETERMINISM: tables are std::map -> KEY-ORDER iteration, never insertion
  order. Every def consumer (5.8+) must iterate, not rely on file order.
- si_from_string("5 km") -> 5000.0; bare / unknown-suffix / inf-nan
  (from_chars accepts them for doubles! — isfinite gate) / multi-token all
  hard errors. def numeric-with-unit fields are STRINGS in TOML.
- gotcha: key insertion must target the CURRENT table (last header opened),
  and [[arr]] must repoint current-table to the fresh array entry — first
  cut inserted everything at root (duplicate-key false positives).
- gotcha: local TomlValue shadowing a table pointer compiled into
  `ptr->emplace` on a value — renamed fresh/tbl.

## 5.8 — hull def TOML -> DEF0 (aab8de0)
- Discriminator: top-level `class_tag` in a .toml def => hull def, cooked to
  normalized DEF0 v1; defs WITHOUT class_tag keep raw-bytes passthrough
  (ok/bad.toml tests unaffected). Refs extracted either way for manifest deps.
- DEF0 v1 layout documented in cooked.h; kCookFormat 4->5 (payload semantics).
- #42 scope interpretation (flagged): length/time/angle authored as
  unit-suffixed strings; kg/N/W/dimensionless as bare SI numbers; unknown
  suffixes and bare unit fields are cook errors.
- TOML gotchas found building this: (1) array-table law — after [[a]],
  [a.b]/[[a.c]] must descend into the MOST RECENT a entry (parser fixed +
  regression subcase in cook_toml); (2) dotted keys nest — [component.attrs]
  + `power.draw = 1` creates a `power` SUB-table, so hulldef flattens nested
  tables to dot-joined registry keys; (3) root bare keys must precede ALL
  table headers (parser inserts into last-opened table).
- Section boxes are FLAT 6-int [lo3 hi3] arrays in CELL coords; nested
  [lo,hi] pair form rejected (error pins the shape).
- attrs keys must be in the ratified 14-key registry (5.4). GIMBAL-LIMIT key
  is NOT in that set — deliberately deferred, decide before 5.13 (5.5 gimbal
  inputs are runtime-side, not def-side today).
- Fixture: test_ship.glb +hp_seat_pilot/hp_reactor/hp_battery (all utility);
  SOCK golden + Blender script synced. Sample assets/defs/test_ship.def.toml:
  8 components, 2 sections, 6 wires, seat facet; test stages both fixtures
  under one tmp src tree so the ../samples ref resolves inside the cook set.
- Old parse_def/DefData line-scanner deleted — DEF0 is the only def payload.

## 5.9a — cooklet data plane (6d7e11a, CI GREEN 89/10567)
- **SOCK payload v2 (#46a):** one named-anchor table for *every* mesh-less named node.
  Collection loop classifies by name prefix: `hp_*` kind 0 (socket_type gate still
  strict), `door_`/`seat_`/`panel_`/`counter_`/`berth_` kinds 1-5 (no socket_type;
  category always 3=utility), other named nodes are free-form art (ignored).
  Entry: str name, u8 kind, u8 cat, 3xf64 t, 4xf64 q, node-index order. Duplicate
  names FAIL the cook (`duplicate named anchor '...'`) — name is the #46a pairing key.
- **MESH socket section stays hp-only *and keeps the name string*.** Gotcha hit in
  this task: rewriting the emit loop to the Named struct silently dropped
  `put_str(p, n.name)` from the MESH section; the reader then decoded garbage
  (~1 GB str length) and ASan aborted *before* doctest could report. Layout changes
  in the cooklet must be diffed against the golden-decode readers, not just compiled.
- **DEF0 payload v2:** facet rows gain `u8 has_target [str target]` after `manual`
  (#28a: repair.target names a wire/junction id; existence validated in 5.9b);
  `[[seam]]` rows parsed (partner/socket/optional offset, #46d) and emitted between
  wires and refs — sample def emits 0 seams; 5.9b adds a two-def seam gate. Seam
  partner refs flow into the manifest dep graph via the normal refs pass.
- **#46e:** `thrust.gimbal_rad` joins the game attr-key registry (15 keys). Authored
  `"15 deg"` -> cooked 0.26179938779914945 rad via #42 units, pinned in the decode.
- **Lattice-exact fixture (#44/#46b):** every anchor in test_ship.glb sits on-grid for
  the sample def (cell 2 m x 2.2 m, subcell_ratio 4 -> 0.5/0.55 m subcells, tolerance
  0.1 m) so 5.9b's snap-rewrite is an exact no-op here; off-lattice + beyond-tolerance
  cases live in cook_hull_validate unit strings in 5.9b. Primary anchors snap to cell
  lattice, furnishings (seat/panel/counter) to subcell lattice.
  **glTF authored translations ARE engine-native in the fixture** (HullRoot identity):
  the (x, z, -y) swap happens in the Blender author script (Blender Z-up -> engine
  Y-up), not in the cook. Anchor translations must be authored so the *cooked* value
  lands on lattice; the cook applies the node's TRS-baked world transform.
- **Sample def rewritten** (lattice/pairing): 4 tiling section boxes, 11 components —
  7 hp-mounted + 4 interior-anchor components (`seat_pilot`, `door_1`, `panel_main`,
  `berth_1`) whose socket field is the anchor *name* (pairing key, #46a); panel_main
  carries the `repair.target = "w_engL"` facet. Goldens re-pinned: DEF0 1639->2113 B,
  SOCK 8-entries->12-entries 897 B.
