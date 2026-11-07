// test: texture_roundtrip — PNG -> cooklet TEX0 unit -> runtime loader
// readback; pixels must match the source exactly. Also pins the doctrine:
// the loader path (ve_gfx + ve_core) contains no decoder — it is byte copy.
#include <doctest/doctest.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION // test-side PNG authoring (approved dep)
#include <stb_image_write.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../ve_test_portability.h" // getpid

#include <ve/cook/cook.h>
#include <ve/gfx/texture.h>

namespace {
std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_tex_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
} // namespace

TEST_CASE("texture_roundtrip") {
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");

    // 3x2 RGBA gradient, written as a real PNG (cook-side tooling).
    const int W = 3, H = 2;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const auto i = static_cast<std::size_t>((y * W + x) * 4);
            rgba[i + 0] = static_cast<std::uint8_t>(x * 80 + 7);
            rgba[i + 1] = static_cast<std::uint8_t>(y * 120 + 13);
            rgba[i + 2] = static_cast<std::uint8_t>(x * 40 + y * 60 + 99);
            rgba[i + 3] = 255;
        }
    }
    const std::string png = (std::filesystem::path(src) / "tex.png").string();
    REQUIRE(stbi_write_png(png.c_str(), W, H, 4, rgba.data(), W * 4) != 0);

    auto res = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(res.ok, res.error);
    REQUIRE(res.counts.cooked == 1);

    // cooked unit is TEX0 (decoded), not RAWD passthrough.
    ve::cook::Unit u;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/tex.png.ck", u, &err));
    CHECK(u.type == ve::cook::kUnitTex0);

    // runtime loader readback: pixels identical to the authored image.
    ve::gfx::CpuTexture tex;
    REQUIRE_MESSAGE(ve::gfx::load_cooked_texture_file(out + "/units/tex.png.ck", tex, &err), err);
    CHECK(tex.width == 3);
    CHECK(tex.height == 2);
    CHECK(tex.format == ve::core::cooked::kTexFmtRgba8);
    REQUIRE(tex.pixels.size() == rgba.size());
    CHECK(tex.pixels == rgba);

    // corrupted unit bytes must be rejected, not loaded.
    {
        ve::cook::Bytes b;
        REQUIRE(ve::core::cooked::read_file_bytes(out + "/units/tex.png.ck", b, &err));
        b[b.size() - 5] ^= 0xFF; // last payload byte (crc trailer follows) -> crc must catch it
        ve::core::cooked::write_file_bytes(out + "/units/bad.ck", b, &err);
        ve::gfx::CpuTexture bad;
        CHECK_FALSE(ve::gfx::load_cooked_texture_file(out + "/units/bad.ck", bad, &err));
    }

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}
