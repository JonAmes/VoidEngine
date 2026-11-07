# Phase 2 — notes

## Decisions & findings
- **#35 (user, 2026-11-07):** ctest gfx = lavapipe (forced ICD); NVIDIA = manual/dev (`VE_GPU`). Box facts: 3090 carries live inference (22.4/24.5 GB used — do-not-disturb), P5000 is visible again and idle, lavapipe (mesa 26.0.8) installed, loader 1.4, no NVIDIA ICD dependency for CI green.
- **#36 (user, 2026-11-07):** no display exists on this box at all (no seat, no $DISPLAY); the window milestone is proven by Xvfb+lavapipe capture, committed under `docs/phase-02-vulkan/evidence/`.
- **#37 (user, 2026-11-07):** Vulkan floor 1.3 (sync2/dyn-rendering core), 1.4 opportunistic only.

## Prereq probe (2026-11-07, pre-install)
- `libvulkan.so.1` + `nvidia_icd.json` + `lvp_icd.json` present; NO validation layer, NO vulkan-tools/glslang/spirv-tools, NO glfw dev. User runs: `sudo apt install vulkan-tools vulkan-validationlayers libglfw3-dev glslang-tools spirv-tools xvfb`.

## Gotchas
(to fill as tasks land)
