#version 450
// 8.18 descent segment (tools/demo --mode descent): frag half of the
// terrain_descent_golden pipeline — EXCEPT depth: the driver's interpolated
// fp32 z is useless across 1.5 m .. 6e6 m in one projection, so the frag
// computes log-zoom depth from the camera distance itself: w_clip == distance
// for the view-space setup (deterministic spine, #49 look half).
// Constants: far_log2 = log2(kF), inv_span = 1/log2(kF/kN) with kN = 0.5 m,
// kF = 1e12 m (8.10 budget) — keep in lockstep with ve/core/math/projection.h
// callers (perspective_logzoom), which maps the SAME interval.
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 out_color;
void main() {
    const float far_log2 = 39.863137138646355;  // log2(1e12)
    const float inv_span = 0.0244719341201584;  // 1 / log2(1e12 / 0.5)
    out_color = col;
    gl_FragDepth = clamp((far_log2 + log2(gl_FragCoord.w)) * inv_span, 0.0, 1.0);
}
