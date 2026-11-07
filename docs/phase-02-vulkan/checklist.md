# Phase 2 — Vulkan right of passage
Status: not started

Decisions that shape this phase: **#35** (ctest gfx on lavapipe via forced ICD; NVIDIA runs are manual/dev), **#36** (window milestone evidenced by Xvfb+lavapipe capture, not a 3090 window), **#37** (Vulkan API floor 1.3; no pre-1.3 code). Architectural rails from AGENTS.md that bind every task below: XR calls stay out of `gfx/`'s core consumers; **nothing renders to "the backbuffer"** — passes write render targets, a *Presenter* presents; ctest green with no display, no compositor, no headset.

## Tasks — execute top to bottom, one commit each
- [x] 2.1 Prereq gate (user runs `sudo apt install vulkan-tools vulkan-validationlayers libglfw3-dev glslang-tools spirv-tools xvfb`) — probe: `vulkaninfo --summary` lists NVIDIA + llvmpipe, validation layer present — commit: —
- [x] 2.2 Volk wiring + `gfx/` module: FetchContent Volk (pinned), new `gfx` static lib (links `core`, includes no XR); surfaceless instance @ API 1.3 + validation layer required — test: `gfx_device_enumerate` (fixture forces lvp ICD per #35; asserts device name contains llvmpipe, API ≥ 1.3) — commit: 955d5c7
- [ ] 2.3 Device + queues: `gfx/device` — physical pick (VE_GPU substring override per #35, default CPU-under-test), graphics queue family (+present only when a surface exists), logical device 1.3 features we actually use — test: `gfx_device_init` (device created on lvp, queue families valid, validation messenger installed and silent) — commit: —
- [ ] 2.4 Presenter seam + offscreen target: `gfx/presenter` interface (target lists in, present out — no backbuffer anywhere) + first impl `OffscreenPresenter` (image/view/fb + CPU readback) — test: `gfx_offscreen_target` (clear-color a 64×64 RT through layout transitions, readback exact) — commit: —
- [ ] 2.5 Cooked shaders: cooklet compiles GLSL→SPIR-V via glslangValidator into a `SHDR` cooked unit (content-hash incremental like everything else); runtime loads only cooked SPIR-V — test: `cook_shader_golden` (two compiles byte-identical; missing tool = clean cook failure), `gfx_shader_module` (cooked SPIR-V → VkShaderModule on lvp; corrupt unit rejected) — commit: —
- [ ] 2.6 Offscreen triangle (Tier 2 golden core): render pass writes the RT; cooked vertex+fragment + push-constant MVP; 64×64 RT — test: `gfx_triangle_golden` (interior pixel exact green, corner pixel exact clear color; edge pixels excluded — cross-driver rasterization is not bit-identical, only deep-interior/exterior pins are) — commit: —
- [ ] 2.7 Validation strictness as policy: debug messenger makes errors/warnings **fail the test** (info filtered); instance refuses to start without the layer — test: `gfx_validation_strict` (a deliberately wrong call trips the messenger and the test sees it; all other gfx tests then carry the watchdog for free) — commit: —
- [ ] 2.8 Desktop WSI presenter (compiles in CI, never runs in CI): GLFW window + `vkCreateWindowSurface` + swapchain (FIFO) + **blit RT→swapchain image** present (the seam from 2.4 stays; the swapchain is just another target), resize → VB + recreate, one frame in flight — test: compile-gate in CI; behavior lands in 2.9 — commit: —
- [ ] 2.9 Rotating triangle + capture evidence (#36): `tools/ve_demo` windowed spin (push-constant rotation, ~120 frames, per-frame ms + avg fps printed) + `tools/dev/xvfb_run.sh` (Xvfb + lvp ICD, demo writes 2 spaced frames + fps log into `docs/phase-02-vulkan/evidence/`); frames differ (rotation), fps stable ≥ 55, validation silent — test: evidence files committed (not ctest; uses a display) — commit: —
- [ ] 2.10 Two frames in flight: fences/semaphores correct on the offscreen path too (frame-sync machinery is presenter-independent), VB-safe resize path — test: `gfx_frame_sync` (10-frame loop over offscreen presenter: fences cycle, readbacks stay deterministic, clean teardown under ASan) — commit: —
- [ ] 2.11 Golden harness + CI integration: minimal golden-PNG compare (stb_image_write, test-side; fuzzy channel tolerance + mismatch %), ci_local.sh preflight checks lvp ICD + validation layer with a clear fail message — test: `gfx_golden_harness` (synthetic pass/fail images pin the comparator), `gfx_triangle_golden` rerun through it — commit: —

## Done-when (every PLAN.md "Done when" bullet linked to a task; all checked = phase DONE)
- [ ] (a) rotating triangle at stable 60fps ← 2.9 (Xvfb+lavapipe evidence per #36; user re-runs on real display later)
- [ ] (b) zero validation errors ← 2.7 strict messenger across all gfx tests + 2.9 capture run silent
- [ ] (c) same scene passes headless as an offscreen golden image ← 2.6 + 2.11 in ctest

## Notes
- Every gfx test fixture forces `VK_ICD_FILENAMES` to lvp **before** `volkInitialize` (loader reads env at ICD-load time) per #35; never assume a display; never touch the 3090 in CI.
- Cross-vendor rasterization is not bit-identical: golden pins live deep inside/outside primitives; harness tolerance covers the rest (2.6, 2.11).
- **OS-neutrality binds every Phase 2 task (decision #38):** `gfx/` and `core/` compile clean on MSVC/Windows — no POSIX headers (`unistd.h` etc.), no unguarded `/proc`, lowercase the same warnings on both toolchains. GLFW/window code lives behind the platform seam; the WSI presenter stays the only swapchain toucher.
- Presenter rule (AGENTS #3) is testable from 2.4 onward: the ONLY thing that ever reads a swapchain image is the WSI presenter itself.
