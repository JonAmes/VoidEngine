# Blender → GLB Proof of Concept — Findings

Date: 2026-11-06 · Blender 4.5.15 LTS (glTF I/O v4.5.51) · PoC, not part of the engine build

## What was tested
Built a small arcade freighter programmatically (`make_test_ship.py`): 7 primitives
(hex fuselage, nose cone, cockpit bubble, 2 wings, 2 engine nacelles, 376 tris),
4 PBR materials (metal hull, painted wings, emissive engines, BLEND-alpha cockpit),
5 named hardpoint sockets as empties (`hp_engine_L` …), a root node with custom
properties (`hull_class`, `mass_t`), cube-projected UVs. Exported to `test_ship.glb`
and parsed it with a stdlib script (`inspect_glb.py`) the way the future cooklet would.
Rendered a headless preview (`preview.png`).

## Setup (no root)
- Blender installed as an official tarball to `~/opt/blender-4.5.15-linux-x64/` (apt/sudo not available; tarball needs nothing else).
- Fully headless: build, save, export **and EEVEE Next rendering** all work under `blender --background --python …` with no display. Useful later for CI-side asset validation/preview thumbs.

## Findings that matter for the cooklet

1. **GLB structure matches our cooked-container design.** 2 chunks (JSON + BIN), 21.7 KB total for 376 tris (14.1 KB raw vertex data ≈ 38 B/prim-index). Rule of thumb: an un-compressed cooked blob runs roughly ~130 bytes per triangle with POSITION+NORMAL+UV+index; a 100k-tri hull ≈ 13 MB raw, readily compressible.

2. **Auto-triangulation happens at export.** Every primitive is `mode 4 (TRIANGLES)`; Blender quads/n-gons never reach the file. The cooklet needs **no triangulation stage** — but must not assume "primitive" == "quad".

3. **Coordinate space flips to Y-up.** Blender authored the 5.0-long fuselage along Z; the GLB has it along Y (bounds ±2.5 on Y, verified). glTF's spec is Y-up and Blender's exporter honors it by default. Decide at cook time: keep `export_yup=True` (spec-conformant, works with every DCC) and convert to engine axes **in the cooklet** — recommended — or export with `export_yup=False` as a project convention. Units: Blender 1 unit = 1 m, glTF convention = meters → no scale conversion needed if we keep scene scale 1.0.

4. **Node names survive; mesh names don't.** Mesh data blocks export as `Cylinder.001` etc., but **node** names keep author names (`Fuselage`, `Wing_L`). The cooklet must key content off **node names**, not mesh names.

5. **Hardpoint sockets work exactly as planned.** Empties export as nodes with **no `mesh` field**, names and parent links preserved, and per-socket custom properties land in node `extras` (`{"socket_type": "engine"}`). This is Phase 5's "named hardpoint sockets" model carried natively by the format — a hull `.glb` can self-describe its sockets.

6. **Custom properties → `extras`** (needs `export_extras=True`). Root node carried `hull_class: "light_freighter"`, `mass_t: 84.0`. Good channel for hull metadata without a sidecar file.

7. **Hierarchy preserved.** `HullRoot → Fuselage → {Nose, Cockpit, Wing_L/R, Engine_L/R, sockets}` came through as a node tree with TRS. Multi-part hulls (Phase 5) map 1:1. Cooklet can flatten transforms into one mesh at cook time or keep the tree for per-part damage bounds.

8. **Materials map 1:1 onto glTF's PBR model.** Principled BSDF base color / metallic / roughness / alpha / emission export exactly as `baseColorFactor`, `metallicFactor`, `roughnessFactor`, `alphaMode: "BLEND"`, `emissiveFactor`. No textures were authored (pure factors) — image textures inside the BIN chunk are the one untested gap; the Phase 0 sample set (cube + PNG) covers it.

9. **Two exporter quirks to handle at cook time:**
   - `doubleSided: true` on **every** material (Blender default). We generally want backface culling; cooklet should own this flag rather than trust the export.
   - Emission **strength** (we used 3.0) is exported via `KHR_materials_emissive_strength` (used but not required). Fold strength into the emissive factor in the cooked material and the extension disappears from our concerns.

10. **No tangents exported.** Attributes present: POSITION, NORMAL, TEXCOORD_0 only. If we ever want normal-mapped hulls, the cooklet must bake tangents (MikkTSpace) itself.

11. **Determinism verified.** The same `.blend` exported 4× across 2 separate Blender processes → **byte-identical SHA-256** (`4320498d…`). Blender's glTF export won't break the plan's "two cooks byte-identical" gate.

12. **No compression by default.** No Draco/Meshext; the BIN chunk is raw. Buffer compression (if wanted) is the cooklet's job, not the exporter's.

## Incidental gotcha found while scripting (worth remembering)
Parenting via `obj.parent = …` after `primitive_*_add(location=…)` silently zeroes nothing, but setting `obj.location` **after** parenting overwrites the world position — first render came out as a collapsed blob. Scripting convention: create at final position, then parent, never rewrite location after parenting.

## Verdict
Blender headless → `.glb` is fully compatible with the planned `assets/ → tools/cook → cooked/` pipeline. Sockets, hierarchy, PBR materials, and metadata all survive export in places the cooklet can read deterministically. No new dependency needed (Blender is a DCC tool, never linked into the engine). Next steps when Phase 0 lands: run this exact `test_ship.glb` through `tools/cook` v0 as a second sample asset, and add a PNG-textured variant to close the image-encoding gap.

## Files in this folder
| file | what |
|---|---|
| `test_ship.blend` | the source model |
| `test_ship.glb` | export under test (21.7 KB, 376 tris, 4 materials, 6 socket/root nodes) |
| `preview.png` | headless EEVEE Next render |
| `make_test_ship.py` | builds the model, saves .blend, exports .glb, renders — reruns end-to-end |
| `inspect_glb.py` | stdlib GLB parser; the read-side check |
