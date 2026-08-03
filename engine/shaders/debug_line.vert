#version 450

// World-space debug lines (S4.5). Physics fills the vertex buffer; this draws it.
//
// A push constant rather than the frame descriptor set, and it is the whole binding
// surface of the pass: a line needs the view-projection and nothing else, and taking the
// frame set would mean declaring the light buffer and the shadow matrices in a shader
// that reads neither.

layout(location = 0) in vec3 inPosition; // world space
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    vColor = inColor;
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
}
