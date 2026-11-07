# platform/ — window+input, Presenter seam (swapchain | xr), session mgmt.
# platform/xr/ (OpenXR) and platform/xr_mock/ arrive in later phases; XR API
# calls live ONLY here (AGENTS.md boundary 2).
add_library(ve_platform STATIC src/version.cpp)
target_include_directories(ve_platform PUBLIC include)
target_link_libraries(ve_platform PUBLIC ve_core)
