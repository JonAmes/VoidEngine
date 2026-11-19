// test: combat_repair_roundtrip — 9.15 docked-repair round-trip, the
// done-when (d) proof. Docking is a GAME-side context; core carries no
// "dock" concept (AGENTS rule 1) — a dock just calls the shipped repair
// verbs. This test drives the full loop at ship scale: the pre-combat
// stats bit state hashes a baseline, combat damage piles up until every
// wired system is offline (the offline modifiers zero their attrs), dock
// repair restores each component's health AND removes the offline tag,
// and the stats bit state hashes back EXACTLY equal to pre-combat —
// bit-exact, not "close enough" (6.5 raw-bit policy: the hash disagrees
// with anything even ±0.0-flavored).
//
// Fixture is the proven 9.11 component_offline_zeroing net; every number
// hand-computed. All pools hold dyadic integers so pins are exact ==
// (doctest Approx-trap note in the phase notes; nothing here needs it).
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/damage.h"
#include "ve/core/sim/gun.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_hash.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/wiring.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

constexpr std::string_view kOffline = "ship.component.offline";
}  // namespace

TEST_CASE("combat_repair_roundtrip") {
    // ======================= PRE-COMBAT SHIP =======================
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

    // Hand net: reactor_1 powers each system directly (9.11 fixture).
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

    damage_sink sink;
    sink_configure_shield(sink, pipe, shield_setup{
        .regen_attr = "shield.regen",
        .capacity_attr = "shield.capacity",
        .bands = {reservoir_band{0.0, "shield.down", band_dir::at_or_below}},
    });
    REQUIRE(sink_add_component(sink, "eng_1", 50.0, kOffline));
    REQUIRE(sink_add_component(sink, "shield_gen", 30.0, kOffline));
    REQUIRE(sink_add_component(sink, "gun_1", 20.0, kOffline));

    // The gun fires while alive (9.10 machinery, rof live from the net).
    gun_slot gun;
    gun.component = 1;
    gun_configure(gun, pipe, gun_attrs{.rof = "weapon.rof", .spread = "",
                                       .heat_limit = "", .heat_per_shot = "",
                                       .heat_leak = "", .overheat_tag = ""});
    REQUIRE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 0, 7).has_value());
    CHECK(gun.shots == 1);

    // THE BASELINE: read every attr (recompute-on-read), then hash.
    CHECK(pipe.derived("thrust.force").value() == 100.0f);
    CHECK(pipe.derived("shield.regen").value() == 60.0f);
    CHECK(pipe.derived("weapon.rof").value() == 2.0f);
    const std::uint64_t hash_pre = stats_state_hash(stats);

    // ========================= COMBAT =========================
    // Same hand-computed sequence as 9.11: shield (100/100) eats the first
    // 100 J, a second 50 J kills the engine; one regen step (+1.0) is eaten
    // by a 31 J hit that kills the generator; 20 J kills the gun. Pile-up:
    // ALL wired systems offline at once.
    auto e1 = sink_apply_hit(sink, &wiring, "eng_1", 100.0);
    REQUIRE(e1.has_value());
    CHECK(e1->absorbed == 100.0);
    auto e2 = sink_apply_hit(sink, &wiring, "eng_1", 50.0);
    REQUIRE(e2.has_value());
    CHECK(e2->went_offline);
    sink.shield.step();  // regen still live: pool 0 -> +1.0
    auto s1 = sink_apply_hit(sink, &wiring, "shield_gen", 31.0);
    REQUIRE(s1.has_value());
    CHECK(s1->went_offline);
    auto g1 = sink_apply_hit(sink, &wiring, "gun_1", 20.0);
    REQUIRE(g1.has_value());
    CHECK(g1->went_offline);

    // Post-combat: every function attr zeroed, the dark set is exactly the
    // three wired systems (registration order), and the gun fail-safe stops.
    CHECK(pipe.derived("thrust.force").value() == 0.0f);
    CHECK(pipe.derived("shield.regen").value() == 0.0f);
    CHECK(pipe.derived("weapon.rof").value() == 0.0f);
    CHECK(wiring.dark_set() ==
          std::vector<std::string>{"eng_1", "shield_gen", "gun_1"});
    CHECK_FALSE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 200, 7).has_value());
    CHECK(gun.shots == 1);

    // NON-VACUITY: the combat damage really moved the stats bit state.
    const std::uint64_t hash_combat = stats_state_hash(stats);
    CHECK(hash_combat != hash_pre);

    // ================== DOCK REPAIR (the round-trip) ==================
    // Refusals first: non-positive and unknown-ref repairs do NOTHING —
    // the engine must stay dark behind them.
    CHECK_FALSE(sink_repair_component(sink, &wiring, "eng_1", 0.0));
    CHECK_FALSE(sink_repair_component(sink, &wiring, "nope_1", 10.0));
    CHECK_FALSE(wiring.lit("eng_1"));
    CHECK(pipe.derived("thrust.force").value() == 0.0f);

    // Partial dock work: any positive value re-opens the band and revives
    // the system in the net (hp 10 < ceiling 50, but thrust is live again).
    REQUIRE(sink_repair_component(sink, &wiring, "eng_1", 10.0));
    CHECK(wiring.lit("eng_1"));
    CHECK(pipe.derived("thrust.force").value() == 100.0f);
    component_hp* eng = sink.comps[0].get();
    component_hp* shield_gen_c = sink.comps[1].get();
    component_hp* gun_c = sink.comps[2].get();
    CHECK(eng->hp.value() == 10.0);

    // Full dock repair: health back to ceiling (over-repair clamps at the
    // set_value door), offline tags off everywhere.
    REQUIRE(sink_repair_component(sink, &wiring, "eng_1", 50.0));
    REQUIRE(sink_repair_component(sink, &wiring, "shield_gen", 30.0));
    REQUIRE(sink_repair_component(sink, &wiring, "gun_1", 20.0));
    REQUIRE(sink_repair_component(sink, &wiring, "gun_1", 999.0));  // clamp
    CHECK(eng->hp.value() == 50.0);
    CHECK(shield_gen_c->hp.value() == 30.0);
    CHECK(gun_c->hp.value() == 20.0);
    for (const component_hp* c : {eng, shield_gen_c, gun_c})
        CHECK_FALSE(c->tags.contains_exact(std::string(kOffline)));
    CHECK(wiring.dark_set().empty());

    // =================== EXACT PRE-COMBAT STATS ===================
    // Attrs bit-identical to the pre-combat values, systems behave again,
    // and the whole stats bit state hashes EQUAL to the pre-combat hash.
    CHECK(pipe.derived("thrust.force").value() == 100.0f);
    CHECK(pipe.derived("power.draw.engine").value() == 5.0f);
    CHECK(pipe.derived("shield.regen").value() == 60.0f);
    CHECK(pipe.derived("weapon.rof").value() == 2.0f);
    CHECK(pipe.derived("power.draw.gun").value() == 4.0f);
    sink.shield.step();  // regen live again: pool 0 -> exactly +1.0
    CHECK(sink.shield.value() == 1.0);
    REQUIRE(gun_try_fire(gun, {1.0, 0.0, 0.0}, 300, 7).has_value());
    CHECK(gun.shots == 2);
    CHECK(stats_state_hash(stats) == hash_pre);
}
