// Phase 4 task 4.7 — record & replay + FNV-1a-64 state hashes (sim_replay).
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"

namespace {

using namespace ve::core;
using ve::core::math::dvec3;  // nested-namespace names don't leak via using namespace
using sim::ReplayLog;
using sim::body_def;
using sim::rail_world;
using sim::rigid_params;
using sim::rigid_state;
using sim::tick_command;
using sim::tick_t;

void add_fall_world(rail_world& w) {
    body_def root{};
    root.surface_radius = 10.0;
    root.well_radius = 0.0;
    REQUIRE(w.add_body(root).has_value());
    body_def p{};
    p.surface_radius = 1000.0;
    p.well_radius = 13000.0;
    p.well_taper_start = 2000.0;
    p.surface_g = 9.8;
    p.parent = 0;
    p.orbit_period_s = 360000.0;
    p.orbit_radius_m = 100000.0;
    p.orbit_phase_rad = 0.0;
    REQUIRE(w.add_body(p).has_value());
}

// 60 s scripted scenario: constant body-frame thrust + gentle torque while
// falling past the rail planet. Returns the per-tick state-hash stream.
std::vector<std::uint64_t> run_replay(const rail_world& w, const ReplayLog& log, const rigid_params& p) {
    rigid_state s{};
    s.pos = dvec3(98500.0, 0.0, 0.0);
    s.vel = dvec3(0.0, 0.0, 10.0);
    std::vector<std::uint64_t> hashes;
    hashes.reserve(log.size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        sim::replay_step(w, log, i, static_cast<tick_t>(i) + 1, p, s);
        hashes.push_back(sim::state_hash(static_cast<tick_t>(i) + 1, s));
    }
    return hashes;
}

ReplayLog make_log() {
    ReplayLog log;
    for (int i = 0; i < 3600; ++i) {
        tick_command c{};
        c.thrust_body = dvec3(5000.0, 0.0, 0.0);
        c.torque_body = dvec3(0.0, 200.0, 0.0);  // slow yaw while thrusting
        c.assist_mask = 0;
        log.append(c);
    }
    return log;
}

} // namespace

TEST_CASE("sim_replay") {
    rail_world w;
    add_fall_world(w);
    const rigid_params p{20000.0, dvec3(2e5, 2e5, 4e5)};
    const ReplayLog log = make_log();
    REQUIRE(log.size() == 3600);

    // ---- replay reproduces the tick-for-tick hash stream bitwise.
    const std::vector<std::uint64_t> a = run_replay(w, log, p);
    const std::vector<std::uint64_t> b = run_replay(w, log, p);
    CHECK(a.size() == 3600);
    CHECK(a == b);
    CHECK(sim::replay_interval_hash(a, 0, 3599) == sim::replay_interval_hash(b, 0, 3599));

    // ---- command perturbations: a raw 1-ULP nudge of a 5 kN command is
    // deterministically ERASED by the rotate/divide roundings (perturbed
    // stream bit-identical — hashes track STATE bits, not input bits). A
    // perturbation that survives rounding (1e-6 N on 5 kN, 2e-10 rel)
    // diverges the stream from the first affected tick on, clean prefix.
    ReplayLog nudged = log;
    tick_command c = nudged.at(1800);
    c.thrust_body.x = std::nextafter(c.thrust_body.x, std::numeric_limits<double>::infinity());
    nudged.set(1800, c);
    const std::vector<std::uint64_t> n1 = run_replay(w, nudged, p);
    CHECK(n1 == a);  // 1-ULP input rounding-erasure (deterministic, pinned)

    nudged = log;
    c = nudged.at(1800);
    c.thrust_body.x += 1e-6;
    nudged.set(1800, c);
    const std::vector<std::uint64_t> n2 = run_replay(w, nudged, p);
    std::size_t first = 0;
    while (first < 3600 && a[first] == n2[first])
        ++first;
    CHECK(first >= 1800);
    CHECK(first <= 1810);  // rounding must surface within a few ticks
    bool diverged = true;
    for (std::size_t i = first; i < 3600; ++i)
        diverged = diverged && (a[i] != n2[i]);
    CHECK(diverged);
    CHECK(sim::replay_interval_hash(a, 0, 3599) != sim::replay_interval_hash(n2, 0, 3599));

    // ---- hash sensitivity primitives: 1 ULP in a state double flips the
    // hash; the tick is part of the hash; raw bits mean -0.0 != +0.0.
    {
        rigid_state s{};
        s.pos = dvec3(1000.0, 200.0, -300.0);
        s.vel = dvec3(1.0, 2.0, 3.0);
        const std::uint64_t h0 = sim::state_hash(42, s);
        CHECK(sim::state_hash(42, s) == h0);                      // deterministic
        CHECK(sim::state_hash(43, s) != h0);                      // tick included
        rigid_state s1 = s;
        s1.pos.y = std::nextafter(s1.pos.y, std::numeric_limits<double>::infinity());
        CHECK(sim::state_hash(42, s1) != h0);                     // 1 ULP flips it
        rigid_state neg_zero = s;
        neg_zero.vel = dvec3(0.0, -0.0, 3.0);
        CHECK(sim::state_hash(42, neg_zero) != sim::state_hash(42, s));  // raw bits
    }

    // ---- interval hash distinguishes ranges (cheap whole-run stamps).
    const std::uint64_t full = sim::replay_interval_hash(a, 0, 3599);
    CHECK(sim::replay_interval_hash(a, 0, 3598) != full);
    CHECK(sim::replay_interval_hash(a, 1800, 3599) != full);
    CHECK(sim::replay_interval_hash(a, 0, 3599) == full);
}
