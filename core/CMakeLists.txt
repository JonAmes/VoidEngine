# core/ — engine's concept-free heart. No gfx/windowing/XR headers, no game
# names; only attributes, modifiers, tags, effects, collision primitives (AGENTS.md).
add_library(ve_core STATIC
    src/version.cpp
    src/log.cpp
    src/assert.cpp
    src/cooked.cpp
    src/rails.cpp
    src/gravity.cpp
    src/rigid.cpp
    src/replay.cpp
    src/preview.cpp
    src/assist.cpp
    src/well_blend.cpp
    src/loadout.cpp
    src/inertia.cpp
    src/thruster.cpp
)
# The crash handler is POSIX by construction (sigaction + backtrace_symbols_fd +
# execinfo.h, and it re-raises so the kernel reports the death). MSVC has no
# execinfo.h; the Windows handler (SetUnhandledExceptionFilter + StackWalk64) is
# decision #38 work still on the table, so the TU is excluded rather than stubbed
# with something that looks like a handler and dumps nothing.
if(NOT MSVC)
  target_sources(ve_core PRIVATE src/crash.cpp)
endif()
target_include_directories(ve_core PUBLIC include)
target_compile_definitions(ve_core PUBLIC VE_VERSION_STRING="${VE_VERSION}")
if(VE_TRACY)
  # Day-one hook (checklist 0.11): PUBLIC so any TU including ve/core/prof.h
  # sees TRACY_ENABLE + the headers; the client links once into every binary.
  target_link_libraries(ve_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(ve_core PUBLIC VE_HAVE_TRACY=1)
endif()
