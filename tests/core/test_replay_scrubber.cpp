// test: replay_scrubber — 6.15 (#16, Tier 1). A replay record = ReplayLog
// + periodic checkpoints; the scrubber's load / step_frame / jump_to land
// on ANY tick with a state whose hash EQUALS the live run's hash at that
// tick, BIT-EXACT (the Tier-1 contract). All stepping goes through the 4.7
// replay_step — the scrubber owns no physics, so equality is structural.
//
// Scenario (gravity-free rail world, same as 6.11): 600 ticks of body -Z
// thrust 1200 N; a body +Y torque of 100 N*m runs ticks 400..499 so the
// attitude/omega legs are non-trivial in diffs. Checkpoints at -1 and
// every 100th post-step tick. Pins:
//   - jump_to hashes == live hashes at hand-picked ticks (both sides of
//     every checkpoint boundary, exact-hit ticks included);
//   - step_frame chains == live continuation; fail-closed past the end;
//   - jump cost observable: exact cp hit = 0 ops, +50 ticks past a cp = 50;
//   - diff_between hashes + dpos/dvel/domega bit-equal the live deltas,
//     cursor untouched; one-step diff = hand-exact accel*kSim_tick_dt.
#include <bit>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/scrubber.h>
#include <ve/core/sim/time.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::int64_t kTicks = 600;
constexpr std::int64_t kCpEvery = 100;  // post-step cp at 99, 199, ...
constexpr double kForce = 1200.0;
constexpr double kMass = 50000.0;

struct live_run {
    ReplayLog log;
    std::vector<std::uint64_t> hashes;
    std::vector<rigid_state> states;  // post-step state of tick t at [t]
    std::vector<sim_checkpoint> cps;  // tick -1 + every kCpEvery-th post-step
};

rigid_params make_params() {
    rigid_params p{};
    p.mass = kMass;
    p.inertia_tensor = dmat3::diag({1.0e5, 8.0e5, 7.0e5});
    return p;
}

live_run run_live() {
    live_run r;
    r.cps.push_back({-1, rigid_state{}});  // pre-step initial state
    const rigid_params p = make_params();
    rail_world w;
    rigid_state s{};
    for (std::int64_t t = 0; t < kTicks; ++t) {
        tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -kForce};
        if (t >= 400 && t < 500)
            cmd.torque_body = {0.0, 100.0, 0.0};
        r.log.append(cmd);
        replay_step(w, r.log, static_cast<std::size_t>(t), static_cast<tick_t>(t), p, s);
        r.hashes.push_back(state_hash(static_cast<tick_t>(t), s));
        r.states.push_back(s);
        if ((t + 1) % kCpEvery == 0)
            r.cps.push_back({static_cast<tick_t>(t), s});
    }
    return r;
}

void check_state_bit(const rigid_state& a, const rigid_state& b) {
    CHECK(std::bit_cast<std::uint64_t>(a.pos.x) == std::bit_cast<std::uint64_t>(b.pos.x));
    CHECK(std::bit_cast<std::uint64_t>(a.pos.z) == std::bit_cast<std::uint64_t>(b.pos.z));
    CHECK(std::bit_cast<std::uint64_t>(a.vel.z) == std::bit_cast<std::uint64_t>(b.vel.z));
    CHECK(std::bit_cast<std::uint64_t>(a.att.w) == std::bit_cast<std::uint64_t>(b.att.w));
    CHECK(std::bit_cast<std::uint64_t>(a.att.y) == std::bit_cast<std::uint64_t>(b.att.y));
    CHECK(std::bit_cast<std::uint64_t>(a.omega_body.y) ==
          std::bit_cast<std::uint64_t>(b.omega_body.y));
}

}  // namespace

TEST_CASE("replay_scrubber") {
    const live_run live = run_live();
    const rail_world w;
    ReplayScrubber sc(w, make_params());
    REQUIRE(sc.load(live.log, live.cps));
    CHECK(sc.checkpoint_count() == live.cps.size());
    CHECK(sc.log_size() == static_cast<std::size_t>(kTicks));

    // --- jump-to-tick == live hash, BIT-EXACT, across every cp boundary --
    for (const tick_t t : std::vector<tick_t>{-1, 0, 37, 98, 99, 100, 250, 299,
                                             300, 350, 437, 499, 500, 599}) {
        REQUIRE(sc.jump_to(t));
        CHECK(sc.cursor() == t);
        if (t >= 0)
            CHECK(sc.hash().value() == live.hashes[static_cast<std::size_t>(t)]);
        else
            CHECK(sc.hash().has_value());
        if (t >= 0)
            check_state_bit(sc.state(), live.states[static_cast<std::size_t>(t)]);
    }

    // --- step_frame chains continue the live run exactly ---------------
    REQUIRE(sc.jump_to(199));
    REQUIRE(sc.step_frame());
    CHECK(sc.cursor() == 200);
    CHECK(sc.hash().value() == live.hashes[200]);
    REQUIRE(sc.step_frame(49));
    CHECK(sc.cursor() == 249);
    CHECK(sc.hash().value() == live.hashes[249]);
    CHECK_FALSE(sc.step_frame(400));          // past the end: cursor holds
    CHECK(sc.cursor() == 249);
    REQUIRE(sc.jump_to(50));                  // backward scrub free
    CHECK(sc.hash().value() == live.hashes[50]);
    CHECK_FALSE(sc.jump_to(kTicks));          // one past the last tick
    CHECK_FALSE(sc.jump_to(-2));              // below the initial state

    // --- checkpoint locality is observable through replay_ops ------------
    sc.reset_replay_ops();
    REQUIRE(sc.jump_to(499));                 // exact cp hit: zero steps
    CHECK(sc.replay_ops() == 0);
    REQUIRE(sc.jump_to(549));                 // 50 ticks past cp 499
    CHECK(sc.replay_ops() == 50);
    REQUIRE(sc.jump_to(350));                 // 51 ticks from cp 299
    CHECK(sc.replay_ops() == 101);

    // --- diff extraction: hashes + deltas match live, cursor untouched --
    REQUIRE(sc.jump_to(249));
    const auto d = sc.diff_between(99, 199);
    REQUIRE(d.has_value());
    CHECK(d->from == 99);
    CHECK(d->to == 199);
    CHECK(d->hash_from == live.hashes[99]);
    CHECK(d->hash_to == live.hashes[199]);
    const rigid_state& a = live.states[99];
    const rigid_state& b = live.states[199];
    CHECK(std::bit_cast<std::uint64_t>(d->dvel.z) ==
          std::bit_cast<std::uint64_t>(b.vel.z - a.vel.z));
    CHECK(std::bit_cast<std::uint64_t>(d->dpos.z) ==
          std::bit_cast<std::uint64_t>(b.pos.z - a.pos.z));
    // No torque inside [100..199]: omega legs bit-exactly zero.
    CHECK(std::bit_cast<std::uint64_t>(d->domega.y) == 0u);
    CHECK(sc.cursor() == 249);                // diff never moves the cursor
    CHECK(sc.hash().value() == live.hashes[249]);

    // Torque leg 400..499: domega.y == live delta, att_delta carries a->b.
    const auto dt = sc.diff_between(399, 499);
    REQUIRE(dt.has_value());
    CHECK(dt->hash_from == live.hashes[399]);
    CHECK(dt->hash_to == live.hashes[499]);
    CHECK(std::bit_cast<std::uint64_t>(dt->domega.y) ==
          std::bit_cast<std::uint64_t>(live.states[499].omega_body.y -
                                       live.states[399].omega_body.y));
    const dquat carry = dt->att_delta * live.states[399].att;  // == att@499
    CHECK(carry.w == doctest::Approx(live.states[499].att.w).epsilon(1e-12));
    CHECK(carry.y == doctest::Approx(live.states[499].att.y).epsilon(1e-12));

    // One-tick diff from the pre-step state is hand-exact: dv = a*dt,
    // mirroring replay_step's op order (thrust * (1/m), then * dt).
    const auto d1 = sc.diff_between(-1, 0);
    REQUIRE(d1.has_value());
    const double accel = kForce * (1.0 / kMass);
    const double kStep = accel * kSim_tick_dt;
    CHECK(std::bit_cast<std::uint64_t>(d1->dvel.z) ==
          std::bit_cast<std::uint64_t>(-kStep));
    CHECK(std::bit_cast<std::uint64_t>(d1->dvel.x) == 0u);
    CHECK(std::bit_cast<std::uint64_t>(d1->domega.y) == 0u);

    // --- record validation: bad loads are refused, old record survives --
    std::vector<sim_checkpoint> bad = live.cps;
    std::swap(bad[1], bad[2]);  // descending ticks
    CHECK_FALSE(sc.load(live.log, bad));
    CHECK(sc.checkpoint_count() == live.cps.size());
    REQUIRE(sc.jump_to(37));                    // old record still live...
    CHECK(sc.hash().value() == live.hashes[37]);
    REQUIRE(sc.jump_to(249));                   // ...and scrubbing still works
    std::vector<sim_checkpoint> oor = live.cps;
    oor.back().tick = kTicks;                   // cp at/past the log end
    CHECK_FALSE(sc.load(live.log, oor));

    // A record whose oldest cp is at 99 cannot cover ticks below 99.
    std::vector<sim_checkpoint> late(live.cps.begin() + 1, live.cps.end());
    ReplayScrubber sc2(w, make_params());
    REQUIRE(sc2.load(live.log, late));
    CHECK_FALSE(sc2.jump_to(98));               // uncovered: fail closed
    CHECK_FALSE(sc2.has_cursor());
    REQUIRE(sc2.jump_to(99));                   // exact cp: fine
    CHECK(sc2.hash().value() == live.hashes[99]);
}
