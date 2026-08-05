#include "scene/LitSprite.h"

namespace scene {

MeshData quadMesh(const QuadDesc& desc) {
    MeshData mesh;
    mesh.material = desc.material;
    mesh.transform = desc.transform;
    mesh.masked = desc.masked;

    // The pivot is a fraction of the size measured from the **top-left**, matching
    // `SpriteDesc::pivot`, so the quad hangs off the point the transform places.
    const float left = -desc.pivot.x * desc.size.x;
    const float right = left + desc.size.x;
    const float top = desc.pivot.y * desc.size.y;
    const float bottom = top - desc.size.y;

    // 0 and 1 rather than the texel rect -- see the header.
    const float u0 = desc.flipX ? 1.0f : 0.0f;
    const float u1 = desc.flipX ? 0.0f : 1.0f;
    // glTF's UV origin is the top-left, so v grows downwards and the *top* of the quad is
    // v = 0. Backwards draws the sprite upside down and changes nothing else.
    const float v0 = desc.flipY ? 1.0f : 0.0f;
    const float v1 = desc.flipY ? 0.0f : 1.0f;

    // Facing +Z, wound anticlockwise seen from +Z -- the order and tangent `unitCube` gives
    // its +Z face, so a quad and a cube agree about handedness.
    const glm::vec3 normal{0.0f, 0.0f, 1.0f};
    const glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};

    mesh.vertices = {
        {{left, bottom, 0.0f}, normal, tangent, {u0, v1}},
        {{right, bottom, 0.0f}, normal, tangent, {u1, v1}},
        {{right, top, 0.0f}, normal, tangent, {u1, v0}},
        {{left, top, 0.0f}, normal, tangent, {u0, v0}},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};

    // A flat box is correct: a quad has no thickness, and the culling test transforms eight
    // corners of whatever it is given.
    mesh.localMin = {left, bottom, 0.0f};
    mesh.localMax = {right, top, 0.0f};
    return mesh;
}

} // namespace scene
