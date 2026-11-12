#version 450
layout(location = 0) in vec3 wnorm;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 N = normalize(wnorm);
    vec3 L = normalize(vec3(-0.45, 0.85, 0.28));
    float ndl = max(dot(N, L), 0.0);
    vec3 bcol = vec3(0.72, 0.73, 0.76);
    vec3 c = bcol * (0.30 + 0.85 * ndl);
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
