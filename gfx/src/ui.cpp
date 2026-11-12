#include "ve/gfx/ui.h"

#include "ve/gfx/font.h"

#include <algorithm>
#include <cstring>

namespace ve::gfx {

namespace {
constexpr float c(float v) { return v / 255.0f; }
} // namespace

namespace {
std::uint64_t fnv1a(const char* s) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char* p = s; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

std::uint64_t ui_context::hash_id(const char* s) const {
    // FNV-1a over the label, mixed with the window scope: identical labels in
    // different windows get different ids (stable across frames by
    // construction — the scope itself must never feed back into itself).
    std::uint64_t h = fnv1a(s);
    h ^= win_id_;
    h *= 1099511628211ull;
    return h;
}

std::uint64_t ui_context::hash_scoped(const char* s, const char* salt) const {
    // Same window mixing as hash_id; salt folded in so equal labels with
    // different salts never collide (and frames stay byte-deterministic).
    std::uint64_t h = fnv1a(s);
    h ^= fnv1a(salt);
    h ^= win_id_;
    h *= 1099511628211ull;
    return h;
}

void ui_context::begin_frame(const ui_io& io, const font0& font,
                             std::int32_t screen_w, std::int32_t screen_h) {
    frame_ = {};
    io_ = &io;
    font_ = &font;
    screen_w_ = screen_w;
    screen_h_ = screen_h;
    mx_ = io.mouse_x;
    my_ = io.mouse_y;
    down_ = io.mouse_down;
    pressed_ = down_ && !prev_down_;
    released_ = !down_ && prev_down_;
    prev_down_ = down_;
    hot_candidate_ = 0;
    in_window_ = false;
    win_client_ = {0, 0, screen_w, screen_h}; // no window: clip to full target
    if (io.escape) focus_ = 0;
}

bool ui_context::begin_window(const char* title, std::int32_t x, std::int32_t y,
                              std::int32_t w, std::int32_t h, bool* open) {
    // Scope root = pure function of the title. (Deriving it via hash_id()
    // would fold the PREVIOUS win_id_ in, making ids shift every frame and
    // breaking press-release matching.)
    win_id_ = fnv1a(title) ^ 0x9e3779b97f4a7c15ull;
    in_window_ = true;
    cur_x_ = x + kPad;
    cur_y_ = y + kTitleH + kPad;
    // Clip = the WHOLE window (chrome included); content beyond the client
    // rect is the caller's layout problem at debug-UI scale.
    win_client_ = {x, y, w, h};
    if (open && !*open) return false; // closed: no chrome, no body

    // Title bar, body, close [x].
    add_rect({x, y, w, kTitleH}, 50, 55, 70);
    add_rect({x, y + kTitleH, w, h - kTitleH}, 38, 42, 55);
    const rect close_btn = {x + w - kTitleH, y, kTitleH, kTitleH};
    const std::uint64_t close_id = hash_id("##close");
    const bool close_hover = close_btn.x <= mx_ && mx_ < close_btn.x + close_btn.w &&
                             close_btn.y <= my_ && my_ < close_btn.y + close_btn.h;
    if (close_hover) hot_candidate_ = close_id;
    if (pressed_ && close_hover) held_ = close_id;
    if (released_ && held_ == close_id && close_hover && open) *open = false;
    add_rect(close_btn,
             close_hover ? 170 : 50, close_hover ? 60 : 55, close_hover ? 60 : 70);
    add_text(close_btn.x + 4, y + (kTitleH - (font_->ascent - font_->descent)) / 2 +
                                   font_->ascent,
             "x", 232, 234, 240);
    add_text(x + kPad, y + (kTitleH - (font_->ascent - font_->descent)) / 2 +
                           font_->ascent,
             title, 232, 234, 240);
    return true;
}

void ui_context::end_window() { in_window_ = false; }

bool ui_context::hot(std::uint64_t id, rect r) {
    const bool inside = r.x <= mx_ && mx_ < r.x + r.w && r.y <= my_ && my_ < r.y + r.h;
    if (inside && id) hot_candidate_ = id; // last emitted under mouse wins (topmost)
    if (inside && pressed_) held_ = id;    // press captures topmost widget
    return inside;
}

void ui_context::quad(rect r, float u0, float v0, float u1, float v1,
                      float cr, float cg, float cb, float ca, int slot) {
    const std::uint32_t base = static_cast<std::uint32_t>(frame_.verts.size());
    frame_.verts.push_back({float(r.x), float(r.y), u0, v0, cr, cg, cb, ca});
    frame_.verts.push_back({float(r.x + r.w), float(r.y), u1, v0, cr, cg, cb, ca});
    frame_.verts.push_back({float(r.x + r.w), float(r.y + r.h), u1, v1, cr, cg, cb, ca});
    frame_.verts.push_back({float(r.x), float(r.y + r.h), u0, v1, cr, cg, cb, ca});
    frame_.indices.insert(frame_.indices.end(), {base, base + 1, base + 2,
                                                 base, base + 2, base + 3});
    if (!frame_.cmds.empty() && frame_.cmds.back().texture_slot == slot &&
        frame_.cmds.back().clip_x == win_client_.x &&
        frame_.cmds.back().clip_y == win_client_.y &&
        frame_.cmds.back().idx_offset + frame_.cmds.back().idx_count ==
            static_cast<std::uint32_t>(frame_.indices.size()) - 6) {
        frame_.cmds.back().idx_count += 6; // merge run within same slot+clip
        return;
    }
    frame_.cmds.push_back({slot,
                           static_cast<std::uint32_t>(frame_.indices.size()) - 6, 6,
                           win_client_.x, win_client_.y, win_client_.w, win_client_.h});
}

void ui_context::add_rect(rect r, std::uint8_t r8, std::uint8_t g8, std::uint8_t b8) {
    quad(r, 0.5f, 0.5f, 0.5f, 0.5f, c(r8), c(g8), c(b8), 1.f, 0);
}

void ui_context::add_text(std::int32_t x, std::int32_t baseline_y, const char* text,
                          std::uint8_t r8, std::uint8_t g8, std::uint8_t b8) {
    // Layout rule (deterministic, mirrors what tests pin): pen is a float
    // summed from f32 advances; the glyph box origin is int(pen) (truncate,
    // == floor for positive pen) plus the integer bearing.
    float pen = float(x);
    for (const char* p = text; *p; ++p) {
        const auto cp = static_cast<unsigned char>(*p);
        const auto* g = font_->glyph(cp);
        if (!g) continue; // outside cooked range: skipped, no advance (debug font)
        if (g->w > 0 && g->h > 0) {
            const rect q = {static_cast<int>(pen) + g->bearing_x, baseline_y - g->bearing_y, g->w, g->h};
            const float aw = float(font_->atlas_w), ah = float(font_->atlas_h);
            quad(q, (g->x + 0.5f) / aw, (g->y + 0.5f) / ah,
                 (g->x + g->w - 0.5f) / aw, (g->y + g->h - 0.5f) / ah,
                 c(r8), c(g8), c(b8), 1.f, 1);
        }
        pen += g->advance;
    }
}

bool ui_context::button(const char* label, const char* id_salt) {
    const std::uint64_t id = id_salt ? hash_scoped(label, id_salt) : hash_id(label);
    const std::int32_t tw = [&] {
        float w = 0.f;
        for (const char* p = label; *p; ++p)
            if (const auto* g = font_->glyph(static_cast<unsigned char>(*p))) w += g->advance;
        return static_cast<int>(w); // same truncation rule as add_text
    }();
    const rect r = {cur_x_, cur_y_, tw + 2 * kPad, kRowH};
    last_rect_ = r;
    const bool hov = hot(id, r);
    if (hov && pressed_) held_ = id;
    const bool clicked = released_ && held_ == id && hov;
    const bool act = held_ == id && down_;
    add_rect(r, act ? 124 : hov ? 92 : 70, act ? 138 : hov ? 102 : 78,
             act ? 162 : hov ? 122 : 95);
    const int text_h = font_->ascent - font_->descent;
    add_text(r.x + kPad, r.y + (kRowH - text_h) / 2 + font_->ascent, label, 232, 234, 240);
    cur_y_ += kRowH + kSpacing;
    return clicked;
}

bool ui_context::checkbox(const char* label, bool* value) {
    const std::uint64_t id = hash_id(label);
    const rect box = {cur_x_, cur_y_ + (kRowH - kBox) / 2, kBox, kBox};
    last_rect_ = box;
    const rect row = {cur_x_, cur_y_, 200, kRowH}; // row hit covers box+label
    const bool hov = hot(id, row);
    const bool clicked = released_ && held_ == id && hov;
    if (clicked) *value = !*value;
    add_rect(box, 24, 27, 35);
    if (*value) add_rect({box.x + 3, box.y + 3, kBox - 6, kBox - 6}, 110, 200, 130);
    const int text_h = font_->ascent - font_->descent;
    add_text(box.x + kBox + 6, box.y + (kBox - text_h) / 2 + font_->ascent, label,
             232, 234, 240);
    cur_y_ += kRowH + kSpacing;
    return clicked;
}

void ui_context::label(const char* text) {
    const int text_h = font_->ascent - font_->descent;
    add_text(cur_x_, cur_y_ + font_->ascent, text, 232, 234, 240);
    cur_y_ += text_h + kSpacing;
}

bool ui_context::text_field(const char* label, char* buf, std::size_t cap) {
    const std::uint64_t id = hash_id(label);
    const int text_h = font_->ascent - font_->descent;
    add_text(cur_x_, cur_y_ + font_->ascent, label, 232, 234, 240);
    cur_y_ += text_h + 2;
    const rect r = {cur_x_, cur_y_, win_client_.w - 2 * kPad, kRowH};
    last_rect_ = r;
    const bool hov = hot(id, r);
    if (hov && pressed_) focus_ = id;
    if (focus_ == id) {
        std::size_t len = std::strlen(buf);
        for (const char ch : io_->chars) {
            if (!ch) continue;
            if (len + 1 < cap) { buf[len++] = ch; buf[len] = '\0'; }
        }
        if (io_->backspace && len > 0) buf[--len] = '\0';
    }
    add_rect(r, 24, 27, 35);
    add_text(r.x + 4, r.y + (kRowH - text_h) / 2 + font_->ascent, buf, 232, 234, 240);
    cur_y_ += kRowH + kSpacing;
    const bool committed = focus_ == id && io_->enter;
    if (committed) focus_ = 0;
    return committed;
}

void ui_context::end_frame() {
    hot_ = hot_candidate_;
    // Press-capture ends only AFTER every widget got its chance to observe
    // released_ && held_ == id (click); zeroing it in begin_frame would make
    // clicks unreachable.
    if (released_) held_ = 0;
}

} // namespace ve::gfx
