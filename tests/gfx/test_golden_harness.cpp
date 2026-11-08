// test: gfx_golden_harness — the 2.11 golden-PNG comparator, pinned with
// SYNTHETIC images only (no GPU): identical passes; a delta inside the
// channel tolerance passes and outside fails; the mismatch-percentage gate
// behaves at its boundary; size mismatch and missing golden fail loudly;
// write->read round-trips byte-exactly (the committed goldens depend on it).
// The same comparator is what gfx_triangle_golden reruns through, so these
// pins are the cross-vendor tolerance policy (#35/#38) made testable.
#include <doctest/doctest.h>

#include "gfx_golden.h"

#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
constexpr int kW = 8, kH = 8;

std::vector<unsigned char> flat(int r, int g, int b, int a = 255) {
    std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
    for (int i = 0; i < kW * kH; ++i) {
        px[static_cast<std::size_t>(i) * 4 + 0] = static_cast<unsigned char>(r);
        px[static_cast<std::size_t>(i) * 4 + 1] = static_cast<unsigned char>(g);
        px[static_cast<std::size_t>(i) * 4 + 2] = static_cast<unsigned char>(b);
        px[static_cast<std::size_t>(i) * 4 + 3] = static_cast<unsigned char>(a);
    }
    return px;
}
} // namespace

TEST_CASE("gfx_golden_harness") {
    const std::string dir = std::filesystem::temp_directory_path().string() +
                            "/ve_golden_" + std::to_string(__LINE__);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto golden = [&](const char* n) { return dir + "/" + n; };

    SUBCASE("write/read round-trip is byte-exact RGBA8") {
        auto px = flat(1, 2, 3, 4);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("rt.png"), px.data(), kW, kH, err));
        auto back = gfx_golden::read_png_rgba8(golden("rt.png"), err);
        REQUIRE(back.size() == px.size());
        CHECK(back == px);
    }

    SUBCASE("identical image passes strictly") {
        auto px = flat(10, 20, 30);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("same.png"), px.data(), kW, kH, err));
        auto v = gfx_golden::compare_png_rgba8(px.data(), kW, kH, golden("same.png"), {});
        CHECK(v.ok);
        CHECK(v.mismatch_pixels == 0);
        CHECK(v.max_channel_delta == 0);
    }

    SUBCASE("channel tolerance gate") {
        auto px = flat(100, 100, 100);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("tol.png"), px.data(), kW, kH, err));
        auto off = px;
        off[3 * 4 + 2] = 105; // pixel (3,0) blue +5

        gfx_golden::policy tight; // default: strict
        auto v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("tol.png"), tight);
        CHECK_FALSE(v.ok);
        CHECK(v.max_channel_delta == 5);
        CHECK(v.mismatch_pixels == 1);
        CHECK(v.first_bad_x == 3); // first mismatch located, for message output
        CHECK(v.first_bad_y == 0);

        gfx_golden::policy loose;
        loose.channel_tolerance = 5; // delta AT tolerance still matches
        v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("tol.png"), loose);
        CHECK(v.ok);
        CHECK(v.mismatch_pixels == 0);

        loose.channel_tolerance = 4;
        v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("tol.png"), loose);
        CHECK_FALSE(v.ok);
        CHECK(v.mismatch_pixels == 1);
    }

    SUBCASE("alpha channel mismatches count") {
        auto px = flat(0, 0, 0, 255);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("alpha.png"), px.data(), kW, kH, err));
        auto off = px;
        off[7 * 4 + 3] = 250;
        gfx_golden::policy tol;
        tol.channel_tolerance = 2; // delta 5 exceeds even the tolerance
        auto v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("alpha.png"), tol);
        CHECK_FALSE(v.ok);
        CHECK(v.mismatch_pixels == 1);
    }

    SUBCASE("mismatch-percentage gate at its boundary") {
        auto px = flat(0, 0, 0);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("pct.png"), px.data(), kW, kH, err));
        auto off = px;
        // 4 of 64 pixels differ hard: exactly 6.25%.
        for (int i = 0; i < 4; ++i) off[static_cast<std::size_t>(i) * 4] = 255;

        gfx_golden::policy pol;
        pol.channel_tolerance = 8;
        pol.max_mismatch_pct = 6.25; // AT the boundary passes
        auto v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("pct.png"), pol);
        CHECK(v.ok);
        CHECK(v.mismatch_pixels == 4);
        CHECK(v.mismatch_pct == doctest::Approx(6.25));

        pol.max_mismatch_pct = 6.2; // one notch under fails
        v = gfx_golden::compare_png_rgba8(off.data(), kW, kH, golden("pct.png"), pol);
        CHECK_FALSE(v.ok);
    }

    SUBCASE("size mismatch fails with dimensions in the message") {
        auto px = flat(0, 0, 0);
        std::string err;
        REQUIRE(gfx_golden::write_png_rgba8(golden("size.png"), px.data(), kW, kH, err));
        auto v = gfx_golden::compare_png_rgba8(px.data(), kW + 1, kH, golden("size.png"), {});
        CHECK_FALSE(v.ok);
        const bool named_dims = v.message.find("8x8") != std::string::npos;
        CHECK_MESSAGE(named_dims, v.message);
    }

    SUBCASE("missing golden fails and says how to regenerate") {
        auto px = flat(0, 0, 0);
        auto v = gfx_golden::compare_png_rgba8(px.data(), kW, kH, golden("nope.png"), {});
        CHECK_FALSE(v.ok);
        CHECK_MESSAGE(v.message.find("VE_GOLDEN_UPDATE") != std::string::npos, v.message);
    }

    SUBCASE("unreadable PNG is an error, not a silent pass") {
        const std::string junk = golden("junk.png");
        {
            std::FILE* f = std::fopen(junk.c_str(), "wb");
            std::fwrite("not a png", 1, 9, f);
            std::fclose(f);
        }
        auto px = flat(0, 0, 0);
        auto v = gfx_golden::compare_png_rgba8(px.data(), kW, kH, junk, {});
        CHECK_FALSE(v.ok);
    }

    std::filesystem::remove_all(dir);
}
