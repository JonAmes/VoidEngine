# Phase 6 — notes

Design notes, findings, and gotchas for the stats & modifier framework. Appended per task.

## 6.1 — StatsSet (450ec46, CI GREEN 101/10978)
- Sparse keyed storage: insertion-ordered `vector<entry>` + linear scan. Tens of stats per entity; ordered iteration is the determinism contract, unordered maps are banned for sim-visible reads here.
- fp32 values per PLAN/AGENTS rule 4. The split that matters later: reservoir STATE is fp64 (6.9); only the attribute surface is fp32.
- `set_base` rejects empty keys and non-finite values atomically (false, no mutation) — NaN silently poisons clamps + hashes; the fp32 pipeline is only hand-verifiable on finite inputs.
- Key grammar is NOT validated here (AGENTS rule 1): dotted-identifier gates live game-side (attr_keys.h) and in the cooklet. StatsSet stores any non-empty bytes as data; a test pins unicode/double-dot passthrough.
- `derived` mirrors `base` until 6.4's dirty-flag recompute replaces that invariant (single door for readers).
