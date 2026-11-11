#pragma once

// Docking = the SAME capture-zone machinery as 7.9 landing pads, aimed at a
// station PORT instead of a moon pad (#23a/b, checklist 7.10). A port is a
// dock-port component bolted on a hull socket (#47b): its world pose derives
// from the host hull's rigid pose (port_local from the socket), so a docked
// ship rides every port motion for free — stations pinned to a rail,
// spinning, or orbiting (#41: tolerances stay port-relative).
//
//   - capture_engage (7.9) is reused verbatim: authored radius +
//     safe_speed, swept closest approach at the pinned tick. Nothing fails
//     — a too-fast pass never engages (#29e).
//   - On engage, snap_to_dock is the alignment assist: one algebraic
//     projection putting the ship's dock-gear point on the port axis at
//     rest depth, ship local -Y onto the port normal, velocity to the
//     port point's velocity.
//   - While docked, the rigid bind is an exact pose projection per tick
//     (docked_pose): no spring, no solver — a hard clamp, which is exactly
//     why a released push-off (release_impulse) is the departure path.
//   - A destroyed clamp NEVER engages: the caller skips the probe (the
//     Phase 6 dark-not-dead idiom; the sim layer stays stateless).
//   - Authored `clamp_strength_N` gates whether a clamp exists at all
//     (callers refuse ports with strength <= 0); the 7.10 sim layer keeps
//     strength as authored data — strain overload against the docked pair's
//     motion is the dark-not-dead repair loop's story (7.11+), not a
//     second solver here.
//
// Same discipline as capture.h: pure fp64 functions of (rails/pose, tick),
// no stored state, so replay hashes ride every engagement/release event.

#include <optional>

#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Authored dock port (a `dock.port` component on a hull socket, #47b).
// The hull is any sim entity: a player ship, a station (decision #24a:
// a station is just a big hull pinned to a rail).
struct dock_port_def {
    std::int32_t body = -1;            // host hull's rail body
    math::dvec3 local_pos{};           // port point, host hull-local (the
                                       // socket's position_local rides here)
    math::dvec3 local_normal{0.0, 0.0, 1.0};  // approach axis, host-local:
                                              // the ship docks from the
                                              // +normal side, gear facing in
    double capture_radius_m = 0.0;     // engagement sphere (as pads)
    double safe_speed_m_s = 0.0;       // max port-RELATIVE speed (#41)
    double rest_depth_m = 0.0;         // gear point rests this far inside
                                       // the port plane along the normal
    double clamp_strength_N = 0.0;     // authored gate: <= 0 = no clamp;
                                       // destroyed components never probe
};

// Port world snapshot at one tick, from the host's CURRENT rigid pose
// (stations ride rails AND their own spin; a docked pair's host may itself
// be free-flying). nullopt on non-unit normal / non-finite inputs.
struct port_world {
    math::dvec3 pos{};               // port point, world
    math::dvec3 vel{};               // port point velocity (spin term:
                                     // vel + omega x r)
    math::dvec3 normal{0.0, 0.0, 0.0};  // approach axis, world
};
std::optional<port_world> port_at(const dock_port_def& port,
                                  const rigid_state& host);

// RAIL-pinned variant (stations are "big inert hull pinned to a rail",
// decision #24a): port pose from rail position + spin orientation at tick t.
std::optional<port_world> port_on_rail(const rail_world& world,
                                       const dock_port_def& port, tick_t t);

// The ship's dock-gear point: hull-local offset of the clamp gear (its
// dock.port component's socket on the SHIP side; a ship docks gear-to-port).
// Docked pose pins THIS point at port.pos - rest_depth * normal.
struct dock_gear_def {
    math::dvec3 local_pos{};         // gear point, ship hull-local
};

// Alignment assist on engage: one algebraic projection (as snap_to_rest —
// no ramp, deterministic pose projection). Gear point lands on the port
// axis at rest_depth inside the port plane; ship local -Y onto the port
// normal by minimal rotation; vel = port.vel; omega zeroed.
rigid_state snap_to_dock(const rigid_state& ship, const port_world& port,
                         const dock_port_def& port_def,
                         const dock_gear_def& gear);

// While docked: the ship's EXACT pose per tick (the rigid bind). World
// attitude stays the engage attitude (a docked pair is ONE rigid body —
// the host can't re-rotate the ship alone); the gear point is re-pinned at
// rest depth on the CURRENT port axis each tick, so any host drift, orbit
// or spin shows up as a pure translation carrying the pair along — docked
// pose rides port motion. clamp_att (ship-relative-to-host, captured once
// at engage) is kept in the signature so tools can rebuild the bind from
// authored data without stored state; the projection itself needs only
// the ship's own attitude.
rigid_state docked_pose(const rigid_state& ship_hull_hint,
                        const port_world& port, const dock_port_def& port_def,
                        const dock_gear_def& gear, math::dquat clamp_att);

// Undock push-off (#23b release): an impulse J along the port normal
// (+ authored tangential nudge) applied at the gear point. Impulses over
// mass ride straight into the velocity (the sim has no stored clamp to
// solve against — the pose bind simply stops being applied, next tick the
// ship is a free rigid body carrying the kick). Deterministic by
// construction: impulse magnitude is authored/sim-side, not solved.
rigid_state release_impulse(const rigid_state& docked,
                            const port_world& port,
                            const dock_gear_def& gear,
                            const rigid_params& params,
                            double normal_impulse_N_s,
                            math::dvec3 tangent_impulse_N_s = {0.0, 0.0, 0.0});

} // namespace ve::core::sim
