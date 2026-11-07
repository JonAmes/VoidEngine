# core/ — engine's concept-free heart. No gfx/windowing/XR headers, no game
# names; only attributes, modifiers, tags, effects, collision primitives (AGENTS.md).
add_library(ve_core STATIC src/version.cpp)
target_include_directories(ve_core PUBLIC include)
target_compile_definitions(ve_core PUBLIC VE_VERSION_STRING="${VE_VERSION}")
