#pragma once
// Phase 8 task 8.9 — the floating origin as SIM-side state (#3 real-scale).
//
// math::origin_frame (1.7) is pure coordinate math; this tracker owns the
// DETERMINISTIC REBASE RULE: the shift decision is evaluated exactly once per
// sim tick, on the post-step sim state of the focus entity, and NEVER from a
// render frame, interpolated focus, or wall clock (determinism rule #5).
// The focus is whatever entity the sim/run is "about" (player ship, chase
// camera subject); the threshold is game/def-chosen (this layer stays free of
// game concepts — it just sees a dvec3 and a double).
//
// Replay contract (pinned by test floating_origin): rebasing mutates ONLY the
// tracker. Positions stay absolute fp64 and dynamics never read the origin,
// so per-tick state hashes (4.7) are bit-identical with and without rebasing;
// and because advance() is a pure function of (tick state, threshold), the
// rebase tick sequence itself replays identically for identical command logs.
//
// GPU contract: every transform composition goes through to_local_f32 —
// fp64 subtraction FIRST, fp32 cast SECOND. Casting world-absolute fp64 to
// f32 first loses every metre past ~1e7 m (ulp at 1 AU is 16 km — pinned by
// camera_relative_precision). Nothing renders world-absolute.

#include <cstdint>

#include "ve/core/math/coords.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

class OriginTracker {
public:
    // Advance the tracker at the END of sim `tick`, with the focus's world
    // position as of that exact tick. Rebase fires iff |focus - origin|
    // strictly exceeds threshold — a threshold crossing stamped at an exact
    // tick, never a per-frame float compare. Returns whether it fired.
    bool advance(tick_t tick, const math::dvec3& focus, double threshold_m) {
        if (!m_frame.needs_shift(focus, threshold_m)) return false;
        m_frame.shift_to(focus);   // world positions untouched; locals shift
        m_last_rebase_tick = tick;
        ++m_rebase_count;
        return true;
    }

    // The sanctioned path from a world position to GPU (fp32) coordinates.
    math::vec3f to_local_f32(const math::dvec3& world) const {
        return m_frame.to_local_f32(world);
    }

    const math::origin_frame& frame() const { return m_frame; }
    tick_t last_rebase_tick() const { return m_last_rebase_tick; }  // 0 = never (tick 0 is genesis)
    std::int32_t rebase_count() const { return m_rebase_count; }

private:
    math::origin_frame m_frame{};
    tick_t m_last_rebase_tick = 0;
    std::int32_t m_rebase_count = 0;
};

} // namespace ve::core::sim
