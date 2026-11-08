#include "ve/gfx/camera.h"

#include <cmath>
#include <optional>

namespace ve::gfx {

using namespace ve::core::math;

frustum symmetric_frustum(float fov_y_rad, float aspect, float n, float f) {
    frustum fr;
    fr.near_z = n;
    fr.far_z = f;
    // same construction the Phase 1 symmetric wrapper uses — kept here so
    // frustum carries tans, never fov/aspect.
    const float t = std::tan(fov_y_rad / 2.0f);
    const float r = t * aspect;
    fr.l = -r;
    fr.r = r;
    fr.b = -t;
    fr.t = t;
    return fr;
}

std::pair<frustum, frustum> converged_stereo_frustums(float fov_y_rad, float aspect,
                                                      float n, float f,
                                                      float ipd, float focal_dist) {
    const frustum base = symmetric_frustum(fov_y_rad, aspect, n, f);
    // see camera.h: right eye (offset +ipd/2) shifts its tans LEFT by
    // ipd/(2*focal_dist); left eye mirrors. focal_dist > 0 required — a
    // focal point behind the eyes is a config error, not a negative shift.
    const float shift = (focal_dist > 0.0f && ipd > 0.0f) ? ipd / (2.0f * focal_dist) : 0.0f;
    frustum right = base;
    right.r = base.r - shift;
    right.l = base.l - shift;
    frustum left = base;
    left.r = base.r + shift;
    left.l = base.l + shift;
    return {left, right};
}

std::size_t camera_rig::add_eye(const eye_def& eye) {
    eyes_.push_back(eye);
    return eyes_.size() - 1;
}

std::vector<eye_frame> camera_rig::compute() const {
    std::vector<eye_frame> frames;
    frames.reserve(eyes_.size());
    const mat4f rig_world = trs_compose(rig_);
    for (const eye_def& e : eyes_) {
        const mat4f eye_world = rig_world * trs_compose(e.rig_local);
        // rig + eye transforms are rigid by construction (no scale on rigs
        // or eyes); invert_rigid is the exact inverse when that holds and
        // nullopt when the rotation block sheared — fall back to identity
        // rather than rendering garbage if a caller ever violates it.
        const std::optional<mat4f> inv = invert_rigid(eye_world);
        eye_frame f;
        f.view = inv.value_or(mat4f{});
        f.proj = perspective_offaxis(e.frust.near_z, e.frust.far_z,
                                     e.frust.l, e.frust.r, e.frust.b, e.frust.t,
                                     e.frust.depth);
        f.viewport_ = e.view;
        frames.push_back(f);
    }
    return frames;
}

} // namespace ve::gfx
