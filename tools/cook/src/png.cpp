// stb_image implementation TU. stb is an approved dependency (PLAN.md) used
// ONLY on the cooklet side; it is never linked into runtime code. This file
// carries scoped -Wno flags (set in CMake; docs/CHEATS.md) — the warning wall
// governs our code, not third-party single headers.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "png.h"

namespace ve::cook {

bool decode_png_file(const std::string& path, int& w_out, int& h_out,
                     Bytes& rgba_out, std::string* err) {
    int w = 0, h = 0;
    // req_comp=4: force RGBA8 so every cooked texture has one pixel layout.
    unsigned char* px = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!px) {
        if (err) *err = std::string("PNG decode failed (") +
                        stbi_failure_reason() + "): " + path;
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    rgba_out.assign(px, px + n);
    stbi_image_free(px);
    w_out = w;
    h_out = h;
    return true;
}

} // namespace ve::cook
