# tools/cook — the asset cooklet (decision #13): assets/ source -> cooked/
# binary units + manifest.bin. Runtime loads cooked units only; zero text
# parsing at runtime. Everything here is deterministic: same sources in,
# byte-identical cooked bytes out (golden test 0.9 enforces this).
add_library(ve_cooklet STATIC
    src/cook.cpp
    src/font.cpp
    src/glb.cpp
    src/json.cpp
    src/mikktspace.cpp
    src/png.cpp
    src/shader.cpp
)
target_include_directories(ve_cooklet PUBLIC include)
target_link_libraries(ve_cooklet PUBLIC ve_core stb_headers)

# stb is third-party single-header code compiled as C++: the warning wall
# (-Wall -Wextra -Wpedantic -Werror) governs OUR code. Scoped suppressions
# for the stb implementation TU only (docs/CHEATS.md). GCC/Clang spellings are
# unknown options to cl.exe (D9002) and /WX would make them fatal, so the block
# is scoped to the compilers that have them; MSVC builds this TU unfiltered.
if(NOT MSVC)
set_source_files_properties(src/png.cpp src/font.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-pedantic;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-shift-negative-value;-Wno-maybe-uninitialized;-Wno-class-memaccess;-Wno-stringop-overflow")
endif()

add_executable(ve_cook src/main.cpp)
target_link_libraries(ve_cook PRIVATE ve_cooklet)
