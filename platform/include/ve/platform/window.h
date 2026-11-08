#pragma once
#include <volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

// AGENTS boundary 2's windowing twin: GLFW is touched ONLY here. gfx/ never
// sees a GLFWwindow — it receives a VkSurfaceKHR and talks VK_KHR_surface,
// which is OS-neutral (decision #38: the Windows port swaps this file's
// guts for the same interface, nothing above it changes).
namespace ve::platform {

class window {
public:
    // GLFW required instance extensions must be enabled on the VkInstance
    // BEFORE any surface is created (2.8: instance::create extras parameter).
    static std::vector<std::string> required_instance_extensions();

    static std::expected<window, std::string> create(std::uint32_t width, std::uint32_t height,
                                                     const char* title);

    ~window();
    window(const window&) = delete;
    window& operator=(const window&) = delete;
    window(window&& other) noexcept;
    window& operator=(window&& other) noexcept;

    bool should_close() const;
    void close();
    void poll_events() const;
    std::pair<std::uint32_t, std::uint32_t> framebuffer_size() const;

    // OS-neutral from here up: GLFW's entry point produces a VkSurfaceKHR;
    // everything gfx calls afterwards is VK_KHR_surface (via volk).
    // VK_NULL_HANDLE + populated error on failure.
    std::expected<VkSurfaceKHR, std::string> create_surface(VkInstance instance) const;

private:
    window() = default;
    void* handle_ = nullptr; // GLFWwindow* — opaque above this file
};

} // namespace ve::platform
