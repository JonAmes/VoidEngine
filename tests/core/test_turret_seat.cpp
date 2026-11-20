// Phase 9 task 9.19 — crewed gun stations (#18): a crew-gated turret fires
// ONLY while its seat is manned and non-dark; a hit on a manned station
// rolls onto the occupant through the seeded (#18d) stream: KO drops the
// seat (turret goes silent), injury keeps it. Refusal consumes nothing:
// gated shots match an ungated twin gun bit-for-bit (the manning gate never
// shifts the spread stream). Severity draws are pinned from the Python
// hand-port (station_port.py) of the FNV+splitmix key chain.
#include <doctest/doctest.h>

#include <bit>
#include <string>
#include <vector>

#include "ve/core/sim/gun.h"
#include "ve/core/sim/loadout.h"
#include "ve/core/sim/occupancy.h"
#include "ve/core/sim/seat_link.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/game/role_tags.h"

namespace {

using namespace ve::core::sim;
using ve::core::StatsSet;
using ve::core::ModifierStack;
using ve::core::StatsPipeline;
using ve::core::modifier_desc;
using ve::core::effect_desc;
using ve::core::mod_op;
using ve::core::math::dvec3;

Loadout ship_loadout() {
    hull_info h;
    h.class_tag = "class.shuttle";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 9000.0;
    std::vector<socket_desc> socks = {
        {"hp_turret", socket_category::gun_, {}, {}},
        {"hp_seat_pilot", socket_category::utility_, {}, {}},
        {"hp_seat_gunner", socket_category::utility_, {}, {}},
    };
    std::vector<component_type> cat = {
        {"turret.autocannon", socket_category::gun_},
        {"seat.station", socket_category::utility_},
    };
    std::vector<mount_request> mounts = {
        {"turret_1", "turret.autocannon", "hp_turret", {}, {}},
        {"seat_pilot", "seat.station", "hp_seat_pilot", {}, {"station.pilot"}},
        {"seat_gunner", "seat.station", "hp_seat_gunner", {}, {"station.gunner"}},
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

// rof 10/s -> 6-tick interval (60 Hz). StatsPipeline borrows the two members.
struct pipe_fixture {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    pipe_fixture() : sp(st, ms) {
        st.set_base("weapon.rof", 10.0f);
        st.set_base("weapon.spread", 0.002f);
    }
};

constexpr std::uint64_t kSeed = 0xBEEF'CAFE'0000'0001ull;  // station_port.py SEED
constexpr std::uint64_t kGunner = 0x111ull;
constexpr dvec3 kAim{0.0, 0.0, 1.0};
constexpr double kIn = 100.0;
constexpr double kKo = 20.0;

void setup_gun(gun_slot& g, pipe_fixture& pf, std::string_view seat) {
    g.component = 77;
    g.gunner_seat = std::string(seat);
    gun_configure(g, pf.sp, {"weapon.rof", "weapon.spread", "", "", "", ""});
}

effect_desc ko_fx() { return effect_desc{"status.ko", {}, {mk("gun.aim_skill", "status.ko", mod_op::multiplier, 0.0f)}}; }
effect_desc inj_fx() { return effect_desc{"status.injured", {}, {mk("gun.aim_skill", "status.injured", mod_op::multiplier, 0.5f)}}; }

}  // namespace

TEST_CASE("turret_requires_seat") {
    SUBCASE("empty seat silent; manning consumes no stream") {
        pipe_fixture pf;
        Occupancy occ(ship_loadout(), role_registry());
        gun_slot gun;
        setup_gun(gun, pf, "seat_gunner");
        gun_slot free_gun;
        setup_gun(free_gun, pf, "");  // ungated twin: same stream key

        // (e) empty seat -> never fires; gated + no occupancy view: fail-safe.
        for (tick_t t = 0; t < 20; ++t)
            CHECK_FALSE(gun_try_fire(gun, kAim, t, kSeed, &occ).has_value());
        CHECK_FALSE(gun_try_fire(gun, kAim, 10, kSeed).has_value());
        CHECK(gun.shots == 0);
        CHECK(gun.heat.value() == 0.0);

        // Seat the gunner -> fires. First gated shot == ungated twin's first
        // shot bit-for-bit: refusals consumed NO stream position.
        REQUIRE(occ.sit(kGunner, "seat_gunner").has_value());
        const auto a = gun_try_fire(gun, kAim, 10, kSeed, &occ);
        const auto b = gun_try_fire(free_gun, kAim, 10, kSeed);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(std::bit_cast<std::uint64_t>(a->aim.x) == std::bit_cast<std::uint64_t>(b->aim.x));
        CHECK(std::bit_cast<std::uint64_t>(a->aim.y) == std::bit_cast<std::uint64_t>(b->aim.y));
        CHECK(std::bit_cast<std::uint64_t>(a->aim.z) == std::bit_cast<std::uint64_t>(b->aim.z));

        // Dark seat, occupant still inside (#18d dark-not-dead): silent.
        REQUIRE(occ.set_dark("seat_gunner", true).has_value());
        CHECK_FALSE(gun_try_fire(gun, kAim, 16, kSeed, &occ).has_value());
        CHECK(gun.shots == 1);
        REQUIRE(occ.set_dark("seat_gunner", false).has_value());

        // Second gated shot == ungated twin's SECOND shot: stream position is
        // the SHOT count, not the attempt count.
        const auto c = gun_try_fire(gun, kAim, 17, kSeed, &occ);
        const auto d = gun_try_fire(free_gun, kAim, 17, kSeed);
        REQUIRE(c.has_value());
        REQUIRE(d.has_value());
        CHECK(std::bit_cast<std::uint64_t>(c->aim.x) == std::bit_cast<std::uint64_t>(d->aim.x));
        CHECK(std::bit_cast<std::uint64_t>(c->aim.y) == std::bit_cast<std::uint64_t>(d->aim.y));
    }

    SUBCASE("KO mid-burst silences the turret; injury does not") {
        pipe_fixture pf;
        Occupancy occ(ship_loadout(), role_registry());
        SeatLink link(occ, pf.ms);
        link.add_affordance({"station.gunner", {mk("gun.aim_skill", "seat.grant", mod_op::multiplier, 1.0f)}, {}});
        gun_slot gun;
        setup_gun(gun, pf, "seat_gunner");
        ModifierStack ostack;
        REQUIRE(link.sit(kGunner, "seat_gunner", ostack).has_value());
        REQUIRE(gun_try_fire(gun, kAim, 10, kSeed, &occ).has_value());

        // Pins from station_port.py (seed, tick, FNV("seat_gunner"), input):
        //   sev(t=10, in=100) = 12.69483122313847
        //   sev(t=11, in=100) = 25.01736473390437
        // ko band [20, inf): tick 10 -> injury, tick 11 -> KO. If the tick
        // left the key the two draws would be equal and one leg must fail.
        const auto i1 = link.station_hit("seat_gunner", kSeed, 10, kIn, kKo, 10.0,
                                         ko_fx(), inj_fx(), 4243u, ostack);
        CHECK(i1 == station_hit_outcome::injury);
        CHECK(occ.occupant_at("seat_gunner").has_value());  // injured, still seated
        CHECK(occ.function_available("seat_gunner"));

        const auto i2 = link.station_hit("seat_gunner", kSeed, 11, kIn, kKo, 10.0,
                                         ko_fx(), inj_fx(), 4242u, ostack);
        CHECK(i2 == station_hit_outcome::ko);
        CHECK_FALSE(occ.occupant_at("seat_gunner").has_value());  // seat unmanned
        CHECK_FALSE(occ.manned("station.gunner"));
        // (e) KO'd gunner stops the turret: every later attempt refuses and
        // the gun never advances its state again.
        for (tick_t t = 16; t < 30; ++t)
            CHECK_FALSE(gun_try_fire(gun, kAim, t, kSeed, &occ).has_value());
        CHECK(gun.shots == 1);
    }

    SUBCASE("input and seat ride the key") {
        pipe_fixture pf;
        Occupancy occ(ship_loadout(), role_registry());
        SeatLink link(occ, pf.ms);
        ModifierStack ostack;

        // sev(t=11, in=200) = 193.11915298155492 vs ungainly 2x of the
        // in=100 draw (50.03) if the input left the key: ko=100 splits them.
        REQUIRE(link.sit(kGunner, "seat_gunner", ostack).has_value());
        CHECK(link.station_hit("seat_gunner", kSeed, 11, 200.0, 100.0, 10.0,
                               ko_fx(), inj_fx(), 5001u, ostack) == station_hit_outcome::ko);

        // sev(t=11, seat_pilot, in=100) = 14.270988622010828 < 20 (injury)
        // while the SAME (tick,input) on seat_gunner was KO (25.017): the
        // seat id rides the key, per-station streams are independent.
        REQUIRE(link.sit(kGunner, "seat_pilot", ostack).has_value());
        CHECK(link.station_hit("seat_pilot", kSeed, 11, kIn, kKo, 10.0,
                               ko_fx(), inj_fx(), 5002u, ostack) == station_hit_outcome::injury);
    }

    SUBCASE("two-run replay is bit-identical, KO mid-burst included") {
        auto run = [](std::uint64_t seed) {
            pipe_fixture pf;
            Occupancy occ(ship_loadout(), role_registry());
            SeatLink link(occ, pf.ms);
            link.add_affordance({"station.gunner", {mk("gun.aim_skill", "seat.grant", mod_op::multiplier, 1.0f)}, {}});
            gun_slot gun;
            setup_gun(gun, pf, "seat_gunner");
            ModifierStack ostack;
            REQUIRE(link.sit(kGunner, "seat_gunner", ostack).has_value());
            std::vector<std::uint64_t> bits;
            for (tick_t t = 10; t < 20; ++t) {
                if (const auto f = gun_try_fire(gun, kAim, t, seed, &occ)) {
                    bits.push_back(std::bit_cast<std::uint64_t>(f->aim.x));
                    bits.push_back(std::bit_cast<std::uint64_t>(f->aim.y));
                    bits.push_back(std::bit_cast<std::uint64_t>(f->heat_after));
                }
                if (t == 14) {
                    // sev(t=14, in=100) = 99.2636165386 pinned EXACTLY at the
                    // ko boundary: s >= ko fires on bit equality.
                    const auto o = link.station_hit("seat_gunner", seed, 14, 100.0,
                                                    99.2636165386, 1e9, ko_fx(), inj_fx(), 4242u, ostack);
                    bits.push_back(o == station_hit_outcome::ko ? 1u : 0u);
                }
            }
            bits.push_back(occ.manned("station.gunner") ? 1u : 0u);
            return bits;
        };
        const auto r1 = run(kSeed), r2 = run(kSeed);
        // shots: @10 only (@15+ refused: the t=14 KO unmanned the seat)
        REQUIRE(r1.size() == 3 + 2);
        CHECK(r1 == r2);
        CHECK(r1[3] == 1u);   // the pinned boundary rolled a KO
        CHECK(r1[4] == 0u);   // seat unmanned by the end
    }
}
