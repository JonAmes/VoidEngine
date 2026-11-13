#pragma once
// ===========================================================================
// safe_bubble — authored per-body safe bubbles: the warp hard wall
// (#22c amended by #50, 8.13).
//
// A body's def authors a safe bubble radius (body_def::safe_bubble_radius,
// 0 = none) riding the same home-well math the wells use. Inside a bubble
// FTL does not exist:
//   * INTERIOR (in_safe_bubble): warp cannot initiate — the game-side glue
//     that drives ftl.h's engage door queries this predicate (core holds
//     no live FTL policy; see ftl.h). Exactly on the surface (d == r) is
//     NOT interior: the engage door is open at the surface.
//   * CROSSING at warp (bubble_warp_contact): any position step that
//     TOUCHES or CROSSES a bubble surface resolves AT the surface — the
//     step truncates to the TOI. While mult > 1 the wall holds every tick:
//     a step starting at the surface re-pins the position BIT-IDENTICALLY
//     at that point (#50a: the ship stops dead at the edge while the
//     forced spool-down — the 8.12 disengage door — runs; momentum carries
//     inward only at sublight). Outward crossings resolve identically
//     (#50b, symmetric). The replay/preview hooks apply the truncation;
//     glue calls bubble_warp_contact with the same predicted step to fire
//     the crossing event and (game-side) the once-per-crossing impact.
//
// Damage magnitude is game-side data (#50c): core reports the event; the
// def's authored impact value rides the game WorldDef, not the core body.
//
// Determinism contract: pure fp64 arithmetic, bodies scanned in ascending
// id, earliest contact wins, bit-equal u ties resolve to the LOWEST body
// id. The bubble centre is the rail position AT THE TICK, frozen for the
// step (same convention as the Phase-7 contact pass sampling bodies at
// tick geometry; rail-speed error is bounded and sub-ppm at sim speeds).
// The quadratic's op order is pinned below — test ports mirror it
// bit-identically (no libm transcendental here; sqrt is exact IEEE).
// ===========================================================================
#include <cstdint>
#include <optional>

#include "ve/core/sim/rails.h"

namespace ve::core::sim {

// The crossing event (#50c): what the wall reports. Glue reads tick (its
// own), body (to look up the authored impact value), and the closing
// displacement length; damage fires once per crossing at the FIRST
// contact tick.
struct bubble_contact {
    std::int32_t body = -1;
    double u = 0.0;            // fraction of this tick's warp step [0,1]
    math::dvec3 pos{};         // world point on the bubble surface
    math::dvec3 normal{};      // unit outward normal at pos
    bool inward = false;       // the step was entering the bubble interior
    double disp_len = 0.0;     // |disp| — closing distance at warp speed (damage input)
};

// Strict-interior test: is `pos` inside ANY authored bubble? Scan is
// ascending-id, first hit wins. nullopt = no bubble here = warp allowed.
std::optional<std::int32_t> in_safe_bubble(const rail_world& w, const math::dvec3& pos, tick_t t);

// First bubble-surface contact along this tick's warp step p0 -> p0 + disp.
// Pass disp with the integrator's EXACT grouping — (vel * mult) * dt — so
// classification agrees bit-identically with where the step actually
// lands. nullopt = the step never touches a wall. A tangent graze
// (disc == 0) is NOT a contact (measure-zero; deterministic rule), and a
// step beginning AND ending strictly inside is not a contact either
// (ball convexity).
std::optional<bubble_contact> bubble_warp_contact(const rail_world& w, const math::dvec3& p0,
                                                  const math::dvec3& disp, tick_t t);

}  // namespace ve::core::sim
