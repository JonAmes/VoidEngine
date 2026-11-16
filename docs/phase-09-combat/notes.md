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
