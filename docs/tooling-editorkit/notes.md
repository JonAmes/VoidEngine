# Editor kit — notes

## EK.1 — UI GPU render pass promoted to gfx::ui_renderer

- Home: `gfx/include/ve/gfx/ui_render.h` + `gfx/src/ui_render.cpp`. The GPU half of
  the old inline harness in `tests/gfx/test_ui_immediate.cpp` (shaders via cooked
  SHDR payloads, pipeline via `pipeline_cache`, solid/text descriptor sets,
  host-coherent VB/IB staging, per-cmd bind/scissor/draw recording) moved verbatim
  into the engine. `create()` takes the two glyph-atlas textures + SHDR payloads;
  `draw()` records a `gfx::ui_frame` into the caller's ACTIVE render pass — no
  backbuffer assumption (decision #15 / VR rules honored).
- Extraction proof: `gfx_ui_immediate` pinned at **1 case / 179 assertions** before
  and after the move, golden `ui_64.png` untouched. The 19 object-creation REQUIREs
  that left the test are replaced 1-for-1 by 17 engine-contract assertions on
  `ui_renderer` itself: corrupt-SHDR create failure (clean error, never reaches the
  driver), degenerate-input guards (false before touching the command buffer), and
  host VB/IB grow-on-demand (frame grown past the 64 KiB floor mid-test; re-render
  byte-identical through the buffer recreate, proven by memcmp + exact pixels).
- **Real bug found during extraction (worth remembering):** `std::expected<T,E>`
  return + defaulted move ctor = silent double-destroy. `return r;` in
  `create()` cannot use NRVO (wrapper type differs), so a real move runs; the
  defaulted move COPIED raw `VkSampler`/`VkPipelineLayout` handles, and the
  moved-from source's destructor destroyed them while the live object still used
  them. Symptom: deterministic SEGV inside `libvulkan_lvp` at submit, only when
  `vkCmdBindDescriptorSets` executed (skipping the bind skipped the crash); the
  validation layer said nothing. Classes with raw handle + owning destructor MUST
  hand-roll moves that null the source (`std::exchange`). `gfx::pipeline_cache`
  sidesteps this by being non-movable; `ui_renderer` holds it via `unique_ptr`.
- Debug tactics that worked without gdb: ASan SEGV logs, env-var bisect guards
  (skip bind / skip draw / only-solid / only-text), and an inline control-object
  probe. Caution: a probe whose owning C++ objects die at scope end while raw
  handles outlive them reproduces the very bug class being investigated and
  taints the bisect — keep probe owners alive for the whole scope.
- Shader modules stay owned by the renderer for its lifetime (pre-refactor harness
  kept them alive for the whole frame; matched rather than relying on spec wording
  about post-pipeline-creation destroy).

## EK.2 — GLFW -> ui_io adapter (060a1b7)
- Home: `platform/include/ve/platform/input_adapter.h` + `platform/src/input_adapter.cpp`. Header is GLFW-free on purpose: ve_glfw links PRIVATE into ve_platform so GLFW.h can never leak; `install_glfw_input(void* glfw_window, ...)` in the .cpp is the ONLY site naming GLFW device codes (same void*-handle convention as `window::glfw_`). ve_platform now links ve_gfx (ui_io is the mapping target, #54d).
- ui_io vocabulary split pinned by test: cursor + left-button are STATEFUL across frames; chars and backspace/enter/escape are per-frame EDGES (drained by next_frame). GLFW_REPEAT dropped at the shim — latched repeat would fight the field; glyph repeat already arrives via the char callback.
- HiDPI: GLFW cursor px are content-space, ui draw space is framebuffer px; `set_frame_scale(content, frame)` scales, content-0x0 (minimize) keeps the last scale.
- utf-8 hand-encoded in the adapter (no codec dependency in the input path, per #15's cooked-everything doctrine); lone surrogates and cp > 0x10FFFF dropped.

## EK.3 — empty-world bootstrap (49d1be8)
- The EK.3 fixture world is `tests/fixtures/world_empty/src/system.toml`:
  one system_class_core system, name "Empty", seed 1, members [] — zero
  bodies/stations/lanes/rows. It opens through the REAL cook path:
  `world_empty` (1c/21a) runs cook_incremental on it, asserts wrldvalidate
  green, decodes via world_load (zero rows), schematic_nodes(0) empty, and
  that a second cook's content hash is stable.
- Production code needed NO changes for the empty world — the cook gate
  already tolerated zero-entity systems; the test pins that it keeps doing
  so.

## EK.4.0 — world system-def schema (4c6ac19)
- `kWorld_system_def_schema` (game/def_schema.h) mirrors the cooklet's
  system-class parse EXACTLY (#43 lock-step): scalars world_class/name/
  seed/members + row tables faction/market/production/lane/waypoint/edge/
  job/job_step/spawn, enum sets world_class + nav_kind. Forms edits of an
  edited system.toml re-cook green through the real gate (`def_schema_world`
  1c/28a: empty fixture + one-row-of-every-kind system).

## EK.4 — ve_edit host (979c243)
- **Mode-B is host-driven, not render()-driven:** `inspector_panel.render()`
  is the ECS-tree side (takes `inspector_data`); the schematic map needs the
  public `render_schematic()` + a `click_schematic(io)` call every frame
  (the panel keeps its own rising-edge select). A host wiring only render()
  shows a blank right half — exactly what the first evidence run caught.
- **Forms are single-string editors** (`edited_field{name,value}`), enough
  for the empty world: only scalars exist, members cell edited as raw
  inline-array text. Row-table add/remove waits for EK.5.
- **Self-attestation over screenshot-reading:** ve_edit exits non-zero if
  the scripted edit never reaches the gate, the gate refuses, the name
  never hits src/ AND baked/, UI ink = 0, last frame == frame 0, or the
  validation layer speaks (2.7 policy, app-side via new
  `WindowedHost::validation_problems()`). PNGs are review artifacts; the
  exit code is the assertion. Independent stdlib PNG decode cross-checks:
  both 1400x900, ~600 unique colors, f0 vs last differ.
- **Work dir is per-PID** (`/tmp/ve_edit_<pid>`): a fixed default would
  inherit a prior run's edited def and desync the scripted erase-count
  (derived from the seeded name length).
- **Driver script** (`tools/dev/xvfb_edit_run.sh`): 2.9 preflight pattern
  (xvfb-run, lavapipe ICD via VK_ICD_FILENAMES+VK_DRIVER_FILES, validation
  layer manifest present), `-screen 0 1600x1000x24`, evidence PNGs into
  docs/tooling-editorkit/evidence/. GLFW init is the only display-dependent
  layer; everything under it stays surfaceless-capable. Never in ci_local.

## EK.5 — remaining panels wired (fb9707e)
- **One inspector_panel cannot show both sides at once:** the panel keeps a
  single hit-rect map and render() clears it every frame — the 8.2 tree side
  and the 8.20 schematic side get separate panel instances over the SAME
  selection_service (shared state lives in the service, view state in the
  panel, per 8.2 doctrine).
- **Browser rows are host-assembled** (8.3 contract): read_manifest on the
  work tree's manifest.bin, per-unit file_size for cooked bytes, and stale =
  core::cooked::sha256(live src) != entry.source_sha. Rebuilt every frame —
  a few entries, dev tool, badges stay honest against disk.
- **Loadout/inspector on an empty world:** set_hulls({}) and an empty
  ecs_world still draw real windows (frames, scroll buttons, "(no live
  selection)") — the panels' empty states are first-class, so EK.5 needed no
  fake data.
- **Handoff spine:** browser selection for a def id the forms is NOT showing
  swaps the forms doc and re-attaches the host gate lambda; same-id
  selection no-ops. The scripted run clicks the real "mrow:world/system.toml"
  rect from the previous frame and asserts selected_id + handoff.
