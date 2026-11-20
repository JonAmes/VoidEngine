// test: breach_venting_band + breach_patch_repair — 9.12 section breach
// (PLAN #19c). Every number hand-computed; all values dyadic (cap 100,
// rate 2.5, refill 60 -> exactly +1.0/tick) so pins are exact ==.
//
// breach_venting_band: breach opens in cabin, venting band {95, atmo.
// venting, at_or_below} throws at the EXACT tick value lands on 95,
// door 0 closed before tick 3 freezes dock_ring at exactly 95.0 forever
// (isolated), un-doored eng_room keeps venting, empty band throws at the
// exact tick value hits 0.0 and the floor keeps it exactly 0.0. The
// section TagSet flips a shipped condition gate (the #18d occupant KO
// path = data gated on these tags — zero new machinery). Refusals
// (unknown section, rate <= 0/NaN, duplicate breach) change nothing.
//
// breach_patch_repair: leak drains 2.5/tick for 10 ticks (75.0 exact),
// patch stops the leak (second patch = false), refill attr 60 J/s gives
// exactly +1/tick, venting tag un-throws the first tick above 95
// (hysteresis-free), refill clamps at ceiling exactly 100.0.
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/breach.h"
#include "ve/core/sim/effects.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

std::vector<reservoir_band> pressure_bands() {
    return {reservoir_band{95.0, "atmo.venting", band_dir::at_or_below},
            reservoir_band{0.0, "atmo.empty", band_dir::at_or_below}};
}

section_air make_field(StatsPipeline& pipe) {
    section_air field;
    std::vector<section_air_setup> setups{
        section_air_setup{"cabin", 100.0, pressure_bands(), ""},
        section_air_setup{"dock_ring", 100.0, pressure_bands(), ""},
        section_air_setup{"eng_room", 100.0, pressure_bands(), ""}};
    std::vector<air_edge> edges{
        air_edge{0, 1, std::uint32_t{0}},  // cabin <-> dock_ring: door 0
        air_edge{0, 2, std::nullopt}};      // cabin <-> eng_room: no door
    CHECK(air_configure(field, setups, edges, pipe));
    return field;
}
}  // namespace

TEST_CASE("breach_venting_band") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);

    section_air field = make_field(pipe);
    REQUIRE(field.sections.size() == 3);
    CHECK(field.air("cabin")->value() == 100.0);   // spawn law: authored full
    CHECK(field.air("dock_ring")->value() == 100.0);

    // Refusals change NOTHING (fail-SAFE on the breach door).
    CHECK_FALSE(field.breach_open("bogus", 2.5));
    CHECK_FALSE(field.breach_open("cabin", -1.0));
    CHECK_FALSE(field.breach_open("cabin", std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(field.is_breached("cabin"));
    CHECK(field.air("bogus") == nullptr);
    CHECK_FALSE(field.breach_patch("cabin"));  // nothing patched

    REQUIRE(field.breach_open("cabin", 2.5));
    CHECK(field.is_breached("cabin"));
    CHECK_FALSE(field.breach_open("cabin", 2.5));  // idempotent

    DoorField doors(1);
    doors.set_open(0, true);  // door open: air flows through the edge

    condition occupant_ok;
    occupant_ok.forbid_tags = {"atmo.empty"};  // #18d KO path = gate on tag

    // t=1: connected region = {cabin, dock_ring, eng_room}: all -> 97.5.
    field.step(doors);
    CHECK(field.air("cabin")->value() == 97.5);
    CHECK(field.air("dock_ring")->value() == 97.5);
    CHECK(field.air("eng_room")->value() == 97.5);
    CHECK_FALSE(field.section_tags("cabin")->contains_exact("atmo.venting"));

    // t=2: 95.0 EXACT -> venting band throws this tick, not the next.
    field.step(doors);
    CHECK(field.air("cabin")->value() == 95.0);
    CHECK(field.section_tags("cabin")->contains_exact("atmo.venting"));
    CHECK(field.section_tags("dock_ring")->contains_exact("atmo.venting"));

    // Shut the door BEFORE step 3: dock_ring froze after exactly 2 drains.
    doors.set_open(0, false);
    field.step(doors);  // t=3: breach side drains again (7 * 2.5 = 17.5 gone)
    CHECK(field.air("dock_ring")->value() == 95.0);  // isolated from here on
    CHECK(field.air("cabin")->value() == 92.5);      // breach side keeps going
    CHECK(field.air("eng_room")->value() == 92.5);   // no door: still draining

    // t=3..40: cabin + eng_room 37 more drains: 95.0 - 37*2.5 = 2.5 at t=39,
    // 0.0 EXACT at t=40; floor keeps it 0.0 at t=41 (never negative).
    for (int t = 4; t <= 39; ++t) field.step(doors);
    CHECK(field.air("cabin")->value() == 2.5);
    CHECK_FALSE(field.section_tags("cabin")->contains_exact("atmo.empty"));
    CHECK(condition_met(occupant_ok, field.section_tags("cabin"), nullptr));
    field.step(doors);  // t=40
    CHECK(field.air("cabin")->value() == 0.0);
    CHECK(field.air("eng_room")->value() == 0.0);
    CHECK(field.section_tags("cabin")->contains_exact("atmo.empty"));
    CHECK_FALSE(condition_met(occupant_ok, field.section_tags("cabin"), nullptr));
    field.step(doors);  // t=41: dry section stays EXACTLY 0.0
    CHECK(field.air("cabin")->value() == 0.0);
    // The frozen neighbor stayed at 95.0 through all of it.
    CHECK(field.air("dock_ring")->value() == 95.0);
}

TEST_CASE("breach_patch_repair") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);
    stats.set_base("air.refill", 0.0f);  // pump OFF until the patch

    section_air field;
    std::vector<section_air_setup> cabin_only{
        section_air_setup{"cabin", 100.0, pressure_bands(), "air.refill"}};
    REQUIRE(air_configure(field, cabin_only, std::span<const air_edge>{}, pipe));

    REQUIRE(field.breach_open("cabin", 2.5));
    DoorField doors(0);
    for (int t = 1; t <= 10; ++t) field.step(doors);
    CHECK(field.air("cabin")->value() == 75.0);  // 100 - 10*2.5 exact
    CHECK(field.section_tags("cabin")->contains_exact("atmo.venting"));

    REQUIRE(field.breach_patch("cabin"));
    CHECK_FALSE(field.is_breached("cabin"));
    CHECK_FALSE(field.breach_patch("cabin"));  // second patch = false
    CHECK_FALSE(field.breach_patch("bogus"));

    // Pump ON at 60 J/s: +1.0 EXACT per tick (60*(1/60) == 1.0).
    stats.set_base("air.refill", 60.0f);
    for (int t = 11; t <= 30; ++t) field.step(doors);
    CHECK(field.air("cabin")->value() == 95.0);
    CHECK(field.section_tags("cabin")->contains_exact("atmo.venting"));  // 95 <= 95
    field.step(doors);  // t=31: 96.0 -> tag un-throws (hysteresis-free)
    CHECK(field.air("cabin")->value() == 96.0);
    CHECK_FALSE(field.section_tags("cabin")->contains_exact("atmo.venting"));

    for (int t = 32; t <= 36; ++t) field.step(doors);
    CHECK(field.air("cabin")->value() == 100.0);  // ceiling clamp, exact
    field.step(doors);
    CHECK(field.air("cabin")->value() == 100.0);  // stays full under the pump
}

TEST_CASE("breach_authoring_refusals") {
    StatsSet stats;
    ModifierStack stack;
    StatsPipeline pipe(stats, stack);

    section_air dup;
    std::vector<section_air_setup> dups{
        section_air_setup{"a", 10.0, {}, ""}, section_air_setup{"a", 10.0, {}, ""}};
    CHECK_FALSE(air_configure(dup, dups, std::span<const air_edge>{}, pipe));
    CHECK(dup.sections.empty());  // no partial field left behind

    section_air bad_edge;
    std::vector<section_air_setup> one{section_air_setup{"a", 10.0, {}, ""}};
    std::vector<air_edge> loop{air_edge{0, 0, std::nullopt}};
    CHECK_FALSE(air_configure(bad_edge, one, loop, pipe));
    CHECK(bad_edge.sections.empty());

    section_air bad_cap;
    std::vector<section_air_setup> zero_cap{section_air_setup{"a", 0.0, {}, ""}};
    CHECK_FALSE(air_configure(bad_cap, zero_cap, std::span<const air_edge>{}, pipe));
    CHECK(bad_cap.sections.empty());
}
