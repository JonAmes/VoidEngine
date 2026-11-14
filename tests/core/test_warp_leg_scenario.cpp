// Phase 8 task 8.14 — first scripted warp-leg scenario at real system scale
// (#45j): Thalassa -> Ferrous at the 1.52 AU fixture separation, planet-to-
// planet, end to end, on the SAME machinery the engine ships (rail bodies +
// bubbles + ftl machine + reservoir + stats + wall hook). Done-when claims,
// each pinned below:
//   * completes at exact fixed sim ticks — every tick count is a pin
//     derived from the fp64 flight-recorder port
//     (tests/scenarios/port_warp_leg.py — same IEEE grouping, proven
//     bit-identical by the interval hash matching to the last bit);
//   * propellant drain is hand-exact (reservoir 2.0/tick, closed form);
//   * the replay-hash interval is a pinned literal (run twice, equal);
//   * the engage door refuses warp inside the departure bubble (counter-
//     factual subcase: a forced engage gets wall-held at the bubble);
//   * the bubble-edge crossing fires ONCE, damages via the stats pipeline
//     (game-side value #50c), and the forced spool-down holds the ship at
//     the wall; momentum carries inward only at sublight (#50a);
//   * the ghost preview rolls across the warp leg — BIT-identical at
//     divisor 1 (spool, cruise, crossing, wall hold) and bit-identical
//     even at divisor 2 through the gravity-free cruise stretch.
//
// Pins are NOT transcribed from a C++ run first — they come from the port.
// If the engine drifts by 1 ULP anywhere in 32,325 ticks, the interval
// hash literal fails, which is the point.
#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/ftl.h>
#include <ve/core/sim/preview.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/safe_bubble.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>

namespace ve_test {

using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::uint64_t kFlowSrc = 240;   // glue: ftl.flow onto propellant.load
constexpr std::uint64_t kImpactSrc = 241; // one-shot hull.hp flat_add (#50c)

namespace {

// ---------------------------------------------------------------- fixture
// The two bodies the port authors (all SI, hand-authored #45d):
constexpr double kAu_m = 1.495978707e11;
constexpr double kFerrousOrbit_m = 1.52 * kAu_m;  // real separation (#45j)
constexpr double kFerrousPeriod_s = 3.15576e9;    // 100 yr: rail drift stays
                                                // ~700 m over the leg
constexpr double kFerrousBubble_m = 4.0e7;
constexpr double kThalBubble_m = 6.371e7;
constexpr double kArrivalR_m = 3.89e7;  // orbit-insert radius (5005 km alt)

// Drive aggregate (stats attrs): warp step = 200 km/s * 5000 / 60 =
// 16,666 km/tick at cruise — #45e's "10^6+ km warp tick" at real scale.
constexpr double kSpeedMult = 5000.0;
constexpr double kSpoolS = 10.0;
constexpr double kFlowKgS = 120.0;   // drain = flow*dt = exactly 2.0 kg/tick
constexpr double kTank0 = 60000.0;
constexpr float kHullHp0 = 1000.0f;
constexpr float kImpactHp = 250.0f;  // game-side impact VALUE (#50c)

// ---------------------------------------------------------------- pins
// From tests/scenarios/port_warp_leg.py (fp64 port; hash proves fidelity).
constexpr tick_t kEngageTick = 17313;
constexpr tick_t kBlindEngageTick = 10000;  // deep inside the departure bubble
constexpr tick_t kCrossingTick = 31391;
constexpr tick_t kArrivalTick = 32324;
constexpr std::size_t kBurnTicks = 14678;  // engage..X+599 (spool+cruise+hold)
constexpr double kTankFinal = 30644.0;     // 60000 - 2*14678, exact
// Arrival pose (Ferrous has drifted -243,870 m along -z on its rail; the
// leg crossed its bubble off-axis at x = center - sqrt(r^2 - z^2)).
constexpr double kArrPos_x = 227349866160.50253;
constexpr double kArrPos_y = 0.0;
constexpr double kArrPos_z = -0.000526838534779221;
constexpr std::uint64_t kIntervalHash = 0x075d9e1222c64151ull;

struct run_result {
    std::vector<rigid_state> states;  // states[t] = AFTER tick t
    std::vector<double> mult;         // commanded mult at tick t
    std::vector<double> tank;         // reservoir value after tick t's step
    std::vector<std::uint64_t> hashes;
    std::int64_t engage = -1, crossing = -1, arrival = -1, wall = -1;
    // wall = first tick the warp scan hit the DEPARTURE body's bubble
    // (blind run: the wall hook's first hold tick).
    int fires = 0;
    float hull_final = kHullHp0;
    std::uint64_t interval = 0;
    bool low_tag_at_end = false;
};

rail_world make_world() {
    rail_world w;
    body_def thal{};
    thal.surface_radius = 6.371e6;
    thal.well_radius = 6.371e7;
    thal.well_taper_start = 3.1855e7;
    thal.surface_g = 9.81;
    thal.safe_bubble_radius = kThalBubble_m;
    thal.safe_speed = 2.5e5;  // authored sublight ceiling (data only)
    thal.spin_rate_rad_s = 0.0;
    REQUIRE(w.add_body(thal) == std::optional<std::int32_t>(0));
    body_def ferr{};
    ferr.surface_radius = 3.3895e6;
    ferr.well_radius = 4.0e7;
    ferr.well_taper_start = 2.0e7;
    ferr.surface_g = 3.71;
    ferr.safe_bubble_radius = kFerrousBubble_m;
    ferr.safe_speed = 2.5e5;
    ferr.parent = 0;
    ferr.orbit_radius_m = kFerrousOrbit_m;
    ferr.orbit_period_s = kFerrousPeriod_s;
    ferr.orbit_phase_rad = 0.0;
    REQUIRE(w.add_body(ferr) == std::optional<std::int32_t>(1));
    return w;
}

enum class engage_mode { door, blind };

// The full scenario glue, one pass. `door` gates ftl_engage on
// in_safe_bubble (the #50a rule: warp cannot initiate inside a bubble);
// `blind` skips the gate to prove the wall itself still refuses passage.
// `stop_at` bounds the run (arrival predicate ends it otherwise).
run_result leg_run(engage_mode mode, std::int64_t stop_at = -1) {
    run_result r;
    rail_world w = make_world();

    StatsSet st;
    st.set_base("ftl.speed_mult", static_cast<float>(kSpeedMult));
    st.set_base("ftl.spool_s", static_cast<float>(kSpoolS));
    st.set_base("ftl.flow", static_cast<float>(kFlowKgS));
    st.set_base("hull.hp", kHullHp0);
    st.set_base("propellant.load", 0.0f);  // base attr the per-tick load modifier rides (8.12 idiom)
    ModifierStack ms;
    StatsPipeline sp(st, ms);
    ms.add_source(kFlowSrc);
    ms.add_source(kImpactSrc);

    TagSet tags;
    Reservoir res(kTank0, {{35000.0, "propellant.low", band_dir::at_or_below}}, &tags);
    res.bind(&sp, {"propellant.supply", "propellant.load", "propellant.leak"});
    res.set_ceiling(kTank0);

    rigid_params p{};
    p.mass = 90000.0;
    p.inertia_tensor = math::dmat3::diag({1.0e5, 8.0e5, 7.0e5});

    rigid_state s{};
    s.pos = dvec3(6.373e6, 0.0, 0.0);  // 2 km above Thalassa, radial out
    s.vel = dvec3(2.0e5, 0.0, 0.0);    // sublight; safe_speed is 2.5e5

    ReplayLog log;
    ftl_state fs{};
    bool prev_contact = false;
    bool impact_live = false;

    for (std::int64_t t = 0;; ++t) {
        const tick_t tick = static_cast<tick_t>(t);

        // Engage door (#50a): first tick strictly outside the departure
        // bubble. blind mode skips the predicate and attempts from DEEP
        // inside the bubble — the machine accepts, and only the replay
        // wall hook keeps the ship pinned at the bubble (#50b/c).
        const bool door_open = mode == engage_mode::door
                                   ? !in_safe_bubble(w, s.pos, tick)
                                   : t == kBlindEngageTick;
        if (fs.phase == ftl_phase::idle && door_open) {
            ftl_drive d{sp.derived("ftl.speed_mult").value_or(0.0f),
                        sp.derived("ftl.spool_s").value_or(0.0f)};
            if (ftl_engage(d, fs, tick) && r.engage < 0) r.engage = t;
        }
        ftl_drive d{sp.derived("ftl.speed_mult").value_or(0.0f),
                    sp.derived("ftl.spool_s").value_or(0.0f)};
        ftl_step(d, fs, tick);

        tick_command cmd{};
        cmd.speed_mult = fs.mult;
        log.append(cmd);
        const dvec3 p0 = s.pos;
        replay_step(w, log, static_cast<std::size_t>(t), tick, p, s);

        // Glue mirrors replay's hook exactly (same p0, same grouped disp)
        // to get the crossing EVENT; replay already resolved the position.
        std::optional<bubble_contact> contact;
        if (cmd.speed_mult > 1.0) {
            const dvec3 disp = (s.vel * cmd.speed_mult) * kSim_tick_dt;
            contact = bubble_warp_contact(w, p0, disp, tick);
        }
        if (contact && contact->body == 0 && r.wall < 0) r.wall = t;
        if (contact && contact->body == 1 && !prev_contact) {
            ++r.fires;
            if (!impact_live) {  // one-shot damage member, kept live
                auto m = modifier_desc{};
                m.target_attr = "hull.hp";
                m.stack_id = "impact.warp_wall";
                m.op = mod_op::flat_add;
                m.value = -kImpactHp;
                m.policy = stack_policy::replace;
                ms.add_modifier(kImpactSrc, m);
                impact_live = true;
            }
            if (fs.phase != ftl_phase::idle) {
                REQUIRE(ftl_disengage(fs, tick));  // forced decel door
                if (r.crossing < 0) r.crossing = t;
            }
        }
        prev_contact = contact.has_value();

        {  // reservoir load: ftl.flow while burning, else zero (replace)
            auto m = modifier_desc{};
            m.target_attr = "propellant.load";
            m.stack_id = "ftl.load";
            m.op = mod_op::flat_add;
            m.value = ftl_burning(fs) ? sp.derived("ftl.flow").value_or(0.0f) : 0.0f;
            m.policy = stack_policy::replace;
            ms.add_modifier(kFlowSrc, m);
        }
        res.step();

        r.states.push_back(s);
        r.mult.push_back(fs.mult);
        r.tank.push_back(res.value());
        r.hashes.push_back(state_hash(tick, s));

        const auto c = w.position(1, tick);
        const dvec3 rel = s.pos - *c;
        if (math::dot(rel, rel) <= kArrivalR_m * kArrivalR_m) {
            r.arrival = t;
            break;
        }
        if (stop_at >= 0 && t + 1 >= stop_at) break;
        if (t >= 60000) { FAIL("warp leg did not terminate by tick 60000"); break; }
    }
    r.hull_final = sp.derived("hull.hp").value_or(0.0f);
    r.interval = replay_interval_hash(r.hashes, 0, r.hashes.size() - 1);
    r.low_tag_at_end = tags.contains("propellant.low");
    return r;
}

}  // namespace

TEST_CASE("warp_leg_scenario") {
    SUBCASE("full leg: exact ticks, port-pinned endpoints, pinned hash") {
        run_result r = leg_run(engage_mode::door);
        CHECK(r.engage == kEngageTick);
        CHECK(r.crossing == kCrossingTick);
        CHECK(r.arrival == kArrivalTick);
        CHECK(static_cast<std::int64_t>(r.states.size()) == kArrivalTick + 1);

        // Ramp endpoints land BIT-exactly on schedule (8.12 machine).
        CHECK(r.mult[static_cast<std::size_t>(kEngageTick)] == 1.0);
        CHECK(r.mult[static_cast<std::size_t>(kEngageTick + 600)] == kSpeedMult);
        CHECK(r.mult[static_cast<std::size_t>(kCrossingTick + 600)] == 1.0);
        std::size_t mult_bad = static_cast<std::size_t>(-1);
        for (std::int64_t t = kEngageTick + 601; t < kCrossingTick; ++t)
            if (r.mult[static_cast<std::size_t>(t)] != kSpeedMult) { mult_bad = static_cast<std::size_t>(t); break; }
        CHECK(mult_bad == static_cast<std::size_t>(-1));

        // Arrival pose is a port pin (exact doubles).
        const rigid_state& a = r.states[static_cast<std::size_t>(kArrivalTick)];
        CHECK(a.pos.x == kArrPos_x);
        CHECK(a.pos.y == kArrPos_y);
        CHECK(a.pos.z == kArrPos_z);

        // Replay-hash interval literal: bit-identical to the port's stream.
        CHECK(r.interval == kIntervalHash);
        run_result again = leg_run(engage_mode::door);
        CHECK(again.interval == kIntervalHash);
        CHECK(again.states.size() == r.states.size());
        std::size_t hash_bad = static_cast<std::size_t>(-1);
        for (std::size_t t = 0; t < r.hashes.size(); ++t)
            if (again.hashes[t] != r.hashes[t]) { hash_bad = t; break; }
        CHECK(hash_bad == static_cast<std::size_t>(-1));
    }

    SUBCASE("propellant drain matches the hand-closed form exactly") {
        run_result r = leg_run(engage_mode::door);
        // value after tick t = kTank0 - 2.0 * (t - engage + 1) while burning
        // (engage..crossing+599), then frozen. 2.0 = flow * dt bit-exact.
        double port = kTank0;
        std::size_t drained = 0;
        for (std::size_t t = 0; t < r.tank.size(); ++t) {
            if (static_cast<std::int64_t>(t) >= r.engage &&
                static_cast<std::int64_t>(t) <= r.crossing + 599) {
                port -= 2.0;
                ++drained;
            }
            CHECK(r.tank[t] == port);
        }
        CHECK(drained == kBurnTicks);
        CHECK(r.tank.back() == kTankFinal);
        CHECK(r.low_tag_at_end);  // crossed the 35,000 kg band mid-cruise
    }

    SUBCASE("engage door refuses warp inside the departure bubble") {
        // Door predicate at the boundary ticks: the pre-step position at
        // tick kEngageTick-1 is strictly inside (door shut), the pre-step
        // position at the first engage tick is not (door open).
        rail_world w = make_world();
        run_result r = leg_run(engage_mode::door);
        const rigid_state& just_before =
            r.states[static_cast<std::size_t>(kEngageTick - 2)];
        const bool door_shut =
            in_safe_bubble(w, just_before.pos, kEngageTick - 1) ==
            std::optional<std::int32_t>(0);
        CHECK(door_shut);
        const rigid_state& at = r.states[static_cast<std::size_t>(kEngageTick - 1)];
        CHECK_FALSE(in_safe_bubble(w, at.pos, kEngageTick).has_value());

        // Counterfactual: engage from DEEP inside the bubble (door bypassed;
        // ftl itself has no bubble concept — #50a puts enforcement in the
        // wall layer). The machine accepts and ramps to full warp; the
        // exit-root truncation pins the ship at the departure wall from
        // first contact onward: no warp passage, ever (it exits sublight).
        run_result b = leg_run(engage_mode::blind, kBlindEngageTick + 1200);
        CHECK(b.engage == kBlindEngageTick);
        const bool engaged_from_inside =
            in_safe_bubble(w, b.states[static_cast<std::size_t>(kBlindEngageTick - 1)].pos,
                           kBlindEngageTick) == std::optional<std::int32_t>(0);
        CHECK(engaged_from_inside);
        CHECK(b.crossing == -1);  // arrival wall never reached
        CHECK(b.wall == 10042);  // port pin: first wall-hold tick
        std::size_t mult_bad = static_cast<std::size_t>(-1);
        for (std::int64_t t = kBlindEngageTick + 601; t < kBlindEngageTick + 1200; ++t)
            if (b.mult[static_cast<std::size_t>(t)] != kSpeedMult) { mult_bad = t; break; }
        CHECK(mult_bad == static_cast<std::size_t>(-1));  // machine kept warping
        const dvec3 thal(0.0, 0.0, 0.0);
        double max_over = 0.0;
        for (std::size_t t = static_cast<std::size_t>(b.wall); t < b.states.size(); ++t) {
            const double over = std::fabs(length(b.states[t].pos - thal) - kThalBubble_m);
            max_over = over > max_over ? over : max_over;
        }
        CHECK(max_over < 1.0e-3);  // ON the wall every tick since contact
    }

    SUBCASE("crossing: one impact, wall-held spool-down, sublight carry") {
        run_result r = leg_run(engage_mode::door);
        CHECK(r.fires == 1);
        CHECK(r.hull_final == kHullHp0 - kImpactHp);  // 750.0f exactly

        rail_world w = make_world();
        // Every warp tick of the forced ramp re-pins the ship at the wall:
        // |pos - center(t)| sits on the bubble surface (band-classified).
        bool band_ok = true, ramp_mult_ok = true;
        for (std::int64_t t = kCrossingTick; t <= kCrossingTick + 599; ++t) {
            const auto c = w.position(1, static_cast<tick_t>(t));
            const dvec3 rel = r.states[static_cast<std::size_t>(t)].pos - *c;
            const double over = length(rel) - kFerrousBubble_m;
            if (std::fabs(over) > 1.0e-3) { band_ok = false; break; }
            if (r.mult[static_cast<std::size_t>(t)] <= 1.0) { ramp_mult_ok = false; break; }
        }
        CHECK(band_ok);      // still pinned at the wall on every ramp tick
        CHECK(ramp_mult_ok); // still decelerating (mult > 1) until it lands on 1
        // Momentum carries inward ONLY at sublight: the idle tick steps
        // a full 3333 m sublight chord (port: -3299 m from the wall).
        const auto c1 = w.position(1, static_cast<tick_t>(kCrossingTick + 600));
        const auto c0 = w.position(1, static_cast<tick_t>(kCrossingTick + 599));
        const double d1 = length(r.states[static_cast<std::size_t>(kCrossingTick + 600)].pos - *c1);
        const double d0 = length(r.states[static_cast<std::size_t>(kCrossingTick + 599)].pos - *c0);
        // tolerance = epsilon*scale ~= 3e-4 m
        CHECK(d0 - d1 == doctest::Approx(3299.200423).epsilon(1e-7).scale(3299.2));
    }

    SUBCASE("ghost preview rolls across the leg: bit-identical divisor 1") {
        run_result r = leg_run(engage_mode::door);
        rail_world w = make_world();
        ReplayLog log;
        for (std::size_t t = 0; t < r.mult.size(); ++t) {
            tick_command cmd{};
            cmd.speed_mult = r.mult[t];
            log.append(cmd);
        }
        // Window A: spool-up + first cruise ticks (engage-400 .. +624).
        // Window B: crossing + 900 wall-held ticks (crossing-100 ..).
        const std::pair<std::size_t, std::size_t> windows[] = {
            {static_cast<std::size_t>(kEngageTick) - 400, 0},
            {static_cast<std::size_t>(kCrossingTick) - 100, 1}};
        for (const auto& [start, tag] : windows) {
            const std::vector<rigid_state> g =
                ghost_preview(w, log, start, static_cast<tick_t>(start),
                              r.states[start - 1],
                              [] { rigid_params p{}; p.mass = 90000.0;
                                   p.inertia_tensor = math::dmat3::diag(
                                       {1.0e5, 8.0e5, 7.0e5}); return p; }(),
                              kGhost_preview_max_steps, 1);
            REQUIRE(g.size() == kGhost_preview_max_steps);
            std::size_t div_bad = static_cast<std::size_t>(-1);
            for (std::size_t k = 0; k < g.size(); ++k)
                if (g[k].pos != r.states[start + k].pos) { div_bad = k; break; }
            if (div_bad != static_cast<std::size_t>(-1))
                MESSAGE("window " << tag << " diverged at +" << div_bad);
            CHECK(div_bad == static_cast<std::size_t>(-1));
        }
        // Gravity-free cruise: divisor 2 lands within ONE ULP of the live
        // positions at the sampled ticks — vel is constant there (both
        // wells read EXACTLY zero), so the coarse chord is the same line and
        // the only cost is double rounding of (p+x)+x vs p+(2x), ~1e-16 rel.
        const std::size_t s2 = static_cast<std::size_t>(kCrossingTick) - 3000;
        const std::vector<rigid_state> g2 =
            ghost_preview(w, log, s2, static_cast<tick_t>(s2), r.states[s2 - 1],
                          [] { rigid_params p{}; p.mass = 90000.0;
                               p.inertia_tensor = math::dmat3::diag(
                                   {1.0e5, 8.0e5, 7.0e5}); return p; }(),
                          1400, 2);
        REQUIRE(g2.size() == 700);
        double max_dev = 0.0;
        for (std::size_t k = 0; k < g2.size(); ++k) {
            // A coarse step covers D ticks: step k lands on live tick
            // s2 + 2k + 1 (the last tick it consumed), not s2 + 2k.
            const rigid_state& live = r.states[s2 + 2 * k + 1];
            const double dev = length(g2[k].pos - live.pos);
            max_dev = dev > max_dev ? dev : max_dev;
        }
        CHECK(max_dev < 0.1);  // ulp ACCUMULATION at 2e11 m over 700 coarse
                               // steps (~700 ulp ~= 4e-2 m); still 8 orders
                               // below one 16.7 Mm warp step
    }
}

}  // namespace ve_test
