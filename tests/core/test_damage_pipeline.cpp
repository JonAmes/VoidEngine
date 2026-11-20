// test: shield_overflow_handcalc + component_offline_zeroing — 9.11 damage
// pipeline core (PLAN #26). Every number is hand-computable; all pools hold
// dyadic integers so pins are exact == (the doctest Approx-trap note in the
// phase notes applies; none of these values need Approx).
//
// shield_overflow_handcalc (no wiring): shield 100/100 absorbs first, the
// overflow lands on the component pool, health floors at 0, the band tag
// throws INSIDE the lethal hit (set_value re-evals), repeat hits never
// re-transition, refusals (unknown ref, negative, NaN) change nothing, and
// regen 60 J/s steps exactly +1.0/tick (60*(1/60) == 1.0 exactly).
//
// component_offline_zeroing (full wiring net): each wired system's zeroing
// is the SHIPPED function-zero source — engine thrust attr and draw go to
// 0, the shield's regen attr goes to 0 so the pool stops regenerating, and
// the gun's weapon.rof goes to 0 where the 9.10 fail-safe silently stops
// firing (no new gate). The offline band tag on the component's own TagSet
// flips a condition gate's condition_met (the shipped 6.6 machinery reads
// it). Repair restores health, tag, attrs, and firing bit-exactly.
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/damage.h"
#include "ve/core/sim/effects.h"
#include "ve/core/sim/gun.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/wiring.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

constexpr std::string_view kOffline = "ship.component.offline";
}  // namespace

TEST_CASE("shield_overflow_handcalc") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);
    stats.set_base("shield.capacity", 100.0f);
    stats.set_base("shield.regen", 0.0f);

    damage_sink sink;
    sink_configure_shield(sink, pipe, shield_setup{
        .regen_attr = "shield.regen",
        .capacity_attr = "shield.capacity",
        .bands = {reservoir_band{0.0, "shield.down", band_dir::at_or_below}},
    });
    CHECK(sink.shield.value() == 100.0);  // spawn law: shields come up full
    CHECK_FALSE(sink.ship_tags.contains_exact("shield.down"));
    REQUIRE(sink_add_component(sink, "eng_1", 50.0, kOffline));
    REQUIRE(sink.comps.size() == 1);
    CHECK(sink.comps[0]->hp.value() == 50.0);  // pools spawn full

    // hit 1: 25 J, all absorbed. shield 100 -> 75 - 10 = hand calc below.
    auto r1 = sink_apply_hit(sink, nullptr, "eng_1", 25.0);
    REQUIRE(r1.has_value());
    CHECK(r1->absorbed == 25.0);
    CHECK(r1->on_component == 0.0);
    CHECK(r1->shield_after == 75.0);
    CHECK(r1->hp_after == 50.0);
    CHECK_FALSE(r1->went_offline);
    CHECK_FALSE(sink.ship_tags.contains_exact("shield.down"));

    // hit 2: 90 J -> absorb 75, overflow 15, hp 50 -> 35; shield hits 0 and
    // the shield.down band throws immediately.
    auto r2 = sink_apply_hit(sink, nullptr, "eng_1", 90.0);
    REQUIRE(r2.has_value());
    CHECK(r2->absorbed == 75.0);
    CHECK(r2->on_component == 15.0);
    CHECK(r2->shield_after == 0.0);
    CHECK(r2->hp_after == 35.0);
    CHECK_FALSE(r2->went_offline);
    CHECK(sink.ship_tags.contains_exact("shield.down"));

    // hit 3: 35 J -> hp 35 -> 0 EXACTLY: offline transition this hit, tag
    // live on the component's OWN TagSet without any step() call.
    auto r3 = sink_apply_hit(sink, nullptr, "eng_1", 35.0);
    REQUIRE(r3.has_value());
    CHECK(r3->absorbed == 0.0);
    CHECK(r3->on_component == 35.0);
    CHECK(r3->hp_after == 0.0);
    CHECK(r3->went_offline);
    CHECK(sink.comps[0]->tags.contains_exact(std::string(kOffline)));

    // hit 4: already dead — health floors at 0 (never negative), and the
    // offline flag reports the TRANSITION, so a repeat hit is false.
    auto r4 = sink_apply_hit(sink, nullptr, "eng_1", 10.0);
    REQUIRE(r4.has_value());
    CHECK(r4->on_component == 10.0);
    CHECK(r4->hp_after == 0.0);
    CHECK_FALSE(r4->went_offline);

    // regen: 60 J/s -> exactly +1.0/tick; crossing above 0 un-throws the
    // band hysteresis-free.
    stats.set_base("shield.regen", 60.0f);
    sink.shield.step();
    CHECK(sink.shield.value() == 1.0);
    CHECK_FALSE(sink.ship_tags.contains_exact("shield.down"));
    sink.shield.step();
    CHECK(sink.shield.value() == 2.0);
    // regen never overfills past the ceiling
    stats.set_base("shield.regen", 1000000.0f);
    for (int i = 0; i < 8; ++i) sink.shield.step();
    CHECK(sink.shield.value() == 100.0);

    // refusals: zero state change.
    const double shield_before = sink.shield.value();
    CHECK_FALSE(sink_apply_hit(sink, nullptr, "nobody", 10.0).has_value());
    CHECK_FALSE(sink_apply_hit(sink, nullptr, "eng_1", -5.0).has_value());
    CHECK_FALSE(sink_apply_hit(sink, nullptr, "eng_1",
                               std::numeric_limits<double>::infinity()).has_value());
    CHECK_FALSE(sink_apply_hit(sink, nullptr, "eng_1",
                               std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK(sink.shield.value() == shield_before);
    CHECK(sink.comps[0]->hp.value() == 0.0);

    // 0 damage is a legal no-op report.
    auto r0 = sink_apply_hit(sink, nullptr, "eng_1", 0.0);
    REQUIRE(r0.has_value());
    CHECK(r0->absorbed == 0.0);
    CHECK(r0->hp_after == 0.0);
    CHECK_FALSE(r0->went_offline);

    // no-shield ship: capacity attr unconfigured = absorbs NOTHING (fail-safe).
    damage_sink bare;
    sink_configure_shield(bare, pipe, shield_setup{.regen_attr = "",
                                                   .capacity_attr = "",
                                                   .bands = {}});
    CHECK(bare.shield.value() == 0.0);
    REQUIRE(sink_add_component(bare, "eng_1", 50.0, kOffline));
    auto rb = sink_apply_hit(bare, nullptr, "eng_1", 30.0);
    REQUIRE(rb.has_value());
    CHECK(rb->absorbed == 0.0);
    CHECK(rb->on_component == 30.0);
    CHECK(rb->hp_after == 20.0);

    // duplicate registration refused; bad max_health refused.
    CHECK_FALSE(sink_add_component(sink, "eng_1", 10.0, kOffline));
    CHECK_FALSE(sink_add_component(sink, "x_1", 0.0, kOffline));
    CHECK_FALSE(sink_add_component(sink, "x_1", -1.0, kOffline));
    CHECK_FALSE(sink_add_component(sink, "x_1", 5.0, ""));
}

TEST_CASE("component_offline_zeroing") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);
    stats.set_base("shield.capacity", 100.0f);
    stats.set_base("shield.regen", 60.0f);
    stats.set_base("thrust.force", 100.0f);
    stats.set_base("power.draw.engine", 5.0f);
    stats.set_base("power.draw.shield", 3.0f);
    stats.set_base("weapon.rof", 2.0f);
    stats.set_base("power.draw.gun", 4.0f);

    // Hand net: reactor_1 powers each system directly (registration order
    // = deterministic iteration, 6.12).
    std::vector<wire_entry> wires = {
        wire_entry{"wp1", wire_kind::power, std::nullopt, {1, "reactor_1"}, {1, "eng_1"}},
        wire_entry{"wp2", wire_kind::power, std::nullopt, {1, "reactor_1"}, {1, "shield_gen"}},
        wire_entry{"wp3", wire_kind::power, std::nullopt, {1, "reactor_1"}, {1, "gun_1"}},
    };
    std::vector<wired_component> comps = {
        {"reactor_1", "", "", false, false, true, false},
        {"eng_1", "thrust.force", "power.draw.engine", true, false, false, false},
        {"shield_gen", "shield.regen", "power.draw.shield", true, false, false, false},
        {"gun_1", "weapon.rof", "power.draw.gun", true, false, false, false},
    };
    WiringReach wiring(pipe, stack, std::move(wires), std::move(comps));
    for (const char* ref : {"reactor_1", "eng_1", "shield_gen", "gun_1"})
        REQUIRE(wiring.mount(ref));
    for (const char* ref : {"eng_1", "shield_gen", "gun_1"})
        REQUIRE(wiring.lit(ref));

    // alive baseline: every system's function attr live.
    CHECK(pipe.derived("thrust.force").value() == 100.0f);
    CHECK(pipe.derived("shield.regen").value() == 60.0f);
    CHECK(pipe.derived("weapon.rof").value() == 2.0f);

    damage_sink sink;
    sink_configure_shield(sink, pipe, shield_setup{
        .regen_attr = "shield.regen",
        .capacity_attr = "shield.capacity",
        .bands = {reservoir_band{0.0, "shield.down", band_dir::at_or_below}},
    });
    REQUIRE(sink_add_component(sink, "eng_1", 50.0, kOffline));
    REQUIRE(sink_add_component(sink, "shield_gen", 30.0, kOffline));
    REQUIRE(sink_add_component(sink, "gun_1", 20.0, kOffline));

    // the gun fires while alive (9.10 machinery, rof live from the net)
    gun_slot gun;
    gun.component = 1;
    gun_configure(gun, pipe, gun_attrs{.rof = "weapon.rof", .spread = "",
                                       .heat_limit = "", .heat_per_shot = "",
                                       .heat_leak = "", .overheat_tag = ""});
    REQUIRE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, 7).has_value());

    // the shipped 6.6 condition machinery sees the offline tag (no new gate)
    condition gate;
    gate.forbid_tags = {std::string(kOffline)};
    component_hp* eng = sink.comps[0].get();
    CHECK(condition_met(gate, &eng->tags, nullptr));

    // ENGINE: shield eats 100, second hit 50 -> eng hp 50 -> 0.
    auto e1 = sink_apply_hit(sink, &wiring, "eng_1", 100.0);
    REQUIRE(e1.has_value());
    CHECK(e1->absorbed == 100.0);
    CHECK(e1->went_offline == false);
    auto e2 = sink_apply_hit(sink, &wiring, "eng_1", 50.0);
    REQUIRE(e2.has_value());
    CHECK(e2->on_component == 50.0);
    CHECK(e2->went_offline);
    // ENGINE HALTS THRUST: function + draw zeroed through the wiring net
    // (the shipped dark source), not by any damage-side machinery.
    CHECK(pipe.derived("thrust.force").value() == 0.0f);
    CHECK(pipe.derived("power.draw.engine").value() == 0.0f);
    CHECK_FALSE(wiring.lit("eng_1"));
    CHECK(eng->tags.contains_exact(std::string(kOffline)));
    CHECK_FALSE(condition_met(gate, &eng->tags, nullptr));
    // the dark set is exactly the killed engine (siblings stay powered)
    auto dark = wiring.dark_set();
    REQUIRE(dark.size() == 1);
    CHECK(dark[0] == "eng_1");
    CHECK(wiring.lit("gun_1"));

    // SHIELD: regen is live (pool 0 -> exactly +1.0), then the generator
    // dies to a 31 J hit (absorb the 1 J regen, 30 J onto its 30 HP pool).
    sink.shield.step();
    CHECK(sink.shield.value() == 1.0);
    auto s1 = sink_apply_hit(sink, &wiring, "shield_gen", 31.0);
    REQUIRE(s1.has_value());
    CHECK(s1->absorbed == 1.0);
    CHECK(s1->on_component == 30.0);
    CHECK(s1->went_offline);
    // SHIELD HALTS REGEN: regen attr zeroed -> pool stays 0.0 through steps.
    CHECK(pipe.derived("shield.regen").value() == 0.0f);
    CHECK(sink.shield.value() == 0.0);
    sink.shield.step();
    sink.shield.step();
    CHECK(sink.shield.value() == 0.0);

    // GUN: 20 J kills it; weapon.rof zeroes and the 9.10 fail-safe (rof <= 0
    // never fires) stops it — zero new gates.
    auto g1 = sink_apply_hit(sink, &wiring, "gun_1", 20.0);
    REQUIRE(g1.has_value());
    CHECK(g1->went_offline);
    CHECK(pipe.derived("weapon.rof").value() == 0.0f);
    CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 200, 7).has_value());
    CHECK(gun.shots == 1);

    // repeat hit on the dead gun: idempotent kill (no transition reported).
    auto g2 = sink_apply_hit(sink, &wiring, "gun_1", 20.0);
    REQUIRE(g2.has_value());
    CHECK_FALSE(g2->went_offline);

    // REPAIR round trip (9.15 rides this): heal the gun -> tag off, rof
    // back bit-exactly, firing resumes, condition re-opens.
    REQUIRE(sink_repair_component(sink, &wiring, "gun_1", 20.0));
    component_hp* gun_c = sink.comps[2].get();
    CHECK_FALSE(gun_c->tags.contains_exact(std::string(kOffline)));
    CHECK(pipe.derived("weapon.rof").value() == 2.0f);
    REQUIRE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 300, 7).has_value());
    CHECK(gun.shots == 2);
    // over-repair clamps at the ceiling (set_value door)
    REQUIRE(sink_repair_component(sink, &wiring, "gun_1", 999.0));
    CHECK(gun_c->hp.value() == 20.0);
    CHECK_FALSE(sink_repair_component(sink, &wiring, "gun_1", 0.0));

    // heal the engine too: thrust attrs come back exactly.
    REQUIRE(sink_repair_component(sink, &wiring, "eng_1", 30.0));
    CHECK(pipe.derived("thrust.force").value() == 100.0f);
    CHECK(pipe.derived("power.draw.engine").value() == 5.0f);
    CHECK(eng->hp.value() == 30.0);
    // shield_gen stays dead: its regen stays zeroed while unrepaired
    CHECK(pipe.derived("shield.regen").value() == 0.0f);
}
