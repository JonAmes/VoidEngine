// test: propellant_burn — 6.11 done-when (f) (#21c): a scripted burn drains
// the propellant reservoir EXACTLY force*t/Isp; the empty tank throws
// `propellant.empty` and function-zeros thrust (dark-not-dead) until a
// market refill restores it — all inside ONE replay hash.
//
// NO new core code and NO stored burn state: per tick the glue reads the
// pipeline's AGGREGATED thrust force (post any ×0 zeroing) and Isp, writes
// the reservoir's load attr as force/Isp (per second) with replace policy,
// and the reservoir integrates force·dt/Isp. Drain is a pure function of
// the impulse actually handed to replay_step at that tick — nothing about
// the burn is remembered between ticks.
//
// Binary-exact by construction: force 1200 N, Isp 20 (m/s effective exhaust
// velocity) -> flow 60/s -> exactly 1.0/tick; tank 120 -> dry at exactly
// n=120 steps; every mid-burn value is exactly 120 - n. All value pins are
// CHECK(==) bit pins (6.9 established the (rate * 1/60) exactness).
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/replay.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr float kForce = 1200.0f;  // thrust.force base (N)
constexpr float kIsp = 20.0f;      // thrust.isp base (flow per N·s)
constexpr double kTank = 120.0;    // capacity; 1.0/tick drain -> dry @ n=120
constexpr std::int64_t kTotal = 170;
constexpr std::int64_t kDefault_refill = 150;
constexpr std::uint64_t kBurnSrc = 200;  // glue: rewrites propellant.load
constexpr std::uint64_t kZeroSrc = 201;  // empty-tank ×0 on thrust.force

struct burn_result {
    std::vector<std::uint64_t> hashes;
    std::vector<double> value;            // reservoir value AFTER each step
    std::vector<double> v_z;              // vel.z AFTER each sim step
    std::vector<std::uint32_t> force_bits;  // derived thrust.force READ that tick
    std::vector<bool> low, empty;         // band tags AFTER each step
    std::uint64_t interval_hash = 0;      // ONE hash over the whole run
};

// refill_at: tick the market refill door opens (-1 = never: the tank stays
// dry and the ship coasts on function-zeroed thrust).
burn_result burn_run(std::int64_t refill_at) {
    StatsSet st;
    ModifierStack ms;
    st.set_base("thrust.force", kForce);
    st.set_base("thrust.isp", kIsp);
    st.set_base("propellant.load", 0.0f);  // modifier target must exist
    StatsPipeline sp(st, ms);

    TagSet tags;
    Reservoir res(kTank,
                  {{20.0, "propellant.low", band_dir::at_or_below},
                   {0.0, "propellant.empty", band_dir::at_or_below}},
                  &tags);
    res.bind(&sp, {"propellant.supply", "propellant.load", "propellant.leak"});
    res.set_ceiling(kTank);

    ms.add_source(kBurnSrc);
    ms.add_source(kZeroSrc);  // source lives from setup; zeroing added on dry

    rigid_params p{};
    p.mass = 50000.0;
    p.inertia_tensor = dmat3::diag({1.0e5, 8.0e5, 7.0e5});

    ReplayLog log;
    rail_world w;
    rigid_state s{};
    burn_result r{};

    // Glue mirror of the zeroing SOURCE's presence — bookkeeping of a
    // modifier source, NEVER of burn flow (no stored burn state).
    bool zeroing_live = false;
    for (std::int64_t t = 0; t < kTotal; ++t) {
        // Market/docked refill (#21c/#23b): the set_value door (bands
        // re-evaluate inside, hysteresis-free) + reverse the dark-not-dead
        // source wholesale so aggregates return to baseline BIT-exactly.
        if (t == refill_at) {
            res.set_value(kTank);
            ms.remove_source(kZeroSrc);
            zeroing_live = false;
        }
        // Aggregate THIS tick's applied force + Isp; write the load attr as
        // a pure function of them (replace: one live member, this tick's).
        const float f = *sp.derived("thrust.force");
        const float isp = *sp.derived("thrust.isp");
        {
            auto m = modifier_desc{};
            m.target_attr = "propellant.load";
            m.stack_id = "burn.load";
            m.op = mod_op::flat_add;
            m.value = f / isp;  // per second; 60.0f or 0.0f exactly here
            m.policy = stack_policy::replace;
            ms.add_modifier(kBurnSrc, m);
        }
        r.force_bits.push_back(std::bit_cast<std::uint32_t>(f));
        tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -static_cast<double>(f)};  // body -Z fwd
        log.append(cmd);
        replay_step(w, log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);
        r.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
        r.v_z.push_back(s.vel.z);
        res.step();  // consumes THIS tick's load; stats already final
        r.value.push_back(res.value());
        r.low.push_back(tags.contains("propellant.low"));
        r.empty.push_back(tags.contains("propellant.empty"));
        if (!zeroing_live && tags.contains("propellant.empty")) {
            auto m = modifier_desc{};
            m.target_attr = "thrust.force";
            m.stack_id = "propellant.thrust_zero";
            m.op = mod_op::multiplier;
            m.value = 0.0f;
            m.policy = stack_policy::exclusive;
            ms.add_modifier(kZeroSrc, m);
            zeroing_live = true;
        }
    }
    r.interval_hash = replay_interval_hash(r.hashes, 0, r.hashes.size() - 1);
    return r;
}
}  // namespace

TEST_CASE("propellant_burn") {
    const std::uint32_t f1200 = std::bit_cast<std::uint32_t>(kForce);
    const std::uint32_t fzero = std::bit_cast<std::uint32_t>(0.0f);

    SUBCASE("done-when (f): exact drain, empty throws, thrust zeroed, refill restores, one hash") {
        const auto r = burn_run(kDefault_refill);
        CHECK(r.hashes.size() == static_cast<std::size_t>(kTotal));

        // Drain identity: value == tank − force·t/Isp EXACTLY (flow 1.0/tick).
        // t index 0-based: after step n = t+1. force·(n/60)/Isp = n exactly.
        for (std::int64_t n : {1, 37, 99})
            CHECK(r.value[n - 1] == kTank - static_cast<double>(n));

        // Bands land at hand-computed ticks (hysteresis-free <= semantics).
        CHECK_FALSE(r.low[98]);           // n=99: 21.0
        CHECK(r.value[98] == 21.0);
        CHECK(r.low[99]);                 // n=100: 20.0 <= 20 -> low live
        CHECK(r.value[99] == 20.0);
        CHECK_FALSE(r.empty[118]);        // n=119: 1.0
        CHECK(r.value[118] == 1.0);
        CHECK(r.value[119] == 0.0);       // n=120: dry EXACTLY
        CHECK(r.empty[119]);
        CHECK(r.low[119]);                // low still live (0 <= 20)

        // Dark-not-dead: from the NEXT tick the applied force reads zero
        // (derived ×0), so no impulse, no drain, no negative tank ever.
        for (std::int64_t t = 120; t < kDefault_refill; ++t) {
            CHECK(r.force_bits[t] == fzero);
            CHECK(r.value[t] == 0.0);
        }

        // Market refill at 150: value full, empty tag dead BEFORE the read,
        // aggregates back to baseline BIT-EXACT, drain resumes at 1.0/tick.
        CHECK(r.force_bits[150] == f1200);
        CHECK(r.value[150] == 119.0);  // 120 - 1 post-refill step
        CHECK_FALSE(r.empty[150]);
        CHECK_FALSE(r.low[150]);
        for (std::int64_t n = 2; n <= 20; ++n)
            CHECK(r.value[149 + n] == kTank - static_cast<double>(n));
        CHECK(r.value[169] == 100.0);

        // ONE replay hash over the whole scenario.
        CHECK(r.interval_hash ==
              replay_interval_hash(r.hashes, 0, r.hashes.size() - 1));
        CHECK(r.interval_hash != 0);
    }

    SUBCASE("empty thrust zeroing is MEASURABLE in the sim; early refill keeps flying") {
        const auto dry = burn_run(-1);
        // Coasting on zeroed thrust: vel frozen from the first dry tick on.
        for (std::int64_t t = 121; t < kTotal; ++t)
            CHECK(dry.v_z[t] == dry.v_z[120]);
        // Burn leg kept exact per-tick velocity gain (1200/60/50000 = 4e-4):
        CHECK(dry.v_z[119] != dry.v_z[118]);

        // Refill BEFORE dry (tick 110): never throws empty, never zeroes.
        const auto early = burn_run(110);
        for (bool e : early.empty)
            CHECK_FALSE(e);
        CHECK(early.force_bits[110] == f1200);
        CHECK(early.value[110] == 119.0);
        CHECK_FALSE(early.low[110]);  // refilled full -> low dead too
        // Still accelerating where the dry run was coasting (body -Z burn
        // -> vel.z goes negative; compare speeds).
        CHECK(std::fabs(early.v_z[125]) > std::fabs(dry.v_z[125]));
        // Refill itself changes no force at t=110 (early wasn't dry yet), so
        // the SIM state stays identical until the dry run goes empty and
        // zeroes its thrust: hashes diverge exactly AT tick 120.
        std::size_t first_diff = 0;
        while (first_diff < dry.hashes.size() && dry.hashes[first_diff] == early.hashes[first_diff])
            ++first_diff;
        CHECK(first_diff == static_cast<std::size_t>(120));
    }

    SUBCASE("the whole scenario is replay-hash deterministic") {
        const auto a = burn_run(kDefault_refill);
        const auto b = burn_run(kDefault_refill);
        CHECK(a.hashes == b.hashes);            // bit-identical per-tick stream
        CHECK(a.interval_hash == b.interval_hash);
        CHECK(a.value == b.value);
        CHECK(a.force_bits == b.force_bits);
        const auto c = burn_run(140);
        CHECK(a.interval_hash != c.interval_hash);  // different script diverges
    }
}
