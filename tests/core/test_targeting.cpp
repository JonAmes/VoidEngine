// test: assist_deflection + assist_refine_wells — 9.17 targeting computer
// (PLAN: analytic first pass, ghost sim refines near wells; lock time and
// deflection quality scale with pilot stats).
//
// assist_deflection: the lead quadratic pinned bit-exact against an
// independent Python port of the SAME expression grouping (all ==, no
// Approx — the 9.16 hand-port discipline). Closed-form cross-checks:
// C1 r⊥w -> T = sqrt(r.r/(m.m-w.w)) = sqrt(25/18), sin(lead) = 1/3·cos...;
// C2 mirrors 9.8 A1 (sin lead = 10/800 = 0.0125 EXACT — same lead angle the
// hand-computed 9.8 intercept flew); no-root legs (faster target, receding
// at shell speed) and the linear A==0 leg (approaching at shell speed).
// Lock machine: required = ceil(60/q) ticks (q=1 -> 59; q=0.25 -> 239);
// missing/0/NaN quality REFUSES (fail-safe); >1 clamps; re-engage restarts.
// Rocket designation: window-gated, in-window matches the same quadratic
// from the dart's current state (R0 pins).
//
// assist_refine_wells: void no-op law (empty world -> refined aim
// BIT-IDENTICAL to the analytic aim — correction never fires on fp noise);
// in-well scenario (smoothstep well, g(400)=8.4375 hand-profile) where the
// analytic aim misses and ghost refinement improves the intercept; iters=0
// returns the flat aim untouched; computer gate: nullopt before lock,
// improved aim after.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/rails.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/targeting.h>

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

deflection_inputs inputs(dvec3 s, dvec3 sv, dvec3 t, dvec3 tv, double m) {
    return {/*pos*/s, /*vel*/sv, /*tgt*/t, /*tgtv*/tv, /*m*/m};
}

rail_world void_world() { return rail_world{}; }

rail_world test_world() {  // same body as 9.8's pin set
    rail_world w;
    body_def pl{};
    pl.surface_radius = 100.0;
    pl.well_taper_start = 200.0;
    pl.well_radius = 1000.0;
    pl.surface_g = 10.0;
    REQUIRE(w.add_body(pl).has_value());
    return w;
}

rigid_state ship_at(dvec3 p, dvec3 v = {}) {
    rigid_state s;
    s.pos = p;
    s.vel = v;
    return s;
}

// Independent verifier fly (test side, mirrors the sim-only loop): closest
// approach of a real round to the constant-velocity target model.
struct fly_miss {
    double miss = std::numeric_limits<double>::infinity();
    bool ok = false;
};
fly_miss sim_miss(const deflection_inputs& in, dvec3 aim, const rail_world& w,
                  tick_t from, std::size_t horizon) {
    fly_miss fm;
    auto pr = spawn_projectile(ship_at(in.shooter_pos, in.shooter_vel), aim,
                               in.shell_m_s, from, static_cast<tick_t>(horizon), -1);
    if (!pr) return fm;
    dvec3 prev = pr->state.pos;
    for (std::size_t n = 1; n <= horizon; ++n) {
        const auto res = projectile_step(w, *pr, from + static_cast<tick_t>(n - 1), 1,
                                         std::span<const comp_bounds>{});
        const double t0 = static_cast<double>(n - 1) * kSim_tick_dt;
        const dvec3 dp = prev - (in.target_pos + in.target_vel * t0);
        const dvec3 dv = pr->state.vel - in.target_vel;
        const double dv2 = dot(dv, dv);
        double tau = dv2 > 0.0 ? -(dot(dp, dv)) / dv2 : 0.0;
        if (tau < 0.0) tau = 0.0;
        if (tau > kSim_tick_dt) tau = kSim_tick_dt;
        const double d = length(dp + dv * tau);
        if (d < fm.miss) { fm.miss = d; fm.ok = true; }
        prev = pr->state.pos;
        if (res.life != projectile_life::alive) break;
    }
    return fm;
}

// owns all three pieces; StatsPipeline borrows the first two, so they must
// outlive it (in-place only, never moved/copied).
struct pipe_fixture {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    explicit pipe_fixture(double q) : sp(st, ms) { st.set_base("pilot.gunnery", static_cast<float>(q)); }
};

}  // namespace

TEST_CASE("assist_deflection") {
    // C1) r perpendicular w. Closed form: T = sqrt(1e6/720000) = sqrt(25/18)
    //     = 1.1785113019775793; aim = (1000, 300T)/(900T); deflection
    //     = atan(0.3333/0.94281) = atan(1/sqrt(8)).
    {
        const auto s = solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 300, 0}, 900.0));
        REQUIRE(s.has_value());
        CHECK(s->aim_dir.x == 0.9428090415820634);
        CHECK(s->aim_dir.y == 0.33333333333333337);
        CHECK(s->aim_dir.z == 0.0);
        CHECK(s->tof_s == 1.1785113019775793);
        CHECK(s->range_m == 1000.0);
        CHECK(s->closure_m_s == 0.0);
        CHECK(s->deflection_rad == 0.339836909454122);
        // Hand-check: closed-form T identical to the port's chain.
        CHECK(s->tof_s == std::sqrt(1000000.0 / 720000.0));
    }
    // C2) mirrors 9.8 A1: shooter v=(0,0,5), target v=(0,0,15), muzzle 800,
    //     range 400 -> lead sin = 10/800 = 0.0125 EXACT, and |aim|=1 bit.
    {
        const auto s = solve_deflection(inputs({0, 3000, 0}, {0, 0, 5}, {400, 3000, 0}, {0, 0, 15}, 800.0));
        REQUIRE(s.has_value());
        CHECK(s->aim_dir.z == 0.0125);
        CHECK(s->aim_dir.x == 0.9999218719480037);
        CHECK(s->aim_dir.y == 0.0);
        CHECK(s->tof_s == 0.5000390670782329);
        CHECK(s->deflection_rad == doctest::Approx(std::asin(10.0 / 800.0)).epsilon(1e-9));
        CHECK(length(s->aim_dir) == doctest::Approx(1.0).epsilon(1e-15));
    }
    // C3) target crossing at 2000 m/s > shell 900 -> NO real root.
    CHECK_FALSE(solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 2000, 0}, 900.0)).has_value());
    // C4) receding at shell speed (A==0, B>0) -> T <= 0 -> none.
    CHECK_FALSE(solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {900, 0, 0}, 900.0)).has_value());
    // C5) approaching at shell speed (A==0, B<0): linear root T = 5/9.
    {
        const auto s = solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {-900, 0, 0}, 900.0));
        REQUIRE(s.has_value());
        CHECK(s->aim_dir.x == 1.0);
        CHECK(s->tof_s == 0.5555555555555556);
        CHECK(s->closure_m_s == 900.0);
    }
    // C6) closing + crossing: closure readout = +300 (range DECREASE).
    {
        const auto s = solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {-300, 60, 0}, 800.0));
        REQUIRE(s.has_value());
        CHECK(s->aim_dir.x == 0.9971835337589569);
        CHECK(s->aim_dir.y == 0.07500000000000001);
        CHECK(s->tof_s == 0.9109568576265834);
        CHECK(s->closure_m_s == 300.0);
        CHECK(s->deflection_rad == 0.07507049107671225);
    }
    // C8) receding along LOS slower than shell: T = 1000/(800-300) = 2.0 EXACT,
    //     closure NEGATIVE (opening), zero deflection.
    {
        const auto s = solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {300, 0, 0}, 800.0));
        REQUIRE(s.has_value());
        CHECK(s->tof_s == 2.0);
        CHECK(s->aim_dir.x == 1.0);
        CHECK(s->closure_m_s == -300.0);
        CHECK(s->deflection_rad == 0.0);
    }
    // C9) both roots positive (target FASTER than shell but initially
    //     closing): smallest positive root wins — the first crossing.
    //     A = 50000, B = -600000, C = 1e6: roots 2 and 10 (all exact dyadic).
    {
        const auto s9 = solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {-300, 0, 0}, 200.0));
        REQUIRE(s9.has_value());
        CHECK(s9->tof_s == 2.0);
        CHECK(s9->aim_dir.x == 1.0);
        CHECK(s9->closure_m_s == 300.0);
        CHECK(s9->deflection_rad == 0.0);
    }
    // Refusals: garbage in -> no solution, never a wild aim.
    CHECK_FALSE(solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 800.0)).has_value()); // co-located
    CHECK_FALSE(solve_deflection(inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 0, 0}, 0.0)).has_value()); // no shell speed
    {
        auto in = inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 0, 0}, 800.0);
        in.target_pos.y = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(solve_deflection(in).has_value());
    }

    // ---- lock machine (stats scale lock time) ----
    {
        StatsSet st;
        pipe_fixture pf{1.0};
        targeting_computer tc;
        tc.component = 7;
        tc.pipe = &pf.sp;
        tc.quality_attr = "pilot.gunnery";

        CHECK_FALSE(targeting_lock_ready(tc, 0));  // never engaged
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_lock_ready(tc, 58));
        CHECK(targeting_lock_ready(tc, 59));       // required = 60 ticks
        const auto in = inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 300, 0}, 900.0);
        CHECK_FALSE(targeting_solution(tc, in, void_world(), 58, 300).has_value());
        const auto sol = targeting_solution(tc, in, void_world(), 59, 300);
        REQUIRE(sol.has_value());
        CHECK(sol->aim_dir == solve_deflection(in)->aim_dir);  // void: pass 2 no-op

        // re-engage (target swap) restarts the clock
        targeting_engage(tc, 100);
        CHECK_FALSE(targeting_lock_ready(tc, 158));
        CHECK(targeting_lock_ready(tc, 159));
    }
    {   // q = 0.25 -> required = 240 ticks (twice the lock)
        pipe_fixture pf{0.25};
        targeting_computer tc{7, &pf.sp, "pilot.gunnery", -1};
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_lock_ready(tc, 238));
        CHECK(targeting_lock_ready(tc, 239));
    }
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        pipe_fixture pf{bad};
        targeting_computer tc{7, &pf.sp, "pilot.gunnery", -1};
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_lock_ready(tc, 100000));  // fail-safe: never
        CHECK_FALSE(targeting_solution(tc, inputs({0, 0, 0}, {0, 0, 0}, {1000, 0, 0}, {0, 300, 0}, 900.0),
                                       void_world(), 100000, 300).has_value());
    }
    {   // attr absent entirely = refuse too; q > 1 clamps to the q=1 timeline
        StatsSet st;
        ModifierStack ms;
        StatsPipeline sp(st, ms);  // no attrs at all
        targeting_computer tc{7, &sp, "pilot.gunnery", -1};
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_lock_ready(tc, 100000));
    }
    {
        pipe_fixture pf{5.0};  // >1 sanitized, not faster-than-instant
        targeting_computer tc{7, &pf.sp, "pilot.gunnery", -1};
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_lock_ready(tc, 58));
        CHECK(targeting_lock_ready(tc, 59));
    }

    // ---- rocket designation assist ----
    {
        auto dart = launch_rocket(ship_at({0, 0, 0}), {1.0, 0.0, 0.0}, 300.0, 60, 50.0, 1000.0, 600, 0, 1);
        REQUIRE(dart.has_value());
        const auto d = rocket_designation(*dart, 0, {600, 0, 0}, {0, 200, 0});
        REQUIRE(d.has_value());
        // R0 pins: solve from dart state at launch (v = (300,0,0), m = 300):
        CHECK(d->aim_dir.x == 0.7453559924999297);
        CHECK(d->aim_dir.y == 0.6666666666666666);
        CHECK(d->tof_s == 1.1458980337503155);
        CHECK(d->closure_m_s == 300.0);
        CHECK(length(d->aim_dir) == doctest::Approx(1.0).epsilon(1e-15));
        // Window law: closed boost window -> assist says nothing (9.9 drift).
        auto late = *dart;
        late.born = -100;  // window [-100, -40): now=0 outside
        CHECK_FALSE(rocket_designation(late, 0, {600, 0, 0}, {0, 200, 0}).has_value());
        auto still = *dart;
        still.state.vel = {0.0, 0.0, 0.0};
        CHECK_FALSE(rocket_designation(still, 0, {600, 0, 0}, {0, 200, 0}).has_value());
        // Window half-open [born, born+boost): last in-window tick works, first
        // out-window tick refuses (9.9: outside boost, aim commands are drift).
        CHECK(rocket_designation(*dart, 59, {600, 0, 0}, {0, 200, 0}).has_value());
        CHECK_FALSE(rocket_designation(*dart, 60, {600, 0, 0}, {0, 200, 0}).has_value());
    }
}

TEST_CASE("assist_refine_wells") {
    // C7 scenario: shot ACROSS the well mouth at y=400 (inside taper 200 <
    // r < 1000: g(400) = 10*(1 - s(0.25)) = 10*0.84375 = 8.4375, hand
    // profile), muzzle 400 -> T ~ 2.0025 s, gravity bends the arc and the
    // flat analytic aim misses; ghost refinement must improve the intercept.
    const auto in = inputs({-400, 400, 0}, {0, 0, 0}, {400, 400, 0}, {0, 0, 20}, 400.0);
    const auto flat = solve_deflection(in);
    REQUIRE(flat.has_value());
    CHECK(flat->aim_dir.x == 0.9987492177719088);
    CHECK(flat->tof_s == 2.0025046972870357);

    // A) VOID NO-OP: empty world -> the correction loop never fires (sim miss
    //    is fp noise around the flat path) -> aim BIT-IDENTICAL to analytic.
    {
        const auto r = refine_deflection(in, *flat, void_world(), 0, 300, 8);
        CHECK(r.aim_dir.x == flat->aim_dir.x);
        CHECK(r.aim_dir.y == flat->aim_dir.y);
        CHECK(r.aim_dir.z == flat->aim_dir.z);
        CHECK(sim_miss(in, r.aim_dir, void_world(), 0, 300).miss < 1e-3);
    }
    // B) iters=0: flat pass returned untouched (q=0 floor behavior).
    {
        const auto r = refine_deflection(in, *flat, test_world(), 0, 300, 0);
        CHECK(r.aim_dir == flat->aim_dir);
    }
    // C) IN THE WELL: flat misses by metres; refinement cuts the miss and
    //    the RETURNED aim is the best candidate (deflection changed).
    {
        const rail_world w = test_world();
        const auto fm = sim_miss(in, flat->aim_dir, w, 0, 300);
        REQUIRE(fm.ok);
        CHECK(fm.miss > 1.0);  // gravity really does move this shot

        const auto r = refine_deflection(in, *flat, w, 0, 300, 8);
        const auto rm = sim_miss(in, r.aim_dir, w, 0, 300);
        REQUIRE(rm.ok);
        CHECK(rm.miss < fm.miss);            // refinement improves (never worse)
        CHECK(rm.miss < 1e-3);               // converges to the model, not just nearer
        const bool corrected = (r.aim_dir.y != flat->aim_dir.y) || (r.aim_dir.z != flat->aim_dir.z);
        CHECK(corrected);  // correction active
        CHECK(length(r.aim_dir) == doctest::Approx(1.0).epsilon(1e-12));
        CHECK(r.range_m == 800.0);           // readouts recomputed, candidate-independent
        CHECK(r.closure_m_s == 0.0);
        CHECK(r.deflection_rad > 0.0);
    }
    // D) Computer gate end-to-end in the well: nullopt before lock; after
    //    lock, the improved aim.
    {
        pipe_fixture pf{1.0};
        targeting_computer tc{7, &pf.sp, "pilot.gunnery", -1};
        const rail_world w = test_world();
        targeting_engage(tc, 0);
        CHECK_FALSE(targeting_solution(tc, in, w, 30, 300).has_value());
        const auto sol = targeting_solution(tc, in, w, 59, 300);
        REQUIRE(sol.has_value());
        const auto sm = sim_miss(in, sol->aim_dir, w, 0, 300);
        REQUIRE(sm.ok);
        CHECK(sm.miss < sim_miss(in, flat->aim_dir, w, 0, 300).miss);
    }
    // E) bit-determinism: two identical refines agree bit-for-bit.
    {
        const rail_world w = test_world();
        const auto r1 = refine_deflection(in, *flat, w, 0, 300, 8);
        const auto r2 = refine_deflection(in, *flat, w, 0, 300, 8);
        CHECK(r1.aim_dir.x == r2.aim_dir.x);
        CHECK(r1.aim_dir.y == r2.aim_dir.y);
        CHECK(r1.aim_dir.z == r2.aim_dir.z);
        CHECK(r1.tof_s == r2.tof_s);
    }
}
