// Phase 9 task 9.10 — firing cycle laws (contract in gun.h).
#include "ve/core/sim/gun.h"
#include "ve/core/sim/occupancy.h"

#include <cmath>
#include <utility>

#include "ve/core/math/base.h"
#include "ve/core/rng.h"
#include "ve/core/sim/bands.h"

namespace ve::core::sim {

void gun_configure(gun_slot& gun, StatsPipeline& pipe, gun_attrs attrs) {
    gun.pipe = &pipe;
    gun.attrs = std::move(attrs);

    // Re-arm the reservoir with its band set (threshold read once from the
    // live pipeline at configure; authoring-time value). Moving in a fresh
    // Reservoir keeps the current heat and the &gun.tags wiring; nothing
    // else holds the old object.
    std::vector<reservoir_band> bands;
    if (!gun.attrs.heat_limit.empty()) {
        if (const auto cap = pipe.derived(gun.attrs.heat_limit);
            cap && std::isfinite(*cap) && *cap > 0.0f) {
            bands.push_back({static_cast<double>(*cap), gun.attrs.overheat_tag,
                             band_dir::at_or_above});
        }
    }
    gun.heat = Reservoir(gun.heat.value(), std::move(bands), &gun.tags);
    gun.heat.bind(&pipe, {/*supply=*/{}, /*load=*/{}, gun.attrs.heat_leak});
}

void gun_tick(gun_slot& gun) {
    gun.heat.step();  // leak attr * kSim_dt (+ hysteresis-free band re-eval)
}

std::optional<fire_result> gun_try_fire(gun_slot& gun, math::dvec3 aim,
                                        tick_t now, std::uint64_t world_seed,
                                        const Occupancy* occ) {
    if (!gun.pipe)  // unconfigured gun has no laws to consult
        return std::nullopt;
    for (const double c : {aim.x, aim.y, aim.z})
        if (!std::isfinite(c))
            return std::nullopt;
    if (!math::approximately_equal(math::length2(aim), 1.0, 1e-6))  // unit-aim law (9.7)
        return std::nullopt;

    // --- crew gate (9.19, #18a): a crew-gated turret is silent unless its
    // seat is manned AND not dark (dark seat or KO'd/dropped occupant both
    // read false). Fail-safe: gated gun + no occupancy view = refuse.
    // Placed before ANY gate that could consume state: refusal consumes
    // nothing, so manning never shifts the spread stream (shot count key).
    if (!gun.gunner_seat.empty() &&
        !(occ && occ->function_available(gun.gunner_seat)))
        return std::nullopt;

    // --- cooldown gate: live rof, fail-SAFE when absent (an act, not a flow)
    const auto rof = gun.pipe->derived(gun.attrs.rof);
    if (!rof || !std::isfinite(*rof) || *rof <= 0.0f)
        return std::nullopt;
    const auto interval = std::llround(kSim_tick_hz / static_cast<double>(*rof));
    if (interval >= 1 && gun.last_shot >= 0 && now - gun.last_shot < interval)
        return std::nullopt;

    // --- overheat gate: same live condition the band tags on (checked
    // directly so a mid-tick set_value already visible here refuses too)
    if (!gun.attrs.heat_limit.empty()) {
        if (const auto cap = gun.pipe->derived(gun.attrs.heat_limit);
            cap && std::isfinite(*cap) && *cap > 0.0f &&
            gun.heat.value() >= static_cast<double>(*cap))
            return std::nullopt;
    }

    // --- spread stream: keyed (seed, component, SHOT count) — refusals
    // consumed nothing above, so shot numbering is the stream position
    math::dvec3 fired = aim;
    if (!gun.attrs.spread.empty()) {
        if (const auto sp = gun.pipe->derived(gun.attrs.spread);
            sp && std::isfinite(*sp) && *sp > 0.0f) {
            std::uint64_t h = splitmix64(world_seed ^
                                         (static_cast<std::uint64_t>(gun.component) * 0x9E3779B97F4A7C15ULL));
            h = splitmix64(h ^ (gun.shots * 0xBF58476D1CE4E5B9ULL + 0x94D049BB133111EBULL));
            const double u1 = unit01(splitmix64(h));
            const double u2 = unit01(splitmix64(h + 0x9E3779B97F4A7C15ULL));

            // Uniform direction in the cone of half-angle sp around aim:
            // cos(theta) uniform over the cap's solid-angle measure
            // (1 - u*(1-cos sp)), azimuth 2*pi*u2, in an orthonormal frame
            // built deterministically from aim.
            const double sp_r = static_cast<double>(*sp);
            const double cos_t = 1.0 - u1 * (1.0 - std::cos(sp_r));
            const double sin_t = std::sqrt(std::max(0.0, 1.0 - cos_t * cos_t));
            const double phi = 2.0 * math::kPi_d * u2;
            // ref = axis of aim's smallest-|component|: cross(aim, ref) has
            // length >= sqrt(2/3) for unit aim (never near-degenerate).
            const math::dvec3 ref = (std::abs(aim.x) <= std::abs(aim.y) && std::abs(aim.x) <= std::abs(aim.z))
                                        ? math::dvec3{1.0, 0.0, 0.0}
                                        : math::dvec3{0.0, 1.0, 0.0};
            const math::dvec3 e1 = math::normalize(math::cross(aim, ref));
            const math::dvec3 e2 = math::cross(aim, e1);
            fired = aim * cos_t + (e1 * std::cos(phi) + e2 * std::sin(phi)) * sin_t;
            fired = math::normalize(fired);
        }
    }

    // --- commit: heat door push (per-shot attr; missing/<=0 = heatless gun),
    // cooldown stamp, stream advance. Unbounded heat on purpose (gun.h).
    double per_shot = 0.0;
    if (!gun.attrs.heat_per_shot.empty()) {
        if (const auto hs = gun.pipe->derived(gun.attrs.heat_per_shot);
            hs && std::isfinite(*hs) && *hs > 0.0f)
            per_shot = static_cast<double>(*hs);
    }
    gun.heat.set_value(gun.heat.value() + per_shot);
    gun.last_shot = now;
    ++gun.shots;
    return fire_result{fired, now, gun.heat.value()};
}

}  // namespace ve::core::sim
