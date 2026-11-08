// FONT0 cook: TTF -> glyph atlas via stb_truetype (3.12). This TU carries the
// stb_truetype implementation; it is third-party C compiled as C++ and the
// warning wall is scoped away for it here (docs/CHEATS.md: same line as
// png.cpp). Everything in OUR code below compiles under the full wall.
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "font.h"

namespace ve::cook {
namespace {

void put32(Bytes& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v & 0xFF));
    o.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    o.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    o.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void puti(Bytes& o, std::int32_t v) { put32(o, static_cast<std::uint32_t>(v)); }
void putf(Bytes& o, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4); // IEEE-754 bit pattern; LE on write
    put32(o, bits);
}

struct Glyph {
    std::int32_t ax = 0, ay = 0; // atlas rect pos
    std::int32_t w = 0, h = 0;   // 0 = blank (space)
    std::int32_t bx = 0, by = 0; // bearing: x0 left of pen; -y0 above baseline
    float advance = 0.f;
    int x0 = 0, y0 = 0; // raster box offsets (per stb, y0 negative = above baseline)
};

// Cheap SFNT guard before stb: the 2021-pinned stb_truetype takes no buffer
// size and trusts the table directory, so a truncated/corrupt .ttf reads OOB
// under ASan. Validate the container ourselves; garbage then fails cleanly
// at cook time (decision: the cooklet must never crash on bad source).
bool validate_sfnt(const Bytes& f, std::string* err) {
    auto be16 = [&](std::size_t o) {
        return (static_cast<std::uint16_t>(f[o]) << 8) | f[o + 1];
    };
    auto be32 = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(f[o]) << 24) | (static_cast<std::uint32_t>(f[o + 1]) << 16) |
               (static_cast<std::uint32_t>(f[o + 2]) << 8) | f[o + 3];
    };
    if (f.size() < 12) {
        if (err) *err = "TTF truncated (no SFNT header)";
        return false;
    }
    const std::uint32_t ver = be32(0);
    if (ver != 0x00010000u && ver != 0x74727565u /*'true'*/) {
        if (err) *err = "not a glyf SFNT container";
        return false;
    }
    const std::size_t num_tables = be16(4);
    if (f.size() < 12 + 16 * num_tables) {
        if (err) *err = "TTF table directory truncated";
        return false;
    }
    for (std::size_t t = 0; t < num_tables; ++t) {
        const std::size_t e = 12 + 16 * t;
        const std::uint32_t off = be32(e + 8), len = be32(e + 12);
        if (len > f.size() || off > f.size() - len) {
            if (err) *err = "TTF table range outside file";
            return false;
        }
    }
    return true;
}

} // namespace

bool cook_font_unit(const Bytes& ttf, Bytes& payload_out, std::string* err) {
    if (!validate_sfnt(ttf, err)) return false;
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf.data(), 0)) {
        if (err) *err = "not a parseable TTF";
        return false;
    }
    const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(kFontCookPxSize));
    if (scale <= 0.f) {
        if (err) *err = "bad font scale";
        return false;
    }
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);

    constexpr std::uint32_t count = kFontCookLastCp - kFontCookFirstCp + 1;
    std::vector<Glyph> glyphs(count);

    // Shelf-pack pass: fixed-width rows, 1px padding. Deterministic: glyphs
    // are placed in codepoint order, rows fill left to right.
    std::int32_t cx = 1, cy = 1, row_h = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto cp = static_cast<int>(kFontCookFirstCp + i);
        Glyph& g = glyphs[i];
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &x0, &y0, &x1, &y1);
        g.x0 = x0;
        g.y0 = y0;
        g.w = x1 - x0;
        g.h = y1 - y0;
        g.bx = x0;
        g.by = -y0;
        int ascii_adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, cp, &ascii_adv, &lsb);
        g.advance = ascii_adv * scale;
        if (g.w > static_cast<std::int32_t>(kFontAtlasWidth) - 2) {
            if (err) *err = "glyph too wide for atlas row (cp " + std::to_string(cp) + ")";
            return false;
        }
        if (cx + g.w + 1 > static_cast<std::int32_t>(kFontAtlasWidth)) {
            cx = 1;
            cy += row_h + 1;
            row_h = 0;
        }
        g.ax = cx;
        g.ay = cy;
        cx += g.w + 1;
        row_h = std::max(row_h, g.h);
    }
    const std::int32_t atlas_h = cy + row_h + 1;

    // Rasterize pass: coverage replicated to RGBA (grayscale mask, no LCD).
    Bytes atlas(static_cast<std::size_t>(kFontAtlasWidth) * atlas_h * 4, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        Glyph& g = glyphs[i];
        if (g.w <= 0 || g.h <= 0) continue;
        int bw = 0, bh = 0, px = 0, py = 0;
        const auto cp = static_cast<int>(kFontCookFirstCp + i);
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &bw, &bh, &px, &py);
        if (!bmp) {
            if (err) *err = "rasterize failed (cp " + std::to_string(cp) + ")";
            return false;
        }
        if (bw != g.w || bh != g.h || px != g.x0 || py != g.y0) {
            stbtt_FreeBitmap(bmp, nullptr);
            if (err) *err = "atlas pack/raster mismatch (cp " + std::to_string(cp) + ")";
            return false;
        }
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const std::size_t d =
                    (static_cast<std::size_t>(g.ay + y) * kFontAtlasWidth + (g.ax + x)) * 4;
                const std::uint8_t v = bmp[static_cast<std::size_t>(y) * bw + x];
                atlas[d] = atlas[d + 1] = atlas[d + 2] = atlas[d + 3] = v;
            }
        stbtt_FreeBitmap(bmp, nullptr);
    }

    // Payload (layout documented in ve/core/cooked.h).
    Bytes p;
    put32(p, kFontCookPxSize);
    put32(p, kFontAtlasWidth);
    put32(p, static_cast<std::uint32_t>(atlas_h));
    puti(p, static_cast<std::int32_t>(std::lround(ascent * scale)));
    puti(p, static_cast<std::int32_t>(std::lround(descent * scale)));
    put32(p, kFontCookFirstCp);
    put32(p, count);
    for (const auto& g : glyphs) {
        puti(p, g.ax); puti(p, g.ay); puti(p, g.w); puti(p, g.h);
        puti(p, g.bx); puti(p, g.by); putf(p, g.advance);
    }
    p.insert(p.end(), atlas.begin(), atlas.end());
    payload_out = std::move(p);
    return true;
}

} // namespace ve::cook
