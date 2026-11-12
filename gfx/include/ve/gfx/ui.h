#pragma once

#include <cstdint>
#include <vector>

// Immediate-mode debug UI (3.13; the widget layer the editor toolkit #16 will
// use). Hand-rolled ImGui-style core, pure CPU: NO Vulkan, NO device/GLFW
// types — input arrives as neutral per-frame actions (ui_io, the #15 input
// seam), and a frame emits a draw-command target list (pos/uv/color quads)
// that one UI render pass draws onto a presenter render target (#15: never
// "the backbuffer").
//
// Determinism: layout is integer-pixel math in emit order, IDs hash label +
// window scope, state containers are ordered. The same io sequence fed to the
// same widget code produces byte-identical draw lists — the test pins that.
namespace ve::gfx {

struct font0; // ve/gfx/font.h (cooked FONT0)

struct ui_io {
    // Pointer in top-left-origin screen pixels (same space as draw coords).
    float mouse_x = 0.f, mouse_y = 0.f;
    bool mouse_down = false;
    // Key events as actions, never device codes (#15). chars = printable
    // bytes typed since the last frame (UTF-8 passed through); editing keys
    // are explicit booleans.
    std::vector<char> chars;
    bool backspace = false, enter = false, escape = false;
};

struct ui_vertex {
    float x = 0.f, y = 0.f;     // pixels, top-left origin (Vulkan y-down space)
    float u = 0.f, v = 0.f;     // texcenter into the command's texture
    float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
};

struct ui_draw_cmd {
    // Texture slot the pass must bind for this command:
    //   0 = solid white (flat rects), 1 = font atlas (glyph quads).
    int texture_slot = 0;
    std::uint32_t idx_offset = 0, idx_count = 0;
    // Scissor rect (px) — window content clips to its client rect.
    std::int32_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
};

struct ui_frame {
    std::vector<ui_vertex> verts;
    std::vector<std::uint32_t> indices;
    std::vector<ui_draw_cmd> cmds; // draw order = emit order (painter's algorithm)
};

class ui_context {
public:
    void begin_frame(const ui_io& io, const font0& font,
                     std::int32_t screen_w, std::int32_t screen_h);

    // Returns false while the window is closed (body code is then skipped by
    // the caller). Closing via the title-bar [x] sets *open = false.
    bool begin_window(const char* title, std::int32_t x, std::int32_t y,
                      std::int32_t w, std::int32_t h, bool* open = nullptr);
    void end_window();

    bool button(const char* label);                  // true on press+release over it
    bool checkbox(const char* label, bool* value);   // true when toggled this frame
    void label(const char* text);                    // static text
    // Editable text: click focuses; chars/backspace edit while focused.
    // true on Enter while focused (commit signal).
    bool text_field(const char* label, char* buf, std::size_t cap);
    // 8.1 def-forms: text_field releases focus on the Enter that commits.
    // A REJECTED entry re-focuses here so the draft stays editable without
    // a re-click (#48 "garbage hard to enter, easy to fix"). Only ever call
    // immediately after text_field returned true, same frame, same window
    // (id = label + window scope hash).
    void refocus_text(const char* label) { focus_ = hash_id(label); }

    void end_frame(); // finalize draw list
    const ui_frame& frame() const { return frame_; }

    // Fixed layout metrics (tests pin exact rects against these).
    static constexpr int kTitleH = 16, kPad = 8, kRowH = 18, kSpacing = 6,
                         kBox = 12;

private:
    struct rect { std::int32_t x, y, w, h; };

    std::uint64_t hash_id(const char* s) const;      // label + window scope
    bool hot(std::uint64_t id, rect r);              // hover + records hot/capture
    void add_rect(rect r, std::uint8_t r8, std::uint8_t g8, std::uint8_t b8);
    void add_text(std::int32_t x, std::int32_t baseline_y, const char* text,
                  std::uint8_t r8, std::uint8_t g8, std::uint8_t b8);
    void quad(rect r, float u0, float v0, float u1, float v1,
              float cr, float cg, float cb, float ca, int slot);

    ui_frame frame_;
    const ui_io* io_ = nullptr;
    const font0* font_ = nullptr;
    std::int32_t screen_w_ = 0, screen_h_ = 0;
    float mx_ = 0.f, my_ = 0.f;
    bool down_ = false, pressed_ = false, released_ = false, prev_down_ = false;
    std::uint64_t hot_ = 0, active_ = 0, held_ = 0, focus_ = 0;
    std::uint64_t hot_candidate_ = 0;

    // Window stack state (one window deep is enough for the debug layer;
    // struct leaves room for stacking without a rewrite).
    bool in_window_ = false;
    std::uint64_t win_id_ = 0;
    rect win_client_{};    // clip rect for emitted content
    std::int32_t cur_x_ = 0, cur_y_ = 0; // layout cursor
};

} // namespace ve::gfx
