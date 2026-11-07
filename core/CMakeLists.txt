# core/ — engine's concept-free heart. No gfx/windowing/XR headers, no game
# names; only attributes, modifiers, tags, effects, collision primitives (AGENTS.md).
add_library(ve_core STATIC
    src/version.cpp
    src/log.cpp
    src/assert.cpp
    src/crash.cpp
    src/cooked.cpp
)
target_include_directories(ve_core PUBLIC include)
target_compile_definitions(ve_core PUBLIC VE_VERSION_STRING="${VE_VERSION}")
if(VE_TRACY)
  # Day-one hook (checklist 0.11): PUBLIC so any TU including ve/core/prof.h
  # sees TRACY_ENABLE + the headers; the client links once into every binary.
  target_link_libraries(ve_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(ve_core PUBLIC VE_HAVE_TRACY=1)
endif()
