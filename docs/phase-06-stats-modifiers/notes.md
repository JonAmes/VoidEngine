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
