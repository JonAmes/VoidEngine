# Phase 2 — notes

## Decisions & findings
- **#35 (user, 2026-11-07):** ctest gfx = lavapipe (forced ICD); NVIDIA = manual/dev (`VE_GPU`). Box facts: 3090 carries live inference (22.4/24.5 GB used — do-not-disturb), P5000 is visible again and idle, lavapipe (mesa 26.0.8) installed, loader 1.4, no NVIDIA ICD dependency for CI green.
- **#36 (user, 2026-11-07):** no display exists on this box at all (no seat, no $DISPLAY); the window milestone is proven by Xvfb+lavapipe capture, committed under `docs/phase-02-vulkan/evidence/`.
- **#37 (user, 2026-11-07):** Vulkan floor 1.3 (sync2/dyn-rendering core), 1.4 opportunistic only.

## Prereq probe (2026-11-07, pre-install)
- `libvulkan.so.1` + `nvidia_icd.json` + `lvp_icd.json` present; NO validation layer, NO vulkan-tools/glslang/spirv-tools, NO glfw dev. User runs: `sudo apt install vulkan-tools vulkan-validationlayers libglfw3-dev glslang-tools spirv-tools xvfb`.

## 2.1 probe evidence (2026-11-07, post-install)
- `vulkaninfo --summary`: **3 devices** — NVIDIA GeForce RTX 3090 (api 1.4.312, driver 580.178.4), **Quadro P5000 visible again and idle** (same driver), llvmpipe (LLVM 21.1.8, 256 bits) = PHYSICAL_DEVICE_TYPE_CPU, api 1.4.335, driver 26.0.8. Loader 1.4.
- `VK_LAYER_KHRONOS_validation` 1.4.341 installed; `glslangValidator`, `spirv-val` present; **GLFW 3.4.0** via pkg-config; Xvfb present.
- P5000 re-appearance flips the #35 note about "no NVIDIA ICD dependency" — real-GPU dev runs now have a *free* 16 GB device (`VE_GPU=P5000`), keeping the 3090 untouched.

## Gotchas
- **volk tags follow Vulkan SDK releases** (`vulkan-sdk-1.4.313.0`), not semver — `v1.4.0` does not exist. Pinned at/above the box's loader (1.4.312); volk compiles clean under our full wall, no `-w` cheat needed.
- `VkPhysicalDeviceProperties` (v1) has **no sType member** — designated-init `.sType=` fails to compile; plain `{}` value-init is correct.
- volk's `volkInitialize()` covers instance-independent commands only (layer/ext enum, `vkCreateInstance`); instance+physical commands need `volkLoadInstance(handle)` immediately after create.
- VE_LOG_* is printf-style (format string first); string concatenation into it = `-Wformat-extra-args`.
- doctest include in this repo is `<doctest/doctest.h>`, not `<doctest.h>`.
