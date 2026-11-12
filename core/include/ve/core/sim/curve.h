#pragma once
// Phase 6 task 6.13 — the authored price curve (PLAN #20b: "price is a
// derived attribute of stock level ... never stored, so the scrubber and
// ghost-sim evaluate it at any tick"). A curve is pure data: breakpoints
// with strictly increasing x, all finite. Evaluation is piecewise-linear
// interpolation, clamped to the end values outside the breakpoints — the
// flat tail is what makes a bountiful market bottom out at a floor price.
//
// The interpolation form is FIXED (y0 + (x-x0)*(y1-y0)/(x1-x0) in fp64) so
// dyadic hand-computed values land exactly (CHECK(==) pins, like every
// other Phase 6 numeric contract).
//
// Concept-free: the curve knows only numbers. Market stock -> price is a
// use; nothing here names commodities, markets, or prices.
#include <optional>
#include <vector>

namespace ve::core::sim {

struct curve_point {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const curve_point&, const curve_point&) = default;
};

class piecewise_curve {
public:
    piecewise_curve() = default;
    // Invalid curve (empty evaluate) if the points fail validation.
    explicit piecewise_curve(std::vector<curve_point> pts) { set_points(std::move(pts)); }

    // All-or-nothing: replaces the breakpoints only if VALID (>= 2 points,
    // all finite, x strictly ascending). Returns false and changes nothing
    // otherwise.
    bool set_points(std::vector<curve_point> pts);

    // nullopt = no valid curve authored. Otherwise y at x: end values
    // outside the breakpoints (clamped tail), else linear interpolation
    // between the bracketing pair. Deterministic fp64 arithmetic.
    [[nodiscard]] std::optional<double> evaluate(double x) const;

    [[nodiscard]] bool valid() const { return m_pts.size() >= 2; }
    [[nodiscard]] const std::vector<curve_point>& points() const { return m_pts; }

private:
    std::vector<curve_point> m_pts;  // linear scan by design: a handful of
                                     // authored breakpoints, and iteration
                                     // order is load-bearing
};

}  // namespace ve::core::sim
