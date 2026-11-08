// Phase 3.2 — camera = rig + eyes. Pure CPU (no GPU fixture needed): pins the
// rig/eye composition, the #34 frame convention through the view, the
// converged off-axis stereo construction, and the done-when (b) clip-space
// shape — a fixation point on the rig axis lands dead-center in BOTH eyes.
// Pins were probe-verified before being written (cpp-build-debug rule: the
// derivation is the suspect, not the matrix).
#include "ve/gfx/camera.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ve::gfx;
using namespace ve::core::math;

TEST_CASE("gfx_camera_rig") {
    // ---- 1. desktop = rig + ONE centered eye: proj is exactly the Phase 1
    // symmetric builder (identical code path, no symmetric shortcut) --------
    camera_rig rig;
    eye_def desktop{};
    desktop.frust = symmetric_frustum(deg_to_rad(60.0f), 2.0f, 0.1f, 100.0f);
    desktop.view = {0, 0, 64, 32};
    CHECK(approximately_equal(desktop.frust.r, 2.0f * std::tan(deg_to_rad(30.0f)), 1e-6f));
    CHECK(rig.add_eye(desktop) == 0);

    auto frames = rig.compute();
    REQUIRE(frames.size() == 1);
    CHECK(approximately_equal(frames[0].view, mat4f{}, 1e-6f)); // identity rig
    CHECK(approximately_equal(frames[0].proj,
                              perspective(deg_to_rad(60.0f), 2.0f, 0.1f, 100.0f), 1e-6f));
    CHECK(frames[0].viewport_.x == 0);
    CHECK(frames[0].viewport_.y == 0);
    CHECK(frames[0].viewport_.width == 64);
    CHECK(frames[0].viewport_.height == 32);

    // ---- 2. rig transform: view maps world -> rig space -------------------
    trsf moved{};
    moved.position = {1.0f, 2.0f, 3.0f};
    rig.set_rig(moved);
    frames = rig.compute();
    const vec3f at_rig = transform_point(frames[0].view, {1.0f, 2.0f, 3.0f});
    CHECK(approximately_equal(at_rig.x, 0.0f, 1e-6f));
    CHECK(approximately_equal(at_rig.y, 0.0f, 1e-6f));
    CHECK(approximately_equal(at_rig.z, 0.0f, 1e-6f));
    const vec3f front = transform_point(frames[0].view, {1.0f, 2.0f, -7.0f}); // 10 ahead (-Z)
    CHECK(approximately_equal(front.x, 0.0f, 1e-6f));
    CHECK(approximately_equal(front.y, 0.0f, 1e-6f));
    CHECK(approximately_equal(front.z, -10.0f, 1e-6f));

    // ---- 3. #34 convention through a yawed rig (quat yaw +90 deg):
    // forward == rig basis -Z == world -X, right == world -Z, up stays +Y ---
    trsf yawed{};
    yawed.rotation = quat_from_euler(90.0f, 0.0f, 0.0f);
    rig.set_rig(yawed);
    frames = rig.compute();
    const vec3f ahead = transform_point(frames[0].view, {-3.0f, 0.0f, 0.0f}); // along rig fwd
    CHECK(approximately_equal(ahead.x, 0.0f, 1e-5f));
    CHECK(approximately_equal(ahead.y, 0.0f, 1e-5f));
    CHECK(approximately_equal(ahead.z, -3.0f, 1e-5f));
    const vec3f rightward = transform_point(frames[0].view, {0.0f, 0.0f, -1.0f}); // rig right
    CHECK(approximately_equal(rightward.x, 1.0f, 1e-5f));
    const vec3f up = transform_point(frames[0].view, {0.0f, 1.0f, 0.0f});
    CHECK(approximately_equal(up.y, 1.0f, 1e-5f));

    // ---- 4. eye offset in rig space (right eye at +ipd/2) -----------------
    rig.set_rig(trsf{});
    eye_def right_eye{};
    right_eye.rig_local.position = {0.032f, 0.0f, 0.0f};
    right_eye.frust = symmetric_frustum(deg_to_rad(60.0f), 1.0f, 1.0f, 10.0f);
    rig.add_eye(right_eye); // index 1
    frames = rig.compute();
    REQUIRE(frames.size() == 2);
    const vec3f under_eye = transform_point(frames[1].view, {0.032f, 0.0f, -1.0f});
    CHECK(approximately_equal(under_eye.x, 0.0f, 1e-6f));
    CHECK(approximately_equal(under_eye.z, -1.0f, 1e-6f));

    // ---- 5. converged off-axis stereo frustums (done-when (b) shape) ------
    const auto stereo = converged_stereo_frustums(deg_to_rad(60.0f), 1.0f, 1.0f, 10.0f,
                                                  0.064f, 2.0f);
    const float t = std::tan(deg_to_rad(30.0f)); // half-fov tangent
    // shift = ipd/(2*focal) = 0.016; right eye tans shift LEFT, left eye RIGHT
    CHECK(approximately_equal(stereo.second.r, t - 0.016f, 1e-6f));
    CHECK(approximately_equal(stereo.second.l, -t - 0.016f, 1e-6f));
    CHECK(approximately_equal(stereo.first.r, t + 0.016f, 1e-6f));
    CHECK(approximately_equal(stereo.first.l, -t + 0.016f, 1e-6f));
    // verticals untouched (no vertical convergence), width unchanged
    CHECK(approximately_equal(stereo.second.t, t, 1e-6f));
    CHECK(approximately_equal(stereo.second.b, -t, 1e-6f));
    CHECK(approximately_equal(stereo.second.r - stereo.second.l, 2.0f * t, 1e-6f));

    // ---- 6. clip pins: fixation point (0,0,-2) dead-center in BOTH eyes ---
    camera_rig head{};
    eye_def le{}, re{};
    le.rig_local.position = {-0.032f, 0.0f, 0.0f};
    le.frust = stereo.first;
    re.rig_local.position = {0.032f, 0.0f, 0.0f};
    re.frust = stereo.second;
    // side-by-side eye viewports on a wide target (Q4 default layout)
    le.view = {0, 0, 64, 64};
    re.view = {64, 0, 64, 64};
    head.add_eye(le);
    head.add_eye(re);
    const auto hf = head.compute();
    REQUIRE(hf.size() == 2);
    for (std::size_t e = 0; e < 2; ++e) {
        const vec3f fixation = transform_point(hf[e].view, {0.0f, 0.0f, -2.0f});
        const vec4f clip = hf[e].proj * vec4f::point(fixation);
        const float ndc_x = clip.x / clip.w;
        const float ndc_z = clip.z / clip.w;
        CHECK_MESSAGE(approximately_equal(ndc_x, 0.0f, 1e-5f), "eye " << e);
        // standard depth n=1 f=10, distance 2: ndc.z = f(d-n)/(d(f-n)) = 5/9
        CHECK_MESSAGE(approximately_equal(ndc_z, 5.0f / 9.0f, 1e-5f), "eye " << e);
    }
    // top plane of the right eye at depth 2 -> Vulkan NDC top is -1
    {
        const vec3f top_pt = transform_point(hf[1].view, {0.0f, 2.0f * t, -2.0f});
        const vec4f clip = hf[1].proj * vec4f::point(top_pt);
        CHECK(approximately_equal(clip.y / clip.w, -1.0f, 1e-5f));
    }

    // ---- 7. eye ORDER is authored order (instance eye index in 3.9) -------
    // proj column-2 x term is (r+l)/(r-l): positive for the converged LEFT
    // eye (tans shifted right), negative for the right eye.
    CHECK(hf[0].proj.m[8] > 0.0f);
    CHECK(hf[1].proj.m[8] < 0.0f);
    CHECK(hf[1].viewport_.x == 64); // viewports ride through untouched

    // ---- 8. degenerate config guards: no convergence => symmetric tans ----
    const auto flat = converged_stereo_frustums(deg_to_rad(60.0f), 1.0f, 1.0f, 10.0f,
                                                0.064f, 0.0f);
    CHECK(flat.first.l == flat.second.l);
    CHECK(approximately_equal(flat.second.r, t, 1e-6f));
}
