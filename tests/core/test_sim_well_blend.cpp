// test: sim_well_blend — decision #17b done-when testbed: a moon orbits deep
// inside its planet's well; "stand on the moon, down = the moon, not the
// giant in its sky" must be a pinned fact. Pins: soft-dominance crossfade
// physics (moon-alone at the lunar surface, smooth zero-crossing in the
// overlap shell, C0 across every shell boundary by finite-difference
// refinement), and the stateful home-well tracker (hysteresis kills argmax
// chatter; a monotone approach switches exactly once; framing persists in
// the interplanetary void). Hand-authored bodies (#45d), tick 0 unless said.
//
// Testbed: planet id0 at origin (surface 1000, well 13000, taper 2000,
// g 9.8); moon id1 parent=planet, period 3600 s, radius 12000, phase 0
// (surface 400, well 5000, taper 1000, g 2.0). At tick 0 the moon center
// sits at (12000, 0, 0) on +X, so the +X axis is the planet->moon line.

#include <cmath>

#include <doctest/doctest.h>

#include "ve/core/sim/well_blend.h"

using namespace ve::core;
using ve::core::math::dvec3;  // nested-namespace names don't leak via using namespace ve::core
using sim::body_def;
using sim::rail_world;
using sim::tick_t;

namespace {
void add_moon_testbed(rail_world& w) {  // REQUIRE needs a void helper (doctest)
    body_def pl{};
    pl.surface_radius = 1000.0;
    pl.well_radius = 13000.0;
    pl.well_taper_start = 2000.0;
    pl.surface_g = 9.8;
    REQUIRE(w.add_body(pl).has_value());  // id 0, root at origin
    body_def mo{};
    mo.parent = 0;
    mo.orbit_period_s = 3600.0;
    mo.orbit_radius_m = 12000.0;
    mo.surface_radius = 400.0;
    mo.well_radius = 5000.0;
    mo.well_taper_start = 1000.0;
    mo.surface_g = 2.0;
    REQUIRE(w.add_body(mo).has_value());  // id 1
}
} // namespace

TEST_CASE("sim_well_blend") {
    rail_world w;
    add_moon_testbed(w);
    const tick_t t0 = 0;

    // Moon where the rail says it is (sanity for every pin below).
    CHECK(math::approximately_equal(*w.position(1, t0), dvec3(12000, 0, 0), 1e-9));

    // ---- #17b done-when: stand on the sub-planet lunar surface (11600,0,0).
    // Moon raw pull = 2.0 (r=400 <= taper 1000). The GIANT is still there:
    // planet raw pull at this point is ~0.436 (mid-deep ramp) — it must be
    // SUPPRESSED because moon/giant = 4.6x > the 0.25 dominance band.
    const dvec3 stand(11600, 0, 0);
    const double giant_raw = length(sim::well_accel(w, 0, stand, t0));
    CHECK(giant_raw > 0.43); CHECK(giant_raw < 0.44);  // the giant IS felt raw...
    const dvec3 net = sim::total_accel(w, stand, t0);
    CHECK(math::approximately_equal(net, dvec3(2.0, 0, 0), 1e-12));  // ...but down = moon.
    // Far side (12400,0,0): planet ALSO pulls -X there; blend must still be
    // pure moon (giant suppressed to w=0): net == (-2,0,0).
    CHECK(math::approximately_equal(sim::total_accel(w, dvec3(12400, 0, 0), t0), dvec3(-2.0, 0, 0), 1e-12));
    // Deep in the planet well, far from the moon: pure planet, hand-derived
    // g = 9.8 * (10/11)^2 * (13/11) = 9.8*1300/1331 at r=3000.
    CHECK(math::approximately_equal(sim::total_accel(w, dvec3(3000, 0, 0), t0),
                                    dvec3(-9.8 * 1300.0 / 1331.0, 0, 0), 1e-9));
    // Interplanetary void (both wells out of reach): EXACT zero vector.
    CHECK(sim::total_accel(w, dvec3(30000, 0, 0), t0) == dvec3(0, 0, 0));

    // ---- overlap shell: strengths comparable -> crossfade, not a jump.
    // At (10050,0,0): moon 1.715 (+X), planet 1.736 (-X); the weight-blended
    // net is a small -X pull (|a| ~0.017), strictly inside (0, min raw) —
    // the field passes CONTINUOUSLY through zero between downs.
    const dvec3 mix = sim::total_accel(w, dvec3(10050, 0, 0), t0);
    const double mix_len = length(mix);
    CHECK(mix.x < 0.0);
    CHECK(mix_len > 0.005); CHECK(mix_len < 0.1);
    CHECK(mix_len < length(sim::well_accel(w, 0, dvec3(10050, 0, 0), t0)));

    // ---- C0 across every shell boundary on the planet->moon line:
    // successive deltas shrink linearly with step (Lipschitz), i.e. no force
    // step anywhere. Two legs, BOTH stopping short of the moon center:
    // leg A x=1000->11500 (planet taper join, moon well edge at 7000, blend
    // zone, lunar taper), leg B x=12500->14000 (lunar far side, planet well
    // edge at 13000). A constant-g core flips pull direction AT the center
    // (r=0 singularity, by design: nothing stands at a center); walking
    // through it would measure that intended flip, not a shell artifact.
    auto walk = [&](double x0, double x1, double dx) {
        double max_delta = 0.0;
        dvec3 prev = sim::total_accel(w, dvec3(x0, 0, 0), t0);
        for (double x = x0 + dx; x <= x1 + 1e-9; x += dx) {
            const dvec3 a = sim::total_accel(w, dvec3(x, 0, 0), t0);
            max_delta = std::max(max_delta, length(a - prev));
            prev = a;
        }
        return max_delta;
    };
    const double d_coarse = std::max(walk(1000.0, 11500.0, 0.5), walk(12500.0, 14000.0, 0.5));
    const double d_fine = std::max(walk(1000.0, 11500.0, 0.05), walk(12500.0, 14000.0, 0.05));
    CHECK(d_coarse < 0.02);
    CHECK(d_fine * 8.0 < d_coarse);  // ~10x smaller with 10x finer steps

    // ---- home-well tracker: hysteresis vs argmax chatter. Right on the
    // strength crossing (10050 vs 10100), the RAW argmax alternates every
    // step; the hysteresis band (0.1) must refuse to flip.
    sim::home_well_tracker tr;
    REQUIRE(*tr.update(w, dvec3(3000, 0, 0), t0) == 0);       // framing: planet
    CHECK(length(sim::well_accel(w, 0, dvec3(10050, 0, 0), t0)) >
          length(sim::well_accel(w, 1, dvec3(10050, 0, 0), t0)));  // argmax at 10050: planet
    CHECK(length(sim::well_accel(w, 1, dvec3(10100, 0, 0), t0)) >
          length(sim::well_accel(w, 0, dvec3(10100, 0, 0), t0)));  // argmax at 10100: moon
    for (int k = 0; k < 20; ++k) {
        const double x = (k % 2 == 0) ? 10050.0 : 10100.0;
        CHECK(*tr.update(w, dvec3(x, 0, 0), t0) == 0);  // zero switches through the chatter
    }

    // Monotone walk moon-ward: exactly ONE switch, and it never flips back.
    int switches = 0;
    std::int32_t home = *tr.home();
    for (double x = 10110.0; x <= 11500.0; x += 10.0) {
        const std::int32_t h = *tr.update(w, dvec3(x, 0, 0), t0);
        if (h != home) { ++switches; home = h; }
    }
    CHECK(switches == 1);
    CHECK(home == 1);

    // Framing persists through the void (coasting keeps last home).
    CHECK(*tr.update(w, dvec3(30000, 0, 0), t0) == 1);

    // ---- moving wells: at t=54000 (quarter period) the moon sits at
    // (0,0,-12000); standing on its sub-planet point (0,0,-11600) gives
    // lunar down = -Z (toward the center below).
    CHECK(math::approximately_equal(sim::total_accel(w, dvec3(0, 0, -11600), 54000),
                                    dvec3(0, 0, -2.0), 1e-9));

    // ---- determinism: blend is pure (pos, tick) — bit-identical repeats.
    const dvec3 off(10075.5, 300.0, -120.0);
    CHECK(sim::total_accel(w, off, 12345) == sim::total_accel(w, dvec3(10075.5, 300.0, -120.0), 12345));
    // NO ship state rides in the force path: a tracker with a DIFFERENT
    // history (moon-first) never perturbs the world — total_accel is
    // stateless, so both trackers see the identical force field bitwise.
    sim::home_well_tracker tr2{0.1};
    tr2.update(w, stand, t0);
    CHECK(sim::total_accel(w, off, 12345) == sim::total_accel(w, off, 12345));
}

TEST_CASE("sim_well_blend_dominance_band") {
    // The 0.25 dominance_band DEFAULT is a pinned engine property: the
    // warp-leg scenario (8.14) never samples a ratio in the discriminating
    // window, so this case is the band's owner. At (9500,0,0) on the
    // planet->moon line: planet g = 2.3450788880540956 (u = 7500/11000
    // mid-ramp, op-order pin from the fp64 port), moon g = 1.3671875
    // (u = 0.375, exact binary), moon/planet = 0.5830 — inside (0.5, 0.75).
    // Band 0.25: moon weight = smoothstep01(clamp((0.583-0.75)/0.25)) = 0
    // EXACTLY -> planet weight 1, w_sum 1 -> total_accel BIT-IDENTICAL to
    // the planet's raw accel. Any band in (0.25, 0.583] gives the moon a
    // nonzero weight -> equality dies. 8.14's mutation of 0.25 -> 0.5 dies
    // here.
    rail_world w;
    add_moon_testbed(w);
    const dvec3 pos(9500, 0, 0);
    const dvec3 planet_raw = sim::well_accel(w, 0, pos, 0);
    CHECK(math::approximately_equal(length(planet_raw), 2.3450788880540956, 1e-12));
    CHECK(math::approximately_equal(length(sim::well_accel(w, 1, pos, 0)), 1.3671875, 1e-12));
    const dvec3 net = sim::total_accel(w, pos, 0);  // DEFAULT band
    CHECK(net == planet_raw);  // exact suppression, bit-identical
    CHECK(net.y == 0.0);
    CHECK(net.z == 0.0);
    CHECK(net.x < 0.0);  // planet at origin pulls -x
}
