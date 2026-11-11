// 7.13 — on-foot verbs fire the SAME bus events as seated crew (#28b):
// identical (tick, seq, kind, a, b) streams and identical door-state
// deltas, actor never in the payload. The ONLY actor-dependence is the
// #19d suit-OK gate on atmosphere worlds.
#include <cstdint>
#include <string>
#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/doors.h"
#include "ve/core/sim/loadout.h"

using namespace ve::core;
namespace sim = ve::core::sim;

namespace {

// Same test-economy loadout as the airlock suite: socket id = type id.
sim::Loadout make_hull() {
    auto socket = [](std::string_view id) {
        return sim::socket_desc{.id = std::string(id),
                                .category = sim::socket_category::utility_};
    };
    auto mount = [](std::string_view instance, std::vector<std::string> tags) {
        return sim::mount_request{.instance_id = std::string(instance),
                                  .type_id = std::string(instance),
                                  .socket_id = std::string(instance),
                                  .attrs = {},
                                  .tags = std::move(tags)};
    };
    std::vector<sim::socket_desc> sockets{
        socket("door_inner"), socket("door_outer"),
        socket("airlock_1"), socket("hatch_1")};
    std::vector<sim::component_type> catalog;
    for (const auto& s : sockets) catalog.push_back({s.id, s.category});
    std::vector<sim::mount_request> mounts{
        mount("door_inner", {"door.toggle"}),
        mount("door_outer", {"door.toggle"}),
        mount("airlock_1", {"airlock.cycle"}),
        mount("hatch_1", {"door.toggle"})};
    sim::hull_info hull{.class_tag = "class.test",
                        .grid = sim::grid_block{2.0, 4, 2.5, 0.05},
                        .hull_mass = 1000.0};
    auto built = sim::Loadout::assemble(std::move(hull), std::move(sockets),
                                        std::move(catalog), std::move(mounts));
    REQUIRE(built.has_value());
    return std::move(*built);
}

constexpr std::uint32_t kInner = 0, kOuter = 1, kAirlock = 2, kHatch = 3;

bool same_streams(const std::vector<rail_event>& x,
                  const std::vector<rail_event>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i].tick != y[i].tick || x[i].seq != y[i].seq ||
            x[i].kind != y[i].kind || x[i].a != y[i].a || x[i].b != y[i].b) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_SUITE("foot_verb_events") {

TEST_CASE("toggle_stream_identical_foot_vs_seat") {
    // One toggle of the hatch door from three actors at the same tick:
    // seated, on-foot on an airless moon (7.12 walk), on-foot inside an
    // atmosphere-world suit-OK dome. Fresh bus + doors each run, so if
    // the executor is actor-agnostic the streams match field-for-field
    // INCLUDING seq numbers.
    const auto hull = make_hull();
    const sim::verb_actor seated{.id = 7, .on_foot = false, .zone_tags = {}};
    const sim::verb_actor foot_airless{.id = 41, .on_foot = true,
                                       .atm_rho0_kg_m3 = 0.0, .zone_tags = {}};
    const sim::verb_actor foot_dome{.id = 42, .on_foot = true,
                                    .atm_rho0_kg_m3 = 1.225,
                                    .zone_tags = {"zone.wilderness", "zone.suit_ok"}};

    auto run = [&](const sim::verb_actor& actor) {
        EffectEventBus bus;
        sim::DoorField doors(4);
        bus.begin_tick(500);
        auto r = sim::toggle_door(bus, hull, doors, kHatch, "door.toggle",
                                  "zone.suit_ok", actor);
        REQUIRE(r.has_value());
        bus.begin_tick(501);
        auto r2 = sim::toggle_door(bus, hull, doors, kHatch, "door.toggle",
                                   "zone.suit_ok", actor);
        REQUIRE(r2.has_value());
        return std::pair{bus.drain(), doors.open(kHatch)};
    };

    const auto [seat_ev, seat_open] = run(seated);
    const auto [foot_ev, foot_open] = run(foot_airless);
    const auto [dome_ev, dome_open] = run(foot_dome);

    // The hand-computed stream itself: flip open @500, flip closed @501,
    // payload = mount+1 + new state — the actor's id appears NOWHERE.
    REQUIRE(seat_ev.size() == 2);
    CHECK((seat_ev[0].tick == 500 && seat_ev[0].seq == 0));
    CHECK(seat_ev[0].kind == rail_kind::door_flip);
    CHECK(seat_ev[0].a == kHatch + 1);
    CHECK(seat_ev[0].b == 1);
    CHECK((seat_ev[1].tick == 501 && seat_ev[1].seq == 1));
    CHECK(seat_ev[1].b == 0);
    CHECK_FALSE(seat_open);  // final state after both flips

    CHECK(same_streams(seat_ev, foot_ev));
    CHECK(same_streams(seat_ev, dome_ev));
    CHECK(foot_open == seat_open);
    CHECK(dome_open == seat_open);
}

TEST_CASE("airlock_cycle_stream_identical_foot_vs_seat") {
    // Same cycle, same scripted chamber facts, foot vs seated: identical
    // streams. Facts are scripted directly here (the reservoir wiring is
    // pinned in the airlock_cycle suite; this case is about the verb).
    const auto hull = make_hull();
    const sim::verb_actor seated{.id = 7, .on_foot = false, .zone_tags = {}};
    const sim::verb_actor foot{.id = 41, .on_foot = true, .atm_rho0_kg_m3 = 0.0, .zone_tags = {}};

    auto run = [&](const sim::verb_actor& actor) {
        EffectEventBus bus;
        sim::DoorField doors(4);
        sim::Airlock lock(sim::airlock_def{kAirlock, kInner, kOuter});
        for (sim::tick_t t = 500; t <= 599; ++t) {
            bus.begin_tick(t);
            if (t == 500) {
                auto r = sim::cycle_airlock(bus, hull, doors, lock, true,
                                            "airlock.cycle", "zone.suit_ok", actor);
                REQUIRE(r.has_value());
            }
            // Chamber dries at 530: the outer door may open there.
            const sim::chamber_state vented{.vented = t >= 530, .pressurized = false};
            lock.step(bus, doors, vented);
            if (t == 559) {
                auto r = sim::cycle_airlock(bus, hull, doors, lock, false,
                                            "airlock.cycle", "zone.suit_ok", actor);
                REQUIRE(r.has_value());
            }
        }
        CHECK(lock.phase() == sim::airlock_phase::pressurizing);
        return bus.drain();
    };

    const auto seat_ev = run(seated);
    const auto foot_ev = run(foot);
    // Hand-computed: outer opens @530 (door_flip 1 + airlock_flip 1),
    // inward cycle @559 closes it (door_flip 0).
    REQUIRE(seat_ev.size() == 3);
    CHECK((seat_ev[0].tick == 530 && seat_ev[0].kind == rail_kind::door_flip));
    CHECK((seat_ev[0].a == kOuter + 1 && seat_ev[0].b == 1));
    CHECK((seat_ev[1].tick == 530 && seat_ev[1].kind == rail_kind::airlock_flip));
    CHECK((seat_ev[1].a == kAirlock + 1 && seat_ev[1].b == 1));
    CHECK((seat_ev[2].tick == 559 && seat_ev[2].kind == rail_kind::door_flip));
    CHECK(seat_ev[2].b == 0);
    CHECK(same_streams(seat_ev, foot_ev));
}

TEST_CASE("atmosphere_gates_foot_only_in_suit_ok_zones") {
    // #19d: an atmosphere world refuses on-foot verbs unless the actor's
    // zone carries the authored suit-OK tag. Refusal is a TOTAL no-op:
    // no event, no state delta. Seated crew are never gated here.
    const auto hull = make_hull();
    const sim::verb_actor foot{.id = 41, .on_foot = true,
                               .atm_rho0_kg_m3 = 1.225,
                               .zone_tags = {"zone.wilderness"}};

    // Pure gate predicate first (airless = suit-OK anywhere, the 7.12
    // walk; atmosphere needs the tag).
    const std::array<std::string, 1> kWild{"zone.wilderness"};
    const std::array<std::string, 2> kDome{"zone.wilderness", "zone.suit_ok"};
    CHECK(sim::suit_ok_zone(0.0, std::span<const std::string>{}, "zone.suit_ok"));
    CHECK_FALSE(sim::suit_ok_zone(1.225, kWild, "zone.suit_ok"));
    CHECK(sim::suit_ok_zone(1.225, kDome, "zone.suit_ok"));

    {
        EffectEventBus bus;
        sim::DoorField doors(4);
        sim::Airlock lock(sim::airlock_def{kAirlock, kInner, kOuter});
        bus.begin_tick(500);
        auto t = sim::toggle_door(bus, hull, doors, kHatch, "door.toggle",
                                  "zone.suit_ok", foot);
        CHECK(t.error() == sim::verb_error::not_suit_ok);
        auto c = sim::cycle_airlock(bus, hull, doors, lock, true,
                                    "airlock.cycle", "zone.suit_ok", foot);
        CHECK(c.error() == sim::verb_error::not_suit_ok);
        CHECK(bus.pending() == 0);            // nothing emitted
        CHECK_FALSE(doors.open(kHatch));      // nothing moved
        CHECK(lock.phase() == sim::airlock_phase::sealed);
    }

    // Same world, same actor, standing in a tagged zone: it fires, with
    // the byte-identical stream a seated crew would produce.
    const sim::verb_actor foot_dome{.id = 41, .on_foot = true,
                                    .atm_rho0_kg_m3 = 1.225,
                                    .zone_tags = {"zone.wilderness", "zone.suit_ok"}};
    {
        EffectEventBus bus;
        sim::DoorField doors(4);
        bus.begin_tick(500);
        auto r = sim::toggle_door(bus, hull, doors, kHatch, "door.toggle",
                                  "zone.suit_ok", foot_dome);
        REQUIRE(r.has_value());
        const auto ev = bus.drain();
        REQUIRE(ev.size() == 1);
        CHECK((ev[0].tick == 500 && ev[0].seq == 0));
        CHECK(ev[0].kind == rail_kind::door_flip);
        CHECK((ev[0].a == kHatch + 1 && ev[0].b == 1));
        CHECK(doors.open(kHatch));
    }

    // Seated crew on the SAME atmosphere world: world facts ignored
    // (their air problem is the hull sections, 7.11's job).
    const sim::verb_actor seated_atmo{.id = 7, .on_foot = false,
                                      .atm_rho0_kg_m3 = 1.225,
                                      .zone_tags = {"zone.wilderness"}};
    {
        EffectEventBus bus;
        sim::DoorField doors(4);
        bus.begin_tick(500);
        auto r = sim::toggle_door(bus, hull, doors, kHatch, "door.toggle",
                                  "zone.suit_ok", seated_atmo);
        REQUIRE(r.has_value());
        CHECK(bus.pending() == 1);
    }
}

TEST_CASE("gate_order_first_cause") {
    // Fixed gate order for scripted pinning: bounds -> facet -> suit-OK.
    const auto hull = make_hull();
    const sim::verb_actor foot_no_tag{.id = 41, .on_foot = true,
                                      .atm_rho0_kg_m3 = 1.225, .zone_tags = {}};
    EffectEventBus bus;
    sim::DoorField doors(4);
    bus.begin_tick(500);

    // Unknown mount outranks the missing tag:
    auto bad = sim::toggle_door(bus, hull, doors, 99, "door.toggle",
                                "zone.suit_ok", foot_no_tag);
    CHECK(bad.error() == sim::verb_error::unknown_component);
    // Facet check outranks the suit gate: this mount has no door.toggle.
    auto no_facet = sim::toggle_door(bus, hull, doors, kAirlock, "door.toggle",
                                     "zone.suit_ok", foot_no_tag);
    CHECK(no_facet.error() == sim::verb_error::missing_action_facet);
    CHECK(bus.pending() == 0);
}

}  // TEST_SUITE
