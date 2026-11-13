// Phase 8 task 8.12 — FTL drive: ramped warp speed multiplier + drain gate
// (decision #22b/#22d). ftl_spool pins the STATE MACHINE and the integrator
// warp path; ftl_flow pins the #19e reservoir drain. Everything hand-
// computed from the exact fp64 op order (tick_to_seconds = integer / 60.0,
// correctly rounded; ramp analytic from the anchor — see ftl.h).
//
// Numbers chosen so ramp intermediates land on BIT-EXACT doubles:
//   drive A: mult 13, spool 4 s  -> ramp_rate 3.0/s, full ramp 240 ticks,
//            intermediates at e=30k give mult = 1 + e/20 (dyadic).
//   drive B: mult 13, spool 3 s  -> ramp_rate 4.0/s, interrupted at e=104:
//            anchor_mult = fl(1 + fl(4*fl(104/60))) lands in the SAME
//            binade as the product, so anchor - product == 1.0 BIT-EXACTLY
//            at the symmetric tick (Sterbenz) — interrupted ramps end at
//            exactly the mirrored tick, no drift.
#include <cmath>
#include <cstring>
#include <random>

#include "ve/core/sim/ftl.h"
#include "ve/core/sim/preview.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"

#include <doctest/doctest.h>

using namespace ve::core;
using namespace ve::core::sim;

namespace {

// Drive A: 13x over 4 s -> ramp_rate exactly 3.0 (mult-units/s).
constexpr ftl_drive kDriveA{13.0, 4.0};
// Drive B: 13x over 3 s -> ramp_rate exactly 4.0.
constexpr ftl_drive kDriveB{13.0, 3.0};

const math::dvec3 kShipPos{1000000.0, 200000.0, 500000.0};
const math::dquat kShipAtt{0, 0, 0, 1};
// vel/60 = 128.0 EXACT -> every constant-mult tick steps a dyadic delta,
// and ramp ticks use the ramp's own (exact-by-construction) mult values.
const math::dvec3 kCoastVel{0.0, 0.0, 7680.0};
constexpr rigid_params kShip{50000.0, math::dvec3{4.0e10, 4.0e10, 4.0e10},
                             std::nullopt};

}  // namespace

TEST_CASE("ftl_spool") {
    SUBCASE("done-when (b): ramp hand-pins, full and interrupted, BIT-exact") {
        ftl_state st;
        // Idle: mult is 1 and stays 1; stepping an idle drive is inert.
        for (tick_t t = 0; t < 10; ++t) ftl_step(kDriveA, st, t);
        CHECK(st.phase == ftl_phase::idle);
        CHECK(st.mult == 1.0);
        CHECK_FALSE(ftl_burning(st));

        // Engage at tick 100 (door before that tick's step).
        REQUIRE(ftl_engage(kDriveA, st, 100));
        CHECK(ftl_burning(st));  // spool ticks burn (propellant is the budget)
        ftl_step(kDriveA, st, 100);
        CHECK(st.phase == ftl_phase::spooling_up);
        CHECK(st.mult == 1.0);  // e=0: still sublight this tick

        // Up-ramp pins: mult = 1 + 3*(e/60); e=30k -> exact (1 + e/20).
        const std::pair<tick_t, double> kUpPins[] = {
            {130, 2.5}, {160, 4.0}, {190, 5.5}, {220, 7.0},
            {250, 8.5}, {280, 10.0}, {310, 11.5}, {339, 12.95},
            // k=23 elapsed: one of the (few) tick counts where
            // k*fl(1/60) != fl(k/60). Every multiple-of-30-ish tick above is
            // bit-equal under BOTH the exact tick_to_seconds divide and the
            // single-product dt-multiply — this pin makes the divide form
            // load-bearing (MUT1 evidence). Pin the expression, not a
            // decimal: fl lands at 2.1500000000000004.
            {123, 1.0 + 3.0 * (23.0 / 60.0)},
        };
        // 339: 1 + 3*fl(239/60) — fl(239/60) rounds UP so the pin lands
        // exactly on 12.95 (verified bit pin, python cross-check).
        CHECK(kUpPins[7].second == 1.0 + 3.0 * (239.0 / 60.0));
        for (tick_t t = 101; t <= 339; ++t) {
            ftl_step(kDriveA, st, t);
            CHECK(st.phase == ftl_phase::spooling_up);
            if (t == 101) CHECK(st.mult > 1.0);       // ramp is live from tick 101
        }
        for (const auto& [t, m] : kUpPins) {
            ftl_state s2;
            REQUIRE(ftl_engage(kDriveA, s2, 100));
            for (tick_t k = 100; k <= t; ++k) ftl_step(kDriveA, s2, k);
            CHECK(s2.mult == m);  // exact equality, every pin tick
        }

        // Warp flip: e=240 -> 1+12 == 13.0 == target -> warped BIT-exactly.
        ftl_step(kDriveA, st, 340);
        CHECK(st.phase == ftl_phase::warped);
        CHECK(st.mult == 13.0);
        ftl_step(kDriveA, st, 400);
        CHECK(st.mult == 13.0);

        // Disengage at 400: down-ramp starts from the LIVE mult, no jump.
        REQUIRE(ftl_disengage(st, 400));
        ftl_step(kDriveA, st, 400);
        CHECK(st.phase == ftl_phase::spooling_down);
        CHECK(st.mult == 13.0);  // e=0 continuity
        const std::pair<tick_t, double> kDownPins[] = {
            {460, 10.0}, {520, 7.0}, {580, 4.0},
            // 239 ticks: the fl-product rounds the OTHER way here
            // (13 - 3*fl(239/60) = 1.0500000000000007, NOT 1.05) — pin
            // the expression itself, doctrine same as the rail pins.
            {639, 13.0 - 3.0 * (239.0 / 60.0)},
        };
        for (tick_t t = 401; t <= 639; ++t) {
            ftl_step(kDriveA, st, t);
            CHECK(st.phase == ftl_phase::spooling_down);
            CHECK(st.mult > 1.0);
            for (const auto& [pt, pv] : kDownPins)
                if (pt == t)
                    CHECK(st.mult == pv);
        }
        // e=240: 13 - 12 = 1.0 exactly -> idle, mult BIT-exactly 1.
        ftl_step(kDriveA, st, 640);
        CHECK(st.phase == ftl_phase::idle);
        CHECK(st.mult == 1.0);
        CHECK_FALSE(ftl_burning(st));

        // Interrupted ramp (drive B, engage 1000, door-close at 1105 after
        // 104 ramp ticks): lands idle at exactly 1105+104 = 1209 (header:
        // Sterbenz-exact subtraction of the identical fp64 product).
        ftl_state ib;
        REQUIRE(ftl_engage(kDriveB, ib, 1000));
        for (tick_t t = 1000; t <= 1104; ++t) ftl_step(kDriveB, ib, t);
        const double anchor = ib.mult;
        CHECK(anchor == 1.0 + 4.0 * (104.0 / 60.0));  // ramp_rate exactly 4.0
        REQUIRE(ftl_disengage(ib, 1105));
        ftl_step(kDriveB, ib, 1105);
        CHECK(ib.mult == anchor);  // continuity: no jump at the door
        for (tick_t t = 1106; t <= 1208; ++t) {
            ftl_step(kDriveB, ib, t);
            CHECK(ib.phase == ftl_phase::spooling_down);
            CHECK(ib.mult > 1.0);
        }
        ftl_step(kDriveB, ib, 1209);
        CHECK(ib.phase == ftl_phase::idle);
        CHECK(ib.mult == 1.0);  // exactly the symmetric tick, no fp drift
    }

    SUBCASE("guards: doors refuse invalid requests; collapse is instant") {
        ftl_state st;
        // engage from non-idle refused; invalid aggregates never engage.
        CHECK_FALSE(ftl_engage(ftl_drive{1.0, 4.0}, st, 0));      // mult 1 = no drive
        CHECK_FALSE(ftl_engage(ftl_drive{13.0, 0.0}, st, 0));     // zero spool
        CHECK_FALSE(ftl_engage(ftl_drive{13.0, -2.0}, st, 0));    // negative spool
        CHECK_FALSE(ftl_engage(ftl_drive{NAN, 4.0}, st, 0));      // NaN mult
        CHECK_FALSE(ftl_engage(ftl_drive{13.0, NAN}, st, 0));     // NaN spool
        CHECK_FALSE(ftl_engage(ftl_drive{INFINITY, 4.0}, st, 0)); // infinite mult
        CHECK(st.phase == ftl_phase::idle);
        CHECK(st.mult == 1.0);
        CHECK_FALSE(ftl_disengage(st, 0));  // idle has nothing to disengage

        REQUIRE(ftl_engage(kDriveA, st, 10));
        CHECK_FALSE(ftl_engage(kDriveA, st, 20));  // double engage refused
        ftl_step(kDriveA, st, 10);
        for (tick_t t = 300; t <= 340; ++t) ftl_step(kDriveA, st, t);
        CHECK(st.phase == ftl_phase::warped);
        CHECK_FALSE(ftl_engage(kDriveA, st, 341));  // engaged cannot re-engage

        // Dark-not-dead: a ×0 modifier zeroes ftl.speed_mult mid-warp ->
        // idle INSTANTLY (function-zeroing is instant; no ramp past the stack).
        ftl_step(ftl_drive{0.0, 4.0}, st, 341);
        CHECK(st.phase == ftl_phase::idle);
        CHECK(st.mult == 1.0);
        CHECK_FALSE(ftl_burning(st));

        // Same collapse mid-RAMP must not copy or compare a dead target.
        ftl_state mid;
        REQUIRE(ftl_engage(kDriveA, mid, 0));
        for (tick_t t = 0; t <= 50; ++t) ftl_step(kDriveA, mid, t);
        CHECK(mid.phase == ftl_phase::spooling_up);
        ftl_step(ftl_drive{NAN, 4.0}, mid, 51);
        CHECK(mid.phase == ftl_phase::idle);
        CHECK(mid.mult == 1.0);

        // Live-aggregate tracking while warped: mult follows the stack.
        ftl_state live;
        REQUIRE(ftl_engage(kDriveA, live, 0));
        for (tick_t t = 0; t <= 240; ++t) ftl_step(kDriveA, live, t);
        REQUIRE(live.phase == ftl_phase::warped);
        ftl_step(ftl_drive{9.0, 4.0}, live, 241);  // hit-damped drive (mutation-checked:
        CHECK(live.mult == 9.0);                   // a cached engage-time target would fail)
    }

    SUBCASE("integrator: mult 1.0 is BIT-identical to the pre-8.12 path") {
        // Same random flight through both paths, every state memcmp-equal.
        std::mt19937 rng(12);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        rigid_state a{.pos = kShipPos, .vel = kCoastVel, .att = kShipAtt};
        rigid_state b = a;
        for (int i = 0; i < 20000; ++i) {
            const math::dvec3 acc{u(rng) * 500.0, u(rng) * 500.0, u(rng) * 500.0};
            const math::dvec3 tq{u(rng) * 1e6, u(rng) * 1e6, u(rng) * 1e6};
            integrate_step(a, kShip, acc, tq);
            integrate_step_warp(b, kShip, acc, tq, 1.0);
            if (std::memcmp(&a, &b, sizeof(rigid_state)) != 0) {
                FAIL("diverged at tick " << i);
                break;
            }
        }
        CHECK(a.pos == b.pos);
        CHECK(a.vel == b.vel);
    }

    SUBCASE("done-when (a): warp distance hand-computed at full mult") {
        // Coast at 7680 m/s (vel/60 = 128 exact); warped 13x: every tick
        // adds EXACTLY 128*13 = 1664 m (all dyadic). 300 full-mult ticks:
        // 499,200 m. Then drive B's 180-tick ramp (rate 4.0): the delta
        // sum mirrors the ramp formula — ported identically below.
        rigid_state s{.pos = kShipPos, .vel = kCoastVel, .att = kShipAtt};
        double expected_z = kShipPos.z;
        for (int i = 0; i < 300; ++i) {
            integrate_step_warp(s, kShip, {}, {}, 13.0);
            expected_z += 1664.0;
        }
        CHECK(s.pos.z == expected_z);           // 1,000,200.0 exactly
        CHECK(s.pos.z == kShipPos.z + 499200.0);
        CHECK(s.vel == kCoastVel);              // warp does NOT touch vel

        // Full leg through the state machine (drive B): position and mult
        // step in lockstep; the port mirrors the exact op order.
        ftl_state st;
        REQUIRE(ftl_engage(kDriveB, st, 0));
        double port_z = s.pos.z;
        for (tick_t t = 0; t <= 420; ++t) {  // ramp 180 + warped 181..420
            ftl_step(kDriveB, st, t);
            integrate_step_warp(s, kShip, {}, {}, st.mult);
            // port mirrors the integrator op order: (vel * mult) * kSim_tick_dt
            port_z += (7680.0 * st.mult) * kSim_tick_dt;
        }
        CHECK(s.pos.z == port_z);
        // Sanity band: the leg covered >> sublight (~562 km vs ~54 km).
        CHECK(s.pos.z - kShipPos.z > 180000.0);
    }

    SUBCASE("determinism: warp leg replays identical; 1-ULP mult diverges") {
        const rail_world w;  // empty sky: warp needs no world
        const math::dvec3 start_pos{5.0e9, 0.0, 0.0};  // AU-scale coordinates
        auto run = [&](ReplayLog& log, std::vector<math::dvec3>& out_pos) {
            rigid_state s{.pos = start_pos, .vel = kCoastVel, .att = kShipAtt};
            ftl_state st;
            for (tick_t t = 0; t < 1500; ++t) {
                if (t == 100) ftl_engage(kDriveA, st, t);
                if (t == 900) ftl_disengage(st, t);
                ftl_step(kDriveA, st, t);
                tick_command cmd{};
                cmd.speed_mult = st.mult;
                log.append(cmd);
                replay_step(w, log, static_cast<std::size_t>(t), t, kShip, s);
                out_pos.push_back(s.pos);
            }
        };
        ReplayLog log_a;
        std::vector<math::dvec3> pos_a;
        run(log_a, pos_a);
        ReplayLog log_b;
        std::vector<math::dvec3> pos_b;
        run(log_b, pos_b);
        REQUIRE(pos_a.size() == pos_b.size());
        bool same = true;
        for (std::size_t i = 0; i < pos_a.size(); ++i)
            same = same && std::memcmp(&pos_a[i], &pos_b[i], sizeof(math::dvec3)) == 0;
        CHECK(same);  // replay-hash doctrine: same commands -> identical leg

        // A perturbed mult at one ramp tick -> the leg diverges from there.
        // (1 ULP is NOT detectable here by design of the scale: it moves
        // one step by ~4e-13 m while pos at 5e9 m has a ~1.1e-6 m ulp —
        // the sum rounds back bit-identical. A 1e-6 relative shift moves
        // the step ~1.6e-3 m, clearing the ulp: the recorded mult IS sim
        // state flowing through the log.)
        tick_command c = log_a.at(150);
        CHECK(c.speed_mult > 1.0);
        CHECK(c.speed_mult < 13.0);
        c.speed_mult *= 1.0 + 1e-6;
        log_a.set(150, c);
        ReplayLog log_c;  // replay the MUTATED log fresh
        rigid_state s{.pos = start_pos, .vel = kCoastVel, .att = kShipAtt};
        for (std::size_t i = 0; i < log_a.size(); ++i)
            replay_step(w, log_a, i, static_cast<tick_t>(i), kShip, s);
        CHECK(s.pos != pos_a.back());  // warp mult is sim state via the log

        // The ghost preview consumes the same recorded mults: divisor 1 is
        // the SAME codepath/dt -> BIT-identical to the fine roll through a
        // warp leg (8.14 gates its shaping; here: the recorded mult path).
        ReplayLog log_d;
        std::vector<math::dvec3> pos_d;
        run(log_d, pos_d);
        const rigid_state start{.pos = start_pos, .vel = kCoastVel, .att = kShipAtt};
        const auto ghost = ghost_preview(w, log_d, 0, 0, start, kShip, 600, 1);
        REQUIRE(ghost.size() == 600);
        bool same_ghost = true;
        for (std::size_t i = 0; i < ghost.size(); ++i)
            same_ghost = same_ghost &&
                         std::memcmp(&ghost[i].pos, &pos_d[i], sizeof(math::dvec3)) == 0;
        CHECK(same_ghost);
        // Coarse roll (divisor 4) stays within 1 m of the fine roll over the
        // whole warp leg: warp does not break the preview's stability band.
        const auto coarse = ghost_preview(w, log_d, 0, 0, start, kShip, 1500, 4);
        REQUIRE(!coarse.empty());
        CHECK(std::abs(coarse.back().pos.z - pos_d.back().z) < 1.0);
    }
}
