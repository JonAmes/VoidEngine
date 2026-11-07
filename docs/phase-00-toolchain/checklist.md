# Phase 0 — Toolchain & skeleton
Status: in progress

## Tasks — execute top to bottom, one commit each
- [x] 0.1 Toolchain bootstrap, no sudo: user-local `cmake` + `ninja` (system pip or official tarballs into `~/.local/`), committed as `tools/bootstrap_toolchain.sh` so any machine reprovisions — test: both report versions from a fresh shell; script re-run is idempotent — commit: 5386e9e
- [x] 0.2 CMake superstructure: root `CMakeLists.txt` + `VERSION` file (`00.00.00`, read by CMake, single source of truth) + stub static libs `core/ gfx/ platform/ game/` + `tools/` + warning wall `-Wall -Wextra -Wpedantic -Werror` on C++23 — test: `cmake -G Ninja` configure + build green headless — commit: fc8eee1
- [x] 0.3 doctest wired (FetchContent) + `engine_tests` target + ctest registration — test: `engine_tests` boots and passes first smoke test headless — commit: ddd82a9
- [x] 0.4 `tools/ci_local.sh`: configure + build + `ctest` under `env -u DISPLAY -u WAYLAND_DISPLAY`, fails if anything needs a display — test: script exits 0 on the stub suite; exists before any later box can be checked — commit: ad6dcb1
- [x] 0.5 Logging (levels + file sink), assert macro (release-strippable, `core/`-safe — no exceptions), crash handler (signal → stack dump to log) — test: `log_levels` (filter + file sink), `assert_macro` — commit: aa3e657
- [x] 0.6 Cooklet skeleton (`tools/cook`) + cooked container: magic + version + payload, self-contained independently-loadable units; `manifest.bin` with stable content IDs (relative paths, decision #13), dependency graph, incremental re-cook by content hash, watch mode (change → instant re-cook) — test: `container_roundtrip`, `manifest_ids_stable`, `incremental_recook` (only changed unit re-cooked), `watch_mode_fires` — commit: fec1690
- [ ] 0.7 PNG texture path: stb (FetchContent) loads PNG → cooklet emits cooked texture unit → runtime loader reads back — test: `texture_roundtrip` (pixels match source via loader, no stb at runtime) — commit: —
- [ ] 0.8 Blender PoC ship cooks: copy `docs/tests/blender/test_ship.glb` → `assets/samples/`; minimal GLB reader in the cooklet (tools-only, hand-rolled per doctrine) emits mesh + material + **5 socket units**; socket names + `extras` present in cooked output; Y-up → engine axis conversion applied — test: `glb_cook_poc` (5 sockets by authored name, extras survive, axis flip verified against the PoC report) — commit: —
- [ ] 0.9 Cook determinism golden: cooking the same sources twice → **byte-identical** cooked units — test: `cook_determinism` (hash-compare two cook runs) — commit: —
- [ ] 0.10 Cook failure hygiene: a def referencing a missing texture fails the cook cleanly — nonzero exit, actionable message, no crash — test: `cook_missing_ref_fails` — commit: —
- [ ] 0.11 Tracy profiler hooked day one — **blocked on dependency approval** (would be the 7th dep; allowed set is GLFW/Volk/OpenXR/stb/doctest + miniaudio #32a). If not approved: box records "deferred by user decision" and PLAN's day-one hook note is amended — test: n/a (integration check: frame mark appears in a Tracy capture) — commit: —

## Done-when (every PLAN.md "Done when" bullet linked to a task; all checked = phase DONE)
- [ ] (a) `engine_tests` green headless ← 0.2, 0.3, 0.4
- [ ] (b) cooklet round-trips a test asset (PNG → cooked texture → loader readback) through the manifest ← 0.6, 0.7
- [ ] (c) Blender PoC ship (`test_ship.glb` in `assets/samples/`) cooks into mesh + material + 5 socket units — socket names + `extras` in cooked output, Y-up→engine-axis conversion applied ← 0.8
- [ ] (d) cooking twice is byte-identical (golden test) ← 0.9
- [ ] (e) a def file referencing a missing texture fails the cook cleanly, not crashes ← 0.10

## Notes
- No system sudo on this machine: Vulkan/GLFW/OpenXR **dev packages are not installed and not needed in Phase 0** — lavapipe runtime (`libvulkan_lvp.so` + ICD) is already present for later phases; dev-side deps arrive via FetchContent/vcpkg in the phases that consume them. If any apt-level package turns out unavoidable, stop and ask the user (interactive sudo required).
- Exact build commands get recorded in README + this folder's notes.md once 0.2 lands (AGENTS.md: Phase 0 defines them).
