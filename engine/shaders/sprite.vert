#version 450

/**
 * Sprite quads: six vertices per sprite, no vertex buffer and no vertex attributes.
 *
 * The shape is `particle.vert`'s, which is the argument for it -- a `const vec2 kCorners[6]`
 * indexed by `gl_VertexIndex`, per-instance data out of an SSBO through `gl_InstanceIndex`,
 * and a bindless texture index carried out `flat`. What a sprite adds over a billboard is
 * rotation, a pivot, a flip and a UV rect; what it drops is the camera-facing basis, because
 * a sprite lives on the z = 0 plane in world space and the camera is orthographic.
 *
 * `gl_InstanceIndex` walks the array in the order `SpriteTable::draws()` produced, which is
 * layer order then creation order. There is no depth test and no depth write: the CPU sort
 * *is* the order, which is what a painter's algorithm means and what lets ten thousand
 * blended quads be one draw.
 *
 * The rotation arrives as a cosine and a sine rather than an angle. `SpriteTable` writes
 * them once per change; computing them here would be a `sin` and a `cos` per vertex per
 * frame for a value that almost never moves.
 */

struct Sprite {
    vec4 posSize;  ///< xy world position of the pivot, zw size in world units
    vec4 rotPivot; ///< xy cos/sin of the rotation, zw pivot as a fraction of size
    vec4 uvRect;   ///< texels: x, y, width, height. Zero width or height = the whole image
    uvec4 meta;    ///< x image slot, y tint packed RGBA8, z flags, w unused
};

layout(std430, set = 1, binding = 0) readonly buffer SpriteBuffer {
    Sprite sprites[];
};

layout(push_constant) uniform Push {
    /// The *unjittered* view-projection, pushed rather than taken from the frame set: this
    /// pass runs after the tonemap and wants the matrix the camera is actually looking
    /// through. It is also the whole of what the pass needs from the frame, and binding
    /// the frame set to get one matrix would mean declaring the light buffer and the
    /// shadow matrices in a shader that reads neither -- the argument `debug_line.vert`
    /// already makes.
    mat4 viewProj;
} pc;

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vTint;
layout(location = 2) flat out uint vImage;
layout(location = 3) flat out vec4 vUvRect;

/// Two triangles as a list, with the origin at the image's top-left corner -- the same
/// convention the glyph metrics and every texture in the engine already use. A strip would
/// be four vertices and would need the pipeline to carry a topology nothing else uses;
/// `particle.vert` made that trade first.
const vec2 kCorners[6] =
    vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

const uint kFlipX = 1u;
const uint kFlipY = 2u;

void main() {
    Sprite s = sprites[gl_InstanceIndex];
    vec2 corner = kCorners[gl_VertexIndex];

    // Local space, pivot at the origin. `corner.y` runs *down* the image and world +Y is
    // up, so the y term is negated -- which is what puts a pivot of {0.5, 1.0} at a
    // character's feet rather than over its head.
    vec2 local = (corner - s.rotPivot.zw) * s.posSize.zw;
    local.y = -local.y;

    float c = s.rotPivot.x;
    float sn = s.rotPivot.y;
    vec2 world = s.posSize.xy + vec2(local.x * c - local.y * sn, local.x * sn + local.y * c);

    gl_Position = pc.viewProj * vec4(world, 0.0, 1.0);

    // The flip mirrors the *rect*, not the geometry: the quad stays where the pivot put it
    // and the image inside it turns round, which is what every 2D tool means by flipX.
    vec2 uvCorner = corner;
    if ((s.meta.z & kFlipX) != 0u) uvCorner.x = 1.0 - uvCorner.x;
    if ((s.meta.z & kFlipY) != 0u) uvCorner.y = 1.0 - uvCorner.y;

    vCorner = uvCorner;
    vTint = unpackUnorm4x8(s.meta.y);
    vImage = s.meta.x;
    // Carried whole and resolved in the fragment stage, because the division that turns
    // texels into normalised coordinates needs `textureSize` -- and the image array is
    // declared to the fragment stage alone.
    vUvRect = s.uvRect;
}
