#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Cooked-font load (3.13): the runtime reads FONT0 units only — the .ttf and
// stb_truetype live cooklet-side, here it is bounds-checked byte reading
// (#15 "zero font parsing at runtime"). Payload layout is documented in
// core/cooked.h next to kUnitFont0.
namespace ve::gfx {

struct font0_glyph {
    std::int32_t x = 0, y = 0, w = 0, h = 0; // atlas rect; w==h==0 = blank
    std::int32_t bearing_x = 0, bearing_y = 0;
    float advance = 0.f; // f32 in the payload (subpixel text layout)
};

struct font0 {
    std::uint32_t px_size = 0;
    std::uint32_t atlas_w = 0, atlas_h = 0;
    std::int32_t ascent = 0, descent = 0;
    std::uint32_t first_cp = 0, glyph_count = 0;
    std::vector<font0_glyph> glyphs;
    std::vector<std::uint8_t> atlas; // atlas_w*atlas_h*4 RGBA, top-down

    // Null on out-of-range cp. Codepoint = first_cp + index.
    const font0_glyph* glyph(std::uint32_t cp) const {
        if (cp < first_cp || cp >= first_cp + glyph_count) return nullptr;
        return &glyphs[cp - first_cp];
    }
};

// Reads a VECK unit (type FONT0). false + err on bad magic/type/size —
// corrupt input is never a crash (no exceptions in gfx).
bool load_cooked_font_file(const std::string& unit_path, font0& out,
                           std::string* err);

} // namespace ve::gfx
