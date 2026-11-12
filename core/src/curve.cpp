// Phase 6 task 6.13 — see curve.h for the contract.
#include "ve/core/sim/curve.h"

#include <cmath>

namespace ve::core::sim {

bool piecewise_curve::set_points(std::vector<curve_point> pts) {
    if (pts.size() < 2)
        return false;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y))
            return false;
        if (i > 0 && !(pts[i].x > pts[i - 1].x))
            return false;  // x must STRICTLY ascend (equal x = ambiguous y)
    }
    m_pts = std::move(pts);
    return true;
}

std::optional<double> piecewise_curve::evaluate(double x) const {
    if (m_pts.size() < 2)
        return std::nullopt;
    if (x <= m_pts.front().x)
        return m_pts.front().y;  // flat tail below the cheapest breakpoint
    if (x >= m_pts.back().x)
        return m_pts.back().y;   // flat tail above the dearest
    // Bracket scan: authored breakpoint count is tiny; keep the fixed
    // left-to-right form so evaluation never depends on search internals.
    for (std::size_t i = 1; i < m_pts.size(); ++i) {
        if (x <= m_pts[i].x) {
            const curve_point& a = m_pts[i - 1];
            const curve_point& b = m_pts[i];
            return a.y + (x - a.x) * (b.y - a.y) / (b.x - a.x);
        }
    }
    return m_pts.back().y;  // unreachable; keeps every path total
}

}  // namespace ve::core::sim
