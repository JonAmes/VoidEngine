#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>

#include "gfx_test_env.h"

#include <cstddef>
#include <cstdint>

TEST_CASE("gfx_offscreen_target") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 presenter test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    auto pres = ve::gfx::offscreen_presenter::create(*dev, 64, 64);
    REQUIRE(pres.has_value());
    CHECK(pres->color_view() != VK_NULL_HANDLE);
    CHECK(pres->width() == 64u);
    CHECK(pres->height() == 64u);
    CHECK(pres->color_format() == VK_FORMAT_B8G8R8A8_UNORM);

    // Exact-value pins: UNORM8 of 1.0 and 0.0 convert to exactly 255/0, so a
    // full-frame clear is a byte-exact golden — no tolerance needed here
    // (tolerance is for 2.6's rasterized edges, not clears).
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pres->clear(red));

    const auto px = pres->capture_rgba8();
    REQUIRE(px.size() == 64u * 64u * 4u);
    // B8G8R8A8 component order: byte0=B, byte1=G, byte2=R, byte3=A.
    CHECK(px[0] == 0u);
    CHECK(px[1] == 0u);
    CHECK(px[2] == 255u);
    CHECK(px[3] == 255u);

    // Corners must match too — a wrong row pitch or partial copy would show
    // up at the last pixel while pixel 0 still looked right.
    const std::size_t last = (64u * 64u - 1u) * 4u;
    CHECK(px[last + 0] == 0u);
    CHECK(px[last + 1] == 0u);
    CHECK(px[last + 2] == 255u);
    CHECK(px[last + 3] == 255u);

    // Second clear starts from TRANSFER_SRC (the bookkept post-capture
    // layout), not UNDEFINED — layout tracking, not guessing. Validation
    // stays silent through the whole chain (it is enabled on this instance).
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    CHECK(pres->clear(green));
    const auto px2 = pres->capture_rgba8();
    REQUIRE(px2.size() == px.size());
    CHECK(px2[0] == 0u);
    CHECK(px2[1] == 255u);
    CHECK(px2[2] == 0u);
    CHECK(px2[3] == 255u);
}
