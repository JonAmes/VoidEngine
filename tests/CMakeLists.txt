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

add_executable(engine_tests
    core/test_smoke.cpp
    core/test_log.cpp
    core/test_assert.cpp
    core/test_crash.cpp
    core/test_profile.cpp
    core/test_math_base.cpp
    core/test_math_vec.cpp
    core/test_math_dvec3.cpp
    core/test_math_mat4.cpp
    tools/test_container.cpp
    tools/test_glb.cpp
    tools/test_incremental.cpp
    tools/test_watch.cpp
    gfx/test_texture.cpp
)
target_compile_definitions(engine_tests PRIVATE
    VE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VE_COOK_TOOL="${CMAKE_BINARY_DIR}/tools/cook/ve_cook")
target_link_libraries(engine_tests PRIVATE
    ve_core ve_gfx ve_platform ve_game ve_cooklet
    doctest::doctest stb_headers
)
# stb implementation macro in this TU: same scoped suppressions as png.cpp
set_source_files_properties(gfx/test_texture.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-pedantic;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-shift-negative-value;-Wno-maybe-uninitialized;-Wno-class-memaccess;-Wno-stringop-overflow;-Wno-missing-field-initializers")
# backtrace_symbols needs dynamic symbols for readable frames
target_link_options(engine_tests PRIVATE -rdynamic)

add_test(NAME engine_tests COMMAND engine_tests)
