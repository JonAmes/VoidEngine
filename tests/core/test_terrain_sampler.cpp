// test: terrain_sampler — the #29a/b analytic height+normal sampler, core/,
// fp64, pure function of (seed, relief params, local direction). Hand pins:
// lattice-corner heights derived by hand from the splitmix64 hash chain
// (1-octave pins are EXACT — a corner eval is one mul chain, FMA-safe),
// 3-octave pins and one gradient normal derived with an independent Python
// port of the same arithmetic (platform libm + FMA sit within ~1ulp, #40,
// so those pins carry a stated epsilon), the analytic |h| bound, seam
// continuity across the cube-face switch (the whole point of the chart),
// and purity/bit-stability.

#include <cmath>
#include <initializer_list>

#include <doctest/doctest.h>

#include "ve/core/math/base.h"
#include "ve/core/sim/terrain.h"

using namespace ve::core;
using sim::face_uv;
using sim::kMax_octaves;
using sim::terrain_def;

namespace {
// The authored test terrain: seed 42, 120 m base relief, 3 octaves,
// lacunarity 2, gain 0.5 (lattice freqs 1, 2, 4 hit EXACT lattice corners on
// the axis dirs, which is what makes the hand pins below exact).
terrain_def def() {
    terrain_def d{};
    d.seed = 42;
    d.relief_m = 120.0;
    d.base_frequency = 1.0;
    d.octaves = 3;
    d.lacunarity = 2.0;
    d.gain = 0.5;
    return d;
}

// Hand-derived via the hash chain (splitmix64(seed + x*G1 + y*G2 + z*G3),
// h>>11 / 2^53 scaled to [-1,1)) at the lattice corners (1,0,0), (0,1,0),
// (0,0,-1) for seed 42 — single-octave height = relief * corner value.
constexpr double kH1_PX = -81.62150570953918;
constexpr double kH1_PY = -66.24514906440113;
constexpr double kH1_NZ = -16.651191848448754;
// 3-octave: relief * (c0 + 0.5*c1 + 0.25*c2) at freqs 1, 2, 4 (Python float
// arithmetic, same binary64 ops).
constexpr double kH3_PX = -135.9075599665078;
constexpr double kH3_PY = -37.242810102823135;
constexpr double kH3_NZ = -44.530137912857384;
} // namespace

TEST_CASE("terrain_sampler") {
    const terrain_def d = def();

    // ---- hand-pinned heights at exact lattice corners ----
    // 1 octave: sum = corner value exactly, then one multiply => EXACT pin.
    const terrain_def d1 = [&] { terrain_def t = d; t.octaves = 1; return t; }();
    CHECK(sim::terrain_height(d1, math::dvec3(1, 0, 0)) == kH1_PX);
    CHECK(sim::terrain_height(d1, math::dvec3(0, 1, 0)) == kH1_PY);
    CHECK(sim::terrain_height(d1, math::dvec3(0, 0, -1)) == kH1_NZ);
    // 3 octaves: FMA contraction of the accumulation can move the sum by
    // ~1ulp per op (#40 platform-local contract) -> stated epsilon.
    CHECK(sim::terrain_height(d, math::dvec3(1, 0, 0)) == doctest::Approx(kH3_PX).epsilon(1e-12));
    CHECK(sim::terrain_height(d, math::dvec3(0, 1, 0)) == doctest::Approx(kH3_PY).epsilon(1e-12));
    CHECK(sim::terrain_height(d, math::dvec3(0, 0, -1)) == doctest::Approx(kH3_NZ).epsilon(1e-12));

    // ---- purity / bit-stability: same (def, dir) => same bits, always ----
    for (const auto dir : {math::dvec3(0.2, 0.9, -0.3), math::dvec3(-1, -1, -1)}) {
        const double a = sim::terrain_height(d, dir);
        const double b = sim::terrain_height(d, dir);
        CHECK(a == b);
    }
    // scale invariance: height() normalizes internally, so a scaled dir lands
    // within a few ulp (normalization of 2000/9000/-3000 rounds to slightly
    // different bits than 0.2/0.9/-0.3 — bit-exact only post-normalize).
    CHECK(sim::terrain_height(d, math::dvec3(0.2, 0.9, -0.3)) ==
          doctest::Approx(sim::terrain_height(d, math::dvec3(2000.0, 9000.0, -3000.0))).epsilon(1e-9));
    // seed actually selects the field
    {
        terrain_def e = d; e.seed = 43;
        CHECK(sim::terrain_height(e, math::dvec3(1, 0, 0)) != sim::terrain_height(d, math::dvec3(1, 0, 0)));
    }

    // ---- analytic amplitude bound: |noise| <= 1 => |h| <= relief * sum(gain^o) ----
    {
        const double bound = 120.0 * (1.0 + 0.5 + 0.25);
        for (int i = 0; i < 64; ++i) {
            const math::dvec3 dir(std::sin(0.1 * i), std::cos(0.07 * i + 1.0), std::sin(0.13 * i + 2.0));
            const double h = sim::terrain_height(d, math::normalize(dir));
            CHECK(std::fabs(h) <= bound);
        }
    }

    // ---- octave clamping: 0 -> 1, > kMax_octaves -> kMax_octaves (still bounded) ----
    {
        terrain_def lo = d; lo.octaves = 0;
        CHECK(sim::terrain_height(lo, math::dvec3(1, 2, 3)) == sim::terrain_height(d1, math::dvec3(1, 2, 3)));
        terrain_def hi = d; hi.octaves = 99; hi.base_frequency = 8.0; hi.relief_m = 100.0;
        const double bound = 100.0 * (1.0 + 0.5 + 0.25 + 0.125 + 0.0625 + 0.03125 + 0.015625 + 0.0078125);
        CHECK(std::fabs(sim::terrain_height(hi, math::dvec3(0.3, -0.6, 0.7))) <= bound);
    }

    // ---- cube-face chart (#29a) hand pins ----
    {
        const face_uv fpx = sim::cube_face(math::dvec3(1, 2, 3));  // dominant +Z
        CHECK(fpx.face == 4);                                      // 2*axis(Z=2) + 0
        CHECK(fpx.u == doctest::Approx(1.0 / 3.0).epsilon(1e-15));
        CHECK(fpx.v == doctest::Approx(2.0 / 3.0).epsilon(1e-15));
        // (u, v) are the RAW RATIOS of the non-dominant coords over
        // |dominant| — together with the face sign they reproduce the
        // projected 3D point exactly (p = dominant*+-1, u, v).
        const face_uv fny = sim::cube_face(math::dvec3(-0.1, -0.9, 0.2));  // dominant -Y
        CHECK(fny.face == 3);                                      // 2*1 + negative
        CHECK(fny.u == doctest::Approx(0.2 / 0.9).epsilon(1e-15));
        CHECK(fny.v == doctest::Approx(-0.1 / 0.9).epsilon(1e-15));
    }

    // ---- seam continuity across the X/Y face switch (the chart's whole point) ----
    // 44.9 deg vs 45.1 deg about Z in the XY diagonal band: a per-face-2D-noise
    // implementation would jump O(relief) here; the shared ambient lattice
    // moves |h| by at most the Lipschitz bound (quintic max slope 1.875 *
    // corner span 2 * freq * stretch sqrt3 * angle, per octave) ~ 8 m total.
    {
        const double a = math::deg_to_rad(44.9), b = math::deg_to_rad(45.1);
        const double ha = sim::terrain_height(d, math::dvec3(std::cos(a), std::sin(a), 0.0));
        const double hb = sim::terrain_height(d, math::dvec3(std::cos(b), std::sin(b), 0.0));
        CHECK(std::fabs(ha - hb) < 12.0);  // discontinuity would be O(100)
    }

    // ---- normal: flat relief => EXACTLY the radial direction on the axes ----
    {
        terrain_def flat = d; flat.relief_m = 0.0;
        CHECK(sim::terrain_normal(flat, 1000.0, math::dvec3(1, 0, 0)) == math::dvec3(1, 0, 0));
        CHECK(sim::terrain_normal(flat, 1000.0, math::dvec3(0, 1, 0)) == math::dvec3(0, 1, 0));
        CHECK(sim::terrain_normal(flat, 1000.0, math::dvec3(0, 0, -1)) == math::dvec3(0, 0, -1));
    }

    // ---- normal: hand pin from the independent Python port of the same
    // arithmetic. Quintic slope is zero at lattice corners, so the +X-axis
    // normal sits within ~1e-9 of radial (central-difference residuals ride
    // libm/FMA noise -> margin, not epsilon, on the tiny tilt components).
    {
        const auto n = sim::terrain_normal(d, 1000.0, math::dvec3(1, 0, 0));
        CHECK(n.x == doctest::Approx(1.0).epsilon(1e-12));
        // doctest::Approx has no margin(): the ~1e-9 tilt components are
        // checked with an explicit absolute window (residuals ride libm/FMA
        // noise on a near-zero difference, #40).
        CHECK(std::fabs(n.y - 1.7432748280415762e-09) < 1e-11);
        CHECK(std::fabs(n.z - 2.1544245516362874e-10) < 1e-11);
        // general direction where the gradient is real: full pin.
        const auto m = sim::terrain_normal(d, 1000.0, math::dvec3(0.2, 0.9, -0.3));
        CHECK(m.x == doctest::Approx(0.2972508165228597).epsilon(1e-9));
        CHECK(m.y == doctest::Approx(0.899123839564075).epsilon(1e-9));
        CHECK(m.z == doctest::Approx(-0.32127600782512294).epsilon(1e-9));
        CHECK(m != n);  // gradient actually tilts the normal
    }

    // ---- normal: unit length + determinism over a direction spread ----
    {
        for (int i = 0; i < 64; ++i) {
            const math::dvec3 dir(std::sin(0.3 * i) + 0.4, std::cos(0.11 * i) - 0.9, std::sin(0.21 * i));
            const auto n = sim::terrain_normal(d, 1000.0, dir);
            CHECK(math::approximately_equal(math::length2(n), 1.0, 1e-12));
            // outward-facing everywhere (surface can't overhang at these slopes)
            CHECK(math::dot(n, math::normalize(dir)) > 0.0);
            CHECK(n == sim::terrain_normal(d, 1000.0, dir));  // bit-stable
        }
    }

    // ---- degenerate dir pins to +Y (exception-free core) ----
    CHECK(sim::terrain_height(d, math::dvec3(0, 0, 0)) == sim::terrain_height(d, math::dvec3(0, 1, 0)));
    CHECK(sim::terrain_normal(d, 1000.0, math::dvec3(0, 0, 0)) ==
          sim::terrain_normal(d, 1000.0, math::dvec3(0, 1, 0)));

    // ---- kMax_octaves is the documented cap used by the clamp above ----
    CHECK(kMax_octaves == 8);
}
