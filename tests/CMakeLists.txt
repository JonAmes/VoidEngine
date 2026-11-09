# tests/ — doctest suites, per-module (Tier 1 here; gfx Tier 2 + xr_mock Tier 3
# suites arrive with their phases). ctest must stay green with no $DISPLAY.
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
)
set(DOCTEST_WITH_TESTS OFF) # library only; we own our main
FetchContent_MakeAvailable(doctest)
# doctest 2.4.11 specializes std::tuple, which VS 18's STL now flags (C5285:
# forbidden by N5014). Third-party code never governs our warning wall
# (docs/CHEATS.md) — the suppression is scoped to the doctest interface target,
# so it lands only on TUs that include that header, and /W4 /WX still apply to
# everything of ours. Pinned tag stays as-is: bumping doctest is its own decision.
if(MSVC)
    # doctest::doctest is an ALIAS; flags have to go on the real target (same
    # alias-vs-real rule as TracyClient in the top-level CMakeLists).
    target_compile_options(doctest INTERFACE /wd5285)
endif()

add_executable(engine_tests
    core/test_smoke.cpp
    core/test_log.cpp
    core/test_assert.cpp
    core/test_profile.cpp
    core/test_math_base.cpp
    core/test_math_vec.cpp
    core/test_math_dvec3.cpp
    core/test_math_mat4.cpp
    core/test_math_quat.cpp
    core/test_math_dquat.cpp
    core/test_sim_rails.cpp
    core/test_sim_gravity.cpp
    core/test_sim_well_blend.cpp
    core/test_sim_integrator.cpp
    core/test_sim_replay.cpp
    core/test_math_projection.cpp
    core/test_math_coords.cpp
    tools/test_container.cpp
    tools/test_glb.cpp
    tools/test_mikktspace.cpp
    tools/test_incremental.cpp
    tools/test_watch.cpp
    tools/test_font_atlas.cpp
    gfx/test_texture.cpp
    gfx/test_texture_gpu.cpp
    gfx/test_instance.cpp
    gfx/test_device.cpp
    gfx/test_offscreen_target.cpp
    tools/test_shader_cook.cpp
    gfx/test_shader_module.cpp
    gfx/test_triangle_golden.cpp
    gfx/test_golden_harness.cpp
    gfx/gfx_golden.cpp
    gfx/test_frame_sync.cpp
    gfx/test_camera_rig.cpp
    gfx/test_depth_reversed_z.cpp
    gfx/test_pipeline_cache.cpp
    gfx/test_mesh_upload.cpp
    gfx/test_instanced_eyes.cpp
    gfx/test_pbr_lite.cpp
    gfx/test_shadow_map.cpp
    gfx/test_text_draw.cpp
    gfx/test_ui_immediate.cpp
    gfx/test_world_canvas.cpp
    gfx/test_scene_orbit.cpp
    gfx/test_wsi_compile.cpp
    gfx/test_validation_strict.cpp
)
target_compile_definitions(engine_tests PRIVATE
    VE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    # $<TARGET_FILE:...> is the only spelling that is right on every generator:
    # the hardcoded tools/cook/ve_cook path assumes Ninja's layout AND a no-suffix
    # executable, so it breaks under Visual Studio generators and on Windows
    # (.exe). Same string on Linux, exact path everywhere else (#38).
    VE_COOK_TOOL="$<TARGET_FILE:ve_cook>")
target_link_libraries(engine_tests PRIVATE
    ve_core ve_gfx ve_platform ve_game ve_cooklet
    doctest::doctest stb_headers
)
# The crash suite forks a child and reads the parent's stack symbols
# (sys/wait.h, unistd.h, execinfo.h) — POSIX by nature, and ve_core only builds
# src/crash.cpp on those platforms. Excluded on MSVC, where the crash handler
# itself is still decision #38 work; every other suite stays in the binary.
if(NOT MSVC)
    target_sources(engine_tests PRIVATE core/test_crash.cpp)
endif()
# stb implementation macro in this TU: same scoped suppressions as png.cpp.
# GCC/Clang spellings only — under MSVC they become D9002 "unknown option" and
# /WX would turn that into a build failure, so the block is scoped, not skipped.
if(NOT MSVC)
set_source_files_properties(gfx/gfx_golden.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-pedantic;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-shift-negative-value;-Wno-maybe-uninitialized;-Wno-class-memaccess;-Wno-stringop-overflow;-Wno-missing-field-initializers")
endif()
# backtrace_symbols needs dynamic symbols for readable frames. The flag is a
# linker option, so a linker that does not know it never sees it: an unknown
# option to link.exe is a warning, and the warning wall makes warnings fatal.
if(NOT MSVC)
target_link_options(engine_tests PRIVATE -rdynamic)
endif()

add_test(NAME engine_tests COMMAND engine_tests)
