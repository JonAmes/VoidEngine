// 5.12 — cargo hold + manifest (#20a). One loadout data model: holds are
// ordinary tagged mounts; the manifest is ordered, atomic, and concept-free
// (the hold tag, capacity key and gate prefix arrive from game/ headers —
// core never names "cargo", "hold", or any commodity).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ve/core/sim/cargo.h"
#include "ve/core/sim/loadout.h"
#include "ve/game/attr_keys.h"
#include "ve/game/cargo_tags.h"

using namespace ve::core::sim;
using namespace std::string_literals;

namespace {

Loadout cargo_ship() {
    hull_info h;
    h.class_tag = "class.freighter";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 20000.0;
    std::vector<socket_desc> socks = {
        {"hp_bulk", socket_category::utility_, {}, {}},
        {"hp_tank", socket_category::utility_, {}, {}},
        {"hp_open", socket_category::utility_, {}, {}},
        {"hp_engine_L", socket_category::main_, {}, {}},
    };
    std::vector<component_type> cat = {
        {"cargo.hold.bulk", socket_category::utility_},
        {"cargo.tank.ferrous", socket_category::utility_},
        {"cargo.hold.open", socket_category::utility_},
        {"thruster.main", socket_category::main_},
    };
    std::vector<mount_request> mounts = {
        // bulk hold: gated to fuel + ore. gate tags ride alongside the hold
        // tag; a facet tag that is NOT a gate stays ignored by the manifest.
        {"bulk_1", "cargo.hold.bulk", "hp_bulk",
         {{std::string(ve::game::kAttr_cargo_capacity), 400.0}},
         {std::string(ve::game::kTag_cargo_hold), "commodity.fuel",
          "commodity.ore", "repair.target"}},
        // ferrous-only tank: name says fuel, gates say fuel (opaque ids).
        {"tank_1", "cargo.tank.ferrous", "hp_tank",
         {{std::string(ve::game::kAttr_cargo_capacity), 100.0}},
         {std::string(ve::game::kTag_cargo_hold), "commodity.fuel"}},
        // open hold: NO gate tags -> accepts everything.
        {"open_1", "cargo.hold.open", "hp_open",
         {{std::string(ve::game::kAttr_cargo_capacity), 50.0}},
         {std::string(ve::game::kTag_cargo_hold)}},
        {"engine_L", "thruster.main", "hp_engine_L", {}, {}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                               std::move(mounts));
    REQUIRE_MESSAGE(r.has_value(), r.error());
    return std::move(*r);
}

CargoManifest make_manifest(const Loadout& lo) {
    return CargoManifest(lo, ve::game::kTag_cargo_hold,
                         ve::game::kAttr_cargo_capacity,
                         ve::game::kPrefix_commodity_gate);
}

}  // namespace

TEST_CASE("loadout_cargo") {
    auto lo = cargo_ship();
    auto m = make_manifest(lo);

    SUBCASE("holds come from tagged mounts in mount order") {
        REQUIRE(m.holds().size() == 3);  // the untagged engine is not a hold
        CHECK(m.holds()[0].mount_id == "bulk_1");
        CHECK(m.holds()[0].capacity == 400.0);
        REQUIRE(m.holds()[0].gates.size() == 2);  // repair.target is not a gate
        CHECK(m.holds()[0].gates[0] == "fuel");
        CHECK(m.holds()[0].gates[1] == "ore");
        CHECK(m.holds()[1].gates == std::vector<std::string>{"fuel"});
        CHECK(m.holds()[2].gates.empty());  // open hold: accepts everything
    }

    SUBCASE("first-fit fill in mount order; split across holds") {
        REQUIRE(m.add_commodity("fuel", 300.0).has_value());
        CHECK(m.quantity_of("fuel") == 300.0);
        REQUIRE(m.stacks().size() == 1);
        REQUIRE(m.stacks()[0].placements.size() == 1);  // all in bulk_1 (400 free)
        CHECK(m.stacks()[0].placements[0].hold_index == 0);

        // 200 more: 100 left in bulk_1, remainder spills into tank_1.
        REQUIRE(m.add_commodity("fuel", 200.0).has_value());
        CHECK(m.quantity_of("fuel") == 500.0);
        REQUIRE(m.stacks()[0].placements.size() == 2);
        CHECK(m.stacks()[0].placements[0] == placement_desc{0, 400.0});
        CHECK(m.stacks()[0].placements[1] == placement_desc{1, 100.0});

        // stacks iterate in insertion order (#5).
        REQUIRE(m.add_commodity("grain", 30.0).has_value());  // open hold only
        CHECK(m.stacks()[0].commodity_id == "fuel");
        CHECK(m.stacks()[1].commodity_id == "grain");
    }

    SUBCASE("removal drains reverse mount order; empty stacks leave") {
        REQUIRE(m.add_commodity("fuel", 500.0).has_value());
        REQUIRE(m.remove_commodity("fuel", 150.0).has_value());  // tank 100 + bulk 50
        CHECK(m.quantity_of("fuel") == 350.0);
        REQUIRE(m.stacks()[0].placements.size() == 1);
        CHECK(m.stacks()[0].placements[0] == placement_desc{0, 350.0});

        REQUIRE(m.remove_commodity("fuel", 350.0).has_value());
        CHECK(m.stacks().empty());  // fully drained stacks are erased
        CHECK(m.quantity_of("fuel") == 0.0);
    }

    SUBCASE("gates: no accepting hold is no_gate, not overflow") {
        auto e = m.remove_commodity("ore", 10.0);
        REQUIRE_FALSE(e.has_value());
        CHECK(e.error() == cargo_error::unknown_commodity);

        CHECK(m.quantity_of("water") == 0.0);  // open hold accepts EVERYTHING,
        REQUIRE(m.add_commodity("water", 10.0).has_value());  // so water fits
        // a gates-only ship would instead give no_gate; verify with a hull
        // whose holds are all gated:
        hull_info h;
        h.class_tag = "class.gated";
        h.grid = grid_block{2.0, 4, 2.5, 0.05};
        std::vector<socket_desc> socks = {
            {"hp_bulk", socket_category::utility_, {}, {}},
        };
        std::vector<component_type> cat = {
            {"cargo.hold.bulk", socket_category::utility_},
        };
        std::vector<mount_request> mounts = {
            {"bulk_1", "cargo.hold.bulk", "hp_bulk",
             {{std::string(ve::game::kAttr_cargo_capacity), 100.0}},
             {std::string(ve::game::kTag_cargo_hold), "commodity.fuel"}},
        };
        auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                                   std::move(mounts));
        REQUIRE(r.has_value());
        auto gm = make_manifest(*r);
        auto e2 = gm.add_commodity("water", 1.0);
        REQUIRE_FALSE(e2.has_value());
        CHECK(e2.error() == cargo_error::no_gate);
    }

    SUBCASE("overflow is atomic: nothing is stored") {
        auto e = m.add_commodity("ore", 500.0);  // bulk 400 max (tank+open refuse ore)
        REQUIRE_FALSE(e.has_value());
        CHECK(e.error() == cargo_error::overflow);
        CHECK(m.quantity_of("ore") == 0.0);
        CHECK(m.stacks().empty());
        // open hold (50) does NOT gate-match ore, so it never counts as room.
    }

    SUBCASE("quantity validation is first") {
        CHECK(m.add_commodity("fuel", 0.0).error() == cargo_error::invalid_quantity);
        CHECK(m.add_commodity("fuel", -5.0).error() == cargo_error::invalid_quantity);
        CHECK(m.add_commodity("", 5.0).error() == cargo_error::invalid_id);
        CHECK(m.remove_commodity("fuel", -1.0).error() == cargo_error::invalid_quantity);
    }

    SUBCASE("spare gear roster (#24c spares; mounting happens in the hangar)") {
        REQUIRE(m.add_spare("eng_R", "thruster.main").has_value());
        REQUIRE(m.add_spare("gun_spare", "gun.autocannon").has_value());
        auto e = m.add_spare("eng_R", "thruster.main");  // dup instance id
        REQUIRE_FALSE(e.has_value());
        CHECK(e.error() == cargo_error::duplicate_spare);
        REQUIRE(m.stacks().empty());  // spares never occupy commodity capacity

        REQUIRE(m.add_commodity("fuel", 10.0).has_value());  // mixed roster
        CHECK(m.stacks()[0].commodity_id == "fuel");         // stacks stay clean

        auto e2 = m.remove_spare("nope");
        REQUIRE_FALSE(e2.has_value());
        CHECK(e2.error() == cargo_error::unknown_spare);
        REQUIRE(m.remove_spare("eng_R").has_value());
        REQUIRE(m.spares().size() == 1);
        CHECK(m.spares()[0].instance_id == "gun_spare");  // roster order kept
    }
}
