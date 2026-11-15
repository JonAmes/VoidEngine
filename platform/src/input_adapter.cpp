#include "ve/platform/input_adapter.h"

#include <GLFW/glfw3.h>

namespace ve::platform {

void input_adapter::set_frame_scale(std::uint32_t content_w, std::uint32_t content_h,
                                    std::uint32_t frame_w, std::uint32_t frame_h) {
    // Minimized windows report content 0x0: keep the last sane scale rather
    // than dividing by zero (no cursor events arrive while minimized anyway).
    if (content_w == 0 || content_h == 0) return;
    sx_ = static_cast<float>(frame_w) / static_cast<float>(content_w);
    sy_ = static_cast<float>(frame_h) / static_cast<float>(content_h);
}

void input_adapter::cursor(double x, double y) {
    cx_ = static_cast<float>(x) * sx_;
    cy_ = static_cast<float>(y) * sy_;
}

void input_adapter::mouse(button b, bool pressed) {
    if (b == button::left) down_ = pressed;
}

void input_adapter::press(key k) {
    switch (k) {
    case key::backspace: bs_ = true; break;
    case key::enter: en_ = true; break;
    case key::escape: esc_ = true; break;
    }
}

void input_adapter::character(std::uint32_t cp) {
    // Manual utf-8 (no runtime dependency; #15 "cooked text" also means no
    // locale/codec machinery in the input path).
    if (cp == 0 || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) return;
    if (cp < 0x80u) {
        chars_.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        chars_.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        chars_.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        chars_.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        chars_.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        chars_.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        chars_.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        chars_.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        chars_.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        chars_.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

ve::gfx::ui_io input_adapter::next_frame() {
    ve::gfx::ui_io io;
    io.mouse_x = cx_;
    io.mouse_y = cy_;
    io.mouse_down = down_;
    io.chars.swap(chars_); // drained per frame: typed bytes belong to ONE frame
    chars_.clear();
    io.backspace = bs_;
    io.enter = en_;
    io.escape = esc_;
    bs_ = en_ = esc_ = false; // edge-latched: one frame per press
    return io;
}

// ---- GLFW shim ------------------------------------------------------------
// The ONLY place GLFW device codes are named outside tools' window setup;
// maps them to the adapter's device-free vocabulary.

input_adapter* adapter_of(GLFWwindow* w) {
    return static_cast<input_adapter*>(glfwGetWindowUserPointer(w));
}

void on_cursor(GLFWwindow* w, double x, double y) {
    if (auto* ad = adapter_of(w)) ad->cursor(x, y);
}

void on_mouse(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* ad = adapter_of(w);
    if (ad == nullptr) return;
    input_adapter::button b;
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT: b = input_adapter::button::left; break;
    case GLFW_MOUSE_BUTTON_RIGHT: b = input_adapter::button::right; break;
    default: b = input_adapter::button::middle; break;
    }
    ad->mouse(b, action != GLFW_RELEASE);
}

void on_key(GLFWwindow* w, int key, int /*sc*/, int action, int /*mods*/) {
    auto* ad = adapter_of(w);
    if (ad == nullptr || action == GLFW_REPEAT) return; // presses only (header rule)
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_BACKSPACE: ad->press(input_adapter::key::backspace); break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER: ad->press(input_adapter::key::enter); break;
    case GLFW_KEY_ESCAPE: ad->press(input_adapter::key::escape); break;
    default: break; // no other ui_io actions exist yet (#54d)
    }
}

void on_char(GLFWwindow* w, unsigned int cp) {
    if (auto* ad = adapter_of(w)) ad->character(cp);
}

void install_glfw_input(void* glfw_window, input_adapter& ad) {
    auto* w = static_cast<GLFWwindow*>(glfw_window);
    glfwSetWindowUserPointer(w, &ad);
    glfwSetCursorPosCallback(w, on_cursor);
    glfwSetMouseButtonCallback(w, on_mouse);
    glfwSetKeyCallback(w, on_key);
    glfwSetCharCallback(w, on_char);
}

} // namespace ve::platform
