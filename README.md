# Void Engine

A from-scratch C++23 game engine whose first project is an **arcade space sim**: Newtonian flight with toggleable assist controllers, arcade gravity wells (Babylon 5 / Battlecruiser school — no orbital-mechanics homework), a hull + hardpoints ship designer, one RPG stats system shared by player pilots, NPCs, and ships, hybrid laser/ballistic combat with shields and per-component damage, fully diegetic positional audio (every sound a physical source — you hear from where you stand), a trading economy with live dynamic markets, faction reputation, and a seeded mission director, sensor contact pictures carried over the ship's own signal wiring — with authored hull classes and seeded measurement uncertainty — real-scale star systems bridged by continuous FTL warp, analytic procedural planetary terrain (generated from a seed, zero baked meshes), onboarding through diegetic opening contracts with a no-pause world you can only speed up via an in-world Time Compression Device, and huge-scale rendering — Linux-first, with first-class VR output via OpenXR.

## Design pillars

- **Headless-first.** The simulation core has no rendering, windowing, or XR dependencies. The full test suite runs with no display and no headset (`ctest` over SSH).
- **Deterministic simulation.** Fixed-timestep physics, fp64 positions, floating-origin world, replay-hash tests.
- **VR is a seam, not an afterthought.** All rendering targets pass through a Presenter abstraction; desktop and OpenXR are interchangeable consumers.
- **Engine vs game is a hard line.** The engine provides attributes, modifiers, gameplay tags, and effects; every named game stat or weapon is data.
- **Cockpit-first UI.** One shared, diegetic cockpit for desktop and VR: the same dash with in-world instrument panels, cockpit view as the default, and a walkable 3D hangar for ship design. Walkable ship interiors put you on foot between crew stations — and stations must be manned: hire a pilot, gunner, engineer, navigator, and trader, or their systems simply don't run. The ship is sectioned: a breach vents one compartment, and you walk to it, close the doors, and patch it — and after landing, you can suit up and step outside. Flight and targeting assists are always visible, named toggles — never silent.

## Status

Phase 0 (toolchain & skeleton) **complete** (2026-11-07): reproducible toolchain bootstrap, CMake+C++23 warning-wall build with ASan/UBSan, doctest/ctest, headless CI script, logging/asserts/crash handler, the content cooklet (incremental + watch-mode, content-hash gating, PNG→texture and glTF→mesh cooking with a runtime readback path), and day-one Tracy profiling hooks.

Phase 1 (math & headless core) **complete** (2026-11-07): hand-rolled `core/math` — fp32 vec/mat4/quat + fp64 `dvec3` sim positions with AU-scale precision proven, euler/rig-frame conventions, `look_at_rh`, TRS decompose/compose, projection builders where **off-axis asymmetric (L/R/T/B-tan) perspective is the primary implementation** (VR per-eye ready, Vulkan depth + reversed-Z variants), spherical↔cartesian, and a floating-origin frame. World frame pinned by decision #34: right-handed, Y-up, −Z forward — matching glTF and OpenXR natively. 33 test cases / 404 assertions, green with no display.

Phase 2 (Vulkan right of passage) **complete** (2026-11-08): instance/device/queues on Volk at API 1.3 with the Khronos validation layer required and a strict messenger that fails tests on any error or warning (2.7); cooked GLSL→SPIR-V; the Presenter seam with an offscreen target and a 64×64 golden triangle in ctest on lavapipe, compared byte-exact against a committed golden PNG via the 2.11 harness (`tests/gfx/goldens/`, regenerate with `VE_GOLDEN_UPDATE=1`; fuzzy tolerances reserved for real-GPU tiers) (2.4–2.6, 2.11); the desktop WSI presenter — GLFW window behind the platform seam, FIFO swapchain, **blit RT→swapchain image** so no pass ever sees a swapchain image (2.8); **two frames in flight via presenter-independent `gfx/frame_sync`** — fence-per-slot, the same sync machinery in both presenters, swapchain semaphores kept as the swapchain's own protocol (2.10); and `tools/ve_demo`, the windowed rotating triangle whose committed capture (`docs/phase-02-vulkan/evidence/`) is the 2.9 evidence — run on the prescribed Xvfb+lavapipe basis: 120/120 frames presented, median 3.77 ms / avg 104 fps, 0 validation problems, 2 resize-triggered swapchain rebuilds, self-gating on exit code (the Windows/NVIDIA dev run is preserved under `evidence/windows-nvidia/`). 44 test cases / 662 assertions green on the headless Linux gate.

Phase 3 (mini render framework) **complete** (2026-11-09): the render-framework pieces a real scene needs, each on the headless lavapipe gate with committed byte-exact goldens — depth buffers + reversed-Z conventions (3.3), GPU textures + mipchains (3.7), cooked material/texture dependency graph through the manifest (3.8), one instanced pass rasterizing **per-eye viewports** — desktop `E=1` and stereo through the same path (3.9) — forward PBR-lite (Cook-Torrance GGX, one directional light, UBO/push-constant plumbing) with CPU-reference pins (3.10), a directional-light **shadow map** sampled by the main pass (3.11), cooked text: `.ttf` rasterized once at cook time into FONT0, zero font parsing at runtime (3.12), an immediate-mode UI core that is pure CPU — Vulkan-free by design — plus its render pass (3.13), world-space canvases = presenter targets sampled by the scene (3.14), and the **done-when scene harness** (3.15): the cooked `test_ship.glb` hull with embedded materials + checker texture loaded by content ID, orbiting camera through the full framework (rig+eyes, depth, PBR, shadow), deck pixels pinned against a CPU GGX/reversed-Z mirror to the byte, per-eye off-axis stereo goldens (decision #34 conventions exercised end to end). 69 test cases / 8,377 assertions, goldens re-rendered byte-identical from a fresh-compiled binary (3.16 evidence).

Phase 4 (arcade flight & gravity core) **complete** (2026-11-09): `core/sim/` opens as the sim's first resident, pure Tier 1 / headless — canonical **60 Hz integer-tick time** (decision #45) and fp64 attitude (`dquat`); analytic **kinematic rails** (parent chains, zero stored state, wells sampled at any tick); arcade **C1 gravity wells** with soft-dominance **overlap crossfade** (the moon-in-planet design case: stand on the moon, its gravity wins against the planet in its sky); a **rigid body + fixed-tick integrator** (linear semi-implicit Euler; angular explicit-midpoint RK2 — forward Euler on the free top is provably unstable) with **bitwise-deterministic replay** (command-only logs, FNV-1a-64 raw-bit state hashes); toggleable **arcade assists** — auto-level (zero yaw authority), stabilizer, air-brake (home-well rail frame, pinned `g/k` equilibrium floor), and a **stopping-distance autopilot** that parks on moving rail waypoints (a plain position PD overshoots by kilometres); and **trajectory preview** — the ghost sim rolls a state copy through the *same* assembled physics as replay at a reduced rational rate, with a hard 1024-step budget and a divisor cap where the assist loops would alias. The done-when harness (`sim_flight_figure8`) flies a 4-leg autopilot figure-8 across a 2-planet + 1-moon testbed: every leg parks within 10 m of its waypoint, windowed ghost previews track live to 23 m worst-case across the whole flight, and assist-off ballistics replay bit-identically. 79 test cases / 9,742 assertions; clean-tree rebuild evidence pass (4.12).

| Phase | Area | Status |
|---|---|---|
| 0 | Toolchain & skeleton | **complete** |
| 1 | Math & headless core | **complete** |
| 2 | Vulkan bring-up | **complete** |
| 3 | Mini render framework | **complete** |
| 4 | Arcade flight & gravity core | **complete** |
| 5 | Ship designer (hull + hardpoints) | not started |
| 6 | RPG stats & modifier framework | not started |
| 7 | Atmosphere, landing, contacts | not started |
| 8 | Time compression & streaming | not started |
| 9 | Combat: weapons, shields, damage | not started |
| 10 | OpenXR bring-up (mock-first) | not started |
| 11 | Vertical slice | not started |
| 12 | Engine graduation projects | optional |

## Building

Target toolchain: g++ 15.2 (C++23), CMake 3.31 + Ninja 1.12, Linux. Beyond a C++23 compiler, the desktop window seam needs GLFW (`libglfw3-dev`); the Phase 2 prereq gate in `docs/phase-02-vulkan/checklist.md` lists the rest (vulkan-tools, validation layers, glslang/spirv-tools, Xvfb).

```sh
# one-time toolchain bootstrap (no sudo; installs cmake+ninja into ~/.local)
./tools/bootstrap_toolchain.sh
export PATH="$HOME/.local/bin:$PATH"

# configure + build + test
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build

# or the all-in-one gate: configure, build, run tests with DISPLAY stripped
./tools/ci_local.sh
```

Sanitizers (ASan+UBSan) are ON by default in test builds (`-DVE_SANITIZERS=OFF` to disable). All tests pass headless — no display, compositor, or headset.

On Windows (decision #38, supported build/test target, not a port): build under a Visual Studio developer shell with the bundled CMake+Ninja — measured on VS 18 BuildTools (MSVC 14.51, CMake 4.3). No generator flag is needed, but the default generator there is Visual Studio (multi-config), so `ctest` needs `-C Debug`, and a box without a Vulkan SDK needs two configure flags:

```bat
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVULKAN_HEADERS_INSTALL_DIR=C:\path\to\Vulkan-Headers
cmake --build build
ctest --test-dir build -C Debug
```

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is there because CMake 4 refuses doctest 2.4.11's `cmake_minimum_required(VERSION 3.0)` (the Linux gate stays on CMake 3.31 and never sees this); `VULKAN_HEADERS_INSTALL_DIR` points volk at a Vulkan-Headers checkout when no SDK is installed — the VulkanRT runtime ships headers in none. Quote those two flags if a shell splits `=3.5`.

The warning wall is compiler-gated rather than softened (MSVC gets `/W4 /permissive- /Zc:preprocessor /WX`), the POSIX crash handler and its suite are excluded there, and sanitizers report as unavailable instead of quietly vanishing (every one of those deviations has a line in `docs/CHEATS.md`). Measured on MSVC 14.51: `engine_tests` 52 cases / 2568 assertions green with `VE_GLSLANG` pointing at a `glslangValidator` (2548 assertions when none is reachable — the shader-golden case prints its skip and returns), `ctest` 1/1 — the Tier-2 gfx GPU halves skip for want of a lavapipe ICD, which is exactly what they print. That gap to Linux's tally is structural, not a porting defect: the two POSIX crash-handler cases are not compiled on MSVC (a Windows crash handler is a tracked #38 item). The strict byte-exact golden gate stays Linux/lavapipe by design; Windows real-GPU runs use the fuzzy golden tiers instead (#35/#36).

Running the windowed demo (`ve_demo`) there additionally needs the two dev tools this box keeps outside any SDK: `VE_GLSLANG` pointed at a `glslangValidator` (otherwise the shader cook fails at exit 2) and `VK_LAYER_PATH` pointed at the Khronos validation layer (engine policy requires the layer, so every instance refuses to start without it). Measured: 120/120 frames presented on an RTX 4060 Laptop GPU, 0 validation problems, 2 resize-triggered swapchain rebuilds.

Profiling: the Tracy client (pinned v0.15.0) is linked into every engine target by default (`-DVE_TRACY=OFF` to drop it). Attach a Tracy GUI (same version) on port 8086 while any engine binary runs to capture frame marks and zones.

## Requirements

**Linux** (development box; the headless `ctest` run here is the merge gate):

- Linux x86_64 with g++ 15.2 (C++23), CMake 3.31, Ninja 1.12 — `./tools/bootstrap_toolchain.sh` installs CMake/Ninja into `~/.local` if missing
- Vulkan-capable GPU; the test suite runs on **software Vulkan (lavapipe) with the Khronos validation layer**, no display/compositor/headset required (`vulkan-tools`, validation layers, glslang/spirv-tools — prereq gate list in `docs/phase-02-vulkan/checklist.md`)
- GLFW 3.3+ for the desktop window/platform seam (`sudo apt install libglfw3-dev`); it is linked only by `platform/`, never by `gfx/`
- Verified green on WSL2 Ubuntu 26.04 as well as bare metal: `ctest` 1/1, `engine_tests` 54 cases / 3091 assertions, 0 skipped, with ASan+UBSan linked and the device reported as `llvmpipe` (lavapipe). CMake/Ninja come from `./tools/bootstrap_toolchain.sh` without sudo; a C++23 compiler and the validation layer are the only apt-level prerequisites.

**Windows x86_64** (supported build/test target — decision #38; tests run on real hardware):

- Visual Studio C++ toolchain — measured on VS18 BuildTools (MSVC 14.51). Build inside a developer shell; VS's bundled CMake + Ninja are used, nothing else to install for a build
- A Vulkan-capable GPU with an installed driver (the Vulkan loader ships with the driver). The Khronos validation layer (Vulkan SDK) is used when present — the Windows dev box runs with it active — but is not required, because the strict golden gate lives on Linux/lavapipe
- No GLFW package needed: CMake fetches GLFW 3.4 and builds it statically, so no DLL next to the binaries
- With no Vulkan SDK installed, configure needs two flags — `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (CMake 4 refuses doctest 2.4.11's `cmake_minimum_required(VERSION 3.0)`) and `-DVULKAN_HEADERS_INSTALL_DIR` pointing at a Vulkan-Headers checkout (the driver-shipped VulkanRT runtime provides the loader but no headers) — and since the default generator there is Visual Studio, `ctest` needs `-C Debug`
- The validation layer must be *reachable* to run `ve_demo`: engine policy requires it, so every instance refuses to start without it (exit 2). A layer registered by an older SDK outside the loader's default search paths needs `VK_LAYER_PATH` pointed at its manifest
- Known compiler deviations, each documented in `docs/CHEATS.md`: MSVC gets the equivalent `/W4 /…/ /WX` wall (never lowered); no UBSan on MSVC and ASan only in debug builds, so sanitizers report as unavailable rather than vanishing; the POSIX crash handler (`core/src/crash.cpp`) and its two tests are excluded on MSVC
- Optional: OpenXR runtime (SteamVR or Monado) for VR mode
