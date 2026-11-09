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

- **3.3 depth:** every `offscreen_presenter` target owns a mandatory `VK_FORMAT_D32_SFLOAT` depth image (usage DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC) + readback staging buffer. It is **pass plumbing**: the consumer surface stays `color_view()`/`capture_rgba8()`; `depth_view()` exists for pass authors, `capture_depth_f32()` for pins only. Presenter bookkeeps `depth_layout_` exactly like color (UNDEFINED→DEPTH_ATTACHMENT under LOAD_OP_CLEAR, DEPTH_ATTACHMENT→TRANSFER_SRC at end_pass). Reversed-Z convention: passes clear depth to **0 == far** and compare **GREATER** (pairs with Phase 1 `reversed_1_to_0`/`reversed_inf_far` builders; precision rides the near plane where VR needs it).
- **3.3 pin derivation** (n=0.5, f=20, reversed_1_to_0: `clip.z = z·n/(f−n) + f·n/(f−n)` = `z/39 + 20/39`, w = dist): d=2 → ndc.z 3/13, d=4 → 4/39, clear → exactly 0. FOV-90 aspect-1 quads: ndc.x = x_view/dist exactly (t=r=1), so coverage px bands are hand-derivable — ring pins sit ≥3 px from quad edges.

- **3.4 pipeline cache:** `pipeline_desc` is the engine-side canonical description (only state this engine uses is representable — no tessellation, no legacy renderpasses; single-sided stencil). Key = byte-exact serialization into an **ordered** `std::map` (no hashing, no unordered iteration: teardown/stats deterministic). Creation always chains `VkPipelineRenderingCreateInfo` (dynamic rendering only). `view_mask` is part of the desc from day one (multiview per-eye bits, VR rule 2). Cache never owns modules/layouts — documented ownership: desc handles must outlive cached pipelines; cache destructs before device.

- **3.5 mesh load/upload:** payload vertex data is **planar** (`f32 pos[vc*3] | nrm[vc*3] | uv[vc*2]`) — parser deinterleaves into 32 B `mesh_vertex`; reading interleaved silently mis-pins values (caught by synthetic round-trip). Manifest is the authority: `.ck` on disk + id not in `manifest.bin` ⇒ load error. Every count is validated against remaining bytes *before* any `resize`. PoC hull = **7 mesh nodes** (glb_cook_poc `mesh_total == 7`), not 3. `gpu_mesh` keeps TRANSFER_SRC usage now so the later device-local switch is a memory-flag change only.
- **3.6 tangent bake:** hand-rolled triangles-only MikkTSpace port (`tools/cook/{include/ve/cook,src}/mikktspace.*`), reference license text kept at top of the .cpp. Triangles-only is safe because the cooklet enforces primitive mode 4 (TRIANGLES) — the reference's quad-split/quad-degen paths are unreachable. Determinism deviations (all PORT-NOTED in-file): `std::sort` replaces the reference's randomized-pivot quicksorts (total order on distinct keys ⇒ identical sequence); the grid-merge weld becomes a `std::map` over the 8 attribute floats (same equivalence relation, same first-in-scan-order representative); reference `assert`s become guards (corrupt glTF fails the cook, never aborts it).
- **3.6 handedness contract (the subtle one, verified against reference line-by-line):** the reference's eq-18/19 `fS` factor is applied to BOTH vOs and vOt, so the final `vOs/vOt` are the TRUE dP/du derivatives and `bOrient` alone carries winding parity. Consequence: with a winding-consistent normal, `vOt == cross(N, vOs) * bOrient` — so the payload stores `w = bOrient` and shaders reconstruct **`bitangent = cross(normal, tangent) * w`** (glTF's formula; an earlier header draft had `cross(T,N)*w` — wrong cross order, caught by the flip fixture before commit). Unreferenced and zero-uv-area vertices fall back to the reference default space `(1,0,0,−1)` — pinned, not accidental. glb.cpp now also bounds-checks every index `< vc` before emitting (the payload used to ship unchecked indices).

## Gotchas (append as found)
- doctest `Approx` has no `.margin()` (v2.4): repo float-comparison convention is `approximately_equal(a, b, eps)` from core math; vec4-from-vec3 is `vec4f::point(p)`.
- `-Werror=missing-field-initializers` fires on *designated* initializers too: every Vk* struct in tests must name all fields (copy the triangle test's fully-explicit style).
- Barrier src stage must match the src access being declared: `TRANSFER_READ` → `TRANSFER_BIT` stage (TOP_OF_PIPE rejects it, VUID-…-02819). When a layout transition's old layout is UNDEFINED, declare no src access at all and use TOP_OF_PIPE.
- This doctest build has **no `CHECK_CLOSE`** (macro undeclared; `CHECK(a == doctest::Approx(b))` or a local `near()` helper instead) and **forbids `||` inside `CHECK`** (static_assert "Expression Too Complex" — write `CHECK(std::fabs(w) == 1.f)` for "exactly ±1").
- Probe expectation bugs outnumber port bugs in tangent-space work: pin fixtures against the REFERENCE's algebra (read the fS sign handling), not intuition. "Wrong" probe answers here (size 6, tangent +1) were both fixture errors; the one real port bug (cross order in the header comment) was caught by a winding+normal-flipped fixture, not a flat one.

- **3.7 GPU textures:** `gpu_texture::create(device, CpuTexture)` is one-shot staging→optimal (host-visible staging buffer → `vkCmdCopyBufferToImage` into a `VK_IMAGE_TILING_OPTIMAL` `R8G8B8A8_UNORM` image; sRGB tagging deferred so readback pins stay byte-exact). The **owner does every layout transition** (UNDEFINED→TRANSFER_DST→SHADER_READ_ONLY, plus TRANSFER_DST again per mip-capture) via a private barrier helper — consumers may not barrier texture images, same policy as presenter-owned targets. **Mip chain is generated at upload**: level count `1 + floor(log2(max(w,h)))` computed by testing `mip_dim(level-1) > 1` (testing the candidate level itself drops the 1×1 tail), each level built by a spec-conformant `vkCmdBlitImage` LINEAR blit from the level above. Sampler is static: NEAREST mag/min/mip (byte-exact LOD-0 pins), REPEAT all axes (glTF default + 3.6 seam-welded UV space), `maxLod = VK_LOD_CLAMP_NONE` — LOD policy itself is later material/pass work, the chain just has to exist. Descriptor plumbing (`gfx/descriptor.h`) is deliberately thin: one `COMBINED_IMAGE_SAMPLER` binding per set, `pImmutableSamplers = nullptr` (the sampler rides the descriptor write), one pool, no bindless — grows when 3.9/3.11 need UBO/storage writes. `capture_mip_rgba8(level)` is the owner readback used for pins (test-only path, like `capture_depth_f32`).
- **3.7 fixture design (blit mid-round trap):** with 255-valued channels the 2→1 LINEAR average is 127.5 at an *intermediate tap* too, and lavapipe rounds channels inconsistently (measured mip1 = 127,128,128 for identical R/G/B sums). Fix the FIXTURE, not the expectation: lit channels = 254 makes every tap a whole number (average exactly 127), so the pin is driver-independent rather than a pinned vendor rounding quirk. Same lesson as 3.6 pins: pin against exact algebra, not against a driver's observable accident.

## Gotchas 3.7 (append as found)
- **Missing `vkBindImageMemory`/`vkBindBufferMemory` after `vkAllocateMemory` segfaults lavapipe mid-suite** — not a validation error, an ASAN SEGV whose top frames are inside libc (looks unrelated). Every allocation in `texture.cpp` missed the bind; `mesh.cpp`/`presenter.cpp` had them. `grep -c vkBind gfx/src/*.cpp` is a 5-second sanity check for new GPU-resource code.
- For **manual lavapipe probes** (outside ci_local.sh) the working manifest here is `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`; the arch-suffixed `lvp_icd.x86_64.json` makes `vkCreateInstance` fail with `VkResult(-9)`. Raw probes against `build/gfx/libve_gfx.a` need `-fsanitize=address,undefined` at link (the libs are sanitizer-built).
- `VkBufferImageCopy` in the installed header uses **`bufferImageHeight`**, not `bufferImageStride` (VK 1.1-era name; the stride field only exists via `VkSubresourceLayout2`/alignment idioms).
- C++23 **designated initializers must appear in declaration order** — `.dstAccessMask` after `.subresourceRange` in a `VkImageMemoryBarrier` is a hard error, not a warning.
- `vkUpdateDescriptorSets(dev, writeCount, writes, copyCount, copies)` — passing `0, nullptr, &w, 0` puts the write in the *copy* slot (silent, then descriptor garbage).
- glslang needs **explicit `layout(location=…)` on vertex outputs / fragment inputs** that cross the stage boundary — "SPIR-V requires location" fails the shader compile; don't rely on auto-assignment.
- doctest also rejects **`&&` inside `CHECK_MESSAGE`** (same "Expression Too Complex" as `||`): compute a `bool ok = …;` then `CHECK_MESSAGE(ok, …)`.

- **`engine_tests` is not safe to run twice at once.** Fixture scratch lives at *fixed* paths under
  `%TEMP%` (`ve_test_log_levels.log`, `ve_golden_33`, `ve_mesh_ship_1`, `ve_shdr_out1_2`), so two
  live processes of the same binary collide inside `std::filesystem::remove`/`remove_all` — measured
  **8/8 failures across 4 concurrent pairs** ("The process cannot access the file because it is being
  used by another process", "Access is denied", plus one genuine `REQUIRE( ship.has_value() )` failure
  when one process deleted a directory the other was mid-write) against **10/10 clean sequentially**.
  The tell is a red run whose *assertion total differs* (2551 / 2538 / 2518 vs the real 2568) — a
  throwing fixture bails mid-case, so later assertions in it never run. Caused here by launching
  `ctest` and `engine_tests.exe` side by side, not by the tree. Not an engine defect; if it ever
  needs fixing, make those fixture paths pid-unique.

## Windows run-through at 3.7 (2026-11-08 — MSVC 14.51 / VS 18 BuildTools, VS-bundled CMake 4.3)
Configure, build, `ctest`, and the windowed demo all green on this box. Facts that changed the
run-sheet (README's Windows paragraph said "same three commands" — it no longer is):
- **CMake 4 vs the pinned deps.** VS 18 bundles CMake 4.3, which hard-refuses doctest 2.4.11's
  `cmake_minimum_required(VERSION 3.0)`. Fixed at configure time, not by bumping a pinned dep
  (bumping doctest is its own decision): `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. The Linux gate's
  CMake 3.31 never sees this, so the divergence lives in the Windows invocation only.
- **volk needs Vulkan headers.** This box has VulkanRT (runtime) and no SDK, so volk's include
  dir came up empty → `C1083: vulkan/vk_platform.h`. Fix:
  `-DVULKAN_HEADERS_INSTALL_DIR=C:\Users\AmesL\Projects\.ve-deps\Vulkan-Headers` — a clone at tag
  **v1.4.313**, exactly volk's pinned `vulkan-sdk-1.4.313.0`, so the generated loader and the
  headers agree. No new dependency: headers the box already had, just named to CMake.
- **The default generator under a dev shell is Visual Studio, not Ninja** (Ninja on PATH doesn't
  change it) → multi-config → `ctest --test-dir build -C Debug`. Without `-C`:
  `Test #1: engine_tests ***Not Run` — a configuration error wearing a test-failure costume.
- **PowerShell argument mangling:** unquoted `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` reaches cmake as
  `-DCMAKE_POLICY_VERSION_MINIMUM=3` + `.5`; the bad value lands in `CMakeCache.txt` and the
  `CMAKE_POLICY_VERSION_MINIMUM` env var does *not* override the cache, so the failure outlives
  the typo. Quote `-D…=x.y` args.
- Tally as measured: `engine_tests` **52 cases / 2568 assertions, 0 failed** with `VE_GLSLANG` set
  (2548 assertions when no GLSL compiler is reachable — `cook_shader_golden` prints its skip and
  returns), vs the Linux gate's 54/3091 — the 2-case gap is the crash suite, not compiled on MSVC
  (CHEATS line). Tier-2
  gfx halves print "no lavapipe ICD on this box" and return, so the case count stays green with
  the GPU coverage absent; same #36 caution as before.
- **Validation-layer reachability moved since Phase 2.** The Phase 2 note said the HKCU
  `ExplicitLayers` registration made the layer reachable. Measured now: the loader (instance
  1.4.341) never surfaces that manifest — `vkEnumerateInstanceLayerProperties` lacks it, and the
  engine's required-layer gate stops `ve_demo` at exit 2 with "Vulkan validation layer not
  installed". `VK_LAYER_PATH=C:\Users\AmesL\Projects\.ve-deps\vvlbin` makes it enumerable; the
  demo then logs `validation ON` and `active tool layers: Khronos Validation Layer`.

## ve_demo at 1920x1080 — run 2026-11-08 at user request (Windows/NVIDIA)

`ve_demo` is the repo's only resolution-parameterized harness; the ctest goldens are fixed
64x64 lavapipe-only fixtures (#35/#36), so 1080p applies to the windowed run, not the gate.
Both runs **exit 0**: 120/120 frames presented, **0 validation problems** (Khronos layer
active, device api 1.4.351), extents 32-bit BG, 2 images in flight, avg 62.24 fps (no resize)
and 63.78 fps with `--resize-at 60` — that second run took **2 clean swapchain rebuilds**
(1920x1061 -> 1792x989@f60 -> 1920x1061@f110, 0 acquire refusals, 3 distinct images).
Evidence committed at `docs/phase-02-vulkan/evidence/windows-nvidia-1080p/` (dev-run only per
#35; the lavapipe capture stays primary, the committed 2.9 evidence is untouched).

- **Requested size != presented extent, and that is correct.** Asking for 1920x1080 yields a
  client area of **1920x1061** — Win32/DWM takes rows for the window frame (display 1920x1080,
  work area 1920x1032 with the taskbar). Swapchain extent, render target, readback and PNGs all
  follow the client area, which is the WSI contract: the extent comes from the surface, not from
  the requested window size. `summary.txt` prints `1920x1061` while the command asked 1920x1080;
  that is the discrepancy explained, not a bug. For a presentation that is exactly 1920x1080 the
  client area must be exactly that (auto-hide taskbar / borderless / taller display, or a hint in
  `platform/window.cpp`) — a small `platform/` change with WSI-test implications, deliberately
  **not** made.
- Geometry scales with the extent as expected: triangle coverage 6.63% of 1920x1061 vs 6.7% at
  640x360. It stretches horizontally with the wider aspect because the demo's MVP carries no
  aspect term — 2.6/2.9's existing push-constant contract, not a regression.
- Frame timing at this resolution: median `present_ms` sub-millisecond, occasional 141-149 ms
  spikes (the implicit overlay layers this box injects into every instance). `ve_demo` logs loader
  notes without gating on them; only correctness is asserted, so `>=55 fps` holds on average.

- `VE_GLSLANG` with an absolute `glslangValidator.exe` path: without it `cook_shader_golden`
  prints its skip and `ve_demo` fails the cook at exit 2 (its message names the env var).

### Gotcha (found by MSVC's debug STL, invisible to the Linux gate)
- `std::search(t.begin(), t.end(), std::string_view("Mat").begin(), std::string_view("Mat").end())`
  **is UB**: the needle range spans two different temporary views. MSVC aborts the whole binary —
  `Assertion failed: string_view iterators in range are from different views`, exit `0xc0000409`,
  with the STL header as the only location printed (doctest's own stdout is lost: a pipe is fully
  buffered and the abort never flushes it, so the crashing test case looks like whatever ran last).
  It "passed" on GCC/libstdc++ and in Release only because both temporaries happen to point into
  the same string literal and libstdc++ ships no such check. Name the view once.
  Site: `tests/gfx/test_mesh_upload.cpp:153` (3.5's corrupt-material fixture).
- ve_demo on this box's real GPU (`RTX 4060 Laptop GPU`, api 1.4.351, VK_KHR_win32_surface):
  120/120 presented, **0 validation problems**, 2 swapchain rebuilds with the extent trail
  `640x360 → 512x288@f80 → 640x360@f110`, avg 60.21 fps at FIFO — but only 70% of frames held
  ≥55 fps (min/median/max 0.15/9.94/96.35 ms). The box auto-loads implicit overlay layers into
  every instance (Steam ×2, RTSS, OBS, EOS per `VK_LOADER_DEBUG=all`), which is the plausible
  source of the 96 ms spikes; the demo logs loader notes without gating on them. Timing, not
  correctness — no action taken.


### Linux merge gate re-run on WSL2 (2026-11-08) — interim, does **not** close 3.16
- Ran `tools/ci_local.sh` unmodified (`BUILD_DIR=build-wsl BUILD_TYPE=Debug`) against a fresh clone
  of the **pushed** `testing` tip `e3a60045211043e3501f5e2578ac622edd9ea27b` (clone HEAD ==
  `origin/testing` == GitHub `refs/heads/testing`; worktree clean; `ci_local.sh` CR count 0).
  Inside a user+mount namespace (`unshare -Urm`) with `/usr/share/vulkan` on tmpfs.
  **GREEN, exit 0.** `ctest` 1/1 `engine_tests` Passed (2.92 s); doctest **54 cases / 3091
  assertions, 0 failed, 0 skipped** — identical to the 3090/P5000 dev box's counts, and 2 cases /
  523 asserts above the Windows run (the POSIX crash-handler + log fixtures `engine_tests` does
  not compile under MSVC; their deliberate `[FATAL] crash: SIGSEGV/SIGILL` + `[ERROR] error-line`
  output is expected suite noise, not a failure).
- Sanitizers and the wall are real, not assumed: 132 × `-fsanitize=address,undefined` in
  `build.ninja`; `ldd` under the gate env resolves `libasan.so.8` + `libubsan.so.1`. Wall intact
  (`-Wall -Wextra -Wpedantic -Werror`, `CMakeLists.txt:27`); the `-Wno-*` that show up in
  `build.ninja` are `tests/CMakeLists.txt:81`'s pre-existing per-target suppressions. Nothing was
  weakened to pass. `VE_TRACY=ON`.
- GPU half genuinely exercised: device string `llvmpipe (LLVM 21.1.8, 256 bits)` in the suite
  output and doctest's own `0 skipped`. The 2.11 preflight (`ci_local.sh:23-39`) refuses to
  configure without a `lvp_icd*.json` **and** `VkLayer_khronos_validation.json`, so a GREEN here
  cannot come from quietly skipping the render tests.
- Toolchain without root/sudo (user's chosen path — a deviation, stated not hidden): WSL2 Ubuntu
  26.04 shipped lavapipe (`mesa-vulkan-drivers`, Mesa 26.0.8) and loader `libvulkan1 1.4.341.0-1`
  but **no C++ compiler at all**. The closure — `g++ 15.2.0`, `vulkan-validationlayers
  1.4.341.0-1`, `glslang-tools 16.2.0`, `spirv-tools`, `libglfw3-dev 3.4`, `pkg-config` (57
  packages, 95 MB) — was resolved via `apt-get update/download` with apt's index+archives
  redirected into user-writable dirs (`-o Dir::State::lists=… -o Dir::Cache::archives=…`, closure
  read out of `apt-get -s install --no-install-recommends`) and unpacked with `dpkg -x` into
  `~/ve-prefix`. cmake 3.31.6 + ninja 1.12.1 came from the repo's own
  `tools/bootstrap_toolchain.sh` (user-local, checksum-pinned). gcc's drivers run through
  `--sysroot=$HOME/ve-prefix` wrappers; sonames the system cannot resolve are symlinked into
  `~/ve-san` on `LD_LIBRARY_PATH`, deliberately **excluding** glibc/libstdc++/libgcc_s so the C
  runtime stays the distro's.
- Deviations from the prescribed gate: WSL2 kernel 6.6.87.2-microsoft-standard, not the dev box's
  hardware; g++ 15.2 (AGENTS.md floor ≥14); packages are Ubuntu's but hand-unpacked, so the
  toolchain is assembled rather than distro-installed. This run covers the **ctest half** only;
  windowed/WSI evidence stays with the Windows/NVIDIA and Xvfb runs (#35/#36). Nothing about the
  3090 box's outcome is inferred from this one.
- **3.16 stays open**: it demands a clean-tree GREEN *on the final Phase 3 tree*. What this run
  retires is the older obligation — that the Windows-session fixes (`f3be6cb` string_view,
  `a97f973` docs) never be measured on Linux. They were, and they are clean.

### Gotchas from the WSL2 gate run (each cost a run)
- `.sh` files authored on Windows arrive CRLF and bash dies with `set: - : invalid option` /
  `head: parse error: 0` — normalize to LF before handing them to WSL. In-repo scripts are clean
  (the gate asserts `ci_local.sh CRs = 0`).
- Cloning the drvfs working tree from WSL fails with `fatal: detected dubious ownership in
  repository at '/mnt/c/…'` (drvfs files carry a foreign uid). Supplied per-process through git's
  env config (`GIT_CONFIG_COUNT=1`, `GIT_CONFIG_KEY_0=safe.directory`, `GIT_CONFIG_VALUE_0=…`)
  instead of mutating `~/.gitconfig`.
- The extracted gcc must be reached through the sysroot wrapper via `CC`/`CXX`:
  `CMAKE_PREFIX_PATH=$PREFIX/usr` also puts `$PREFIX/usr/bin` on CMake's search path, and that raw
  driver fails the ABI test — `ld.bfd: cannot find /usr/lib/x86_64-linux-gnu/libc_nonshared.a`.
- `VK_ADD_LAYER_PATH`/`VK_LAYER_PATH` alone will **not** satisfy the 2.11 preflight: it greps
  `/usr/share/vulkan/explicit_layer.d`. Without root, mount tmpfs there inside `unshare -Urm`
  (uid-mapped root, no host writes) and copy the staged manifest + `libVkLayer_khronos_validation.so`
  in.
- The gate scripts live outside the repo (`C:\Users\AmesL\Projects\.ve-deps\gate2.sh`, logs beside
  it) on purpose: they encode this box's paths, not engine facts.


## 3.7 GPU textures — 3.8 (MAT0 texture refs) decisions & gotchas
- MAT0 v3 appends two length-prefixed **content-ID** strings per material
  (baseColor, metallicRoughness); `kCookFormat` is global so it went 2 -> 3
  with everything else (bump commit `2f26f8a` set that precedent). The gfx
  loader only *consumes* them for now — sampling binds land with 3.10 PBR.
- Texture doctrine: only **external** glTF image URIs are accepted. Embedded
  bufferView images have no content ID/hash of their own, so they'd break the
  manifest-deps story (#13) — the cook rejects them. URIs resolve relative to
  the `.glb`'s directory through `resolve_ref` (same lexical normalization +
  escape-root policy as def refs): a `models/ship.glb` saying `../shared/mr.png`
  is fine; a root-level glb saying `../` fails, and the test suite pins both.
- Dep edges are validated at cook time (missing ref = cook fails naming glb +
  ref, matching def-ref doctrine) and land deduped+sorted in the manifest.
  The incremental test pins the payoff: editing the PNG re-cooks only the
  TEX0 (1 cooked, 1 skipped) — the glb keeps its content-ID ref, its hash is
  unchanged, and the preserved manifest entry keeps the dep edge.
- Test fixture: `tests/tools/glb_fixture.h` authors minimal valid GLB bytes
  (JSON+BIN, one triangle, N material specs with texture-slot URIs) — no DCC
  needed to pin material plumbing. Real-PNG fixtures reuse the golden
  harness's `write_png_rgba8` (single `engine_tests` binary makes that free).

## 3.9 Instanced eye pass (2026-11-08) — decisions & gotchas
- Shape: ONE `vkCmdDraw(6, N*E)`, `eye = gl_InstanceIndex / N` (instance eye
  order = ordered rig eyes, the 3.2 camera contract). UBO holds
  `mat4 mvp[2]` (per-eye proj*view) + `mat4 model[N]` (per-instance); the
  vertex shader writes `gl_ViewportIndex = eye`; pipeline declares
  viewportCount = scissorCount = E (dynamic), `vkCmdSetViewport/Scissor`
  carry per-eye rects from `eye_frame::viewport_`. Desktop = same path,
  E=1, one centered eye — and its instanced capture is byte-checked against
  the N-single-draw (firstInstance) sequence.
- Why viewport-index and not multiview: the checklist's "eye index derived
  from instance id" needs per-eye matrices without changing the render pass;
  the viewport-index path keeps the RT, presenter, and pass code untouched
  and works for desktop E=1 identically.
- Feature plumbing: `multiViewport` (1.0 feature, mandatory since 1.1) rides
  `pEnabledFeatures` — it is NOT in `VkPhysicalDeviceVulkan11Features`
  (that struct lists only *new-in-1.1* members), and validation demanded it
  explicitly even at apiVersion 1.3. Gotcha.
- `VK_EXT_shader_viewport_index_layer` is enabled *iff advertised*
  (device_extension_available). VK 1.3 folded it into core — Vulkan13Features
  has no such member — but lavapipe still advertises the EXT name and the
  validation layer refuses pipeline creation with a shader writing
  gl_ViewportIndex unless the extension is enabled on the device. NVIDIA
  drivers advertise nothing to enable (pure core). Hence conditional.
- GLSL: `#extension GL_ARB_shader_viewport_layer_array : require` is the
  only spelling glslangValidator accepts for writing `gl_ViewportIndex`
  (the EXT-named pragma does not exist in glslang).
- Descriptor additions: `uniform_set_layout` (one UNIFORM_BUFFER binding),
  `uniform_buffer` (host-visible|coherent, persistently mapped; write() =
  memcpy — device-local staging buys nothing for per-frame updates), and
  `descriptor_pool` now sizes two set kinds + `write_uniform`. Gotcha: a
  `VkDescriptorPoolSize` entry with count 0 is a validation error
  (VUID-00302) — omit zero-count kinds from the pool sizes.
- Pins: pixel centers computed on the CPU from the *same* matrices that
  fill the UBO (Vulkan y-down NDC: ndc +1 = bottom row — 2.11's comment).
  ipd exaggerated (0.2 m) so the between-eye parallax pin (>= 2 px shift,
  0 px vertical) can't be rounded away; frustum optics have realistic pins
  in 3.2 already. No-bleed pins: exact clear color at row 1 of each eye
  rect and at the computed gap column between quad columns 1/2.

## 3.10 PBR-lite (2026-11-08) — decisions & gotchas
- Shader shape: hand-written Cook-Torrance in 25 lines of GLSL — GGX NDF
  (a=rough^2, a2=a^2, D=a2/max(pi*h^2,1e-7)), Smith-Schlick visibility in
  the UE4 approx form vis=0.25/max(ndl*ndv,1e-4), Schlick Fresnel with
  F0=mix(0.04,baseColor,metallic), Lambert diffuse scaled (1-metallic)/pi,
  ambient*baseColor+emissive kept even when ndl clamps to 0. No engine-side
  BRDF lib, no IBL (PLAN scope: one directional light).
- Data flow per Q5: ONE descriptor set, three UBO bindings — Cam
  (view|proj|camPos, VERTEX|FRAGMENT), Mats (base[2]|emis[2]|mr[2],
  FRAGMENT), Lights (dir|color|ambient, FRAGMENT). Per-draw = push constants
  only (model mat4 + material index, 68 B). uniform_set_layout gained a
  multi-binding create() (ubo_binding list) for exactly this.
- CPU reference matched lavapipe BYTE-EXACT at both material pins — Mesa
  did not fma-contract the shader math. Pins keep the <= 2/255 tolerance
  anyway (a different driver/LLVM may contract; the golden is the strict
  byte gate, pins are the analytic gate).
- capture_rgba8 is image-order (B8G8R8A8); the golden PNG harness wants
  RGBA — same 0<->2 swap as test_triangle_golden before check_golden.
- Golden pbr_lite_64.png strict byte-exact on lavapipe (#35).

## 3.11 Shadow map (2026-11-08) — decisions & gotchas
- Shape: one 256x256 D32 depth pass for the cube through `dir_light_shadow_rig`
  (new in projection.h: ortho fitted to the scene AABB, eye at aD+margin along
  -dir, near/far bound the box + pads), consumer PBR pass hard-samples it with
  texelFetch + reversed-Z compare `vis = (lc.z + bias < stored) ? 0 : 1`,
  bias 0.002 (scene has no self-shadowing faces at risk).
- The offscreen presenter's own depth target IS the shadow map: depth image
  usage gained VK_IMAGE_USAGE_SAMPLED_BIT; `expose_depth_for_sampling(cmd)`
  barriers depth -> DEPTH_READ_ONLY_OPTIMAL between passes (presenter tracks it
  so end_pass/capture still work).
- GOTCHA (validation VUID 13.2.5 image-layout matching): sampling a DEPTH
  aspect requires descriptor imageLayout == DEPTH_READ_ONLY_OPTIMAL, NOT
  SHADER_READ_ONLY. `descriptor_pool::write_sampler` gained a layout param
  (default keeps color behavior).
- GOTCHA (lavapipe): a pass with ZERO color attachments executes but draws
  NOTHING (no validation error, all-zero depth). Shadow pass carries a
  sacrificial color attachment, loadOp/storeOp DONT_CARE, frag writes black.
- GOTCHA (GLSL precedence): `ivec2(uv) * 256` truncates the vec to 0/1 first —
  texelFetch coords must be `ivec2(uv * 256.0)`. Symptom: shadow reads always
  hit a map corner (always "unoccluded").
- GOTCHA: UBO table pos vec4 with w=0 => gl_Position w=0 => entire draw
  clipped, silently. CPU pins that recompute from .xyz (like the corner probe)
  stay green while the GPU draws nothing — pin the GPU OUTPUT, not matrices.
- Pins: depth readback texel == CPU lightVP depth within 2e-3 (top face is
  oblique; texel-CENTER vs query-point differ by one texel of face gradient);
  cleared texels == 0 exactly; coverage == projected cube area +-5% (1110
  texels); floor pins deep inside/outside the parallelogram == CPU GGX with
  vis from the GPU's own read-back texel; golden scene_64.png byte-exact.

## 3.12 Cooked text (2026-11-08) — decisions & gotchas
- `.ttf` -> FONT0 unit at cook time via stb_truetype (already FetchContent'd; no new dep).
  Runtime never parses fonts (decision #15). Params pinned in `tools/cook/src/font.h`:
  16 px, cp 32..126 (95 glyphs), atlas width 512, 1-px-gutter shelf packer.
- FONT0 payload (doc in `ve/core/cooked.h`): LE header (px, atlas w/h, ascent/descent,
  first_cp, count) + per-glyph (atlas x/y/w/h, bearing, advance) + RGBA atlas;
  coverage replicated R=G=B=A; `w==h==0` = blank; cp = first_cp + index.
- Vendored `assets/fonts/DejaVuSansMono.ttf`; license text lives in
  `docs/licenses/DejaVu-fonts.txt` — anything under `assets/` gets cooked, so
  license/text files must sit outside it.
- Pinned 2021 stb_truetype takes NO buffer size and trusts the table directory:
  truncated/corrupt .ttf reads OOB under ASan. `validate_sfnt()` (version magic +
  table-dir bounds) runs first; bad source = clean cook error, not crash.
- stb gotchas: `stbtt_GetCodepointHMetrics` is 4-arg in the pinned version;
  `stbtt_GetCodepointBitmapBox` offsets x0/y0 must be STORED during the pack pass
  (raster pass re-queries size and compares against them).
- GFX test (`gfx_text_draw`): atlas uploads byte-exact through the 3.7 gpu_texture
  path; glyphs drawn as push-constant quads (6 verts each, NEAREST sampler, texel-
  center UVs); CPU atlas model pins cov==255 -> exact white, cov==0 -> exact clear,
  outside quads -> exact clear; golden `text_64.png` byte-exact on lavapipe.
- **Vulkan NDC y is DOWN**: framebuffer row r = y_ndc 2r/H-1. The OpenGL-style
  `gl_Position.y = 1 - 2r/H` flip mirrors the whole text off the pins — this cost
  most of the debugging (draw looked "dead"; it was drawing mirrored at the bottom).
- GLSL has no `(void)expr;` cast-away trick — glslang errors "cannot construct
  this type 'void'". Just don't declare what you don't use.
- DejaVu Mono 16 px stems ~1.7 px: AA coverage rarely reaches 255, so exact-pin
  thresholds must derive from the CPU model (measured model_lit > 15), not guesses.

## 3.13 Immediate UI (2026-11-08) — decisions & gotchas
- Hand-rolled immediate-mode debug UI in `ve::gfx` (`ui.h/ui.cpp`): pure CPU —
  NO Vulkan/device/GLFW types in the UI core. Input arrives as neutral per-frame
  actions (`ui_io`, the #15 input seam); output is a draw-command frame
  (pos2/uv2/color4 interleaved verts + uint16 indices, hard cap 64 quads) that
  ONE render pass draws onto a presenter render target. Font arrives only as
  cooked FONT0 via `gfx/src/font.cpp` loader (`load_cooked_font_file`).
- ID stability bug caught by test: the window scope hash MUST be a pure function
  of the title. Folding the PREVIOUS `win_id_` into it (via `hash_id`) made every
  frame's widget ids differ -> press/release ids never match, clicks unreachable.
- Press capture (`held_`) is cleared in `end_frame`, NOT `begin_frame`: zeroing it
  on the release frame before widgets run kills `released_ && held_==id` clicks.
- FONT0 `advance` is **f32** in the payload (cooked.h doc said i32 — fixed).
  Layout rule: pen is a float summed from advances; glyph box origin =
  `int(pen)` (truncate) + integer bearing. Deterministic and mirrored by tests.
- UI blend: RGB src-alpha-over-dst; alpha channel keeps the target
  (src_alpha=ZERO, dst_alpha=ONE) — UI coverage never leaks into target alpha.
- lavapipe pins: exact-colour checks only on bare (no-glyph, no-AA) pixels and on
  atlas texels with coverage exactly 0/255 through the blend; AA texels land on
  uncheckable intermediate values. `ui_64.png` golden strict per #35.
- CPU half of the test proves: same io sequence -> byte-identical frames across
  two fresh contexts (determinism), exact integer layout of chrome/rows, full
  click lifecycle incl. press-off-cancel, checkbox toggle, text-field
  focus/edit/backspace/commit, `[x]` close, corrupt-FONT0 clean load error.

## 3.14 World canvas (2026-11-08) — decisions & gotchas
- Canvas doctrine: a world-space canvas is NOT a new subsystem — it is just an
  `offscreen_presenter` target + a scene transform. The target's color image now
  always advertises `VK_IMAGE_USAGE_SAMPLED_BIT`; `expose_color_for_sampling()`
  barriers COLOR_ATTACHMENT -> SHADER_READ_ONLY (mirror of 3.11's depth expose).
  Presenter owns the layout; a pass never barriers it.
- mvp for the scene quad rides a UBO (binding 1 in a raw 2-binding test layout:
  sampler 0 / UBO 1) — engine's proven path (3.10 UBOs). Push constants would
  work too (the earlier "push mat4 drops the draw" lead was wrong, see below).
- **GOTCHA (the one that cost this task hours): vertex attribute format MUST
  match the shader input width.** The scene `pos` attribute was declared
  `R32G32_SFLOAT` while the shader reads `in vec3 pos`. Vulkan default-fills
  missing components with (0,0,1) -> z=1 silently -> `clip.w = m[11]*z = -1` ->
  entire quad discarded, no validation error, no layer message. Bisection
  probes (fixed vertex drew, any mat4-mult didn't) looked like a lavapipe
  mat4 bug; it was my own attribute layout. When a draw vanishes but state
  probes are clean, dump the per-vertex inputs into a varying and read them
  back as color — PROBE8-style diagonal readout localizes in one run.
- **GOTCHA: `pipeline_cache` sizes color-attachment writes from `blends.size()`**
  — `blends.clear()` compiles a write-nothing pipeline (lavapipe drops it
  silently, same zero-color lesson as 3.11). For an opaque pass pass one
  explicit `blend_attachment_state{}` (blend_enable=false), never an empty list.
- **GOTCHA: `offscreen_presenter::submit()` does not fence-wait here (test
  pattern).** Destroying vertex/index buffers right after submit SEGVs inside
  lavapipe draw threads; `vkDeviceWaitIdle(dev.handle())` before destroy.
- CPU pinning: forward map is just `mvp * point(x,y,-2)` (CPU mat4, GLSL order);
  back-projection solves xv=(2*nx+2*B)/A, yv=(2*D-2*ny)/C from the off-axis
  rows — both agree with the GPU to the byte, all sampled interior pixels
  byte-exact (NEAREST at texel centers keeps raster jitter under the half-texel
  margin; quad ~28 px per 32 texels).


## 3.15 — done-when scene harness (commit c2af615)
`tests/gfx/test_scene_orbit.cpp` — `gfx_scene_orbit` (centered eye) +
`gfx_scene_stereo` (checklist said `gfx_scene_stereo_offaxis`; same intent,
Q4 converged off-axis pair, per-eye goldens `scene_stereo_l/r_64.png`).
PoC hull `assets/samples/test_ship.glb` cooked with a 64x64 RGBA checker
TEX0 + hand-authored 12x12 deck quad at hull-floor height; one
`render_scene(yaw)` closure runs the whole framework per frame: shadow pass
(3.11 rig + expose) then scene pass Q4-style (3.9 viewportIndex, 3.10 PBR
frags, 3.11 shadow sample). Pins: silhouette center painted at 4 yaws,
yaw0 vs yaw90 captures differ, deck depth == CPU reversed-Z unproject
(2e-3), deck color == CPU GGX mirror w/ checker texel + GPU shadow vis
(2/255 — matched exactly, BGR(90,86,85) both sides), determinism frame
byte-identical, 3 goldens strict.
- Engine side (this commit): MESH payloads now SURFACE their embedded MAT0
  blocks (`cooked_material` + `parse_mesh_materials` +
  `load_materials_by_id` in gfx/mesh) and `load_texture_by_id(cooked_dir,
  id)` in gfx/texture goes through the manifest — an id the manifest does
  not list is not loadable even if a matching .ck sits on disk (decision
  #13 authority pattern).
- **GOTCHA: `pipeline_desc::depth_format` defaults UNDEFINED and rides into
  `VkPipelineDepthStencilStateCreateInfo::depthStencilFormat` — every depth
  write is silently dropped (depth buffer reads all-zero in captures, color
  fine). Any pipeline drawn over a depth attachment MUST set
  `depth_format = VK_FORMAT_D32_SFLOAT`.** 3.11 set it via its helper;
  3.3 built pipelines by hand — only a from-scratch cache desc hits this.
- **GOTCHA: with `depth_range::reversed_1_to_0`, `clip.z/clip.w` IS the
  window depth** (near 1, far 0) — a CPU-side `0.5*(z+1)` remap makes every
  depth pin fail with values like win_z 0.503 vs captured 0.0066.
- **GOTCHA: fragment shaders cannot read `gl_InstanceIndex` for per-eye
  data** — pass `layout(location=4) flat out vec3 veye` from the vertex
  stage. And `gl_ViewportIndex` in a vertex shader needs
  `#extension GL_ARB_shader_viewport_layer_array : require` (3.9 test got
  it from SPIR-V env; cooked GLSL must say it).
