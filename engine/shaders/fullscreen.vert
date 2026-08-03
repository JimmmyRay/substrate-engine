#version 450

// One oversized triangle rather than two: no diagonal seam, one fewer vertex, and
// the GPU rasterises a single primitive.
layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
