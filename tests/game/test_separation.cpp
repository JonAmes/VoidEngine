// Phase 9 task 9.23 — ONE separation law for every ship (#27b).
// lane_separation_deflect: player-side autopilot ship and NPC ship on
// crossing lanes; both driven by drive-toward-waypoint + THE SAME
// separation_accel call; pinned min-distance band (deflected, but not
// flung) and bit-identical replay hash.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <span>
#include <vector>

#include "ve/core/fnv.h"
#include "ve/game/separation.h"

namespace {

using ve::core::math::dvec3;
using ve::core::math::length;
using ve::game::hazard;
using ve::game::separation_accel;
using ve::game::separation_params;

constexpr double kRadius = 8.0;
constexpr double kSpeed = 60.0;
constexpr double kDt = 1.0 / 20.0;
constexpr int kTicks = 120;

struct ship {
    dvec3 pos, vel;
    dvec3 waypoint;
};

// One tick for either ship: straight-line drive toward the waypoint at
// authored speed + the shared separation term. Identical code path for
// the autopilot ship and the NPC ship — no privileged avoidance.
void step_ship(ship& s, const std::vector<hazard>& others,
               const separation_params& sp) {
    const dvec3 want_dir = s.waypoint - s.pos;
    const double dl = length(want_dir);
    const dvec3 desired = dl > 0.0 ? want_dir * (kSpeed / dl) : dvec3{};
    const dvec3 drive = (desired - s.vel) * 2.0;  // lane-following drive
    const dvec3 sep = separation_accel(s.pos, kRadius, others, sp);
    const dvec3 acc = drive + sep;
    s.vel = s.vel + acc * kDt;
    s.pos = s.pos + s.vel * kDt;
}

// Returns {min distance, FNV of both position streams}.
std::pair<double, std::uint64_t> run_crossing_lanes() {
    ship a{{-200.0, 0.0, 0.0}, {kSpeed, 0.0, 0.0}, {200.0, 0.0, 0.0}};
    ship b{{0.0, 0.0, -200.0}, {0.0, 0.0, kSpeed}, {0.0, 0.0, 200.0}};
    const separation_params sp{.margin = 40.0, .stiffness = 200.0};
    double min_d = 1e9;
    ve::core::Fnv1a64 h;
    for (int t = 0; t < kTicks; ++t) {
        const std::vector<hazard> for_a{{b.pos, kRadius}};
        const std::vector<hazard> for_b{{a.pos, kRadius}};
        step_ship(a, for_a, sp);
        step_ship(b, for_b, sp);
        const double d = length(a.pos - b.pos);
        min_d = std::min(min_d, d);
        for (const dvec3* p : {&a.pos, &b.pos})
            for (const double c : {p->x, p->y, p->z})
                h.mix_u64(std::bit_cast<std::uint64_t>(c));
    }
    return {min_d, h.value()};
}

}  // namespace

TEST_CASE("lane_separation_deflect") {
    const auto [min_d, hash] = run_crossing_lanes();

    // Deflected: never inside the summed radii...
    CHECK(min_d > 2.0 * kRadius);
    // ...and not flung away either (term is a nudge, not a wall): the
    // upper bound catches "unnormalized push" and stiffness mutants.
    CHECK(min_d < 35.0);

    // Identical on replay: bit-for-bit same hash from the same law.
    const auto [min_d2, hash2] = run_crossing_lanes();
    CHECK(min_d2 == min_d);
    CHECK(hash2 == hash);
}
