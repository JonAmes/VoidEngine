#pragma once
// core/sim rigid body + flight integrator (Phase 4 / decision #45): the one
// integrated state in the sim. Pos/vel/attitude/omega are fp64 (#45b —
// dquat carries sim attitude; fp32 rigid_frame stays the render/stats
// width). Attitude convention (decision #34): att maps BODY->WORLD;
// omega_body is BODY-frame angular velocity, integrated by
// quat::integrate_angular (right-multiplied local spin, pinned in 4.2).
//
// Integrator is SEMI-IMPLICIT EULER at the canonical fixed tick (dt is
// kSim_tick_dt, never a caller-chosen float — decision #45a): vel-first,
// then pos with the NEW velocity. The LINEAR part is plain semi-implicit
// (that is where the flight feel lives); the ANGULAR part uses explicit
// midpoint (RK2) because forward-Euler on the free top is exponentially
// unstable at ANY dt (see rigid.cpp). Deterministic: pure function of
// (state, per-tick inputs); ordered iteration is the caller's job (#5).
// The known semi-implicit bias (+½·a·t·dt on position under constant
// accel) is pinned by tests, not hidden — arcade feel tuning happens on
// this exact curve.
//
// Angular part is Euler's equations in the BODY frame with DIAGONAL
// principal-axis inertia (#45b: full tensor is Phase 5):
//   I·dω/dt = τ − ω × (I∘ω)     (∘ = componentwise)
// which is exact-per-component for diagonal I; the ω×(I∘ω) gyroscopic
// term vanishes when ω rides a principal axis.

#include <optional>

#include "ve/core/math/mat3.h"
#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Authored by hand in Phase 4 tests (#45d). Units SI: kg, kg·m^2.
// 5.3 (decision #45b): when the body's FULL inertia tensor is known (the
// 5.2 ship_inertia, expressed in BODY/hull-local axes), set inertia_tensor.
// It then REPLACES the diagonal path entirely; when absent, the Phase-4
// diagonal arithmetic runs bitwise-unchanged (all Phase-4 replay-hash pins
// double as the identity proof for that path).
struct rigid_params {
    double mass = 1.0;
    math::dvec3 inertia_diag{1.0, 1.0, 1.0};
    std::optional<math::dmat3> inertia_tensor = std::nullopt;
};

struct rigid_state {
    math::dvec3 pos{};                    // world
    math::dvec3 vel{};                    // world, m/s
    math::dquat att{};                    // body -> world
    math::dvec3 omega_body{};             // body-frame angular velocity, rad/s
};

// True when every field is finite with mass/inertia > 0 — tools/tests gate
// on it; integrate_step itself stays branch-light (NaN leaks instead of
// hiding bad input).
bool params_ok(const rigid_params& p);  // tensor (when set) must be finite + positive definite
bool state_ok(const rigid_state& s);

// Advance exactly ONE sim tick (kSim_tick_dt). accel_world: net linear
// acceleration already summed by the caller (thrust + total_accel wells) —
// mass already divided out. torque_body: raw body-frame torque in N·m;
// the integrator divides by inertia_diag per axis (Euler's equations).
void integrate_step(rigid_state& s, const rigid_params& p, const math::dvec3& accel_world,
                    const math::dvec3& torque_body);

// Variable-dt core of integrate_step. SANCTIONED USE ONLY: the 4.10 ghost
// preview, which rolls the SAME integrator at reduced substep rate with
// dt = step_divisor*kSim_tick_dt (a rational multiple of the canonical
// tick, computed — never a free caller-chosen float, #45a). Sim code must
// always go through integrate_step.
void integrate_step_dt(rigid_state& s, const rigid_params& p, const math::dvec3& accel_world,
                       const math::dvec3& torque_body, double dt);

} // namespace ve::core::sim
