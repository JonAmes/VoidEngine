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
