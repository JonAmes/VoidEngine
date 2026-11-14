// Phase 8 task 8.15 — time compression + physics LOD (decision #53).
// Observer-relative distance bands (apply_bands contract) -> per-body
// integer step divisor; the coarse step samples gravity/assists/warp-mult
// ONCE at interval start and integrates dt = the elapsed tick gap via the
// sanctioned integrate_step_warp_dt path (#53b). Pins, per #53:
//   1. a divisor-1 body under the LOD spine is the reference run
//      BIT-FOR-BIT — the scheduler is not a second physics implementation
//      (same recorded-command spine, same wall hook, dt = kSim_tick_dt);
//   2. the coarse lattice is pure integer state: a closed-form walk over
//      the recorded divisor schedule matches the live scheduler's
//      next-due/phase at every tick; a divisor drop mid-interval resumes
//      full rate on the switch tick itself;
//   3. bands are the reservoir_band contract (#53a): the validator
//      rejects non-ascending / out-of-range input; first registered match
//      wins; past every band = divisor 1 (full rate);
//   4. an assisted cruise (autopilot, 8.14's Thalassa -> Ferrous fixture
//      at 1.52 AU) completes under 1000x observer-band compression with
//      bounded error vs the 1x reference — bounded by the authored
//      geometry (arrival window vs the coarse step's worst one-interval
//      sampling lag), pinned numerically — and the far leg's physics
//      steps collapse by >98%;
//   5. a compressed capture stays scrubbable (#53c): the scrubber
//      replays the RECORDED divisor schedule and its per-tick state
//      hashes equal the live compressed run's at every sampled tick.
// Determinism: the compressed run executes twice; hashes, schedule, and
// the emitted stats-event stream must be identical (TSan/atomics: 9.4).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/assist.h>
#include <ve/core/sim/ftl.h>
#include <ve/core/sim/lod.h>
#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/safe_bubble.h>
#include <ve/core/sim/scrubber.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_bus.h>
#include <ve/core/sim/stats_pipeline.h>

namespace ve_test {

using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::uint64_t kFlowSrc = 340;    // glue: propellant.load modifier
constexpr std::uint64_t kImpactSrc = 341;  // one-shot hull.hp flat_add (#50c)
constexpr std::uint64_t kMultSrc = 342;    // bus witness: warp.live_mult
constexpr std::uint64_t kNavSrc = 343;     // bus witness: nav.closing

namespace {

// ------------------------------------------------------------- fixture
// 8.14's Thalassa -> Ferrous world at the 1.52 AU separation, verbatim.
constexpr double kAu_m = 1.495978707e11;
constexpr double kFerrousOrbit_m = 1.52 * kAu_m;
constexpr double kFerrousPeriod_s = 3.15576e9;
constexpr double kFerrousBubble_m = 4.0e7;
constexpr double kThalBubble_m = 6.371e7;
constexpr double kArrivalR_m = 3.89e7;

constexpr double kSpeedMult = 5000.0;
constexpr double kSpoolS = 10.0;
constexpr double kFlowKgS = 120.0;
constexpr double kTank0 = 60000.0;
constexpr float kHullHp0 = 1000.0f;
constexpr float kImpactHp = 250.0f;

// #53a authored LOD bands: within 1000 km-scaled "player can see it"
// radius (1e9 m) of either bubble the ship runs full rate; the whole
// interplanetary leg rides divisor 1000 (16.65 s coarse steps).
constexpr std::uint64_t kFarDiv = 1000;
inline std::vector<lod_band> cruise_bands() {
    return {{1.0e9, 1}, {1.0e18, kFarDiv}};
}

inline rail_world make_world() {
    rail_world w;
    body_def thal{};
    thal.surface_radius = 6.371e6;
    thal.well_radius = 6.371e7;
    thal.well_taper_start = 3.1855e7;
    thal.surface_g = 9.81;
    thal.safe_bubble_radius = kThalBubble_m;
    thal.safe_speed = 2.5e5;
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

inline rigid_params ship_params() {
    rigid_params p{};
    p.mass = 90000.0;
    p.inertia_tensor = dmat3::diag({1.0e5, 8.0e5, 7.0e5});
    return p;
}

enum class spine_mode { reference, lod };

struct leg_result {
    ReplayLog log;                              // tick-dense (#53c)
    std::vector<std::uint64_t> hashes;          // state_hash per tick
    std::vector<rigid_state> states;            // retained state per tick
    std::vector<sim_checkpoint> cps;            // every 400 ticks + tick -1
    std::vector<lod_transition> sched;          // recorded divisor schedule
    std::vector<stat_change> events;            // stats-bus emissions
    std::vector<std::uint8_t> stepped;          // physics-step map
    std::int64_t engage = -1, crossing = -1, arrival = -1;
    std::size_t steps = 0, far_steps = 0;
    std::uint64_t initial_hash = 0;  // the -1 checkpoint (spawn state)
    double tank_final = kTank0;
    bool low_at_end = false;
};

// One full cruise: sublight ejection -> FTL engage (8.12 machine, ramp
// included) -> warp leg (far leg may ride the coarse lattice) ->
// disengage + ramp-down at the Ferrous bubble wall -> sublight arrival.
// The drive/door/reservoir/bus glue runs EVERY tick on retained state
// (input machinery is tick-dense; only physics compresses).
inline leg_result run_leg(const rail_world& w, spine_mode mode,
                          const std::vector<lod_band>& bands,
                          std::int64_t horizon, bool with_bus) {
    StatsSet st;
    st.set_base("ftl.speed_mult", static_cast<float>(kSpeedMult));
    st.set_base("ftl.spool_s", static_cast<float>(kSpoolS));
    st.set_base("ftl.flow", static_cast<float>(kFlowKgS));
    st.set_base("hull.hp", kHullHp0);
    st.set_base("propellant.load", 0.0f);
    st.set_base("warp.live_mult", 0.0f);  // bus witnesses (#53c claim)
    st.set_base("nav.closing", 0.0f);
    ModifierStack ms;
    StatsPipeline sp(st, ms);
    ms.add_source(kFlowSrc);
    ms.add_source(kImpactSrc);
    ms.add_source(kMultSrc);
    ms.add_source(kNavSrc);
    StatsEventBus bus;
    if (with_bus)
        sp.attach_bus(&bus);

    TagSet tags;
    Reservoir res(kTank0, {{35000.0, "propellant.low", band_dir::at_or_below}}, &tags);
    res.bind(&sp, {"propellant.supply", "propellant.load", "propellant.leak"});
    res.set_ceiling(kTank0);

    const rigid_params p = ship_params();
    rigid_state s{};
    s.pos = dvec3(6.373e6, 0.0, 0.0);
    s.vel = dvec3(2.0e5, 0.0, 0.0);

    leg_result r;
    ReplayLog log;
    ftl_state fs{};
    lod_body b{};  // divisor 1, last_step_tick -1 (lattice starts at 0)
    float last_nav = 0.0f;
    bool have_nav = false;
    r.cps.push_back({-1, s});
    r.initial_hash = state_hash(-1, s);

    for (tick_t tick = 0; tick <= horizon; ++tick) {
        if (with_bus)
            bus.begin_tick(tick);

        // #53a: bands re-evaluated EVERY tick against the retained state
        // (the only position the run's other per-tick machinery reads —
        // the #8.9/20c "same source" invariant). Transitions are recorded
        // the moment they happen (#53c).
        if (mode == spine_mode::lod) {
            const double d0 = length(s.pos - *w.position(0, tick));
            const double d1 = length(s.pos - *w.position(1, tick));
            const std::uint64_t want = lod_divisor(std::min(d0, d1), bands);
            if (want != b.divisor) {
                r.sched.push_back({tick, want});
                b.divisor = want;
            }
        }

        // Engage door + machine, per tick, on the retained position
        // (#50c door stays tick-dense even across coarse gaps).
        const ftl_drive d{sp.derived("ftl.speed_mult").value_or(0.0f),
                          sp.derived("ftl.spool_s").value_or(0.0f)};
        if (fs.phase == ftl_phase::idle && !in_safe_bubble(w, s.pos, tick)) {
            if (ftl_engage(d, fs, tick) && r.engage < 0)
                r.engage = tick;
        }
        ftl_step(d, fs, tick);

        tick_command cmd{};
        cmd.assist_mask = kAssist_autopilot | kAssist_stabilizer;
        cmd.autopilot_body = 1;
        cmd.speed_mult = fs.mult;
        log.append(cmd);  // log stays TICK-DENSE (#53c)

        // Physics: reference spine, or the LOD lattice over the SAME
        // spine. lod_replay_step returns false on ticks with no step —
        // the retained state IS the frozen state at those ticks.
        const dvec3 p0 = s.pos;
        bool stepped = false;
        double mult_used = cmd.speed_mult;
        double dt = kSim_tick_dt;
        if (mode == spine_mode::reference) {
            replay_step(w, log, static_cast<std::size_t>(tick), tick, p, s);
            stepped = true;
        } else {
            const tick_t prev_last = b.last_step_tick;
            stepped = lod_replay_step(w, log, tick, p, s, b);
            if (stepped && prev_last >= 0) {
                // the #53b sample the lattice step actually used: the
                // interval-start command's mult, over the elapsed gap.
                mult_used = log.at(static_cast<std::size_t>(prev_last + 1)).speed_mult;
                dt = tick_to_seconds(tick - prev_last);
            }
        }
        if (stepped) {
            ++r.steps;
            if (b.divisor > 1)
                ++r.far_steps;
        }

        // #50 wall hook at retained state — SAME predicate the spine's
        // resolve_warp_bubble_wall used: same p0, the sampled mult, and
        // the step's actual dt (grouping verbatim, (vel * mult) * dt).
        std::optional<bubble_contact> contact;
        if (stepped && mult_used > 1.0)
            contact = bubble_warp_contact(w, p0, (s.vel * mult_used) * dt, tick);
        if (contact && r.crossing < 0 && contact->body != 0) {
            r.crossing = tick;
            auto m = modifier_desc{};
            m.target_attr = "hull.hp";
            m.stack_id = "warp-cross";
            m.op = mod_op::flat_add;
            m.value = -kImpactHp;
            m.policy = stack_policy::exclusive;
            ms.add_modifier(kImpactSrc, m);
            if (fs.phase != ftl_phase::idle)
                ftl_disengage(fs, tick);  // forced at the wall (#50c)
        }

        // Per-tick reservoir load modifier + step (8.12/8.14 idiom).
        {
            auto m = modifier_desc{};
            m.target_attr = "propellant.load";
            m.stack_id = "ftl-load";
            m.op = mod_op::flat_add;
            m.value = ftl_burning(fs) ? static_cast<float>(kFlowKgS * kSim_tick_dt) : 0.0f;
            m.policy = stack_policy::replace;
            ms.add_modifier(kFlowSrc, m);
        }
        // Bus witnesses: live warp mult every tick (mult is INPUT, it
        // keeps ramping in real time across coarse gaps); nav closes only
        // when the retained velocity changes = physics-step ticks.
        {
            auto m = modifier_desc{};
            m.target_attr = "warp.live_mult";
            m.stack_id = "warp-live";
            m.op = mod_op::flat_add;
            m.value = static_cast<float>(fs.mult);
            m.policy = stack_policy::replace;
            ms.add_modifier(kMultSrc, m);
        }
        {
            const float nv = static_cast<float>(length(s.vel));
            if (!have_nav || std::bit_cast<std::uint32_t>(nv) !=
                                 std::bit_cast<std::uint32_t>(last_nav)) {
                auto m = modifier_desc{};
                m.target_attr = "nav.closing";
                m.stack_id = "nav-live";
                m.op = mod_op::flat_add;
                m.value = nv;
                m.policy = stack_policy::replace;
                ms.add_modifier(kNavSrc, m);
                last_nav = nv;
                have_nav = true;
            }
        }
        res.step();
        (void)sp.derived("warp.live_mult");
        (void)sp.derived("nav.closing");
        (void)sp.derived("hull.hp");
        if (with_bus)
            for (const auto& e : bus.drain())
                r.events.push_back(e);

        r.stepped.push_back(stepped ? 1 : 0);
        r.hashes.push_back(state_hash(tick, s));
        r.states.push_back(s);
        if (tick % 400 == 0)
            r.cps.push_back({tick, s});
        if (length(s.pos - *w.position(1, tick)) <= kArrivalR_m) {
            r.arrival = tick;
            break;
        }
    }
    r.tank_final = res.value();
    r.low_at_end = tags.contains_exact("propellant.low");
    r.log = std::move(log);
    return r;
}

}  // namespace

// ---------------------------------------------------------------- pin 3
TEST_CASE("lod_band_contract") {
    // Validator: strictly ascending, finite, in range (#6.9 contract).
    CHECK(lod_bands_ok({}));
    CHECK(lod_bands_ok({{1.0e9, 1}, {1.0e18, kFarDiv}}));
    CHECK_FALSE(lod_bands_ok({{1.0e9, 1}, {1.0e9, 10}}));    // not ascending
    CHECK_FALSE(lod_bands_ok({{1.0e18, 2}, {1.0e9, 1}}));    // unsorted
    CHECK_FALSE(lod_bands_ok({{-5.0, 4}}));                  // negative
    CHECK_FALSE(lod_bands_ok({{std::numeric_limits<double>::infinity(), 4}}));
    CHECK_FALSE(lod_bands_ok({{std::nan(""), 4}}));          // non-finite
    CHECK(lod_bands_ok({{1.0e30, 4}}));                      // finite extreme OK
    CHECK_FALSE(lod_bands_ok({{1.0, 0}}));                   // divisor 0
    CHECK_FALSE(lod_bands_ok({{1.0, kLod_max_divisor + 1}}));

    // First registered match wins; past every band = divisor 1 (full rate
    // is the NO-match payload, mirroring "no tag set" for reservoirs).
    const std::vector<lod_band> bands{{1.0e9, 1}, {1.0e18, 1000}};
    CHECK(lod_divisor(0.0, bands) == 1);
    CHECK(lod_divisor(1.0e9, bands) == 1);        // <= inclusive
    CHECK(lod_divisor(1.0e9 + 1.0, bands) == 1000);
    CHECK(lod_divisor(1.0e18, bands) == 1000);
    CHECK(lod_divisor(1.0e30, bands) == 1);       // no match: full rate
    CHECK(lod_divisor(std::nan(""), bands) == 1);
}

// ---------------------------------------------------------------- pin 2
TEST_CASE("lod_lattice_schedule_closed_form") {
    // #53b: the lattice is pure integer state. Divisors switch mid-
    // interval: 1 -> 1000 at t=100 (the coarse steps then start), 1000 ->
    // 1 at t=5310 — and the CATCH-UP rule ("a divisor drop resumes full
    // rate on the switch tick itself, stepping the whole elapsed gap in
    // one authorized long step") pins at exact ticks. The closed-form
    // walk must equal a live-order hand walk (transition applied at its
    // tick BEFORE that tick's due test) at every single tick.
    const std::vector<lod_transition> sched{{100, 1000}, {5310, 1}};
    tick_t last = -1;
    std::uint64_t n = 0, div = 1, si = 0;
    bool first_far_pinned = false;
    for (tick_t t = 0; t <= 6200; ++t) {
        while (si < sched.size() && sched[si].tick <= t) {
            div = sched[si].divisor;
            ++si;
        }
        if (t - last >= static_cast<tick_t>(div)) {
            last = t;
            ++n;
            if (div > 1 && !first_far_pinned) {
                // First coarse step: last full-rate step was t=99, so
                // the coarse lattice lands on 99 + 1000 exactly (#45a).
                CHECK(t == 1099);
                first_far_pinned = true;
            }
        }
        const lod_lattice L = lod_lattice_at(sched, t);
        CHECK(L.divisor == div);
        CHECK(L.last_step_tick == last);
        CHECK(L.steps_applied == n);
        if (t == 5310)
            CHECK(L.last_step_tick == 5310);  // resumes ON the switch tick
    }
    // 100 full-rate ticks + coarse steps at 1099/2099/3099/4099/5099 +
    // catch-up jump at 5310 + every tick 5311..6200.
    const lod_lattice end = lod_lattice_at(sched, 6200);
    CHECK(end.steps_applied == 100 + 5 + (6200 - 5310 + 1));
    CHECK(end.last_step_tick == 6200);
    CHECK(first_far_pinned);
    // Past the schedule horizon the lattice keeps its phase: t way past
    // 6200 stays full rate (divisor ended at 1).
    const lod_lattice far = lod_lattice_at(sched, 7000);
    CHECK(far.divisor == 1);
    CHECK(far.last_step_tick == 7000);
    CHECK(far.steps_applied == end.steps_applied + 800);
}

// ---------------------------------------------------------------- pins 1+4+5
TEST_CASE("time_compression_lod") {
    rail_world w_ref = make_world();
    rail_world w_a = make_world();

    // 1x reference run (8.14 machinery, full rate every tick).
    const leg_result ref = run_leg(w_ref, spine_mode::reference, {}, 36000, true);
    // 1000x observer-band compressed run.
    const leg_result a = run_leg(w_a, spine_mode::lod, cruise_bands(), 36000, true);

    // ---- scenario completes under compression (done-when claim).
    REQUIRE(a.arrival >= 0);
    REQUIRE(ref.arrival >= 0);
    CHECK(a.engage >= 0);
    CHECK(a.crossing >= 0);

    // ---- Pin 1: divisor-1 prefix is BIT-FOR-BIT the reference. Up to
    // (but not including) the first transition the LOD spine stepped
    // every tick at dt = kSim_tick_dt; every hash must match to the last
    // bit (#53b). AT the transition tick the lattice legitimately skips
    // (divisor just became 1000, gap < divisor): the retained state is
    // the frozen reference state of the previous tick, hashed fresh.
    REQUIRE(!a.sched.empty());
    CHECK(a.sched[0].tick >= a.engage);  // departure is near: full rate
    const std::size_t t0 = static_cast<std::size_t>(a.sched[0].tick);
    for (std::size_t t = 0; t < t0; ++t)
        CHECK(a.hashes[t] == ref.hashes[t]);
    CHECK(a.hashes[t0] == state_hash(static_cast<tick_t>(t0), ref.states[t0 - 1]));
    CHECK(a.engage == ref.engage);  // engage tick is EXACT, not "close"

    // ---- Schedule is a true compression window: near -> far -> near.
    // Divisor starts at 1 (no transition), so exactly two: leg out, arrival in.
    REQUIRE(a.sched.size() == 2);
    CHECK(a.sched[0].divisor == kFarDiv);           // leg goes coarse
    CHECK(a.sched.back().divisor == 1);             // arrival resumes full
    for (std::size_t i = 1; i < a.sched.size(); ++i)
        CHECK(a.sched[i].tick > a.sched[i - 1].tick);

    // ---- Lattice vs closed form at EVERY tick of the live run (pin 2):
    // the live scheduler's (last_step_tick, divisor) equals the recorded-
    // schedule walk, so the schedule fully reconstructs the lattice.
    lod_lattice L{};
    tick_t last = -1;
    std::uint64_t n = 0, si = 0;
    std::size_t far_steps_live = 0;
    for (std::size_t t = 0; t < a.stepped.size(); ++t) {
        while (si < a.sched.size() && a.sched[si].tick <= static_cast<tick_t>(t)) {
            L.divisor = a.sched[si].divisor;
            ++si;
        }
        const bool due = static_cast<tick_t>(t) - last >= static_cast<tick_t>(L.divisor);
        CHECK(a.stepped[t] == (due ? 1 : 0));  // physics-step map exact
        if (due) {
            last = static_cast<tick_t>(t);
            ++n;
            if (L.divisor > 1)
                ++far_steps_live;
        }
    }
    CHECK(far_steps_live == a.far_steps);
    CHECK(a.steps == n);

    // ---- Pin 4: >98% step collapse over the far leg. Far-leg ticks
    // span engage..crossing; the coarse lattice stepped that span only
    // every 1000 ticks (16.65 s coarse steps at 16,666 km/tick).
    const std::int64_t far_leg = a.crossing - a.engage;
    REQUIRE(far_leg > 10000);
    CHECK(a.far_steps >= 5);
    CHECK(a.far_steps * 50 <= static_cast<std::size_t>(far_leg));

    // ---- Pin 4: bounded error vs the 1x reference, bounded by the
    // authored geometry: worst one-interval sampling lag of the ramp at
    // interval start = 1000 ticks x ramp, position converges at the
    // wall pin (wall hold is exact).
    CHECK(std::abs(static_cast<double>(a.crossing - ref.crossing)) <= 2000.0);
    CHECK(std::abs(static_cast<double>(a.arrival - ref.arrival)) <= 3000.0);
    const dvec3 cross_pos = a.states[static_cast<std::size_t>(a.crossing)].pos;
    const double cross_r = length(cross_pos - *w_a.position(1, a.crossing));
    CHECK(cross_r >= kFerrousBubble_m - 1.0e3);   // pinned at the wall
    CHECK(cross_r <= kFerrousBubble_m + 1.0e3);   // never tunneled
    const dvec3 dpos = a.states[static_cast<std::size_t>(a.arrival)].pos -
                       ref.states[static_cast<std::size_t>(ref.arrival)].pos;
    CHECK(length(dpos) <= 1.0e8);  // vs the 4e7 wall radius: convergent

    // ---- The far leg never tunnels: while warp is engaged (engage ->
    // the first contact tick inclusive) the retained state sits outside
    // Ferrous' bubble. AFTER the forced disengage, sublight travel
    // inside the bubble is legal (#50 walls WARP steps only).
    for (std::size_t t = static_cast<std::size_t>(a.engage);
         t <= static_cast<std::size_t>(a.crossing); ++t)
        CHECK(length(a.states[t].pos - *w_a.position(1, static_cast<tick_t>(t))) >=
              kFerrousBubble_m - 1.0e3);

    // ---- Per-tick machinery rode the WHOLE run regardless of divisor:
    // the wall-cross impact landed, and both tanks drained the SAME
    // amount — reservoirs and the FTL load ride the tick-dense command
    // stream, never the physics divisor.
    CHECK(a.crossing >= 0);
    CHECK(a.tank_final < kTank0);
    // Flow rides the tick-dense live drive state, so the tanks can only
    // diverge by the FTL burn window between the two crossings: bounded
    // by |a.crossing - ref.crossing| ticks of kFlowKgS flow (the same
    // bound the geometry error is pinned to above — the divisor itself
    // never touches the reservoir).
    CHECK(std::abs(a.tank_final - ref.tank_final) <=
          kFlowKgS * static_cast<double>(std::abs(a.crossing - ref.crossing)) /
              static_cast<double>(kSim_tick_hz));

    // ---- Pin 5: the compressed capture replays bit-for-bit through the
    // SCRUBBER (recorded schedule + tick-dense log + checkpoints).
    ReplayScrubber scr(w_a, ship_params());
    REQUIRE(scr.load_compressed(a.log, a.cps, a.sched));
    std::vector<tick_t> samples{-1,
                                0,
                                1,
                                a.sched[0].tick,
                                a.sched[0].tick + 1,
                                a.sched[0].tick + 500,  // mid coarse window
                                a.engage - 1,
                                a.engage,
                                a.crossing - 250,
                                a.crossing,
                                a.crossing + 1,
                                a.crossing + 301,  // mid ramp-down
                                a.arrival - 1,
                                a.arrival,
                                a.arrival - 1500};  // BACKWARD jump
    for (const auto& cp : a.cps)
        if (cp.tick >= 0)
            samples.push_back(cp.tick);
    std::sort(samples.begin(), samples.end());
    samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
    for (const tick_t t : samples) {
        if (t < -1 || t >= static_cast<tick_t>(a.hashes.size()))
            continue;
        REQUIRE(scr.jump_to(t));
        CHECK(scr.cursor() == t);
        REQUIRE(scr.hash().has_value());
        CHECK(*scr.hash() ==
              (t < 0 ? a.initial_hash : a.hashes[static_cast<std::size_t>(t)]));
    }
    // step_frame continues the same lattice: the compressed cursor keeps
    // its phase across jumps (jump_to reseeds it from the schedule).
    const tick_t base = samples.front();
    REQUIRE(scr.jump_to(base));
    REQUIRE(scr.step_frame(600));
    CHECK(scr.cursor() == base + 600);
    CHECK(*scr.hash() == a.hashes[static_cast<std::size_t>(base + 600)]);

    // Rebuilt-from-zero scrub cost: walking from the -1 checkpoint costs
    // one lattice op per tick in the near regions plus one coarse step
    // per 1000-tick far interval — the compression's scrub dividend.
    ReplayScrubber fresh(w_a, ship_params());
    REQUIRE(fresh.load_compressed(a.log, a.cps, a.sched));
    REQUIRE(fresh.jump_to(a.arrival));
    CHECK(fresh.replay_ops() > 0);
    CHECK(fresh.replay_ops() <= static_cast<std::uint64_t>(a.arrival + 1));
    CHECK(fresh.replay_ops() < static_cast<std::uint64_t>(a.steps / 2));

    // Empty schedule via load_compressed = plain full-rate replay, and it
    // still matches the reference run bit-for-bit (pin 1 again, through
    // the scrubber's lattice path at divisor 1).
    ReplayScrubber full(w_ref, ship_params());
    REQUIRE(full.load_compressed(ref.log, ref.cps, {}));
    REQUIRE(full.jump_to(ref.arrival));
    REQUIRE(full.hash().has_value());
    CHECK(*full.hash() == ref.hashes[static_cast<std::size_t>(ref.arrival)]);

    // Bad schedules are rejected all-or-nothing, keeping the old record.
    ReplayScrubber rej(w_a, ship_params());
    REQUIRE(rej.load_compressed(a.log, a.cps, a.sched));
    REQUIRE(rej.jump_to(1000));
    const std::size_t ls = rej.log_size();
    CHECK_FALSE(rej.load_compressed(a.log, a.cps, {{7, 1}, {3, 5}}));    // unsorted
    CHECK_FALSE(rej.load_compressed(a.log, a.cps, {{5, 1}, {5, 9}}));    // dup tick
    CHECK_FALSE(rej.load_compressed(a.log, a.cps, {{10, 0}}));           // divisor 0
    CHECK_FALSE(rej.load_compressed(a.log, a.cps,
                                    {{10, kLod_max_divisor + 1}}));      // divisor big
    CHECK_FALSE(rej.load_compressed(a.log, a.cps,
                                    {{static_cast<tick_t>(ls), 4}}));      // out of range
    CHECK(rej.log_size() == ls);
    CHECK(*rej.hash() == a.hashes[1000]);  // still standing
}

// ---------------------------------------------------------------- pin 1
TEST_CASE("lod_divisor1_equals_replay_step_bitwise") {
    // The scheduler must not be a second physics implementation: with no
    // bands (divisor pinned 1) the LOD spine reproduces replay_step tick
    // for tick, bit for bit, through the whole warp leg.
    rail_world w1 = make_world();
    rail_world w2 = make_world();
    const leg_result ref = run_leg(w1, spine_mode::reference, {}, 36000, false);
    const leg_result one = run_leg(w2, spine_mode::lod, {}, 36000, false);
    REQUIRE(ref.arrival >= 0);
    REQUIRE(one.arrival == ref.arrival);
    REQUIRE(one.hashes.size() == ref.hashes.size());
    CHECK(one.hashes == ref.hashes);
    CHECK(one.sched.empty());  // never left divisor 1: no transitions
    CHECK(one.steps == ref.steps);
    CHECK(one.tank_final == ref.tank_final);
}

// ---------------------------------------------------------------- pin 6
// Checklist name `compression_event_order`: tick ordering stays
// deterministic through the stats event bus UNDER compression — the
// compressed run executes twice with a live bus; hashes, schedule, and
// the entire event stream (order + seq included) must be identical, and
// the step/frozen interleave is part of the contract.
TEST_CASE("compression_event_order") {
    rail_world w1 = make_world();
    rail_world w2 = make_world();
    const leg_result a = run_leg(w1, spine_mode::lod, cruise_bands(), 36000, true);
    const leg_result b = run_leg(w2, spine_mode::lod, cruise_bands(), 36000, true);

    REQUIRE(a.hashes.size() == b.hashes.size());
    CHECK(a.hashes == b.hashes);
    CHECK(a.sched == b.sched);
    REQUIRE(a.events.size() == b.events.size());
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        CHECK(a.events[i].attr == b.events[i].attr);
        CHECK(a.events[i].tick == b.events[i].tick);
        CHECK(a.events[i].old_value == b.events[i].old_value);
        CHECK(a.events[i].new_value == b.events[i].new_value);
        CHECK(a.events[i].seq == b.events[i].seq);
    }

    // Bus interleave (#53c claim): nav/velocity stats change only on
    // physics-step ticks (retained state is frozen between steps), while
    // live drive stats change on NON-step ticks inside the far interval.
    bool mult_off_step = false, nav_any = false, nav_only_step = true;
    for (const auto& e : a.events) {
        const bool st = a.stepped[static_cast<std::size_t>(e.tick)] != 0;
        if (e.attr == "nav.closing") {
            nav_any = true;
            if (!st)
                nav_only_step = false;
        }
        if (e.attr == "warp.live_mult" && !st && e.tick > a.sched[0].tick &&
            e.tick < a.crossing)
            mult_off_step = true;
    }
    CHECK(nav_any);
    CHECK(nav_only_step);
    CHECK(mult_off_step);
}

}  // namespace ve_test
