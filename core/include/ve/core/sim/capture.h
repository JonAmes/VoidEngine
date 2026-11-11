#pragma once

// Capture zones + snap-to-rest (#29, #41; checklist 7.9): a landing pad is a
// hand-authored POI on a (possibly spinning, #41) body. A hull inside the
// capture radius, moving SLOW RELATIVE TO THE PAD (#41 — tolerances are
// pad-relative, not world-rest), engages at a fixed tick; snap-to-rest
// drives pose/velocity to pad-relative rest. #29e: NOTHING fails here —
// a too-hard or too-fast pass simply never engages (arcade).
//
// The engagement test is the swept closest approach of the hull's linear
// path over one fixed tick to the pad point's linear path over the same
// tick (the #45e "swept engagement at a fixed tick" shape; the sphere is
// the capture zone, no solid to enter). Same pure-fp64 discipline as the
// rest of sim: no stored state, evaluated from rail poses at the tick, so
// replay hashes ride it for free.
//
// Snap-to-rest is one algebraic re-aim (the arcade "alignment assist"):
// gear contact point pinned at authored rest depth under the pad surface
// (matching the 7.8 penalty squat depth), velocity set to the pad point's
// velocity (rides spin), hull -Y aimed at the pad normal by the minimal
// rotation from the current attitude. No smoothing, no ramp — a deterministic
// pose projection, not a control loop.

#include <optional>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/swept.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// Authored pad (hand-written this phase; world cooking is Phase 8).
struct pad_def {
    std::int32_t body = -1;         // pad host body
    math::dvec3 local_dir{0.0, 0.0, 0.0};  // body-local unit direction of
                                           // the pad centre from body centre
    double capture_radius_m = 0.0;  // engagement sphere radius
    double safe_speed_m_s = 0.0;    // max RELATIVE speed to engage (#29d)
    double rest_depth_m = 0.0;      // gear contact point rests this far
                                    // under the pad surface at rest
};

// Pad world snapshot at one tick (pure function of the rails).
struct pad_world {
    math::dvec3 pos{};         // pad surface point, world
    math::dvec3 vel{};         // pad point velocity (spin rides, #41)
    math::dvec3 normal{0.0, 0.0, 0.0};  // surface normal at the pad centre
                                        // (= local_dir rotated to world)
};

// Pad snapshot at tick t. nullopt on bad id / non-unit local_dir / garbage
// rails (core is exception-free).
std::optional<pad_world> pad_at(const rail_world& world, const pad_def& pad,
                                tick_t t);

// Swept engagement over one fixed tick starting at tick t: closest
// approach of p(tau) = pos + vel*tau to q(tau) = pad_pos + pad_vel*tau,
// tau in [0, kSim_tick_dt]. Engages iff the minimum distance is within
// capture_radius AND the relative speed is within safe_speed for the
// whole tick (parallel-motion passes use their constant distance).
// nullopt on non-finite inputs or bad pad numbers (radius/safe/depth < 0);
// NEVER mutates ship — the caller only applies snap on a hit.
// Reuses swept_hit: pos = hull pos at the closest-approach instant,
// normal = zone-surface normal pointing back at the hull (pad -> hull),
// t = seconds into the tick — NOT a solid-surface hit (the sphere is the
// zone).
// `zone` carries the authored radius/safe-speed; `pad` the tick snapshot.
std::optional<swept_hit> capture_engage(const pad_def& zone,
                                        const pad_world& pad,
                                        math::dvec3 ship_pos,
                                        math::dvec3 ship_vel, tick_t t);

// The snap-to-rest pose at tick t (see header note; one algebraic
// projection). Returns the new rigid_state: gear contact point at
// pad.pos - rest_depth * normal, vel = pad.vel, hull local -Y aligned to
// the pad normal by the minimal rotation from ship.att.
rigid_state snap_to_rest(const rigid_state& ship, const pad_world& pad,
                         const pad_def& def);

} // namespace ve::core::sim
