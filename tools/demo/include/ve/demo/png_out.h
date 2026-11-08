#pragma once
#include <cstdint>

namespace ve::demo {

// Evidence images, written tool-side (never runtime — decision #13 is about the
// engine's decode deps, and this TU lives in tools/). `src` is the presenter's
// capture_rgba8() output, i.e. bytes in the swapchain format's component order:
// swap_rb is true for B8G8R8A8_*, false for R8G8B8A8_*.
bool write_png_rgba8(const char* path, std::uint32_t width, std::uint32_t height,
                     const std::uint8_t* src, bool swap_rb);

} // namespace ve::demo
