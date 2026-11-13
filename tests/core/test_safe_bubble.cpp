// Phase 8 task 8.13 — authored safe bubbles: the warp hard wall
// (decision #22c amended by #50). safe_bubble_interior pins the strict
// interior predicate, the def gates, and the engage-door composition;
// safe_bubble_crossing pins the hard-wall truncation through replay +
// the direct predicate, symmetric in/out, at the exact tick.
//
// All pins mirror the engine's EXACT fp64 op order (safe_bubble.cpp's
// pinned quadratic: a = dot(disp,disp), bq = dot(rel,disp),
// cq = dot(rel,rel) - r*r, roots (-bq ± sqrt(bq*bq - a*cq)) / a; the
// warp-step grouping (vel * mult) * kSim_tick_dt — same doctrine as the
// 8.12 ramp pins). Fixture r = 1024 m (dyadic) so the entry TOI lands
// BIT-EXACTLY on the surface: u1 = 0.8533333333333334 (fl of 64/75)
// with z' = 2048 + (-1200)*u1 == 1024.0 exactly; the band (r*r*2^-40)
// is what keeps the next tick's ulp residue classified ON the surface
// instead of inside-by-1ulp (which would take the exit root and jump
// the pinned ship by 2r — the hazard the band was added for).
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "ve/core/sim/ftl.h"
#include "ve/core/sim/preview.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/safe_bubble.h"
#include "ve/core/sim/time.h"

#include <doctest/doctest.h>

using namespace ve::core;
using namespace ve::core::sim;

namespace {

constexpr rigid_params kShip{50000.0, math::dvec3{4.0e10, 4.0e10, 4.0e10},
                             std::nullopt};

// Rail world with ONE static body (root, origin) carrying a safe bubble.
rail_world bubble_world(double r, double well = 0.0, double speed = 3000.0) {
    rail_world w;
    body_def b{};
    b.surface_radius = 1.0;  // add_body requires > 0; no field, no terrain
    b.well_radius = well;
    b.safe_bubble_radius = r;
    b.safe_speed = speed;
    REQUIRE(w.add_body(b).has_value());
    return w;
}

// Independent z-only port of bubble_warp_contact's classification for a
// static origin-centred bubble — the engine's op order verbatim. Returns
// {hit, pinned z}. The replay legs below CHECK against this port.
struct port_step {
    bool hit = false;
    double z = 0.0;
};
port_step port_wall(double z, double dispz, double r) {
    const double a = dispz * dispz;
    const double bq = z * dispz;
    const double cq = z * z - r * r;
    const double fr = (z + dispz) * (z + dispz) - r * r;
    const double band = r * r * 0x1p-40;
    const double f0 = (std::fabs(cq) <= band) ? 0.0 : cq;
    const double f1 = (std::fabs(fr) <= band) ? 0.0 : fr;
    double u = -1.0;  // -1 = no contact
    if (f0 == 0.0) {
        if (f1 == 0.0) return {false, z + dispz};
        u = 0.0;
    } else if (f0 < 0.0 && f1 < 0.0) {
        return {false, z + dispz};
    } else if (f0 < 0.0) {
        const double disc = bq * bq - a * cq;
        const double u2 = (-bq + std::sqrt(disc)) / a;
        if (!(u2 > 0.0) || u2 > 1.0) return {false, z + dispz};
        u = u2;
    } else {
        const double disc = bq * bq - a * cq;
        if (disc <= 0.0) return {false, z + dispz};
        const double u1 = (-bq - std::sqrt(disc)) / a;
        if (!(u1 > 0.0) || u1 > 1.0) return {false, z + dispz};
        u = u1;
    }
    if (u == 0.0) return {true, z};  // pin is BIT-IDENTICAL, verbatim p0
    return {true, 0.0 + (z + dispz * u)};  // c + (rel + disp*u), c = 0
}

}  // namespace

TEST_CASE("safe_bubble_interior") {
    SUBCASE("strict interior; surface is NOT interior (engage door open)") {
        const auto w = bubble_world(4.0e6);
        CHECK(!in_safe_bubble(w, math::dvec3{0.0, 0.0, 4.0e6 + 1.0}, 0).has_value());
        CHECK(!in_safe_bubble(w, math::dvec3{0.0, 0.0, 4.0e6}, 0).has_value());  // d == r: surface
        CHECK(in_safe_bubble(w, math::dvec3{0.0, 0.0, 4.0e6 - 1.0}, 0) == 0);
        CHECK(!in_safe_bubble(w, math::dvec3{1.0e9, 0.0, 0.0}, 0).has_value());
    }

    SUBCASE("no bubble = never interior; add_body gates") {
        rail_world w;
        body_def plain{};
        plain.surface_radius = 100.0;
        plain.well_radius = 1000.0;  // no [safe] -> 0 radius
        plain.well_taper_start = 500.0;
        REQUIRE(w.add_body(plain).has_value());
        CHECK(!in_safe_bubble(w, math::dvec3{}, 0).has_value());  // dead centre: no wall
        // bubble INSIDE the well: rejected (wall can't sit inside the well).
        // well_taper_start is legal so the ONLY rejecting gate is the safe one.
        body_def bad{};
        bad.surface_radius = 100.0;
        bad.well_radius = 2000.0;
        bad.well_taper_start = 1500.0;
        bad.safe_bubble_radius = 1000.0;
        bad.safe_speed = 3000.0;
        CHECK(!w.add_body(bad).has_value());
        // non-finite safe fields: rejected.
        body_def nan{};
        nan.surface_radius = 100.0;
        nan.safe_bubble_radius = std::numeric_limits<double>::quiet_NaN();
        CHECK(!w.add_body(nan).has_value());
    }

    SUBCASE("engage door: glue composes the predicate with ftl.h") {
        // Core holds no live FTL policy (ftl.h); the 8.14+ glue door is
        // "engage only when !in_safe_bubble". Pin that composition here.
        const auto w = bubble_world(4.0e6);
        constexpr ftl_drive kDrive{13.0, 4.0};  // ramp_rate exactly 3/s

        ftl_state inside;  // deep inside the bubble -> door closed
        if (!in_safe_bubble(w, math::dvec3{0.0, 0.0, 4.0e6 - 1.0}, 0).has_value())
            REQUIRE(ftl_engage(kDrive, inside, 0));
        ftl_step(kDrive, inside, 1);
        CHECK(inside.phase == ftl_phase::idle);
        CHECK(inside.mult == 1.0);  // warp cannot initiate inside a bubble

        ftl_state surface;  // exactly ON the surface -> door open
        if (!in_safe_bubble(w, math::dvec3{0.0, 0.0, 4.0e6}, 0).has_value())
            REQUIRE(ftl_engage(kDrive, surface, 0));
        ftl_step(kDrive, surface, 1);
        CHECK(surface.phase == ftl_phase::spooling_up);
        CHECK(surface.mult == 1.0 + 3.0 * (1.0 / 60.0));  // exact divide form
    }
}

TEST_CASE("safe_bubble_crossing") {
    constexpr double kR = 1024.0;
    const auto w = bubble_world(kR);

    SUBCASE("entry truncates to the TOI; event fields pinned") {
        const math::dvec3 p0{0.0, 0.0, 2048.0};
        const math::dvec3 disp{0.0, 0.0, (-6000.0 * 12.0) * kSim_tick_dt};  // -1200.0 exact
        const auto c = bubble_warp_contact(w, p0, disp, 0);
        REQUIRE(c.has_value());
        CHECK(c->body == 0);
        CHECK(c->inward);
        // Hand pin: u1 = (-bq - sqrt(bq*bq - a*cq)) / a with
        // a = 1200^2, bq = -2048*1200, cq = 2048^2 - 1024^2 -> fl(64/75).
        CHECK(c->u == 0.8533333333333334);
        CHECK(c->pos == math::dvec3{0.0, 0.0, 1024.0});  // TOI exact at this fixture
        CHECK(c->normal.x == 0.0);
        CHECK(c->normal.y == 0.0);
        CHECK(c->normal.z == 1.0);  // outward at the +z crossing point
        CHECK(c->disp_len == 1200.0);
    }

    SUBCASE("exact tangent graze is NOT a contact (measure-zero rule)") {
        // Line {(1024, t, 0)}: closest approach = exactly r at t = 0
        // -> disc == 0 exactly -> skipped by the disc <= 0 gate.
        const math::dvec3 p0{kR, 2048.0, 0.0};
        const math::dvec3 disp{0.0, (-6000.0 * 12.0) * kSim_tick_dt, 0.0};
        CHECK(!bubble_warp_contact(w, p0, disp, 0).has_value());
    }

    SUBCASE("warp leg: pinned dead at the wall every tick, sublight drift after") {
        // 10 ticks: 0-4 warp (mult 12), 5-9 sublight (mult 1). Wall holds
        // BIT-IDENTICALLY for ticks 1-4 (u == 0 re-pin, #50a), then
        // momentum carries inward ONLY at sublight once mult returns to 1.
        ReplayLog log;
        for (tick_t t = 0; t < 10; ++t) {
            tick_command cmd{};
            cmd.speed_mult = (t < 5) ? 12.0 : 1.0;
            log.append(cmd);
        }
        rigid_state s{.pos = math::dvec3{0.0, 0.0, 2048.0}, .vel = math::dvec3{0.0, 0.0, -6000.0},
                      .att = math::dquat{0, 0, 0, 1}};
        std::vector<double> engine_z;
        for (std::size_t i = 0; i < log.size(); ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), kShip, s);
            engine_z.push_back(s.pos.z);
        }
        // Port the identical leg.
        std::vector<double> port_z;
        double z = 2048.0;
        for (tick_t t = 0; t < 10; ++t) {
            const double mult = (t < 5) ? 12.0 : 1.0;
            const double dispz = (-6000.0 * mult) * kSim_tick_dt;
            if (mult > 1.0) {
                const auto st = port_wall(z, dispz, kR);
                z = st.hit ? st.z : z + dispz;
            } else {
                z = z + dispz;
            }
            port_z.push_back(z);
        }
        REQUIRE(engine_z.size() == port_z.size());
        for (std::size_t i = 0; i < port_z.size(); ++i)
            CHECK(engine_z[i] == port_z[i]);  // bit-identical per tick
        // Human-readable pins (also kill mutations that keep the port honest):
        CHECK(engine_z[0] == 1024.0);   // truncated to the surface at tick 0
        CHECK(engine_z[4] == 1024.0);   // pinned dead while mult > 1
        CHECK(engine_z[9] == 524.0);    // 1024 - 5*100: sublight inward drift
        CHECK(in_safe_bubble(w, s.pos, 9) == 0);  // drift ended INSIDE: legal at sublight

        // The wall KEEPS HOLDING: glue re-querying the same step at the
        // pinned point gets the u == 0 re-pin, position verbatim (#50a).
        const math::dvec3 pinned{0.0, 0.0, 1024.0};
        const math::dvec3 disp{0.0, 0.0, (-6000.0 * 12.0) * kSim_tick_dt};
        const auto c = bubble_warp_contact(w, pinned, disp, 5);
        REQUIRE(c.has_value());
        CHECK(c->u == 0.0);
        CHECK(c->pos == pinned);  // bit-identical, never re-derived via centre
    }

    SUBCASE("the 1-ulp residue after truncation stays ON the wall (band, #50)") {
        // Non-dyadic TOI: entry from 1629.5 truncates to z' =
        // 1023.9999999999999 — ONE ulp inside the sphere (f(0) residue
        // -2.33e-10, well inside the band r*r*2^-40 = 9.09e-7). Without
        // the band the next warp tick reads "fully inside", takes no
        // contact, and the ship sails through the wall; with it the u==0
        // re-pin holds the position bit-identically (#50a).
        ReplayLog log;
        for (tick_t t = 0; t < 5; ++t) {
            tick_command cmd{};
            cmd.speed_mult = 12.0;
            log.append(cmd);
        }
        rigid_state s{.pos = math::dvec3{0.0, 0.0, 1629.5}, .vel = math::dvec3{0.0, 0.0, -6000.0},
                      .att = math::dquat{0, 0, 0, 1}};
        std::vector<double> engine_z;
        for (std::size_t i = 0; i < log.size(); ++i) {
            replay_step(w, log, i, static_cast<tick_t>(i), kShip, s);
            engine_z.push_back(s.pos.z);
        }
        std::vector<double> port_z;
        double z = 1629.5;
        for (tick_t t = 0; t < 5; ++t) {
            const auto st = port_wall(z, (-6000.0 * 12.0) * kSim_tick_dt, kR);
            z = st.hit ? st.z : z + (-6000.0 * 12.0) * kSim_tick_dt;
            port_z.push_back(z);
        }
        REQUIRE(engine_z.size() == port_z.size());
        for (std::size_t i = 0; i < port_z.size(); ++i) CHECK(engine_z[i] == port_z[i]);
        CHECK(engine_z[0] == 1023.9999999999999);  // truncates 1 ulp INSIDE
        CHECK(engine_z[4] == engine_z[0]);         // held, never escaped, never crept
        const math::dvec3 residue{0.0, 0.0, 1023.9999999999999};
        const auto c = bubble_warp_contact(w, residue, math::dvec3{0.0, 0.0, -1200.0}, 1);
        REQUIRE(c.has_value());
        CHECK(c->u == 0.0);
        CHECK(c->pos == residue);  // pinned verbatim, not re-derived
    }

    SUBCASE("outward crossing at warp resolves symmetrically (#50b)") {
        // Start deep inside, warp out: exit root u2 = fl(32/75) -> z'
        // exactly 1024.0, then pinned every warp tick.
        ReplayLog log;
        for (tick_t t = 0; t < 5; ++t) {
            tick_command cmd{};
            cmd.speed_mult = 12.0;
            log.append(cmd);
        }
        rigid_state s{.pos = math::dvec3{0.0, 0.0, 512.0}, .vel = math::dvec3{0.0, 0.0, 6000.0},
                      .att = math::dquat{0, 0, 0, 1}};
        for (std::size_t i = 0; i < log.size(); ++i)
            replay_step(w, log, i, static_cast<tick_t>(i), kShip, s);
        CHECK(s.pos.z == 1024.0);  // exit TOI exact, then pinned

        const math::dvec3 p0{0.0, 0.0, 512.0};
        const math::dvec3 disp{0.0, 0.0, (6000.0 * 12.0) * kSim_tick_dt};
        const auto c = bubble_warp_contact(w, p0, disp, 0);
        REQUIRE(c.has_value());
        CHECK(c->u == 0.4266666666666667);  // fl(32/75), engine op order
        CHECK(!c->inward);                  // an EXIT crossing
        CHECK(c->pos == math::dvec3{0.0, 0.0, 1024.0});
    }

    SUBCASE("sublight legs ignore the wall entirely (warp-only wall)") {
        // mult == 1: the hook never runs; the ship sails straight through
        // the bubble — bubbles gate WARP, not motion (#50).
        ReplayLog log;
        for (tick_t t = 0; t < 12; ++t) {
            tick_command cmd{};  // speed_mult default 1.0
            log.append(cmd);
        }
        rigid_state s{.pos = math::dvec3{0.0, 0.0, 2048.0}, .vel = math::dvec3{0.0, 0.0, -6000.0},
                      .att = math::dquat{0, 0, 0, 1}};
        for (std::size_t i = 0; i < log.size(); ++i)
            replay_step(w, log, i, static_cast<tick_t>(i), kShip, s);
        CHECK(s.pos.z == 848.0);  // 2048 - 12*100: through the wall, deep inside
        CHECK(in_safe_bubble(w, s.pos, 11) == 0);
    }

    SUBCASE("ghost preview (divisor 1) replays the wall bit-identically") {
        ReplayLog log;
        for (tick_t t = 0; t < 10; ++t) {
            tick_command cmd{};
            cmd.speed_mult = (t < 5) ? 12.0 : 1.0;
            log.append(cmd);
        }
        const rigid_state start{.pos = math::dvec3{0.0, 0.0, 2048.0},
                                .vel = math::dvec3{0.0, 0.0, -6000.0},
                                .att = math::dquat{0, 0, 0, 1}};
        const auto ghost = ghost_preview(w, log, 0, 0, start, kShip, 10, 1);
        REQUIRE(ghost.size() == 10);
        // Fine roll for comparison.
        rigid_state s = start;
        for (std::size_t i = 0; i < log.size(); ++i)
            replay_step(w, log, i, static_cast<tick_t>(i), kShip, s);
        CHECK(std::memcmp(&ghost.back().pos, &s.pos, sizeof(math::dvec3)) == 0);
    }
}
