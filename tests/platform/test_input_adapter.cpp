// EK.2 (#54d): the GLFW->ui_io mapping must be testable with ZERO GLFW and no
// display — feed the adapter's raw-event vocabulary directly and pin the
// per-frame gfx::ui_io it produces.
#include <ve/platform/input_adapter.h>

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

std::string to_str(const std::vector<char>& v) { return std::string(v.begin(), v.end()); }

} // namespace

TEST_CASE("input_adapter") {
    ve::platform::input_adapter ad;

    // Idle frame: everything neutral, nothing pending.
    {
        auto io = ad.next_frame();
        CHECK(io.mouse_x == 0.f);
        CHECK(io.mouse_y == 0.f);
        CHECK(!io.mouse_down);
        CHECK(io.chars.empty());
        CHECK(!io.backspace);
        CHECK(!io.enter);
        CHECK(!io.escape);
    }

    // HiDPI: cursor px arrive in CONTENT space, ui_io must carry FRAMEBUFFER
    // px (2x here). Scale set on resize; cursor events after it scale.
    ad.set_frame_scale(800, 600, 1600, 1200);
    ad.cursor(100.0, 50.0);
    {
        auto io = ad.next_frame();
        CHECK(io.mouse_x == 200.f);
        CHECK(io.mouse_y == 100.f);
    }
    // Minimized content 0x0 keeps the last scale (no divide-by-zero collapse).
    ad.set_frame_scale(0, 0, 0, 0);
    ad.cursor(10.0, 20.0);
    {
        auto io = ad.next_frame();
        CHECK(io.mouse_x == 20.f);
        CHECK(io.mouse_y == 40.f);
    }

    // Button edges are STATEFUL across frames (press held -> held); only the
    // left button drives ui_io::mouse_down.
    ad.mouse(ve::platform::input_adapter::button::left, true);
    ad.mouse(ve::platform::input_adapter::button::right, true);
    CHECK(ad.next_frame().mouse_down);
    ad.mouse(ve::platform::input_adapter::button::left, false);
    CHECK_FALSE(ad.next_frame().mouse_down); // release lands next frame
    ad.mouse(ve::platform::input_adapter::button::right, false); // no effect

    // Chars: utf-8 encoded, order preserved, DRAINED after one frame.
    ad.character('A');
    ad.character(0xE9);    // e-acute -> C3 A9
    ad.character(0x1F600); // grinning face -> F0 9F 98 80
    ad.character(0xD800);  // lone surrogate -> dropped
    ad.character(0x110000); // > max cp -> dropped
    {
        auto io = ad.next_frame();
        CHECK(to_str(io.chars) == std::string("A\xC3\xA9\xF0\x9F\x98\x80"));
    }
    CHECK(ad.next_frame().chars.empty()); // drained

    // Editing keys are EDGE-latched: one press -> exactly one frame of true.
    ad.press(ve::platform::input_adapter::key::backspace);
    ad.press(ve::platform::input_adapter::key::enter);
    ad.press(ve::platform::input_adapter::key::escape);
    {
        auto io = ad.next_frame();
        CHECK(io.backspace);
        CHECK(io.enter);
        CHECK(io.escape);
    }
    {
        auto io = ad.next_frame();
        CHECK(!io.backspace);
        CHECK(!io.enter);
        CHECK(!io.escape);
    }
}
