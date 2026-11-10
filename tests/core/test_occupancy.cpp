// Phase 5 task 5.11 — seats + occupancy (#18): seat records derive from
// tagged mounts via a game-side role registry; sit/stand/dark are atomic;
// dead-until-manned is a plain query; a station is just a big hull with the
// same schema (#24a). Modifier-stack attachment is the Phase 6 seam.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ve/core/sim/loadout.h"
#include "ve/core/sim/occupancy.h"
#include "ve/game/role_tags.h"
#include "ve/game/role_tags.h"

namespace {

using namespace ve::core::sim;

std::vector<std::string> role_registry() {
    return {ve::game::kPhase5_role_tags.begin(), ve::game::kPhase5_role_tags.end()};
}

// Shuttle: turret + engine (untagged machinery) and two role seats.
Loadout ship_loadout() {
    hull_info h;
    h.class_tag = "class.shuttle";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 9000.0;
    std::vector<socket_desc> socks = {
        {"hp_turret", socket_category::gun_, {}, {}},
        {"hp_engine_L", socket_category::main_, {}, {}},
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
        {"hp_seat_gunner", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {
        {"turret.autocannon", socket_category::gun_},
        {"thruster.main", socket_category::main_},
        {"seat.station", socket_category::utility_},
    };
    std::vector<mount_request> mounts = {
        {"turret_1", "turret.autocannon", "hp_turret", {}, {}},
        {"engine_L", "thruster.main", "hp_engine_L", {}, {}},
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
        {"seat_gunner", "seat.station", "hp_seat_gunner", {}, {"station.gunner"}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                               std::move(mounts));
    REQUIRE(r.has_value());
    return std::move(*r);
}

}  // namespace

TEST_CASE("loadout_seats") {
    const Loadout lo = ship_loadout();
    const std::vector<std::string> reg = role_registry();

    // Seats = tagged mounts, in mount order; machinery never becomes a seat.
    Occupancy occ(lo, reg);
    REQUIRE(occ.seats().size() == 2);
    CHECK(occ.seats()[0].instance_id == "seat_pilot");  // mount order kept (#5)
    CHECK(occ.seats()[1].instance_id == "seat_gunner");
    CHECK(occ.seats()[0].role_tags == std::vector<std::string>{"station.pilot"});
    CHECK_FALSE(occ.occupant_at("seat_pilot").has_value());

    // #18a dead-until-manned: a turret exists but is not a seat, and the
    // gunner seat is empty -> no gunner function runs, no fallback.
    CHECK_FALSE(occ.manned("station.gunner"));
    CHECK_FALSE(occ.function_available("turret_1"));  // not a seat at all
    CHECK_FALSE(occ.function_available("seat_pilot"));

    // Sit / occupancy queries.
    REQUIRE(occ.sit(42, "seat_pilot").has_value());
    CHECK(occ.manned("station.pilot"));
    CHECK(occ.occupant_at("seat_pilot") == 42u);
    REQUIRE(occ.seat_of(42).has_value());
    CHECK(*occ.seat_of(42) == "seat_pilot");
    CHECK(occ.function_available("seat_pilot"));

    // Someone else's seat is busy: refused, no change.
    CHECK(occ.sit(7, "seat_pilot").error() == seat_error::seat_busy);
    CHECK(occ.occupant_at("seat_pilot") == 42u);

    // sit() MOVES an already-seated occupant atomically (swap recomputes).
    REQUIRE(occ.sit(42, "seat_gunner").has_value());
    CHECK_FALSE(occ.occupant_at("seat_pilot").has_value());
    CHECK_FALSE(occ.manned("station.pilot"));
    CHECK(occ.occupant_at("seat_gunner") == 42u);
    CHECK(*occ.seat_of(42) == "seat_gunner");

    // stand, and the two refusal paths.
    REQUIRE(occ.stand("seat_gunner").has_value());
    CHECK(occ.stand("seat_gunner").error() == seat_error::not_seated);
    CHECK(occ.sit(9, "turret_1").error() == seat_error::unknown_seat);
    CHECK(occ.stand("nope").error() == seat_error::unknown_seat);

    // #18d dark-not-dead: a hit darkens the station; the occupant stays,
    // availability drops as a unit (the gunner-KO'd-turret-goes-silent gate).
    REQUIRE(occ.sit(7, "seat_gunner").has_value());
    CHECK(occ.manned("station.gunner"));
    REQUIRE(occ.set_dark("seat_gunner", true).has_value());
    CHECK(occ.occupant_at("seat_gunner") == 7u);   // sitter untouched
    CHECK_FALSE(occ.function_available("seat_gunner"));
    CHECK_FALSE(occ.manned("station.gunner"));     // turret goes silent
    REQUIRE(occ.set_dark("seat_gunner", false).has_value());
    CHECK(occ.manned("station.gunner"));           // repair restores the unit

    // Multi-role seat (#18c hiring flexibility): one mount, two roles.
    {
        hull_info h;
        h.class_tag = "class.shuttle";
        h.grid = grid_block{2.0, 4, 2.5, 0.05};
        std::vector<socket_desc> socks = {
            {"hp_seat_xo", socket_category::utility_, {}, {}},
        };
        std::vector<component_type> cat = {{"seat.station", socket_category::utility_}};
        std::vector<mount_request> mounts = {
            {"seat_xo", "seat.station", "hp_seat_xo", {},
             {"station.navigator", "station.engineer"}},  // def order kept
        };
        auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                                   std::move(mounts));
        REQUIRE(r.has_value());
        Occupancy o2(*r, reg);
        REQUIRE(o2.seats().size() == 1);
        CHECK(o2.seats()[0].role_tags ==
              std::vector<std::string>{"station.navigator", "station.engineer"});
        REQUIRE(o2.sit(5, "seat_xo").has_value());
        CHECK(o2.manned("station.navigator"));
        CHECK(o2.manned("station.engineer"));  // same sitter covers both
    }

    // #24a smoke: a station is just a big hull with the same schema. Dock
    // ports and a market counter mount like anything else; only the trader
    // SEAT is a seat (trade.counter is a facet tag, not a role).
    {
        hull_info h;
        h.class_tag = "class.station";
        h.grid = grid_block{2.0, 4, 2.5, 0.05};
        std::vector<socket_desc> socks = {
            {"hp_dock_1", socket_category::utility_, {}, {}},
            {"hp_dock_2", socket_category::utility_, {}, {}},
            {"hp_counter", socket_category::utility_, {}, {}},
            {"hp_seat_trader", socket_category::utility_, {}, {}},
        };
        std::vector<component_type> cat = {
            {"dock.port", socket_category::utility_},
            {"counter.market", socket_category::utility_},
            {"seat.station", socket_category::utility_},
        };
        std::vector<mount_request> mounts = {
            {"dock_1", "dock.port", "hp_dock_1", {}, {}},
            {"dock_2", "dock.port", "hp_dock_2", {}, {}},
            {"counter_1", "counter.market", "hp_counter", {}, {"trade.counter"}},
            {"seat_trader", "seat.station", "hp_seat_trader", {}, {"station.trader"}},
        };
        auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                                   std::move(mounts));
        REQUIRE(r.has_value());
        CHECK(r->free_socket_count() == 0);  // 4 sockets, 4 mounts
        Occupancy st(*r, reg);
        REQUIRE(st.seats().size() == 1);     // counter is NOT a seat
        CHECK(st.seats()[0].instance_id == "seat_trader");
        CHECK_FALSE(st.manned("station.trader"));  // station trade is dead-until-manned too
        REQUIRE(st.sit(11, "seat_trader").has_value());
        CHECK(st.manned("station.trader"));
    }

    // Total validation: a doubled tag is an authoring bug (set semantics).
    {
        hull_info h;
        h.grid = grid_block{2.0, 4, 2.5, 0.05};
        std::vector<socket_desc> socks = {
            {"hp_seat_pilot", socket_category::utility_, {}, {}},
        };
        std::vector<component_type> cat = {{"seat.station", socket_category::utility_}};
        std::vector<mount_request> mounts = {
            {"seat_pilot", "seat.station", "hp_seat_pilot", {},
             {"station.pilot", "station.pilot"}},
        };
        auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                                   std::move(mounts));
        CHECK(r.error() == loadout_error::duplicate_tag);
    }
}
