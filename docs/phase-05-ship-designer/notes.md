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


## 5.9b — cooklet cross-validation gates (4f60996, CI GREEN 93/10659)

- New module `tools/cook/{include/ve/cook/,src/}hullvalidate.{h,cpp}`: byte-faithful DEF0-v2 / SOCK-v2
  decoders + SOCK re-encoder + `cook_hull_validate` (fixed gate order -> deterministic first error).
- Gate list (order fixed): face required on door.*/airlock.* (#46c1) -> comp socket names anchor ->
  #46a interior-empty 1:1 pairing (name==comp id) -> #46b snap/rewrite (cell lattice for kind 0/1,
  SUBCELL lattice for furnishings; >tolerance = cook fail; snapped coords REWRITTEN into the SOCK unit,
  re-written only when the bytes change) -> #44 anchor in exactly one section -> #46c/c1 facing
  (door: neighbor must be a DIFFERENT section; door into space = fail "declare it airlock"; airlock:
  must face NO section) -> #26a wire endpoints -> #28a facet target names a wire -> #46d seams
  (partner cooked, socket hardpoint on both sides, seam cell on the EXTREME (not enclosed by sections),
  grid compatible = integer-multiple cell sizes, symmetric back-declaration).
- cook.cpp trigger: gates run post-cook for every hull whose def OR input units were (re)cooked this
  run, in manifest (content-ID) order; reused untouched hulls keep last-run verdict.
- GOTCHA (caught by e2e): plain passthrough defs are ALSO `kUnitDef0` unit files (raw TOML bytes in
  payload, 5.8 design). The hull scan must PEEK payload_ver==2 (first 4 LE bytes) or every non-hull
  def fails the cook with "DEF0 payload version must be 2".
- #28a action tags: hulldef schema gate rejects tags outside `game/include/ve/game/facet_actions.h`
  (closed 12-tag registry; grammar via is_valid_attr_key, membership separate). DEF0 component rows
  gained `u8 face` (0 none, 1 +x, 2 -x, 3 +y, 4 -y, 5 +z, 6 -z) -> DEF0 golden re-pinned (+1B/comp);
  SOCK payload unchanged.
- Test footguns: hull anchors must sit on CELL-CORNER multiples (floor(pos/cell) membership), not cell
  centers; Wire aggregate order is (id,type,from_kind,to_kind,from_ref,to_ref) — kind 1=component.
  must_fail helper logs `why + " -> " + err`: match the REAL err substring ("facet-action registry").
- Next: 5.10 runtime load path (loadout_load.h).
## 5.10 — runtime load path (990a18e, CI GREEN 95/10719)

- `core/sim/loadout_load.h`: `load_hull_loadout(def0_unit, sock_unit, mesh_content_id)
  -> std::expected<loaded_hull, std::string>`. The caller resolves refs -> content
  ids (manifest job); the loader never opens files. `loaded_hull` carries the
  assembled `Loadout` + the MESH unit id string (render-side follow).
- Design: **every named anchor becomes a loadout socket** (hardpoints AND
  interior empties, kind byte not interpreted). Components mount uniformly onto
  anchors -> #46a pairing is total (door_1 component -> door_1 anchor works with
  plain category equality). Kind prefixes are authoring-time cook conventions
  only; the loader checks no name grammar (concept-free).
- Loader doctrine: own bounds-checked little-endian Reader (cooklet writers are
  private to cooked.cpp); decode order byte-identical to hulldef emit; first
  violation latches; deterministic error strings ("DEF0 payload: ...", "SOCK
  payload: ...", "loadout: <assemble_kind>"). Version pins (DEF0 v2, SOCK v2),
  truncation, trailing bytes, category byte range all gated. face/wires/facets/
  seams decoded-and-skipped (interior graph 5.11+, stats Phase 6, seams later).
- Assemble stays the runtime second line of defense (cook gates are author-time).
- `invalid_hull_mass` gate is `!(mass >= 0.0)` -> NaN/negative only; mass 0 is
  deliberately legal (dry-mass-zero hulls), so the gate test uses mass = -1.
- GOTCHA (pinned in commit msg): `std::string pat = "\x09\x00...hp_engine"`
  silently has size 1 — the const char* ctor stops at the first embedded NUL.
  Byte patterns with NULs must be range-constructed (mklen() helper in the test).
  Also: C++ hex escapes are greedy — `\x00eng_1` parses as `\x00e` + "ng_1";
  split literals (`"\x00" "eng_1"`) or build from Bytes.

## 5.11 — seats + occupancy (7a469c7, CI GREEN 97/10785)

- **Seat = tagged mount, core never knows it.** `Loadout` now carries opaque
  `tags` per mount (`mount_request`/`mounted_component`); `loadout_load`
  preserves DEF0 tag rows verbatim in def order. `Occupancy` (new
  `core/sim/occupancy.h/cpp`) seeds a seat for every mount whose tags
  intersect the *caller-supplied* role registry — the roster itself lives in
  `game/role_tags.h` (#18c five stations), so AGENTS rule 1 holds.
- **Set semantics:** duplicate tag in one mount = authoring bug →
  `loadout_error::duplicate_tag` (assemble-time, after duplicate_attr_key).
- **Atomicity + determinism:** `sit()` MOVES an already-seated occupant in
  one call (detach+attach), `seat_busy` refuses without touching anything;
  seat order = mount order, role_tags order = mount tag order (#5). Errors:
  unknown_seat > seat_busy/not_seated first-cause order.
- **#18d dark-not-dead:** `set_dark(seat, bool)` is the health transition
  stand-in — it NEVER changes occupancy, only availability. `manned(role)` =
  first non-dark seated seat carrying the role; gunner-KO'd → gun silent
  with the body still in the chair.
- **#18a seam to Phase 6:** sitting/standing/dark are the ONLY signals;
  attaching the StatsSet modifier stack "as a unit" subscribes to these
  transitions. No stats machinery in core/sim/occupancy.
- **#24a smoke:** station loadout (dock.port x2 + counter.market + trader
  seat) assembles like any hull; `trade.counter` is a *facet* tag, not a
  role, so the counter is not a seat — only `station.trader` is.
- **Gotcha:** adding a vector field to an aggregate struct trips
  `-Werror=missing-field-initializers` in EVERY positional brace-init of it
  (same lesson as 5.3 `optional inertia_tensor`); test_loadout.cpp needed
  `, {}` on ~14 mount inits.

## 5.12 — cargo hold + manifest (632a9f1, CI GREEN 98/10848)

- **Holds are ordinary tagged mounts** — same pattern as 5.11 seats:
  `CargoManifest(loadout, hold_tag, capacity_key, gate_prefix)`; all three
  names arrive from `game/cargo_tags.h` (`cargo.hold`, `commodity.` prefix)
  + `kAttr_cargo_capacity` (already ratified in the 5.4 registry). Commodity
  ids are the gate tags' suffixes — opaque cooked market data (#20b).
- **Capacity is per-hold, first-fit in mount order:** add splits across
  accepting holds (bulk→tank→open order), remove drains placements in
  REVERSE mount order, so add/remove pairs cancel exactly at any tick —
  scrubber-safe (#20b "price evaluated at any tick" depends on this).
- **Gate rules pinned:** no gate tags = open hold accepts everything; a
  commodity no hold gates is `no_gate`, distinct from `overflow`; a non-gate
  tag (`repair.target`) on a hold mount is simply ignored.
- **Atomicity:** overflow/insufficient store nothing; errors are
  first-cause ordered (quantity/id → gate → capacity).
- **Spares (#20a/#24c):** roster entries only (instance+type id, unique id,
  ordered); they never occupy commodity capacity; bolting one on later goes
  through the normal `Loadout::assemble` mount path, not this module.
- Interpretation logged: cargo MASS aggregation into the ship is Phase 6
  (component mass flows into stats aggregation like every other component).

## 5.12a — mass_point identity-default trap (74ea8c1, CI GREEN 100/10941)

`math::mat3`'s default constructor is the **identity** (right for transforms).
`mass_point::local_tensor{}` and `ship_inertia::tensor{}` value-init through
it, so every default-constructed part silently carried 1 kg*m^2 per diagonal
while the header comment promised "zero matrix = point mass". The 5.13
harness hit it as a purely isotropic +4.0 tensor offset (four point-mass
mounts), which is what made it findable: off-diagonals and mass/com stayed
exact. Defaults are now `= math::dmat3::zero()` with trap comments. If a
future struct pairs a concept-y zero default with mat3, spell the NSDMI —
`T x{}` on mat3 means identity, always.

## 5.13 — ship_designer_flight done-when harness (03f5ef4, CI GREEN 100/10941)

Whole Phase 5 chain in one headless test: `cook_incremental` fixture copies ->
DEF0+SOCK -> `load_hull_loadout` (mesh identity + 12 anchors as sockets) ->
designer `Loadout` (hull + 2 engines + 2 wings) assembled **on loaded socket
anchors** -> full-tensor mass properties -> thruster torque -> derived-gain
stabilizer flight -> replay-hash determinism.

Pins are **exact rationals** pre-computed (Python Fraction) from the
lattice-exact fixture anchors, so the test proves the engine's arithmetic,
not self-consistency: mass 110000; com (0, 49/220, 38/275); tensor diag
(6360726/11, 15857376/11, 15671350/11) with yz couple -70560/11 as the only
off-diagonal (symmetric-x layout cancels xy/xz exactly — a real regression
tripwire: an x-asymmetry would light those up); principal axes
(578247.818, 1422510.44, 1443737.38) from the yz 2x2 quadratic.

Flight: empty `rail_world` (no gravity). Spin decay through the replay_step
assist path: w_after_spin = tau*(I^-1)_yy*decay_ticks exactly; after 90 more
ticks w < 1e-8, drift 0.0836 rad matches the first-order
w0/(kd/I)=w0*I/(5*I)=0.2*I envelope. Burn: quasi-steady w = T/kd
(0.0612 pinned, |wy|,|wz| < 0.05); contrast run WITHOUT assist veers to
w=0.308 with 2.4x lateral velocity — that contrast IS the done-when claim
("assist gains scale with derived inertia"). Determinism: per-tick FNV state
hash, 690-tick interval hash pin-free (byte identity across reruns), 1-ULP
command mutation canary diverges.

Gotchas hit: doctest `CHECK(a && b)` hides which side failed — split; and
`grep`-filtered test runs mask exit status — check the doctest summary line
explicitly before calling anything green.
