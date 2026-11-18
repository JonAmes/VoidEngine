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
