# Space Engine — Build Plan

**Goal:** A from-scratch C++ game engine whose first (and driving) project is an **arcade space sim** — Newtonian flight with assist toggles, arcade gravity wells (Babylon 5 / Battlecruiser school, no orbital-mechanics homework), hull + hardpoints ship designer, **one RPG stats system shared by player pilots, NPCs, and ships**, hybrid laser/ballistic combat with shields + component damage, huge-scale rendering — with **first-class VR output and a headless Linux test story from day one**. The journey is the point: every phase must compile, run, and be testable before moving on.

**Machine check (verified 2026-11-06):**
- g++ 15.2 (C++23/25 ready) ✅
- RTX 3090 + Quadro P5000 (Vulkan-capable; 3090 is a comfortable VR target) ✅
- Missing: cmake, ninja, Vulkan SDK/headers, SDL2/GLFW, OpenXR loader → **Phase 0 installs**

---

## Architecture decisions up front (these shape everything)

1. **C++23, CMake + Ninja, per-module static libs.** vcpkg/FetchContent only for: GLFW (window/input), Volk (Vulkan loader), OpenXR-SDK (loader only), stb, doctest. Everything else hand-rolled — that's the learning.
2. **Hand-rolled math library first.** `vec3f/mat4f/quatf` (fp32 for rendering) **plus fp64 position types** (`dvec3`). You cannot retrofit double precision later. The projection math must support **off-axis asymmetric frustums from day one** — VR eye projections are asymmetric; that's a Phase 1 feature, not a Phase 10 retrofit.
3. **Floating-origin world.** The camera rig is always at the origin; all world positions are offsets from it. This + fp64 sim positions is the single most important space-game decision (see Valve's moving-origin paper). Arcade physics removes the orbital math, not the scale problem — planet-to-planet distances still demand this.
4. **Simulation is headless and deterministic.** `core/` has zero rendering/windowing/XR dependencies. Unit-testable against known flight-model numbers, runs on a server someday (multiplayer later), and lets you build the flight/gravity core before the renderer is finished.
5. **Vulkan, not OpenGL.** 2026-only choice for learning modern engines. Brutal up front; use Volk to skip loader pain. Keep the renderer behind a thin `gfx/` interface so your future self can swap backends.
6. **Presentation is an abstraction, never assumed.** Render passes produce *render targets*; a `Presenter` consumes them: desktop swapchain, or XR swapchain images owned by the OpenXR runtime. Nothing may assume "the backbuffer", a fixed aspect ratio, or a single camera. This one seam is what makes VR an option instead of a rewrite.
7. **VR = OpenXR.** The Linux-native answer: SteamVR (Index/Quest via Link/WiRVR) or Monado (fully open, native Linux). No headset or even SteamVR needed for development — everything before real-hardware bring-up runs against a **mock XR runtime** (see Test tiers). Constraints baked in early: per-eye view rendering (single-pass instanced — cheap desktop-side too), runtime-chosen resolution scale + recommended FOVs consumed rather than assumed, 90 Hz budget for VR mode, and world-space (diegetic) UI because a fixed screen HUD doesn't exist in VR — which fits the genre perfectly: **cockpit-first is the native VR scene**, and cockpit-locked views are the most comfortable VR there is.
8. **ECS, but pragmatic.** Archetype-lite: entity + component arrays + systems in a fixed update order. Don't chase EnTT purity; chase cache-friendly iteration over the hot systems (thrust, assist controllers, LOD culling).
9. **Headless-first testing.** `ctest` must pass with no `$DISPLAY`, no compositor, no headset — over SSH, on this box, forever. Three tiers below. **Every phase's "Done when" must be satisfiable headless.**
10. **Arcade physics, hand-rolled (user decision):** gravity = arcade wells (surface "down" near a planet, blending to micro-gravity at well edge, nothing in deep space); flight = Newtonian drift with **toggleable assist controllers** (stabilizer, air-brake, auto-level); ships = **hull + hardpoints**, no free-form stacking. No Kepler elements, no SOI patching, no patched conics, no Δv planner. No physics middleware (Jolt/PhysX) — contact physics is part of the learning journey.
11. **One stats system for player, NPCs, and ships — engine machinery vs game content (user decision).** The engine (`core/`) ships only the framework: sparse tagged attributes, source-tagged modifier stacks with deterministic ordered aggregation, hierarchical gameplay tags, effects/conditions, and an attribute-change event bus. **Which stats exist is cooked data, never engine code** — the engine has no idea what "Piloting" means. Player pilots, NPC pilots, and ships all use the *same* `StatsSet` component — no special-cased player schema — so AI reads exactly the same data the player does, and pilot skill modifies ship stats through the *same* crew-seat → ship modifier linkage as mounted equipment. Stats state is part of deterministic sim state (hashed in replay tests) so a future server-authoritative multiplayer can trust it.
12. **Combat spec (user decision):** **Hybrid weapons** — instant-hit lasers/beams + physical projectile guns + unguided shells/rockets with real Newtonian ballistics (Elite/Free-Space school: drift makes aiming a skill; shells fall in gravity wells like everything else). **Defense** — regenerating shields soak first, then **per-component health**: components go *offline* (zeroed by modifiers, never destroyed), dock repair restores them — hardpoint loadouts stay meaningful. **Aiming** — the targeting computer is a **toggleable assist**, same philosophy as flight assists: free-aim base, assist adds lead/deflection reticle, range/closure readout, and optional rocket designation assist; pilot stats scale assist quality. **Slice AI** — one hostile faction, FSM pilots (patrol / engage / disengage / withdraw) running on the same assists + stats, aggression from stats, faction reputation as gameplay tags. Everything combat is `game/` data riding the Phase 6 effects system (damage = effects); the engine contributes only swept-collision and prediction primitives.
13. **Content ingestion pipeline (user requirement; plan defaults the user may override):** All content flows one way: human-editable source in `assets/` → **cooklet** (`tools/cook`) → deterministic binary units in `cooked/`. The runtime parses **only cooked data** — zero text parsing in the engine. Authoring format defaults to **TOML** for definitions (diffable, boring) + standard formats for binaries (GLTF, PNG, GLSL). One cooked envelope for everything: magic + format version + payload; units are **self-contained and independently loadable** — that's what makes cooked-data hot-reload and mod packs (Phase 12) a loader change, not a format change. A **manifest** (`manifest.bin`) maps stable content IDs (deterministic relative paths) → cooked blobs + a dependency graph (mesh→texture→material), so renames never break references. The cooklet resolves cross-references and **fails the cook** on dangling ones — you learn about a missing texture at cook time, not render time. Re-cooking is **incremental by content hash**, and cooking is deterministic: same inputs ⇒ byte-identical outputs (golden-tested, Tier 1). Content types: **models** (.glb → cooked mesh/material blobs), **textures** (PNG → cooked mip/layout), **fonts** (TTF → cooked glyph atlas via stb_truetype, per decision #15), **shaders** (GLSL → SPIR-V at cook time), **maps** — hybrid: authored system/planet definitions (bodies + gravity wells + stations + POIs + spawn/encounter tables, cooked like everything else) + seeded procedural terrain/scatter inside them — plus hull/hardpoint configs, component/weapon definitions, stats schemas, factions. Audio ingestion deferred by default to the slice; the container supports it from day one.
14. **Blender authoring pipeline (user decision; validated by PoC 2026-11-06, `docs/tests/blender/report.md` in this repo).** Blender is the DCC for all models; engine never touches `.blend` — the hand-off file is an exported `.glb`. Run Blender itself headless (`blender --background --python`) for batch export/validation inside the cook pipeline (PoC proved build+export+EEVEE render work with no display). Export conventions baked into the cooklet, each verified by the PoC: **export settings** `export_extras=True`, default Y-up kept (cooklet converts Y-up → engine axes; never trust the DCC's up axis); **content keys off node names**, not mesh names (mesh data blocks export as `Cylinder.001` junk); **hardpoint sockets = empties** — mesh-less named nodes (`hp_*`) parented in the hull tree, with per-socket custom properties arriving as node `extras` — a hull `.glb` self-describes its sockets, no sidecar file; hull-level metadata (class, mass) rides root-node `extras`; **cooklet owns renderer flags** — ignore the exporter's blanket `doubleSided: true`, fold `KHR_materials_emissive_strength` into the cooked emissive factor, **bake MikkTSpace tangents at cook time** (glTF ships POSITION/NORMAL/TEXCOORD_0 only), no triangulation needed (exporter emits triangles only); **exports are deterministic** — byte-identical GLB across Blender processes (verified by hash), so the two-cooks golden gate holds upstream; buffer compression is the cooklet's job (no Draco/Meshopt by default).
15. **UI/UX spec (user decisions 2026-11-06).** Engine side — primitives only, no game concepts: immediate-mode debug UI (Phase 3; ancestor of all editors), **world-space canvas** for in-world panels, **cooked text** (font atlas baked by the cooklet via stb_truetype — runtime draws glyph runs, zero font parsing), action-based input end-to-end (UI consumes actions, GLFW binds + OpenXR bindings map to the same names — every panel works with mouse/gamepad/controllers, no per-device UI code), UI as a render pass onto the Presenter's target list (no backbuffer, one desktop/VR path). Game side — **(a) one shared cockpit** for desktop and VR: same dash, same world-space canvas panels, one asset serving both modes — desktop may composite a light screen-space layer (reticle, target bracket) the VR presenter skips; **(b) desktop default = cockpit view**, external cam on toggle; **(c) diegetic 3D hangar** for loadout/ship design: walk around the ship, UI on screens in the space (scales unchanged to VR; the Phase 11 loadout editor's game-side twin — headless data model, two front-ends). UX rules that fall out of pinned systems: **assists always visibly offered** — level/stabilizer/air-brake/autopilot/targeting are named dash toggles with stat-scaled quality readout, never silent defaults (decision #12's assist state shows as a lit switch); **components go dark, they don't die** — component-offline (decision #12) renders as dead instruments on the dash, dock repair lights them back up; **what the player sees, the AI sees** — AI pilots read the same cooked stats and action interface, no human-only info channel.
16. **Editor doctrine (user decisions 2026-11-06).** All dev editors are **panels inside the running game/engine** — the one debug-UI tool (its widget layer built in Phase 3) summoned over any scene; no standalone editor executables, no second rendering stack. **Write-back rule: editors may only change human-editable source (TOML defs, etc.), never cooked artifacts** — watch-mode cooklet (Phase 0) closes the edit→see loop, git is the undo. The **replay scrubber is a formal deliverable** (Phase 6): headless replay stepping (Tier 1) + scrubber panel. Map authoring stays **viewer + live def tweaker** (Phase 8) — placement lives in text defs + coordinates, no full map-editor GUI. Model editing is Blender's job (decision #14); no node-graph material editor. Roster: cooklet CLI (P0) · manifest/asset browser + ECS/stats inspector (P3+) · **replay scrubber (P6)** · **map def tweaker (P8)** · **loadout editor panel (P11)** — the dev twin of the diegetic hangar (decision #15) on the same headless data model · keyframe editor + mod browser (P12).
17. **Planets & moons (user decisions 2026-11-06).** A moon is just another body def — same struct, smaller radius, weaker well, usually no atmosphere; no new systems. **(a) All major bodies move, from day one: authored kinematic rails.** Every body's position is an analytic function of sim time in its def (`orbit: <parent>, period, radius, phase`) — not simulated, so it's free: evaluable at any tick (replay scrubber, ghost-sim, time compression), zero drift, deterministic; the gravity well rides with the body. Rails make moonrise/moonset real and make moving-well integration a Phase 4 problem, not a Phase 8 surprise. **(b) 'Down' resolves via dominant home-well + crossfade blend bands:** each ship/round carries one home well (deterministic selection rule + hysteresis on switch); inside overlap shells between two wells' fields, the acceleration is a smooth crossfade — nested systems (moon inside a planet's well) are the design case, and "stand on the moon, down = the moon, not the giant in its sky" is a done-when test, not a hope. **(c) slice world = 2 planets + 1 moon + 1 station** — the moon proves nested wells + landing on an airless body. **(d) terrain art direction is a Phase 8 spike** (throwaway demo: stylized low-poly PBR + palette cards vs realistic-leaning) — the seeded-procedural pipeline doesn't care which, so decide with pixels in hand, not on paper. Gas giants, if ever: visual shell + no-fly authored volume, never a surface to land on.

## Repo layout

```
space-engine/
  core/        # math, gravity wells, flight model, hull/components, contacts +
               # swept-collision primitives,
               # stats/modifiers/gameplay-tags/effects, time  (no gfx!)
  gfx/         # Vulkan device, render passes, mesh/material, shader compiler
  platform/    # window+input, Presenter (swapchain | xr), session mgmt
    xr/        # OpenXR session, swapchains, actions, haptics  (the ONLY xr code)
    xr_mock/   # mock runtime behind the same interface, for CI
  game/        # THIS space sim on top of the engine: stat/weapon definitions,
               # combat FSMs, cockpit/hangar UI, content glue
  assets/      # source content: models/ textures/ maps/ defs/ (audio/ later)
               # cooked/ = binary units + manifest.bin (content ID → blob + deps)
  docs/        # per-phase folders: phase-NN-name/checklist.md + notes.md (see AGENTS.md)
  tools/       # asset cooklet (content ingestion) + ci_local.sh; dev editor
               # panels live IN the engine (decision #16), not standalone tools
  tests/       # doctest suites, per-module; golden-image harness in tests/gfx/
```

The `core/` vs `game/` line is a hard boundary: anything that names a *game concept* (Piloting, Hull Integrity, Laser Cannon, reputation) lives in `game/` or in cooked data; `core/` only knows attributes, modifiers, tags, effects, and collision primitives.

## Test tiers (the headless contract)

- **Tier 1 — pure core:** everything in `core/` + ECS logic. No GPU, no display, no XR. ~90% of tests live here (assist-PID behavior, well-integration accuracy, fuel budgets, CoM/inertia, stats aggregation & modifier stacking, ballistics lead math, combat hit resolution, save/load round-trips). Runs anywhere.
- **Tier 2 — gfx offscreen:** Vulkan without WSI — surfaceless instance / headless EGL on the real GPU, or lavapipe (software Vulkan) where no GPU exists. Render passes write offscreen images; assert via readback or golden-PNG comparison with fuzzy tolerance. The XR present path is exercised against an offscreen target exactly like the desktop path.
- **Tier 3 — XR session logic vs `xr_mock`:** session state machine (idle→ready→synchronized→visible→focused), action binding setup, swapchain acquire/release, frame timing — all driven headlessly by the mock. Real-headset runs are a manual smoke checklist, never part of `ctest`.

---

## Phase 0 — Toolchain & skeleton (a weekend)
- Install cmake, ninja, Vulkan SDK (`vulkan-loader`, `vulkan-tools`, `glslangValidator`), GLFW, **OpenXR-SDK loader**, **mesa vulkan-drivers (lavapipe)** for headless gfx tests.
- CMake superstructure, warning wall (`-Wall -Wextra -Wpedantic -Werror`), clang-tidy, doctest wired, Tracy profiler hooked up day one (you'll want it forever).
- `tools/ci_local.sh`: builds + runs `ctest` and fails if anything needs `$DISPLAY` — prove it over SSH.
- **Cooklet skeleton + cooked container:** magic + version + payload; every cooked unit self-contained and independently loadable. Start `manifest.bin`: stable content IDs (relative paths per decision #13), dependency graph, incremental re-cook by content hash, **watch mode** (file change → instant re-cook).
- Logging (levels + file), assert macro, crash handler.
- **Done when:** `engine_tests` green headless; the cooklet round-trips a test asset (PNG → cooked texture → loader readback) through the manifest; **the Blender PoC ship (`test_ship.glb`, copied into `assets/samples/`) cooks into mesh + material + 5 socket units** (socket names + `extras` present in cooked output, Y-up→engine-axis conversion applied); cooking twice is **byte-identical** (golden test); a def file referencing a missing texture **fails the cook** cleanly, not crashes.

## Phase 1 — Math & the headless core (1–2 weeks)
- `core/math`: fp32 vec/mat/quat + fp64 position/velocity types, angle wrapping, coordinate conversions.
- **Projection builders:** symmetric perspective, **off-axis asymmetric (VR per-eye from L/R/T/B tans)**, reversed-Z, ortho — unit-tested against hand-derived clip coords.
- Tests against hand-computed values (quaternion round-trips, frame changes).
- **Done when:** 100+ math assertions pass headless. This is the foundation of everything.

## Phase 2 — Vulkan right of passage (2–3 weeks)
- Instance → device pick (3090) → **Presenter seam from the first frame**: all passes render to targets; the desktop WSI presenter blits/presents. No pass touches "the swapchain image" directly.
- Surface → swapchain → one triangle, clear color, resize handled. One-frame-in-flight first, then 2. Validation layer ON always.
- Build the **headless surfaceless init path** here too — it's a different instance setup; do it while learning, not retrofitting.
- **Done when:** rotating triangle at stable 60fps, zero validation errors, **and the same scene passes headless as an offscreen golden image**.

## Phase 3 — Mini render framework (3–4 weeks)
- **Camera = rig + eyes** (desktop: rig + one centered eye; identical code path). Per-eye view/projection pairs flow through every pass; single-pass instanced rendering from the start.
- Depth, pipeline state cache, mesh + vertex layout, GLTF import (source) → cooked meshes (Blender conventions per decision #14: node-name keys, cook-side tangent bake, cooklet-owned culling/emissive flags), texturing, PBR-lite (Cook-Torrance, one directional light), simple shadow map — **all gfx assets referenced by content ID through the manifest**, material→texture deps exercised in the incremental-cook test.
- Immediate-mode debug UI (hand-rolled ImGui-style) rendering into a *target list*, plus a **world-space canvas primitive** for later diegetic UI. The debug-UI widget layer is the toolkit for **every dev editor panel** (decision #16): it summons over any scene, desktop or XR.
- **Done when:** a .glb hull model in a scene, orbit camera — **and the same scene renders correctly through two asymmetric off-axis frustums** (proves the VR shape with no headset; golden-image tested).

## Phase 4 — Arcade flight & gravity core, headless (3–4 weeks) ⭐ the soul of the project
- **Gravity wells:** each planet has a well radius; inside, a smooth down-field (constant near surface, tapering to zero at the well edge); outside, nothing. Ships coast ballistically between wells. Tune the blend so crossing the well edge is imperceptible — that blend *is* the feel of the game. **Bodies ride authored kinematic rails from day one** (decision #17a): each well's center is an analytic position function of sim time (`orbit: parent/period/radius/phase`), so wells move — the integrator must sample moving wells, and rails stay evaluable at any tick for replays/ghost-sim. **Home-well selection + crossfade blend bands** (decision #17b): each ship/round carries one dominant home well with a deterministic switch rule + hysteresis; overlapping wells crossfade smoothly in their overlap shell.
- **Newtonian flight integrator:** fixed-timestep semi-implicit Euler with substepping; forces = thrust + gravity + (later) aero. Deterministic replay; record-and-replay tests from day one.
- **Assist controllers** (toggleable, Elite-style): auto-level, stabilizer (kills rotation), air-brake (velocity-kill toward relative-velocity-zero), waypoint autopilot. All plain PD/PID controllers over the same physics — a *lot* of the game feel lives here, and it's all headless-testable.
- **Trajectory preview:** since there's no analytic conic, the nav preview is a low-precision forward roll of the actual sim (cheap fixed-step ghost sim). Rollout == sim within tolerance is a testable property, not a hope.
- **Done when:** headless tests show (a) a scripted figure-8 with assists on/off ends within tolerance, (b) ghost-sim preview matches live sim, (c) deterministic replay hashes match. (Pure Tier 1.)

## Phase 5 — The ship designer: hull + hardpoints (3–4 weeks)
- Hull = mesh + named hardpoint sockets — sockets come **from the hull `.glb` itself** (mesh-less `hp_*` nodes per decision #14; def file only adds/overrides and supplies stats), cooked to binary; hull mesh referenced by content ID. Components bolt on: engines (thrust/Isp/gimbal), wings (lift coefficient), cockpit, reactors, shields, cargo — stats all data. Weapon hardpoints (turrets, gun ports) come in Phase 9 using this same socket model.
- Derived ship stats: mass, CoM, inertia tensor from mounted components; engines off-CoM produce real torque; gimbals + optional RCS handle attitude authority. **No part breaking, no staging** — forgiving by design.
- Component-derived numbers (thrust, turn authority, shield capacity…) are declared as *attribute keys* now so Phase 6's modifier framework can aggregate over them without touching this phase's code again.
- The loadout editor (later, Phase 11) operates on exactly this data model — headless now, two front-ends later: the in-engine debug-UI panel (decision #16) and the diegetic hangar (decision #15).
- **Done when:** a headless test builds hull+2 engines+2 wings, asserts the CoM/inertia numbers, misaligns one engine, and a stabilizer PID holds attitude straight — printed to console.

## Phase 6 — RPG stats & modifier framework (2–3 weeks)
- **Engine machinery:** sparse `StatsSet` (tagged attribute → base + derived fp32), one component type shared by pilots, NPCs, and ships. No hardcoded stat names anywhere in `core/`.
- **Modifier stack:** every modifier carries a source (entity / mounted component / effect / seat), gameplay tags, and a stacking policy (replace / refresh / stack-N / exclusive). Deterministic ordered aggregation: **base → flat add → percent → multiplier → clamp**, with per-attribute clamp ranges. Self-modifying cycles and dependent-modifier chains are rejected by design, not debugged.
- **Gameplay tags:** hierarchical string tags (`pilot.stunned`, `ship.shields.down`, `faction.hostile`) as the universal glue for effects, conditions, AI queries, and UI.
- **Effects & conditions:** an effect is a timed/instant bundle of modifiers (buff/debuff/damage as one system); a condition is a tag gate. Damage/heal/shield-drain are all just effects — the combat vocabulary gets built here for free.
- **Crew ↔ ship linkage:** a pilot in a seat contributes a modifier source scoped to the ship — pilot `Piloting`-style stats scale assist authority and turn response; swapping/ejecting crew recomputes cleanly. Mounted components use the identical linkage (Phase 5), so equipment, crew, buffs, and environmental effects are one pipeline.
- **Change-event bus:** attribute-change notifications with deterministic (tick, insertion) ordering; stats state hashes into replay/multiplayer state.
- Dirty-flag propagation recalculation on read — never a per-frame full recompute.
- **Replay scrubber (formal deliverable, decision #16):** replays stored as fixed-step sim-state records; a **headless replay-stepping API** (load, step frame-by-frame, jump to tick — Tier 1: scrubbed-to-tick state hashes equal live-run hashes at the same ticks); the debug-UI scrubber panel wraps that API — timeline, entity/stat inspector, state diff between ticks.
- **Done when:** hand-computed aggregation suites (stack caps, exclusivity, clamp order) pass; a test proves a pilot-stat change measurably changes the Phase 4 flight model's numbers through the seat linkage; a scripted 10-min combat-style scenario replays with identical stats-state hash; the scrubber API can step that replay to any tick with matching hash. (Pure Tier 1.)

## Phase 7 — Atmosphere, landing, contacts (3–5 weeks)
- Arcade aero *inside wells only*: density altitude curve, hull/wing lift + drag coefficients, forgiving stall. Reentry is just high-speed aero + a heat *number* (a budget, not a simulation) — heat tolerance is an attribute, so per-hull and per-modifier variation comes free.
- Landed-body physics: rigid hull, contact points per leg/gear, simple penalty-based contacts (or hand-rolled sequential impulses — great learning). Gear squash, no gear breaking. Capture zones (landing pads) snap-to-rest for VR-friendly low-stress landings.
- **Swept-collision primitives** (moving point/capsule vs oriented component bounds) land here as part of the contacts toolkit — Phase 9's ballistics hit tests depend on them existing and being tested.
- Surface: at this stage a heightfield/sphere with analytic normals is fine.
- **Done when:** a scripted takeoff → cruise → pad landing runs headless and lands inside tolerance every time, and the same flight is visible in the Phase 3 renderer; swept-vs-moving-target unit tests pass.

## Phase 8 — Time compression, scale, streaming (3–4 weeks)
- **Time compression for travel** (replaces orbital warp): 1×–1000×+ with assisted autopilot + physics LOD — distant ships integrate at coarse fixed steps, near ships at full rate; bounded error is a test, not a vibe. Time-compressed tick ordering must stay deterministic through the stats event bus (Phase 6).
- Floating-origin rebasing, camera-relative rendering, depth precision fixes (reversed-Z float24 — re-verified against the VR asymmetric projections).
- **Maps (hybrid, decision #13):** authored system/planet definitions — well params, stations, POIs, landing pads, scatter tables — cooked like any asset; terrain + scatter generated from the map's seed within authored bounds. Planet as geosphere/quadtree chunks with morphing skirts; scatter via instancing. **Terrain spike first** (decision #17d): throwaway demo of both art directions (stylized low-poly PBR + palette cards vs realistic-leaning) before committing the surface look.
- **Nested moving wells under compression (decision #17):** rails + home-well crossfade at scale — a ship landing on a moon that itself orbits its planet must stay deterministic through 1000× LOD, and coarse-integrated distant ships must not fight well switches.
- **Map def tweaker (decision #16):** a debug-UI panel to fly/orbit a cooked system and edit its authored defs live (well params, POI/station placement by coordinates) — writes back the source def, watch-mode cook hot-swaps the cooked unit. No interactive paint GUI; placement stays in text defs.
- **Done when:** a headless planet-to-planet cruise at 1000× compression finishes within bounded error with matching stats-hash, and land + zoom to surface has no pops or Z-fighting — verified headless via fixed-pose golden images + frame-delta assertions; plus a scripted moon landing inside a planet's well keeps `down` = moon throughout (blend-band test).

## Phase 9 — Combat: weapons, shields, damage (3–4 weeks)
- **Weapon components mount like everything else** (Phase 5 sockets): lasers/beams = instant-hit raycast vs component bounds; guns = spawn physical projectile entities (muzzle velocity inherited from the firing ship — drift cuts both ways); rockets = unguided but steerable-ish darts with a short boost. Damage/range/ROF/spread/heat are all attributes.
- **Projectiles are ordinary sim entities:** same fixed-step integrator, gravity-well affected, deterministic, time-compressible; the ghost-sim from Phase 4 doubles as the ballistic prediction backend.
- **Damage pipeline = Phase 6 effects:** hit → shield attribute drain → overflow to hit-component health → at zero, component goes OFFLINE via a function-zeroing modifier (engines stop thrusting, guns stop firing, shields stop regenerating — `ship.component.offline` tag) → dock repair restores health + removes the modifier. Whole-hull destruction only via a dramatic threshold (hull breach), not component pile-up.
- **Targeting computer = assist controller** (the Phase 4 pattern again): deflection solver for ballistic lead (analytic flat-space solve first, ghost-sim refine inside wells), range + closure + deflection readout, optional rocket designation assist. Pilot stats scale lock time / deflection quality. The assist's output is a *suggestion* — the assist-reticle test property is that firing on its solution actually hits a scripted evader.
- **Combat AI (slice scale):** one hostile faction, FSM pilots (patrol / engage / disengage / withdraw) that fly with the *same* assist controllers and stats the player uses — aggression, engagement range, and retreat thresholds read from attributes; kills/truces update faction reputation tags.
- **Done when:** (a) a scripted 1v1 duel replays with identical trajectory + stats hash, (b) assist-ON ballistic solution hits a scripted evader while assist-OFF straight-shot fire misses it — proves the lead math does real work, (c) shield-absorption overflow numbers match hand calc, (d) offline→dock-repair round-trip restores exact pre-combat stats. All Tier 1; tracers/impacts/reticle FX deferred to the slice + XR phases.

## Phase 10 — OpenXR bring-up (2–3 weeks, mock-first)
- `platform/xr`: session state machine, composition layers, per-eye XR swapchains plugged into the existing render-target flow, recommended FOV / resolution scale consumed.
- **Action-based input** (grab, pinch, throttle, fire, menu — bindings live in runtime profile JSON, not code) + haptics. Throttle/attitude on sticks, fire on trigger, assists (flight *and* targeting) as physical toggle switches on the cockpit panel — diegetic by default.
- Everything testable via `xr_mock`: state transitions, acquire/release, frame loop. Headset smoke checklist (SteamVR or Monado on the 3090) is manual and separate.
- Comfort design: cockpit-locked view, never teleport — always move the ship; assists are also the comfort system (less drift-sickness than raw Newtonian).
- **Done when:** `ctest` green with a full mock XR session frame loop; on hardware when available: cockpit view of the launch pad at 90 Hz.

## Phase 11 — The vertical slice (ongoing)
- Game loop: **diegetic 3D hangar** loadout (decision #15 — walk around the ship, pick hull, bolt on components incl. weapons on in-space screens) → launch (cockpit view default on desktop) → cruise to a second planet or the moon → land. Two planets + one moon (nested-well testbed, decision #17c), one station, ~25 components including weapons.
- RPG layer plays: pilot XP/progression from flying, trading, and combat with the hostile faction from Phase 9 (its AI pilots already run on the same stats/effects/tags — no special AI stat path); reputation as gameplay tags; trade prices derived from trade-style attributes + faction tags. All stat *definitions* are `game/`-side cooked data — this phase proves the engine/content split was real.
- Diegetic cockpit panels (VR + desktop) render from the stats change-event bus: no polling, no duplicate state. Combat FX/tracers/impact flashes and the targeting reticle are presentation-side consumers of sim events.
- Audio (default here, pull earlier if desired): OGG ingestion through the same cooklet + a minimal mixer — spatial engine hum, weapon and alarm cues; VR lands flat without sound.
- Save/load via the cooked asset format: serialize ship loadout + crew stats + component health + sim state, reload, replay identical trajectory and stats hashes.
- Editor tooling as natural byproduct: the loadout editor is an in-engine debug-UI panel (decision #16) — ECS + pick/drag + ghost previews — and it is the dev twin of the diegetic hangar, both front-ends on the Phase 5 data model.
- **Done when:** you fly it with a keyboard **and** from the cockpit in VR, with a hostile NPC engaging you through the same stats machinery — while all its tests stay headless.

## Phase 12 — "Engine" graduation projects (pick any, anytime)
- Shader + cooked-data hot-reload (watch mode already exists since Phase 0; this makes the engine consume swapped cooked units live). Multithreaded ECS + job scheduler. GPU-driven culling. Deferred/clustered lighting for component-heavy hulls. Keyframe editor (another debug-UI panel, decision #16). Mod loading (mods are the platform — with stats and weapons fully data-driven, mods adding stats/effects/weapons become trivial; the mod browser is a panel over the manifest).
- **VR extras:** motion vectors + reprojection (add motion-vector output to every pass *early* — cheap now, costly later), fixed foveation, eye-tracked foveated rendering.

---

## Sequencing notes (why this order)
- Core-before-renderer (Phases 1/4 before 2/3, swap allowed): the flight/gravity core and assist controllers are the unique learning; rendering is commodity learning you can interleave. If Vulkan stalls for two weeks, jump to Phase 4 — you lose nothing and gain momentum.
- Stats framework (Phase 6) deliberately follows the ship designer (Phase 5): you aggregate stats you have already physically derived, so every Phase 6 test has real inputs instead of toy numbers.
- Combat (Phase 9) deliberately comes after stats (6), swept contacts (7), and time compression (8): damage is an effect, hit tests need the collision primitives, and duels under warp must already be deterministic. Nothing new to learn at combat time — it's pure composition, which is the payoff the engine/content split was built for.
- Arcade physics removes the astrodynamics cliff, but two hard problems remain and they come *after* something runs: well-edge blending for **nested, moving** wells (rails + crossfade, decision #17) + time-compressed physics LOD (Phase 8), and floating-origin/depth precision (Phase 8). Motivated, not abstract.
- **VR is ~free if the Presenter seam + rig/eyes cameras + off-axis projections land in Phases 1–3, and ~a rewrite if discovered in Phase 11.** That's why those constraints are written into the earlier phases' definitions of done, even though the XR phase is Phase 10.

## Reading / references
- OpenXR Specification (swapchains, action system, spaces) + `openxr-hello` as the minimal reference; Monado docs (native Linux runtime)
- Jason Gregory, *Game Engine Architecture* (structure of everything above)
- Modifier-stack patterns as concepts — Dota 2 modifier system and Unreal GAS writeups: steal the *ideas* (source-tagged modifiers, tag gating, effect bundles), implement none of their code
- Babylon 5 / Battlecruiser / **Free Space 2** flight- and combat-feel writeups and postmortems — reference points for wells + assists + hybrid weapons (FS2 is the model for the hybrid weapon kit)
- PD/PID control primers (arcticfux "PD for beginners" or equivalent) — the assist controllers are the game
- Ballistic interception (pure vector / lead-solve derivations, e.g. classic "gunsight lead" writeups) — Phase 9's targeting assist
- CppCon: Mike Acton "Data-Oriented Design", Scott Myers ECS talks
- Valve's moving-origin papers; Leffler/Gummesson floating-origin writeups
- Real-Time Collision Detection (Ericson) for Phase 7 contacts + swept tests

## Standing rules
- Process rules for coding agents live in `AGENTS.md` at repo root (read it before any implementation work).
- One phase-task per commit, tests in the same commit.
- **Branching:** all work on `testing`; `main` gets a promotion only on the user's explicit request, and README.md is the only documentation on `main`.
- **Standalone project:** shares no code, assets, remotes, or repo with any other project.
- No subsystem ships without a headless test proving it; `ctest` green with no `$DISPLAY`, no headset.
- **XR API calls live only in `platform/xr/`** — everything else talks to the Presenter/action interfaces, so `xr_mock` covers CI.
- **Game concepts never enter `core/`** — if it names a game stat or weapon, it's `game/` code or cooked data.
- Validation layers + sanitizers (ASan/UBSan) on every test build; TSan when threading starts.
- Anything you "cheat" (Volk, GLFW, OpenXR loader, stb) gets written in `docs/CHEATS.md` with why, so the from-scratch claim stays honest.
