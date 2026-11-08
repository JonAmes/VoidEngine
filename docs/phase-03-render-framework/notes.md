# Phase 3 — Notes

Running design record for the mini render framework. Process rules live in AGENTS.md; task state lives in `checklist.md`.

## Where Phase 2 left off (inputs)
- `gfx/`: surfaceless `instance` (validation required + strict messenger), `device` (1.3 features chained explicitly), `frame_sync` (presenter-independent), `presenter` seam with `offscreen_presenter` (image/view/fb + `capture_rgba8`) and `wsi_presenter` (RT → blit → swapchain).
- Cooked units already loadable by content ID: `SHDR` (SPIR-V), `TEX0` (RGBA8), `MESH` (pos/nrm/uv + tris, node TRS baked), `MAT0` (color/metallic/rough/emissive/alpha), `manifest.bin` (deps + source hash). `core/cooked.h` documents each payload layout.
- Golden harness: `tests/gfx/gfx_golden` — strict on lavapipe (byte-exact), `VE_GOLDEN_UPDATE=1` regen, `.actual.png` dump on mismatch. Fixtures force `VK_ICD_FILENAMES` → lvp **before** `volkInitialize` (#35).
- `test_ship.glb` cooks to 3 meshes + 2 materials + 5 `hp_*` socket units (Phase 0 gate).

## Known gaps feeding this phase (verified in code, not assumed)
- `MESH` payload has no tangents — `tools/cook/src/glb.cpp` reads POSITION/NORMAL/TEXCOORD_0 only; #14 makes tangent bake a cooklet job → task 3.6.
- `MAT0` has no texture refs; `glb.cpp` ends its mesh pass with `// glb->texture deps land with material textures (later task)` → task 3.8.
- No FONT0 type, no UI, no depth beyond the triangle pass → 3.12, 3.13, 3.3.

## Deferred design questions (asked at the next checkpoint, per user "defer questions till later")
- **Q1 dynamic rendering.** All new Phase 3 passes use `vkCmdBeginRendering` (VK1.3 core per #37) and new render targets skip legacy `VkRenderPass`/`VkFramebuffer`. Migration of the Phase 2 offscreen clear/triangle plumbing happens inside 3.3 where depth joins the target, so there is exactly one pass-plumbing story by phase end. Risk: low — lavapipe 1.4 supports it; goldens may shift if the old path's implicit state differed (caught by strict goldens + validation).
- **Q2 MESH version bump.** Tangent bake bumps the MESH payload format 0.8→0.9 in place (fourcc unchanged, payload version rides the unit `format` field discipline); committed mesh goldens/tests regenerate in the same commit. Alternative (new fourcc `MES1`) rejected as needless format sprawl while nothing but the in-repo loader consumes MESH.
- **Q3 font asset.** stb_truetype is already in the pinned stb package (`build/_deps/stb-src/stb_truetype.h` — no new dependency). A test font must be vendored (`assets/fonts/`) so cooking is hermetic; default DejaVu Sans (Bitstream Vera license, free use/redistribution — CHEATS/README line if it stays in the slice, not just tests).
- **Q4 stereo evidence layout.** One wide RT (e.g. 2×64×64 side-by-side) with per-eye viewport+scissor proves the per-eye flow *and* gives per-eye golden crops. Two separate offscreen targets would prove less plumbing.
- **Q5 uniform strategy.** Push constants ≤256 B can't hold per-material PBR params across instanced draws: UBO per frame (lights + per-eye MVP array + material table), push constants for the instance transform basis. Boring and debuggable.

## Per-task decisions (recorded as they happen)
- **3.2 camera:** `camera_rig` = rig TRS + ordered `eye_def` list (authored order = instance eye index for 3.9 — never hash order). `compute()` is pure: per eye, `view = inverse_rigid(trs_compose(rig) * trs_compose(eye.rig_local))`, `proj = perspective_offaxis(n,f,L,R,B,T)`; desktop is a 1-eye rig through the same path. `frustum` carries tans only (fov/aspect is input sugar). `converged_stereo_frustums`: eyes at ±ipd/2, no toe-in; derivation `clip.x=0 ⇒ x/z = -(r+l)/2` with fixation at eye-space (-ipd/2, 0, -focal) gives `r+l = -ipd/focal_dist` (right eye; left mirrors) — verified: fixation on rig axis → ndc.x == 0 in both eyes, width/verticals stay symmetric. All camera math fp32 (rig space = floating origin, decision #3).

## Gotchas (append as found)
- doctest `Approx` has no `.margin()` (v2.4): repo float-comparison convention is `approximately_equal(a, b, eps)` from core math; vec4-from-vec3 is `vec4f::point(p)`.
