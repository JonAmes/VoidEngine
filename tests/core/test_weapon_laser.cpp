// Phase 9.7 — instant-hit laser raycast vs oriented component bounds.
// Everything hand-computed closed-form (no reference implementation used).
#include <cmath>
#include <vector>

#include "ve/core/math/quat.h"
#include "ve/core/sim/weapon.h"

#include <doctest/doctest.h>

using namespace ve::core;
using ve::core::sim::comp_bounds;
using ve::core::sim::laser_hit;
using ve::core::sim::laser_raycast;
using ve::core::sim::obb;

namespace {
comp_bounds box2d(std::uint32_t id, math::dvec3 c, math::dvec3 h, math::dquat q = {}) {
    return comp_bounds{id, obb{c, h, q}};
}
} // namespace

TEST_CASE("weapon_laser_hit") {
    const math::dvec3 origin{0.0, 0.0, 0.0};
    const math::dvec3 fwd{0.0, 0.0, -1.0};

    // 1) Nearest wins regardless of container order.
    //    Cube h=5 at z=-100: entry face z=-95 -> dist 95. Second cube at
    //    z=-200 (entry dist 195) is scanned FIRST and must still lose.
    {
        std::vector<comp_bounds> targets = {
            box2d(1, {0.0, 0.0, -200.0}, {5.0, 5.0, 5.0}),
            box2d(0, {0.0, 0.0, -100.0}, {5.0, 5.0, 5.0}),
        };
        auto hit = laser_raycast(origin, fwd, 300.0, targets);
        REQUIRE(hit.has_value());
        CHECK(hit->component == 0u);
        CHECK(hit->dist == 95.0);  // bit-exact: dyadic slabs, no libm
        CHECK(hit->pos.x == 0.0);
        CHECK(hit->pos.z == -95.0);
        CHECK(hit->normal.z == 1.0);  // back at shooter
        CHECK(hit->normal.x == 0.0);
    }

    // 2) Near miss at a bounds GAP: two h=5 cubes at x=-2 and x=+12 (faces
    //    at x=3 and x=7); ray down x=5.0 passes the 4 m gap -> no hit.
    {
        std::vector<comp_bounds> targets = {
            box2d(0, {-2.0, 0.0, -100.0}, {5.0, 5.0, 5.0}),
            box2d(1, {12.0, 0.0, -100.0}, {5.0, 5.0, 5.0}),
        };
        auto hit = laser_raycast({5.0, 0.0, 0.0}, fwd, 300.0, targets);
        CHECK_FALSE(hit.has_value());
    }

    // 3) Oriented bounds: half-extents (10,5,5) yawed 45 deg about Y.
    //    Box-local ray frame (conjugate yaw): x0 = (70.7107, 0, 70.7107),
    //    v = (0.7071, 0, -0.7071). Slab entries: x -> (70.7107-10)/0.7071 =
    //    85.86; z -> (70.7107-5)/0.7071 = 92.93 = LAST entry. Exits:
    //    x -> 114.1, z -> 107.1 -> window open -> hit on the local +z face
    //    at dist = (100*sin45 - 5)/cos45 = 100 - 5*sqrt(2).
    //    World normal = yaw45 * (0,0,1) = (sin45, 0, cos45).
    {
        const double kPi = 3.14159265358979323846;
        const math::dquat q = math::quat_yaw(kPi / 4.0);
        std::vector<comp_bounds> targets = {
            box2d(7, {0.0, 0.0, -100.0}, {10.0, 5.0, 5.0}, q),
        };
        auto hit = laser_raycast(origin, fwd, 300.0, targets);
        REQUIRE(hit.has_value());
        CHECK(hit->component == 7u);
        const double want_d = 100.0 - 5.0 * std::sqrt(2.0);  // 92.9289...
        CHECK(hit->dist == doctest::Approx(want_d).epsilon(1e-12));
        const double s = std::sqrt(0.5);
        CHECK(hit->normal.x == doctest::Approx(s).epsilon(1e-12));
        CHECK(hit->normal.z == doctest::Approx(s).epsilon(1e-12));
    }

    // 4) Range is authored (weapon.range): same geometry, range 50 -> miss;
    //    range exactly to the face (95) -> hit at the boundary (t0 > range
    //    is the reject, equality is inside).
    {
        std::vector<comp_bounds> targets = {box2d(0, {0.0, 0.0, -100.0}, {5.0, 5.0, 5.0})};
        CHECK_FALSE(laser_raycast(origin, fwd, 50.0, targets).has_value());
        auto edge = laser_raycast(origin, fwd, 95.0, targets);
        REQUIRE(edge.has_value());
        CHECK(edge->dist == 95.0);
    }

    // 5) Degenerate inputs refuse (core is exception-free): non-unit dir,
    //    negative/NaN range, NaN origin.
    {
        std::vector<comp_bounds> targets = {box2d(0, {0.0, 0.0, -100.0}, {5.0, 5.0, 5.0})};
        CHECK_FALSE(laser_raycast(origin, {0.0, 0.0, -2.0}, 300.0, targets).has_value());
        CHECK_FALSE(laser_raycast(origin, fwd, -1.0, targets).has_value());
        CHECK_FALSE(laser_raycast(origin, fwd, std::nan(""), targets).has_value());
        CHECK_FALSE(laser_raycast({std::nan(""), 0.0, 0.0}, fwd, 300.0, targets).has_value());
        CHECK_FALSE(laser_raycast(origin, fwd, 300.0, {}).has_value());
    }

    // 6) Start inside a candidate -> hit at dist 0, first candidate in
    //    container order wins the exact-zero tie (ordered-container rule).
    {
        std::vector<comp_bounds> targets = {
            box2d(3, {0.0, 0.0, -10.0}, {5.0, 5.0, 5.0}),
            box2d(4, {0.0, 0.0, -10.0}, {8.0, 8.0, 8.0}),
        };
        auto hit = laser_raycast({0.0, 0.0, -12.0}, fwd, 100.0, targets);
        REQUIRE(hit.has_value());
        CHECK(hit->component == 3u);
        CHECK(hit->dist == 0.0);
    }
}
