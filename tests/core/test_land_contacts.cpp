// test: landed_contacts — 7.8 penalty contacts (#47a): spring-damper
// push-out + damping vs the #29b analytic surface, evaluated at the
// body's moving/spinning pose (#41). NO contact solver, NO stored
// contacts, deterministic by construction at fixed tick. NOTHING breaks
// here — gear failure is never a contact concern (arcade).
//
// Hand pins (all SI, authored fixtures — no unit math this phase):
//   ship 1000 kg, 4 legs (k=2e5 N/m, c_n=14142.135623730951 N·s/m =
//   2*sqrt(k*m): zeta=1 at full weight), surface g=10.
//   Static 4-leg: each leg holds mg/4 = 2500 N -> depth 2500/2e5 = 0.0125 m.
//   One leg under the CoM: full mg -> depth 10000/2e5 = 0.05 m.
//   Free fall 5 m at g=10: first sampled contact tick is exactly 60.
//   Tangential force is deliberately ABSENT in 7.8 (see contacts.cpp):
//   the analytic normal is radial, so a sustained tangential cap makes a
//   "resting" body ride the curvature instead of settling. Capture-zone
//   hold-down and snap-to-rest (7.9) carry landed bodies; friction with
//   a flat pad frame returns with on-foot verbs (7.13). So tangential
//   motion coasts untouched and every pin below is hand-computable.
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"
#include "ve/core/sim/contacts.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/terrain.h"
#include "ve/core/sim/time.h"

namespace {

using namespace ve::core;
using sim::body_def;
using sim::contact_hit;
using sim::contact_params;
using sim::integrate_step;
using sim::rail_world;
using sim::rigid_params;
using sim::rigid_state;
using sim::stamp_type;
using sim::surface_world;
using sim::terrain_def;
using sim::terrain_stamp;

// Moon at world root: R = 1e6 m, surface g = 10 m/s^2, well to 1.3e6.
struct moon_world {
    rail_world rw;
    std::int32_t id = 0;
    explicit moon_world(double spin_rad_s = 0.0) {
        body_def b;
        b.surface_radius = 1'000'000.0;
        b.well_radius = 1'300'000.0;
        b.well_taper_start = 1'100'000.0;
        b.surface_g = 10.0;
        b.spin_axis = {0.0, 1.0, 0.0};
        b.spin_rate_rad_s = spin_rad_s;
        id = *rw.add_body(b);
    }
    // #29d pad: full-weight flatten disc centered on local +X (the pad
    // azimuth the spin scenarios use) — authored-flat to the bit. relief
    // 0 keeps the rest of the moon a smooth analytic sphere.
    terrain_def land{.seed = 7, .relief_m = 0.0, .base_frequency = 1.0, .octaves = 1};
    std::vector<terrain_stamp> pads{terrain_stamp{
        .type = stamp_type::flatten,
        .center_dir = {1.0, 0.0, 0.0},
        .radius_rad = 0.1,   // 100 km pad on a 1000 km moon
        .level_m = 0.0,
        .blend = 0.0}};      // blend=0 -> full weight across the whole disc
    // Surface snapshot at tick t (the only legal construction: rails are
    // the pose source, spin omega rides the authored axis/rate).
    surface_world surf(std::uint64_t t) const {
        surface_world s;
        s.body = rw.body(id);
        s.terrain = &land;
        s.stamps = pads;
        s.center = *rw.position(id, t);
        s.center_vel = *rw.velocity(id, t);
        s.orient = *rw.orientation(id, t);
        s.spin_omega = s.body->spin_rate_rad_s * s.body->spin_axis;
        return s;
    }
};

constexpr contact_params kLegs{
    .k_pushout_N_m = 200'000.0, .c_normal_N_s_m = 14'142.135623730951,
    .friction_mu = 0.0, .c_tangent_N_s_m = 0.0};  // 7.8: normal channel only
constexpr rigid_params kShip{.mass = 1000.0, .inertia_diag = {1e7, 1e7, 1e7}};
constexpr math::dvec3 kFeet4[4] = {{10.0, -15.0, 10.0}, {10.0, -15.0, -10.0},
                                   {-10.0, -15.0, 10.0}, {-10.0, -15.0, -10.0}};

// One fixed-tick step: snapshot surface at t, probe every gear foot, sum
// penalty forces + torques, add the well pull, integrate. Ordered, pure per
// tick — the determinism argument of #47a is that nothing else exists.
void tick(moon_world& mw, rigid_state& s, const std::vector<math::dvec3>& feet,
          const contact_params& p, std::uint64_t t) {
    const surface_world surf = mw.surf(t);
    math::dvec3 accel = sim::well_accel(mw.rw, mw.id, s.pos, t);  // mass-free
    math::dvec3 tau_world{0.0, 0.0, 0.0};
    for (const math::dvec3& off : feet) {
        const std::optional<contact_hit> hit = contact_vs_surface(surf, off, s);
        if (!hit) continue;
        const math::dvec3 pf = penalty_force(*hit, p);
        accel = accel + pf / kShip.mass;
        tau_world = tau_world + math::cross(math::rotate(s.att, off), pf);
    }
    // integrate_step wants raw torque in BODY axes (#45b).
    const math::dvec3 tau_body = math::rotate(math::conjugate(s.att), tau_world);
    integrate_step(s, kShip, accel, tau_body);
}

rigid_state drop_state(double com_alt_m) {
    rigid_state s;
    s.pos = {0.0, 1'000'000.0 + com_alt_m, 0.0};
    return s;  // att default-constructed identity, vel/omega zero
}

TEST_CASE("landed_contacts") {
    SUBCASE("static drop: settles at hand pin mg/4k, all motion gone") {
        moon_world mw;
        std::vector<math::dvec3> feet{std::begin(kFeet4), std::end(kFeet4)};
        // Feet hang 15 m below CoM: CoM at surface+20 -> lowest foot 5 m
        // above ground; 5 m free fall at g = 10 is exactly 1 s = 60 ticks.
        rigid_state s = drop_state(20.0);
        double first_depth = -1.0, max_depth = 0.0;
        double last_depth = 0.0;
        for (std::uint64_t t = 0; t < 3000; ++t) {
            const surface_world surf = mw.surf(t);
            // record deepest foot this tick (pre-step state)
            double deepest = 0.0;
            for (const math::dvec3& off : feet) {
                const auto hit = contact_vs_surface(surf, off, s);
                if (hit) deepest = std::max(deepest, hit->depth_m);
            }
            if (deepest > 0.0) {
                if (first_depth < 0.0) first_depth = static_cast<double>(t);
                max_depth = std::max(max_depth, deepest);
            }
            last_depth = deepest;
            tick(mw, s, feet, kLegs, t);
        }
        CHECK(first_depth == 60.0);  // 60 ticks of 5 m free fall exactly
        // Compliance bound: the per-foot normal force cannot exceed the
        // hard ceiling k*dmax = 2e4 N (push-out reverses the motion long
        // before), so depth stays in (rest pin, 2W/k = 0.1 m). An
        // underdamped contact (c=14142 vs 2*sqrt(kW)=28284) overshoots to
        // ~0.083 — pinned from the run.
        CHECK(max_depth > 0.0125);
        CHECK(max_depth <= 0.1);
        CHECK(max_depth == doctest::Approx(0.0832333).epsilon(1e-4));
        // Rest (50 s ~ 17 settling time constants): total spring force
        // carries weight -> d = mg/4k = 0.0125, to the last guarded bit.
        CHECK(last_depth == doctest::Approx(0.0125).epsilon(1e-3));  // discrete-contact fixed point
        // Velocity annihilated (fixed point at rest; damper holds it).
        CHECK(std::fabs(s.vel.x) < 1e-9);
        CHECK(std::fabs(s.vel.y) < 1e-8);  // discrete-contact limit cycle
        CHECK(std::fabs(s.vel.z) < 1e-9);
        CHECK(math::length(s.omega_body) < 1e-9);
        // Gear squash read-out: depth per foot, symmetric (level surface +
        // symmetric gear). Per-leg normal force = k*d = 2500 N (weight/4),
        // exactly: settled v_n = 0 leaves only the spring term.
        const surface_world surf = mw.surf(3000);
        const auto hit = contact_vs_surface(surf, kFeet4[0], s);
        REQUIRE(hit.has_value());
        CHECK(hit->depth_m == doctest::Approx(0.0125).epsilon(1e-3));
        const math::dvec3 f = penalty_force(*hit, kLegs);
        CHECK(f.y == doctest::Approx(2500.0).epsilon(1e-9));
        // No tangential force channel in 7.8: the ONLY off-normal force is
        // the foot normal's own tiny tilt at 15 m stance on a 1e6 m sphere
        // (10/1e6 * 2500 = 0.025 N — radial geometry, not friction).
        CHECK(std::fabs(f.x) <= 0.0251);
        CHECK(std::fabs(f.z) <= 0.0251);
    }

    SUBCASE("one leg on the CoM line: mg/k pin, zero torque") {
        moon_world mw;
        std::vector<math::dvec3> feet{math::dvec3{0.0, -15.0, 0.0}};
        rigid_state s = drop_state(20.0);
        for (std::uint64_t t = 0; t < 3000; ++t) tick(mw, s, feet, kLegs, t);
        const auto hit = contact_vs_surface(mw.surf(3000), feet[0], s);
        REQUIRE(hit.has_value());
        CHECK(hit->depth_m == doctest::Approx(0.05).epsilon(1e-9));  // mg/k
        // Force line passes through the CoM: no torque, no lateral drift.
        CHECK(math::length(s.omega_body) == 0.0);
        CHECK(s.vel.x == 0.0);
    }

    SUBCASE("off-axis legs: equal depths, zero net torque") {
        moon_world mw;
        std::vector<math::dvec3> feet{std::begin(kFeet4), std::end(kFeet4)};
        rigid_state s = drop_state(20.0);
        for (std::uint64_t t = 0; t < 3000; ++t) tick(mw, s, feet, kLegs, t);
        const surface_world surf = mw.surf(3000);
        double d0 = 0.0, d1 = 0.0;
        for (int i = 0; i < 2; ++i) {
            const auto hit = contact_vs_surface(surf, feet[i], s);
            REQUIRE(hit.has_value());
            if (i == 0) d0 = hit->depth_m; else d1 = hit->depth_m;
        }
        CHECK(d0 == doctest::Approx(0.0125).epsilon(1e-3));
        CHECK(d1 == d0);  // symmetric stance on a level pad: identical squash
        CHECK(math::length(s.omega_body) == 0.0);
    }

    SUBCASE("no tangential force: a sliding hull coasts, still settles vertically") {
        // 60 m/s lateral at touchdown: with the tangential channel absent
        // the lateral velocity is untouched; only the normal channel acts.
        moon_world mw;
        std::vector<math::dvec3> feet{std::begin(kFeet4), std::end(kFeet4)};
        rigid_state s = drop_state(20.0);
        s.vel = {60.0, 0.0, 0.0};
        for (std::uint64_t t = 0; t < 1800; ++t) tick(mw, s, feet, kLegs, t);
        // Lateral motion coasts: the normal channel alone cannot brake it
        // (tiny curved-surface modulation only, no Coulomb drain).
        CHECK(s.vel.x == doctest::Approx(60.0).epsilon(1e-5));
        CHECK(std::fabs(s.vel.z) < 1e-3);
        // Still settles vertically to the same mg/4k pin while sliding.
        const auto hit = contact_vs_surface(mw.surf(1800), kFeet4[0], s);
        REQUIRE(hit.has_value());
        CHECK(hit->depth_m == doctest::Approx(0.0125).epsilon(1e-3));
    }

    SUBCASE("spinning moon: pad under the spun pose, sampled where it actually is") {
            // Authored flatten disc centered on local +Z: at spin_rate
            // 0.001 rad/s the pad surface sweeps v = omega x r = 1000 m/s.
            moon_world mw(0.001);
            const surface_world s0 = mw.surf(0);
            const math::dvec3 c0(*mw.rw.position(mw.id, 0));
            const math::dvec3 pad_dir0 = math::rotate(s0.orient, math::dvec3{0.0, 0.0, 1.0});
            const math::dvec3 pad_pt = c0 + 1'000'000.0 * pad_dir0;
            const math::dvec3 pad_vel = math::cross(s0.spin_omega, pad_pt - c0);
            CHECK(pad_vel.x == doctest::Approx(1000.0).epsilon(1e-9));

            // The pad's flat is authored to the bit: a pose 1 m under the
            // spun pad center registers exactly depth 1 (the pad rides the
            // orientation: sampled AT the spun pose, #41).
            {
                const math::dvec3 qpos = c0 + (1'000'000.0 - 1.0) * pad_dir0;
                const auto hit = contact_vs_surface(s0, {}, {qpos, {}, math::dquat{}, {}});
                REQUIRE(hit.has_value());
                CHECK(hit->depth_m == doctest::Approx(1.0).epsilon(1e-9));
                CHECK(hit->normal_world.x == doctest::Approx(pad_dir0.x).epsilon(1e-9));
                CHECK(hit->normal_world.y == doctest::Approx(pad_dir0.y).epsilon(1e-9));
                CHECK(hit->normal_world.z == doctest::Approx(pad_dir0.z).epsilon(1e-9));
            }

            // Invariant: standing off the pad azimuth (local +X, same
            // radius) still finds the authored pad under the SPUN pose —
            // sampling the pad direction in the world frame at tick 0.
            {
                const math::dvec3 spun_pad = math::rotate(
                    *mw.rw.orientation(mw.id, 3000), math::dvec3{0.0, 0.0, 1.0});
                const math::dvec3 c(*mw.rw.position(mw.id, 3000));
                const auto hit = contact_vs_surface(
                    mw.surf(3000), {}, {c + (1'000'000.0 - 1.0) * spun_pad, {}, math::dquat{}, {}});
                REQUIRE(hit.has_value());
                CHECK(hit->depth_m == doctest::Approx(1.0).epsilon(1e-9));
            }

            // Free-fall onto the spinning moon: first contact is set by
            // altitude + g only (tangential motion does not change the
            // radial fall), so touchdown lands in the sqrt(2h/g) band
            // whatever the pad does.
            std::vector feet4(kFeet4, kFeet4 + 4);
            rigid_state s = drop_state(16.0);  // feet 1 m clear of the surface
            double max_depth = 0.0;
            for (std::uint64_t t = 0; t < 600; ++t) {
                tick(mw, s, feet4, kLegs, t);
                const surface_world surf = mw.surf(t);
                const math::dvec3 c(*mw.rw.position(mw.id, t));
                const auto hit = contact_vs_surface(surf, feet4[0], s);
                if (hit) max_depth = std::max(max_depth, hit->depth_m);
            }
            // 1 m fall onto the moving pad (impact v ~ 4.5 m/s): depth
            // sits between the rest pin and the k*dmax compliance ceiling.
            CHECK(max_depth > 0.0125);
            CHECK(max_depth < 0.05);
            // No-sink invariant through the whole fall+settle.
            for (std::uint64_t t = 590; t < 600; ++t) {
                const math::dvec3 c(*mw.rw.position(mw.id, t));
                CHECK(math::length(s.pos - c) > 1'000'000.0 - 0.2);
            }
        }

        SUBCASE("no contact above surface, and gates: degenerate -> zero force") {
        moon_world mw;
        rigid_state s = drop_state(100.0);  // feet 85 m clear
        CHECK(!contact_vs_surface(mw.surf(0), kFeet4[0], s).has_value());

        // Degenerate params: k <= 0, negative c/mu, NaN -> penalty_force 0
        // and params_ok false.
        const contact_hit planted{.depth_m = 0.01,
                                  .normal_world = {0.0, 1.0, 0.0},
                                  .pos_world = {0.0, 1'000'000.0, 0.0},
                                  .rel_vel = {10.0, 0.0, 0.0}};
        CHECK(penalty_force(planted, contact_params{.k_pushout_N_m = 0.0}) ==
              math::dvec3{0.0, 0.0, 0.0});
        CHECK(penalty_force(planted, contact_params{.k_pushout_N_m = -5.0}) ==
              math::dvec3{0.0, 0.0, 0.0});
        CHECK(!contact_params_ok(contact_params{.k_pushout_N_m = 1.0, .c_normal_N_s_m = -1.0}));
        CHECK(!contact_params_ok(contact_params{.k_pushout_N_m = 1.0, .friction_mu = -0.5}));
        CHECK(!contact_params_ok(contact_params{.k_pushout_N_m = 1.0, .c_tangent_N_s_m = std::numeric_limits<double>::quiet_NaN()}));
        CHECK(contact_params_ok(kLegs));

        // depth <= 0 is not a contact: zero force even with valid params.
        contact_hit grazing{.depth_m = 0.0, .normal_world = {0.0, 1.0, 0.0},
                            .pos_world = {}, .rel_vel = {}};
        CHECK(penalty_force(grazing, kLegs) == math::dvec3{0.0, 0.0, 0.0});

        // Poisoned state -> no hit (state_ok gate inside contact_vs_surface).
        rigid_state poison = s;
        poison.vel = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
        CHECK(!contact_vs_surface(mw.surf(0), kFeet4[0], poison).has_value());
    }

    SUBCASE("determinism: same tick stream -> bit-identical") {
        moon_world mw;
        std::vector<math::dvec3> feet{std::begin(kFeet4), std::end(kFeet4)};
        auto run = [&](std::vector<std::uint64_t>& out) {
            rigid_state s = drop_state(15.0);
            s.vel = {37.0, -4.0, 11.0};
            for (std::uint64_t t = 0; t < 600; ++t) {
                const surface_world surf = mw.surf(t);
                double d0 = 0.0;
                for (const math::dvec3& off : feet) {
                    const auto h = contact_vs_surface(surf, off, s);
                    if (h) d0 = std::max(d0, h->depth_m);
                }
                out.push_back(std::bit_cast<std::uint64_t>(d0));
                tick(mw, s, feet, kLegs, t);
            }
        };
        std::vector<std::uint64_t> a, b;
        run(a);
        run(b);
        CHECK(a.size() == b.size());
        CHECK(std::memcmp(a.data(), b.data(), a.size() * 8) == 0);

        // fp-sensitivity pin: a 1-ulp attitude nudge on the SAME stream
        // must change the trajectory hash somewhere — the hash chain is
        // sensitive to state, so replay-hash enforcement (Phase 8) bites.
        auto run_pert = [&](std::vector<std::uint64_t>& out) {
            rigid_state s = drop_state(15.0);
            s.vel = {37.0, -4.0, 11.0};
            s.vel.x += 1e-9;  // ulp-scale state nudge, diverges on contact
            for (std::uint64_t t = 0; t < 600; ++t) {
                const surface_world surf = mw.surf(t);
                double d0 = 0.0;
                for (const math::dvec3& off : feet) {
                    const auto h = contact_vs_surface(surf, off, s);
                    if (h) d0 = std::max(d0, h->depth_m);
                }
                out.push_back(std::bit_cast<std::uint64_t>(d0));
                tick(mw, s, feet, kLegs, t);
            }
        };
        std::vector<std::uint64_t> c;
        run_pert(c);
                    CHECK(std::memcmp(a.data(), c.data(), a.size() * 8) != 0);
    }
}

} // namespace
