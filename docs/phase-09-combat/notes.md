# Phase 9 — notes

## 9.3 wire routes (2026-11-16)
- **Cook driver hull peek is payload-version-sensitive.** `tools/cook/src/cook.cpp`
  decides "is this unit a hull?" by peeking the first u32 of the cooked DEF0
  payload (plain-def bytes can never equal it). When DEF0 moved v3->v4 the peek
  still compared 3, so every hull was treated as a plain def: art resolution,
  per-component manifest edges and ALL hull gates were silently skipped and the
  cook still reported green. Any future payload bump MUST touch the two peek
  sites (kept adjacent in one helper pair for this reason).
- **Door anchors ride the 2 m CELL lattice, not half-cell seams.** G4 snaps
  primary anchors (hardpoints + doors) to cell centres within `grid.tolerance`;
  a seam door between cell z=-1 and z=-2 authors its GLB empty AT cell centre
  z=-2 with `face = "-z"`. The seam pair is (anchor cell, anchor cell + face),
  never the geometric wall midpoint.
- **G12 order per wire is pinned:** empty route -> start anchor -> end anchor ->
  adjacency -> lattice membership -> door seam. Membership (G12c) must run
  before seam (G12d): G12d dereferences `sections_containing(cell)[0]` and is
  only safe because G12c already returned on size != 1.
- **Section->wires index (#51b)** is emitted right after the wire table
  (authored section order, ascending wire indices, a wire listed once per
  section ANY of its route cells lies in — re-entry included). It is the query
  site for 9.6 severing; the core reader reads-and-skips it until then.
- **Forms schema mirrors the parser** (def_schema.h header rule): `route`
  needed a new `field_kind::int_cells` (array of int triples; `int_boxes`'s
  regroup-6 write-back re-groups 3 for cells via form char 'Y').
- Mutation discipline paid off again: the route-ends-off-anchor guard survived
  the first mutation round (no test pinned it) — pin added, guard re-proven.

## 9.4 (611bea9) wire tube visuals
- **MESH payload is PLANAR channel blocks** (pos|nrm|uv|tan|tri|idx per
  cooked.h) — an interleaved-vertex emit cooked fine but the decoder read
  garbage mesh #2. Pin a full-consumption CHECK (reader.at == payload.size())
  so any channel-order bug trips immediately.
- Tri count is 2 per side quad: (n-1)*sides*2. A silent x1 corrupts every
  offset after mesh #1, not just counts.
- Geometry determinism: fixed kRingC/kRingS literal table + integer
  dominant-axis frames; no libm anywhere in hulldef tube code — payload bytes
  are bit-identical across machines (replay doctrine).
- tubes_out is opt-in: inline WRLD hull copies pass null (tubes are a
  per-source companion, sock doctrine); manifest edge .wires -> hull def,
  source_sha = hull def sha (edited def re-cooks both together).

## 9.5 (b72589a) loadout editor wire-route edit mode
- **One route law, two entry points.** Grammar (9.5a) + spatial G12a–G12d now
  both live in `ve::game::route_law` (text: `route_text_ok`/`parse_route_text`/
  `route_text_of`; spatial: `route_section`/`route_door_seam`/`route_wire` +
  `route_law_error`). Cook G12 = thin wrapper `route_views_from_def`
  (DEF0+SOCK -> views; endpoint cells via the wire's own component socket
  anchors) + the shared law. Editor refusal strings are byte-identical to
  cook's by construction — pinned editor-vs-cook equal, not by copy.
- **Entry-gated write-back.** Draft goes grammar-then-law; a refusal leaves
  the TOML at zero bytes and shows the cook-shaped error; an accept
  canonicalizes only that wire's `route` line, unchanged wire lines stay
  byte-for-byte (diff-stable files, watch-mode cook stays incremental).
- **doctest `-tc` matches case names EXACTLY** — `cook_hull_validate` does NOT
  select `cook_hull_validate_gates`. A mutation ladder filtered to the base
  case reported four live spatial guards as survivors; they were never
  exercised. Triage that phantom with a stderr call-count probe at the law's
  entry (fewer calls than fixtures = pins aren't running), then list EVERY
  pin-bearing TEST_CASE into the filter.
- **Payload can carry what the grammar cannot**: TOML `route` needs >=1 cell
  but a hand-built DEF0 row can have zero cells — the G12 empty guard is the
  load-bearing check (its mutant, and the membership guard's, segfault the
  test binary = caught, exactly the UB they stop). `nil` pin added.

## 9.6 (a2e41c6) weapon component types + attr registry
- **Registry keys are membership, not consumers.** The six `weapon.*` keys join `kPhase5_attr_keys` (18→24) and the cook attrs gate; nothing sim-side reads them yet — 9.7–9.10 are the consumers. Bare SI numbers per #42 (damage J, range m, rof 1/s, spread rad, heat J, heat J/s); no new velocity-suffix table.
- **Hardpoint anchors must satisfy BOTH #44 gates to be usable**: on the cell lattice (x/z multiples of `cell_m`, y multiples of `cell_height` = 2.2 m — the dorsal turret at y=2.2 and engines at y=0 set the rule) AND inside exactly one section box (G4 "anchor is in 0 sections"). First pick y=-2 passed the lattice but fell below every section's y-range; second pick z-cell collided with no section. Lattice alone is not enough.
- **A registry extension without a negative pin is dead weight.** `weapon.*` keys cooking fine proves nothing; the load-bearing pin is the near-miss key `weapon.dmage` dying at "not in the ratified attr-key registry" (key grammar accepts it, membership kills it). Mutation `if (false && !known)` fails exactly there.
- **Component-append doctrine holds**: 9.6 weapon `[[component]]` entries appended at END of the component list (before first `[[wire]]`) so `component.N` def_forms/loadout pins didn't shift from mid-file insertion — same trick as 9.3's door_2/door_3.
- **Count ripples when a sample grows**: DEF0 components 13→16, catalog 10→13, manifest 16→19 (+3 TYPE defs), GLB sockets 8→11 / nodes 14→17, loadout panel wants list +3, ship_designer fixture sockets 14→17. Regenerate both goldens (`VE_GOLDEN_UPDATE=1`) after moving anchor translations.

## 9.7 (368e93f) instant-hit laser raycast
- **One slab kernel.** A laser is a static segment; `sweep_point_vs_obb(origin, dir_unit, range, obb)` with unit velocity makes the swept t-domain metres — segment-vs-OBB exactly. Reusing the Phase 7 primitive (not a second raycast implementation) means identical determinism guarantees and replay-hash coverage for free.
- **Unit-dir is a law, not a nicety:** non-unit dir silently scales t (and normals via rotate) — the same reason sweep rejects non-unit OBB attitudes. Guarded with the same `approximately_equal(length2, 1, 1e-6)` tolerance shape.
- **Total order for the pick:** candidates scanned in container order, incumbent replaced only on STRICTLY smaller dist → with ordered containers the result is input-permutation-independent (test pins nearest wins even when the far box is scanned first) and zero-dist ties resolve to first-in-order (ordered-container sim rule, no unordered-map iteration).
- **doctest `Approx(x).epsilon(0.0)` never passes** — the allowed margin is a strict inequality and 0*scale margin fails even bit-identical values. Pin dyadic closed-form values with plain `==` (as swept.h tests do) and reserve epsilon(1e-12) for quat/libm ulp cases.
- Closed-form rotated pin: ray through a yaw-45 box half-extent h on the ray-normal slab gives dist = D − h/cos45; normal = yaw45·(0,0,1). Range boundary: `t0 > dt` reject means t0 == range HITS (inclusive max range).

## 9.8 (8a457e0) gun projectiles — ordinary entities, one physics path
- projectile_step calls the SAME assemble_tick_inputs + integrate_step_dt the
  ship spine calls (empty tick_command -> wells only). Everything the engine
  already guarantees for ships — determinism, LOD lattice compression
  (dt_ticks=divisor, sample at interval start), state_hash/replay_interval_hash —
  covers rounds for free. Zero new integrator math.
- Drift cuts both ways by construction: spawn vel = ship.vel + aim_dir*muzzle.
  A1 pins the lead solution (sin th = u/muzzle) to tick 29; entry x is EXACTLY
  the static -X face (395) because drift is along z — do NOT subtract target
  drift from the closing speed unless it is on the firing axis (A3 does:
  receding 60 m/s -> t_e = 395/740, tick 32).
- No tunneling even when a coarse block walks a round clean past a 10 m slab:
  the step sweeps its own integrate segment (p0 -> p0 + v_new*dt) vs the
  moving target OBB — #45e pays off in combat for free.
- ttl expiry is a TERMINAL step: end-born > ttl returns expired and does NOT
  advance state — caller despawns; re-stepping is idempotent.
- Mutation trap: brace-init `{0,0,0}` as the sweep's target_vel arg fails to
  compile under the mutation (template deduction vs obb arg). `target_vel * 0.0`
  keeps the symbol referenced and compiles — the compilable form for this
  guard.

## 9.9 (64dc74c) rocket boost dart — 9.8 + a bounded delta-v reservoir
- #19e reservoir pattern reduced to a transient entity: one fp64 `dv_remaining`, no bands/tags — nobody observes a bullet, so the stats pipeline binding of the full reservoir is skipped. The drain law is `applied = min(accel*dt, dv_remaining)`; the min IS the floor: dyadic budgets hit EXACTLY 0.0 (50/(20/60)=150 ticks), non-dyadic land exactly 0 via one partial tick, a dry dart coasts, the budget can never go negative, and the drained amount is a pure function of applied impulse (determinism).
- Boost window `[born, born+boost_ticks)` is command-exact and `rocket_step` is single-tick only (dt_ticks=1): window boundaries must not be block-approximated. Callers LOD rockets by simply stepping every tick — darts are cheap and short-lived.
- Steering = full authority (heading snaps to a unit steer aim) ONLY inside the window; `steer_aim` outside it or non-unit is ignored, and post-boost the dart is bit-identical to an unsteered 9.8 round (R1 vs R3 pins). Launch keeps 9.8's unit-aim + velocity-inheritance laws.
- Boost accel is added to `assemble_tick_inputs`' well accel and fed to the SAME `integrate_step_dt` — one physics path; R6 (y=50, boost +Y 20 vs g −10) pins the net +10 closed form, so gravity bends the boost arc exactly like everything else.
- Contact + ttl are the 9.8 rules verbatim (swept slab + target_vel, strict-less nearest, terminal-idempotent expiry) — `rocket_step` reuses them, so 9.8's mutation-proven collision guarantees cover rockets for free.
- Test pins (all closed-form semi-implicit, dt=1/60): boost v(60)=120 and p(60)=100+20·1830/3600; mid-boost steer@30 split v=(110,0,10), p=(105+2.583, 2.583); reservoir cutoff tick 150 → v=150 exact; contact cmd 118 entry x=395. Six guards mutation-proven from /tmp/mut99.py (window <=, cap removed, steering dead, unit-steer off, launch unit-aim off, ttl off).

## 9.10 (36b86b9) firing cycle — gun_slot state + one gate chain
- **PENDING RATIFICATION:** the two 9.10 design questions (clarify, 2026-11-18) went unanswered in-session; implementation uses the RECOMMENDED defaults — (1) overheat ceiling = new attr key `weapon.heat_capacity` driving a #19e reservoir band that throws `gun.overheated` (dark-not-dead, same pattern as `propellant.empty`); (2) spread RNG owned by CORE: seeded stream through `splitmix64`, fired direction is sim state and hashes. If either is rejected the change is local: ceiling source is one `derived()` call in `gun_try_fire`/`gun_configure`; RNG ownership just moves the stream mix into game glue passing a pre-jittered `aim_dir`.
- `gun_slot` = per-gun sim state: `#19e Reservoir` (heat, UNBOUNDED above — overshoot past the limit is the refusal signal, not clamped away), `last_shot` tick, fired-`shots` counter, shared ship `TagSet`, `gun_attrs` id-bundle resolved from cooked DEF0 attrs (missing key = empty id = defined behavior, never a default number).
- Cooldown law: `interval = llround(kSim_tick_hz / rof)`; allowed iff `last_shot < 0 || now - last_shot >= interval`. `rof` missing/<=0/non-finite ⇒ NEVER fires — fail-SAFE, the deliberate opposite of the reservoir fail-open flows (firing is an act, not a flow).
- Gate chain in `gun_try_fire` (order is law): unit-aim guard (9.7 law) → cooldown → overheat (capacity re-read LIVE from the pipeline, so a mid-tick `set_value` is visible immediately) → spread. A refusal consumes NOTHING: no cooldown stamp, no heat, no RNG tick.
- Spread = deterministic stream keyed (world seed, component id, FIRED-SHOT count — refusals can't shift the stream) → two `unit01` draws → cos uniform over the cone cap (`cos_t = 1 − u1·(1−cos half)`, uniform solid angle) → azimuth `u2·2π` around the smallest-|component| reference frame. Splitmix64 promoted from terrain-private copies to `core/rng.h` (terrain keeps its local copies — no drive-by refactor).
- Heat: dissipation = `weapon.heat_dissipate` leak attr through the reservoir integrator (`leak·kSim_dt` exact dyadic in tests); a shot pushes `weapon.heat_per_shot` through the set_value door; overheat band `at_or_above capacity` throws `gun.overheated`; NO hysteresis — the tag un-throws the instant the leak drops heat under the band (mid-tick un-throw proven by `gun_heat_cycle` t=7).
- Registry: `weapon.heat_capacity` added (25 keys, bare SI = J); TYPE defs carry it; gun_laser merged DEF0 attrs now 7; golden regenerated (`VE_GOLDEN_UPDATE=1`).
- Tests: `gun_rof_ticks` (spacing rof 2/7/0.5 → {0,30,…}/{0,9,…}/{0,120,240}, llround law, fail-safe never-fires, degenerate/nan aims, splitmix chain hand-pinned at seed 0xC0FFEE shots 0–2 (python replay), shot-keyed-not-tick-keyed (late first shot bit-identical), refusal no-ops, seed divergence, cone containment over 24 shots, spread-missing = exact aim) + `gun_heat_cycle` (dyadic timeline: leak 30 J/s = 0.5/tick, shot 2 J, capacity 3, rof 30 = interval 2; refused-at-6 pins heat 3.0 = the shot-4 accumulation (1.5+1.5) not 3.5 — refusal never added heat; t=7 fires at 2.5 proving mid-tick un-throw; +8 idle returns to 3.0).
- 7 guards mutation-proven (`/tmp/mut100.py` + round-2 `mut100b.py`): interval `<`→`<=`, rof fail-safe off (fires at 60/0=UB-interval), overheat gate off (caught as doctest FAILURE after the test was made crash-proof with a bounds CHECK — a sanitizer CRASH is detection but leaves `test cases: 0`, which the ladder must not count as proof), stream tick-keyed, shots frozen, heat door empty (`+ per_shot * 0.0` keeps it compilable), unit-aim off.
- CI green 413.44s (/tmp/ci_100.log). Pitfall: after a mutation ladder, the binary carries the LAST mutated object until rebuilt — a "failure" from the stale mutated binary looks like a test regression.

## 9.11 (3ea0643) damage pipeline — shield → component health → offline
- `damage_sink` (core/sim/damage.h): shield = ONE ship-wide Reservoir (regen
  supply attr through the pipeline at the reservoir door, capacity attr sets
  ceiling at configure; capacity attr missing/<=0/non-finite = absorbs NOTHING
  — fail-safe like 9.10 rof). Components = ordered vector<unique_ptr>: a
  Reservoir self-refers its OWN TagSet, so unique_ptr pins addresses.
- Hit law (sink_apply_hit, fp64): absorb = min(damage, shield value) FIRST,
  overflow lands on the hit component only; reservoir floor clamps hp at 0
  (never negative); zero-crossing band {0, tag, at_or_below} throws inside
  set_value — no step wait; repeat hits change nothing (hp already 0).
- OFFLINE machinery = shipped WiringReach `kill_component` (the #26 "function-
  zeroing modifier": kill zeroes every attr the component names as
  function_attr/draw_attr through the standard dark source; idempotent) +
  `revive_component` for repair. Proof in `component_offline_zeroing`:
  engine thrust attr AND draw go 0; shield regen attr goes 0 so the reservoir
  door multiplies flow to zero (regen stops with NO shield-side code); gun
  weapon.rof goes 0 and the 9.10 fail-safe silently refuses fire — zero new
  systems (#26e "all read the tag, no new machinery" satisfied through the
  modifier mechanism rather than tag polling).
- `ship.component.offline` is PLAN-literal, lives on the component TagSet
  (hull-local ref), read by anything via condition_met (6.6 gate; effects
  condition_met already gates bundles on it for free).
- ctor trap: Reservoir(initial, bands, tags) sets VALUE only — capacity clamp
  rides set_ceiling; forgetting it made regen overfill the shield (caught by
  the exact pins). Repairs clamp at ceiling via the same door.
- Refusals = zero state change: unknown component ref, damage < 0 or NaN,
  repair hp <= 0 or NaN (nullopt/false).
- Tests exact-== throughout (dyadic pins; regen 60 J/s = exactly +1.0/tick
  since 60.0*(1.0/60.0) == 1.0). 6 guards ALL-CAUGHT non-vacuous (absorb
  max-flip, drain-door empty, transition-law always, kill-call off,
  repair-refusal off, band-dir flip). Full ci_local GREEN 437.20s.
- 9.12/9.13 ride this: breach = section air Reservoir vent (same band/tag
  pattern), wire sever = kill cascades through the SAME WiringReach.

## 9.12 (77d7c16) section breach — per-section air + door-isolated vent
- `section_air` field (core/src/breach.cpp): per-section air = 6.9 Reservoir
  behind unique_ptr `air_entry` (reservoir self-refers the section's OWN
  TagSet for authored bands — 9.11 self-ref pattern; ctor sets VALUE only,
  capacity clamp rides set_ceiling — trap recidivist).
- Vent law per tick: pipeline flows (refill supply attr, fail-open empty id)
  FIRST, then per breach: BFS from the breached section along edges whose
  door is OPEN or ABSENT (DoorField 7.13 facts; edges iterated in
  registration order, FIFO queue — ordered containers, rule 5); every
  section in the reachable region loses the breach rate through the
  set_value door. Floor clamps at EXACTLY 0.0 (dry stays dry, never
  negative). Simplification documented: rate leaks from EVERY section in
  the region, not one redistributed total; multiple breaches each add
  their own rate.
- Isolation: door shut before step T freezes neighbors from tick T on
  (dock_ring pinned 95.0 forever after exactly 2 drains); the breached
  section keeps venting until `breach_patch` — PLAN's "until doors are
  closed or a patch repair runs" = spread stops at doors, leak stops at
  patch.
- atmo.venting / atmo.empty are CALLER-authored bands (opaque strings,
  AGENTS rule 1: core never names "atmo"): shipped hysteresis-free bands
  throw/un-throw inside set_value with no step wait (venting tag at
  at_or_below 95 throws the exact tick value lands on 95.0, un-throws at
  96.0). Occupant #18d KO path = a condition_met gate on the section's
  TagSet (pinned flip at the empty tick) — zero new machinery (#19c).
- Fail-safes: breach_open refuses unknown section, rate <= 0, non-finite,
  duplicate (idempotent); breach_patch false when nothing there;
  air_configure rejects dup id / cap <= 0 / self-edge leaving NO partial
  field (built off-tree then moved).
- Refill math: 60 J/s = +1.0/tick EXACT (60*(1/60) == 1.0), ceiling clamp
  pins 100.0 forever under the pump. Whole-ship hull-breach threshold
  untouched here (9.14 owns the dramatic tier).
- 6-guard mutation ladder ALL-CAUGHT non-vacuous (doors-ignored, drain
  door emptied, BFS never seeds, ceiling doubled, patch erase off, rate
  fail-safe off); full ci_local GREEN 415.15s.

## 9.13 (a814d08) wire sever cascade — section->wire cook index + #46a mount hop
- Cook side: hull def `[[section]].repair_facets = [...]` names repair
  targets; cooklet validates facet refs through the facet table and appends
  `(facet_id, {ascending wire ids routed through the section box})` to
  `hull.section_wires` (parallel to section_volumes, same walk as
  hull_section_volume_rows). Test ship: panel_main owns w_engL+w_engR,
  berth_1 owns w_fire_ctrl; every wire crosses midship. Section index feeds
  ONLY gameplay (sever targeting) — physics untouched, golden re-pinned
  via VE_GOLDEN_UPDATE (facet-row bytes only).
- Sever (loadout_load.sever_wires_in_section): one hit cuts EVERY routed
  wire of the section (net.sever_wire, already-severed rows report false
  and cut NOTHING again — second hit on a dead section = empty cut set).
  Dark cascade is 100% WiringReach recompute: sever removes the wire's
  endpoints from the net graph.
- #46a MOUNT HOP (the wiring-design gap that blocked the draft): def wires
  terminate at sockets, components live at hardpoints, and the sim never
  joins them. Fix = `WiringReach::mount(ref, mount_socket)`: when the
  #46a loadout pairing names a socket that is a wire-table node, the
  component's terminals bond to that socket node — UNDIRECTED one-hop
  (socket reaches every component mounted on it in registry order; a
  source component feeds its mount socket). No new node kinds, cooked
  wires stay verbatim, sockets naming no wire = legal no-op attach.
  Sever w_engL/w_engR at engine_bay -> exactly engine_L/engine_R dark:
  thrust 0, v_z bit-frozen through the dark ticks (coast adds +0.0),
  rail resumes after. Non-dyadic kSim_dt ladder rides 6.12 Approx idiom.
- Repair law (repair_wire_at_facet): sever sets hull repair_facets = the
  facet rows whose wire list CONTAINS the severed id (no facet -> sever
  skipped). Repair needs facet present (no_facet), wire actually severed
  (unknown_wire), a matching cargo spare (unknown_spare), and costs
  EXACTLY one spare (spare_wire count). All refusals consume nothing.
  Repair restores reach through the same recompute.
- Draft had NO test file at compaction; re-derived every pin from the
  on-disk def (4 sections, 6 wires, w_engR facet row existed but its test
  was never written — the in-flight dirty work was the facet plumbing).
- 3-guard mutation ladder ALL-CAUGHT (mount-hop removed from flood,
  repair live-wire gate off, sever counts already-severed rows).

## 9.14 (affec22) whole-hull destruction — ship-wide hull-integrity reservoir (#56)
- Mechanism RATIFIED by the user at task start (decision #56 in PLAN.md):
  the dramatic tier of #19c is a SHIP-WIDE hull-integrity Reservoir —
  the 9.11 single shield pool pattern — living on the 9.12 section_air
  breach field (the breach domain owns both tiers). Rejected at the same
  checkpoint: single-hit magnitude gate, aggregate open-breach vent-rate.
- Authoring: `integrity_configure(field, {capacity, bands})` AFTER
  air_configure (air_configure rebuilds the field). Bad capacity
  (<=0/NaN) refused leaving the tier unconfigured; reconfigure while
  configured refused (no pool swap mid-flight). Ctor sets VALUE only —
  ceiling clamped at configure (the standing 9.11 trap).
- Drain law: `integrity_hit(damage)` = overflow that landed on the
  STRUCTURE (the caller's game-side routing decides what counts —
  shield soak and component health are the 9.11 sink domain, untouched
  here). set_value door floors at exactly 0.0. Refusals: unconfigured
  tier, damage <= 0 / NaN / non-finite, already destroyed — all zero
  state change (fail-SAFE).
- Terminal event: first landing on 0.0 flips `is_destroyed()` and the
  authored zero-crossing band has thrown on the HULL's own TagSet
  (hull.breached in the test). Further hits refuse; the wreck keeps
  simulating (air steps on — Phase 11 owns downstream meaning).
- The two isolations are the law, each pinned: (a) breach AIR venting
  runs full-rate for ticks and the pool never moves; (b) the 9.11
  component_offline_zeroing fixture killed to ALL systems offline (dark
  set = 3, all derived attrs 0) and the pool sits at full capacity,
  not destroyed — component pile-up can never reach the dramatic
  threshold. Separate domains = the guard, no cross-wiring exists.
- All values dyadic (60/25/35/10, air 100-4x2.5=90, 87.5) — exact ==
  pins, no Approx anywhere.
- Design note: `integrity_entry` is unique_ptr-owned because Reservoir
  holds a raw TagSet* to its own tags (same fix as air_entry).

## 9.15 (ef80cca) dock repair round-trip — bit-exact stats restore, zero new machinery
- Done-when (d) proof. Docking is GAME-side (core has no "dock" concept,
  AGENTS rule 1): a dock just calls the shipped 9.11 repair verbs. The
  test is the scenario: pre-combat stats_state_hash baseline -> combat
  pile-up kills ALL wired systems (offline modifiers zero attrs) ->
  dock repair heals + revives -> hash lands back EQUAL to baseline.
- Non-vacuity pinned twice: `hash_combat != hash_pre` (combat really
  moved the bit state) and refusal pins checked BETWEEN repairs (a dead
  revive-guard can't hide: refused repairs must leave the system dark).
- Revive law (9.11, now scenario-proven): ANY positive hp re-opens the
  band un-throwing the offline tag and `revive_component`s the node —
  partial repair (10/50) already restores attrs; over-repair clamps at
  the ceiling via set_value; 0/NaN/unknown-ref refuse with zero change.
- Mutation trap hit again: deleting the `wiring->revive_component` line
  made `wiring` unused -> -Werror=unused-parameter -> build fails ->
  chained run tested the STALE binary (vacuous "CAUGHT"-looking pass was
  actually BUILD-FAIL). Mutate compilable: `if (wiring) { (void) wiring; }`.
