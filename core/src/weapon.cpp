#include "ve/core/sim/weapon.h"

#include <cmath>

#include "ve/core/math/quat.h"

namespace ve::core::sim {

std::optional<laser_hit> laser_raycast(
    math::dvec3 origin, math::dvec3 dir, double range,
    std::span<const comp_bounds> targets)
{
    const bool finite = std::isfinite(origin.x) && std::isfinite(origin.y) &&
                        std::isfinite(origin.z) && std::isfinite(dir.x) &&
                        std::isfinite(dir.y) && std::isfinite(dir.z) &&
                        std::isfinite(range);
    if (!finite || !(range >= 0.0)) return std::nullopt;
    // Unit dir keeps the swept primitive's t-domain in METRES. rotate()
    // would otherwise scale the reported t/normal the same way it scales a
    // non-unit OBB attitude (rejected there for the same reason).
    if (!math::approximately_equal(math::length2(dir), 1.0, 1e-6))
        return std::nullopt;

    std::optional<laser_hit> best;
    for (const auto& c : targets) {
        // Segment-vs-OBB == point sweep at unit speed for `range` seconds:
        // t is then exactly the metres-along-ray of the slab entry.
        const auto hit = sweep_point_vs_obb(origin, dir, range, c.bounds);
        if (!hit) continue;
        if (!best || hit->t < best->dist) {
            best = laser_hit{c.component, hit->t, hit->pos, hit->normal};
        }
    }
    return best;
}

} // namespace ve::core::sim
