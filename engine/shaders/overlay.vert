#version 450

// Debug text. Positions arrive in pixels with the origin top-left, which is the
// coordinate system the glyph metrics and the layout code already use; converting
// here keeps every CPU-side number in the units a HUD is actually reasoned about in.

layout(location = 0) in vec2 inPosition; // pixels, origin top-left
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in uint inTexture; // index into the overlay's image array (C5)

layout(push_constant) uniform Push {
    vec2 invScreen; // 1 / framebuffer extent
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
// flat: an index is not a quantity to interpolate, and the two vertices of a quad's
// shared edge always agree, so there is nothing a smooth qualifier could mean here.
layout(location = 2) flat out uint vTexture;

void main() {
    vUv = inUv;
    vColor = inColor;
    vTexture = inTexture;
    gl_Position = vec4(inPosition * pc.invScreen * 2.0 - 1.0, 0.0, 1.0);
}
