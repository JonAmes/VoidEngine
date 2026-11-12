# Phase 8 — Notes

Design notes, findings, and gotchas worth keeping. One section per task number as work lands.

## Phase-shaping decisions on the table
- **8.6 terrain art-direction spike ends in a user decision** (#17d): stylized low-poly PBR + palette cards vs realistic-leaning, chosen with pixels in hand. 8.18 must not commit the surface look before that decision is recorded.
- Everything else in this phase is pre-ratified (#13, #16, #17, #20, #22, #24, #25, #27, #29, #33, #43, #45, #48).

## Pre-existing machinery this phase builds on (grep before building)
- `gfx::ui_context` (3.13): pure-CPU immediate UI; headless panel pattern = `game/scrubber_panel` (ui_io in, draw list out).
- `core/sim/terrain.h` (7.2/7.3): the shared analytic height+normal sampler + stamp stack already exist — streaming and the spike must call it, never a copy.
- `core/sim/loadout*.h` (Phase 5): the loadout editor's entire data model is headless already.
- `core/sim/swept.h` (7.4): the warp-tick contact primitive (bit-exact TOI across speed decades 1..1e12).
- `core/sim/scrubber.h` + `stats_hash` (6.15/6.5), `well_blend`, `rails`, `reservoir`, `assist`, `preview` (ghost-sim): all live.
- Cooklet: TOML parser + hulldef validation + watch mode + manifest; attr-registry grammar 5.4, extended 7.5.
