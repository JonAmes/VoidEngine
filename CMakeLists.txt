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

add_subdirectory(core)
add_subdirectory(gfx)
add_subdirectory(platform)
add_subdirectory(game)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
