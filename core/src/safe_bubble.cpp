// Safe bubbles: the warp hard wall (#22c/#50, 8.13). See safe_bubble.h.
//
// PINNED QUADRATIC ORDER — the wall test asks where |rel + u*disp|^2 == r^2
// for u in [0,1], written in quarter-coefficient form
//     a*u^2 + 2*bq*u + cq = 0,   roots u = (-bq ± sqrt(bq^2 - a*cq)) / a
// with a = dot(disp,disp), bq = dot(rel,disp), cq = dot(rel,rel) - r*r.
// Every operation is one correctly-rounded IEEE64 op in this exact order;
// tests port it bit-identically. Do not "simplify" (factoring, reciprocal
// multiply, fused forms change bits — same doctrine as the 8.12 ramp pins).
#include "ve/core/sim/safe_bubble.h"

#include <cmath>

namespace ve::core::sim {

using ve::core::math::dvec3;
using ve::core::math::dot;

std::optional<std::int32_t> in_safe_bubble(const rail_world& w, const dvec3& pos, tick_t t) {
    const auto n = static_cast<std::int32_t>(w.body_count());
    for (std::int32_t id = 0; id < n; ++id) {
        const body_def* b = w.body(id);
        const double r = b ? b->safe_bubble_radius : 0.0;
        if (!(r > 0.0)) continue;  // NaN-safe: no bubble
        const dvec3 rel = pos - *w.position(id, t);  // valid ids always have a position
        if (dot(rel, rel) - r * r < 0.0) return id;  // strict; d == r is surface
    }
    return std::nullopt;
}

std::optional<bubble_contact> bubble_warp_contact(const rail_world& w, const dvec3& p0,
                                                  const dvec3& disp, tick_t t) {
    std::optional<bubble_contact> best;
    const auto n = static_cast<std::int32_t>(w.body_count());
    for (std::int32_t id = 0; id < n; ++id) {
        const body_def* b = w.body(id);
        const double r = b ? b->safe_bubble_radius : 0.0;
        if (!(r > 0.0)) continue;
        const dvec3 c = *w.position(id, t);
        const dvec3 rel = p0 - c;
        const double a = dot(disp, disp);
        if (a == 0.0) continue;  // zero-length step cannot touch the wall
        const double bq = dot(rel, disp);
        const double cq = dot(rel, rel) - r * r;   // f(0): < 0 strictly inside
        const dvec3 e = rel + disp;
        const double fr = dot(e, e) - r * r;       // f(1) at the step endpoint
        // SURFACE BAND (classification only — roots still use the raw
        // coeffs): the truncation TOI is fp-evaluated, so a pinned ship's
        // next-tick f(0) is ulp noise around 0, NOT 0. Without the band an
        // inside-by-1ulp residue takes the exit-root branch and pins the
        // ship at the FAR side of the sphere (a 2r jump). The band is a
        // fixed constant — 2^-40 * r^2 sits ~2^12 above worst-case f
        // evaluation noise and ~2^12 below any physically meaningful
        // clearance — so classification stays deterministic.
        const double band = r * r * 0x1p-40;
        const double f0 = (std::fabs(cq) <= band) ? 0.0 : cq;
        const double f1 = (std::fabs(fr) <= band) ? 0.0 : fr;

        double u = 0.0;
        bool inward = false;
        if (f0 == 0.0) {
            // Step STARTS on (or fp-on) the surface: the wall is exactly
            // here — pin (bit-identical) whenever the step does not merely
            // skate the surface (#50a/b). f1 == 0 too = touch-to-touch
            // degeneracy: no contact event (next tick re-evaluates).
            if (f1 == 0.0) continue;
            u = 0.0;
            inward = f1 < 0.0;
        } else if (f0 < 0.0 && f1 < 0.0) {
            continue;  // strictly inside -> strictly inside: ball is convex
        } else if (f0 < 0.0) {
            // Inside -> out: the exit root is the wall (#50b outward leg).
            // -a*cq > 0 makes disc > 0 exactly (raw cq: the band only
            // classifies, never perturbs the roots).
            const double disc = bq * bq - a * cq;
            const double u2 = (-bq + std::sqrt(disc)) / a;
            if (!(u2 > 0.0) || u2 > 1.0) continue;  // fp slip: band path catches next tick
            u = u2;
            inward = false;
        } else {  // f0 > 0: start outside — the entry root is the wall.
            const double disc = bq * bq - a * cq;
            if (disc <= 0.0) continue;  // miss, or exact tangent: NOT a contact
            const double u1 = (-bq - std::sqrt(disc)) / a;
            if (!(u1 > 0.0) || u1 > 1.0) continue;
            u = u1;
            inward = u < 1.0;  // u == 1: the step lands exactly ON the surface (touch)
        }

        bubble_contact bc;
        bc.body = id;
        bc.u = u;
        if (u == 0.0) {
            // Surface pin must be BIT-IDENTICAL to the pre-step position
            // (#50a: the wall holds forever, so any c+(p0-c) drift would
            // jitter the ship every tick). Never round-trip through the
            // centre: keep p0 verbatim.
            bc.pos = p0;
            bc.normal = rel * (1.0 / std::sqrt(dot(rel, rel)));
        } else {
            const dvec3 wv = rel + disp * u;
            bc.pos = c + wv;
            bc.normal = wv * (1.0 / std::sqrt(dot(wv, wv)));
        }
        bc.inward = inward;
        bc.disp_len = std::sqrt(a);
        if (!best || u < best->u) best = bc;  // earliest; bit-equal u ties keep the lower id
    }
    return best;
}

}  // namespace ve::core::sim
