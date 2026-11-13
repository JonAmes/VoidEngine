// Phase 8 task 8.9 — floating-origin rebasing + camera-relative rendering.
// floating_origin: the rebase rule fires at EXACT ticks on a hand-computed
// focus path, mutates nothing a per-frame probe can reach, and leaves the
// 4.7 state-hash stream bit-identical with and without rebasing.
// camera_relative_precision: fp64-relative-then-f32 composition keeps metre
// detail at AU-scale offsets; the world-absolute f32 path collapses bodies.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "ve/core/math/coords.h"
#include "ve/core/math/mat4.h"
#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/origin.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"

namespace {

using namespace ve::core;
using ve::core::math::dvec3;
using ve::core::math::mat4f;
using ve::core::math::quatf;
using ve::core::math::vec3f;
using ve::core::math::vec4f;
using sim::OriginTracker;
using sim::ReplayLog;
using sim::body_def;
using sim::rail_world;
using sim::rigid_params;
using sim::rigid_state;
using sim::tick_command;
using sim::tick_t;

// Same fall world + command log as test_sim_replay (4.7) so the hash-stream
// identity proof rides the already-pinned deterministic scenario.
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

ReplayLog make_log() {
    ReplayLog log;
    for (int i = 0; i < 3600; ++i) {
        tick_command c{};
        c.thrust_body = dvec3(5000.0, 0.0, 0.0);
        c.torque_body = dvec3(0.0, 200.0, 0.0);
        c.assist_mask = 0;
        log.append(c);
    }
    return log;
}

// Run the 4.7 replay, hashing every tick; when `tr` is non-null, advance the
// tracker on the post-step focus at every exact tick. Returns per-tick state
// hashes; `rebase_ticks` (if given) collects the ticks the tracker fired at.
std::vector<std::uint64_t> run_and_track(const rail_world& w, const ReplayLog& log,
                                         const rigid_params& p, OriginTracker* tr,
                                         std::vector<tick_t>* rebase_ticks) {
    rigid_state s{};
    s.pos = dvec3(98500.0, 0.0, 0.0);
    s.vel = dvec3(0.0, 0.0, 10.0);
    std::vector<std::uint64_t> hashes;
    hashes.reserve(log.size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        const tick_t tick = static_cast<tick_t>(i) + 1;
        sim::replay_step(w, log, i, tick, p, s);
        if (tr != nullptr && tr->advance(tick, s.pos, 250.0) && rebase_ticks != nullptr)
            rebase_ticks->push_back(tick);
        hashes.push_back(sim::state_hash(tick, s));
    }
    return hashes;
}

} // namespace

TEST_CASE("floating_origin") {
    // ---- exact-tick rebase on a hand-computed linear focus path ----------
    // focus(t) = (8t, 0, 0) m (binary-exact steps), threshold 1000 m:
    // 8*125 = 1000 is NOT > 1000 (strict crossing), 8*126 = 1008 is the first;
    // after the origin snaps to the focus at t=126 the corridor re-opens and
    // the next strict crossings land at t=252 and t=378 (every 126 ticks).
    {
        OriginTracker tr;
        std::vector<tick_t> fires;
        for (tick_t t = 1; t <= 400; ++t) {
            const dvec3 focus(8.0 * static_cast<double>(t), 0.0, 0.0);
            if (tr.advance(t, focus, 1000.0))
                fires.push_back(t);
        }
        const std::vector<tick_t> want{126, 252, 378};
        CHECK(fires == want);
        CHECK(tr.rebase_count() == 3);
        CHECK(tr.last_rebase_tick() == 378);
        // Origin snapped exactly onto the focus at the firing tick.
        const dvec3 org = tr.frame().origin;
        CHECK(org.x == 8.0 * 378.0);
        CHECK(org.y == 0.0);
        CHECK(org.z == 0.0);
        // Locals stay small: distance from origin is bounded by the corridor.
        const dvec3 loc = tr.frame().to_local(dvec3(8.0 * 400.0, 0.0, 0.0));
        CHECK(loc.x == 8.0 * 22.0);
    }

    // ---- threshold equality does NOT fire; mid-tick probes mutate nothing -
    {
        OriginTracker tr;
        for (tick_t t = 1; t <= 125; ++t)
            tr.advance(t, dvec3(8.0 * static_cast<double>(t), 0.0, 0.0), 1000.0);
        CHECK(tr.rebase_count() == 0);                       // 1000 !> 1000
        // A render frame asking "would this interpolated focus shift us?" is
        // a pure question: needs_shift is const, the tracker cannot move.
        const bool would = tr.frame().needs_shift(dvec3(1004.0, 0.0, 0.0), 1000.0);
        CHECK(would == true);
        CHECK(tr.rebase_count() == 0);
        CHECK(tr.last_rebase_tick() == 0);                   // never fired
        const dvec3 org0 = tr.frame().origin;
        CHECK(org0.x == 0.0);
        CHECK(tr.advance(126, dvec3(1008.0, 0.0, 0.0), 1000.0) == true);
        CHECK(tr.last_rebase_tick() == 126);
    }

    // ---- replay hash stream identical WITH and WITHOUT rebasing ----------
    // The tracker is observation-only: positions stay absolute fp64 and the
    // physics never reads the origin, so the 4.7 per-tick hash stream must be
    // bit-identical across runs with and without rebasing.
    rail_world w;
    add_fall_world(w);
    const rigid_params p{20000.0, dvec3(2e5, 2e5, 4e5)};
    const ReplayLog log = make_log();
    REQUIRE(log.size() == 3600);

    const std::vector<std::uint64_t> plain = run_and_track(w, log, p, nullptr, nullptr);

    OriginTracker tr;
    std::vector<tick_t> fires;
    const std::vector<std::uint64_t> tracked = run_and_track(w, log, p, &tr, &fires);
    CHECK(plain == tracked);  // hash streams bit-identical
    const std::uint64_t h_plain = sim::replay_interval_hash(plain, 0, 3599);
    const std::uint64_t h_tracked = sim::replay_interval_hash(tracked, 0, 3599);
    CHECK(h_plain == h_tracked);

    // The rebasing actually happened (threshold 250 m vs a 98.5 km start:
    // tick 1 must fire; the ~1 km fall path outruns 250 m repeatedly).
    CHECK(tr.rebase_count() >= 3);
    REQUIRE(!fires.empty());
    CHECK(fires.front() == 1);

    // ---- the rebase tick sequence itself replays identically -------------
    OriginTracker tr2;
    std::vector<tick_t> fires2;
    run_and_track(w, log, p, &tr2, &fires2);
    CHECK(fires == fires2);
    CHECK(tr2.rebase_count() == tr.rebase_count());
    CHECK(tr2.last_rebase_tick() == tr.last_rebase_tick());
}

TEST_CASE("camera_relative_precision") {
    // ---- AU-scale offset: relative composition keeps metres, absolute dies -
    // fp32 ulp at 1.496e11 m is 2^14 = 16384 m. Bodies a/b sit 10 m apart at
    // that offset; world-absolute f32 casts land them on the SAME bucket.
    const double kAU = 149597870700.0;
    math::origin_frame of{dvec3{kAU, 0.0, 0.0}};
    const dvec3 a{kAU + 1234.5, 678.25, -900.125};  // binary-exact offsets
    const dvec3 b = a + dvec3{10.0, 0.0, 0.0};

    // Sanctioned path (fp64 subtract FIRST, then f32 cast): exact metres.
    const vec3f la = of.to_local_f32(a);
    const vec3f lb = of.to_local_f32(b);
    const vec3f drel = lb - la;
    CHECK(drel == vec3f{10.f, 0.f, 0.f});

    // Forbidden path (cast world-absolute to f32): total collapse.
    const vec3f va{float(a.x), float(a.y), float(a.z)};
    const vec3f vb{float(b.x), float(b.y), float(b.z)};
    const vec3f dabs = vb - va;
    CHECK(dabs == vec3f{0.f, 0.f, 0.f});  // same 16 km bucket
    const double err = std::abs(static_cast<double>(va.x) - a.x);
    CHECK(err == 7998.5);  // hand-computed f32 rounding of a.x

    // ---- composition rides the local, never the world --------------------
    const mat4f M = math::trs_compose(math::trsf{la, quatf{}, {1.f, 1.f, 1.f}});
    CHECK(M.m[12] == la.x);
    CHECK(M.m[13] == la.y);
    CHECK(M.m[14] == la.z);

    // f32 view built from a LOCAL eye separates the two bodies correctly.
    const vec3f eye = la + vec3f{0.f, 0.f, 50.f};
    const mat4f V = math::look_at_rh<float>(eye, la, vec3f{0.f, 1.f, 0.f});
    const vec4f pa = V * vec4f{la.x, la.y, la.z, 1.f};
    const vec4f pb = V * vec4f{lb.x, lb.y, lb.z, 1.f};
    const float dx = pb.x - pa.x;
    const float dy = pb.y - pa.y;
    const float dz = pb.z - pa.z;
    const float sep = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float sep_err = std::abs(sep - 10.f);
    CHECK(sep_err < 1e-3f);  // 10 m separation survives at 1 AU offset

    // World-absolute coords through the same view land on one point: this is
    // WHY nothing may render world-absolute (8.9 rule).
    const vec4f qa = V * vec4f{va.x, va.y, va.z, 1.f};
    const vec4f qb = V * vec4f{vb.x, vb.y, vb.z, 1.f};
    CHECK(qa == qb);
}
