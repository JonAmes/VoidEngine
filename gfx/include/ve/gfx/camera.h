#pragma once
//
// Phase 3.2 — camera = rig + eyes. The VR shape from AGENTS.md VR rules:
// nothing may assume one camera, a fixed aspect, or a window. A rig carries
// an ORDERED list of eyes; every pass consumes per-eye eye_frames
// (view + projection + viewport). Desktop is a rig with ONE centered eye
// going through the identical code path — stereo/XR later only add eyes.
//
// fp32 throughout (AGENTS #5: fp64 is for sim positions; the rig lives at the
// floating origin (decision #3), so rig-space offsets are always small).
// compute() is a pure function of rig + eyes: no wall clock, no hidden state,
// eye order is authored order = instance eye index for single-pass instanced
// rendering (3.9) — never a hash order (determinism, AGENTS #5).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ve/core/math/mat4.h"
#include "ve/core/math/projection.h"
#include "ve/core/math/quat.h"

namespace ve::gfx {

// Rectangle an eye rasterizes into on the pass's render target.
struct viewport {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// One frustum as near-plane TANGENTS (AGENTS VR rule 2: projection builders
// take L/R/T/B tans from day one; fov/aspect is a convenience input only).
struct frustum {
    float near_z{0.1f};
    float far_z{1000.0f};
    float l{}, r{}, b{}, t{};
    ve::core::math::depth_range depth = ve::core::math::depth_range::standard_0_to_1;
};

// Symmetric eye frustum (desktop centered eye) — built through tans anyway,
// so one code path serves desktop and VR.
frustum symmetric_frustum(float fov_y_rad, float aspect, float n, float f);

// The "two asymmetric off-axis frustums" done-when shape: eyes sit at
// +/-ipd/2 along the rig's X axis with NO toe-in (both look parallel to rig
// forward), and each frustum's CENTER RAY passes through the focal point on
// the rig axis at distance focal_dist. Derivation: the off-axis center ray
// satisfies clip.x = 0 -> x/z = -(r+l)/2 (A = 2/(r-l), B = (r+l)/(r-l) from
// the Phase 1 builder); the fixation point sits in the right eye's view
// space at (-ipd/2, 0, -focal_dist), so x/z = +ipd/(2*focal_dist) and
// r + l = -ipd/focal_dist for the right eye — mirrored for the left.
// The width r - l stays the symmetric one.
std::pair<frustum, frustum> converged_stereo_frustums(float fov_y_rad, float aspect,
                                                      float n, float f,
                                                      float ipd, float focal_dist);

// An eye authored in rig space: offset + frustum + target rectangle.
struct eye_def {
    ve::core::math::trsf rig_local{};  // eye transform relative to the rig
    frustum frust{};
    viewport view{};                   // where this eye lands on the target(s)
};

// The per-eye pair every pass consumes.
struct eye_frame {
    ve::core::math::mat4f view;
    ve::core::math::mat4f proj;
    viewport viewport_;
};

class camera_rig {
public:
    // Rig transform in world/floating-origin space (decision #3: the world
    // arrives as offsets from the rig, keeping fp32 rig math exact enough).
    void set_rig(const ve::core::math::trsf& rig) { rig_ = rig; }
    const ve::core::math::trsf& rig() const { return rig_; }

    // Ordered: eye index is semantic (instance eye index in 3.9).
    std::size_t add_eye(const eye_def& eye);
    std::size_t eye_count() const { return eyes_.size(); }
    const eye_def& eye(std::size_t i) const { return eyes_.at(i); }

    // Per-frame evaluation: view = inverse_rigid(compose(rig) *
    // compose(eye.rig_local)), proj = perspective_offaxis(frust).
    std::vector<eye_frame> compute() const;

private:
    ve::core::math::trsf rig_{};
    std::vector<eye_def> eyes_;
};

} // namespace ve::gfx
