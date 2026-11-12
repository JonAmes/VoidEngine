#version 450
// 8.6 terrain-look spike (#17d) — the two art directions, same geometry
// contract, switched by the push look id:
//   look 0: stylized low-poly — flat CPU normals, quantized palette cards
//           (uv.x = height band, uv.y = slope band, band CENTER encoded so
//           floor() decodes exactly), cel-quantized NDL, band-edge lines.
//   look 1: realistic-leaning — smooth sampler normals, continuous
//           height/slope color blend, Blinn sheen on rock faces, distance
//           haze toward the horizon.
// The sky clear color set by the demo MUST equal kSky below or the painted
// gate (summary + driver cross-check) measures the wrong background.
layout(set = 0, binding = 0, std140) uniform Cam {
    mat4 view; mat4 proj; vec4 camPos;
} cam;
layout(location = 0) in vec3 wnorm;
layout(location = 1) in vec3 wpos;
layout(location = 2) in vec2 hq;
layout(push_constant) uniform TerrainPush {
    mat4 model;
    vec4 look;
} tp;
layout(location = 0) out vec4 outColor;

const vec3 kSky = vec3(0.53, 0.67, 0.82);             // shared sky/fog anchor
const vec3 kSun = normalize(vec3(-0.45, 0.85, 0.28)); // hull.frag's sun, shared

// Palette cards (#17d "palette card"): hand-picked flat colors, the stylized
// look's entire material story. Band 0 = shadow scree, 1 = basalt, 2 = soil,
// 3 = scrub, 4 = pale cap; steep slopes snap to the basalt card.
vec3 palette(int band) {
    if (band == 0) return vec3(0.16, 0.13, 0.11);
    if (band == 1) return vec3(0.32, 0.27, 0.24);
    if (band == 2) return vec3(0.47, 0.36, 0.22);
    if (band == 3) return vec3(0.36, 0.44, 0.26);
    return vec3(0.78, 0.76, 0.72);
}

void main() {
    const vec3 N = normalize(wnorm);
    const float ndl = max(dot(N, kSun), 0.0);
    const float dist = distance(wpos, cam.camPos.xyz);

    if (tp.look.x < 0.5) {
        // ---- stylized low-poly cel -------------------------------------
        int hband = int(floor(hq.x * 5.0));
        int sband = int(floor(hq.y * 3.0));
        if (sband == 2) hband = 1; // steep -> bare rock card
        vec3 c = palette(hband);
        // cel light: NDL quantized to 4 steps, hard terminator, no gradient.
        const float cel = 0.30 + 0.72 * (floor(ndl * 3.0) / 3.0);
        c *= cel;
        // band-edge contour: neighboring fragments landed on another card.
        if (dFdx(hq.x) != 0.0 || dFdy(hq.x) != 0.0) c *= 0.62;
        outColor = vec4(c, 1.0);
        return;
    }

    // ---- realistic-leaning ----------------------------------------------
    const float h = hq.x;             // continuous height param 0..1
    const float slope = hq.y;         // continuous slope param 0..1
    vec3 c = mix(vec3(0.34, 0.27, 0.19), vec3(0.30, 0.37, 0.21), smoothstep(0.15, 0.50, h));
    c = mix(c, vec3(0.52, 0.49, 0.45), smoothstep(0.55, 0.80, h));
    c = mix(c, vec3(0.86, 0.86, 0.85), smoothstep(0.82, 0.95, h));
    c = mix(c, vec3(0.40, 0.37, 0.33), smoothstep(0.35, 0.85, slope)); // steep -> rock
    const vec3 warm = vec3(1.00, 0.94, 0.84);
    vec3 lit = c * (vec3(0.34, 0.42, 0.52) * 0.42 + warm * ndl);
    // Blinn sheen, rougher the looser the slope: rock faces catch the sun.
    const vec3 V = normalize(cam.camPos.xyz - wpos);
    const vec3 H = normalize(V + kSun);
    const float shin = mix(4.0, 64.0, 1.0 - slope);
    lit += warm * pow(max(dot(N, H), 0.0), shin) * mix(0.06, 0.30, 1.0 - slope);
    // distance haze to the sky: single scale (600 m) — spike-grade atmosphere.
    const float fog = 1.0 - exp(-dist / 600.0);
    lit = mix(lit, vec3(0.66, 0.74, 0.85), fog * 0.85);
    outColor = vec4(clamp(lit, 0.0, 1.0), 1.0);
}
