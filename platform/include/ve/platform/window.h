#pragma once
#include <volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

// Desktop windowing lives HERE and nowhere else (Phase 2 note: "GLFW/window code
// lives behind the platform seam"). gfx/ is handed a plain VkSurfaceKHR and never
// learns that GLFW exists — the same reason XR calls never enter gfx/
// (AGENTS boundary 2). The GLFWwindow* is stored as void* so this header drags no
// GLFW types into consumers; the cast happens only inside window.cpp.
//
// A real window needs a desktop seat. On a headless box glfwInit fails outright,
// which is why nothing here runs in CI: 2.8 ships as a compile gate, 2.9 proves
// the window under Xvfb + lavapipe (decision #36).
namespace ve::platform {

class window {
public:
    static std::expected<window, std::string> create(
        int width, int height, const char* title = "void-engine");

    ~window();
    window(const window&) = delete;
    window& operator=(const window&) = delete;
    window(window&& other) noexcept;
    window& operator=(window&& other) noexcept;

    // The instance extensions this surface needs (VK_KHR_surface plus the
    // platform surface extension for whichever window backend GLFW picked).
    // Feed these into ve::gfx::instance::create's extra_extensions. GLFW only
    // answers once glfwInit has chosen its backend, so this needs a live window.
    // Empty on failure — the caller then never gets a surface (see create_surface).
    std::vector<std::string> required_instance_extensions() const;

    // Surface for the WSI presenter. The CALLER owns it: destroy with
    // vkDestroySurfaceKHR before the VkInstance dies. The presenter never does.
    std::expected<VkSurfaceKHR, std::string> create_surface(VkInstance instance) const;

    // Ask the window system for a new size. This is the only way a resize gets
    // *requested* from outside: the frame loop still decides what to do by
    // watching frame_size(), because a window manager (and a headless X server
    // under Xvfb) is free to ignore or clamp the request.
    void set_size(int width, int height);

    bool should_close() const;
    void poll_events();

    // Framebuffer size in pixels — the swapchain extent source. The presenter
    // compares this against its own extent; a mismatch is the resize signal that
    // triggers recreate(). (GLFW's window size is in logical units; the swapchain
    // must match the framebuffer, which is what a HiDPI resize actually changes.)
    std::pair<std::uint32_t, std::uint32_t> frame_size() const;

private:
    window() = default;
    void* glfw_ = nullptr; // GLFWwindow*
};

} // namespace ve::platform
