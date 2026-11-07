#include <doctest/doctest.h>
#include <ve/gfx/device.h>
#include <ve/gfx/instance.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/wsi_presenter.h>
#include <ve/platform/window.h>

#include "gfx_test_env.h"

#include <cstdint>
#include <expected>
#include <string>
#include <type_traits>

// 2.8's CI contract: "compile-gate in CI, never runs in CI". A window needs a
// desktop seat, and per decision #36 the behavior lands in 2.9 under
// Xvfb+lavapipe. What this case CAN pin, on any box, is that the whole WSI path
// compiles, links, and keeps its promises at the seams:
//   1. the swapchain machinery sits behind the 2.4 presenter interface;
//   2. the frame-loop entry points 2.9/2.10 will call exist with these shapes;
//   3. ve_platform links against GLFW (taking window::create's address drags
//      window.cpp.obj — and with it the GLFW import — into this binary, without
//      ever opening a window);
//   4. a surfaceless device is refused by the WSI presenter instead of quietly
//      inventing a target.
TEST_CASE("gfx_wsi_compile_gate") {
    // (1) The seam itself. If present() ever starts taking a swapchain image
    // from upstream, this stops being true and the boundary is broken.
    static_assert(std::is_base_of_v<ve::gfx::presenter, ve::gfx::wsi_presenter>,
                  "the WSI presenter must stay a ve::gfx::presenter (AGENTS boundary 3)");
    static_assert(std::is_final_v<ve::gfx::wsi_presenter>,
                  "wsi_presenter is a leaf implementation; XR gets its own presenter");

    // (2) Frame-loop shapes. Named explicitly so a signature drift is a compile
    // error HERE rather than a surprise inside ve_demo in 2.9.
    auto acquire = &ve::gfx::wsi_presenter::acquire;
    auto begin_command = &ve::gfx::wsi_presenter::begin_command;
    auto begin_pass = &ve::gfx::wsi_presenter::begin_pass;
    auto end_pass = &ve::gfx::wsi_presenter::end_pass;
    auto present = &ve::gfx::wsi_presenter::present;
    auto recreate = &ve::gfx::wsi_presenter::recreate;
    auto needs_recreate = &ve::gfx::wsi_presenter::needs_recreate;
    auto capture = &ve::gfx::wsi_presenter::capture_rgba8;
    static_assert(std::is_same_v<decltype(acquire),
                                 std::expected<std::uint32_t, std::string> (ve::gfx::wsi_presenter::*)()>);
    static_assert(std::is_same_v<decltype(present), bool (ve::gfx::wsi_presenter::*)(VkCommandBuffer)>);
    static_assert(std::is_same_v<decltype(recreate), bool (ve::gfx::wsi_presenter::*)(std::uint32_t, std::uint32_t)>);
    static_assert(std::is_same_v<decltype(needs_recreate), bool (ve::gfx::wsi_presenter::*)() const>);
    // No member here hands a VkImage or VkImageView of a swapchain image outward.
    static_assert(std::is_same_v<decltype(capture), std::vector<std::uint8_t> (ve::gfx::wsi_presenter::*)()>);
    CHECK(acquire != nullptr);
    CHECK(begin_command != nullptr);
    CHECK(begin_pass != nullptr);
    CHECK(end_pass != nullptr);
    CHECK(present != nullptr);
    CHECK(recreate != nullptr);
    CHECK(capture != nullptr);

    // (3) The platform seam links. Deliberately NOT called: a real window is 2.9's
    // job, and ctest stays display-free. The address is enough to make the linker
    // resolve window.cpp and everything GLFW pulls in behind it.
    auto make_window = &ve::platform::window::create;
    auto close_requested = &ve::platform::window::should_close;
    auto frame_size = &ve::platform::window::frame_size;
    auto make_surface = &ve::platform::window::create_surface;
    CHECK(make_window != nullptr);
    CHECK(close_requested != nullptr);
    CHECK(frame_size != nullptr);
    CHECK(make_surface != nullptr);

    // (4) Host-side contract, exercised for real on the Tier-2 stack.
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — WSI presenter contract checks skipped");
        return;
    }
    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst); // surfaceless on purpose: the default path
    REQUIRE(dev.has_value());

    // A surfaceless device has no present story at all — and nothing about the
    // 2.2-2.7 path changed because 2.8 exists.
    CHECK_FALSE(dev->present_capable());
    CHECK(dev->present_queue() == VK_NULL_HANDLE);
    CHECK(dev->present_queue_family() == VK_QUEUE_FAMILY_IGNORED);

    // Refuses a surfaceless device rather than inventing a target.
    auto no_surface = ve::gfx::wsi_presenter::create(*dev, VK_NULL_HANDLE, 64, 64);
    REQUIRE_FALSE(no_surface.has_value());
    CHECK(no_surface.error().find("VkSurfaceKHR") != std::string::npos);

    // Zero extent is rejected before anything touches the surface. The handle
    // below is a fake, and that is only safe because create() returns on the
    // extent check first — never widen this case to a non-zero extent.
    auto zero_extent = ve::gfx::wsi_presenter::create(
        *dev, reinterpret_cast<VkSurfaceKHR>(std::uintptr_t{1}), 0, 0);
    REQUIRE_FALSE(zero_extent.has_value());
    CHECK(zero_extent.error().find("non-zero extent") != std::string::npos);

    // A device built without a surface also has no present queue, so the WSI
    // presenter can never be handed one by accident through that device: the
    // extent guard fires first. (Deliberately not exercised with a *valid*
    // surface and valid extent here — that needs 2.9's window.)
    auto zero_second = ve::gfx::wsi_presenter::create(
        *dev, reinterpret_cast<VkSurfaceKHR>(std::uintptr_t{1}), 64, 0);
    REQUIRE_FALSE(zero_second.has_value());
    CHECK(zero_second.error().find("non-zero extent") != std::string::npos);

    // Tier-2 validation still silent: nothing above reached the driver, and the
    // strict messenger (2.7) is the witness.
    gfx_test::check_clean_validation(*inst);
}
