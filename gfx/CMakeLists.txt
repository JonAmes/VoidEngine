# gfx/ — Vulkan device, render passes, mesh/material, shader compiler.
# Renders to render targets only; a Presenter presents (AGENTS.md boundary 3).
add_library(ve_gfx STATIC
    src/version.cpp
    src/texture.cpp
    src/instance.cpp
    src/device.cpp
    src/presenter.cpp
    src/shader.cpp
)
target_include_directories(ve_gfx PUBLIC include)
target_link_libraries(ve_gfx PUBLIC ve_core volk)
