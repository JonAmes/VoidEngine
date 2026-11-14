// Phase 8 task 8.16 — nested moving wells under compression.
// Decision #53a says the home-well selection and the #17b crossfade are
// re-evaluated EVERY tick against the retained (possibly divisor-1 stale)
// state. 8.15 proved the lattice steps agree with the spine; this proves
// the well MACHINERY riding alongside the lattice — the home tracker and
// the blend — stays stable while bodies coarse-step and the wells
// THEMSELVES move (rail moons).
//
// Hand-authored testbed (Tier-1, no cook, same dimensions as the
// well_blend owner testbed): a planet well + a moon well orbiting inside
// it on a rail, a landing pad on the moon's sub-planet point (tidally
// locked frame: spin 0), a scripted braked descent under the divisor-1000
// LOD lattice, and a distant parked ship coarse-integrated through the
// moon's flyby. Tests: `moon_landing_down` (down = moon THROUGHOUT the
// landing, engagement snaps, determinism) and `nested_wells_compressed`
// (distant coarse ship keeps home = planet through the flyby; crossfade
// bit-stable at every physics tick).
#include <doctest/doctest.h>

#include <ve/core/math/quat.h>
#include <ve/core/math/vec.h>
#include <ve/core/sim/capture.h>
#include <ve/core/sim/gravity.h>
#include <ve/core/sim/lod.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/well_blend.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using math::dvec3;

// ---- the nested-wells testbed ------------------------------------------------
// Planet at rest at origin (root), moon on the rails in the XZ plane
// (positive about +Y, #34). Period 36000 s: pad drift ~2 m/s, so one
// coarse divisor-1000 step (16.7 s) slides the whole moon well ~34 m
// under a frozen ship — small vs the capture radius, but real drift.
rail_world make_world(double moon_phase_rad) {
    rail_world w;
    body_def pl{};
    pl.surface_radius = 1000.0;
    pl.well_radius = 13000.0;
    pl.well_taper_start = 2000.0;
    pl.surface_g = 9.8;
    REQUIRE(w.add_body(pl).has_value());  // id 0, root at origin
    body_def mo{};
    mo.parent = 0;
    mo.orbit_radius_m = 12000.0;
    mo.orbit_period_s = 36000.0;
    mo.orbit_phase_rad = moon_phase_rad;
    mo.surface_radius = 400.0;
    mo.well_radius = 5000.0;
    mo.well_taper_start = 1000.0;
    mo.surface_g = 2.0;
    // spin 0: locked frame — the pad stays on the sub-planet point forever.
    REQUIRE(w.add_body(mo).has_value());  // id 1
    return w;
}

// Landing pad on the moon's sub-planet point (local -X points at the
// planet in the locked frame). Capture generous vs the 34 m/step drift.
pad_def moon_pad() {
    return pad_def{
        .body = 1,
        .local_dir = {-1.0, 0.0, 0.0},
        .capture_radius_m = 300.0,
        .safe_speed_m_s = 30.0,
        .rest_depth_m = 0.5,
    };
}

rigid_params ship_params() {
    rigid_params p{};
    p.mass = 90000.0;
    p.inertia_diag = {9.0e6, 9.0e6, 9.0e6};
    return p;
}

// #53a bands, observer = the planet center (a "station" watching from
// home): both test ships sit beyond 2000 m => divisor 1000 (far band).
std::vector<lod_band> far_bands() {
    return {lod_band{2000.0, 1}, lod_band{1.0e18, 1000}};
}

// Control profile tuned for COARSE sampling, not for fidelity: the loop
// bandwidth (~0.02 rad/s) sits ~300x under the ~1/17 s Nyquist of the
// divisor-1000 lattice, and the engine cap keeps every coarse Δv inside
// the capture safe-speed. The staleness of the
// interval-start sample is exactly what this test is about.
constexpr double kDescent_mps = 2.0;   // scripted setpoint down the column
constexpr double kPd_kp = 0.0005;
constexpr double kPd_kd = 0.5;
constexpr double kMax_accel_cmd = 4.0;  // cap: above moon surface g => braking authority

// The scripted pilot (#8.16 "scripted"): PD toward the CURRENT pad world
// + descent setpoint along the CURRENT pad normal + gravity feed-forward
// through the SHIPPED blend (the same code path the integrator uses).
// Computed every tick, consumed by the lattice at interval start.
tick_command pilot_cmd(const rail_world& w, const pad_def& pad, const rigid_state& s,
                       tick_t tick) {
    const auto pt = pad_at(w, pad, tick);
    REQUIRE(pt.has_value());
    const dvec3 v_des = pt->vel - pt->normal * kDescent_mps;
    dvec3 a_des = (pt->pos - s.pos) * kPd_kp + (v_des - s.vel) * kPd_kd;
    a_des = a_des - total_accel(w, s.pos, tick);  // cancel local blend gravity
    const double amag = length(a_des);
    if (amag > kMax_accel_cmd)
        a_des = a_des * (kMax_accel_cmd / amag);
    tick_command cmd{};  // att stays identity until snap => body == world frame
    cmd.thrust_body = a_des * ship_params().mass;
    return cmd;
}

struct descent_record {
    std::int64_t snap_tick = -1;
    std::uint64_t interval_hash = 0;
    std::uint64_t rest_hash = 0;
    std::uint64_t steps = 0;       // physics steps actually taken
    std::uint64_t far_steps = 0;   // steps taken at divisor > 1
    std::int64_t last_phys_tick = -1;
    std::vector<lod_transition> sched;
    std::vector<double> residual;  // |total - home well| at each physics tick
    std::uint64_t switches = 0;    // home-well switch events (incl. acquisition)
    double first_residual = -1.0;
    std::int64_t band_exit_tick = -1;  // first tick the blend goes bit-exact home
    rigid_state rest{};
    dvec3 rest_rel{};  // rest pos minus pad pos at snap tick
};

enum class drive_mode { reference, compressed };

descent_record run_descent(const rail_world& w, const pad_def& pad, tick_t horizon,
                           drive_mode mode) {
    descent_record r;
    ReplayLog log;
    const rigid_params p = ship_params();
    lod_body b{};  // divisor 1, last_step_tick -1
    home_well_tracker track{0.1};

    // Spawn on the pad column, 1300 m out from the pad surface — past the
    // planet/moon crossover (~10100 m), so the MOON is argmax home from
    // tick 0, yet the whole column sits deep inside the PLANET well's
    // reach: the #17b nested regime, near the crossfade band's outer edge.
    // Co-moving with the pad (rail velocity, moving-well spawn).
    const auto pad0 = pad_at(w, pad, 0);
    REQUIRE(pad0.has_value());
    rigid_state s{.pos = pad0->pos + pad0->normal * 1300.0, .vel = pad0->vel};

    std::vector<std::uint64_t> hashes;
    hashes.reserve(static_cast<std::size_t>(horizon) + 1);
    bool landed = false;
    std::int32_t last_home = -1;

    for (tick_t tick = 0; tick <= horizon; ++tick) {
        // #53a: bands re-evaluated every tick on the retained state.
        if (mode == drive_mode::compressed) {
            const std::uint64_t want = lod_divisor(length(s.pos), far_bands());
            if (want != b.divisor) {
                r.sched.push_back({tick, want});
                b.divisor = want;
            }
        }

        // Home-well feed EVERY tick — including the ~999 frozen ticks
        // between coarse steps, while the moon (its well, the pad, the
        // whole frame) drifts up to ~34 m under the frozen ship. "Same
        // source" (#8.9/20c): the retained position only.
        const std::int32_t home = track.update(w, s.pos, tick).value_or(-1);
        if (home != last_home) {
            ++r.switches;
            last_home = home;
        }
        // "down = moon THROUGHOUT": from the tick-0 acquisition, home is
        // the moon EVERY tick — across frozen gaps, coarse jumps, the
        // engagement, and the pad-locked rest phase.
        CHECK(home == 1);

        // Crossfade stability sample (#17b): residual of the SHIPPED blend
        // against the moon well's own pull. Sampled EVERY tick, not just
        // physics steps — the crossfade is re-evaluated on the retained
        // (possibly divisor-1 stale) state, exactly what #53a mandates.
        r.residual.push_back(
            length(total_accel(w, s.pos, tick) - well_accel(w, 1, s.pos, tick)));
        if (r.first_residual < 0.0)
            r.first_residual = r.residual.back();
        if (r.band_exit_tick < 0 && r.residual.back() == 0.0)
            r.band_exit_tick = static_cast<std::int64_t>(tick);

        if (!landed) {
            log.append(pilot_cmd(w, pad, s, tick));
            bool stepped = false;
            if (mode == drive_mode::reference) {
                replay_step(w, log, static_cast<std::size_t>(tick), tick, p, s);
                stepped = true;
            } else {
                stepped = lod_replay_step(w, log, tick, p, s, b);
            }
            if (stepped) {
                ++r.steps;
                if (b.divisor > 1)
                    ++r.far_steps;
                r.last_phys_tick = tick;
                // #29c capture engagement, tick-dense on the retained state.
                const auto pw = pad_at(w, pad, tick);
                REQUIRE(pw.has_value());
                if (capture_engage(pad, *pw, s.pos, s.vel, tick)) {
                    s = snap_to_rest(s, *pw, pad);
                    landed = true;
                    r.snap_tick = static_cast<std::int64_t>(tick);
                    r.rest = s;
                    r.rest_rel = s.pos - pw->pos;
                }
            }
        } else {
            log.append(tick_command{});  // log stays tick-dense (#53c)
            // Capture-zone rest lock (7.9): the game pins the ship at the
            // pad — carried by the moon as it orbits. Pin the down-well
            // every carried tick, deep inside the planet's reach:
            // surface rest sits FAR under the 0.75 suppression ratio, so
            // the shipped blend must return the moon's pull BIT-EXACT at
            // every tick of the orbit — no flicker under compression.
            const auto pw = pad_at(w, pad, tick);
            REQUIRE(pw.has_value());
            s.pos = pw->pos - pw->normal * pad.rest_depth_m;
            s.vel = pw->vel;
            CHECK(total_accel(w, s.pos, tick) == well_accel(w, 1, s.pos, tick));
        }
        hashes.push_back(state_hash(tick, s));
    }
    r.interval_hash = replay_interval_hash(hashes, 0, static_cast<std::size_t>(horizon));
    r.rest_hash = state_hash(static_cast<tick_t>(r.snap_tick), r.rest);
    return r;
}

constexpr tick_t kDescent_horizon = 90000;  // 1800 s: descent ~650 s + margin

}  // namespace

TEST_CASE("moon_landing_down") {
    const rail_world w = make_world(0.0);
    const pad_def pad = moon_pad();

    const descent_record comp = run_descent(w, pad, kDescent_horizon, drive_mode::compressed);
    const descent_record ref = run_descent(w, pad, kDescent_horizon, drive_mode::reference);

    // ---- the landing happened, under 1000x ----
    REQUIRE(comp.snap_tick >= 0);
    REQUIRE(ref.snap_tick >= 0);
    CHECK(comp.snap_tick > 5000);
    CHECK(comp.snap_tick < 12000);
    // Bounded error (#53b), loose on purpose: under divisor 1000 the
    // gravity feed-forward is sampled once per ~17 s, so the coarse descent
    // free-falls the stale-sample gap — it lands ~14 coarse steps earlier
    // than full rate. Both spines land; that is the contract (tight bounded
    // error was 8.15's cruise pin; the descent loop here is band-limited
    // far under the coarse Nyquist, so arrival ordering is all that pins).
    CHECK(std::abs(comp.snap_tick - ref.snap_tick) <= 15000);

    // Physics collapsed ~1000x: divisor 1000 from tick 0 (far band), so
    // every step is a far step; ~8 of them did the whole landing.
    CHECK(comp.steps == comp.far_steps);
    CHECK(comp.far_steps >= 6);
    CHECK(comp.far_steps <= 12);
    CHECK(ref.steps == static_cast<std::uint64_t>(ref.snap_tick) + 1);

    // Schedule: exactly one transition (tick 0: far band => 1000); the
    // closed-form lattice (#53c) agrees with the walked run through the
    // landing tick, step for step.
    REQUIRE(comp.sched.size() == 1);
    CHECK(comp.sched[0].tick == 0);
    CHECK(comp.sched[0].divisor == 1000);
    const auto closed = lod_lattice_at(comp.sched, static_cast<tick_t>(comp.snap_tick));
    CHECK(closed.steps_applied == comp.steps);
    CHECK(closed.last_step_tick == comp.last_phys_tick);
    CHECK(closed.divisor == 1000);

    // ---- down = moon THROUGHOUT the landing ----
    // run_descent CHECK-ed home == moon at EVERY tick (frozen gaps
    // included, descent endgame with stale samples included). Bookkeeping:
    // exactly ONE switch event — the tick-0 acquisition — never a flip
    // back to the planet near the crossover, never one during the carried
    // rest phase either.
    CHECK(comp.switches == 1);
    CHECK(ref.switches == 1);

    // ---- engagement snapped to pad rest (7.9 pins, moving host) ----
    const auto pw_snap = pad_at(w, pad, static_cast<tick_t>(comp.snap_tick));
    REQUIRE(pw_snap.has_value());
    // Gear contact rests rest_depth under the pad surface (bit-tight).
    const dvec3 want = pw_snap->pos - pw_snap->normal * pad.rest_depth_m;
    CHECK(comp.rest.pos.x == doctest::Approx(want.x).epsilon(1e-12));
    CHECK(comp.rest.pos.y == doctest::Approx(want.y).epsilon(1e-12));
    CHECK(comp.rest.pos.z == doctest::Approx(want.z).epsilon(1e-12));
    // Pad-relative rest, bit-exact: the ship moves WITH the orbiting moon.
    CHECK(comp.rest.vel == pw_snap->vel);
    CHECK(length(comp.rest.omega_body) == 0.0);
    CHECK(length(comp.rest.vel) > 1.0);  // a moving pad, not a static rest
    // Hull -Y aims along the pad normal => the moon is under the keel, at
    // the surface: |rest - moon center| = 399.5 m.
    const auto down = math::rotate(comp.rest.att, {0.0, -1.0, 0.0});
    const auto moon_c = w.position(1, static_cast<tick_t>(comp.snap_tick));
    REQUIRE(moon_c.has_value());
    const auto nrm = math::normalize(comp.rest.pos - *moon_c);
    CHECK(std::abs(math::dot(down, nrm)) > 0.999999);
    CHECK(std::abs(length(comp.rest.pos - *moon_c) - (400.0 - pad.rest_depth_m)) < 1e-9);
    // The rest offset in the PAD frame is exactly the authored rest depth,
    // compressed and reference alike: same touchdown geometry while the
    // pad itself travelled ~1 km along its orbit between the two snaps.
    CHECK(length(comp.rest_rel) == doctest::Approx(pad.rest_depth_m).epsilon(1e-12));
    const auto pw_ref = pad_at(w, pad, static_cast<tick_t>(ref.snap_tick));
    REQUIRE(pw_ref.has_value());
    CHECK(length(ref.rest_rel) == doctest::Approx(pad.rest_depth_m).epsilon(1e-12));

    // ---- crossfade stayed stable under the coarse sampling ----
    // The blend residual (giant contribution at the ship) starts STRICTLY
    // INSIDE the #17b band near the crossover (positive giant weight,
    // measured 0.378 m/s^2 at spawn), decays monotonically — never
    // re-growing across a coarse interval, no flicker from stale
    // sampling — and hits BIT-EXACT zero by the landing physics ticks
    // (suppression floor).
    REQUIRE(comp.residual.size() >= 6);
    CHECK(comp.first_residual > 0.05);
    // No re-growth past the orbital wobble band: the moon (its well, the
    // pad column) drifts sideways ~2 m/s under the retained ship, so the
    // ratio wobbles +-1e-8/tick — but the ENVELOPE never rises across
    // coarse intervals (a stale sample never re-lights the crossfade).
    bool no_regrow = true;
    for (std::size_t i = 1; i < comp.residual.size(); ++i)
        if (comp.residual[i] > comp.residual[i - 1] + 1e-3)
            no_regrow = false;
    CHECK(no_regrow);
    for (std::size_t i = comp.residual.size() - 2; i < comp.residual.size(); ++i)
        CHECK(comp.residual[i] == 0.0);
    // Band exit under compression: the crossover is crossed within the
    // first FEW coarse steps (measured: 2-3 steps), long before the
    // landing tick, and stays pinned at zero through touchdown + rest.
    CHECK(comp.band_exit_tick > 500);
    CHECK(comp.band_exit_tick < 7000);
    CHECK(ref.band_exit_tick > 500);
    CHECK(ref.band_exit_tick < 7000);

    // ---- determinism: two identical compressed runs, bit-for-bit ----
    const descent_record comp2 = run_descent(w, pad, kDescent_horizon, drive_mode::compressed);
    CHECK(comp2.interval_hash == comp.interval_hash);
    CHECK(comp2.snap_tick == comp.snap_tick);
    CHECK(comp2.rest_hash == comp.rest_hash);
}

TEST_CASE("nested_wells_compressed") {
    // Distant parked ship (#53a "far" band, divisor 1000) hovering against
    // the PLANET's pull at (0,0,9000) while the moon — started at phase
    // pi, i.e. (-12000, 0, 0) — swings OVERHEAD (+Z, closest approach
    // 3000 m, deep inside its 5000 m well reach) at a quarter period:
    // 9000 s = 540000 ticks (kSim_tick_dt = 1/60 s, 18000 s / 4). ~7600 s
    // of margin past the flyby.
    const rail_world w = make_world(3.14159265358979323846);
    constexpr tick_t horizon = 1000000;
    const std::vector<lod_band> bands = far_bands();

    auto run = [&](std::uint64_t& switches_out, std::uint64_t& steps_out,
                   std::vector<std::uint64_t>& hashes_out, std::int64_t& closest_tick_out,
                   double& min_ratio_out, double& min_d_out) {
        ReplayLog log;
        const rigid_params p = ship_params();
        lod_body b{};
        home_well_tracker track{0.1};
        rigid_state s{.pos = {0.0, 0.0, 9000.0}};
        std::int32_t last_home = -1;
        switches_out = 0;
        steps_out = 0;
        closest_tick_out = -1;
        min_ratio_out = 1.0e18;
        min_d_out = 1.0e18;
        double best_d = 1.0e18;
        hashes_out.reserve(static_cast<std::size_t>(horizon) + 1);

        for (tick_t tick = 0; tick <= horizon; ++tick) {
            const std::uint64_t want = lod_divisor(length(s.pos), bands);
            if (want != b.divisor)
                b.divisor = want;  // single tick-0 transition; schedule not
                                   // pinned here — this run pins HOME state.
            const std::int32_t home = track.update(w, s.pos, tick).value_or(-1);
            if (home != last_home) {
                ++switches_out;
                last_home = home;
            }
            // Home NEVER leaves the planet: at the 3000 m flyby the
            // moon (g=2, outer ramp) reaches only ~34% of the planet's
            // pull (g=9.8, deep) here — under the 0.75 crossfade floor, so
            // its blend weight stays pinned at zero and the 1.1x home
            // hysteresis is never approached. A stale sample cannot steal.
            CHECK(home == 0);

            tick_command cmd{};
            cmd.thrust_body = -total_accel(w, s.pos, tick) * p.mass;  // hover
            log.append(cmd);
            if (lod_replay_step(w, log, tick, p, s, b)) {
                ++steps_out;
                // Crossfade bit-stability at EVERY physics tick, including
                // the flyby itself: the planet is the max well and the
                // moon's weight sits pinned at zero by suppression — the
                // shipped blend must return the planet's pull EXACTLY.
                CHECK(total_accel(w, s.pos, tick) == well_accel(w, 0, s.pos, tick));
            }
            const auto mp = w.position(1, tick);
            REQUIRE(mp.has_value());
            const double d = length(s.pos - *mp);
            if (d < best_d) {
                best_d = d;
                closest_tick_out = static_cast<std::int64_t>(tick);
                const double moon_a = length(well_accel(w, 1, s.pos, tick));
                const double home_a = length(well_accel(w, 0, s.pos, tick));
                min_ratio_out = moon_a / home_a;
                min_d_out = d;
            }
            hashes_out.push_back(state_hash(tick, s));
        }
    };

    std::uint64_t sw1 = 0, sw2 = 0, st1 = 0, st2 = 0;
    std::vector<std::uint64_t> h1, h2;
    std::int64_t ct1 = -1, ct2 = -1;
    double mr1 = 0.0, mr2 = 0.0, bd1 = 0.0, bd2 = 0.0;
    run(sw1, st1, h1, ct1, mr1, bd1);
    run(sw2, st2, h2, ct2, mr2, bd2);

    // The flyby is real and genuinely inside the moon's well reach:
    // closest approach 3000 m analytic (12000 orbit - 9000 hover); the
    // hover's meter-scale wander widens the pin to +-100 m.
    CHECK(bd1 > 2900.0);
    CHECK(bd1 < 3100.0);
    CHECK(bd2 == bd1);

    // (Geometry pins live in the CHECK block below, on the final minima.)

    // Exactly one acquisition (tick 0, planet), never a flip — ~1M
    // tick-dense home feeds per run, coarse-frozen gaps included.
    CHECK(sw1 == 1);
    CHECK(sw2 == 1);

    // The flyby happened INSIDE the coarse lattice: closest approach lands
    // at the analytic quarter-period tick window (540000 +- 1000 — the
    // hover's meter-scale wander shifts the exact minimum), the moon well
    // genuinely engaged yet stayed far under the suppression floor —
    // home and blend both unfazed.
    CHECK(ct1 > 539000);
    CHECK(ct1 < 541000);
    CHECK(ct2 == ct1);
    // Measured 0.3395 at closest: 34% of home's pull — the moon well is
    // genuinely lit yet 2x under the 0.75 crossfade floor; window pinned.
    CHECK(mr1 > 0.30);
    CHECK(mr1 < 0.40);
    CHECK(mr2 == mr1);

    // Steps collapsed 1000x and the closed-form lattice (#53c) agrees.
    const std::vector<lod_transition> sched{{0, 1000}};
    const auto closed = lod_lattice_at(sched, horizon);
    CHECK(closed.steps_applied == st1);
    // Divisor 1000 from tick 0: due ticks are 999, 1999, ... 459999.
    CHECK(closed.last_step_tick == 999999);
    CHECK(st1 == st2);
    CHECK(st1 >= 999);
    CHECK(st1 <= 1002);

    // Bit-for-bit determinism across the whole flyby window.
    CHECK(h1.size() == h2.size());
    const auto last = static_cast<std::size_t>(horizon);
    CHECK(replay_interval_hash(h1, 0, last) == replay_interval_hash(h2, 0, last));
}
