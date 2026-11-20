// test: duel_replay_hash — 9.24, the Phase 9 determinism capstone. Two
// armed ships duel under the FULL combat spine: game-side pilot FSMs
// (9.21) drive weapons-free intent, the core targeting computer (9.17,
// assist ON both sides) solves deflection, crewed gun stations (9.19)
// gate firing by seat state and add 9.10 spread, projectiles fly the
// live 9.8 spine, hits run through the 9.11 damage sink and the #18d
// seat KO roll. Ship tracks are scripted (kinematics are not the
// subject; every SIM decision — shots, spread, hits, KO, FSM flips —
// is spine output). Every tick the world's bit state (positions,
// projectile streams, shot counts, seat/FSM state, stats hashes) is
// FNV-accumulated; replaying the identical inputs must land on the same
// 64-bit hash, bit-for-bit. The duel must show: A's EMPTY-seat turret
// never fires, B's gunner gets KO'd mid-duel (silencing B's turret),
// and B's pilot FSM withdraws on integrity. (done-when (g).)
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "ve/core/fnv.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/damage.h"
#include "ve/core/sim/gun.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/occupancy.h"
#include "ve/core/sim/projectile.h"
#include "ve/core/sim/seat_link.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_hash.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/targeting.h"
#include "ve/core/sim/wiring.h"
#include "ve/game/ai_pilot.h"
#include "ve/game/role_tags.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;
using ve::game::ai_intent_of;
using ve::game::ai_next_state;
using ve::game::ai_pilot_attrs;
using ve::game::ai_state;

constexpr std::string_view kOffline = "ship.component.offline";

Loadout ship_loadout() {  // minimal ship, 9.19 pattern: two gunner seats
    hull_info h;
    h.class_tag = "class.shuttle";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 9000.0;
    std::vector<socket_desc> socks = {
        {"hp_turret", socket_category::gun_, {}, {}},
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
        {"hp_seat_gunner", socket_category::utility_, {}, {}},
        {"hp2_seat_gunner", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {
        {"turret.autocannon", socket_category::gun_},
        {"seat.station", socket_category::utility_},
    };
    std::vector<mount_request> mounts = {
        {"turret_1", "turret.autocannon", "hp_turret", {}, {}},
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
        {"seat_gunner_a", "seat.station", "hp_seat_gunner", {}, {"station.gunner"}},
        {"seat_gunner_b", "seat.station", "hp2_seat_gunner", {}, {"station.gunner"}},
    };
    auto r = Loadout::assemble(std::move(h), std::move(socks), std::move(cat), std::move(mounts));
    REQUIRE(r.has_value());
    return std::move(*r);
}
std::vector<std::string> role_registry() {
    return {ve::game::kPhase5_role_tags.begin(), ve::game::kPhase5_role_tags.end()};
}

modifier_desc mk(std::string_view attr, std::string_view id, mod_op op, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = op;
    m.value = v;
    return m;
}
effect_desc ko_fx() { return effect_desc{"status.ko", {}, {mk("gun.aim_skill", "status.ko", mod_op::multiplier, 0.0f)}}; }
effect_desc inj_fx() { return effect_desc{"status.injured", {}, {mk("gun.aim_skill", "status.injured", mod_op::multiplier, 0.5f)}}; }

// Same 9.8-profile well the targeting duel used.
rail_world test_world() {
    rail_world w;
    body_def pl{};
    pl.surface_radius = 100.0;
    pl.well_taper_start = 200.0;
    pl.well_radius = 1000.0;
    pl.surface_g = 10.0;
    REQUIRE(w.add_body(pl).has_value());
    return w;
}
std::vector<comp_bounds> hull_at(const dvec3& c, std::uint32_t id) {
    return {comp_bounds{id, obb{c, {2.0, 2.0, 2.0}, {}}}};  // 9.18 hull
}
rigid_state ship_at(const dvec3& p, const dvec3& v) {
    rigid_state s;
    s.pos = p;
    s.vel = v;
    return s;
}

// Scripted tracks (kinematics under test NOT; they only feed the spine).
// A from (-300,0,0) at +X, jinks +Y at t=1 s. B from (400,0,0) at -X,
// jinks -Y at t=1 s: heads close, both turning — a deflection problem.
dvec3 pos_a(double t) { return t < 1.0 ? dvec3{-300.0 + 80.0 * t, 0.0, 0.0}
                                       : dvec3{-300.0 + 80.0 * t, 70.0 * (t - 1.0), 0.0}; }
dvec3 vel_a(double t) { return t < 1.0 ? dvec3{80.0, 0.0, 0.0} : dvec3{80.0, 70.0, 0.0}; }
dvec3 pos_b(double t) { return t < 1.0 ? dvec3{400.0 - 60.0 * t, 0.0, 0.0}
                                       : dvec3{400.0 - 60.0 * t, -55.0 * (t - 1.0), 0.0}; }
dvec3 vel_b(double t) { return t < 1.0 ? dvec3{-60.0, 0.0, 0.0} : dvec3{-60.0, -55.0, 0.0}; }

struct pipe_fixture {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    pipe_fixture() : sp(st, ms) {
        st.set_base("weapon.rof", 5.0f);      // 60 Hz -> 12-tick interval
        st.set_base("weapon.spread", 0.004f);
        st.set_base("pilot.gunnery", 1.0f);   // q=1 -> 1 s lock, full refine
    }
};

constexpr double kMuzzle = 500.0;
constexpr double kHitJ = 8.0;     // per-hit damage to hull component
constexpr double kIn = 100.0;     // seat-hit input (9.19 law)
constexpr double kKo = 20.0;      // KO band edge
constexpr double kHp = 40.0;      // gun_1 health both ships
constexpr double kWithdrawFrac = 0.30;
constexpr std::uint64_t kGunner = 0x201ull;

struct facts {
    std::uint64_t hash = 0;
    int shots_a1 = 0, shots_a2 = 0, shots_b = 0;
    std::optional<int> ko_tick, withdraw_tick;
    int b_shots_at_ko = -1, b_shots_after = -1;
};

// Ship B side runs its own pipeline/sink/seat; ship A runs a pipe + a
// second (empty-seat) gun. run_duel is the whole world: replay == same bits.
facts run_duel(const std::uint64_t world_seed, const int ticks) {
    const rail_world w = test_world();
    facts f;

    pipe_fixture pa, pb;
    const ai_pilot_attrs attrs{.engage_range = 4000.0, .disengage_range = 6000.0,
                               .withdraw_integrity = kWithdrawFrac, .aggression = 0.5};

    // ---- ship A: seats, guns (manned gun_1 + EMPTY-seat gun_2) ----
    Occupancy occ_a(ship_loadout(), role_registry());
    SeatLink link_a(occ_a, pa.ms);
    link_a.add_affordance({"station.gunner", {mk("gun.aim_skill", "seat.grant", mod_op::multiplier, 1.0f)}, {}});
    ModifierStack ostack_a;
    const auto sit_a = link_a.sit(kGunner, "seat_gunner_a", ostack_a);
    REQUIRE_MESSAGE(sit_a.has_value(), static_cast<int>(sit_a.error()));
    gun_slot gun_a1, gun_a2;
    gun_a1.component = 71; gun_a1.gunner_seat = "seat_gunner_a";
    gun_a2.component = 72; gun_a2.gunner_seat = "seat_gunner_b";  // seat exists, never manned
    gun_configure(gun_a1, pa.sp, {"weapon.rof", "weapon.spread", "", "", "", ""});
    gun_configure(gun_a2, pa.sp, {"weapon.rof", "weapon.spread", "", "", "", ""});
    damage_sink sink_a;
    sink_configure_shield(sink_a, pa.sp, shield_setup{.regen_attr = "", .capacity_attr = "", .bands = {}});
    REQUIRE(sink_add_component(sink_a, "hull_a", kHp, kOffline));
    targeting_computer tc_a{71, &pa.sp, "pilot.gunnery", -1};

    // ---- ship B: manned turret + sink + seat link ----
    Occupancy occ_b(ship_loadout(), role_registry());
    SeatLink link_b(occ_b, pb.ms);
    link_b.add_affordance({"station.gunner", {mk("gun.aim_skill", "seat.grant", mod_op::multiplier, 1.0f)}, {}});
    ModifierStack ostack_b;
    REQUIRE(link_b.sit(kGunner + 1, "seat_gunner_b", ostack_b).has_value());
    gun_slot gun_b;
    gun_b.component = 81; gun_b.gunner_seat = "seat_gunner_b";
    gun_configure(gun_b, pb.sp, {"weapon.rof", "weapon.spread", "", "", "", ""});
    damage_sink sink_b;
    sink_configure_shield(sink_b, pb.sp, shield_setup{.regen_attr = "", .capacity_attr = "", .bands = {}});
    REQUIRE(sink_add_component(sink_b, "gun_1", kHp, kOffline));
    targeting_computer tc_b{81, &pb.sp, "pilot.gunnery", -1};

    WiringReach wiring_a(pa.sp, pa.ms, {}, {});  // A: no wired systems to kill
    WiringReach wiring_b(pb.sp, pb.ms, {}, {});

    ai_state s_a = ai_state::patrol, s_b = ai_state::patrol;
    targeting_engage(tc_a, 0);
    targeting_engage(tc_b, 0);

    std::vector<projectile> proj_a, proj_b;
    double hp_b = kHp;
    Fnv1a64 h;

    for (tick_t t = 0; t < ticks; ++t) {
        const double tt = static_cast<double>(t) / kSim_tick_hz;
        const dvec3 pA = pos_a(tt), vA = vel_a(tt), pB = pos_b(tt), vB = vel_b(tt);
        const double slant = length(pA - pB);

        // ---- pilot FSMs (9.21): intent only ----
        const double int_a = 1.0, int_b = std::max(0.0, hp_b / kHp);
        s_a = ai_next_state(s_a, slant, true, int_a, attrs);
        s_b = ai_next_state(s_b, slant, true, int_b, attrs);
        if (s_b == ai_state::withdraw && !f.withdraw_tick) f.withdraw_tick = static_cast<int>(t);
        const auto intent_a = ai_intent_of(s_a);
        const auto intent_b = ai_intent_of(s_b);

        // ---- A fires (assist ON) while weapons free ----
        if (intent_a.weapons_free && targeting_lock_ready(tc_a, t)) {
            const auto sol = targeting_solution(
                tc_a, {pA, vA, pB, vB, kMuzzle}, w, t, 240);
            if (sol) {
                if (const auto fr = gun_try_fire(gun_a1, sol->aim_dir, t, world_seed, &occ_a))
                    proj_a.push_back(*spawn_projectile(ship_at(pA, vA), fr->aim, kMuzzle, t, 180, -1));
                // Empty-seat turret asked to fire under the SAME intent:
                // must refuse forever (crew gate, consumes nothing).
                if (const auto fr2 = gun_try_fire(gun_a2, sol->aim_dir, t, world_seed, &occ_a))
                    proj_a.push_back(*spawn_projectile(ship_at(pA, vA), fr2->aim, kMuzzle, t, 180, -1));
            }
        }
        // ---- B fires while weapons free ----
        if (intent_b.weapons_free && targeting_lock_ready(tc_b, t)) {
            const auto sol = targeting_solution(
                tc_b, {pB, vB, pA, vA, kMuzzle}, w, t, 240);
            if (sol) {
                if (const auto fr = gun_try_fire(gun_b, sol->aim_dir, t, world_seed, &occ_b))
                    proj_b.push_back(*spawn_projectile(ship_at(pB, vB), fr->aim, kMuzzle, t, 180, -1));
            }
        }

        // ---- fly projectiles on the live spine; hits -> sink + KO roll ----


        std::vector<std::size_t> dead_a, dead_b;
        for (std::size_t i = 0; i < proj_a.size(); ++i) {
            const auto res = projectile_step(w, proj_a[i], t, 1, hull_at(pB, 81));
            if (res.life == projectile_life::struck) {
                const auto e = sink_apply_hit(sink_b, &wiring_b, "gun_1", kHitJ);
                REQUIRE(e.has_value());
                hp_b -= std::min(kHp, e->on_component);
                const auto o = link_b.station_hit("seat_gunner_b", world_seed, t, kIn, kKo, 10.0,
                                                  ko_fx(), inj_fx(), 9000u + static_cast<std::uint64_t>(t), ostack_b);
                if (o == station_hit_outcome::ko && !f.ko_tick) {
                    f.ko_tick = static_cast<int>(t);
                    // gun_b.shots is the sim's cumulative counter (uint64); the
                    // sentinel field is signed (int, -1 = "no KO"). Duel shot
                    // counts stay far below INT_MAX, so the cast is lossless here.
                    f.b_shots_at_ko = static_cast<int>(gun_b.shots);
                }
                dead_a.push_back(i);
            } else if (res.life != projectile_life::alive) {
                dead_a.push_back(i);
            }
        }
        for (std::size_t i = 0; i < proj_b.size(); ++i) {
            const auto res = projectile_step(w, proj_b[i], t, 1, hull_at(pA, 71));
            if (res.life == projectile_life::struck) {
                const auto e = sink_apply_hit(sink_a, &wiring_a, "hull_a", kHitJ);
                REQUIRE(e.has_value());
                link_a.station_hit("seat_gunner_a", world_seed, t, kIn, kKo, 10.0,
                                   ko_fx(), inj_fx(), 7000u + static_cast<std::uint64_t>(t), ostack_a);
                dead_b.push_back(i);
            } else if (res.life != projectile_life::alive) {
                dead_b.push_back(i);
            }
        }
        auto erase_dead = [](std::vector<projectile>& ps, std::vector<std::size_t> dead) {
            std::sort(dead.begin(), dead.end());
            dead.erase(std::unique(dead.begin(), dead.end()), dead.end());
            for (std::size_t k = dead.size(); k-- > 0;) ps.erase(ps.begin() + static_cast<std::ptrdiff_t>(dead[k]));
        };
        erase_dead(proj_a, dead_a);
        erase_dead(proj_b, dead_b);

        // ---- world bit state -> hash stream ----
        for (const dvec3* p : {&pA, &pB})
            for (const double c : {p->x, p->y, p->z}) h.mix_u64(std::bit_cast<std::uint64_t>(c));
        for (const auto& pr : proj_a)
            for (const double c : {pr.state.pos.x, pr.state.pos.y, pr.state.pos.z})
                h.mix_u64(std::bit_cast<std::uint64_t>(c));
        for (const auto& pr : proj_b)
            for (const double c : {pr.state.pos.x, pr.state.pos.y, pr.state.pos.z})
                h.mix_u64(std::bit_cast<std::uint64_t>(c));
        h.mix_u64(static_cast<std::uint64_t>(gun_a1.shots));
        h.mix_u64(static_cast<std::uint64_t>(gun_a2.shots));
        h.mix_u64(static_cast<std::uint64_t>(gun_b.shots));
        h.mix_u64(stats_state_hash(pa.st));
        h.mix_u64(stats_state_hash(pb.st));
        h.mix_u64(occ_a.occupant_at("seat_gunner_a").has_value() ? 1u : 0u);
        h.mix_u64(occ_b.occupant_at("seat_gunner_b").has_value() ? 1u : 0u);
        h.mix_u64(static_cast<std::uint64_t>(s_a));
        h.mix_u64(static_cast<std::uint64_t>(s_b));
    }

    f.shots_a1 = static_cast<int>(gun_a1.shots);
    f.shots_a2 = static_cast<int>(gun_a2.shots);
    f.shots_b = static_cast<int>(gun_b.shots);
    if (f.ko_tick) f.b_shots_after = f.shots_b;
    f.hash = h.value();
    return f;
}

}  // namespace

TEST_CASE("duel_replay_hash") {
    const auto r1 = run_duel(0xD13E0000DEAD0003ull, 240);
    const auto r2 = run_duel(0xD13E0000DEAD0003ull, 240);

    // (c) empty-seat turret NEVER fired; manned turret did.
    CHECK(r1.shots_a2 == 0);
    CHECK(r1.shots_a1 > 0);
    // (e) enemy turret KO'd mid-duel, then silent: shot count frozen at KO.
    REQUIRE(r1.ko_tick.has_value());
    CHECK(*r1.ko_tick > 0);
    CHECK(*r1.ko_tick < 239);
    CHECK(r1.b_shots_after == r1.b_shots_at_ko);
    CHECK(r1.shots_b > 0);  // it fired BEFORE being KO'd
    // pilot FSM reacted to damage: B withdrew on integrity.
    CHECK(r1.withdraw_tick.has_value());

    // THE claim: replay bit-for-bit.
    CHECK(r1.hash == r2.hash);
    CHECK(r1.shots_a1 == r2.shots_a1);
    CHECK(r1.ko_tick == r2.ko_tick);
    CHECK(r1.withdraw_tick == r2.withdraw_tick);

    // A different world seed changes the duel outcome (seed is load-
    // bearing, not decoration).
    const auto r3 = run_duel(0xBEEF0000DEAD0003ull, 240);
    CHECK(r3.hash != r1.hash);
}
