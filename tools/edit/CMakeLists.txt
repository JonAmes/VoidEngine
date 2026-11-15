# tools/edit — the editor-kit host (decision #54, docs/tooling-editorkit).
# ve_edit: a live editor window driving the authoring panels over an EMPTY
# world (no art beyond the one cooked glyph atlas, no viewport, no live sim).
# Executable, NEVER a ctest case (it needs a display). Scripted mode carries
# the 2.9 self-attesting exit-code contract; tools/dev/xvfb_edit_run.sh is
# the driver, ci_local never touches either.
add_executable(ve_edit src/main.cpp)
target_sources(ve_edit PRIVATE ../demo/src/png_out.cpp)
target_include_directories(ve_edit PRIVATE ../demo/include)
target_link_libraries(ve_edit PRIVATE ve_core ve_gfx ve_platform ve_cooklet volk stb_headers)
target_compile_definitions(ve_edit PRIVATE VE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  set_source_files_properties(../demo/src/png_out.cpp PROPERTIES COMPILE_OPTIONS "/wd4702;/wd4267;/wd4244;/wd4100;/wd4996")
else()
  set_source_files_properties(../demo/src/png_out.cpp PROPERTIES COMPILE_OPTIONS "-Wno-pedantic;-Wno-unused-function;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wnull-dereference;-Wno-missing-field-initializers")
endif()
