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
  triangle area (1/2Â·baseÂ·height)/NDC-areaÂ·pixels = 409.6 px; the first
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

## 2.8 gotchas (dev-box pass, merged with the Windows pass below)
- GLFW's Vulkan entry points take **instance first**: `glfwCreateWindowSurface(instance, window, ...)`
  and `glfwGetPhysicalDevicePresentationSupport(instance, device, family)` — opposite of intuition
  from the native-* functions; the compile error is a confusing arg-swap diagnostic.
- CMake: `find_package(glfw3 3.3)` target name is `glfw` (lowercase). Link it PRIVATE on
  ve_platform so no higher layer can include GLFW (window.h keeps the handle as void*);
  volk must be PUBLIC on ve_platform because window.h's public API speaks VkSurfaceKHR.
- Designated initializers demand Vulkan struct **declaration order** AND every member:
  VkPresentInfoKHR is waitSemaphores then swapchainCount THEN pSwapchains (swapping those two is
  a -Werror designator-order error, easy to hit since pSwapchains reads like it comes first).
- Swapchain teardown order: image views → vkDestroySwapchainKHR BEFORE the surface (surface belongs
  to platform/window; the presenter must never destroy it). Window teardown: presenter → surface →
  glfwDestroyWindow.
- **Merged, not duplicated:** 2.8 was implemented independently on both boxes (dev box 1358464,
  this box 372ffce). The merge keeps ONE WSI presenter and ONE seam test (`gfx_wsi_compile_gate`),
  and drops the shared `cmd_helpers.h` the other variant used — `find_memory_type`, `alloc_cmd`,
  `begin_one_shot` and `submit_and_wait` are file-local in each presenter TU again. Folding them
  into one header is 2.10's job, where frame-sync machinery becomes presenter-independent on
  purpose; refactoring it now would put the green Tier-2 path at risk for cosmetics.

## 2.8 gotchas (learned 2026-11-07)
- GLFW's Vulkan entry points are inside `#if defined(VK_VERSION_1_0)`, and
  `glfw3.h` only pulls `<vulkan/vulkan.h>` in itself under `GLFW_INCLUDE_VULKAN`.
  Including `<volk.h>` first (via `platform/window.h`) makes GLFW's Vulkan API
  visible with no extra define — so no SDK header path is needed in platform/.
  There is no `glfwDestroyWindowSurface`: GLFW made the surface, the caller
  destroys it with `vkDestroySurfaceKHR` before the VkInstance dies.
- GLFW must be initialized before `glfwGetRequiredInstanceExtensions` can answer — GLFW 3.4
  `src/vulkan.c` guards it with `_GLFW_REQUIRE_INIT_OR_RETURN(NULL)`, so pre-init it returns
  **NULL with `*count = 0`** (the null-platform backend is not the only reason for NULL: no
  Vulkan backend compiled in gives NULL too, which is why `window::required_instance_extensions()`
  checks the pointer, not just the count). An earlier note here claimed it works without
  `glfwInit` "baked at build time" — that is wrong against 3.4 source, corrected on 2026-11-07.
  The window must also exist before `glfwCreateWindowSurface`. Hence the seam: `window::create` →
  `required_instance_extensions()` → `instance::create(app, exts)` →
  `window::create_surface(inst)` → `device::create(inst, surface)`.
- `instance::create` gained `extra_extensions` (deduped against
  VK_EXT_debug_utils). gfx/ still names no platform surface extension: the names
  arrive from GLFW through the platform seam, so win32/xlib/xcb/wayland stay
  invisible to gfx/.
- Blit legality decides the format, not taste: `vkCmdBlitImage` between optimal
  images requires **format-compatible** images, and `B8G8R8A8_UNORM` vs
  `B8G8R8A8_SRGB` is not format-compatible. So the swapchain format IS the
  render-target format, chosen once at create and reused for every view. Only
  8-bit RGBA is accepted at all, because `capture_rgba8()`'s 4-bytes-per-pixel
  readback assumes it — anything else is a hard stop naming the candidate count.
- `minImageCount + 1`, clamped by `maxImageCount` (0 = unlimited): asking for
  exactly `minImageCount` deadlocks acquire/present overlap. Extent is clamped
  into `[minImageExtent, maxImageExtent]`, never trusted from the caller.
- Staleness is never swallowed (2.8's resize contract): OUT_OF_DATE fails the
  call, SUBOPTIMAL returns true (the frame DID go up) but sets `needs_recreate()`.
  Stale-at-acquire DROPS the frame — the acquire semaphore may already be
  signaled with nothing waiting on it, which is why `recreate()` rebuilds the
  sync objects and not just the chain. `recreate()` refuses while an image is
  held: present or abandon the frame first.
- `VkImageBlit::srcOffsets/dstOffsets` are `int32_t` while the extent is
  `uint32_t`: implicit narrowing in a braced initializer is a hard error on both
  toolchains (GCC `-Wnarrowing`, MSVC C2397). Cast written out.
- **#38 gap found by the MSVC compile gate:** `log.h` declared `log_msg` with a
  bare `__attribute__((format(printf,2,3)))` — MSVC has no `__attribute__`, and
  every gfx/ TU includes that header, so nothing in gfx/ compiled on Windows.
  Fixed with `VE_PRINTF_FORMAT(fmt,args)`: GCC/Clang keep the check, MSVC
  compiles the same declaration without it.
- Windows evidence available today (MSVC 14.51, `/W4 /permissive-`): all touched
  TUs compile clean; the gate case links against a real static GLFW and runs
  green; a scratch driver opened a 640×360 window and got
  `VK_KHR_surface` + `VK_KHR_win32_surface` from GLFW, then `instance::create`
  refused cleanly — this box has 7 instance layers and NONE of them is
  VK_LAYER_KHRONOS_validation, so the 2.2 required-layer policy stopped it.
  A live WSI frame on Windows therefore needs the SDK installed; the real
  behavior evidence is 2.9's Xvfb+lavapipe run (#36).
- The MSVC **build mode** #38 needed landed right after (45e7ef2), not as a
  softening: GCC/Clang keep `-Wall -Wextra -Wpedantic -Werror`, MSVC gets
  `/W4 /permissive- /Zc:preprocessor /WX`. What that mode exposed, in order:
  unknown options to `link.exe` are *warnings* and the wall makes warnings fatal
  (stb's TU suppressions, `-rdynamic`); doctest 2.4.11 trips VS18's new C5285;
  `core/src/crash.cpp` is POSIX by construction (`sigaction`,
  `backtrace_symbols_fd`, `execinfo.h`) so it and its test are excluded on MSVC
  rather than stubbed into a handler that dumps nothing; `VE_COOK_TOOL` was
  hardcoded to `tools/cook/ve_cook` — Ninja's layout, no `.exe` — now
  `$<TARGET_FILE:ve_cook>`; sanitizers are reported OFF out loud on MSVC instead
  of silently skipped.
- **A test bug the portability seam caught:** `test_glb.cpp` drove the cook CLI
  through `std::system` with a fully quoted line, and cmd.exe's `/c` parser
  **strips a leading quote pair** — the child never ran, the redirect file was
  never written, and cmd returned 1 on its own. Every "nonzero exit" assertion
  passed for the wrong reason. Measured: rc=1 with no stderr file; with one
  extra quote pair (`ve_test::shell_cmd`) rc=0 and the file appears. POSIX sh has
  no such quirk, so the helper is a no-op there. `cook_determinism`'s manifest
  lookup also failed only on Windows: MSVC keeps a literal leading slash from a
  path literal and appends a backslash, so neither reconstructed prefix spelling
  equalled the real key — now matched on the dir-relative suffix.
- `cook_shader_golden` gates on a **probed** GLSL compiler (`ve_test::tool_on_path`),
  not on the cooklet's error text: that text also matches a real GLSL error, so
  parsing it would let a genuine bug skip. The missing-tool *cook* failure path
  stays asserted unconditionally. The probe's Windows half was itself wrong at
  first and the fix is the lesson: it asked a shell for `where`, which lives in
  System32, and a compiler-toolchain environment can leave that directory off
  PATH — measured inside a vcvars-imported environment, `where` itself was not
  found, so `std::system` returned 1 for a GLSL compiler that exists, is on PATH
  and demonstrably works (2.9's demo cooked two units with exactly that tool).
  A probe failing for reasons unrelated to the tool is worse than no probe: it
  silently deletes coverage and points at the wrong fix. Now a name containing a
  separator is probed as a FILE via `std::filesystem::exists` (`VE_GLSLANG` may be
  absolute); a bare name gets an own PATH scan across `""` and `.exe`/`.bat`/`.cmd`
  (cmd resolves bare names through PATHEXT, and SDK wrappers ship as .bat/.cmd).
  POSIX keeps `command -v`, which is a shell builtin and so carries none of this
  fragility.
- Windows tally as measured (MSVC 14.51 / VS18 BuildTools, VS-bundled CMake +
  Ninja): `engine_tests` **40 cases / 427 assertions green**, `ctest` 1/1, exit
  0. The shader golden now **executes** for the first time since the probe fix
  (7711df4), which is where 407 → 427 came from. The gap to the last Linux tally
  (41/518 at 2.7; +1 case for the WSI gate since) is structural and named:
  `core/test_crash.cpp`'s two cases (`crash_dump writes a labelled stack dump to
  the log sink`, `installed handler dumps and re-raises so the child dies by the
  signal`) are not compiled on MSVC at all.
- Read the Windows case count carefully: the 8 Tier-2 gfx cases (including the
  WSI gate's live half) **run and pass** after printing a skip reason
  ("no lavapipe ICD on this box"), because those fixtures return early rather
  than calling doctest's `SKIP()` — so doctest reports `0 skipped`. A passing
  case count there is not evidence of GPU coverage; the assertion counts are.
  That is #36's tier logic working as designed, not a loophole.
- Stale claim corrected: the earlier note said this box has "no validation
  layer". It does have one — `VK_LAYER_KHRONOS_validation` is reachable and was
  active for 2.9's run with 0 problems. What this box lacks is a **lavapipe
  ICD**, which is the only reason Tier-2 still skips here.
