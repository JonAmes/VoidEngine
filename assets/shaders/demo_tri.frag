#version 450
// ve_demo (checklist 2.9) fragment stage. Interpolated vertex colors, no
// lighting, no textures: the evidence this task produces is about motion and
// frame pacing, and every extra stage is one more thing that could differ
// between lavapipe and a real driver.
layout(location = 0) in vec3 col;
layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(col, 1.0); }
