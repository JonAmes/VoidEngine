# tools/demo — checklist 2.9: the windowed rotating triangle that RUNS the WSI
# presenter, plus the capture evidence (decision #36). This is an executable, not
# a ctest case: it needs a display. ctest must stay headless-safe (AGENTS), and
# tools/dev/xvfb_run.sh is what drives it on the dev box.
#
# Links ve_platform, so this is also the build-time proof that the GLFW seam is
# the only consumer: ve_demo includes ve/platform/window.h and never GLFW.
add_executable(ve_demo
    src/main.cpp
    src/png_out.cpp
)
target_include_directories(ve_demo PRIVATE include)
target_link_libraries(ve_demo PRIVATE ve_core ve_gfx ve_platform ve_cooklet volk stb_headers)
# The default --shaders path is the repo's assets/, resolved from the source tree
# exactly the way the test targets do it — no hard-coded absolute paths.
target_compile_definitions(ve_demo PRIVATE VE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# stb_image_write is third-party single-header code: the warning wall governs our
# code, so the suppressions are scoped to this one TU (docs/CHEATS.md). MSVC
# spells the same intent with numeric disabling of what stb actually provokes.
if(MSVC)
    set_source_files_properties(src/png_out.cpp PROPERTIES COMPILE_OPTIONS "/wd4702;/wd4267;/wd4244;/wd4100;/wd4996")
else()
    set_source_files_properties(src/png_out.cpp PROPERTIES COMPILE_OPTIONS
        "-Wno-pedantic;-Wno-unused-function;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wnull-dereference")
endif()
