// test: terrain_stamps — the #29c authored stamp stack (flatten/crater/mesa:
// center dir, angular radius, level, blend) applied BEFORE noise:
//   h = h_base + (1 - W) * noise.  Full-weight interiors pin h to the
// authored level with EXACT bits and zero noise influence — which makes the
// pad guarantee (surface normal ∥ local well normal, #29c) an exact proof on
// the inner disk, not a tolerance. Also pins: mid-blend values (quintic(0.5)
// = 0.5 exactly; dd from acos jitters ~ulp, so those pins ride 1e-12 eps), stack order, hard-disc blend=0,
// degenerate-stamp skipping, and stamps riding the 7.1 spin frame (#41).

#include <cmath>
#include <span>

#include <doctest/doctest.h>

#include "ve/core/math/base.h"
#include "ve/core/math/quat.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/terrain.h"

using namespace ve::core;
using sim::body_def;
using sim::rail_world;
using sim::stamp_type;
using sim::terrain_def;
using sim::terrain_stamp;

namespace {
terrain_def def() {
    terrain_def d{};
    d.seed = 7;
    d.relief_m = 80.0;
    d.base_frequency = 2.0;
    d.octaves = 3;
    d.lacunarity = 2.0;
    d.gain = 0.5;
    return d;
}

constexpr double kR = 1.0e6;  // mean surface radius (m)
constexpr double kPadRadius = 0.01;  // rad (~10 km at R)
constexpr double kPadLevel = 50.0;   // m

terrain_stamp pad() {
    return terrain_stamp{stamp_type::flatten, math::dvec3(0, 1, 0), kPadRadius, kPadLevel, 0.25};
}

// direction at angular distance `dd` rad from +Y, in the XY plane.
math::dvec3 from_y(double dd) { return math::dvec3(std::sin(dd), std::cos(dd), 0.0); }

constexpr std::span<const terrain_stamp> one(const terrain_stamp& s) { return std::span<const terrain_stamp>(&s, 1); }
} // namespace

TEST_CASE("terrain_stamps") {
    const terrain_def d = def();

    // ---- pad interior: EXACT level, EXACT radial normal, noise-proof ----
    {
        const auto p = pad(); const auto stamps = one(p);
        CHECK(sim::terrain_height(d, stamps, math::dvec3(0, 1, 0)) == kPadLevel);
        // interior points strictly inside (1-blend)*radius = 0.0075 rad:
        for (const double dd : {0.0005, 0.0031, 0.0073}) {
            const auto dir = from_y(dd);
            CHECK(sim::terrain_height(d, stamps, dir) == kPadLevel);
            const auto n = sim::terrain_normal(d, kR, stamps, dir);
            // h is bit-constant on the inner disk => tangential differences
            // vanish exactly => normal is radial up to normalize rounding.
            CHECK(math::approximately_equal(n, math::normalize(dir), 1e-12));
            CHECK(math::approximately_equal(math::dot(n, math::normalize(dir)), 1.0, 1e-12));
        }
        // noise influence is structurally zero, not "small": different seeds
        // give the same exact level at the pad.
        terrain_def other = d; other.seed = 99;
        CHECK(sim::terrain_height(other, stamps, math::dvec3(0, 1, 0)) == kPadLevel);
    }

    // ---- outside the pad: bit-identical to the unstamped sampler ----
    {
        const auto p = pad(); const auto stamps = one(p);
        const auto far = from_y(0.05);
        CHECK(sim::terrain_height(d, stamps, far) == sim::terrain_height(d, far));
        CHECK(sim::terrain_normal(d, kR, stamps, far) == sim::terrain_normal(d, kR, far));
        // empty stamp span is the same path, bit-identical
        CHECK(sim::terrain_height(d, std::span<const terrain_stamp>{}, far) == sim::terrain_height(d, far));
    }

    // ---- blend ring: neither level nor raw; rim converges to raw; ring tilts the normal ----
    {
        const auto p = pad(); const auto stamps = one(p);
        const auto ring = from_y(0.0076);  // just past inner edge (inner = 0.0075)
        const double h_ring = sim::terrain_height(d, stamps, ring);
        CHECK(h_ring != kPadLevel);
        CHECK(h_ring != sim::terrain_height(d, ring));
        CHECK(h_ring > kPadLevel - 1.0);  // still near level this early in the ring
        const auto rim = from_y(0.00999);  // blend ~ complete
        CHECK(std::fabs(sim::terrain_height(d, stamps, rim) - sim::terrain_height(d, rim)) < 0.01);
        // the blend wall is real relief: sample the steepest point of the
        // ring (u=0.5: quintic' peaks, zero at the ring edges) — normal tilts
        const auto steep = from_y(0.00875);  // inner + (radius-inner)/2
        const auto n = sim::terrain_normal(d, kR, stamps, steep);
        CHECK(math::dot(n, math::normalize(steep)) < 0.999);
    }

    // ---- mid-blend pins: quintic(0.5) == 0.5 exactly in binary64; dd from
    // acos carries ulp-jitter in u, so the pin rides epsilon(1e-12) ----
    {
        // crater point-profile at half radius: w = 0.5 exactly
        const auto crater = terrain_stamp{stamp_type::crater, math::dvec3(0, 1, 0), 0.01, -40.0, 0.0};
        const auto stamps = one(crater);
        CHECK(sim::terrain_height(d, stamps, math::dvec3(0, 1, 0)) == -40.0);  // floor, w = 1
        const auto mid = from_y(0.005);  // u = dd/r ~ 0.5 -> w ~ 0.5; acos(dd)
        const double raw = sim::terrain_height(d, mid);  // carries ~ulp, not exact
        CHECK(sim::terrain_height(d, stamps, mid) == doctest::Approx(-20.0 + 0.5 * raw).epsilon(1e-12));
        // blend=1 plateau at half radius: same w
        const auto wide = terrain_stamp{stamp_type::mesa, math::dvec3(0, 1, 0), 0.01, 60.0, 1.0};
        CHECK(sim::terrain_height(d, one(wide), mid) == doctest::Approx(30.0 + 0.5 * raw).epsilon(1e-12));
    }

    // ---- mesa stands at its level; center pin exact ----
    CHECK(sim::terrain_height(d, one(terrain_stamp{stamp_type::mesa, math::dvec3(0, 1, 0), 0.02, 60.0, 0.25}),
                          math::dvec3(0, 1, 0)) == 60.0);

    // ---- stack order matters (#29c "stamp stack") ----
    {
        const auto p = pad();
        const auto cr = terrain_stamp{stamp_type::crater, math::dvec3(0, 1, 0), 0.002, -40.0, 0.5};
        const terrain_stamp pad_then_crater[2] = {p, cr};
        const terrain_stamp crater_then_pad[2] = {cr, p};
        CHECK(sim::terrain_height(d, std::span(pad_then_crater), math::dvec3(0, 1, 0)) == -40.0);
        CHECK(sim::terrain_height(d, std::span(crater_then_pad), math::dvec3(0, 1, 0)) == kPadLevel);
    }

    // ---- blend = 0 hard disc: full weight right up to the rim, hard cut ----
    {
        const auto hard = terrain_stamp{stamp_type::flatten, math::dvec3(0, 1, 0), 0.01, kPadLevel, 0.0};
        const auto stamps = one(hard);
        CHECK(sim::terrain_height(d, stamps, from_y(0.00999)) == kPadLevel);
        CHECK(sim::terrain_height(d, stamps, from_y(0.0101)) == sim::terrain_height(d, from_y(0.0101)));
    }

    // ---- degenerate stamps are skipped, never contaminate ----
    {
        const auto dir = from_y(0.004);
        const terrain_stamp zero_r{stamp_type::flatten, math::dvec3(0, 1, 0), 0.0, 50.0, 0.25};
        const terrain_stamp nan_level{stamp_type::flatten, math::dvec3(0, 1, 0), 0.01, std::nan(""), 0.25};
        const terrain_stamp zero_c{stamp_type::flatten, math::dvec3(0, 0, 0), 0.01, 50.0, 0.25};
        const terrain_stamp nan_c{stamp_type::flatten, math::dvec3(std::nan(""), 1, 0), 0.01, 50.0, 0.25};
        for (const auto& bad : {zero_r, nan_level, zero_c, nan_c}) {
            CHECK(sim::terrain_height(d, one(bad), dir) == sim::terrain_height(d, dir));
            CHECK(sim::terrain_normal(d, kR, one(bad), dir) == sim::terrain_normal(d, kR, dir));
        }
    }

    // ---- purity: stamped evals are bit-stable ----
    {
        const auto p = pad(); const auto stamps = one(p);
        const auto dir = from_y(0.006);
        CHECK(sim::terrain_height(d, stamps, dir) == sim::terrain_height(d, stamps, dir));
    }

    // ---- #41: stamps ride the spin frame. Pad is authored at body-local
    // +Y; the body spins about +X, so the pad center MOVES in world space.
    {
        rail_world world;
        body_def b{};
        b.surface_radius = kR;
        b.parent = -1;
        b.spin_axis = math::dvec3(1, 0, 0);
        b.spin_rate_rad_s = 0.35;
        const auto id = *world.add_body(b);
        const sim::tick_t t{12345};
        const auto q = *world.orientation(id, t);

        const auto p = pad(); const auto stamps = one(p);
        // world-space view of the pad: the sampler ONLY ever sees local
        // dirs — the caller rotates the world dir into the body frame with
        // the 7.1 orientation, and rotates the sampled normal back out.
        const auto c_world = math::rotate(q, math::dvec3(0, 1, 0));
        const auto c_back = math::rotate(math::conjugate(q), c_world);
        CHECK(math::approximately_equal(c_back, math::dvec3(0, 1, 0), 1e-12));

        // back in the local frame the pad pin is EXACT again (round-trip
        // lands inside the full-weight inner disk, ~1e-8 rad from +Y):
        CHECK(sim::terrain_height(d, stamps, c_back) == kPadLevel);
        // pad normal in WORLD = q * local radial normal: plane ∥ well normal,
        // at the pad's spinning world position (#29c guarantee, #41 motion).
        const auto n_local = sim::terrain_normal(d, kR, stamps, c_back);
        CHECK(math::approximately_equal(n_local, c_back, 1e-12));
        const auto n_world = math::rotate(q, n_local);
        CHECK(math::approximately_equal(n_world, c_world, 1e-12));
        CHECK(math::dot(n_world, c_world) > 0.999999999);
        // the pad center is NOT at its tick-0 world spot (spin really moved it)
        CHECK(math::dot(c_world, math::dvec3(0, 1, 0)) < 0.999);
        static_cast<void>(math::deg_to_rad(1.0));  // base.h in use for the tolerance idiom above
    }
}
