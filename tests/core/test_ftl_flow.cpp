// test: ftl_flow — 8.12 done-when (a): per-warp-second propellant drain
// through the #19e reservoir. Same doctrine as 6.11: NO stored burn state.
// Per tick the glue asks whether the drive is BURNING (spool or warp — the
// ramp itself is paid for), writes ftl.flow flat_add (replace) onto the
// reservoir's load attr, and the reservoir integrates. Drain is a pure
// function of "was the drive burning THIS tick".
//
// Binary-exact by construction: ftl.flow base 120 (kg per WARP-second) ->
// 120*kSim_tick_dt = exactly 2.0/tick (the (rate*1/60) exactness 6.11
// established). Drive A: engage 300, warped flip 540, disengage door 900,
// idle 1140 -> draining ticks 300..1139 (840 ticks, 1680.0 kg exactly).
// Every value pin is a CHECK(==) bit pin.
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/ftl.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::int64_t kTotal = 1320;
constexpr std::int64_t kEngage = 300;
constexpr std::int64_t kDisengage = 900;
constexpr std::uint64_t kFlowSrc = 210;  // glue: ftl.flow onto propellant.load
constexpr std::uint64_t kDarkSrc = 211;  // scenario C: x0 on ftl.speed_mult

struct leg_setup {
    double tank = 2100.0;                  // low band @1000, empty @0
    std::int64_t dark_from = -1;           // x0 ftl.speed_mult window [from, to)
    std::int64_t dark_to = -1;
};

struct leg_result {
    std::vector<double> value;             // reservoir value after each step
    std::vector<double> mult;              // speed_mult consumed each tick
    std::vector<bool> low, empty;          // band tags after each step
    std::vector<tick_t> forced;            // ticks the empty-guard disengaged
    std::vector<std::uint64_t> hashes;     // state hash per tick
    double end_z = 0.0;                    // ship z after the run
    std::uint64_t interval_hash = 0;       // ONE hash over the whole leg
};

leg_result leg_run(const leg_setup& sc) {
    StatsSet st;
    ModifierStack ms;
    st.set_base("ftl.speed_mult", 13.0f);
    st.set_base("ftl.spool_s", 4.0f);
    st.set_base("ftl.flow", 120.0f);       // kg per warp-second
    st.set_base("propellant.load", 0.0f);
    StatsPipeline sp(st, ms);

    TagSet tags;
    Reservoir res(sc.tank,
                  {{1000.0, "propellant.low", band_dir::at_or_below},
                   {0.0, "propellant.empty", band_dir::at_or_below}},
                  &tags);
    res.bind(&sp, {"propellant.supply", "propellant.load", "propellant.leak"});
    res.set_ceiling(sc.tank);

    ms.add_source(kFlowSrc);
    ms.add_source(kDarkSrc);

    rigid_params p{};
    p.mass = 50000.0;
    p.inertia_tensor = dmat3::diag({1.0e5, 8.0e5, 7.0e5});

    ReplayLog log;
    rail_world w;
    rigid_state s{};
    s.vel = dvec3{0.0, 0.0, 7680.0};

    ftl_state fstate;
    leg_result r{};
    bool dark_live = false;

    for (std::int64_t t = 0; t < kTotal; ++t) {
        // Scenario C: zero the AGGREGATE drive mid-warp (mutation-checked:
        // an engage-time cached target would keep the ship burning through
        // the x0). restore via remove_source = BIT-exact back to base.
        if (sc.dark_from == t) {
            auto m = modifier_desc{};
            m.target_attr = "ftl.speed_mult";
            m.stack_id = "dark.zero";
            m.op = mod_op::multiplier;
            m.value = 0.0f;
            m.policy = stack_policy::exclusive;
            ms.add_modifier(kDarkSrc, m);
            dark_live = true;
        }
        if (sc.dark_to == t && dark_live) {
            ms.remove_source(kDarkSrc);
            dark_live = false;
        }
        // Flow guard: an empty tank forces a warp->down spin-down at the
        // NEXT tick boundary. Throttle-zeroing is instant, but you always
        // SPIN DOWN — the tank just can't pay for more warp. Guard only
        // fires from `warped`, so it is naturally one-shot.
        if (tags.contains("propellant.empty") && fstate.phase == ftl_phase::warped) {
            REQUIRE(ftl_disengage(fstate, static_cast<tick_t>(t)));
            r.forced.push_back(static_cast<tick_t>(t));
        }
        if (t == kEngage)
            REQUIRE(ftl_engage(ftl_drive{13.0, 4.0}, fstate, static_cast<tick_t>(t)));
        if (t == kDisengage &&
            (fstate.phase == ftl_phase::warped || fstate.phase == ftl_phase::spooling_up))
            REQUIRE(ftl_disengage(fstate, static_cast<tick_t>(t)));

        // The drive aggregate is READ from the pipeline every tick — the
        // stack is the truth (#21b), never a cached engage-time value.
        ftl_drive d{sp.derived("ftl.speed_mult").value_or(0.0f),
                    sp.derived("ftl.spool_s").value_or(0.0f)};
        ftl_step(d, fstate, static_cast<tick_t>(t));
        r.mult.push_back(fstate.mult);

        tick_command cmd{};
        cmd.speed_mult = fstate.mult;
        log.append(cmd);
        replay_step(w, log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);

        // Glue: ftl.flow rides the reservoir load ONLY while burning
        // (replace policy: one live member, this tick's answer).
        {
            auto m = modifier_desc{};
            m.target_attr = "propellant.load";
            m.stack_id = "ftl.load";
            m.op = mod_op::flat_add;
            m.value = ftl_burning(fstate)
                          ? sp.derived("ftl.flow").value_or(0.0f)
                          : 0.0f;
            m.policy = stack_policy::replace;
            ms.add_modifier(kFlowSrc, m);
        }
        res.step();  // consumes THIS tick's load
        r.value.push_back(res.value());
        r.low.push_back(tags.contains("propellant.low"));
        r.empty.push_back(tags.contains("propellant.empty"));
        r.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
    }
    r.end_z = s.pos.z;
    r.interval_hash = replay_interval_hash(r.hashes, 0, r.hashes.size() - 1);
    return r;
}

}  // namespace

TEST_CASE("ftl_flow") {
    SUBCASE("per-warp-second drain: every value is a BIT pin") {
        // Draining ticks 300..1139: exactly 2.0/tick. value(n) =
        // 2100 - 2*clamp(n-299, 0, 840) — exact everywhere.
        leg_result r = leg_run({});
        REQUIRE(static_cast<std::int64_t>(r.value.size()) == kTotal);
        double port = 2100.0;
        std::int64_t drained = 0;
        for (std::int64_t t = 0; t < kTotal; ++t) {
            if (t >= kEngage && t < 1140) { port -= 2.0; ++drained; }
            CHECK(r.value[static_cast<std::size_t>(t)] == port);
        }
        CHECK(drained == 840);                            // 14 warp-seconds
        CHECK(r.value[kTotal - 1] == 420.0);              // exact

        // Schedule pins: flip 540 (exactly 13.0), disengage door 900 holds
        // 13.0 on continuity, idle 1140 with mult exactly 1.
        CHECK(r.mult[kEngage] == 1.0);
        CHECK(r.mult[539] == 1.0 + 3.0 * (239.0 / 60.0));
        CHECK(r.mult[540] == 13.0);
        CHECK(r.mult[899] == 13.0);
        CHECK(r.mult[900] == 13.0);
        CHECK(r.mult[1139] == 13.0 - 3.0 * (239.0 / 60.0));
        CHECK(r.mult[1140] == 1.0);

        // Drain starts at ENGAGE, not at flip: the spool ramp is paid for.
        CHECK(r.value[kEngage] == 2098.0);
        CHECK(r.value[539] == 2100.0 - 2.0 * 240);  // 1620: spool drained

        // Low band fires MID-WARP (v(849) == 1000): low does NOT stop the
        // drain — only empty forces disengage.
        CHECK_FALSE(r.low[848]);   // 1002
        CHECK(r.low[849]);         // 1000: at_or_below
        CHECK(r.value[899] == 2100.0 - 2.0 * 600);  // 900: kept draining
        CHECK_FALSE(r.empty[kTotal - 1]);

        // Warp distance: exact port of (vel * recorded_mult) * dt.
        double z = 0.0;
        for (std::int64_t t = 0; t < kTotal; ++t)
            z += (7680.0 * r.mult[static_cast<std::size_t>(t)]) * kSim_tick_dt;
        CHECK(r.end_z == z);
        // ~2.3x the sublight distance over the same wall-clock run.
        CHECK(r.end_z > 2.0 * 7680.0 * (static_cast<double>(kTotal) / 60.0));

        // Determinism: same scripted leg -> identical one-interval replay
        // hash (warp legs ride the replay-hash train like everything else).
        leg_result r2 = leg_run({});
        CHECK(r.interval_hash == r2.interval_hash);
    }

    SUBCASE("empty mid-warp: forced spin-down, drain floors at 0") {
        // Tank 1100: v(849) = 1100 - 2*550 = 0.0 -> empty after step 849,
        // guard fires at 850 mid-WARP, spin-down ends idle at 850+240=1090.
        leg_result r = leg_run({.tank = 1100.0});
        CHECK(r.value[848] == 2.0);
        CHECK(r.value[849] == 0.0);
        CHECK(r.empty[849]);
        REQUIRE(r.forced.size() == 1);
        CHECK(r.forced[0] == 850);
        // Spun down from the LIVE 13x with no jump; idle exactly +240 ticks.
        CHECK(r.mult[849] == 13.0);
        CHECK(r.mult[850] == 13.0);
        CHECK(r.mult[1089] == 13.0 - 3.0 * (239.0 / 60.0));
        CHECK(r.mult[1090] == 1.0);
        CHECK(r.mult[kTotal - 1] == 1.0);
        // Drain stays gated on (never negative, never refilled): the
        // spin-down burns "on credit" at the floor — value pinned 0.0.
        for (std::int64_t t = 849; t < kTotal; ++t)
            CHECK(r.value[static_cast<std::size_t>(t)] == 0.0);
    }

    SUBCASE("dark-not-dead: x0 ftl.speed_mult idles instantly, drain freezes") {
        // Zero the aggregate 700..750 mid-warp: idle INSTANTLY (no ramp
        // past the stack), drain freezes, and the drive stays COLD after
        // restore — relight needs a new engage door, glue's call.
        leg_result r = leg_run({.tank = 2100.0, .dark_from = 700, .dark_to = 751});
        CHECK(r.mult[699] == 13.0);
        CHECK(r.mult[700] == 1.0);   // instant collapse (mutation-checked)
        CHECK(r.mult[750] == 1.0);
        CHECK(r.mult[751] == 1.0);   // restored base but idle: cold
        CHECK(r.mult[kTotal - 1] == 1.0);
        // Drained 300..699 only: 400 ticks * 2.0 -> 1300.0, then flat.
        CHECK(r.value[699] == 1300.0);
        CHECK(r.value[700] == 1300.0);
        CHECK(r.value[kTotal - 1] == 1300.0);
        for (std::int64_t t = 700; t < kTotal; ++t)
            CHECK(r.value[static_cast<std::size_t>(t)] == 1300.0);
    }
}
