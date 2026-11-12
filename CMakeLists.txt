cmake_minimum_required(VERSION 3.28)

# Single source of truth for the version (AGENTS.md: never hardcode it elsewhere).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VE_VERSION LIMIT_COUNT 1)

project(void-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning wall (AGENTS.md): never weakened to pass a build — fix the code.
# Compiler-gated, not softened (decision #38): the GCC/Clang wall is exactly what
# it always was; MSVC has no -Wextra/-Wpedantic spelling, so it gets the closest
# real wall (/W4 + conformance mode) and /WX keeps "warnings are errors" true on
# both. Nothing here lowers the bar for code that is also built on Linux.
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /Zc:preprocessor /WX)
  # C4996 is the CRT's "consider the _s variant" nudge, not a correctness
  # diagnostic: fopen/getenv are the portable spellings, and the _s variants do
  # not exist on the Linux toolchain this code primarily targets. Same call GLFW
  # makes for its own MSVC build. /W4 + /WX stay in force — the wall itself is
  # untouched.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Sanitizers on by default for everything that runs tests (AGENTS.md contract).
# UBSan has no MSVC equivalent, so ASan-only is the Windows story — and ASan
# needs /Zi (CMake's DEBUG/RELWITHDEBINFO), which the default ctest build here is
# not. Off is stated out loud rather than silently skipped.
option(VE_SANITIZERS "Build ASan+UBSan instrumentation into test builds" ON)
if(VE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(VE_SANITIZERS AND MSVC)
  message(STATUS "VE_SANITIZERS: no UBSan on MSVC and ASan needs a debug build — "
                 "instrumentation OFF for this configuration (decision #38 open item)")
endif()


# Tracy day-one profiler hook (user approved 2026-11-07 as 7th dependency).
# VE_TRACY=ON: TRACY_ENABLE propagates to every engine TU; FrameMark/zones are
# one macro away wherever the frame loop lands. OFF keeps zero-cost no-op macros.
option(VE_TRACY "Wire the Tracy profiler client into engine targets" ON)
if(VE_TRACY)
  include(FetchContent)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy
    GIT_TAG        v0.15.0
    GIT_SHALLOW    TRUE)
  set(TRACY_STATIC ON CACHE BOOL "" FORCE)
  set(TRACY_ENABLE ON CACHE BOOL "" FORCE)          # instrument: this IS the hook
  set(TRACY-manual-lifetime OFF CACHE BOOL "" FORCE) # profiler lives with the process
  FetchContent_MakeAvailable(tracy)
  if(TARGET TracyClient)  # Tracy::TracyClient is an alias; flags need the real target
    # Third-party TUs never govern our wall (docs/CHEATS.md lists the scope):
    # our flags stay in the line; -w last means tracy emits nothing.
    if(MSVC)
      target_compile_options(TracyClient PRIVATE /w) # same intent, MSVC spelling
    else()
      target_compile_options(TracyClient PRIVATE -w)
    endif()
  endif()
endif()

# Volk (user-approved dep): generated Vulkan loader wrapper. Pinned tag; the
# generated loader code is vendored by the repo, so no new build-time network
# fetch beyond this checkout. volk.h includes vulkan.h headers it ships itself.
include(FetchContent)
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.313.0 # volk tags follow Vulkan SDK releases; closest >= this box's 1.4.312 loader
    GIT_SHALLOW TRUE
)
set(VOLK_STATIC_INLINE OFF)
set(VOLK_BUILD_TESTS OFF)
FetchContent_MakeAvailable(volk)

add_subdirectory(core)
add_subdirectory(gfx)
add_subdirectory(platform)
add_subdirectory(game)

# doctest fetch hoisted above tools/ (7.16.2): ve_demo --mode flight includes
# the shared flight fixture (tests/core/flight_scenario.h), which includes
# <doctest/doctest.h> (disabled via DOCTEST_CONFIG_DISABLE in that TU), so the
# header-only doctest target must exist before tools/ links it. tests/ keeps
# its MSVC flag patch and still owns its test sources.
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
)
set(DOCTEST_WITH_TESTS OFF) # library only; we own our main
FetchContent_MakeAvailable(doctest)

add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
