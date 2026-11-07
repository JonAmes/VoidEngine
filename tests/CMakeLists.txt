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
)
target_link_libraries(engine_tests PRIVATE
    ve_core ve_gfx ve_platform ve_game
    doctest::doctest
)

add_test(NAME engine_tests COMMAND engine_tests)
