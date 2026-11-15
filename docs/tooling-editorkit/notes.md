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
