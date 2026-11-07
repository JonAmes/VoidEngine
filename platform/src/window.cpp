#include <ve/platform/window.h>

// Include order matters: GLFW declares glfwCreateWindowSurface / glfwInitVulkanLoader
// only under '#if defined(VK_VERSION_1_0)', and it only pulls <vulkan/vulkan.h> in
// itself when GLFW_INCLUDE_VULKAN is set. volk.h (via window.h) already put the
// Vulkan headers up, so GLFW's Vulkan API is visible without that define — and we
// never need GLFW_INCLUDE_VULKAN, which would demand an SDK header path here.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <utility>

namespace ve::platform {

namespace {

// GLFW reports init/window failures asynchronously through this callback; the
// last message is the only actionable half of a failed create(). A file-local
// sink inside platform/ — nothing in core/ or gfx/ reads it, so it is not
// engine state (AGENTS: no global mutable state in core/).
const char* last_glfw_error = "(no glfw error reported)";

void on_glfw_error(int /*code*/, const char* description) {
    last_glfw_error = description != nullptr ? description : "(no description)";
}

GLFWwindow* as_glfw(void* handle) {
    return static_cast<GLFWwindow*>(handle);
}

} // namespace

std::expected<window, std::string> window::create(int width, int height, const char* title) {
    glfwSetErrorCallback(on_glfw_error);
    if (glfwInit() == GLFW_FALSE) {
        return std::unexpected(std::string("glfwInit failed: ") + last_glfw_error);
    }

    // No client GL context: this window exists for a Vulkan surface, and a GL
    // context would silently make the driver promise things we never use.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Resizable on purpose — the resize -> recreate path in the WSI presenter is
    // only testable by hand if the window can actually change size.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (handle == nullptr) {
        const std::string why = last_glfw_error;
        glfwTerminate(); // GLFW came up; the window did not. Put GLFW back down.
        return std::unexpected(std::string("glfwCreateWindow failed: ") + why);
    }

    window out;
    out.glfw_ = handle;
    return out;
}

window::~window() {
    if (glfw_ != nullptr) {
        glfwDestroyWindow(as_glfw(glfw_));
        // One window per process for Phase 2: GLFW is a process-wide singleton,
        // so teardown is paired here rather than at some later app-level exit.
        glfwTerminate();
        glfw_ = nullptr;
    }
}

window::window(window&& other) noexcept : glfw_(std::exchange(other.glfw_, nullptr)) {}

window& window::operator=(window&& other) noexcept {
    if (this != &other) {
        if (glfw_ != nullptr) {
            glfwDestroyWindow(as_glfw(glfw_));
            glfwTerminate();
        }
        glfw_ = std::exchange(other.glfw_, nullptr);
    }
    return *this;
}

std::vector<std::string> window::required_instance_extensions() const {
    std::vector<std::string> out;
    if (glfw_ == nullptr) {
        return out;
    }
    std::uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (names == nullptr) {
        return out; // no WSI on this platform/backend — caller must not ask for a surface
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.emplace_back(names[i]);
    }
    return out;
}

std::expected<VkSurfaceKHR, std::string> window::create_surface(VkInstance instance) const {
    if (glfw_ == nullptr) {
        return std::unexpected("window is not live");
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (const VkResult r = glfwCreateWindowSurface(instance, as_glfw(glfw_), nullptr, &surface);
        r != VK_SUCCESS) {
        return std::unexpected("glfwCreateWindowSurface failed: VkResult(" +
                               std::to_string(static_cast<int>(r)) +
                               ") — is every required_instance_extensions() name on the VkInstance?");
    }
    return surface;
}

bool window::should_close() const {
    return glfw_ != nullptr && glfwWindowShouldClose(as_glfw(glfw_)) == GLFW_TRUE;
}

void window::poll_events() {
    if (glfw_ != nullptr) {
        glfwPollEvents();
    }
}

std::pair<std::uint32_t, std::uint32_t> window::frame_size() const {
    int width = 0;
    int height = 0;
    if (glfw_ != nullptr) {
        glfwGetFramebufferSize(as_glfw(glfw_), &width, &height);
    }
    // A minimized window reports 0; the swapchain extent is never allowed to go
    // below 1x1, so the floor lands here rather than in the presenter.
    const auto clamp = [](int v) { return static_cast<std::uint32_t>(std::max(v, 1)); };
    return {clamp(width), clamp(height)};
}

} // namespace ve::platform
