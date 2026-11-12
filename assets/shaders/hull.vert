#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uvin;
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(push_constant) uniform Model { mat4 model; } mdl;
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec3 wnorm;
void main() {
    vec4 wp = mdl.model * vec4(pos, 1.0);
    wnorm = mat3(mdl.model) * nrm;   // rotation-only model: mat3 is exact
    gl_Position = cam.proj * (cam.view * wp);
}
