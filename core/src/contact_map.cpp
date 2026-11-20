// Phase 9 task 9.16 — contact map measurement law (see contact_map.h).
// The jitter draw mirrors gun.cpp's 9.10 spread law VERBATIM in expression
// grouping (cap-uniform cos, azimuth, smallest-|component| ref axis) so one
// proven stream/frame recipe covers both — hand-ports of either apply to
// both. Keyed (seed, tick, sensor, target) exactly as #31b spells: a sensor
// measures every tick, so the tick IS the stream position (contrast the gun
// stream, keyed on SHOT count because refusals consume nothing).
#include "ve/core/sim/contact_map.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "ve/core/math/base.h"
#include "ve/core/rng.h"

namespace ve::core::sim {
namespace {

// arcminutes -> radians. (1/60 then deg->rad; deg_to_rad's pi/180 constant
// is shared with every other angle site — one libm-free arithmetic chain.)
double arcmin_to_rad(double arcmin) {
    return math::deg_to_rad(arcmin / 60.0);
}

// One bounded-error measurement of `t` by `s` at tick `now` (#31b law).
contact_record measure(const sensor_view& s, const contact_target& t,
                       std::int64_t slant_key, tick_t now,
                       std::uint64_t world_seed) {
    contact_record rec;
    rec.target = t.id;
    rec.seen = now;
    rec.seen_by = s.id;

    const math::dvec3 rel = t.pos - s.at;
    const double slant = math::length(rel);

    // assist gain: sanitize into [0,1] (non-finite = no assist).
    double gain = t.assist_gain;
    if (!std::isfinite(gain)) gain = 1.0;
    gain = std::clamp(gain, 0.0, 1.0);

    const double cone = arcmin_to_rad(s.res_arcmin) * gain;

    // Perfect cone (gain 0 or res<=0) or co-located: EXACT passthrough —
    // same bit-for-bit rule as the gun's missing-spread passthrough.
    if (!(cone > 0.0) || !(slant > 0.0)) {
        rec.measured = t.pos;
        rec.uncertainty = 0.0;
    } else {
        // stream key: (seed, tick, sensor, target) — #31b verbatim.
        std::uint64_t h = splitmix64(world_seed ^
                                     (static_cast<std::uint64_t>(s.id) * 0x9E3779B97F4A7C15ULL));
        h = splitmix64(h ^ (static_cast<std::uint64_t>(now) * 0xBF58476D1CE4E5B9ULL + 0x94D049BB133111EBULL));
        h = splitmix64(h ^ (static_cast<std::uint64_t>(slant_key) * 0x9E3779B97F4A7C15ULL));
        const double u1 = unit01(splitmix64(h));
        const double u2 = unit01(splitmix64(h + 0x9E3779B97F4A7C15ULL));

        // Uniform direction in the cone of half-angle `cone` around the LOS
        // (identical grouping to gun.cpp's spread block).
        const math::dvec3 axis = math::normalize(rel);
        const double cos_t = 1.0 - u1 * (1.0 - std::cos(cone));
        const double sin_t = std::sqrt(std::max(0.0, 1.0 - cos_t * cos_t));
        const double phi = 2.0 * math::kPi_d * u2;
        const math::dvec3 ref = (std::abs(axis.x) <= std::abs(axis.y) && std::abs(axis.x) <= std::abs(axis.z))
                                    ? math::dvec3{1.0, 0.0, 0.0}
                                    : math::dvec3{0.0, 1.0, 0.0};
        const math::dvec3 e1 = math::normalize(math::cross(axis, ref));
        const math::dvec3 e2 = math::cross(axis, e1);
        math::dvec3 dir = axis * cos_t + (e1 * std::cos(phi) + e2 * std::sin(phi)) * sin_t;
        dir = math::normalize(dir);
        rec.measured = s.at + dir * slant;
        rec.uncertainty = slant * cone;  // grows linearly with distance (#31b)
    }

    // Resolution bands (#31c) at the winner's slant, against the EFFECTIVE
    // cone — assist buys silhouette classification down with the same knob
    // that buys position uncertainty down.
    rec.class_resolved = t.hull_size > 0.0 && t.hull_size >= slant * cone;
    if (rec.class_resolved) rec.class_tag = t.class_tag;
    rec.owner_resolved = s.iff_range > 0.0 && slant <= s.iff_range;
    if (rec.owner_resolved) rec.owner = t.owner;
    return rec;
}

}  // namespace

void contact_measure_tick(contact_board& board,
                          std::span<const sensor_view> sensors,
                          std::span<const contact_target> targets,
                          tick_t now, std::uint64_t world_seed) {
    // Age out ghosts first (#31d): lifetime is measured from the LAST
    // measurement, so a record frozen at tick T survives while
    // now - T <= ghost_ticks and vanishes the tick after.
    std::erase_if(board.records, [&](const contact_record& r) {
        return now - r.seen > board.ghost_ticks;
    });

    for (const auto& t : targets) {  // targets in caller order (rule 5)
        // Nearest live sensor wins; ties keep the FIRST registered sensor
        // (strict-less improvement => total order, replay-identical).
        const sensor_view* winner = nullptr;
        double best = 0.0;
        for (const auto& s : sensors) {
            if (!std::isfinite(s.res_arcmin)) continue;  // corrupt sensor
            const double slant = math::length(t.pos - s.at);
            // One comparison covers EVERY dead case: NaN range makes the
            // comparison false, range <= 0 can never be reached by a
            // positive slant (co-located slant 0 still measures — exact).
            if (!(slant <= s.range)) continue;
            if (!winner || slant < best) {
                winner = &s;
                best = slant;
            }
        }
        if (!winner) continue;  // unseen this tick: just ages (ghost)

        contact_record rec = measure(*winner, t, t.id, now, world_seed);
        bool replaced = false;
        for (auto& r : board.records) {
            if (r.target == t.id) { r = std::move(rec); replaced = true; break; }
        }
        if (!replaced) board.records.push_back(std::move(rec));
    }
}

bool contact_targetable(const contact_board& board, std::int64_t target,
                        tick_t now) {
    const contact_record* r = board.find(target);
    return r && r->seen == now;  // a ghost is never targetable (#31d)
}

}  // namespace ve::core::sim
