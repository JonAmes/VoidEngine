# gfx/ — Vulkan device, render passes, mesh/material, shader compiler.
# Renders to render targets only; a Presenter presents (AGENTS.md boundary 3).
add_library(ve_gfx STATIC
    src/version.cpp
    src/texture.cpp
    src/camera.cpp
    src/descriptor.cpp
    src/pipeline_cache.cpp
    src/mesh.cpp
    src/instance.cpp
    src/device.cpp
    src/presenter.cpp
    src/frame_sync.cpp
    src/wsi_presenter.cpp
    src/shader.cpp
    src/font.cpp
    src/ui.cpp
)
target_include_directories(ve_gfx PUBLIC include)
target_link_libraries(ve_gfx PUBLIC ve_core volk)
