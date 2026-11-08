#include "ve/gfx/font.h"

#include <ve/core/cooked.h>

#include <cstring>

namespace ve::gfx {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;

namespace {
std::uint32_t get_u32(const Bytes& b, std::size_t& p, bool& bad) {
    if (p + 4 > b.size()) { bad = true; return 0; }
    const std::uint32_t v = b[p] | (std::uint32_t(b[p + 1]) << 8) |
                            (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
    p += 4;
    return v;
}
std::int32_t get_i32(const Bytes& b, std::size_t& p, bool& bad) {
    return static_cast<std::int32_t>(get_u32(b, p, bad));
}
} // namespace

bool load_cooked_font_file(const std::string& unit_path, font0& out, std::string* err) {
    Unit unit;
    if (!ve::core::cooked::read_unit_file(unit_path, unit, err)) return false;
    if (unit.type != ve::core::cooked::kUnitFont0) {
        if (err) *err = "not a FONT0 unit: " + unit_path;
        return false;
    }
    bool bad = false;
    std::size_t p = 0;
    out.px_size = get_u32(unit.payload, p, bad);
    out.atlas_w = get_u32(unit.payload, p, bad);
    out.atlas_h = get_u32(unit.payload, p, bad);
    out.ascent = get_i32(unit.payload, p, bad);
    out.descent = get_i32(unit.payload, p, bad);
    out.first_cp = get_u32(unit.payload, p, bad);
    out.glyph_count = get_u32(unit.payload, p, bad);
    if (bad) {
        if (err) *err = "bad FONT0 header: " + unit_path;
        return false;
    }
    // Sized before reading: the payload itself bounds every read below.
    const std::uint64_t want_glyph_bytes = std::uint64_t(out.glyph_count) * 7 * 4;
    const std::uint64_t want_atlas_bytes = std::uint64_t(out.atlas_w) * out.atlas_h * 4;
    if (p + want_glyph_bytes + want_atlas_bytes != unit.payload.size()) {
        if (err) *err = "FONT0 payload size mismatch: " + unit_path;
        return false;
    }
    out.glyphs.clear();
    out.glyphs.reserve(out.glyph_count);
    for (std::uint32_t i = 0; i < out.glyph_count; ++i) {
        font0_glyph g;
        g.x = get_i32(unit.payload, p, bad);
        g.y = get_i32(unit.payload, p, bad);
        g.w = get_i32(unit.payload, p, bad);
        g.h = get_i32(unit.payload, p, bad);
        g.bearing_x = get_i32(unit.payload, p, bad);
        g.bearing_y = get_i32(unit.payload, p, bad);
        {
            const std::uint32_t bits = get_u32(unit.payload, p, bad);
            std::memcpy(&g.advance, &bits, 4);
        }
        out.glyphs.push_back(g);
    }
    // Atlas must lie inside the unit payload (re-check after glyph reads).
    if (bad || p + want_atlas_bytes > unit.payload.size()) {
        if (err) *err = "FONT0 glyph table truncated: " + unit_path;
        return false;
    }
    out.atlas.assign(unit.payload.begin() + static_cast<std::ptrdiff_t>(p),
                     unit.payload.end());
    return true;
}

} // namespace ve::gfx
