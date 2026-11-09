#pragma once
// Phase 5 task 5.2 — mass properties from the loadout's parts (#18/#26a
// payoff, decision #45b: full tensor now that the loadout provides real
// offsets). Derive total mass, center of mass, and the FULL 3x3 inertia
// tensor about the CoM by the parallel-axis theorem; Jacobi diagonalization
// yields principal axes + principal inertias.
//
// Determinism: fixed sweep order + fixed iteration count in the Jacobi
// solver (no data-dependent convergence tests beyond a fixed threshold —
// the same input always produces bitwise the same output; pinned by test).
// No game concepts here: "part" is just (mass, position, local tensor).

#include <expected>
#include <span>

#include "ve/core/math/mat3.h"
#include "ve/core/math/vec.h"

namespace ve::core::sim {

// One contributor to mass properties: mass at a hull-local position with an
// optional tensor about its OWN center of mass (zero = point mass — good
// enough for small modules; the hull itself carries its authored tensor).
struct mass_point {
    double mass = 0.0;
    math::dvec3 pos{};
    math::dmat3 local_tensor{};  // zero matrix = point mass
};

struct ship_inertia {
    double total_mass = 0.0;
    math::dvec3 com_local{};
    math::dmat3 tensor{};  // about com_local, hull-local axes (symmetric)
};

enum class inertia_error { non_positive_total_mass };

// Hull body + mounted parts -> ship mass properties. The hull enters as a
// mass_point too (its authored tensor about ITS com), so one uniform path.
// Rejects total mass <= 0 (a loadout of nothing but massless structure is
// not flammable physics — it cannot be simulated).
std::expected<ship_inertia, inertia_error> inertia_from_parts(
    std::span<const mass_point> parts);

// Principal axes of a symmetric tensor: `axes` columns are eigenvectors
// (right-handed after sign normalization), `principal` the eigenvalues
// sorted ASCENDING (deterministic order). Standard cyclic Jacobi: fixed
// 12 sweeps max (3 pair rotations each), fixed rotation selection —
// bitwise reproducible for identical input.
struct principal_frame {
    math::dmat3 axes{};
    math::dvec3 principal{};
};
principal_frame principal_axes(const math::dmat3& sym);

}  // namespace ve::core::sim
