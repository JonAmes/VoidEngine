# tools/cook — the asset cooklet (decision #13): assets/ source -> cooked/
# binary units + manifest.bin. Runtime loads cooked units only; zero text
# parsing at runtime. Everything here is deterministic: same sources in,
# byte-identical cooked bytes out (golden test 0.9 enforces this).
add_library(ve_cooklet STATIC
    src/container.cpp
    src/cook.cpp
)
target_include_directories(ve_cooklet PUBLIC include)
target_link_libraries(ve_cooklet PUBLIC ve_core)

add_executable(ve_cook src/main.cpp)
target_link_libraries(ve_cook PRIVATE ve_cooklet)
