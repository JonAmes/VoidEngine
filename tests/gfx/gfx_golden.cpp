// gfx_golden.cpp — golden-PNG harness implementation.
// Write side: this TU owns STB_IMAGE_WRITE_IMPLEMENTATION for engine_tests
// (nothing else in the binary defines it). Read side: stbi_load & friends come
// from ve_cooklet's png.cpp — the project's single STB_IMAGE_IMPLEMENTATION
// owner — which engine_tests already links for the cook tests. Do NOT define
// STB_IMAGE_IMPLEMENTATION here as well: ld reports a wall of "multiple
// definition of stbi_*" (2026-11-08, learned the hard way). If engine_tests
// ever stops linking ve_cooklet, the read calls fail to *link* loudly — move
// the implementation define here then, not before.
#define STB_IMAGE_WRITE_IMPLEMENTATION
// stb's C in a C++ TU under the wall: same scoped suppressions as the other
// stb sites (cook png.cpp, demo png_out.cpp) — TU-scoped, never global.
#include <stb_image.h>
#include <stb_image_write.h>

#include "gfx_golden.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace gfx_golden {

std::vector<unsigned char> read_png_rgba8(const std::string& path, std::string& err) {
    int w = 0, h = 0, ch = 0;
    // 4 = force RGBA8 regardless of the PNG's own channel count/palette.
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        err = "cannot read PNG '" + path + "': " + stbi_failure_reason();
        return {};
    }
    std::vector<unsigned char> out(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return out;
}

bool write_png_rgba8(const std::string& path, const unsigned char* rgba, int w, int h,
                     std::string& err) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    if (stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) == 0) {
        err = "cannot write PNG '" + path + "'";
        return false;
    }
    return true;
}

verdict compare_png_rgba8(const unsigned char* rgba, int w, int h,
                          const std::string& golden_png, const policy& pol) {
    verdict v;
    // Load once; stbi reports the true dimensions at decode, so no second
    // stbi_info probe (which would also be a TOCTOU double-read).
    int gw = 0, gh = 0, ch = 0;
    unsigned char* px = stbi_load(golden_png.c_str(), &gw, &gh, &ch, 4);
    if (!px) {
        v.message = pol.label + ": cannot read PNG '" + golden_png + "': " +
                    stbi_failure_reason() +
                    " — regenerate with VE_GOLDEN_UPDATE=1 (and commit the new golden)";
        return v;
    }
    const std::vector<unsigned char> gold(px, px + static_cast<std::size_t>(gw) * gh * 4);
    stbi_image_free(px);
    if (gw != w || gh != h) {
        v.golden_w = gw;
        v.golden_h = gh;
        v.message = pol.label + ": size mismatch — captured " + std::to_string(w) + "x" +
                    std::to_string(h) + ", golden '" + golden_png + "' is " +
                    std::to_string(gw) + "x" + std::to_string(gh);
        return v;
    }
    v.golden_w = gw;
    v.golden_h = gh;

    const int npix = w * h;
    for (int i = 0; i < npix; ++i) {
        int delta = 0;
        for (int c = 0; c < 4; ++c) {
            delta = std::max(delta, std::abs(static_cast<int>(rgba[i * 4 + c]) -
                                             static_cast<int>(gold[i * 4 + c])));
        }
        v.max_channel_delta = std::max(v.max_channel_delta, delta);
        if (delta > pol.channel_tolerance) {
            if (v.mismatch_pixels == 0) {
                v.first_bad_x = i % w;
                v.first_bad_y = i / w;
            }
            ++v.mismatch_pixels;
        }
    }
    v.mismatch_pct = 100.0 * static_cast<double>(v.mismatch_pixels) /
                     static_cast<double>(npix);
    // AT the boundary passes: the gates are <=, pinned by gfx_golden_harness.
    v.ok = static_cast<double>(v.mismatch_pixels) * 100.0 <= pol.max_mismatch_pct * npix;
    if (!v.ok) {
        v.message = pol.label + ": " + std::to_string(v.mismatch_pixels) + "/" +
                    std::to_string(npix) + " pixels differ beyond tolerance " +
                    std::to_string(pol.channel_tolerance) + " (" +
                    std::to_string(v.mismatch_pct) + "% > " +
                    std::to_string(pol.max_mismatch_pct) + "%), max channel delta " +
                    std::to_string(v.max_channel_delta) + ", first at (" +
                    std::to_string(v.first_bad_x) + "," + std::to_string(v.first_bad_y) + ")";
    } else {
        v.message = pol.label + ": ok (" + std::to_string(v.mismatch_pixels) +
                    " soft-diff pixels, max channel delta " +
                    std::to_string(v.max_channel_delta) + ")";
    }
    return v;
}

std::string golden_path(const std::string& name) {
    if (const char* dir = std::getenv("VE_GOLDEN_DIR"); dir && *dir) {
        return (std::filesystem::path(dir) / name).string();
    }
#ifdef VE_SOURCE_DIR
    return (std::filesystem::path(VE_SOURCE_DIR) / "tests/gfx/goldens" / name).string();
#else
    return name; // only reachable if someone compiles the harness without the define
#endif
}

bool golden_update_requested() {
    const char* v = std::getenv("VE_GOLDEN_UPDATE");
    return v && *v && std::string(v) != "0";
}

verdict check_golden(const std::string& name, const unsigned char* rgba, int w, int h,
                     const policy& pol) {
    const std::string path = golden_path(name);
    if (golden_update_requested()) {
        verdict v;
        std::string err;
        v.ok = write_png_rgba8(path, rgba, w, h, err);
        v.message = v.ok ? ("golden updated: " + path) : err;
        return v;
    }
    auto v = compare_png_rgba8(rgba, w, h, path, pol);
    if (!v.ok) {
        // Dump the actual image where the developer will actually look:
        // next to the golden, git-untracked, name says it is the *actual*.
        std::string err;
        const std::string actual = path + ".actual.png";
        if (write_png_rgba8(actual, rgba, w, h, err)) {
            v.message += "\n  actual image written to: " + actual;
        }
    }
    return v;
}

} // namespace gfx_golden
