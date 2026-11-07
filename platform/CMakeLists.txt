# GLFW (user-approved dep, PLAN.md list) — the ONLY place it is linked.
# System package first: the dev box carries libglfw3-dev (prereq gate 2.1), and a
# source build there would need X11/Wayland dev packages that are not installed.
# The FetchContent fallback is pinned to 3.4 for boxes without the package; if it
# cannot build for lack of window-system headers, that is a configure-time stop
# with a name on it, never a silently missing WSI.
find_package(glfw3 3.3 CONFIG QUIET)
add_library(ve_glfw INTERFACE)
if(TARGET glfw)
    target_link_libraries(ve_glfw INTERFACE glfw)
else()
    include(FetchContent)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # never compiled in CI
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    # Static on the FetchContent path: a shared GLFW drops glfw3.dll next to the
    # build tree, and an exe that cannot find it dies before main() — a missing
    # DLL is a far worse test failure than a static link. Linux distro packages
    # above stay as they ship (shared), which is what the dev box already runs.
    set(GLFW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    target_link_libraries(ve_glfw INTERFACE glfw)
    # Third-party C TU: the warning wall governs our code, never vendored deps
    # (docs/CHEATS.md carries the line). MSVC spells "quiet" differently (#38).
    if(MSVC)
        target_compile_options(glfw PRIVATE /W0)
    else()
        target_compile_options(glfw PRIVATE -w)
    endif()
endif()

# platform/ — window+input, Presenter seam (swapchain | xr), session mgmt.
# platform/xr/ (OpenXR) and platform/xr_mock/ arrive in later phases; XR API
# calls live ONLY here (AGENTS.md boundary 2). 2.8 adds the desktop window half:
# ve_platform is the one and only GLFW consumer; gfx/ receives a VkSurfaceKHR.
add_library(ve_platform STATIC src/version.cpp src/window.cpp)
target_include_directories(ve_platform PUBLIC include)
# ve_glfw is PRIVATE on purpose: window.h exposes only VkSurfaceKHR + void*, so
# nothing outside platform/ needs a GLFW include. PUBLIC here would hand every
# consumer (gfx/, tools/demo) a GLFW include path and let boundary 2 rot into a
# code-review convention; PRIVATE makes an accidental `#include <GLFW/glfw3.h>`
# upstream a compile error. volk stays PUBLIC — window.h includes <volk.h>.
target_link_libraries(ve_platform PUBLIC ve_core volk PRIVATE ve_glfw)

