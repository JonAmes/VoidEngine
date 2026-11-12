#version 450
// 8.6 terrain-look spike (#17d): the 7.16 Cam UBO contract verbatim, plus an
// 80-byte push block (model + look vec4). mesh_vertex attribute contract
// (pos@0 nrm@12 uv@24); uv is re-purposed as (height param, slope param) and
// tan is unused — a spike, so the repurpose lives here and in
// tools/demo/src/terrain_mode.cpp, nowhere in core/gfx.
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uvin;
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(push_constant) uniform TerrainPush {
    mat4 model;
    vec4 look; // x: 0 = stylized low-poly cel, 1 = realistic-leaning
} tp;
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec3 wnorm;
layout(location = 1) out vec3 wpos;
layout(location = 2) out vec2 hq;
void main() {
    vec4 wp = tp.model * vec4(pos, 1.0);
    wnorm = mat3(tp.model) * nrm;   // identity model in this spike: exact
    wpos = wp.xyz;
    hq = uvin;
    gl_Position = cam.proj * (cam.view * wp);
}
