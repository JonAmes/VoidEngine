#version 450
// ve_demo (checklist 2.9) vertex stage. Cooked GLSL -> SPIR-V by the cooklet
// (decision #13); the runtime only ever sees cooked bytes.
//
// The MVP arrives as a push constant — the same Phase 1 pattern 2.6 pinned, and
// the reason a spin costs one vkCmdPushConstantRange per frame instead of a
// buffer rebuild.
//
// Shape is deliberately SCALANE and off-center: an equilateral triangle centered
// on the NDC origin looks identical under some rotation steps, which would make
// the two evidence frames an unreliable proof that the spin actually spins.
layout(push_constant) uniform Push { mat4 mvp; } push;

layout(location = 0) out vec3 col;

void main() {
    vec2 p[3] = vec2[3](
        vec2(-0.75, -0.10),
        vec2( 0.55,  0.20),
        vec2(-0.05,  0.80));
    vec3 c[3] = vec3[3](
        vec3(1.00, 0.15, 0.15),
        vec3(0.15, 0.90, 0.25),
        vec3(0.20, 0.35, 1.00));

    // Vertex data baked from gl_VertexIndex, exactly as in 2.6: no vertex
    // buffer, so the resize path has no geometry to re-upload (2.8's
    // "resize -> VB + recreate" note: there is no VB here to touch).
    gl_Position = push.mvp * vec4(p[gl_VertexIndex], 0.0, 1.0);
    col = c[gl_VertexIndex];
}
