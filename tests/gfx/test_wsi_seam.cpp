// 2.8 gate: the WSI presenter and the GLFW window seam must COMPILE and
// LINK in CI, but no surface/swapchain is ever created here — ctest has no
// display (AGENTS contract; Tier 2 stays lavapipe-surfaceless). Actual WSI
// behavior lands in 2.9 (Xvfb + lavapipe capture evidence, decision #36).
#include <doctest/doctest.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/wsi_presenter.h>
#include <ve/platform/window.h>

#include "gfx_test_env.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

TEST_CASE("gfx_wsi_seam") {
    // Compile+link gate: the type must exist, derive from the presenter
    // seam, and expose the frame API with the offscreen call shape (a pass
    // written for one presenter must compile against the other — 2.4).
    static_assert(std::is_base_of_v<ve::gfx::presenter, ve::gfx::wsi_presenter>);
    static_assert(std::is_final_v<ve::gfx::wsi_presenter>);
    static_assert(std::is_move_constructible_v<ve::gfx::wsi_presenter>);
    static_assert(std::is_move_assignable_v<ve::gfx::wsi_presenter>);
    const auto touch = []<typename T>(const T&) { static_assert(sizeof(T) > 0u); };
    touch(&ve::gfx::wsi_presenter::create);
    touch(&ve::gfx::wsi_presenter::present);
    touch(&ve::gfx::wsi_presenter::recreate);
    touch(&ve::gfx::wsi_presenter::surface_lost);
    touch(&ve::platform::window::create_surface);

    // The extras plumbing on instance::create, provable HEADLESS:
    // VK_KHR_surface needs no window system to enable at instance level
    // (lavapipe + the loader both advertise it surfaceless). This is the
    // exact extension class GLFW will add on a real display.
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — WSI-extras instance check skipped");
        return;
    }
    const std::vector<std::string> extras{"VK_KHR_surface"};
    auto inst = ve::gfx::instance::create("ve-wsi-seam", extras);
    REQUIRE_MESSAGE(inst.has_value(), inst.error());

    // volkLoadInstance must have resolved the instance-level WSI query —
    // the first function wsi_presenter::create calls. Null here means the
    // extension never made it into the instance (extras ignored = bug).
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR != nullptr);

    gfx_test::check_clean_validation(*inst);

    // GLFW's own list must contain VK_KHR_surface wherever a window system
    // exists. On headless CI glfwInit can't talk to any display, and GLFW
    // documents returning NULL there — empty is legal, skipping is honest.
    const auto glfw_exts = ve::platform::window::required_instance_extensions();
    if (glfw_exts.empty()) {
        MESSAGE("GLFW has no instance extensions on this headless box (expected in CI)");
    } else {
        CHECK(std::find(glfw_exts.begin(), glfw_exts.end(), "VK_KHR_surface") != glfw_exts.end());
    }
}
