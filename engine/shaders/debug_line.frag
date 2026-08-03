#version 450

// The colour, unchanged. The pass runs after the tonemap, so what arrives here is
// already display-referred and applying an exposure or a curve to it would mean a
// wireframe whose colour depended on how bright the scene behind it happened to be.

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
