// The ONLY file in the repo that includes GLFW (AGENTS boundary 2's twin for
// windowing, per header notes in ve/platform/window.h). Everything above
// this seam sees VkSurfaceKHR + VK_KHR_surface — #38's Windows port swaps
// this file's guts for glfwWin32 equivalents (same GLFW API there anyway).
#include <ve/platform/window.h>

#include <GLFW/glfw3.h>

#include <string>
#include <utility>

namespace ve::platform {

namespace {

// glfwInit/glfwTerminate are process-wide; refcount so multiple windows
// (or tools coexisting) don't trample each other's lifecycle. Global state
// like this is banned in core/ — platform/ is its designated home.
int glfw_live_count = 0;

GLFWwindow* as_glfw(void* h) { return static_cast<GLFWwindow*>(h); }

std::string glfw_error_text() {
    const char* desc = nullptr;
    const int code = glfwGetError(&desc);
    return "glfw error code=" + std::to_string(code) + " " + (desc != nullptr ? desc : "(no description)");
}

} // namespace

std::vector<std::string> window::required_instance_extensions() {
    // GLFW answers this WITHOUT glfwInit and without a window system (its
    // docs guarantee the call): the list is baked at GLFW build time
    // (xlib here, win32 on Windows — #38). Empty on boxes where GLFW could
    // not even talk to a Vulkan loader; callers must handle empty.
    std::uint32_t count = 0;
    const char** raw = glfwGetRequiredInstanceExtensions(&count);
    std::vector<std::string> out;
    if (raw != nullptr) {
        out.assign(raw, raw + count);
    }
    return out;
}

std::expected<window, std::string> window::create(std::uint32_t width, std::uint32_t height,
                                                  const char* title) {
    if (glfw_live_count == 0 && glfwInit() != GLFW_TRUE) {
        return std::unexpected("glfwInit failed: " + glfw_error_text());
    }
    ++glfw_live_count;

    // No OpenGL context, ever: this engine speaks Vulkan; GLFW only makes
    // the window + VkSurfaceKHR.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* handle = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                          title, nullptr, nullptr);
    if (handle == nullptr) {
        const std::string err = "glfwCreateWindow failed: " + glfw_error_text();
        if (--glfw_live_count == 0) {
            glfwTerminate();
        }
        return std::unexpected(err);
    }

    window out;
    out.handle_ = handle;
    return out;
}

window::~window() {
    if (handle_ == nullptr) {
        return;
    }
    glfwDestroyWindow(as_glfw(handle_));
    if (--glfw_live_count == 0) {
        glfwTerminate();
    }
}

window::window(window&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

window& window::operator=(window&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            glfwDestroyWindow(as_glfw(handle_));
            if (--glfw_live_count == 0) {
                glfwTerminate();
            }
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

bool window::should_close() const {
    return handle_ != nullptr && glfwWindowShouldClose(as_glfw(handle_)) != 0;
}

void window::close() {
    if (handle_ != nullptr) {
        glfwSetWindowShouldClose(as_glfw(handle_), GLFW_TRUE);
    }
}

void window::poll_events() const {
    glfwPollEvents();
}

std::pair<std::uint32_t, std::uint32_t> window::framebuffer_size() const {
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(as_glfw(handle_), &w, &h); // pixel size, not CSS-ish window size
    return {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

std::expected<VkSurfaceKHR, std::string> window::create_surface(VkInstance instance) const {
    if (handle_ == nullptr) {
        return std::unexpected("window is dead");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        return std::unexpected("glfwVulkanSupported: no Vulkan loader or WSI support in GLFW");
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // The one GLFW→Vulkan call in the codebase. Everything gfx does with
    // the result is VK_KHR_surface via volk (OS-neutral, #38). Note the
    // GLFW argument order (instance first — unlike the native-* functions).
    const VkResult r = glfwCreateWindowSurface(instance, as_glfw(handle_), nullptr, &surface);
    if (r != VK_SUCCESS) {
        return std::unexpected("glfwCreateWindowSurface: VkResult(" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    return surface;
}

} // namespace ve::platform
