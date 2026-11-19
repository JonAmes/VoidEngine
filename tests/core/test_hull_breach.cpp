// test: hull_breach_threshold + offline_pileup_never_destroys +
// hull_integrity_authoring — 9.14 whole-ship destruction (PLAN #19c
// dramatic tier; mechanism ratified as user decision #56: ship-wide
// hull-integrity Reservoir, the 9.11 single shield pool pattern).
//
// hull_breach_threshold: the destruction ladder is HAND-COMPUTED —
// capacity 60, hits 25 + 25 + 10 land on 0.0 at the third hit; the
// authored band tag throws on the hull's own TagSet at that landing,
// is_destroyed flips exactly there, and further hits refuse (terminal).
// Breach AIR venting runs for ticks at full rate and never drains the
// pool; the wreck still vents after destruction (air keeps stepping).
//
// offline_pileup_never_destroys: the 9.11 component_offline_zeroing
// fixture driven to ALL components offline (dark set = every system,
// every derived attr zero) — the integrity pool sits untouched at full
// capacity and the hull is not destroyed. Component accumulation can
// never reach the dramatic threshold; only structure hits can.
//
// hull_integrity_authoring: bad capacity refused leaving the tier
// unconfigured, reconfigure refused, hit on unconfigured tier refused.
//
// All pools hold dyadic integers so pins are exact == (9.11 notes trap).
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/breach.h"
#include "ve/core/sim/damage.h"
#include "ve/core/sim/doors.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/wiring.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

constexpr std::string_view kOffline = "ship.component.offline";
constexpr std::string_view kBreached = "hull.breached";

std::vector<reservoir_band> air_bands() {
    return {reservoir_band{95.0, "atmo.venting", band_dir::at_or_below},
            reservoir_band{0.0, "atmo.empty", band_dir::at_or_below}};
}
}  // namespace

TEST_CASE("hull_breach_threshold") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);

    section_air field;
    std::vector<section_air_setup> setups{
        section_air_setup{"cabin", 100.0, air_bands(), ""}};
    REQUIRE(air_configure(field, setups, std::span<const air_edge>{}, pipe));

    // The dramatic tier: capacity 60, zero-crossing throws the authored tag.
    REQUIRE(integrity_configure(field, hull_integrity_setup{
                                             .capacity = 60.0,
                                             .bands = {reservoir_band{
                                                 0.0, std::string(kBreached),
                                                 band_dir::at_or_below}},
                                         }));
    REQUIRE(field.integrity_value());
    CHECK(field.integrity_value()->value() == 60.0);
    CHECK_FALSE(field.is_destroyed());

    // Refusals change nothing (fail-SAFE).
    CHECK_FALSE(field.integrity_hit(0.0));
    CHECK_FALSE(field.integrity_hit(-5.0));
    CHECK_FALSE(field.integrity_hit(std::numeric_limits<double>::quiet_NaN()));
    CHECK(field.integrity_value()->value() == 60.0);

    // Hit 1: 60 - 25 = 35, no destruction, no tag.
    REQUIRE(field.integrity_hit(25.0));
    CHECK(field.integrity_value()->value() == 35.0);
    CHECK_FALSE(field.is_destroyed());
    CHECK_FALSE(field.hull_tags()->contains_exact(std::string(kBreached)));

    // Breach AIR NEVER DRAINS integrity: open a breach and vent for 4
    // ticks (cabin 100 - 4*2.5 = 90.0 exactly, venting tag live there —
    // the SMALL tier doing its thing while the dramatic tier holds).
    REQUIRE(field.breach_open("cabin", 2.5));
    DoorField doors(1);
    for (int t = 0; t < 4; ++t) field.step(doors);
    CHECK(field.air("cabin")->value() == 90.0);
    CHECK(field.section_tags("cabin")->contains_exact("atmo.venting"));
    CHECK(field.integrity_value()->value() == 35.0);
    CHECK_FALSE(field.is_destroyed());

    // Hit 2: 35 - 25 = 10, still alive. Hit 3 lands EXACTLY on 0.0:
    // the dramatic hull breach, this tick.
    REQUIRE(field.integrity_hit(25.0));
    CHECK(field.integrity_value()->value() == 10.0);
    CHECK_FALSE(field.is_destroyed());
    REQUIRE(field.integrity_hit(10.0));
    CHECK(field.integrity_value()->value() == 0.0);
    CHECK(field.is_destroyed());
    CHECK(field.hull_tags()->contains_exact(std::string(kBreached)));

    // TERMINAL event: further hits refuse with zero state change.
    CHECK_FALSE(field.integrity_hit(50.0));
    CHECK(field.integrity_value()->value() == 0.0);
    CHECK(field.is_destroyed());

    // The wreck keeps simulating: air still vents post-destruction
    // (90.0 - 2.5 = 87.5; Phase 11 owns what a destroyed hull means
    // downstream, 9.14 only marks the event).
    field.step(doors);
    CHECK(field.air("cabin")->value() == 87.5);
}

TEST_CASE("offline_pileup_never_destroys") {
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

    // Hand net (the 9.11 component_offline_zeroing fixture): reactor_1
    // powers eng_1, shield_gen, gun_1 directly.
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

    // Dramatic tier configured and FULL.
    section_air field;
    std::vector<section_air_setup> setups{
        section_air_setup{"cabin", 100.0, air_bands(), ""}};
    REQUIRE(air_configure(field, setups, std::span<const air_edge>{}, pipe));
    REQUIRE(integrity_configure(field, hull_integrity_setup{
                                             .capacity = 60.0,
                                             .bands = {reservoir_band{
                                                 0.0, std::string(kBreached),
                                                 band_dir::at_or_below}},
                                         }));

    damage_sink sink;
    sink_configure_shield(sink, pipe, shield_setup{
        .regen_attr = "shield.regen",
        .capacity_attr = "shield.capacity",
        .bands = {reservoir_band{0.0, "shield.down", band_dir::at_or_below}},
    });
    REQUIRE(sink_add_component(sink, "eng_1", 50.0, kOffline));
    REQUIRE(sink_add_component(sink, "shield_gen", 30.0, kOffline));
    REQUIRE(sink_add_component(sink, "gun_1", 20.0, kOffline));

    // Kill EVERYTHING: shield eats 100, eng dies to 50; one regen tick
    // then shield_gen dies to 31; gun dies to 20 (9.11's exact ladder).
    auto e1 = sink_apply_hit(sink, &wiring, "eng_1", 100.0);
    REQUIRE(e1.has_value());
    auto e2 = sink_apply_hit(sink, &wiring, "eng_1", 50.0);
    REQUIRE(e2.has_value());
    CHECK(e2->went_offline);
    sink.shield.step();
    auto s1 = sink_apply_hit(sink, &wiring, "shield_gen", 31.0);
    REQUIRE(s1.has_value());
    CHECK(s1->went_offline);
    auto g1 = sink_apply_hit(sink, &wiring, "gun_1", 20.0);
    REQUIRE(g1.has_value());
    CHECK(g1->went_offline);

    // PILE-UP CONFIRMED: every system dark, every derived attr zero.
    auto dark = wiring.dark_set();
    REQUIRE(dark.size() == 3);
    CHECK(pipe.derived("thrust.force").value() == 0.0f);
    CHECK(pipe.derived("shield.regen").value() == 0.0f);
    CHECK(pipe.derived("weapon.rof").value() == 0.0f);

    // ...and the hull is NOT destroyed: offline accumulation never drains
    // the integrity pool (separate domains — the guard PLAN demands).
    CHECK_FALSE(field.is_destroyed());
    CHECK(field.integrity_value()->value() == 60.0);
    CHECK_FALSE(field.hull_tags()->contains_exact(std::string(kBreached)));
}

TEST_CASE("hull_integrity_authoring") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);

    section_air field;
    std::vector<section_air_setup> setups{
        section_air_setup{"a", 10.0, air_bands(), ""}};
    REQUIRE(air_configure(field, setups, std::span<const air_edge>{}, pipe));

    // Bad capacities refused, tier stays unconfigured, hit refused.
    CHECK_FALSE(integrity_configure(field, hull_integrity_setup{.capacity = 0.0, .bands = {}}));
    CHECK_FALSE(integrity_configure(field, hull_integrity_setup{.capacity = -1.0, .bands = {}}));
    CHECK_FALSE(integrity_configure(field, hull_integrity_setup{
                                              .capacity = std::numeric_limits<
                                                  double>::quiet_NaN(), .bands = {}}));
    CHECK_FALSE(field.integrity_value());
    CHECK_FALSE(field.integrity_hit(10.0));

    // Good configure, then reconfigure refused (no pool swap mid-flight).
    REQUIRE(integrity_configure(field, hull_integrity_setup{
                                             .capacity = 8.0, .bands = {}}));
    CHECK_FALSE(integrity_configure(field, hull_integrity_setup{.capacity = 99.0, .bands = {}}));
    CHECK(field.integrity_value()->value() == 8.0);

    // Overshoot: 9 J onto an 8 J pool floors at exactly 0.0 = destroyed.
    REQUIRE(field.integrity_hit(9.0));
    CHECK(field.integrity_value()->value() == 0.0);
    CHECK(field.is_destroyed());
}
