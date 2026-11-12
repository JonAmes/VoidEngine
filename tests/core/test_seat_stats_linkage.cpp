// Phase 6 task 6.7 — done-when (c): "a modifier granted by a seat is
// present exactly when an occupant sits there; the pilot seat zeroes the
// engine-control function of unseated stations." The seat affordance
// bundle attaches/detaches as ONE unit through sit/stand/KO, with the
// #18b zeroing proven on a StatsPipeline over the ship stack.
#include <bit>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/effect_bus.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/occupancy.h"
#include "ve/core/sim/seat_link.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/game/role_tags.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

std::vector<std::string> role_registry() {
    return {ve::game::kPhase5_role_tags.begin(), ve::game::kPhase5_role_tags.end()};
}

// Same shuttle schema as 5.11: pilot + gunner seats in mount order.
Loadout ship_loadout() {
    hull_info h;
    h.class_tag = "class.shuttle";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 9000.0;
    std::vector<socket_desc> socks = {
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
        {"hp_seat_gunner", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {
        {"seat.station", socket_category::utility_},
    };
    std::vector<mount_request> mounts = {
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
        {"seat_gunner", "seat.station", "hp_seat_gunner", {}, {"station.gunner"}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat),
                               std::move(mounts));
    REQUIRE(r.has_value());
    return std::move(*r);
}

modifier_desc mk(std::string_view attr, std::string_view id, mod_op op, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = op;
    m.value = v;
    m.policy = stack_policy::exclusive;  // one grant unit per stack id
    return m;
}
}  // namespace

TEST_CASE("seat_stats_linkage") {
    SUBCASE("grant present EXACTLY when seated; drops as one unit") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship, pilot;
        StatsSet pst;
        pst.set_base("pilot.reflex", 10.0f);
        StatsPipeline pp(pst, pilot);

        SeatLink link(occ, ship);
        seat_affordance pilot_aff;
        pilot_aff.role_tag = "station.pilot";
        pilot_aff.occupant_grant = {
            mk("pilot.reflex", "seat.pilot", mod_op::percent, 50.0f),
            mk("pilot.stamina", "seat.pilot", mod_op::flat_add, 2.0f),  // 2nd member
        };
        REQUIRE(link.add_affordance(pilot_aff));

        CHECK(pp.derived("pilot.reflex") == 10.0f);  // unseated: base only
        REQUIRE(link.sit(7, "seat_pilot", pilot).has_value());
        CHECK(pilot.modifier_count() == 2);          // bundle landed as one source
        CHECK(pilot.sources().size() == 1);          //   = grant_source(0)
        CHECK(pilot.sources()[0].id == SeatLink::grant_source(0));
        CHECK(pp.derived("pilot.reflex") == 15.0f);  // hand-computed: 10 * 1.5

        REQUIRE(link.stand(7, "seat_pilot", pilot).has_value());
        CHECK(pilot.modifier_count() == 0);          // the UNIT dropped together
        CHECK(pp.derived("pilot.reflex") == 10.0f);  // bit-exact pre-seat state
    }

    SUBCASE("#18b: unseated pilot station is function-zeroed; sitting lifts it") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship;
        StatsSet sst;
        sst.set_base("function.engine_cmd", 1.0f);  // full authority when manned
        StatsPipeline sp(sst, ship);

        SeatLink link(occ, ship);
        seat_affordance pilot_aff;
        pilot_aff.role_tag = "station.pilot";
        pilot_aff.when_unmanned = {
            mk("function.engine_cmd", "pilot.zero", mod_op::multiplier, 0.0f),
        };
        REQUIRE(link.add_affordance(pilot_aff));
        // Seat starts empty -> the zeroing is live from wiring time. Ship
        // stacks get their initial state via a scripted sit/stand pair so
        // the "as a unit" invariant itself is what's under test:
        REQUIRE(link.sit(7, "seat_pilot", ship).has_value());  // grant empty; lifts zero
        CHECK(sp.derived("function.engine_cmd") == 1.0f);       // manned: full
        REQUIRE(link.stand(7, "seat_pilot", ship).has_value());
        CHECK(sp.derived("function.engine_cmd") == 0.0f);       // unseated: ZEROED
        CHECK(ship.modifier_count() == 1);                      // one zeroing source
        CHECK(ship.sources()[0].id == SeatLink::unmanned_source(0));
    }

    SUBCASE("moving between seats: old grant drops, new lands, old station re-arms") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship, crew;
        StatsSet cst;
        cst.set_base("crew.output", 8.0f);
        StatsPipeline cp(cst, crew);

        SeatLink link(occ, ship);
        seat_affordance p_aff;
        p_aff.role_tag = "station.pilot";
        p_aff.occupant_grant = {mk("crew.output", "seat.pilot", mod_op::multiplier, 2.0f)};
        seat_affordance g_aff;
        g_aff.role_tag = "station.gunner";
        g_aff.occupant_grant = {mk("crew.output", "seat.gunner", mod_op::flat_add, 4.0f)};
        REQUIRE(link.add_affordance(p_aff));
        REQUIRE(link.add_affordance(g_aff));

        REQUIRE(link.sit(7, "seat_pilot", crew).has_value());
        CHECK(cp.derived("crew.output") == 16.0f);  // 8 * 2
        REQUIRE(link.sit(7, "seat_gunner", crew).has_value());  // MOVE (one call)
        CHECK(occ.seat_of(7) == "seat_gunner");
        CHECK(crew.modifier_count() == 1);
        CHECK(crew.sources()[0].id == SeatLink::grant_source(1));
        CHECK(cp.derived("crew.output") == 12.0f);  // 8 + 4 — pilot's 2x is GONE
    }

    SUBCASE("KO drop: victim's stack reverses even for a claimant id that sits nowhere") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship, gunner;
        SeatLink link(occ, ship);
        seat_affordance g_aff;
        g_aff.role_tag = "station.gunner";
        g_aff.occupant_grant = {mk("gunner.aim", "seat.gunner", mod_op::flat_add, 3.0f)};
        REQUIRE(link.add_affordance(g_aff));

        REQUIRE(link.sit(9, "seat_gunner", gunner).has_value());
        CHECK(gunner.modifier_count() == 1);
        REQUIRE(link.drop(0xDEAD, "seat_gunner", gunner).has_value());  // forced
        CHECK(gunner.modifier_count() == 0);
        CHECK_FALSE(occ.occupant_at("seat_gunner").has_value());
        CHECK_FALSE(link.drop(0, "seat_gunner", gunner).has_value());   // already empty
    }

    SUBCASE("rail events: seat_attach / seat_drop with tick + seq, move emits pair") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship, crew;
        EffectEventBus bus;
        SeatLink link(occ, ship, &bus);
        seat_affordance a;
        a.role_tag = "station.pilot";
        a.occupant_grant = {mk("x.y", "g", mod_op::flat_add, 1.0f)};
        REQUIRE(link.add_affordance(a));

        bus.begin_tick(50);
        REQUIRE(link.sit(7, "seat_pilot", crew).has_value());
        REQUIRE(link.sit(7, "seat_gunner", crew).has_value());  // move = drop+attach
        REQUIRE(link.stand(7, "seat_gunner", crew).has_value());
        auto ev = bus.drain();
        REQUIRE(ev.size() == 4);
        CHECK(ev[0].kind == rail_kind::seat_attach);
        CHECK(ev[0].tick == 50);  CHECK(ev[0].seq == 0);
        CHECK(ev[0].a == 7);      CHECK(ev[0].b == 1);  // seat idx 0 + 1
        CHECK(ev[1].kind == rail_kind::seat_drop);  CHECK(ev[1].seq == 1);
        CHECK(ev[1].b == 1);
        CHECK(ev[2].kind == rail_kind::seat_attach); CHECK(ev[2].seq == 2);
        CHECK(ev[2].b == 2);
        CHECK(ev[3].kind == rail_kind::seat_drop);  CHECK(ev[3].seq == 3);
        CHECK(ev[3].b == 2);
    }

    SUBCASE("error paths are refusal-clean and never half-apply") {
        const Loadout lo = ship_loadout();
        const std::vector<std::string> reg = role_registry();
        Occupancy occ(lo, reg);
        ModifierStack ship, a_st, b_st;
        SeatLink link(occ, ship);
        seat_affordance a;
        a.role_tag = "station.pilot";
        a.occupant_grant = {mk("x.y", "g", mod_op::flat_add, 1.0f)};
        REQUIRE(link.add_affordance(a));

        CHECK(link.sit(1, "nope", a_st).error() == link_error::seat_not_found);
        REQUIRE(link.sit(1, "seat_pilot", a_st).has_value());
        CHECK(link.sit(2, "seat_pilot", b_st).error() == link_error::seat_busy);
        CHECK(b_st.modifier_count() == 0);                    // refused: no grant
        CHECK(link.stand(2, "seat_pilot", b_st).error() == link_error::not_seated);
        REQUIRE(link.stand(1, "seat_pilot", a_st).has_value());
        // Caller source squatting the reserved range -> stack_conflict, and
        // occupancy stays EMPTY (no half-apply):
        a_st.add_source(SeatLink::grant_source(0));
        CHECK(link.sit(1, "seat_pilot", a_st).error() == link_error::stack_conflict);
        CHECK_FALSE(occ.occupant_at("seat_pilot").has_value());
    }

    SUBCASE("determinism: identical script twice -> identical derived bits + events") {
        auto run = [] {
            const Loadout lo = ship_loadout();
            const std::vector<std::string> reg = role_registry();
            Occupancy occ(lo, reg);
            ModifierStack ship, crew;
            StatsSet cst;
            cst.set_base("crew.output", 8.0f);
            StatsPipeline cp(cst, crew);
            EffectEventBus bus;
            SeatLink link(occ, ship, &bus);
            seat_affordance p_aff;
            p_aff.role_tag = "station.pilot";
            p_aff.occupant_grant = {mk("crew.output", "s", mod_op::percent, 33.0f),
                                    mk("crew.output", "t", mod_op::multiplier, 1.25f)};
            link.add_affordance(p_aff);
            bus.begin_tick(3);
            link.sit(7, "seat_pilot", crew);
            const float d = *cp.derived("crew.output");  // (8*1.33)*1.25 order-pinned
            link.stand(7, "seat_pilot", crew);
            const float d0 = *cp.derived("crew.output");
            return std::vector<std::uint32_t>{
                std::bit_cast<std::uint32_t>(d), std::bit_cast<std::uint32_t>(d0),
                static_cast<std::uint32_t>(bus.pending())};
        };
        const auto x = run();
        const auto y = run();
        CHECK(x == y);
        CHECK(x[2] == 2);  // attach + drop recorded both runs
    }
}
