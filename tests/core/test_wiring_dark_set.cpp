// test: wiring_dark_set — Phase 6 done-when (e): a cut wire or killed
// junction darkens its EXACT downstream set (snapshot = registration-ordered
// dark_set), a repair returns the aggregates to baseline exactly (stats
// state hash equality), and the whole scenario — wiring events + flight —
// lives inside ONE replay hash run twice bit-identical. No reservoir needed:
// the engines component's function attr IS the thrust force, so the cut
// cascade is visible in the flight numbers themselves.
//
// Exact pins: thrust base 1200 N / mass 50000 kg on a rail world with no
// gravity: before step t, v_z = -t·kStep while thrusting (kStep =
// 1200·dt/50000 per thrusting tick), bit-frozen while dark.
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/reservoir.h>  // kSim_dt
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_hash.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/wiring.h>

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using ve::core::math::dmat3;

constexpr double kMass = 50000.0;
constexpr double kStep = 1200.0 * kSim_dt / kMass;  // per thrusting tick

struct Run {
    std::vector<std::uint64_t> hashes;
    std::vector<double> v_z;
    std::vector<std::uint32_t> force_bits;
    std::vector<std::size_t> dark_n;
    std::size_t recomputes = 0;
    std::vector<std::string> snapshot_at_cut;  // dark_set right after the cut
    std::uint64_t stats_hash_before_cut = 0;   // baseline (captured at cut-1)
    std::uint64_t stats_hash_after_repair = 0;
    bool repair_seen = false;
};

// cut_at: tick the junction is killed (-1 = never). repair_at: tick revived.
Run run(std::int64_t cut_at, std::int64_t repair_at, std::int64_t ticks) {
    Run r;
    r.hashes.reserve(ticks);

    // ---- the wire network (same row shape the DEF0 loader carries) ----
    wire_entry w1; w1.id = "w_r_j"; w1.kind = wire_kind::power;
    w1.from = {1, "reactor"}; w1.to = {1, "junction_main"};
    wire_entry w2; w2.id = "w_j_e"; w2.kind = wire_kind::power;
    w2.from = {1, "junction_main"}; w2.to = {1, "engines"};
    std::vector<wire_entry> wires = {std::move(w1), std::move(w2)};
    std::vector<wired_component> comps = {
        {"reactor", "", "", false, false, true, false},
        {"junction_main", "", "", false, false, false, false},  // relay
        {"engines", "thrust.force", "engines.draw", true, false, false, false},
    };

    StatsSet st;
    st.set_base("thrust.force", 1200.0f);
    st.set_base("engines.draw", 10.0f);
    ModifierStack ms;
    StatsPipeline sp(st, ms);
    WiringReach wr(sp, ms, std::move(wires), std::move(comps));

    // Membership: all three mounted up front (loadout complete).
    REQUIRE(wr.mount("reactor"));
    REQUIRE(wr.mount("junction_main"));
    REQUIRE(wr.mount("engines"));

    rigid_params p{};
    p.mass = kMass;
    p.inertia_tensor = dmat3::diag({1.0e5, 8.0e5, 7.0e5});
    ReplayLog log;
    rail_world w;
    rigid_state s{};

    for (std::int64_t t = 0; t < ticks; ++t) {
        if (t == cut_at) {
            REQUIRE(wr.kill_component("junction_main"));
            r.snapshot_at_cut = wr.dark_set();
        }
        if (t == repair_at)
            REQUIRE(wr.revive_component("junction_main"));

        // Glue reads the AGGREGATED function attr: dark engines => 0 N.
        const float f = *sp.derived("thrust.force");
        (void)sp.derived("engines.draw");  // warm both attrs for the state hash
        // Baseline restoration pins are captured AFTER the derived reads:
        // the pipeline is recompute-on-read, so the StatsSet cache only
        // shows the restored bits once THIS tick's read has run.
        if (t == cut_at - 1)
            r.stats_hash_before_cut = stats_state_hash(st);
        if (t == repair_at) {
            r.stats_hash_after_repair = stats_state_hash(st);  // == baseline
            r.repair_seen = true;
        }
        r.force_bits.push_back(std::bit_cast<std::uint32_t>(f));
        r.dark_n.push_back(wr.dark_set().size());
        r.v_z.push_back(s.vel.z);

        tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -static_cast<double>(f)};  // body -Z fwd
        log.append(cmd);
        replay_step(w, log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);
        r.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
    }
    r.recomputes = wr.recompute_count();
    return r;
}

std::size_t first_diff(const std::vector<std::uint64_t>& a,
                       const std::vector<std::uint64_t>& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return i;
    return a.size();
}

}  // namespace

TEST_CASE("wiring_dark_set") {
    SUBCASE("cut cascade -> exact dark set -> repair restores baseline, in one replay hash") {
        constexpr std::int64_t kCut = 60, kRepair = 90, kTicks = 170;
        const auto a = run(kCut, kRepair, kTicks);
        const auto b = run(kCut, kRepair, kTicks);
        CHECK(a.hashes == b.hashes);  // determinism: twice, bit-identical
        CHECK(a.force_bits == b.force_bits);

        // Exact downstream set of the killed junction: ONLY engines (the
        // reactor and the relay itself carry no gates). Registration order.
        REQUIRE(a.snapshot_at_cut.size() == 1);
        CHECK(a.snapshot_at_cut[0] == "engines");

        // Dark membership per phase: 0 -> 1 (cut) -> 0 (repair).
        CHECK(a.dark_n[59] == 0);
        CHECK(a.dark_n[60] == 1);
        CHECK(a.dark_n[89] == 1);
        CHECK(a.dark_n[90] == 0);

        // Applied force bits: 1200 up to the cut, 0 while dark, 1200 after.
        const std::uint32_t kF = std::bit_cast<std::uint32_t>(1200.0f);
        const std::uint32_t kZ = std::bit_cast<std::uint32_t>(0.0f);
        CHECK(a.force_bits[59] == kF);
        CHECK(a.force_bits[60] == kZ);
        CHECK(a.force_bits[89] == kZ);
        CHECK(a.force_bits[90] == kF);

        // Repair returns the aggregates EXACTLY to baseline: post-repair
        // stats state hash == pre-cut baseline hash.
        REQUIRE(a.repair_seen);
        CHECK(a.stats_hash_after_repair == a.stats_hash_before_cut);

        // Hand-exact flight: v_z = -t·kStep before step t while thrusting;
        // bit-frozen through the dark leg (steps 60..89 apply 0 N).
        for (std::int64_t t = 0; t <= kCut; ++t)
            CHECK(a.v_z[std::size_t(t)] == doctest::Approx(-double(t) * kStep).epsilon(1e-12));
        for (std::int64_t t = kCut; t < kRepair; ++t)
            CHECK(a.v_z[std::size_t(t)] == a.v_z[kCut]);  // coast: exact freeze
        CHECK(a.v_z[kRepair] == doctest::Approx(-double(kCut) * kStep).epsilon(1e-12));
        CHECK(a.v_z[kRepair + 1] == doctest::Approx(-double(kCut + 1) * kStep).epsilon(1e-12));

        // Events-only recompute: ctor + 3 mounts + kill + revive = 6.
        CHECK(a.recomputes == 6);

        // One replay interval hash for the whole scenario; identical runs
        // agree bit-for-bit.
        CHECK(replay_interval_hash(a.hashes, 0, a.hashes.size() - 1) ==
              replay_interval_hash(b.hashes, 0, b.hashes.size() - 1));
    }

    SUBCASE("never-cut control: divergence starts EXACTLY at the cut tick") {
        auto cut = run(60, -1, 170);
        auto clean = run(-1, -1, 170);
        CHECK(first_diff(cut.hashes, clean.hashes) == 60);
    }

    SUBCASE("late-repair control: identical until the repair tick, then diverge") {
        auto repaired = run(60, 90, 170);
        auto dry = run(60, -1, 170);
        CHECK(first_diff(repaired.hashes, dry.hashes) == 90);
    }
}
