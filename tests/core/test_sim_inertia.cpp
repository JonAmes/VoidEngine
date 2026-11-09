// Phase 5 task 5.2 — full-tensor inertia (sim_inertia): parallel-axis
// assembly against hand-computed numbers, Jacobi principal axes against
// construction, mat3 basics (inverse, transpose, determinant).
//
// Cluster used throughout (hull-local positions):
//   m0 = 100 at (0,0,0)   m1 = 10 at (2,0,0)   m2 = 20 at (0,1,0)   m3 = 5 at (0,0,-4)
// total  = 135
// com    = (20, 20, -20)/135 = (4/27, 4/27, -4/27)
// Ixx = [100*(2*16/729) + 10*(32/729) + 20*(545/729) + 5*(10832/729)] = 68580/729
// Iyy = [100*(32/729) + 10*(2516/729) + 20*(32/729) + 5*(10832/729)]  = 83160/729
// Izz = [100*(32/729) + 10*(2516/729) + 20*(545/729) + 5*(32/729)]    = 39420/729
// Ixy = 2160/729  Ixz = -2160/729  Iyz = -2160/729   (=-sum m dx dy etc)
#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include "ve/core/sim/inertia.h"

namespace {

using namespace ve::core;
using math::dmat3;
using math::dvec3;
using sim::inertia_error;
using sim::inertia_from_parts;
using sim::mass_point;
using sim::principal_axes;

std::array<mass_point, 4> cluster() {
    // NB: dmat3{} is IDENTITY (Phase-1 convention, like mat4) — point
    // masses must pass dmat3::zero() or omit the member.
    return {
        mass_point{100.0, dvec3(0, 0, 0), dmat3::zero()},
        mass_point{10.0, dvec3(2, 0, 0), dmat3::zero()},
        mass_point{20.0, dvec3(0, 1, 0), dmat3::zero()},
        mass_point{5.0, dvec3(0, 0, -4), dmat3::zero()},
    };
}

constexpr double kInv729 = 1.0 / 729.0;

}  // namespace

TEST_CASE("sim_inertia") {
    auto r = inertia_from_parts(cluster());
    REQUIRE(r.has_value());

    // Total mass and CoM (hand-computed above).
    CHECK(r->total_mass == doctest::Approx(135.0));
    CHECK(r->com_local.x == doctest::Approx(4.0 / 27.0));
    CHECK(r->com_local.y == doctest::Approx(4.0 / 27.0));
    CHECK(r->com_local.z == doctest::Approx(-4.0 / 27.0));

    // Full tensor, column-major: m[c*3+r]. Diagonals.
    CHECK(r->tensor.m[0] == doctest::Approx(68580.0 * kInv729));   // Ixx
    CHECK(r->tensor.m[4] == doctest::Approx(83160.0 * kInv729));   // Iyy
    CHECK(r->tensor.m[8] == doctest::Approx(39420.0 * kInv729));   // Izz
    // Off-diagonals: tensor.m[c*3+r] = I_{rc}.
    CHECK(r->tensor.m[1] == doctest::Approx(2160.0 * kInv729));    // I_xy (r=x,c=y)
    CHECK(r->tensor.m[3] == doctest::Approx(2160.0 * kInv729));    // I_yx symmetric
    CHECK(r->tensor.m[2] == doctest::Approx(-2160.0 * kInv729));   // I_xz
    CHECK(r->tensor.m[6] == doctest::Approx(-2160.0 * kInv729));   // I_zx
    CHECK(r->tensor.m[5] == doctest::Approx(-2160.0 * kInv729));   // I_yz
    CHECK(r->tensor.m[7] == doctest::Approx(-2160.0 * kInv729));   // I_zy

    // Degenerate input rejected: massless structure only, and empty.
    const std::array<mass_point, 1> massless{mass_point{0.0, dvec3(1, 2, 3), dmat3::zero()}};
    CHECK_FALSE(inertia_from_parts(massless).has_value());
    CHECK(inertia_from_parts({}).error() == inertia_error::non_positive_total_mass);

    // Hull's own authored tensor rides along: add diag(10,20,30) to the
    // m0 point's local tensor -> diagonal entries shift by exactly that.
    auto with_hull_tensor = cluster();
    with_hull_tensor[0].local_tensor = dmat3::diag(dvec3(10, 20, 30));
    auto r2 = inertia_from_parts(with_hull_tensor);
    REQUIRE(r2.has_value());
    CHECK(r2->tensor.m[0] == doctest::Approx(68580.0 * kInv729 + 10.0));
    CHECK(r2->tensor.m[4] == doctest::Approx(83160.0 * kInv729 + 20.0));
    CHECK(r2->tensor.m[8] == doctest::Approx(39420.0 * kInv729 + 30.0));
    CHECK(r2->tensor.m[1] == r->tensor.m[1]);  // off-diags untouched

    // CoM shift: translating every part by the SAME vector moves the CoM by
    // it; the tensor is translation-invariant in EXACT arithmetic (parallel
    // axis uses only offsets from the CoM) but not bitwise — fp rounding of
    // com/d differs per origin, so compare with tight Approx.
    auto shifted = cluster();
    for (auto& p : shifted) p.pos = p.pos + dvec3(-7.5, 2.25, 100.0);
    auto r3 = inertia_from_parts(shifted);
    REQUIRE(r3.has_value());
    CHECK(r3->com_local.x == doctest::Approx(4.0 / 27.0 - 7.5));
    CHECK(r3->com_local.z == doctest::Approx(-4.0 / 27.0 + 100.0));
    for (int i = 0; i < 9; ++i)
        CHECK(std::fabs(r3->tensor.m[i] - r->tensor.m[i]) < 1e-9);

    // ---- principal axes (Jacobi) against construction ----
    // Hand tensor: off-diag 20, diag (100, 80, 50) — known to have one
    // eigenvector at 45 deg in xy: [x,y,z] with x=y solves the 2x2 block.
    dmat3 sym{{100, 20, 0,
               20, 80, 0,
               0,  0, 50}};
    auto pf = principal_axes(sym);
    // Ascending principal values: 2x2 block eigenvalues = 90 +/- sqrt(100+400)
    // = 90 +/- 22.360679...; third = 50.
    const double s = std::sqrt(500.0);
    // ascending: 50 sits BELOW 90-sqrt(500)=67.64 — axis 0 IS the z axis.
    CHECK(pf.principal.x == doctest::Approx(50.0));
    CHECK(pf.principal.y == doctest::Approx(90.0 - s));
    CHECK(pf.principal.z == doctest::Approx(90.0 + s));
    // Reconstruction axes * diag * axes^T reproduces the input (rotation of
    // a symmetric tensor preserves it).
    const dmat3 rec = pf.axes * dmat3::diag(pf.principal) * transpose(pf.axes);
    for (int i = 0; i < 9; ++i)
        CHECK(std::fabs(rec.m[i] - sym.m[i]) < 1e-8);
    // Orthonormal right-handed axes.
    const dmat3 i3 = pf.axes * transpose(pf.axes);
    CHECK(std::fabs(i3.m[0] - 1.0) < 1e-12);
    CHECK(std::fabs(i3.m[4] - 1.0) < 1e-12);
    CHECK(std::fabs(i3.m[8] - 1.0) < 1e-12);
    // axis 0 (lambda=50) is the z axis: its x,y components vanish; axes 1,2
    // are the xy-block pair: their z components vanish. (m[c*3+r], z = r=2)
    CHECK(std::fabs(pf.axes.m[0]) < 1e-12);  // axis0 x
    CHECK(std::fabs(pf.axes.m[1]) < 1e-12);  // axis0 y
    CHECK(std::fabs(pf.axes.m[5]) < 1e-12);  // axis1 z
    CHECK(std::fabs(pf.axes.m[8]) < 1e-12);  // axis2 z
    CHECK(determinant(pf.axes) == doctest::Approx(1.0).epsilon(1e-9));

    // Determinism: same input twice -> bitwise identical output.
    auto pf2 = principal_axes(r->tensor);
    for (int i = 0; i < 9; ++i) CHECK(pf2.axes.m[i] == principal_axes(r->tensor).axes.m[i]);
    // Trace preserved by diagonalization.
    const double tr = r->tensor.m[0] + r->tensor.m[4] + r->tensor.m[8];
    CHECK(pf2.principal.x + pf2.principal.y + pf2.principal.z ==
          doctest::Approx(tr).epsilon(1e-12));

    // ---- mat3 basics ----
    dmat3 t{{2, 1, 0,
             0, 3, 1,
             1, 0, 4}};  // column-major
    // c0=(2,1,0), c1=(0,3,1), c2=(1,0,4): det = c0.(c1 x c2) = (2,1,0).(12,1,-3) = 25.
    CHECK(determinant(t) == doctest::Approx(25.0));
    const dmat3 id = t * inverse(t);
    for (int i = 0; i < 9; ++i) {
        const double want = (i % 4 == 0) ? 1.0 : 0.0;
        CHECK(id.m[i] == doctest::Approx(want).epsilon(1e-12));
    }
    // transpose * vector identity: dot(u, M v) == dot(M^T u, v)
    const dvec3 u(1.5, -2.0, 0.25), v(0.1, 7.0, -3.0);
    CHECK(dot(u, t * v) == doctest::Approx(dot(transpose(t) * u, v)).epsilon(1e-12));
}
