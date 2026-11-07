cmake_minimum_required(VERSION 3.28)

# Single source of truth for the version (AGENTS.md: never hardcode it elsewhere).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VE_VERSION LIMIT_COUNT 1)

project(void-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning wall (AGENTS.md): never weakened to pass a build — fix the code.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Sanitizers on by default for everything that runs tests (AGENTS.md contract).
option(VE_SANITIZERS "Build ASan+UBSan instrumentation into test builds" ON)
if(VE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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
    target_compile_options(TracyClient PRIVATE -w)
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
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
