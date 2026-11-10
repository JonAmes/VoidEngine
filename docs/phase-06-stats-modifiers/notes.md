# Phase 6 — notes

Design notes, findings, and gotchas for the stats & modifier framework. Appended per task.

## 6.1 — StatsSet (450ec46, CI GREEN 101/10978)
- Sparse keyed storage: insertion-ordered `vector<entry>` + linear scan. Tens of stats per entity; ordered iteration is the determinism contract, unordered maps are banned for sim-visible reads here.
- fp32 values per PLAN/AGENTS rule 4. The split that matters later: reservoir STATE is fp64 (6.9); only the attribute surface is fp32.
- `set_base` rejects empty keys and non-finite values atomically (false, no mutation) — NaN silently poisons clamps + hashes; the fp32 pipeline is only hand-verifiable on finite inputs.
- Key grammar is NOT validated here (AGENTS rule 1): dotted-identifier gates live game-side (attr_keys.h) and in the cooklet. StatsSet stores any non-empty bytes as data; a test pins unicode/double-dot passthrough.
- `derived` mirrors `base` until 6.4's dirty-flag recompute replaces that invariant (single door for readers).

## 6.2 — gameplay tags (CI GREEN 102/11013)
- Ancestry is pure string: T answers Q iff T==Q or T starts with Q+'.'. The dot boundary is the whole ballgame — half-typed query tags must not match ("pilot.stun" vs "pilot.stunned"), and "pilotstunned" is not under "pilot". Test pins both directions.
- "" (empty tag) rejected at add: it would answer every prefix query; symmetrically the empty query answers nothing.
- remove() is exact-only; descendants are independent entries (removing "faction.hostile" leaves "faction.hostile.alert" answering "faction"). If a cascade-drop semantic is ever needed, callers enumerate with count_matching in insertion order.
- Same ops sequence -> identical order, pinned by replaying a build() twice (tags will feed sim-visible queries from 6.6 on).

## 6.3 — modifier stack (CI GREEN 103/11071)
- Stacking identity = (target_attr, stack_id) and is resolved GLOBALLY across sources (2 sources carrying the same effect id interact); the INCOMING modifier's policy decides. stack_n's cap counts all live instances incl. the new one.
- refresh semantics pinned: existing values/positions untouched, deadline overwritten on EVERY same-identity modifier (nullopt -> permanent); refresh with no collision = plain insert.
- Expiry: live while current_tick < expire_tick (dropped exactly at T). Deterministic drop walk; no-op expire_due does NOT bump revision.
- Cycles-by-construction: no data-model edge from modifier to modifier exists, so dependent chains are unbuildable; pipeline reads base only (pinned deeper in 6.4).
- revision() is the dirty clock: source add/remove, mod add/refresh/expire/dispel bump; no-op expiries don't. 6.4 stamps per-attribute last-revision against it.
- Ordered view: source order beats modifier insertion order — pinned by replaying build() twice AND an exact string vector.

## 6.4 — aggregation pipeline (CI GREEN 104/11130, done-when (a))
- Stage order pinned: base -> +sum(flat) -> *(1+sum(pct)/100) -> *prod(mult) -> clamp. Percent values are PERCENT-POINTS (10 = +10%). The 86.25 exact pin discriminates wrong stage order (percent-before-flat gives 82.5) — all suite pins binary-exact so CHECK(==) is a bit pin.
- fp32 accumulation walks the stack's TOTAL ORDERED VIEW; associativity is not free, the walk order is the replay-hash contract.
- Overflow policy: inputs finite by construction (6.1/6.3), so only arithmetic overflow leaves the range -> saturate to +/-FLT_MAX; NaN/inf never reach consumers, clamps, or the state hash. Test forces stage-1 inf then *0 (the only NaN route).
- Dirty clock is coarse-global (max of the two revisions): any mutation re-dirties every attribute. Deliberate: tens of attrs; fine-grained per-attr dirtying would cost more than it saves. Cold-attr reads are the only extra walks.
- set_base's derived-mirror stays for stack-free StatsSets; once a pipeline stamps an attr, the stamp is authority. clear() bumps revision only when non-empty (no-op clears must not re-dirty pipelines).

## 6.5 — event bus + canonical hash (15d27fc, CI GREEN 106/11176)
- FNV-1a-64 was duplicated-by-necessity in replay.cpp; now ve/core/fnv.h is the single primitive ("compatible with the replay-hash machinery" == same mixing, bit-for-bit). Existing replay hash pins proved the refactor free.
- Bus emission is bit-compare (std::bit_cast u32), NOT ==: shared raw-bit policy with the state hash means the event stream and hash never disagree about "changed" (-0.0 vs +0.0 both change).
- Events fire at RECOMPUTE time, not mutation time (recompute-on-read model): "when did a consumer see it" = which tick's drain. begin_tick tags; seq is bus-wide monotonic.
- Canonical stats form: insertion order IS state (never re-sort keys); what done-when (e) guarantees is recompute-ORDER independence, pinned with forward/reverse/scrambled/recompute_all equality.
- test_stats_hash carries an independent test-local FNV loop on purpose: a shared impl could not pin the byte format (bugs would cancel).

## 6.6 — effects + conditions + rails (02b6ca2, CI GREEN 108/11263)
- Effect atomicity has TWO halves: pre-validate the whole bundle BEFORE add_source, AND roll back (remove_source) if any member policy-rejects mid-bundle. A taken source id is refused without touching anything.
- Timed effects: effect_desc::expire_tick STAMPS OVER each member's own expiry — the bundle is one life-clock. Dispel-by-tag still works (member tags carry into the stack).
- condition attr tests read DERIVED (pipeline view), never base — the gate sees what the sim sees. Missing attribute = gate closed (fail-safe default).
- binary32 gotcha pinned in test: 0.1f+0.2f == 0.3f exactly (unlike doubles); ULP discriminators must use nextafter.
- rail_event payloads: two opaque u64s; flip events carry NEW STATE in b so the log alone answers "what state is it now" for scrubbing. Bus cannot distinguish seated vs on-foot verbs — that IS decision #28's identity claim, pinned as a shape-equality test.

## 6.7 — seat<->stats linkage (219419c, CI GREEN 109/11334)
- The affordance = 6.6 effect bundles keyed by ROLE TAG: occupant_grant lands on the occupant's stack as one source; when_unmanned lands on the SHIP stack while empty (#18b zeroing). Roles stay game-side; core keys on opaque tag strings.
- Source-id reservation: seat index i owns kSeatGrantBase+i / kSeatUnmannedBase+i. Caller sources must stay out of these ranges; a squat is refused (stack_conflict) BEFORE occupancy changes — never a half-applied sit.
- sit() MOVE semantics: occupancy already moves atomically; the link must reverse the OLD seat (grant drop + zeroing re-arm) and apply the NEW one around that single occ.sit call — order: busy-check, conflict pre-check, occ.sit, reverse old, apply new, rail events (drop then attach for a move).
- Initial when_unmanned state: seats start empty, so wiring a ship stack means scripting needs the zeroing live; tested honestly via sit/stand so the unit property itself is the assertion. If a phase-8 sim needs cold-start unmanned state, call sit+stand once at wiring or add a sync pass then.
- drop() = KO (#18d): ejects WHOEVER holds the seat; caller passes the victim's stack (sim knows the victim; the claimant id is decorative).

## 6.8 — flight<->stats glue (4e85acf, CI GREEN 110/11394)
- Zero new core code: done-when (b) is proven by COMPOSITION (SeatLink + pipeline sampled per tick + Phase 4 assist law). That is the point of the spine.
- Crew-ship stack wiring: seat grants for SHIP functions ride the ship's own ModifierStack (occupant stack == ship stack). Occupant-personal attrs (pilot.reflex) keep per-entity stacks; both patterns coexist (6.7 vs 6.8).
- Control law must SAMPLE gains per tick from the pipeline (pre-tick state), so a mid-flight sit/stand bends the trajectory at exactly that tick — decay pins are the geometric law omega_n*(1-kd*dt/Iy)^n, doubles-exact to 1e-12.
- Stabilizer gain zero = assist silently off (tau = -kd*omega). Unmanned zeroing IS "assist authority requires a pilot" — no engine bool for "assist enabled".

## 6.5/6.6 docs catch-up + done-when audit
- 6.5 = 15d27fc (CI 106/11176), 6.6 = 02b6ca2 (CI 108/11263) — both were committed+pushed with docs earlier but the 6.5 checklist box replacement silently no-oped (regex miss); now fixed.
- Done-when audit vs PLAN: 6d9814b checked "(e)" believing 6.5's commit label; but the spine maps (e) = junction-dark-set wiring test ← 6.12. Unchecked again. 6.5's commit-message "(done-when (e))" tag is a mislabel; the (e) bullet stays with 6.12. (a) <- 6.4 ca777aa and (c) <- 6.7 219419c now properly checked.

## 6.9 — reservoir primitive (3da4068, CI GREEN 111/11642)
- Flow delta is net/60.0 (division, not *(1/60)): keeps hand-picked rates binary-exact (30/s -> 0.5/tick); crossings pin to the exact tick, no hysteresis fuzz.
- Hysteresis-free band eval = re-evaluate ALL bands every tick: tag add is idempotent (unique set), remove is exact. Consequence tested: ancestor query `tank` answers exactly while some `tank.*` band tag is live.
- Pipeline must be NON-const for the reservoir: StatsPipeline::derived() is recompute-on-read (mutates dirty stamps). "Pure function of aggregated attrs at the tick" refers to VALUES, not constness.
- Refill/set_value evaluates bands immediately (market refills, authoring); construction too, so cold-start state is tagged at tick 0 without a warmup step.
## 6.10 — power pool (dc1a37d, CI GREEN 112/11680)

- PowerPool holds NO state: lit/dark is recomputed from the pipeline on every
  `rebalance()`, so the pool can never desync from stats. Call it every tick
  (linear in systems) or after any mutation — idempotent.
- Priority/draw/function/supply are opaque attr keys; priority being an attr
  means a damage event can re-rank who sheds, live (pinned).
- Shed order: priority DESC, ties by registration index (stable_sort over a
  copy; `m_sys` stays registration order). Drops take the order TAIL.
- **Trial-lift restoration**: a dark system's own draw reads 0.0 (its ×0 mod
  is live), so the headroom test cannot be done from the draw alone — lift
  for real, measure, re-dark if over. One pass highest-first is the fixed
  point (lifts only consume headroom; skipped systems stay dark until the
  next call — no backfill past a richer system).
- Shed bundle = ONE source per system: `{draw.X ×0, function.X ×0}` — draw
  zero takes load off the pool, function zero is what the sim sees (#26
  dark-not-dead). remove_source returns aggregates bit-exact to baseline.
- Source-id range: `kDefaultBase = 0xF000'0000'0000'0000`, one id per system
  by registration index; collision with a caller's own ids = deterministic
  refusal at add_source.
- **6.4 dirty-clock bug found here**: the old `max(set_rev, mod_rev)` stamp
  freezes once base revisions run ahead of stack revisions — every power
  modifier landed INVISIBLE. Staleness is now per-SIDE (revision PAIR).
  Regression subcase in `stats_aggregation` pins the exact old failure
  (base rev 51, stack-only flat_add served stale 10.0).

## 6.11 — propellant drain, done-when (f) (4fba91e, CI GREEN 113/12009)

- Pure composition proof (like 6.8): NO new core code, NO stored burn state.
  Per tick the glue reads the AGGREGATED thrust.force (post-zeroing) + isp,
  writes `propellant.load = force/isp` (replace policy, one live member),
  the reservoir integrates `force·dt/Isp` — drain is a pure function of the
  impulse actually handed to `replay_step` that tick.
- Binary-exact scenario design: 1200 N / Isp 20 -> 1.0/tick; every mid-burn
  value is exactly `120 - n` (CHECK(==) bit pins, no Approx anywhere).
- Closed loop: dry at exactly n=120 -> glue adds ONE x0 source on
  thrust.force (dark-not-dead) -> applied force reads 0 BITS -> no impulse,
  no drain, coast forever; value never negative (floor).
- Refill = `set_value` door (hysteresis-free band re-eval inside) +
  `remove_source` -> derived force returns to baseline BIT-exact
  (bit_cast pin), drain resumes at 1.0/tick.
- Subtle pin: an early refill changes NO force at its tick (tank isn't dry
  yet), so the sim hash stream stays identical until the dry variant
  zeroes thrust — first hash divergence lands exactly at tick 120.
- Whole scenario folded to ONE `replay_interval_hash`; two identical
  scripts produce bit-identical per-tick streams.

## 6.7 docs repair (checklist line 11)

- The 6.7 box line had been mangled by a later sed into garbage and the
  a84183e "6.7 checked" patch had actually NO-OPED on the box (third time
  sed patching this checklist silently failed — always verify box flips by
  grepping the SHA back after patching). Restored from a98839f spine text
  with commit 219419c (CI 109/11334); test name corrected to the real
  doctest case `seat_stats_linkage`.

## 6.12 — wire networks as reachability BFS, done-when (e) (93102cc, CI GREEN 115/12235)

- `WiringReach` (core/sim/wiring.h/.cpp): power/signal flow = BFS flood over
  the hull's authored wire rows; NO persistent lit/dark state — recompute
  only on topology events (mount/unmount, sever/repair, kill/revive).
  `recompute_count()` pins: 600 clean reads = zero walks; refused events
  (unknown ref, no-op revive) = zero walks.
- Loader change: DEF0 wires are now CARRIED into `loaded_hull::wires`
  (5.10 decoded-and-skipped them). Wire `kind` u8 validated `<= 2`
  (0 power, 1 signal, 2 power_signal — cook-side grammar; core treats the
  bytes as cooked ranges only), endpoint kind `<= 1` (0 socket, 1 comp).
- Flood semantics (concept-free): power floods from mounted+alive
  `is_power_source` comps through live wires into sockets AND comps —
  sockets are pure relays, which is what lets a wire pass hop through a
  hardpoint/socket bus (`w_r_b -> bus_a -> w_b_l` pin). `power_signal`
  wires carry BOTH floods; the signal flood seeds from
  `is_signal_source` comps and is independent of power.
- Consumers declare `gate_power` / `gate_signal`; ungated = relay. Dark =
  gate set and its flood fails, OR node killed, OR already dark (one-pass
  cascade to the fixed point — registration order makes it total).
- Dark-not-dead = the exact 6.10 machinery: ONE source per dark component
  carrying `{function_attr ×0, draw_attr ×0}`; empty attr string skips that
  member. Remove the source -> aggregates return BIT-exactly to baseline
  (bit_cast pins in `wiring_reach`; `stats_state_hash` equality pin in
  `wiring_dark_set` — post-repair hash == pre-cut baseline hash).
- Semantics pinned: killed node is excluded from flooding (a killed junction
  stops relaying; a killed gated consumer fails its own gate -> dark);
  mounted ≠ dark (unmounted = absent, holds no source — remount re-runs the
  BFS into whatever state the net dictates); a mounted signal source emits
  even while dark (documented Phase 6 simplification, revisit with
  damage/glitches if it bites).
- `wiring_dark_set` (done-when (e)): junction killed at tick 60 in a FLIGHT
  loop where the engines comp's function attr IS `thrust.force` — exact
  dark snapshot `{"engines"}` (registration order), force bits
  1200/0/1200 across cut/leg/repair, v_z hand-exact `-t·kSim_dt·F/m` then
  bit-frozen coast then resume, first replay-hash divergence EXACTLY the
  cut tick (60) and EXACTLY the repair tick (90), whole scenario one
  `replay_interval_hash`, two runs bit-identical.
- Gotcha: recompute-on-read cache — the StatsSet derived mirror only shows
  restored bits after the NEXT `derived()` read; capture baseline-restoration
  hashes AFTER the glue's per-tick reads, not at the event tick itself.

## 6.13 — markets + reputation, done-when (g) pt 1 (40d5e86, CI GREEN 117/13291)

- **bands.h = one threshold primitive everywhere**: the 6.9 band machinery
  extracted verbatim (same `band_dir`/`reservoir_band` names — one type),
  `apply_bands(value, bands, tags)` now backs BOTH the reservoir and
  reputation flips (#20c "same threshold machinery"). Reservoir shrank to a
  call; 6.9 regression (248 assertions) untouched.
- **piecewise_curve**: authored data, all-or-nothing validated (≥2 pts,
  finite, x strictly ascending — equal x rejected: ambiguous y). Fixed fp64
  interpolation form `y0+(x-x0)*(y1-y0)/(x1-x0)`, linear bracket scan (n is
  tiny, order load-bearing), clamped flat tails both ends — the tail is
  what floors a bountiful market. Concept-free: numbers in, numbers out.
- **Market = pure composition, price NEVER stored**: stock = Reservoir;
  production+lane flow = supply-attr modifiers; player buy/sell = ONE-TICK
  pulse EFFECTS (`qty/kSim_dt` on load/supply, `expire_tick = t+1`) through
  the 6.6 machinery — the reservoir integrates them the same tick, no
  transaction code. Glue re-derives `curve(stock)` into the price base attr
  every tick; the #18 trader seat discount is just a `percent` pipeline
  modifier on that attr (seat never touches stock).
- **Dyadic pins**: +60/s flow → +1.0/tick; buy 16 @100 → delta −15 → 213;
  sell 64 @140 → +65 → 317; curve (0,10)(128,2)(256,1) → price 1.3359375
  mid-curve, 1.0 tail; `ore.plentiful >= 256` flips EXACTLY tick 140; two
  scripted runs bit-identical (level/price/tag vectors).
- **Gotcha (reconfirmed)**: flow attrs must exist in the StatsSet at zero
  base — `derived()` on an authored-away attr fails closed → silently zero
  flow (first draft froze stock at 128). Same rule as 6.9 tests.
- **Off-by-one gotcha**: recorded vector index t = state after t+1 steps;
  pin formulas must use 128+(t+1), not 128+t.
- **reputation_tags**: rep attr on the faction's own StatsSet; player
  actions = INSTANT effects (no expire_tick → live until remove_source;
  removal restores the attr bit-exactly). Bands hostile ≤20 / friendly ≥60
  flip at EXACTLY ticks 40/70/95 under scripted strike/accord/forgive;
  hysteresis-free so friendly flips the same tick hostile drops; ancestor
  `faction` query rides 6.2 for free.

## 6.14 — faction power = one more reservoir, done-when (g) pt 2 (db7f17c, CI GREEN 118/15121)

- No new core code: `faction_power_bands` composes 6.9 reservoir + 6.13
  bands + modifier-fed supply/load attrs exactly like propellant/market.
  #25c's "one more reservoir instance" claim validated literally.
- Scripted war/escalation/peace/rebuild with dyadic nets (+2/−1/−3/0/+3
  per tick): watch (<=320) ON at EXACT tick 127 (v==320, inclusive
  boundary pinned), declining (<=200) ON 182 / OFF 237, watch OFF 277
  (319@276 still tagged), surge (>=400) never (scripted peak 384).
  Peace = `remove_source(war)`; annexed lane = new supply source — same
  add/remove_source pattern as 6.13 rep nudges.
- Pitfall (ancestor queries on band tags): the band TagSet is transient —
  all bands cleared by t=299, so post-run `tags.contains("faction...")`
  queries FAIL. Record per-tick query vectors during the run; don't
  inspect the final TagSet state.

## 6.15 — replay scrubber API (7ee0912, CI GREEN 119/15295)

- `scrubber.h`: replay RECORD = ReplayLog + `sim_checkpoint` snapshots
  (tick = post-step state; tick -1 = initial pre-step — tick_t is signed,
  use it). `ReplayScrubber` owns copies; stepping ONLY via 4.7
  `replay_step`, so scrubbed hash == live hash by construction, pinned
  bit-exact at cp boundaries, exact-hit ticks, and 37/437 mid-legs.
- Semantics pinned: `jump_to` re-replays from nearest cp <= target
  (backward scrub is just a forward re-replay — no step-back op); exact
  cp hit = 0 `replay_ops`, gap cost observable; `load` all-or-nothing
  (refused load keeps old record AND cursor — pin that the survivor still
  scrubs to live hashes, do NOT pin refusal-to-jump); oldest-cp rule:
  ticks below the oldest cp fail closed.
- `diff_between(a,b)` never moves the cursor (pinned by re-hashing);
  one-step diff from tick -1 is hand-exact `accel*kSim_tick_dt` mirroring
  replay_step's op order (`thrust * (1/m)` then `* dt`).
- doctest traps: `CHECK(a->x == 1 && ...)` = "Expression Too Complex"
  (arrow + &&); `CHECK(*opt == v)` too — split / use `.value()`. Mixed
  `tick_t{-1}, 0, ...` braced range-for won't deduce — `std::vector<tick_t>{...}`.

## 6.16 — scrubber panel on the pure-CPU UI core (8aa96ae, CI GREEN 120/15347)

- Panel lives in `game/` (ve_game is the layer that links core+gfx; panel is
  glue, concept-free anyway). `scrubber_panel::draw(ui, sc, x, y, actions)`
  -> buttons/text_field/checkbox/labels; state mutation ONLY through the
  6.15 scrubber API. Actions struct records stepped/jumped/jumped_to/
  diff_toggled so tests assert interaction without poking sc.
- Headless pinning: clicks = neutral ui_io frames (down frame, then release
  frame with pointer still over the widget -> click), output = ui_frame
  draw list compared byte-wise; DejaVu Mono advance 8.27515/cp -> button
  width int(sum)+2*kPad. Layout rects derived straight from ui.cpp rules.
- text_field traps: commit does NOT clear the buffer (Go re-commits stale
  text — pin "backspace n times" clears before retyping); chars apply only
  while focused, focus set on press-over-box; Enter commits same frame the
  chars arrive.
- Draw-list size comparisons must be at the SAME cursor: inspector label
  text length varies with the values, so verts.size() across different
  cursors is meaningless — grow at cursor, exact-restore at same cursor.
- Diff view fail-closed: diff_between(cursor-100, cursor) under -1 ->
  "diff n/a" row (view grows, no bogus deltas).
