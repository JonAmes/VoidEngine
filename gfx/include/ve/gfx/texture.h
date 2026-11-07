#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Cooked-texture readback (decision #13): the runtime loads cooked TEX0
// units only — PNG never reaches runtime code, and no decoder is linked
// here. This is pure bounds-checked byte reading.
namespace ve::gfx {

struct CpuTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0; // ve::core::cooked::kTexFmtRgba8
    std::vector<std::uint8_t> pixels; // top-down rows, width*height*4
};

// Reads a VECK unit (type TEX0) from disk and copies its payload.
// Returns false (no exceptions; gfx may not throw) on bad magic/type/size.
bool load_cooked_texture_file(const std::string& unit_path, CpuTexture& out,
                              std::string* err);

} // namespace ve::gfx
