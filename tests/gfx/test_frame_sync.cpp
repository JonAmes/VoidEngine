// test: gfx_frame_sync — two frames in flight (checklist 2.10).
// The frame-sync machinery (fence-per-slot, round-robin rotation) is a
// presenter-independent class: the same object serves the offscreen path here
// and the WSI presenter at the same call sites. Pins:
//   1. frame_sync rotation/wait/reset semantics stand alone (no GPU work).
//   2. a 10-frame loop through the offscreen presenter: slots alternate, every
//      readback stays exactly the frame that produced it — including one
//      overlap stretch where frame k+1 is submitted while k's fence is still
//      unwaited (the actual "two in flight" case).
//   3. teardown under scope-exit is clean; the ASan build + strict messenger
//      (no in-flight work at fence/pool/queue destroy) are the real gate.
#include <doctest/doctest.h>
#include <ve/gfx/frame_sync.h>
#include <ve/gfx/presenter.h>

#include "gfx_test_env.h"

#include <cstdint>
#include <vector>

namespace {

struct RenderClear {
    // Pipeline-free clear via VK1.3 dynamic rendering: exercises the same
    // RT layout chain and submit path as a real pass, minus shader machinery.
    void operator()(ve::gfx::offscreen_presenter& pres, VkCommandBuffer cmd, const float rgba[4]) {
        REQUIRE(pres.begin_pass(cmd));
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = pres.color_view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = rgba[0];
        att.clearValue.color.float32[1] = rgba[1];
        att.clearValue.color.float32[2] = rgba[2];
        att.clearValue.color.float32[3] = rgba[3];
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {pres.width(), pres.height()}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
        REQUIRE(pres.end_pass(cmd));
    }
};

// Distinct per-frame colors from exactly-representable UNORM values only
// (0.0 -> 0, 1.0 -> 255 — the 2.4 pin), so readback comparison is byte-exact.
constexpr float kFrameColors[10][4] = {
    {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {1, 0, 1, 1},
    {0, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {0, 1, 0, 0},
};

} // namespace

TEST_CASE("gfx_frame_sync") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — skipping (decision #35)");
        return;
    }
    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    // --- 1. the machinery itself, presenter-independent.
    {
        auto fs = ve::gfx::frame_sync::create(dev->handle(), 3);
        REQUIRE(fs.has_value());
        CHECK(fs->frame_count() == 3);
        CHECK(fs->current() == 0);
        CHECK(fs->fence() != VK_NULL_HANDLE);
        // Fences are created SIGNALED: the first frames of a session have no
        // previous work in their slot, and wait_current must return
        // immediately, never hang.
        CHECK(fs->wait_current());
        CHECK(fs->advance() == 1);
        CHECK(fs->advance() == 2);
        CHECK(fs->advance() == 0); // round-robin
        CHECK(fs->wait_all());
        // Re-arm semantics: after reset the fence is genuinely unsignaled
        // (the next submit must signal it before any wait returns). Probed
        // via status, not wait — an unsubmitted reset fence would block.
        CHECK(fs->reset_current());
        CHECK(vkGetFenceStatus(dev->handle(), fs->fence()) == VK_NOT_READY);

        // Move-assign must release the destination's fences and take on the
        // source's identity. recreate()'s create_sync does exactly this, and
        // an earlier operator= ran the destructor then assigned — ASan's
        // double-free surfaced ONLY in the demo's resize path, so the unit
        // test pins it too.
        auto fs2 = ve::gfx::frame_sync::create(dev->handle(), 2);
        REQUIRE(fs2.has_value());
        *fs = std::move(*fs2); // fs's three fences die here
        CHECK(fs->frame_count() == 2);
        CHECK(fs->current() == 0);
        CHECK(fs->wait_current()); // the NEW slot, not a dead handle
        CHECK(fs2->wait_all());    // moved-from: empty no-op success
    }

    // --- 2. the same machinery driving the offscreen presenter.
    {
        auto pres = ve::gfx::offscreen_presenter::create(*dev, 64, 64);
        REQUIRE(pres.has_value());
        CHECK(pres->frames_in_flight() == 2);

        RenderClear render_clear;
        std::vector<std::uint32_t> slot_seen;
        for (int f = 0; f < 10; ++f) {
            VkCommandBuffer cmd = pres->begin_command();
            REQUIRE(cmd != VK_NULL_HANDLE);
            slot_seen.push_back(pres->current_frame());
            render_clear(*pres, cmd, kFrameColors[f]);
            REQUIRE(pres->submit(cmd));

            // The overlap stretch: frames 4 and 5 are both submitted before
            // anyone waits — two fences genuinely in flight at once.
            if (f != 4) {
                const auto px = pres->capture_rgba8();
                REQUIRE(px.size() == 64u * 64u * 4u);
                // capture_rgba8 hands back raw B8G8R8A8 memory: bytes are
                // B,G,R,A (the 2.4 format pin), not the kFrameColors order.
                const unsigned char want[4] = {
                    static_cast<unsigned char>(kFrameColors[f][2] * 255.0f),
                    static_cast<unsigned char>(kFrameColors[f][1] * 255.0f),
                    static_cast<unsigned char>(kFrameColors[f][0] * 255.0f),
                    static_cast<unsigned char>(kFrameColors[f][3] * 255.0f),
                };
                // Every pixel: the readback shows exactly THIS frame. The
                // f==5 capture below is the post-overlap case: frame 4's fence
                // was never waited before frame 5 went out, so the RT holds
                // frame 5's work and the drain-then-read must agree.
                bool all_exact = true;
                for (std::size_t i = 0; i < px.size(); i += 4) {
                    if (px[i] != want[0] || px[i + 1] != want[1] ||
                        px[i + 2] != want[2] || px[i + 3] != want[3]) {
                        all_exact = false;
                        break;
                    }
                }
                CHECK_MESSAGE(all_exact, "frame ", f, " readback not the exact clear color");
            }
        }
        // Slots strictly alternate 0,1,0,1,... — fences cycle.
        for (std::size_t f = 0; f < slot_seen.size(); ++f) {
            CHECK(slot_seen[f] == static_cast<std::uint32_t>(f % 2));
        }
        // Scope exit = teardown: fences waited, pool+cmd buffers freed, queue
        // idle — any in-flight leak shows as a validation problem or ASan hit.
    }

    gfx_test::check_clean_validation(*inst);
}
