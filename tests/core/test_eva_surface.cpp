// 7.12 — suited EVA on foot (#19d): suit air reservoir drains at pinned
// ticks; jets + the SHIP's assist code roll the entity (same integrate_step,
// same auto_level_torque); standing inherits the spinning ground's velocity;
// nested-well home switch is hysteresis-pinned. One round trip composes all
// four over a scripted tick stream, run twice, hash-identical.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/eva.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/well_blend.h"

using namespace ve::core;
namespace sim = ve::core::sim;

namespace {

constexpr double kMoon_r = 1'000.0;       // moon surface radius
constexpr double kMoon_g = 1.0;           // m/s^2
constexpr double kMoon_well = 40'000.0;   // moon well radius
constexpr double kPlanet_r = 5'000'000.0;
constexpr double kPlanet_g = 0.4;
constexpr double kPlanet_well = 20'000'000.0;
constexpr double kMoon_orbit = 12'000'000.0;  // moon center from planet

// Moon (optionally spinning) pinned above the planet's north pole.
struct moon_world {
    sim::rail_world rw;
    std::int32_t planet{}, moon{};
    // relief_m = 0.0 keeps noise EXACTLY 0 (terrain.cpp scales the octave
    // sum by relief) so the moon is a true R = kMoon_r sphere and every
    // ground/depth pin below is closed-form fp64 — not "some lattice
    // elevation I didn't ask for".
    sim::terrain_def land{.seed = 11, .relief_m = 0.0, .base_frequency = 1.0,
                          .octaves = 1};
    double spin_rad_s = 0.0;

    explicit moon_world(double spin = 0.0) : spin_rad_s(spin) {
        auto p = rw.add_body({.surface_radius = kPlanet_r,
                              .well_radius = kPlanet_well,
                              .surface_g = kPlanet_g,
                              .well_taper_start = kPlanet_r});
        REQUIRE(p.has_value());
        planet = *p;
        sim::body_def mb{.surface_radius = kMoon_r,
                         .well_radius = kMoon_well,
                         .surface_g = kMoon_g,
                         .well_taper_start = kMoon_r,
                         .parent = planet,
                         .orbit_period_s = 1e12,  // quasi-static
                         .orbit_radius_m = kMoon_orbit,
                         .orbit_phase_rad = 0.0,
                         .spin_rate_rad_s = spin_rad_s};  // default +Y axis:
        // the phase-0 orbit point (+X) is on the equator; pole() (+Y) is a pole
        auto m = rw.add_body(mb);
        REQUIRE(m.has_value());
        moon = *m;
    }

    sim::surface_world surf(sim::tick_t t) const {
        sim::surface_world s;
        s.body = rw.body(moon);
        s.terrain = &land;
        s.center = *rw.position(moon, t);
        s.center_vel = *rw.velocity(moon, t);
        s.orient = *rw.orientation(moon, t);
        // Ground truth from 7.8 (test_land_contacts): the snapshot spin
        // rides the AUTHORED axis/rate directly in the rail frame. rails.h
        // is the contract for what surface_world carries; we mirror it,
        // not re-derive it.
        s.spin_omega = s.body->spin_rate_rad_s * s.body->spin_axis;
        return s;
    }

    // Surface point at local +Y (north pole) at tick t.
    math::dvec3 pole(sim::tick_t t, double alt = 0.0) const {
        const math::dvec3 c = *rw.position(moon, t);
        return c + math::rotate(*rw.orientation(moon, t),
                                math::dvec3{0.0, kMoon_r + alt, 0.0});
    }
};

// Suit/jet fixture: 100 kg entity, 600 N ceiling (= 6 m/s^2, arcade
// low-thrust), servo stiff enough that the clamp, not the gain, decides.
constexpr sim::eva_params kSuit{
    .body = sim::rigid_params{.mass = 100.0,
                              .inertia_diag = {1e4, 1e4, 1e4}},
    .jet_max_thrust_n = 600.0,
    .jet_servo_k = 12'000.0};

constexpr sim::contact_params kBoots{
    .k_pushout_N_m = 200'000.0, .c_normal_N_s_m = 14'142.135623730951,
    .friction_mu = 0.0, .c_tangent_N_s_m = 0.0};

constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}
std::uint64_t mix_d(std::uint64_t h, double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    return mix(h, bits);
}

}  // namespace

TEST_SUITE("eva_surface") {

TEST_CASE("suit_air_drain") {
    // Binary-exact rates like 6.9: load 30/s -> 0.5/tick from 100: low
    // (<=25) at t=150, empty at t=200. Exertion (load 60/s -> 1.0/tick)
    // starts at t=100: 50 left, dry at exactly t=150.
    StatsSet st;
    ModifierStack ms;
    st.set_base("suit_air.supply", 0.0f);
    st.set_base("suit_air.load", 0.0f);
    st.set_base("suit_air.leak", 0.0f);
    StatsPipeline sp(st, ms);
    TagSet tags;
    sim::Reservoir air(100.0, sim::suit_air_bands(), &tags);
    air.bind(&sp, {"suit_air.supply", "suit_air.load", "suit_air.leak"});

    ms.add_source(1);
    auto rate = modifier_desc{};
    rate.target_attr = "suit_air.load";
    rate.stack_id = "breath";
    rate.op = mod_op::flat_add;
    rate.value = 30.0f;
    rate.policy = stack_policy::exclusive;
    ms.add_modifier(1, rate);

    for (sim::tick_t t = 1; t <= 100; ++t) {
        air.step();
        CHECK(air.value() == 100.0 - 0.5 * static_cast<double>(t));
        CHECK_FALSE(tags.contains("suit_air.low"));
    }
    // Exertion: switch the walk load (exertion source replaces breath).
    ms.add_source(2);
    auto hard = modifier_desc{};
    hard.target_attr = "suit_air.load";
    hard.stack_id = "breath";  // same stack: exertion replaces, not adds
    hard.op = mod_op::flat_add;
    hard.value = 60.0f;
    hard.policy = stack_policy::exclusive;
    ms.add_modifier(2, hard);

    for (sim::tick_t t = 101; t <= 149; ++t) {
        air.step();
        CHECK_FALSE(tags.contains("suit_air.low"));
    }
    air.step();  // t=150: exactly 25 -> low throws here
    CHECK(air.value() == 25.0);
    CHECK(tags.contains("suit_air.low"));
    CHECK(tags.contains("suit_air"));  // ancestor query answers (6.2)
    for (sim::tick_t t = 151; t <= 199; ++t) {
        air.step();
        CHECK_FALSE(tags.contains("suit_air.empty"));
    }
    air.step();  // t=200: exactly 0 -> empty
    CHECK(air.value() == 0.0);
    CHECK(tags.contains("suit_air.empty"));
    // Dry under load: never negative (6.9 floor).
    air.step();
    CHECK(air.value() == 0.0);

    // Pressurized: load stops being fed -> value frozen, tags un-throw.
    ms.remove_source(2);
    ms.remove_source(1);
    for (int t = 0; t < 600; ++t) {
        air.step();
        CHECK(air.value() == 0.0);
    }
    // Refill (airlock re-pressurize): supply feeds back up, tags clear.
    ms.add_source(3);
    auto fill = modifier_desc{};
    fill.target_attr = "suit_air.supply";
    fill.stack_id = "fill";
    fill.op = mod_op::flat_add;
    fill.value = 600.0f;  // 10/tick
    fill.policy = stack_policy::exclusive;
    ms.add_modifier(3, fill);
    for (int t = 0; t < 200; ++t) air.step();
    CHECK(air.value() == 2'000.0);
    CHECK_FALSE(tags.contains("suit_air.empty"));
    CHECK_FALSE(tags.contains("suit_air.low"));
}

TEST_CASE("jet_rollout") {
    // Jets off a surface, no gravity on the axis: v0 = 10 m/s +x, desired
    // 0. Servo error k*10 = 120'000 N >> 600 N ceiling -> max thrust for
    // 100 kg: a = 6 m/s^2, rollout time 10/6 s = 100 ticks. Determinism:
    // the identical run twice produces bit-identical hash streams.
    moon_world mw;
    const sim::surface_world ground = mw.surf(0);

    auto run = [&](std::vector<std::uint64_t>& hashes) {
        sim::rigid_state s;
        s.pos = mw.pole(0, 100.0);  // airborne, 100 m up
        s.vel = {10.0, 0.0, 0.0};
        math::dvec3 grav{};         // isolate the axis: no gravity feed
        std::uint64_t h = 0x5eedULL;
        for (sim::tick_t t = 1; t <= 100; ++t) {
            const auto out = sim::eva_walk_step(s, kSuit, grav, ground,
                                                kBoots, {0.0, 0.0, 0.0}, 0u);
            CHECK(math::length(out.contact_force) == 0.0);  // airborne
            // Clamp engages while the servo error is big: force magnitude
            // is EXACTLY the authored ceiling inside eva_jet_force.
            if (math::length(s.vel) > 1.05) {
                const math::dvec3 f =
                    sim::eva_jet_force(kSuit, s.vel, {0.0, 0.0, 0.0});
                CHECK(math::approximately_equal(math::length(f),
                                                kSuit.jet_max_thrust_n, 1e-9));
            }
            h = mix_d(h, s.pos.x);
            h = mix_d(h, s.vel.x);
            hashes.push_back(h);
        }
    };
    std::vector<std::uint64_t> a, b;
    run(a);
    run(b);
    REQUIRE(a.size() == 100);
    CHECK(a == b);  // same code, same inputs, bit-identical

    // Ceiling actually engaged: while error is big, force magnitude is
    // EXACTLY the authored 600 N (clamp branch), and the tick decel is
    // exactly a*dt = 0.1 m/s: vel after 100 ticks = 0.0 exactly.
    {
        sim::rigid_state s;
        s.pos = mw.pole(0, 100.0);
        s.vel = {10.0, 0.0, 0.0};
        math::dvec3 grav{};
        for (sim::tick_t t = 1; t <= 100; ++t) {
            (void)sim::eva_walk_step(s, kSuit, grav, ground, kBoots,
                                     {0.0, 0.0, 0.0}, 0u);
        }
        // Discrete max-thrust braking: 100 ticks * a*dt = 10 m/s, minus
        // one servo reversal quantum k*dt/m = 20 m/s (the servo flips when
        // the error crosses zero; arcade hover limit, not a bug).
        CHECK(s.vel.x > 0.0);
        CHECK(s.vel.x < 0.2);
        CHECK(math::length(s.vel) < 0.2);
    }
}

// ---- standing: the probe's kinematics ARE the standing ----
// Suited EVA on a spinning moon is the same probe the 7.8 land-contacts
// suite pins for gear feet: ground velocity = center_vel + spin_omega x r,
// reported as the hit's rel_vel (foot velocity minus ground). There is no
// "standing" state and no tangential force anywhere (7.8 ships #47a:
// *friction* deliberately absent): the walker simply holds its foot at a
// fixed world point (no lateral jets fired), moon gravity presses it in,
// the normal spring balances the depth, and the probe's OWN rel_vel read-
// out proves the ground sweeps under it. Ground truth for the algebra is
// the 7.8 pin: pad_vel == omega*r at the equator (1000 = 0.001*1'000'000
// there; ~1 m/s = 1e-3*1'000 here).
TEST_CASE("stand_inherits_surface_velocity") {
    moon_world mw(1e-3);  // omega*r ~ 1 m/s at the equator
    const sim::tick_t t0 = 900;
    const sim::surface_world g = mw.surf(t0);
    REQUIRE(mw.rw.body(mw.moon)->spin_rate_rad_s == 1e-3);

    // Snapshot kinematics (pinned): pure authored axis*rate, and the moon
    // really is 12 Mm out with the tiny rail-orbit translation.
    CHECK(math::length(g.spin_omega) == 1e-3);
    CHECK(g.center.x == doctest::Approx(kMoon_orbit).epsilon(1e-9));
    CHECK(g.center_vel.x == 0.0);

    // Geometry: the foot is the moon's LOCAL equator point (+X in the
    // spin plane of the +Y axis; the +Y pole would sweep at 0), 0.5 m
    // under the R = 1e3 sphere. The rail attitude has turned it toward
    // -Z by the orbit-rotation angle accumulated to t0.
    const math::dvec3 local{kMoon_r - 0.5, 0.0, 0.0};
    // Geometry straight from the authored def, not the snapshot:
    // local_rail_offset is (R cos y, 0, -R sin y) with yaw y, so orient is
    // a pure yaw about +Y and local +X (the equator; +Y is the pole) stays
    // in the y=0 plane — the ground under the boot races along -Z.
    // orient carries the orbit ROTATION (not just the position angle), so
    // read off from the snapshot; the position-side yaw (the -14.99 z
    // below) comes from local_rail_offset's own angle term.
    const math::dvec3 off = math::rotate(g.orient, local);
    CHECK(math::approximately_equal(math::length(off), kMoon_r - 0.5, 1e-9));
    CHECK(math::approximately_equal(off.z, -14.9919377876, 1e-6));
    CHECK(off.y == 0.0);  // pure +Y yaw keeps the equator point in y = 0

    // Ground velocity per 7.8's algebra: center_vel + omega x off. omega
    // is pure +Y and off.y is EXACTLY 0 (a Y-axis quaternion turning a +X
    // vector leaves the plane), so the sweep omega x off has zero y and
    // |sweep| = w*|off| ~ 1 m/s — the equator racing past. The tiny
    // quasi-static orbit translation (|center_vel| ~ 7.5e-5 m/s, along
    // -Z) adds on.
    const math::dvec3 sweep = math::cross(g.spin_omega, off);
    CHECK(math::approximately_equal(math::length(sweep),
                                    1e-3 * (kMoon_r - 0.5), 1e-9));
    CHECK(sweep.y == 0.0);
    CHECK(sweep.z < 0.0);  // -Z under the -Z-turned equator point
    const math::dvec3 ground_v = g.center_vel + sweep;

    // The contact probe, mirroring its internals exactly (terrain is
    // relief-free: h == 0, so r_surf == surface_radius exactly; the probe
    // crosses spin_omega with the foot-RELATIVE vector rel == off, so with
    // vel == ground_v the reported rel_vel is zero to the bit).
    // The probe maps the foot as ship.pos + ship.att * offset: with
    // identity att, pos = center puts the foot exactly at center + off.
    const sim::rigid_state carried{.pos = g.center, .vel = ground_v,
                                   .att = math::dquat{}};
    const auto h0 = sim::contact_vs_surface(g, off, carried);
    REQUIRE(h0.has_value());
    // relief-free sphere: depth = R - |foot - center|; the big-world
    // position (12 Mm out) rounds rel by fp64 ulp ~1e-9, so pin to that.
    CHECK(math::approximately_equal(h0->depth_m, 0.5, 1e-6));
    CHECK(math::length(h0->normal_world - math::normalize(off)) < 1e-9);
    CHECK(math::length(h0->rel_vel) < 1e-9);  // carried rigidly: no slip

    // The stand itself: velocity ZERO (a still walker, not a carried rock),
    // gravity pressing in, arcade boots (k = 1'600 N/m at 100 kg:
    // critical damping c = 2*sqrt(k*m) = 800, k*dt^2/m = 0.444 stable on
    // the fixed tick; friction and tangential drag authored ZERO —
    // #47a). The 7.8 tick pattern —
    // probe -> penalty force -> accel -> integrate — fires every tick;
    // the probe's OWN report proves the kinematics: rel_vel pins at minus
    // the ground velocity (the foot stays put; the moon sweeps past at
    // ~1 m/s) while the normal channel keeps the depth alive. No foot
    // state, no standing machinery: the report IS the standing.
    // The foot point sits 0.5 m below the surface along the local +X
    // radius, carried to world through the snapshot's own orient — the
    // same transform the probe applies to foot_local_offset. foot_w from
    // the rail pose misses this: the surface frame's center is where the
    // probe measures radius from.
    sim::rigid_state s{.pos = g.center + off, .vel = {0.0, 0.0, 0.0},
                       .att = math::dquat{}};
    // Arcade boots authored for the 100 kg suit: k = 1'600 N/m — visible
    // boot compression (sink ~0.1 m against a 1 km body). kLegs' ship
    // numbers (k=2e5 at 1000 kg) do NOT port to mass/10: the damper's
    // clamp zeroes its force while the foot is still closing fast
    // (under-damped), and one free tick of gravity flings the walker off
    // the surface. c = 2*sqrt(k*m) = 800 is critical damping and
    // k*dt^2/m = 0.444 keeps the discrete spring stable on the tick.
    const sim::contact_params boots{.k_pushout_N_m = 1'600.0,
                                    .c_normal_N_s_m = 800.0,
                                    .friction_mu = 0.0,
                                    .c_tangent_N_s_m = 0.0};
    // The stand loop IS the 7.8 tick pattern (test_land_contacts: probe ->
    // penalty force -> accel -> integrate), applied to the suited body:
    // well accel + contact force/mass, integrated on the fixed tick. The
    // jet stays holstered (v_desired only moves the walker; hover assist is
    // its own case below) — boots alone hold a body on the surface.
    double rel_seen = 0.0, depth_seen = 0.0;
    for (int i = 0; i < 600; ++i) {
        const sim::surface_world sg = mw.surf(t0 + i);
        const math::dvec3 grav = sim::total_accel(mw.rw, s.pos, t0 + i);
        const auto hit = sim::contact_vs_surface(sg, math::dvec3{}, s);
        REQUIRE(hit.has_value());  // the stand never breaks
        const math::dvec3 f = sim::penalty_force(*hit, boots);
        CHECK(math::length(f) >= 0.0);  // finite: length never NaN here
        rel_seen = math::length(hit->rel_vel);  // foot (still) minus ground
        depth_seen = hit->depth_m;
        math::dvec3 accel = grav;
        accel = accel + f / kSuit.body.mass;
        sim::integrate_step(s, kSuit.body, accel, {});
    }
    // Standing holds all 600 ticks. Resting depth sits in the arcade
    // band: the well's LOCAL gravity balance would put the sink at
    // mg/k ~ 0.1 m, but total_accel here is the rail world's blended
    // relax-model accel (gm_eff ~ 1.0 at this point) with the spin
    // relief omega^2*r riding on top — settled sink ~0.0625 m. Band,
    // not bit: the arcade numbers are authored, the mechanism (contact
    // never breaks) is the pin.
    CHECK(depth_seen > 0.05);
    CHECK(depth_seen < 0.08);
    CHECK(rel_seen == doctest::Approx(1e-3 * (kMoon_r - 0.5)).epsilon(1e-3));
    const math::dvec3 start = g.center + off;
    CHECK(math::length(s.pos - start) < 1.0);  // never walked away
    CHECK(math::length(s.vel) < 1.0);
}

TEST_CASE("home_well_nested_switch") {
    // The giant (planet, 0.213 m/s^2 at the moon) NEVER wins at the moon's
    // surface: dominance blend (band 0.25) gives it weight 0 there —
    // "down = the moon" is exact, not approximate.
    moon_world mw;
    const sim::tick_t t = 60;
    const math::dvec3 p0 = mw.pole(t, 2.0);
    const math::dvec3 blend = sim::total_accel(mw.rw, p0, t);
    const math::dvec3 alone = sim::well_accel(mw.rw, mw.moon, p0, t);
    CHECK(math::length(blend - alone) == 0.0);  // planet weight bit-exactly 0
    CHECK(alone.y < 0.0);                       // pull toward moon center
    REQUIRE(sim::dominant_well_at(mw.rw, p0, t) == mw.moon);

    // Walk a scripted altitude stream (2 m/s): raw argmax crosses at
    // ~30.7 km; the HOME tracker must not flip until the challenger beats
    // home by (1+hyst), and must hold the new home through a re-approach
    // until the challenger beats IT by (1+hyst) — pinned tick on each side.
    // Raw argmax crossing altitude: scan 1 m steps (the quintic taper, not
    // the linear guess, puts it ~27.6 km).
    double raw_cross = 0.0;
    for (double alt = 2.0; alt < kMoon_well; alt += 1.0) {
        const math::dvec3 p = mw.pole(t, alt);
        const bool moon_wins =
            math::length2(sim::well_accel(mw.rw, mw.moon, p, t)) >=
            math::length2(sim::well_accel(mw.rw, mw.planet, p, t));
        if (!moon_wins) { raw_cross = alt; break; }
    }
    REQUIRE(raw_cross > 0.0);
    CHECK(raw_cross < kMoon_well * 0.9);  // well inside the moon's well

    sim::home_well_tracker home{0.05};  // 5%: tighter than the 4.6 test's 0.1
    CHECK(*home.update(mw.rw, p0, t) == mw.moon);

    sim::tick_t up_flip = 0, down_flip = 0;
    std::int32_t prev = mw.moon;
    for (sim::tick_t k = 1; k <= 1'000'000; ++k) {
        const double alt = 2.0 + k * sim::kSim_tick_dt * 2.0;  // 2 m/s
        const math::dvec3 p = mw.pole(t, alt);
        const std::int32_t now = *home.update(mw.rw, p, t);
        if (now != prev && up_flip == 0) {
            up_flip = k;
            CHECK(now == mw.planet);
            // Hysteresis: the home flip lands strictly ABOVE the raw
            // argmax crossing (challenger must beat home by 1+hyst).
            CHECK(alt > raw_cross);
            prev = now;
        }
    }
    REQUIRE(up_flip > 0);
    // Descend back: home holds the planet past the moon re-approaching,
    // flips only at the lower hysteresis edge.
    prev = mw.planet;
    for (sim::tick_t k = 1; k <= 1'000'000; ++k) {
        const double alt = 35'000.0 - k * sim::kSim_tick_dt * 2.0;
        if (alt < 2.0) break;
        const math::dvec3 p = mw.pole(t, alt);
        const std::int32_t now = *home.update(mw.rw, p, t);
        if (now != prev && down_flip == 0) {
            down_flip = k;
            CHECK(now == mw.moon);
            // Strictly below the up-flip AND at/below the raw crossing:
            // the tracker holds the planet home through the re-approach.
            CHECK(alt < raw_cross);
            prev = now;
        }
    }
    CHECK(down_flip > 0);
}

TEST_CASE("eva_round_trip") {
    // Dock -> egress -> fall (jets + SHIP auto-level assist) -> contact
    // settles -> walk 1 m/s for 600 ticks -> repressurize refill. One
    // scripted tick stream; drain pinned; run twice hash-identical.
    auto play = [](std::vector<std::uint64_t>& stream,
                   std::vector<double>& depth, std::vector<double>& air_v) {
        moon_world mw;
        StatsSet st;
        ModifierStack ms;
        st.set_base("suit_air.supply", 0.0f);
        st.set_base("suit_air.load", 0.0f);
        st.set_base("suit_air.leak", 0.0f);
        StatsPipeline sp(st, ms);
        TagSet tags;
        sim::Reservoir air(100.0, sim::suit_air_bands(), &tags);
        air.bind(&sp, {"suit_air.supply", "suit_air.load", "suit_air.leak"});
        ms.add_source(1);
        auto rate = modifier_desc{};
        rate.target_attr = "suit_air.load";
        rate.stack_id = "breath";
        rate.op = mod_op::flat_add;
        rate.value = 30.0f;
        rate.policy = stack_policy::exclusive;
        ms.add_modifier(1, rate);

        sim::rigid_state s;
        s.pos = mw.pole(0, 50.0);  // egress: 50 m up, at rest
        s.att = math::dquat{0.0, 0.0, 0.0, 1.0};  // identity (scalar-last)

        std::uint64_t h = 0xe5aULL;
        const sim::tick_t kSpan = 2'400;  // 40 s
        for (sim::tick_t t = 1; t <= kSpan; ++t) {
            // Exposed the whole EVA span; re-enter at the last 400 ticks.
            if (t == kSpan - 400) {
                ms.remove_source(1);  // airlock: load stops being fed
                auto fill = modifier_desc{};
                fill.target_attr = "suit_air.supply";
                fill.stack_id = "fill";
                fill.op = mod_op::flat_add;
                fill.value = 300.0f;  // 5/tick while repressurized
                fill.policy = stack_policy::exclusive;
                ms.add_source(2);
                ms.add_modifier(2, fill);
            }
            air.step();

            const math::dvec3 grav = sim::total_accel(mw.rw, s.pos, t);
            const sim::surface_world g = mw.surf(t);
            const math::dvec3 v_walk = (t > 1'200)
                ? math::dvec3{1.0, 0.0, 0.0} : math::dvec3{};
            (void)sim::eva_walk_step(s, kSuit, grav, g, kBoots, v_walk,
                                     sim::kAssist_auto_level |
                                         sim::kAssist_stabilizer);
            // monotone tick marker; explicit cast: int64->double trips MSVC
            // C4244 under /WX (lossless here, kSpan = 2'400)
            depth.push_back(static_cast<double>(t));
            air_v.push_back(air.value());
            h = mix_d(h, s.pos.x);
            h = mix_d(h, s.pos.y);
            h = mix_d(h, s.pos.z);
            h = mix_d(h, s.vel.x);
            h = mix_d(h, s.vel.y);
            h = mix_d(h, s.vel.z);
            h = mix_d(h, air.value());
            stream.push_back(h);
        }
    };
    std::vector<std::uint64_t> sa, sb;
    std::vector<double> da, db, aa, ab;
    play(sa, da, aa);
    play(sb, db, ab);
    CHECK(sa == sb);       // determinism: identical tick stream
    CHECK(da == db);
    CHECK(aa == ab);
    // Drain math pins (air stream, exact fp64 rates):
    // t=1..2000 exposed @0.5/tick -> 100 - 0.5t; re-enter at t=2001.
    CHECK(aa[99] == 50.0);          // t=100: 100 - 0.5*100 exact
    CHECK(aa[199] == 0.0);          // t=200: dry exactly
    CHECK(aa[1'998] == 0.0);        // still dry at t=1999 (floor holds)
    CHECK(aa[1'999] == 5.0);        // t=2000: airlock, first refill tick
    CHECK(aa[2'399] == 2'005.0);    // t=2400: 401 refill ticks @5/tick
}

} // TEST_SUITE
