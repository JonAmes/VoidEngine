// test: cook_font_atlas — .ttf -> FONT0 unit (3.12). Two cooks of the same
// font are byte-identical (cook determinism contract); metric pins are
// hand-checked against DejaVu Sans Mono's metrics (see checks below); a
// non-TTF .ttf is a clean cook failure, not a crash.
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ve/cook/cook.h>
#include "../font_layout.h" // shared FONT0 payload parser (tests + gfx text test)

#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_font_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

ve::test::Font0 parse_font0(const ve::core::cooked::Unit& u) {
    ve::test::Font0 f;
    REQUIRE(ve::test::parse_font0_payload(u.payload, f));
    return f;
}
} // namespace

TEST_CASE("cook_font_atlas") {
    const auto font_src =
        std::filesystem::path(VE_SOURCE_DIR) / "assets/fonts/DejaVuSansMono.ttf";
    REQUIRE(std::filesystem::exists(font_src));

    // Cook twice into separate outputs: the .ck bytes must match exactly.
    const auto src = tmp_root("src");
    std::filesystem::copy_file(font_src, std::filesystem::path(src) / "DejaVuSansMono.ttf");
    const auto out1 = tmp_root("out1");
    const auto out2 = tmp_root("out2");

    ve::cook::CookResult r1 = ve::cook::cook_incremental(src, out1);
    CHECK(r1.ok);
    CHECK(r1.counts.cooked == 1);
    ve::cook::CookResult r2 = ve::cook::cook_incremental(src, out2);
    CHECK(r2.counts.failures == 0);

    const auto unit_path = std::filesystem::path(out1) / "units/DejaVuSansMono.ttf.ck";
    REQUIRE(std::filesystem::exists(unit_path));
    CHECK(read_bytes(unit_path) == read_bytes(std::filesystem::path(out2) / "units/DejaVuSansMono.ttf.ck"));

    ve::core::cooked::Unit u;
    std::string err;
    REQUIRE(ve::core::cooked::read_unit_file(unit_path.string(), u, &err));
    CHECK(u.type == ve::core::cooked::kUnitFont0);

    // ---- header ----
    ve::test::Font0 f = parse_font0(u);
    CHECK(f.px_size == 16);
    CHECK(f.atlas_w == 512);
    CHECK(f.first_cp == 32);
    CHECK(f.glyphs.size() == 95); // 32..126
    CHECK(f.atlas.size() == static_cast<std::size_t>(f.atlas_w) * f.atlas_h * 4);

    // ---- hand-checked metric pins (DejaVu Sans Mono, 16 px em-mapped height)
    // fontBBox-ascender=1901, descender=-483 (unitsPerEm 2048); stb maps pixel
    // height to ascent-descent=2384 -> scale=16/2384=0.0067114.
    // ascent = 1901*scale = 12.76 -> 13, descent = -483*scale = -3.24 -> -3.
    CHECK(f.ascent == 13);
    CHECK(f.descent == -3);

    const auto& sp = f.glyphs[0];      // ' ' (cp 32)
    const auto& ex = f.glyphs[1];      // '!' (cp 33)
    const auto& a = f.glyphs['A' - 32];// 'A' (cp 65)
    const auto& i = f.glyphs['i' - 32];// 'i' (cp 105)

    // space: no bitmap, but the advance is the em advance.
    CHECK(sp.w == 0);
    CHECK(sp.h == 0);
    CHECK(sp.advance == doctest::Approx(8.2752f).epsilon(1e-3));
    CHECK(ex.advance == doctest::Approx(8.2752f).epsilon(1e-3));

    // monospace: every printable cp shares one advance (hand-checked 1233 uem).
    for (std::size_t g = 1; g < f.glyphs.size(); ++g)
        CHECK(f.glyphs[g].advance == f.glyphs[1].advance);

    // 'A' cap box: 9x11 px, flush left, top 11 px above baseline (stb box
    // (0,-11,9,0)); 'i': 7x11, 1 px sidebearing — exact integer pins.
    CHECK(a.w == 9);
    CHECK(a.h == 11);
    CHECK(a.bearing_x == 0);
    CHECK(a.bearing_y == 11);
    CHECK(i.w == 7);
    CHECK(i.h == 11);
    CHECK(i.bearing_x == 1);
    CHECK(a.advance == doctest::Approx(8.2752f).epsilon(1e-3));

    // 'A' atlas rect carries coverage (grayscale mask: R=G=B=A).
    std::size_t lit = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const std::size_t d = (static_cast<std::size_t>(a.y) + y) * f.atlas_w + a.x + x;
            const std::uint8_t v = f.atlas[d * 4];
            if (v > 0) ++lit;
            CHECK(f.atlas[d * 4] == f.atlas[d * 4 + 1]);
            CHECK(f.atlas[d * 4 + 1] == f.atlas[d * 4 + 2]);
            CHECK(f.atlas[d * 4 + 2] == f.atlas[d * 4 + 3]);
        }
    CHECK(lit > 40); // 'A' strokes cover a good part of the 9x11 box
    // padding around the rect is zero (shelf packer, 1px gutters)
    CHECK(f.atlas[(static_cast<std::size_t>(a.y) - 1) * f.atlas_w * 4 + (a.x - 1) * 4 + 3] == 0);
}

TEST_CASE("cook_font_bad_ttf") {
    const auto src = tmp_root("bad");
    {
        std::ofstream f(std::filesystem::path(src) / "junk.ttf", std::ios::binary | std::ios::trunc);
        f << "not a font at all";
    }
    ve::cook::CookResult r = ve::cook::cook_incremental(src, tmp_root("bad_out"));
    CHECK(r.counts.failures == 1);
    CHECK(r.error.find("font") != std::string::npos);
}
