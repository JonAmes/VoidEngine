# Void Engine

A from-scratch C++23 game engine whose first project is an **arcade space sim**: Newtonian flight with toggleable assist controllers, arcade gravity wells (Babylon 5 / Battlecruiser school — no orbital-mechanics homework), a hull + hardpoints ship designer, one RPG stats system shared by player pilots, NPCs, and ships, hybrid laser/ballistic combat with shields and per-component damage, a trading economy with live dynamic markets, faction reputation, and a seeded mission director, real-scale star systems bridged by continuous FTL warp, and huge-scale rendering — Linux-first, with first-class VR output via OpenXR.

## Design pillars

- **Headless-first.** The simulation core has no rendering, windowing, or XR dependencies. The full test suite runs with no display and no headset (`ctest` over SSH).
- **Deterministic simulation.** Fixed-timestep physics, fp64 positions, floating-origin world, replay-hash tests.
- **VR is a seam, not an afterthought.** All rendering targets pass through a Presenter abstraction; desktop and OpenXR are interchangeable consumers.
- **Engine vs game is a hard line.** The engine provides attributes, modifiers, gameplay tags, and effects; every named game stat or weapon is data.
- **Cockpit-first UI.** One shared, diegetic cockpit for desktop and VR: the same dash with in-world instrument panels, cockpit view as the default, and a walkable 3D hangar for ship design. Walkable ship interiors put you on foot between crew stations — and stations must be manned: hire a pilot, gunner, engineer, navigator, and trader, or their systems simply don't run. The ship is sectioned: a breach vents one compartment, and you walk to it, close the doors, and patch it — and after landing, you can suit up and step outside. Flight and targeting assists are always visible, named toggles — never silent.

## Status

Planning complete; implementation not started.

| Phase | Area | Status |
|---|---|---|
| 0 | Toolchain & skeleton | not started |
| 1 | Math & headless core | not started |
| 2 | Vulkan bring-up | not started |
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

Build and test commands are defined in Phase 0 and will be documented here once they exist. Target toolchain: g++ 15.2 (C++23), CMake + Ninja, Linux.

## Requirements

- Linux (x86_64), Vulkan-capable GPU (tests additionally run on software Vulkan)
- Optional: OpenXR runtime (SteamVR or Monado) for VR mode
