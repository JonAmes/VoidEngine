#include <cmath>
#include <limits>
#include <utility>

#include <doctest/doctest.h>

#include "ve/core/math/projection.h"

using namespace ve::core::math;

namespace {
struct ndc3 { float x, y, z; };
// project a VIEW-space point (w = -z_view for the perspective builders)
ndc3 to_ndc(const mat4f& P, vec3f v) {
    const vec3f p = transform_point(P, v);   // applies the w divide
    return {p.x, p.y, p.z};
}
} // namespace

TEST_CASE("proj_symmetric") {
    // fov_y = 90 deg -> t = tan(45) = 1; aspect 2 -> r = 2. n=1, f=100.
    const mat4f P = perspective(kHalfPi_f, 2.f, 1.f, 100.f);

    auto c = to_ndc(P, vec3f(0, 0, -1));
    CHECK(approximately_equal(c.x, 0.f, 1e-6f));
    CHECK(approximately_equal(c.y, 0.f, 1e-6f));
    CHECK(approximately_equal(c.z, 0.f, 1e-6f));          // Vulkan near -> 0

    // top edge of near quad: y = t*n = 1 -> NDC y = -1 (Vulkan NDC y is DOWN)
    const auto top = to_ndc(P, vec3f(0, 1.f, -1.f));
    CHECK(approximately_equal(top.y, -1.f, 1e-6f));
    // left edge x = -r*n = -2 -> NDC x = -1
    const auto left = to_ndc(P, vec3f(-2.f, 0, -1.f));
    CHECK(approximately_equal(left.x, -1.f, 1e-6f));
    // far quad: same tans at z=-100 -> x = +/-200, y = +/-100
    const auto fcorner = to_ndc(P, vec3f(200.f, -100.f, -100.f));
    CHECK(approximately_equal(fcorner.x, 1.f, 1e-5f));
    CHECK(approximately_equal(fcorner.y, 1.f, 1e-5f));
    CHECK(approximately_equal(fcorner.z, 1.f, 1e-5f));    // Vulkan far -> 1

    // z curve hand-derived: clip.z = -f z/(f-n) - fn/(f-n), w = d = 50
    // -> ndc.z = f(d-n) / (d(f-n)) = 100*49/(50*99) = 0.989899
    CHECK(approximately_equal(to_ndc(P, vec3f(0, 0, -50.f)).z, 100.f * 49.f / (50.f * 99.f), 1e-6f));

    // reversed-Z: near -> 1, far -> 0 (depth precision concentrates near the
    // camera, which is what VR wants)
    const mat4f PR = perspective(kHalfPi_f, 2.f, 1.f, 100.f, depth_range::reversed_1_to_0);
    CHECK(approximately_equal(to_ndc(PR, vec3f(0, 0, -1.f)).z, 1.f, 1e-6f));
    CHECK(approximately_equal(to_ndc(PR, vec3f(0, 0, -100.f)).z, 0.f, 1e-6f));
    CHECK(to_ndc(PR, vec3f(0, 0, -5.f)).z > to_ndc(PR, vec3f(0, 0, -50.f)).z);

    // reversed + infinite far: ndc.z = n/distance exactly, never 0 at finite
    // depth, monotonic toward 0 (no far plane -> no pop)
    const mat4f PI = perspective(kHalfPi_f, 2.f, 1.f, std::numeric_limits<float>::infinity(),
                                 depth_range::reversed_inf_far);
    CHECK(approximately_equal(to_ndc(PI, vec3f(0, 0, -1.f)).z, 1.f, 1e-6f));
    CHECK(approximately_equal(to_ndc(PI, vec3f(0, 0, -10.f)).z, 0.1f, 1e-6f));
    CHECK(to_ndc(PI, vec3f(0.f, 0.f, -1e7f)).z > 0.f);
}

TEST_CASE("proj_ortho") {
    const mat4f P = ortho(-4.f, 4.f, -2.f, 2.f, 0.5f, 50.f);
    auto tl = to_ndc(P, vec3f(-4.f, 2.f, -0.5f));
    CHECK(approximately_equal(tl.x, -1.f, 1e-6f));
    CHECK(approximately_equal(tl.y, -1.f, 1e-6f));   // top -> -1 (Vulkan y-down)
    CHECK(approximately_equal(tl.z, 0.f, 1e-6f));    // near -> 0
    auto br = to_ndc(P, vec3f(4.f, -2.f, -50.f));
    CHECK(approximately_equal(br.x, 1.f, 1e-6f));
    CHECK(approximately_equal(br.y, 1.f, 1e-6f));
    CHECK(approximately_equal(br.z, 1.f, 1e-6f));
    // w stays 1 (no perspective divide surprises for UI passes)
    const vec4f h = {0.3f, 0.4f, -3.f, 1.f};
    const vec4f clip = P * h;
    CHECK(approximately_equal(clip.w, 1.f, 1e-6f));
}

TEST_CASE("proj_offaxis") {
    // asymmetric per-eye frustum (the HMD fitting case); tangents:
    // l=-0.9, r=0.3, b=-0.2, t=0.7, n=1, f=100
    const mat4f P = perspective_offaxis(1.f, 100.f, -0.9f, 0.3f, -0.2f, 0.7f);
    // near quad corners -> full NDC square (hand-derived with A=2/(r-l) etc.)
    for (auto [x_t, want_x] : {std::pair{-0.9f, -1.f}, {0.3f, 1.f}}) {
        const auto c = to_ndc(P, vec3f(x_t, 0.f, -1.f));
        CHECK(approximately_equal(c.x, want_x, 1e-6f));
    }
    for (auto [y_t, want_y] : {std::pair{0.7f, -1.f}, {-0.2f, 1.f}}) {
        const auto c = to_ndc(P, vec3f(0.f, y_t, -1.f));
        CHECK(approximately_equal(c.y, want_y, 1e-6f));
    }
    // same tangent rays at the FAR plane must also hit the square (proves the
    // no-n-factor form, the bug a length-based off-axis matrix gets wrong)
    for (auto [x_t, want_x] : {std::pair{-0.9f, -1.f}, {0.3f, 1.f}}) {
        const auto c = to_ndc(P, vec3f(x_t * 100.f, 0.f, -100.f));
        CHECK(approximately_equal(c.x, want_x, 1e-5f));
    }
    // asymmetry pin: the VIEW AXIS (x=0, not the angle mid-ray) maps to
    // ndc.x = -B, B = (r+l)/(r-l) = -0.6/1.2 = -0.5  ->  +0.5 — an off-axis
    // eye renders the view-axis RIGHT of screen center. The angle mid-ray
    // ((l+r)/2) trivially maps to 0 (the map is affine).
    CHECK(approximately_equal(to_ndc(P, vec3f(0.f, 0.f, -1.f)).x, 0.5f, 1e-6f));
    // symmetric call must produce a byte-identical matrix to explicit
    // symmetric off-axis args (single code path guarantee)
    const auto sym = perspective(kHalfPi_f, 2.f, 1.f, 100.f);
    const auto man = perspective_offaxis(1.f, 100.f, -2.f, 2.f, -1.f, 1.f);
    for (int i = 0; i < 16; ++i) CHECK(sym.m[i] == man.m[i]);
}

// test: logzoom_projection — 8.10 math half. The logarithmic depth mapping
// spans surface metres -> AU in ONE pass (n=1 m, f=2^38 m ~ 2.75e11 m,
// 38 octaves); the matrix half stays exactly the 1.6 off-axis builder so the
// VR asymmetric per-eye shape (#43 AU-drag dependency) is untouched.
TEST_CASE("logzoom_projection") {
    // 2^38 = 274877906944.0 exact in double AND float; 38-octave span.
    const double fspan = 274877906944.0; // = 2^38 exactly
    const float ff = 274877906944.0f;

    SUBCASE("mapping pins (reversed orientation: near -> 1, far -> 0)") {
        CHECK(logzoom_ndc_z(1.0, 1.0, fspan) == 1.0);
        CHECK(logzoom_ndc_z(fspan, 1.0, fspan) == 0.0);
        // geometric mean of near/far maps to the exact middle of the budget
        CHECK(logzoom_ndc_z(std::sqrt(fspan), 1.0, fspan) == 0.5);
        // Octave linearity — every octave costs exactly 1/38 of the [0,1]
        // budget, i.e. CONSTANT RELATIVE resolution from metres to AU.
        for (int e = 0; e < 37; ++e) {
            const double d = std::pow(2.0, e);
            const double step = logzoom_ndc_z(d, 1.0, fspan) - logzoom_ndc_z(2.0 * d, 1.0, fspan);
            CHECK(std::abs(step - 1.0 / 38.0) < 1e-12);
        }
        // fp32 depth buckets (D32 attachment): a relative nudge of 1e-4
        // changes the stored float depth at EVERY octave — the whole span is
        // representable, not just the near end.
        for (int e = 0; e < 38; ++e) {
            const double d = std::pow(2.0, e);
            const float a = static_cast<float>(logzoom_ndc_z(d, 1.0, fspan));
            const float b = static_cast<float>(logzoom_ndc_z(d * (1.0 + 1e-4), 1.0, fspan));
            CHECK_MESSAGE(b < a, "octave " << e);
        }
    }

    SUBCASE("shader refinement params") {
        const auto p = logzoom_params(1.0, fspan);
        CHECK(p.far_log2 == 38.0f);
        CHECK(p.inv_log2_span == doctest::Approx(1.0f / 38.0f).epsilon(1e-7));
    }

    SUBCASE("matrix half = off-axis + reversed_inf_far z row, untouched VR shape") {
        // Hand-authored HMD-style ASYMMETRIC tangents (a converged per-eye
        // frustum shape): log-zoom must not perturb x/y at any depth.
        const float l = -0.75f, r = 0.45f, b = -0.4f, t = 0.6f;
        const auto P = perspective_logzoom(1.0f, ff, l, r, b, t);
        const auto ref = perspective_offaxis(1.0f, ff, l, r, b, t, depth_range::reversed_inf_far);
        for (int i = 0; i < 16; ++i) CHECK(P.m[i] == ref.m[i]);

        // hardware fallback z = hyperbolic n/d (correctly ordered pre-refine)
        CHECK(to_ndc(P, vec3f(0.f, 0.f, -2.f)).z == doctest::Approx(0.5).epsilon(1e-6));
        CHECK(to_ndc(P, vec3f(0.f, 0.f, -100000000000.f)).z == doctest::Approx(1e-11).epsilon(1e-6));

        // off-axis frustum edges pierce BOTH planes at ±1 (1.6 pin carried):
        const float ds[2] = {1.f, ff};
        for (const float d : ds) {
            CHECK(approximately_equal(to_ndc(P, vec3f(l * d, 0.f, -d)).x, -1.f, 1e-6f));
            CHECK(approximately_equal(to_ndc(P, vec3f(r * d, 0.f, -d)).x, 1.f, 1e-6f));
            CHECK(approximately_equal(to_ndc(P, vec3f(0.f, b * d, -d)).y, 1.f, 1e-6f));
            CHECK(approximately_equal(to_ndc(P, vec3f(0.f, t * d, -d)).y, -1.f, 1e-6f));
        }
    }
}
