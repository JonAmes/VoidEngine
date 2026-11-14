#version 450
// 8.18 descent segment (tools/demo --mode descent): flat-color terrain pass,
// vertex half of the terrain_descent_golden test's pipeline — plus a
// height-brightness ramp: uv[0] carries the normalized surface height (the
// uv slot is dead weight in this pass). Flat albedo at nadir would render
// ANY surface as a uniform fill; shading height makes the relief legible
// to human eyes without touching the coverage/depth contract.
// Flat RGB color rides the nrm attribute (band table, #29d); depth is computed in the frag
// (log-zoom), so the vertex stage is a plain MVP transform.
layout(push_constant) uniform Push { mat4 mvp; } push;
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in float hgt;  // normalized height [0,1]
// explicit location: out_position already pins 0; implicit numbering
// would mismatch the frag (position-in-as-color = flat clamped garbage).
layout(location = 1) out vec4 col;
void main() {
    gl_Position = push.mvp * vec4(pos, 1.0);
    // band color x height ramp x soft slope shade: this fixture's local
    // relief is genuinely smooth, so height alone quantizes flat; the
    // per-vertex normal gives the smooth shading the eye reads as form.
    // Cosmetic only — coverage and depth contracts are untouched.
    col = vec4(nrm * (0.55 + 0.65 * hgt) *
               (0.7 + 0.5 * max(dot(normalize(nrm), vec3(0.3, -0.4, 0.85)), 0.0)),
               1.0);
}
