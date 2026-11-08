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

Phase 2 (Vulkan right of passage) **in progress**: instance/device/queues on Volk at API 1.3 with the Khronos validation layer required and a strict messenger that fails tests on any error or warning (2.7); cooked GLSL→SPIR-V; the Presenter seam with an offscreen target and a 64×64 golden triangle in ctest on lavapipe (2.4–2.6); and the desktop WSI presenter — GLFW window behind the platform seam, FIFO swapchain, one frame in flight, and **blit RT→swapchain image** so no pass ever sees a swapchain image (2.8). Behavior evidence for the windowed path lands in 2.9 (Xvfb+lavapipe capture per decision #36).

| Phase | Area | Status |
|---|---|---|
| 0 | Toolchain & skeleton | **complete** |
| 1 | Math & headless core | **complete** |
| 2 | Vulkan bring-up | **in progress** (2.1–2.8) |
| 3 | Mini render framework | not started |
| 4 | Arcade flight & gravity core | not started |
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

On Windows (decision #38, supported build/test target, not a port): build under a Visual Studio developer shell with the bundled CMake+Ninja — same three commands, no generator flag needed. The warning wall is compiler-gated rather than softened (MSVC gets `/W4 /permissive- /Zc:preprocessor /WX`), the POSIX crash handler and its suite are excluded there, and sanitizers report as unavailable instead of quietly vanishing (every one of those deviations has a line in `docs/CHEATS.md`). Measured on MSVC 14.51: `engine_tests` 40 cases / 407 assertions green, `ctest` 1/1 — the Tier-2 gfx cases skip there for want of a lavapipe ICD and a validation layer, which is exactly what they print.

Profiling: the Tracy client (pinned v0.15.0) is linked into every engine target by default (`-DVE_TRACY=OFF` to drop it). Attach a Tracy GUI (same version) on port 8086 while any engine binary runs to capture frame marks and zones.

## Requirements

- Linux (x86_64), Vulkan-capable GPU (tests additionally run on software Vulkan)
- GLFW 3.3+ for the desktop window/platform seam (`sudo apt install libglfw3-dev`); it is linked only by `platform/`, never by `gfx/`
- Windows x86_64 is a supported target (decision #38); Windows tests run on real hardware. The engine's own code compiles clean under MSVC — the CMake build mode for Windows (warning flags, sanitizers, POSIX bits in `core/src/crash.cpp`) is still open
- Optional: OpenXR runtime (SteamVR or Monado) for VR mode
