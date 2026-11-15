#pragma once
// Decision #15's action-input seam, first slice (decision #54d): GLFW raw
// events map through this adapter to gfx::ui_io. The header knows NOTHING of
// GLFW (ve_glfw links PRIVATE into ve_platform precisely so GLFW.h can never
// leak); the callbacks that call these methods live in input_adapter.cpp.
// Panels consume gfx::ui_io only — no per-device input code, ever.
#include <ve/gfx/ui.h>

#include <cstdint>

namespace ve::platform {

class input_adapter {
public:
    enum class button { left, right, middle };
    enum class key { backspace, enter, escape }; // grow when panels need more

    // GLFW cursor px are CONTENT-space; ui draw space is FRAMEBUFFER px
    // (HiDPI). Feed both sizes on create/resize; cursor() scales by the ratio.
    void set_frame_scale(std::uint32_t content_w, std::uint32_t content_h,
                         std::uint32_t frame_w, std::uint32_t frame_h);
    void cursor(double x, double y); // content-space px
    // ui_io carries a single mouse_down: left button drives it; others are
    // tracked for future panels but never set it.
    void mouse(button b, bool pressed);
    // Presses only: the GLFW shim drops GLFW_REPEAT here (glyph repeat already
    // arrives through character(), and latching repeat would fight the field).
    void press(key k);
    // Unicode codepoint (GLFW char callback); utf-8 encoded into pending
    // chars. Lone surrogates dropped, cp > 0x10FFFF dropped.
    void character(std::uint32_t cp);

    // Snapshot this frame's ui_io and drain the one-shot state (typed chars,
    // latched keys). Cursor/button are stateful; chars/keys are edge data.
    ve::gfx::ui_io next_frame();

private:
    float cx_ = 0.f, cy_ = 0.f;      // framebuffer-space px
    float sx_ = 1.f, sy_ = 1.f;      // content -> framebuffer scale
    bool down_ = false;              // left button
    bool bs_ = false, en_ = false, esc_ = false;
    std::vector<char> chars_;
};

// Install the GLFW cursor-pos / mouse-button / key / char callbacks that route
// into ad (via the window's user pointer). glfw_window is a GLFWwindow* —
// void* keeps GLFW out of this header (same convention as window::glfw_).
void install_glfw_input(void* glfw_window, input_adapter& ad);

} // namespace ve::platform
