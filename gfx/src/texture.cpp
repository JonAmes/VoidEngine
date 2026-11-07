#include "ve/gfx/texture.h"

#include <ve/core/cooked.h>

namespace ve::gfx {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;

namespace {
std::uint32_t get_u32(const Bytes& b, std::size_t& p, bool& bad) {
    if (p + 4 > b.size()) { bad = true; return 0; }
    std::uint32_t v = b[p] | (std::uint32_t(b[p + 1]) << 8) |
                      (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
    p += 4;
    return v;
}
} // namespace

bool load_cooked_texture_file(const std::string& unit_path, CpuTexture& out,
                              std::string* err) {
    Unit unit;
    if (!ve::core::cooked::read_unit_file(unit_path, unit, err)) return false;
    if (unit.type != ve::core::cooked::kUnitTex0) {
        if (err) *err = "not a TEX0 unit: " + unit_path;
        return false;
    }
    bool bad = false;
    std::size_t p = 0;
    out.width = get_u32(unit.payload, p, bad);
    out.height = get_u32(unit.payload, p, bad);
    out.format = get_u32(unit.payload, p, bad);
    if (bad || out.format != ve::core::cooked::kTexFmtRgba8) {
        if (err) *err = "bad TEX0 header: " + unit_path;
        return false;
    }
    // overflow-safe expected size (fp64 avoided: integer bound check)
    const std::uint64_t want =
        std::uint64_t(out.width) * out.height * 4;
    if (want != unit.payload.size() - p) {
        if (err) *err = "TEX0 payload size mismatch: " + unit_path;
        return false;
    }
    out.pixels.assign(unit.payload.begin() + static_cast<std::ptrdiff_t>(p),
                      unit.payload.end());
    return true;
}

} // namespace ve::gfx
