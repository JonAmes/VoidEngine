// 7.13 — airlock two-door state machine over door components, pumped by
// the chamber's Phase-6 reservoir facts. Full rail stream hand-computed
// tick-by-tick (6.6 contract); never-both-open invariant checked every
// tick; refusals emit nothing.
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/doors.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

using namespace ve::core;
namespace sim = ve::core::sim;

namespace {

// Same test-economy loadout builder as 7.11: socket id doubles as type id.
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

// Mount identity (index into the loadout above; payloads carry +1):
constexpr std::uint32_t kInner = 0, kOuter = 1, kAirlock = 2, kHatch = 3;

sim::airlock_def hull_airlock() {
    return sim::airlock_def{.airlock_mount = kAirlock,
                            .inner_mount = kInner,
                            .outer_mount = kOuter};
}

// Chamber bands are authored data: empty at 0 (binary-exact drain, 6.9
// style), nominal at >= 59 of a 60-unit chamber.
std::vector<sim::reservoir_band> chamber_bands() {
    return {
        {0.0, "chamber.empty", sim::band_dir::at_or_below},
        {59.0, "chamber.norm", sim::band_dir::at_or_above},
    };
}

modifier_desc rate(std::string_view attr, std::string_view stack, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(stack);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::exclusive;
    return m;
}

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

TEST_SUITE("airlock_cycle") {

TEST_CASE("airlock_round_trip_stream") {
    // Chamber 60.0 units, ceiling 60. Vent 60/s -> 1.0/tick (binary-exact):
    // dry exactly 60 steps after the vent is on. Refill 60/s -> nominal
    // (>= 59) exactly 59 steps after the supply is on. Every flip tick is
    // hand-computed below, not observed.
    auto hull = make_hull();
    sim::DoorField doors(4);
    sim::Airlock lock(hull_airlock());
    EffectEventBus bus;

    StatsSet st;
    ModifierStack ms;
    st.set_base("chamber.air.supply", 0.0f);
    st.set_base("chamber.air.load", 0.0f);
    st.set_base("chamber.air.leak", 0.0f);
    StatsPipeline sp(st, ms);
    TagSet tags;
    sim::Reservoir chamber(60.0, chamber_bands(), &tags);
    chamber.set_ceiling(60.0);
    chamber.bind(&sp, {"chamber.air.supply", "chamber.air.load", "chamber.air.leak"});

    ms.add_source(1);
    ms.add_modifier(1, rate("chamber.air.load", "chamber_vent", 60.0f));

    const sim::verb_actor crew{.id = 7, .on_foot = false, .zone_tags = {}};
    const sim::tick_t t0 = 1000;

    bus.begin_tick(t0);
    auto out = sim::cycle_airlock(bus, hull, doors, lock, /*outward=*/true,
                                  "airlock.cycle", "zone.suit_ok", crew);
    REQUIRE(out.has_value());
    CHECK(lock.phase() == sim::airlock_phase::venting);
    CHECK(bus.pending() == 0);  // inner was already closed: no events yet

    // Refusals while venting: every one changes NOTHING and emits nothing.
    auto re_out = sim::cycle_airlock(bus, hull, doors, lock, true,
                                     "airlock.cycle", "zone.suit_ok", crew);
    CHECK(re_out.error() == sim::verb_error::wrong_phase);
    auto manual_inner = sim::toggle_door(bus, hull, doors, kInner, "door.toggle",
                                         "zone.suit_ok", crew, &lock.def());
    CHECK(manual_inner.error() == sim::verb_error::interlock);
    auto manual_outer = sim::toggle_door(bus, hull, doors, kOuter, "door.toggle",
                                         "zone.suit_ok", crew, &lock.def());
    CHECK(manual_outer.error() == sim::verb_error::interlock);
    CHECK(bus.pending() == 0);
    CHECK_FALSE(doors.open(kInner));
    CHECK_FALSE(doors.open(kOuter));

    // Venting: dry at t0+59 (60 steps from t0 inclusive) -> outer opens.
    for (sim::tick_t t = t0; t <= 1100; ++t) {
        if (t > t0) bus.begin_tick(t);
        chamber.step();
        const sim::chamber_state ch{tags.contains("chamber.empty"),
                                    tags.contains("chamber.norm")};
        lock.step(bus, doors, ch);
        CHECK_FALSE((doors.open(kInner) && doors.open(kOuter)));  // invariant
    }
    CHECK(lock.phase() == sim::airlock_phase::open_out);
    CHECK(doors.open(kOuter));

    // At t0+100: cycle back inward.
    bus.begin_tick(1100);
    auto in = sim::cycle_airlock(bus, hull, doors, lock, false,
                                 "airlock.cycle", "zone.suit_ok", crew);
    REQUIRE(in.has_value());
    CHECK(lock.phase() == sim::airlock_phase::pressurizing);
    ms.remove_source(1);
    ms.add_source(2);
    ms.add_modifier(2, rate("chamber.air.supply", "chamber_fill", 60.0f));

    for (sim::tick_t t = 1101; t <= 1200; ++t) {
        bus.begin_tick(t);
        chamber.step();
        const sim::chamber_state ch{tags.contains("chamber.empty"),
                                    tags.contains("chamber.norm")};
        lock.step(bus, doors, ch);
        CHECK_FALSE((doors.open(kInner) && doors.open(kOuter)));
    }
    // Refill steps start at 1101 (the 1100 step ran before the supply was
    // added): value = t - 1100, nominal crossed at 1159 -> inner opened.
    CHECK(lock.phase() == sim::airlock_phase::open_in);
    CHECK(doors.open(kInner));
    CHECK_FALSE(doors.open(kOuter));

    // Cycle out again from open_in: the open inner door is closed first.
    bus.begin_tick(1200);
    auto out2 = sim::cycle_airlock(bus, hull, doors, lock, true,
                                   "airlock.cycle", "zone.suit_ok", crew);
    REQUIRE(out2.has_value());
    CHECK_FALSE(doors.open(kInner));  // closed as the first interlock act
    ms.remove_source(2);
    ms.add_source(1);
    ms.add_modifier(1, rate("chamber.air.load", "chamber_vent", 60.0f));

    for (sim::tick_t t = 1201; t <= 1270; ++t) {
        bus.begin_tick(t);
        chamber.step();
        const sim::chamber_state ch{tags.contains("chamber.empty"),
                                    tags.contains("chamber.norm")};
        lock.step(bus, doors, ch);
        CHECK_FALSE((doors.open(kInner) && doors.open(kOuter)));
    }

    // The complete hand-computed rail stream (tick, seq, kind, a, b):
    const std::vector<rail_event> want{
        {1059, 0, rail_kind::door_flip, kOuter + 1, 1},
        {1059, 1, rail_kind::airlock_flip, kAirlock + 1, 1},   // space side
        {1100, 2, rail_kind::door_flip, kOuter + 1, 0},
        {1159, 3, rail_kind::door_flip, kInner + 1, 1},
        {1159, 4, rail_kind::airlock_flip, kAirlock + 1, 0},   // ship side
        {1200, 5, rail_kind::door_flip, kInner + 1, 0},
        {1260, 6, rail_kind::door_flip, kOuter + 1, 1},
        {1260, 7, rail_kind::airlock_flip, kAirlock + 1, 1},
    };
    const auto got = bus.drain();
    REQUIRE(got.size() == want.size());
    CHECK(same_streams(got, want));
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(got[i].tick == want[i].tick);
        CHECK(got[i].seq == want[i].seq);
        CHECK(got[i].kind == want[i].kind);
        CHECK(got[i].a == want[i].a);
        CHECK(got[i].b == want[i].b);
    }
    CHECK(lock.phase() == sim::airlock_phase::open_out);
}

TEST_CASE("airlock_first_cause_gates_emit_nothing") {
    // Bounds -> facet -> interlock order, and every refusal is a total
    // no-op on BOTH the bus and the door state.
    auto hull = make_hull();
    sim::DoorField doors(4);
    sim::Airlock lock(hull_airlock());
    EffectEventBus bus;
    const sim::verb_actor crew{.id = 7, .zone_tags = {}};
    bus.begin_tick(600);

    auto bad = sim::toggle_door(bus, hull, doors, 99, "door.toggle",
                                "zone.suit_ok", crew, &lock.def());
    CHECK(bad.error() == sim::verb_error::unknown_component);
    auto no_facet = sim::toggle_door(bus, hull, doors, kAirlock, "door.toggle",
                                     "zone.suit_ok", crew, &lock.def());
    CHECK(no_facet.error() == sim::verb_error::missing_action_facet);
    auto wrong_verb = sim::toggle_door(bus, hull, doors, kHatch, "airlock.cycle",
                                       "zone.suit_ok", crew, &lock.def());
    CHECK(wrong_verb.error() == sim::verb_error::missing_action_facet);

    auto ok = sim::cycle_airlock(bus, hull, doors, lock, true,
                                 "airlock.cycle", "zone.suit_ok", crew);
    REQUIRE(ok.has_value());
    auto twice = sim::cycle_airlock(bus, hull, doors, lock, true,
                                    "airlock.cycle", "zone.suit_ok", crew);
    CHECK(twice.error() == sim::verb_error::wrong_phase);

    CHECK(bus.pending() == 0);  // nothing above emitted a single event
    CHECK_FALSE(doors.open(kInner));
    CHECK_FALSE(doors.open(kOuter));
    CHECK_FALSE(doors.open(kHatch));
    CHECK(lock.phase() == sim::airlock_phase::venting);
}

}  // TEST_SUITE
