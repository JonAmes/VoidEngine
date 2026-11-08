#pragma once
// Shared FONT0 payload parser for tests (layout in ve/core/cooked.h).
// tests/tools cooks the font; tests/gfx consumes the same bytes at runtime —
// both sides decode through this one header so pins can't drift from loads.
#include <cstdint>
#include <cstring>
#include <vector>

#include <ve/core/cooked.h>

namespace ve::test {
struct Font0Glyph {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    std::int32_t bearing_x = 0, bearing_y = 0;
    float advance = 0.f;
};
struct Font0 {
    std::uint32_t px_size = 0, atlas_w = 0, atlas_h = 0;
    std::int32_t ascent = 0, descent = 0;
    std::uint32_t first_cp = 0;
    std::vector<Font0Glyph> glyphs;
    ve::core::cooked::Bytes atlas; // atlas_w*atlas_h*4 RGBA
};

inline bool parse_font0_payload(const ve::core::cooked::Bytes& p, Font0& f) {
    std::size_t off = 0;
    auto u32 = [&](std::uint32_t& v) {
        if (off + 4 > p.size()) return false;
        std::memcpy(&v, p.data() + off, 4);
        off += 4;
        return true;
    };
    auto i32 = [&](std::int32_t& v) {
        std::uint32_t u;
        if (!u32(u)) return false;
        v = static_cast<std::int32_t>(u);
        return true;
    };
    bool bad = false;
    bad |= !u32(f.px_size);
    bad |= !u32(f.atlas_w);
    bad |= !u32(f.atlas_h);
    bad |= !i32(f.ascent);
    bad |= !i32(f.descent);
    bad |= !u32(f.first_cp);
    std::uint32_t count = 0;
    bad |= !u32(count);
    if (bad || count == 0) return false;
    f.glyphs.resize(count);
    for (auto& g : f.glyphs) {
        bad |= !i32(g.x); bad |= !i32(g.y); bad |= !i32(g.w); bad |= !i32(g.h);
        bad |= !i32(g.bearing_x); bad |= !i32(g.bearing_y);
        std::uint32_t bits = 0;
        bad |= !u32(bits);
        std::memcpy(&g.advance, &bits, 4);
    }
    if (bad) return false;
    const std::size_t bytes = static_cast<std::size_t>(f.atlas_w) * f.atlas_h * 4;
    if (off + bytes != p.size()) return false; // exact payload size guard
    f.atlas.assign(p.begin() + static_cast<std::ptrdiff_t>(off), p.end());
    return true;
}
} // namespace ve::test
