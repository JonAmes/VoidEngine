// test: gun_rof_ticks + gun_heat_cycle — 9.10 firing cycle (PLAN #19/#21).
//
// gun_rof_ticks: cooldown interval = llround(60/rof) tick-exact — rof 2 fires
// exactly {0,30,60,90,120}, rof 7 (8.571 -> 9) fires {0,9,18,27}, rof 0.5
// fires {0,120,240}; a rof nobody authored (or <=0) NEVER fires (fail-safe);
// the spread stream is keyed (seed, component, SHOT count) — pins match a
// hand-run splitmix64 chain (same constants as core/rng.h), refusals consume
// nothing (shot 1 lands identical whether or not ticks were skipped), the
// same shot number at different ticks is bit-identical, a different world
// seed diverges, all fired dirs stay inside the cone, and spread missing =
// exact aim bit-for-bit.
//
// gun_heat_cycle: dyadic law — leak 30 J/s -> exactly 0.5/tick (6.9
// established rate*kSim_dt exactness), per-shot 2 J, capacity 3 J, rof 30 ->
// interval 2. Every heat pin is CHECK(==) on dyadic values (the exact-
// epsilon doctest trap is documented in the phase notes). Timeline (attempt
// THEN tick each command): fire t=0,2,4; t=6 cooldown-CLEAR but heat 3.0 >=
// capacity -> REFUSED (the overheat gate, not cooldown — pinned by the tag
// being live that instant); fire resumes t=7. The tag throws/removes
// hysteresis-free (ON at t=4,5, OFF after t=6's step). Heat accumulates PAST
// the limit (4.0, 4.5 > 3.0): the overshoot is the signal; unbounded
// reservoir on purpose.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/gun.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

gun_attrs weapon_attrs() {
    return {/*rof=*/"weapon.rof", /*spread=*/"weapon.spread",
            /*limit=*/"weapon.heat_capacity", /*per_shot=*/"weapon.heat_per_shot",
            /*leak=*/"weapon.heat_dissipate", /*tag=*/"gun.overheated"};
}

constexpr std::uint64_t kSeed = 0xC0FFEEull;
}  // namespace

TEST_CASE("gun_rof_ticks") {
    // --- exact tick spacing: rof 2 -> interval 30
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 2.0f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        std::vector<tick_t> fired;
        for (tick_t t = 0; t <= 120; ++t) {
            if (gun_try_fire(gun, {1.0, 0.0, 0.0}, t, kSeed))
                fired.push_back(t);
            gun_tick(gun);
        }
        CHECK(fired == std::vector<tick_t>{0, 30, 60, 90, 120});
        CHECK(gun.shots == 5);
    }
    // --- rounding: rof 7 -> llround(8.571..) = 9
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 7.0f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        std::vector<tick_t> fired;
        for (tick_t t = 0; t <= 27; ++t) {
            if (gun_try_fire(gun, {1.0, 0.0, 0.0}, t, kSeed))
                fired.push_back(t);
            gun_tick(gun);
        }
        CHECK(fired == std::vector<tick_t>{0, 9, 18, 27});
    }
    // --- sub-1 rof: 0.5 -> interval 120
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 0.5f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        std::vector<tick_t> fired;
        for (tick_t t = 0; t <= 240; ++t) {
            if (gun_try_fire(gun, {1.0, 0.0, 0.0}, t, kSeed))
                fired.push_back(t);
            gun_tick(gun);
        }
        CHECK(fired == std::vector<tick_t>{0, 120, 240});
    }
    // --- fail-safe: no rof authored / rof <= 0 -> NEVER fires, no state
    {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed));
        CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 999, kSeed));
        CHECK(gun.shots == 0);
        CHECK(gun.last_shot == -1);
    }
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 0.0f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed));
        CHECK(gun.shots == 0);
    }
    // --- degenerate aims refused, state untouched
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 2.0f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.1}, 0, kSeed));  // non-unit (9.7 law)
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(gun_try_fire(gun, {nan, 0.0, 0.0}, 0, kSeed));
        CHECK(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed).has_value());  // the valid attempt fires
        CHECK(gun.shots == 1);  // ...only it; the two degenerate ones changed nothing
    }
    // --- spread stream pins: hand-run splitmix64 chain (seed 0xC0FFEE,
    // component 3, spread 0.004, aim +X). rof 60 -> interval 1, fire 0,1,2.
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 60.0f);
        st.set_base("weapon.spread", 0.004f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        const auto r0 = gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed);
        const auto r1 = gun_try_fire(gun, {1.0, 0.0, 0.0}, 1, kSeed);
        const auto r2 = gun_try_fire(gun, {1.0, 0.0, 0.0}, 2, kSeed);
        REQUIRE(r0); REQUIRE(r1); REQUIRE(r2);
        CHECK(r0->aim.x == doctest::Approx(0.9999997695682126).epsilon(1e-9));
        CHECK(r0->aim.y == doctest::Approx(0.0006721698338776347).epsilon(1e-9));
        CHECK(r0->aim.z == doctest::Approx(9.513798512921684e-05).epsilon(1e-9));
        CHECK(r1->aim.x == doctest::Approx(0.9999990667642092).epsilon(1e-9));
        CHECK(r1->aim.y == doctest::Approx(-0.0012167803205529364).epsilon(1e-9));
        CHECK(r1->aim.z == doctest::Approx(-0.0006212216690185774).epsilon(1e-9));
        CHECK(r2->aim.x == doctest::Approx(0.9999933910163507).epsilon(1e-9));
        CHECK(r2->aim.y == doctest::Approx(0.0030041345533613688).epsilon(1e-9));
        CHECK(r2->aim.z == doctest::Approx(-0.002047705839562756).epsilon(1e-9));
        // cone containment + unit law on every fired direction
        for (const auto* r : {&*r0, &*r1, &*r2}) {
            CHECK(math::approximately_equal(math::length2(r->aim), 1.0, 1e-12));
            CHECK(std::abs(r->aim.y) <= 0.004);
            CHECK(std::abs(r->aim.z) <= 0.004);
        }
        // shot-keyed, NOT tick-keyed: same params, first shot at t=999 ->
        // bit-identical deflection to shot 0 at t=0.
        {
            gun_slot g2;
            g2.component = 3;
            gun_configure(g2, sp, weapon_attrs());
            const auto late = gun_try_fire(g2, {1.0, 0.0, 0.0}, 999, kSeed);
            REQUIRE(late);
            CHECK(late->aim.x == r0->aim.x);
            CHECK(late->aim.y == r0->aim.y);
            CHECK(late->aim.z == r0->aim.z);
        }
        // refusals consume nothing: rof 2 (interval 30), attempts at 0 and
        // 30 -> shot 1's deflection identical to consecutive-fire shot 1.
        {
            StatsSet st2;
            ModifierStack ms2;
            st2.set_base("weapon.rof", 2.0f);
            st2.set_base("weapon.spread", 0.004f);
            StatsPipeline sp2(st2, ms2);
            gun_slot g3;
            g3.component = 3;
            gun_configure(g3, sp2, weapon_attrs());
            REQUIRE(gun_try_fire(g3, {1.0, 0.0, 0.0}, 0, kSeed));
            CHECK_FALSE(gun_try_fire(g3, {1.0, 0.0, 0.0}, 5, kSeed));  // cooldown
            const auto r1s = gun_try_fire(g3, {1.0, 0.0, 0.0}, 30, kSeed);
            REQUIRE(r1s);
            CHECK(r1s->aim.x == r1->aim.x);
            CHECK(r1s->aim.y == r1->aim.y);
            CHECK(r1s->aim.z == r1->aim.z);
        }
        // different world seed -> different deflection (deterministic divergence)
        {
            gun_slot g4;
            g4.component = 3;
            gun_configure(g4, sp, weapon_attrs());
            const auto d = gun_try_fire(g4, {1.0, 0.0, 0.0}, 0, 0xBADC0DEull);
            REQUIRE(d);
            CHECK(d->aim.x == doctest::Approx(0.9999978307401752).epsilon(1e-9));
            CHECK(d->aim.y == doctest::Approx(-0.000765657792501468).epsilon(1e-9));
            CHECK(d->aim.z == doctest::Approx(0.0019370810743589773).epsilon(1e-9));
        }
    }
    // --- spread missing -> aim EXACT (bit-for-bit passthrough)
    {
        StatsSet st;
        ModifierStack ms;
        st.set_base("weapon.rof", 60.0f);
        StatsPipeline sp(st, ms);
        gun_slot gun;
        gun.component = 3;
        gun_configure(gun, sp, weapon_attrs());
        const auto r = gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed);
        REQUIRE(r);
        CHECK(r->aim.x == 1.0);
        CHECK(r->aim.y == 0.0);
        CHECK(r->aim.z == 0.0);
    }
    // --- unconfigured gun (no pipeline) refuses
    {
        gun_slot gun;
        gun.component = 3;
        CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, kSeed));
    }
}

TEST_CASE("gun_heat_cycle") {
    StatsSet st;
    ModifierStack ms;
    st.set_base("weapon.rof", 30.0f);            // interval = 2 ticks
    st.set_base("weapon.heat_capacity", 3.0f);
    st.set_base("weapon.heat_per_shot", 2.0f);
    st.set_base("weapon.heat_dissipate", 30.0f);  // leak = exactly 0.5/tick
    StatsPipeline sp(st, ms);
    gun_slot gun;
    gun.component = 3;
    gun_configure(gun, sp, weapon_attrs());

    std::vector<tick_t> fired, refused;
    // timeline: heat AFTER each step; dyadic exact (CHECK ==)
    const double heat_after_step[] = {1.5, 1.0, 2.5, 2.0, 3.5, 3.0, 2.5, 4.0, 3.5};
    const bool tag_after_step[] = {false, false, false, false, true, true,
                                   false, true, true};
    double heat_after_shot[] = {2.0, 3.0, 4.0, 4.5};  // t=0,2,4,7 (past limit!)
    std::size_t shot_i = 0;
    for (tick_t t = 0; t <= 8; ++t) {
        const auto r = gun_try_fire(gun, {1.0, 0.0, 0.0}, t, kSeed);
        if (r) {
            fired.push_back(t);
            if (shot_i < 4)
                CHECK(r->heat_after == heat_after_shot[shot_i]);
            CHECK(shot_i < 4);  // an extra shot fails this guard, not an OOB read
            ++shot_i;
        } else {
            refused.push_back(t);
        }
        gun_tick(gun);
        CHECK(gun.heat.value() == heat_after_step[t]);
        CHECK(gun.tags.contains_exact("gun.overheated") == tag_after_step[t]);
    }
    CHECK(fired == std::vector<tick_t>{0, 2, 4, 7});
    CHECK(refused == std::vector<tick_t>{1, 3, 5, 6, 8});
    CHECK(shot_i == 4);

    // t=6 is the OVERHEAT refusal, not cooldown: interval 2, last shot t=4
    // -> t=6 is cooldown-clear; heat 3.0 >= capacity 3.0 refuses and the
    // tag is live at that instant (thrown by t=5's step, 3.0 >= 3).
    // (reconstructed state at t=6 is asserted by the timeline pins: tag ON
    // after t=5, heat exactly 3.0 after t=5 — the refusal at t=6 had no
    // cooldown basis.)
    CHECK(gun.shots == 4);
    CHECK(gun.last_shot == 7);

    // heat runs PAST the limit unclamped (4.5 > 3.0 pinned in the timeline):
    // the overshoot keeps the gun refused while it cools back under.
    // Cooling under the capacity un-throws the tag hysteresis-free (t=6).

    // cooling resumes firing: t=8 refused (cooldown), so step-only t=9,10
    // (2.5 -> 2.0 -> 1.5) then t=10 fires (1.5 < 3, cooldown clear since t=7).
    gun_tick(gun);  // t=9: heat 3.5 -> 3.0
    CHECK(gun.heat.value() == 3.0);
    gun_tick(gun);  // t=10: 3.0 -> 2.5, tag OFF (un-thrown, no hysteresis)
    CHECK(gun.tags.contains_exact("gun.overheated") == false);
    const auto r = gun_try_fire(gun, {1.0, 0.0, 0.0}, 11, kSeed);  // 11-7 >= 2, heat 2.5 < 3
    REQUIRE(r);
    CHECK(r->heat_after == 4.5);
}
