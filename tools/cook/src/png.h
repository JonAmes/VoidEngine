#pragma once
#include <string>

#include <ve/cook/cook.h>

// PNG decode (stb, cooklet-side only — the runtime never decodes PNG).
// Decodes to RGBA8 regardless of source color type; w*h*4 bytes out.
namespace ve::cook {
bool decode_png_file(const std::string& path, int& w_out, int& h_out,
                     Bytes& rgba_out, std::string* err);
} // namespace ve::cook
