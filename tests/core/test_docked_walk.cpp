// 7.11 — one clamped state opens the service suite via tags (#23b);
// the airlock walk-over junction is a walk-graph edge gated by section
// pressure (#23c, Phase 6 primitive). Pure predicates, mutation-gated.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/capture.h"
#include "ve/core/sim/dock.h"
#include "ve/core/sim/docked.h"
#include "ve/core/sim/loadout.h"

using namespace ve::core;
namespace sim = ve::core::sim;

namespace {

// Build a loadout directly (Phase 7 authors test structs in code;
// cooked-fixture versions land with Phase 8 cooking).
sim::Loadout make_loadout(std::vector<sim::socket_desc> sockets,
                          std::vector<sim::mount_request> mounts) {
    // Every socket id doubles as a component type (test economy: a
    // component of type X on socket X always satisfies category).
    std::vector<sim::component_type> catalog;
    for (const auto& s : sockets) catalog.push_back({s.id, s.category});  // component_type{type_id, category}
    sim::hull_info hull{.class_tag = "class.test",
                        .grid = sim::grid_block{2.0, 4, 2.5, 0.05},
                        .hull_mass = 1000.0};
    auto built = sim::Loadout::assemble(std::move(hull), std::move(sockets),
                                        std::move(catalog), std::move(mounts));
    REQUIRE(built.has_value());
    return std::move(*built);
}

sim::socket_desc util_socket(std::string_view id) {
    return sim::socket_desc{.id = std::string(id),
                            .category = sim::socket_category::utility_};
}

// Station: two ports, a trade counter with facets, a berth, a section
// map wired for the junction tests below.
sim::mount_request mount(std::string_view instance, std::string_view socket,
                         std::vector<std::string> tags) {
    return sim::mount_request{.instance_id = std::string(instance),
                              .type_id = std::string(socket),
                              .socket_id = std::string(socket),
                              .attrs = {},
                              .tags = std::move(tags)};
}

sim::Loadout make_station() {
    return make_loadout(
        {util_socket("socket.port_a"), util_socket("socket.port_b"),
         util_socket("socket.counter"), util_socket("socket.berth")},
        {mount("port_a", "socket.port_a", {"dock.port"}),
         mount("port_b", "socket.port_b", {"dock.port"}),
         mount("counter_truck", "socket.counter",
               {"trade.goods", "trade.fuel", "trade.repair"}),
         mount("berth_3", "socket.berth", {"crew.berth"})});
}

sim::Loadout make_ship() {
    return make_loadout(
        {util_socket("socket.gear"), util_socket("socket.lock")},
        {mount("gear_bow", "socket.gear", {"dock.gear"}),
         mount("airlock_hull", "socket.lock", {"airlock"})});
}

const sim::mounted_component* tag_first(const sim::Loadout& lo,
                                        std::string_view tag) {
    for (const auto& mc : lo.mounts())
        for (const auto& t : mc.tags)
            if (t == tag) return &mc;
    return nullptr;
}

} // namespace

TEST_SUITE("docked_walk") {
TEST_CASE("docked_service_gates") {
    const auto station = make_station();
    const auto ship = make_ship();
    const sim::clamped_pair pair{.ship_port_index = 0, .station_port_index = 0};
    constexpr std::string_view kDark = "station.dark";

    // (a) The WHOLE suite keys off the one clamped state (#23b): every
    // service opens with the clamp, none opens without it.
    for (std::string_view facet : {"trade.goods", "trade.fuel", "trade.repair"}) {
        const auto open = sim::station_service_check(pair, station, facet, kDark);
        CHECK(open.available);
        CHECK(open.counter_id == "counter_truck");
        const auto shut = sim::station_service_check(std::nullopt, station, facet, kDark);
        CHECK_FALSE(shut.available);
        CHECK(shut.reason == "undocked");
    }

    // (b) Dark-not-dead (Phase 6): shed the counter's facets -> every
    // service closes; restore -> re-opens. Same component, whole suite.
    sim::Loadout dark = station;
    auto* counter = const_cast<sim::mounted_component*>(tag_first(dark, "trade.goods"));
    REQUIRE(counter);
    // Destroyed counter that KEEPS its facets (dark tag ADDED): closes
    // with an explicit reason — not serving, not falling through to
    // "no counter" (survived an earlier dark-dropped mutant precisely
    // because dropping facets alone lets "no counter" answer too).
    counter->tags = {"trade.goods", "trade.fuel", "trade.repair",
                     std::string(kDark)};
    for (std::string_view facet : {"trade.goods", "trade.fuel", "trade.repair"}) {
        const auto dead = sim::station_service_check(pair, dark, facet, kDark);
        CHECK_FALSE(dead.available);
        CHECK(dead.reason == "counter dark");
        CHECK(dead.counter_id == "counter_truck");
    }
    // Facets fully dropped too (repair-swap shape): still closed.
    counter->tags = {std::string(kDark)};
    for (std::string_view facet : {"trade.goods", "trade.fuel", "trade.repair"})
        CHECK_FALSE(sim::station_service_check(pair, dark, facet, kDark).available);
    counter->tags = {"trade.goods", "trade.fuel", "trade.repair"}; // repaired
    CHECK(sim::station_service_check(pair, dark, "trade.goods", kDark).available);

    // (c) One state opens the suite != one service: a facet nobody
    // mounts is simply "no counter" even while clamped.
    const auto berth = sim::station_service_check(pair, station, "trade.medical", kDark);
    CHECK_FALSE(berth.available);
    CHECK(berth.reason == "no counter");
    // Crew berth IS present though — tag set is the whole truth.
    CHECK(sim::station_service_check(pair, station, "crew.berth", kDark).available);

    // (d) #47b: the ship carries its dock gear and airlock as Phase-5
    // loadout components (tags), and the clamp itself stays load-bearing:
    // 7.10's pose projection + 7.11's pair identity coexist.
    CHECK(tag_first(ship, "dock.gear") != nullptr);
    CHECK(tag_first(ship, "airlock") != nullptr);
    CHECK(tag_first(station, "dock.port") != nullptr);

    // Clamp round-trip through the 7.10 API (fixture pinned like dock
    // tests: gear seats at rest_depth, pose projection is exact).
    sim::dock_port_def port{.capture_radius_m = 30.0,
                            .safe_speed_m_s = 10.0, .rest_depth_m = 1.5};
    sim::dock_gear_def gear{.local_pos = {0.0, 0.0, 2.0}};
    const sim::port_world pw{.pos = {0.0, 0.0, 100.0}, .vel = {0.0, 0.0, 0.0},
                             .normal = {0.0, 0.0, 1.0}};
    sim::rigid_state s{.pos = {0.0, 0.0, 130.0}, .vel = {0.0, 0.0, -5.0}};
    const auto out = sim::snap_to_dock(s, pw, port, gear);
    // Gear point lands exactly at rest depth on the port axis: the ship
    // docks from the +normal side, gear resting INSIDE the port plane,
    // so the gear point sits at port.pos - rest_depth * normal (#23).
    const math::dvec3 gear_world =
        out.pos + math::rotate(out.att, gear.local_pos);
    CHECK(math::approximately_equal(gear_world.z, 100.0 - 1.5, 1e-9));
    // Keel mates the port normal (the 7.10 gate stays load-bearing).
    const math::dvec3 keel_out = math::rotate(out.att, {0.0, 0.0, 1.0});
    CHECK(std::abs(keel_out.x) < 1e-9);
    CHECK(std::abs(keel_out.y) < 1e-9);
    CHECK(keel_out.z > 1.0 - 1e-9);
    // Pair identity indexes into the SAME loadout mounts the gate reads:
    const auto* port_mc = tag_first(station, "dock.port");
    REQUIRE(port_mc);
    const auto idx = static_cast<std::uint32_t>(
        port_mc - station.mounts().data());
    CHECK(sim::station_service_check(
              sim::clamped_pair{.ship_port_index = 0, .station_port_index = idx},
              station, "trade.goods", kDark)
              .available);
}

TEST_CASE("docked_walk_junction") {
    const auto station = make_station();
    const auto ship = make_ship();
    const sim::clamped_pair pair{.ship_port_index = 0, .station_port_index = 0};
    (void)pair;

    // Authored walk graphs: ship has lock->cabin; station has the port
    // section beside the counter ring. Section ids are opaque cooked
    // names (Phase 8 cooks these maps; #19d: "how connected" is data).
    sim::section_graph ship_g{.sections = {
                                  {"cargo", {1}},
                                  {"cabin", {0, 2}},
                                  {"lounge", {1}},
                              }};
    sim::section_graph st_g{.sections = {
                                {"docking_ring", {1}},
                                {"market", {0, 2}},
                                {"berths", {1}},
                            }};
    // Authored junction wiring (which section each named component
    // pressurizes): airlock_hull -> cabin (ship), port_a -> docking_ring
    // (station). Authored directly this phase; the cooklet emits the
    // same pairs from geometry in Phase 8 (#19a).
    const auto junction = sim::mated_junction(ship, station, ship_g, st_g,
                                              "cabin", "docking_ring");
    REQUIRE(junction.has_value());

    const std::array<sim::section_state, 2> ship_press{
        sim::section_state{.section_id = "cabin"},
        sim::section_state{.section_id = "cargo"}};
    const std::array<sim::section_state, 2> st_press{
        sim::section_state{.section_id = "docking_ring"},
        sim::section_state{.section_id = "market"}};
    CHECK(sim::junction_walkable(*junction, ship_press, st_press));

    // Ship side breach (venting = section band tag, #19c): walk closes.
    const std::array<sim::section_state, 1> ship_leak_s{
        sim::section_state{.section_id = "cabin", .venting = true}};
    CHECK_FALSE(sim::junction_walkable(*junction, ship_leak_s, st_press));

    // Station side breach closes it too — same rule, other span.
    const std::array<sim::section_state, 1> st_leak_s{
        sim::section_state{.section_id = "docking_ring", .venting = true}};
    CHECK_FALSE(sim::junction_walkable(*junction, ship_press, st_leak_s));

    // Empty (reservoir crossed vacuum threshold, Phase 6 primitive) is
    // as bad as venting: one primitive covers both, no special case.
    const std::array<sim::section_state, 1> st_empty_s{
        sim::section_state{.section_id = "docking_ring", .empty = true}};
    CHECK_FALSE(sim::junction_walkable(*junction, ship_press, st_empty_s));
    const std::array<sim::section_state, 1> ship_empty_s{
        sim::section_state{.section_id = "cabin", .empty = true}};
    CHECK_FALSE(sim::junction_walkable(*junction, ship_empty_s, st_press));

    // Fail closed: named section, no air report -> no green light.
    const std::array<sim::section_state, 0> none{};
    CHECK_FALSE(sim::junction_walkable(*junction, none, none));
    // Junction with an unauthored section name is inert.
    const sim::walk_junction inert{.ship_section = "", .station_section = ""};
    const std::array<sim::section_state, 1> press{
        sim::section_state{.section_id = "cabin"}};
    CHECK_FALSE(sim::junction_walkable(inert, press, press));
    // A ship without the airlock facet gets NO junction at all (the
    // clamp still holds; there is just nothing to walk through): a
    // ship loadout assembled with only the gear fails to mate.
    const auto no_lock_ship = make_loadout(
        {util_socket("socket.gear")},
        {mount("gear_bow", "socket.gear", {"dock.gear"})});
    CHECK_FALSE(sim::mated_junction(no_lock_ship, station, ship_g, st_g,
                                    "cabin", "docking_ring").has_value());
    // A section that merely EXISTS elsewhere in the graph doesn't count:
    // states are looked up by the junction's own ids.
    const std::array<sim::section_state, 1> wrong_ship{
        sim::section_state{.section_id = "lounge"}};
    const std::array<sim::section_state, 1> wrong_st{
        sim::section_state{.section_id = "market"}};
    // Both sections report pressure, but NOT for the junction's own
    // named sections: lookups fail -> fail closed.
    CHECK_FALSE(sim::junction_walkable(*junction, wrong_ship, wrong_st));
}

TEST_CASE("docked_walk_round_trip") {
    // done-when (b) part 1: dock -> clamped-walk-check -> undock at
    // KNOWN TICKS. A scripted approach engages by swept entry at one
    // fixed tick (clamp binds), the walk junction is open across the
    // clamped span (never while free — a free ship has no clamped-pair
    // snapshot), and a released push-off detaches at one later fixed
    // tick. One state: the walk-open boolean flips exactly twice.
    const sim::Loadout station = make_station();
    const sim::Loadout ship = make_ship();

    // Authored section maps + live pressure (pressurized the whole
    // ride here; breach cases are pinned in docked_walk_junction).
    const sim::section_graph ship_g{.sections = {{"cabin", {}}}};
    const sim::section_graph st_g{.sections = {{"docking_ring", {}}}};
    const std::array<sim::section_state, 1> ship_press{
        sim::section_state{.section_id = "cabin"}};
    const std::array<sim::section_state, 1> st_press{
        sim::section_state{.section_id = "docking_ring"}};
    constexpr std::string_view kDark = "station.dark";

    sim::dock_port_def port{.capture_radius_m = 30.0,
                            .safe_speed_m_s = 10.0, .rest_depth_m = 1.5};
    sim::dock_gear_def gear{.local_pos = {0.0, 0.0, 2.0}};
    const sim::port_world pw{.pos = {0.0, 0.0, 100.0}, .vel = {0.0, 0.0, 0.0},
                             .normal = {0.0, 0.0, 1.0}};
    const sim::rigid_params params{.mass = 1000.0};

    // Approach straight down -Z at 6 m/s from z=330.45 (safe speed):
    // 0.1 m/tick, so the swept 30 m shell entry lands strictly INSIDE
    // tick 2004 (start of tick 2004 is at gap 30.05, end at 29.95 —
    // no boundary case), and engagement pins to that one fixed tick.
    sim::rigid_state s{.pos = {0.0, 0.0, 330.45}, .vel = {0.0, 0.0, -6.0}};

    bool clamped = false;
    math::dquat clamp_att{};
    sim::rigid_state pose_at_release{};
    constexpr sim::tick_t kEngageWindowA = 2003, kEngageWindowB = 2006;
    constexpr sim::tick_t kReleaseTick = 10000;
    constexpr sim::tick_t kSpanTicks = 10001;

    std::array<bool, kSpanTicks> walk_open{};
    int engage_tick = -1;

    for (sim::tick_t t = 0; t < kSpanTicks; ++t) {
        if (!clamped) {
            if (t >= kEngageWindowA && t <= kEngageWindowB) {
                const auto hit = sim::capture_engage(port, pw, s.pos, s.vel, t);
                if (hit.has_value()) {
                    s = sim::snap_to_dock(s, pw, port, gear);
                    clamp_att = s.att;
                    clamped = true;
                    if (engage_tick < 0) engage_tick = static_cast<int>(t);
                }
            }
            if (!clamped)
                s.pos = s.pos + math::dvec3(s.vel * sim::kSim_tick_dt);
        } else if (t == kReleaseTick) {
            // Release + authored push-off: 1000 N*s along the normal
            // -> +1 m/s departure (mass 1000 kg). The bind simply
            // stops being applied; next tick the ship is a free body.
            // Hull sits 2 m below the port plane (gear rests 1.5 m
            // INSIDE it); departure is along +normal.
            const auto pre = clamped ? sim::docked_pose(
                s, pw, port, gear, clamp_att)
                : s;
            pose_at_release = pre;
            REQUIRE(pose_at_release.pos.z < 100.0);
            // gear local +Z already mates the port normal (keel-docked): the
            // torque arm r x J is zero, so departure is pure translation —
            // no rotation to perturb the single-step flight pin.
    // Mass-correct departure (gear local +Z is the normal, so the
    // torque arm r x J vanishes): 1000 N*s on the 1000 kg hull leaves
    // exactly +1 m/s. A dropped params argument (mass defaults to 1.0)
    // would launch at 1000 m/s — this pin catches that class of bug.
    s = sim::release_impulse(s, pw, gear, params, 1000.0, {});
    CHECK(math::approximately_equal(s.vel.z, 1.0, 1e-9));
    CHECK(s.vel.x == 0.0);
    CHECK(s.vel.y == 0.0);
            clamped = false;
        } else {
            // The clamp is an EXACT pose projection: a docked pair is
            // one rigid body, so the projected state becomes the
            // clamped state (attitude rides the pair).
            s = sim::docked_pose(s, pw, port, gear, clamp_att);
        }

        // The clamped-pair snapshot exists ONLY while clamped: a free
        // ship passes nullopt. The pair IS the one docked state — the
        // gates and the junction all answer "docked things" from it.
        const std::optional<sim::clamped_pair> pair =
            clamped ? std::optional<sim::clamped_pair>{sim::clamped_pair{}}
                    : std::nullopt;
        const auto junc = sim::mated_junction(ship, station, ship_g, st_g,
                                              "cabin", "docking_ring");
        const bool svc = pair.has_value() &&
                         sim::station_service_check(pair, station,
                                                    "trade.goods", kDark)
                             .available;
        const bool walk = pair.has_value() && junc.has_value() &&
                          sim::junction_walkable(*junc, ship_press, st_press);
        walk_open[t] = svc && walk;
    }

    // Known-tick boundaries, pinned exactly.
    CHECK(engage_tick == 2004);         // swept entry tick, pinned
    int flips_up = -1, flips_down = -1, flips = 0;
    for (sim::tick_t t = 1; t < kSpanTicks; ++t) {
        if (walk_open[t] != walk_open[t - 1]) {
            ++flips;
            if (walk_open[t]) flips_up = static_cast<int>(t);
            else flips_down = static_cast<int>(t);
        }
    }
    CHECK(flips == 2);                  // one state, two boundaries
    CHECK(flips_up == engage_tick);     // opens exactly at the clamp tick
    CHECK(flips_down == kReleaseTick);  // closes exactly at release

    CHECK(flips == 2);                  // one state, two boundaries
    CHECK(flips_up == engage_tick);     // opens exactly at the clamp tick
    CHECK(flips_down == kReleaseTick);  // closes exactly at release

    // Departure rides the impulse: the release pose (captured AT the
    // release tick, before any free-flight step) plus 1 m/s along +z
    // stepped by every tick after it. Gear sits 1.5 m INSIDE the port
    // plane (z=98.5), so the hull rides on the -normal side and
    // departs through the plane: z crosses 100 ~150 ticks after
    // release, never a pin at the release instant.
    // docked_pose re-derives from attitude + port, so it projects the
    // CURRENT (departed) ship too — that's the pure-function contract.
    // The release INSTANT pose is just the last clamped pose: re-run
    // docked_pose at tick kReleaseTick - 1 state instead (static port
    // => same pose every clamped tick; take it from the loop below).
    const double flew = 1.0 * (kSpanTicks - kReleaseTick - 1) * sim::kSim_tick_dt;
    const math::dvec3 want{pose_at_release.pos.x, pose_at_release.pos.y,
                           pose_at_release.pos.z + flew};
    CHECK(math::approximately_equal(math::length(s.pos - want), 0.0, 1e-9));
    // The impulse is mass-scaled exactly: 1000 N*s on a 1000 kg hull
    // leaves +1 m/s (mass 1.0 — the struct default — would launch at
    // 1000 m/s; this pin catches a dropped params argument). Torque
    // arm is zero (gear axis mates the normal), so the first free step
    // is exactly one tick of that velocity — and crossing the port
    // plane lands ~200 ticks out: 96.5 + 1000*dt, exactly. (The old
    // "1000 ticks" comment was wrong: 1 m/s * 16.67 s = 16.7 m, not 2.)
    // The impulse is mass-scaled exactly: 1000 N*s on a 1000 kg hull
    // leaves +1 m/s along the port normal (mass 1.0 — the struct
    // default — would launch at 1000 m/s; this pin catches a dropped
    // params argument). Gear local +Z already mates the normal, so the
    // torque arm r x J is zero: departure is pure translation.
    CHECK(math::approximately_equal(s.vel.z, 1.0, 1e-9));
    CHECK(s.vel.x == 0.0);
    CHECK(s.vel.y == 0.0);
    // pose_at_release was captured WHILE clamped (one tick before the
    // release fires), so it is the rest pose; every free tick after it
    // steps +1 m/s * dt. Pin: total flight = the same per-tick sum the
    // loop performed (1.0*dt accumulated in fp64 is NOT bit-identical
    // to 10000*dt — the pin mirrors the loop, not the closed form).
    const int free_steps = static_cast<int>(kSpanTicks - kReleaseTick) - 1;
    double flew_z = 0.0;
    for (int i = 0; i < free_steps; ++i) flew_z += 1.0 * sim::kSim_tick_dt;
    CHECK(pose_at_release.pos.z < 100.0);  // rest pose: hull below plane
    CHECK(math::approximately_equal(s.pos.z, pose_at_release.pos.z + flew_z,
                                    1e-12));
}  // TEST_CASE("docked_walk_round_trip")

} // TEST_SUITE
