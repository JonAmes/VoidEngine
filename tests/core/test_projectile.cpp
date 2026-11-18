// Phase 9.8 — projectile ballistics: ordinary entities on the fixed-step
// integrator. Everything hand-computed closed-form; the kernel is the ship
// spine (assemble_tick_inputs + integrate_step*), so the pins below derive
// from the semi-implicit Euler closed form:
//   v_n = v0 + a*n*dt;  p_n = p0 + v0*n*dt + a*dt^2 * n(n+1)/2   (dt = 1/60)
// Corridor cases fly at |y| = 3000 m, beyond every well edge -> EXACTLY
// zero gravity (arcade wells are 0 outside, no 1/r^2 tails).
#include <doctest/doctest.h>

#include <ve/core/math/vec.h>
#include <ve/core/sim/projectile.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>

#include <cmath>
#include <vector>

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using math::dvec3;

rail_world test_world() {
    rail_world w;
    body_def pl{};  // root, at rest at origin
    pl.surface_radius = 100.0;
    pl.well_taper_start = 200.0;
    pl.well_radius = 1000.0;
    pl.surface_g = 10.0;
    REQUIRE(w.add_body(pl).has_value());  // id 0
    return w;
}

std::vector<comp_bounds> hull_at(dvec3 c, std::uint32_t id = 42) {
    return {comp_bounds{id, obb{c, {5.0, 5.0, 5.0}, {}}}};
}

rigid_state ship_at(dvec3 p, dvec3 v = {}) {
    rigid_state s;
    s.pos = p;
    s.vel = v;
    return s;
}

} // namespace

TEST_CASE("projectile_ballistics") {
    const rail_world w = test_world();

    // A1) Hand-computed intercept, drift cuts both ways.
    //     Shooter (0,3000,0) v=(0,0,5); target (400,3000,0) v=(0,0,15).
    //     Target-frame relative vel u = v_t - v_s = (0,0,10); muzzle 800
    //     must cancel it: sin(theta) = 10/800 about +X. World bullet vel
    //     z = 5 + 800*sin = 15 (rides the target's lane exactly).
    //     Entry (target -X face at fixed x = 395):
    //     t_e = 395 / 800*cos ~ 0.4938 s -> lands in step cmd 29.
    {
        const double th = std::asin(10.0 / 800.0);
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}, {0.0, 0.0, 5.0}),
                                   {std::cos(th), 0.0, std::sin(th)}, 800.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        // Inherited velocity pinned: ship vel + aim * muzzle.
        CHECK(pr->state.vel.x == doctest::Approx(800.0 * std::cos(th)).epsilon(1e-12));
        CHECK(pr->state.vel.z == doctest::Approx(15.0).epsilon(1e-12));

        std::optional<projectile_hit> hit;
        int hit_cmd = -1;
        for (tick_t t = 0; t < 60 && !hit; ++t) {
            const double s = static_cast<double>(t) / 60.0;
            auto res = projectile_step(w, *pr, t, 1,
                                       hull_at({400.0, 3000.0, 15.0 * s}), {0.0, 0.0, 15.0});
            if (res.life == projectile_life::struck) { hit = res.hit; hit_cmd = static_cast<int>(t); }
            REQUIRE(res.life != projectile_life::expired);
        }
        REQUIRE(hit.has_value());
        CHECK(hit_cmd == 29);  // t_e ~ 0.4938 s -> the (29/60, 30/60] step
        CHECK(hit->component == 42u);  // the report NAMES the component
        // Drift is along z; the -X face never moves in x -> closing speed is
        // 800*cos(theta) alone and entry x is EXACTLY 395 (the face).
        CHECK(hit->at.pos.x == doctest::Approx(395.0).epsilon(1e-9));
        CHECK(hit->at.pos.y == doctest::Approx(3000.0).epsilon(1e-12));
        CHECK(hit->at.t > 0.0);
        CHECK(hit->at.t <= kSim_tick_dt);
        CHECK(hit->at.normal.x == doctest::Approx(-1.0).epsilon(1e-12));  // back at shooter
    }

    // A2) Equal drift cancels to zero lead: target v = (0,0,5) == shooter,
    //     aim straight +X. t_e = 395 / 800 ~ 0.49375 s -> step cmd 29.
    {
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}, {0.0, 0.0, 5.0}),
                                   {1.0, 0.0, 0.0}, 800.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        bool struck = false;
        int hit_cmd = -1;
        for (tick_t t = 0; t < 60 && !struck; ++t) {
            const double s = static_cast<double>(t) / 60.0;
            auto res = projectile_step(w, *pr, t, 1,
                                       hull_at({400.0, 3000.0, 5.0 * s}), {0.0, 0.0, 5.0});
            if (res.life == projectile_life::struck) { struck = true; hit_cmd = static_cast<int>(t); }
        }
        CHECK(struck);
        CHECK(hit_cmd == 29);  // ceil(0.49375*60) - 1... in (29/60, 30/60]
    }

    // A3) MOVING target along the firing axis: target recedes at (60,0,0).
    //     -X face rides x = 400 + 60t - 5 = 395 + 60t; closing 800-60=740
    //     -> t_e = 395/740 ~ 0.5338 s -> step cmd 32. (A sweep that ignored
    //     target_vel would hit early at cmd 29.)
    {
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}), {1.0, 0.0, 0.0}, 800.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        bool struck = false;
        int hit_cmd = -1;
        for (tick_t t = 0; t < 60 && !struck; ++t) {
            const double s = static_cast<double>(t) / 60.0;
            auto res = projectile_step(w, *pr, t, 1,
                                       hull_at({400.0 + 60.0 * s, 3000.0, 0.0}), {60.0, 0.0, 0.0});
            if (res.life == projectile_life::struck) { struck = true; hit_cmd = static_cast<int>(t); }
        }
        CHECK(struck);
        CHECK(hit_cmd == 32);
    }

    // A4) Nearest entry wins within a step, regardless of candidate order:
    //     two overlapping hulls entered in the SAME tick segment — id 8
    //     (entry x=196) listed FIRST, id 7 (entry x=195) second. Entry
    //     195/800 = 0.24375 s -> step cmd 14; strict-less pick keeps id 7.
    {
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}), {1.0, 0.0, 0.0}, 800.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        std::vector<comp_bounds> two{comp_bounds{8, obb{{201.0, 3000.0, 0.0}, {5.0, 5.0, 5.0}, {}}},
                                     comp_bounds{7, obb{{200.0, 3000.0, 0.0}, {5.0, 5.0, 5.0}, {}}}};
        std::uint32_t comp = 0;
        int hit_cmd = -1;
        for (tick_t t = 0; t < 60 && comp == 0; ++t) {
            auto res = projectile_step(w, *pr, t, 1, two);
            if (res.life == projectile_life::struck) { comp = res.hit->component; hit_cmd = static_cast<int>(t); }
        }
        CHECK(comp == 7u);   // nearer entry wins despite being scanned second
        CHECK(hit_cmd == 14);
    }

    // B) Gravity well: radial shot from (0,50,0) straight +Y, muzzle 100,
    //    inside the CONSTANT core (r < taper 200) accel = -10y EXACT.
    //    n = 30 ticks: p = 50 + 100*30/60 - 10*(30*31/2)/3600
    //                   = 100 - 4650/3600 = 98.708333...; v = 95.
    {
        auto pr = spawn_projectile(ship_at({0.0, 50.0, 0.0}), {0.0, 1.0, 0.0}, 100.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        for (tick_t t = 0; t < 30; ++t) {
            auto res = projectile_step(w, *pr, t, 1, {});
            REQUIRE(res.life == projectile_life::alive);
        }
        CHECK(pr->state.pos.y == doctest::Approx(100.0 - 4650.0 / 3600.0).epsilon(1e-9));
        CHECK(pr->state.vel.y == doctest::Approx(95.0).epsilon(1e-12));
    }

    // C) Lifetime: ttl 120 ticks. Alive through step ending at tick 120,
    //    the NEXT step refuses to advance and reports expired.
    {
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}), {1.0, 0.0, 0.0}, 800.0, 0, 120, 1);
        REQUIRE(pr.has_value());
        for (tick_t t = 0; t < 120; ++t) {
            auto res = projectile_step(w, *pr, t, 1, {});
            REQUIRE(res.life == projectile_life::alive);
        }
        const dvec3 p_last = pr->state.pos, v_last = pr->state.vel;
        auto res = projectile_step(w, *pr, 120, 1, {});
        CHECK(res.life == projectile_life::expired);
        CHECK(pr->state.pos == p_last);  // terminal: no advance
        CHECK(pr->state.vel == v_last);
    }

    // D) Coarse LOD block (divisor 5, gravity sampled at interval start —
    //    exactly what lod.cpp does for far bodies): still deterministic,
    //    still NO TUNNELING — a 5-tick block moves the round ~67 m, far
    //    past the 10 m hull slab, and the swept segment registers it.
    {
        const double th = std::asin(10.0 / 800.0);
        auto pr = spawn_projectile(ship_at({0.0, 3000.0, 0.0}, {0.0, 0.0, 5.0}),
                                   {std::cos(th), 0.0, std::sin(th)}, 800.0, 0, 600, 1);
        REQUIRE(pr.has_value());
        bool struck = false;
        int hit_cmd = -1;
        for (tick_t t = 0; t <= 30; t += 5) {
            const double s = static_cast<double>(t) / 60.0;
            auto res = projectile_step(w, *pr, t, 5,
                                       hull_at({400.0, 3000.0, 15.0 * s}), {0.0, 0.0, 15.0});
            if (res.life == projectile_life::struck) { struck = true; hit_cmd = static_cast<int>(t); break; }
        }
        CHECK(struck);
        CHECK(hit_cmd == 25);  // t_e 0.4938 s sits in block [25,30)
    }

    // E) Degenerate spawns refuse (exception-free core).
    {
        CHECK_FALSE(spawn_projectile(ship_at({}), {0.0, 0.0, 0.0}, 800.0, 0, 10, 1).has_value());
        CHECK_FALSE(spawn_projectile(ship_at({}), {0.0, 0.0, -2.0}, 800.0, 0, 10, 1).has_value());
        CHECK_FALSE(spawn_projectile(ship_at({}), {0.0, 0.0, -1.0}, -5.0, 0, 10, 1).has_value());
    }
}

TEST_CASE("projectile_replay_hash") {
    const rail_world w = test_world();

    // One arc: radial shot through the core INTO the taper zone (closed-
    // form irrelevant here — determinism is the point), 200 ticks.
    auto run = [&](double muzzle) {
        auto pr = spawn_projectile(ship_at({0.0, 50.0, 0.0}), {0.0, 1.0, 0.0}, muzzle, 0, 1000, 1);
        REQUIRE(pr.has_value());
        std::vector<std::uint64_t> hashes;
        for (tick_t t = 0; t < 200; ++t) {
            auto res = projectile_step(w, *pr, t, 1, {});
            REQUIRE(res.life == projectile_life::alive);
            hashes.push_back(state_hash(t, pr->state));
        }
        return hashes;
    };

    const auto a = run(100.0), b = run(100.0);
    CHECK(a == b);  // bit-identical per-tick stream (deterministic kernel)
    CHECK(replay_interval_hash(a, 0, a.size() - 1) != 0);

    const auto c = run(100.0 + 1e-9);  // 1-ULP-ish muzzle canary
    CHECK(a != c);
    CHECK(replay_interval_hash(a, 0, a.size() - 1) != replay_interval_hash(c, 0, c.size() - 1));

    // Time-compressed: divisor-5 lattice (lod pattern). Deterministic vs
    // itself, and within the hand-computed coarse-vs-fine deviation bound
    // for constant accel: |dt^2*(n(n+1)/2 - D^2*m(m+1)/2)|*g with n=200
    // (fine), D=5, m=40 (coarse) = |10*20100 - 10*820*25| / 3600 = 1.11 m
    // (upper bound; taper-zone sampling only shrinks g).
    auto run_coarse = [&](double muzzle) {
        auto pr = spawn_projectile(ship_at({0.0, 50.0, 0.0}), {0.0, 1.0, 0.0}, muzzle, 0, 1000, 1);
        REQUIRE(pr.has_value());
        std::vector<std::uint64_t> hashes;
        for (tick_t t = 0; t < 200; t += 5) {
            auto res = projectile_step(w, *pr, t, 5, {});
            REQUIRE(res.life == projectile_life::alive);
            hashes.push_back(state_hash(t, pr->state));
        }
        return std::make_pair(hashes, pr->state.pos.y);
    };
    const auto h1 = run_coarse(100.0), h2 = run_coarse(100.0);
    CHECK(h1.first == h2.first);                       // compressed is deterministic
    // fine-run end y (last fine step state): recompute
    auto pr = spawn_projectile(ship_at({0.0, 50.0, 0.0}), {0.0, 1.0, 0.0}, 100.0, 0, 1000, 1);
    REQUIRE(pr.has_value());
    for (tick_t t = 0; t < 200; ++t) (void)projectile_step(w, *pr, t, 1, {});
    CHECK(std::abs(h1.second - pr->state.pos.y) <= 1.12);  // hand-computed bound
}
