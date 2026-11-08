// PNG writing for ve_demo's evidence frames. stb_image_write is third-party and
// COOKLET/TOOL-side only — the same scoped-suppression pattern tools/cook uses
// for stb_image_read (docs/CHEATS.md: the warning wall governs our code).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <ve/demo/png_out.h>

#include <vector>

namespace ve::demo {

bool write_png_rgba8(const char* path, std::uint32_t width, std::uint32_t height,
                     const std::uint8_t* src, bool swap_rb) {
    if (path == nullptr || src == nullptr || width == 0u || height == 0u) {
        return false;
    }
    std::vector<std::uint8_t> rgba;
    const std::uint8_t* pixels = src;
    if (swap_rb) {
        // PNG has no "BGRA" type: the R/B swap has to happen before stb sees it,
        // otherwise every evidence frame comes back with its channels inverted.
        rgba.resize(static_cast<std::size_t>(width) * height * 4u);
        for (std::size_t i = 0; i < rgba.size(); i += 4u) {
            rgba[i + 0] = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i + 0];
            rgba[i + 3] = src[i + 3];
        }
        pixels = rgba.data();
    }
    const int stride = static_cast<int>(width * 4u);
    return stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 4, pixels,
                          stride) != 0;
}

} // namespace ve::demo
