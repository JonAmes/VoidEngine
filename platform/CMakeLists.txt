# platform/ — window+input, Presenter seam (swapchain | xr), session mgmt.
# platform/xr/ (OpenXR) and platform/xr_mock/ arrive in later phases; XR API
# calls ONLY live here (AGENTS.md boundary 2). Same rule for GLFW: the
# find_package below is the single point where the window library enters
# the build (#38: the Windows port points at its own GLFW — same target).
find_package(glfw3 3.3 REQUIRED)
add_library(ve_platform STATIC src/version.cpp src/window.cpp)
target_include_directories(ve_platform PUBLIC include)
# glfw is PRIVATE on purpose: no target above platform/ may include GLFW
# headers (window.h keeps GLFWwindow* as void*); PRIVATE still links it
# through the static archive for the final executables.
# volk PUBLIC: window.h's public API speaks VkSurfaceKHR (VK_KHR_surface via
# volk, decision #36 — OS-agnostic WSI), so consumers need the vulkan headers.
target_link_libraries(ve_platform PUBLIC ve_core volk PRIVATE glfw)
