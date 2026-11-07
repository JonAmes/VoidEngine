# Phase 0 — notes

## Gotchas that bit, kept so they don't bite twice
- **pip is externally managed (PEP 668)** on this machine — no `pip install --user`. Toolchain comes from checksum-pinned official tarballs via `tools/bootstrap_toolchain.sh` into `~/.local` (no sudo, idempotent).
- **SHA-256 self-consistency is not correctness.** The first hand-rolled hash passed every roundtrip but had three wrong K constants (cube-root fractions, not square roots). `hash_known_vectors` (FIPS vectors for "abc", empty, CRC32 quick-fox) is the gate — always pin known-good vectors when hand-rolling any hash/codec.
- **Watch-mode "failure" was a test bug**, not the watcher: the assertion hashed a single byte instead of the full rewritten body. Diagnose with the real binary + real manifest before touching product code.
- **Corruption tests must corrupt payload bytes.** Flipping a byte near the head hit the id field (rejected for the wrong reason); `b[b.size() - 5]` is the last payload byte before the CRC trailer.
- **`Reader::str()` consumes its own length prefix** — a test payload-walker that also reads the length separately drifts one u32 early and "reads text as a length" (`{"hul` → 1969758843).
- **KHR emissive extension property is `emissiveStrength`**, not `strength` — a silently-skipped fold shipped until the golden-value test caught it.
- **doctest messages** take streams: `REQUIRE_MESSAGE(x == y, "msg " << extra)`, not `"msg " + extra`.
- **`pkill -f pattern`** in a `bash -c` one-liner matches its own shell (SIGTERM to the commit itself).

## Decisions & doctrine notes
- Cooked container codec lives in `core/` (`ve::core::cooked`) — cooklet and runtime share ONE codec (0.6a); cook/load drift is structurally impossible.
- Cooklet JSON parser: ordered `std::map` objects — deterministic iteration, byte-identical re-cook (0.9 golden proves it).
- stb is cooklet+test-side only; `ve_gfx`/`ve_core` never see it (`texture_roundtrip` proves the runtime loader is a byte-copy path). Scoped warning suppressions documented in `docs/CHEATS.md`.
- MESH cook bakes node TRS and folds `KHR_materials_emissive_strength`; `hp_*` mesh-less nodes become named socket records with sorted-key extras; hull-level metadata rides root-node extras.
- **World-axis convention is an open user decision** (asked twice at the 0.8 checkpoint; timed out). Cooklet currently passes glTF Y-up through unchanged; `glb_cook_poc` asserts baked magnitudes per axis so the assertion survives whichever convention lands. When decided: one transform matrix in `glb.cpp` + assertion update.
- **Tracy: deferred**, not approved (would be the 7th dep). Day-one hook note in PLAN.md amended; adding the profiler is a one-word user decision whenever wanted.

## Exact commands (single source in README)
`cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`, or `./tools/ci_local.sh` (strips DISPLAY/WAYLAND_DISPLAY/XDG_SESSION_TYPE; ASan+UBSan default ON).
