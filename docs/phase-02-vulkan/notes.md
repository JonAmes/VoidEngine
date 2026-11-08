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
- Vulkan enum suffixes bite under -Werror: `VK_SHARING_MODE_EXCLUSIVE` (not `VK_IMAGE_SHARING_MODE_*`), pipeline stages carry `_BIT` (`VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT`). Same lesson as struct names: check the header.
- Queue family properties struct is **`VkQueueFamilyProperties`** (no `VkDeviceQueueFamilyProperties` — that's a D3D-ish name I reached for); v1 `VkPhysicalDeviceProperties` has no sType either. Vulkan v1 struct naming: check the header, don't guess.
- VE_LOG_* is printf-style (format string first); string concatenation into it = `-Wformat-extra-args`.
- doctest include in this repo is `<doctest/doctest.h>`, not `<doctest.h>`.

## 2.5/2.6 gotchas (learned 2026-11-07)
- `dynamicRendering` is an explicit **device feature** even at apiVersion 1.3 —
  chain `VkPhysicalDeviceVulkan13Features{dynamicRendering=VK_TRUE}` into
  `VkDeviceCreateInfo::pNext` or every `vkCmdBeginRendering` is a validation
  error. The engine enables it at device creation; VkRenderPass objects never
  exist here (decision #37 premise made concrete).
- 1.4 SDK headers **add fields** to 1.3 structs (`resolveMode` landed in
  `VkRenderingAttachmentInfo`). Designated initializers then break across SDK
  revisions (#38: Windows SDK headers may differ in revision). Rule: for
  structs that version across SDK releases, value-init `{}` + explicit field
  assignment; keep designated init for frozen v1 structs.
- `VkClearColorValue` is a union: `.color.float32[i]`, not `.color[i]` or
  `.float32[i]`.
- Presenter owns layout transitions: `begin_command/begin_pass/end_pass/submit`
  on `offscreen_presenter`; a render pass that sees only `color_view()` must
  never barrier the image. `submit()`'s wait-idle is the Tier-2 stand-in the
  2.10 real sync replaces at the same call sites.
- glslangValidator via cooklet popen/_popen shim (`shader.cpp`); success is
  decided by reading back SPIR-V + magic check, never wait-status bits
  (POSIX-only under #38). `VE_GLSLANG` overrides the tool path (Windows SDK).
- Golden pixel-count pins need exact analytic geometry, not mental math:
  triangle area (1/2·base·height)/NDC-area·pixels = 409.6 px; the first
  guess band (>500) failed against the correct 416.

## 2.7 gotchas (learned 2026-11-07)
- The strict messenger caught a latent bug the old stderr-only policy let
  slide: begin_command() called vkBeginCommandBuffer on a buffer alloc_one_shot
  had already begun (VUID-vkBeginCommandBuffer-commandBuffer-00049). Rule:
  allocate+begin helpers (alloc_one_shot) are internal one-op use; caller-facing
  begin_command() = alloc_cmd + exactly one begin.
- vkFreeCommandBuffers on an already-freed handle is itself a validation error
  (00048) — the gate caught the double-free during that fix. Rule: whoever
  allocates frees; submit_and_wait is end+submit+wait only.
- VK_EXT_debug_utils_name macro absent in some header revisions — use the
  string literal. Messenger by design exists twice: pNext chain catches
  vkCreateInstance's own errors, then vkCreateDebugUtilsMessengerEXT persists.
  Messenger must die before the instance.
- With a messenger installed, layer output does NOT also go to stdout —
  messages only reach validation_problems(); tests print them via
  CHECK_MESSAGE so failures self-document.

## 2.8 gotchas (learned 2026-11-08)
- GLFW's Vulkan entry points take **instance first**: `glfwCreateWindowSurface(instance, window, ...)`
  and `glfwGetPhysicalDevicePresentationSupport(instance, window, family)` — opposite of intuition
  from the native-* functions; the compile error is a confusing arg-swap diagnostic.
- `glfwGetRequiredInstanceExtensions` works WITHOUT glfwInit (baked at GLFW build time) — safe to
  call headless; returns empty/NULL where no window system exists, so CI must treat empty as legal.
- CMake: `find_package(glfw3 3.3)` target name is `glfw` (lowercase). Link it PRIVATE on
  ve_platform so no higher layer can include GLFW (window.h keeps the handle as void*);
  volk must be PUBLIC on ve_platform because window.h's public API speaks VkSurfaceKHR.
- Designated initializers demand Vulkan struct **declaration order** AND every member:
  VkPresentInfoKHR is waitSemaphores then swapchainCount THEN pSwapchains (swapping those two is
  a -Werror designator-order error, easy to hit since pSwapchains reads like it comes first).
  VkImageBlit offsets are int32_t (VkOffset3D) — uint32 extents need explicit casts.
- wsi_presenter stores VkPhysicalDevice (not ve::gfx::device): recreate() and memory-type lookups
  must work with handles only; find_memory_type is keyed on VkPhysicalDevice in cmd_helpers.h.
- Swapchain teardown order: image views → vkDestroySwapchainKHR BEFORE the surface (surface belongs
  to platform/window; the presenter must never destroy it). Window teardown: presenter → surface →
  glfwDestroyWindow.
