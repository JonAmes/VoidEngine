#pragma once
#include <string>

#include <ve/cook/cook.h>

// .ttf -> FONT0 glyph atlas (3.12). stb_truetype runs here, at cook time;
// the runtime never parses fonts (decision #15: "zero font parsing at
// runtime"). Rasterization is float-stable for a fixed binary/platform pair
// (#40), so the byte-identical re-cook contract holds like every other unit.
namespace ve::cook {

// Fixed cook parameters (the debug-UI look; a second size = a second unit id).
inline constexpr std::uint32_t kFontCookPxSize = 16;
inline constexpr std::uint32_t kFontCookFirstCp = 32;
inline constexpr std::uint32_t kFontCookLastCp = 126;
inline constexpr std::uint32_t kFontAtlasWidth = 512;

// Payload layout is documented in ve/core/cooked.h (FONT0 block).
bool cook_font_unit(const Bytes& ttf, Bytes& payload_out, std::string* err);

} // namespace ve::cook
