#pragma once
// core/sim kinematic rails (decision #17a, #45c): every body's position is
// an ANALYTIC function of the integer sim tick — no stored state, no
// integration, evaluable at any tick (replay scrubber, ghost sim, time
// compression), zero drift, deterministic by construction. The gravity well
// (#4.4) rides with the body. Rails are circular and coplanar: the local
// offset spins in the parent's XZ plane, mathematically positive about +Y
// (#34), so rail motion agrees with the pinned quat_yaw rotation sense.
// Eccentricity/inclination are later *data*, not code (#45c).

#include <cstdint>
#include <optional>
#include <vector>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// All lengths SI (m) and rates SI (s, rad) — #42 puts human units in defs,
// the runtime only ever sees cooked/hand-authored SI.
struct body_def {
    double surface_radius = 0.0;   // m, > 0
    // Well parameters (consumed by the field math in 4.4; they ride here):
    double well_radius = 0.0;      // m; 0 = no well, else > surface_radius
    double surface_g = 0.0;        // m/s^2 inside the well (constant near surface)
    double well_taper_start = 0.0; // m; field ramps to 0 over [taper_start, well_radius]
    // Atmosphere (#17, 7.6): hand-authored SI this phase (#45d; world-def
    // cooking is Phase 8). rho0 == 0 authors NO atmosphere (airless moons).
    // Density rides rho0*exp(-h/H) with h radial altitude above
    // surface_radius; aero only applies inside well_radius (#17).
    double atm_rho0_kg_m3 = 0.0;    // sea-level density, >= 0
    double atm_scale_height_m = 0.0; // > 0 whenever rho0 > 0
    // Safe bubble (#22c amended by #50, 8.13): authored warp hard-wall
    // radius about this body, m; 0 = no bubble. When > 0 it must reach
    // >= max(surface_radius, well_radius) — the wall sits OUTSIDE the well
    // and the terrain (add_body gate; cook gate in wrldvalidate). The
    // wall's crossing math lives in safe_bubble.h; safe_speed is the
    // authored sublight ceiling inside the bubble, consumed by game
    // assist/autopilot glue (8.14+) — it rides here like the atm params.
    // The impact damage VALUE is game-side data, never core.
    double safe_bubble_radius = 0.0;
    double safe_speed = 0.0;         // m/s; > 0 whenever a bubble exists
    // Kinematic rail (#17a). parent = -1 pins the body to the static root
    // anchor (world origin); otherwise a circular orbit about the parent.
    std::int32_t parent = -1;
    double orbit_period_s = 0.0;    // > 0 when parent >= 0
    double orbit_radius_m = 0.0;    // >= 0
    double orbit_phase_rad = 0.0;   // angle at tick 0, positive about +Y (#34)
    // Body spin (#41): angular velocity about the body's own center. The
    // axis is authored in the PARENT rail frame and defaults to local +Y
    // (coplanar rails #45c keep spin and orbit sharing the +Y sense until
    // eccentric rails exist); rate is rad/s — cooked/hand-authored defs are
    // SI only (#42: human °/day stays in TOML). Orientation joins position
    // as an ANALYTIC function of the tick: no stored state, evaluable at
    // any tick. add_body stores the axis normalized (normalize ONCE at add,
    // never per call, so evals stay bit-stable).
    math::dvec3 spin_axis{0.0, 1.0, 0.0};
    double spin_rate_rad_s = 0.0;
};

// Ordered world (#5): body id == index into the body vector, and a body may
// only reference a parent added BEFORE it (parent < id). add_body enforces
// that, which makes parent cycles impossible by construction and lookup
// O(chain depth). Deterministic iteration = vector order.
class rail_world {
public:
    // Validates and appends; returns the new body id, or nullopt (and
    // changes nothing) on invalid input — core is exception-free.
    std::optional<std::int32_t> add_body(body_def def);

    std::size_t body_count() const { return m_bodies.size(); }
    const body_def* body(std::int32_t id) const;

    // Local orbit offset in the parent's frame at tick t (zero for a root
    // body). Pure: same inputs => bit-identical dvec3.
    static math::dvec3 local_rail_offset(const body_def& body, tick_t t);

    // World position at tick t: local offsets summed up the parent chain
    // (moon rides its planet). nullopt on bad id. Stores nothing — calling
    // it never advances or perturbs anything.
    std::optional<math::dvec3> position(std::int32_t id, tick_t t) const;

    // Rail velocity at tick t: central-difference of the analytic position
    // (exact for the static root; chord-vs-tangent error ~ (2*pi*dt/P)^2/24,
    // ~1e-9 rel for authored periods >= 60 s). Deterministic by construction.
    std::optional<math::dvec3> velocity(std::int32_t id, tick_t t) const;

    // The body's OWN spin rotation at tick t, expressed in the parent rail
    // frame (identity when the rate is 0). Pure: same inputs => bit-identical
    // dquat. Angle is authored-rate * elapsed seconds, pre-wrapping-free
    // (same bit-identity discipline as local_rail_offset).
    static math::dquat local_spin_orient(const body_def& body, tick_t t);

    // World attitude of the body frame at tick t: spin composed up the
    // parent chain, orientation(child) = orientation(parent) * local_spin —
    // so a surface coordinate rides the spin and a moon inherits its
    // planet's rotation (#41). position()/velocity() read NONE of this:
    // spin moves surface points, never rail centers — the gravity well and
    // safe bubbles stay position-only ("centers move, normals don't").
    // nullopt on bad id; stores nothing.
    std::optional<math::dquat> orientation(std::int32_t id, tick_t t) const;

private:
    std::vector<body_def> m_bodies;
};

} // namespace ve::core::sim
