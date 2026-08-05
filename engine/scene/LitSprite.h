#pragma once

#include "gfx/ImageTable.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

namespace scene {

/**
 * @file engine/scene/LitSprite.h
 * @brief A sprite that goes through the shading path: a quad, a material, an instance.
 *
 * A lit sprite is geometry -- it writes depth and velocity, and it is culled, shadowed,
 * ambient-occluded, reflected, fogged, temporally resolved and tonemapped, so **it is not
 * bit-identical to its source file**. `scene::SpriteTable` is the path that is, drawing
 * after the tonemap with no depth test; the two cannot both be true of one draw.
 *
 * A lit sprite is a `GltfScene::ModelId`, freed by `Engine::removeModel` like any other
 * model, and ALPHA_MODE MASK rather than BLEND -- which is what keeps it inside
 * `InstanceTable::dynamicCount()` and its TAA motion correction.
 */

/// How a quad is laid out, in the terms `SpriteDesc` already uses.
struct QuadDesc {
    /// Into the scene's material table, from `GltfScene::createMaterial`.
    uint32_t material = 0;
    /// World units, across and up. The quad lies in the XY plane and faces +Z.
    glm::vec2 size{1.0f, 1.0f};
    /// The point `transform` places, as a fraction of `size` measured from the quad's
    /// **top-left** -- `SpriteDesc::pivot`'s convention, so `{0.5, 1.0}` is a character's
    /// feet in both paths.
    glm::vec2 pivot{0.5f, 0.5f};
    /// Mirror the image about the middle of its UV rect. On the UVs rather than the
    /// positions: a negative scale flips the winding and the normal with it, and a flip
    /// must not move the anchor.
    bool flipX = false;
    bool flipY = false;
    /// Where the pivot goes, in world space.
    glm::mat4 transform{1.0f};
    /// ALPHA_MODE MASK, which is what puts `sprite_lit_shadow.frag` in the shadow pass.
    bool masked = true;
};

/**
 * @brief The four vertices and six indices of one quad, as `createMesh` wants them.
 *
 * **The UVs are the 0..1 corner, not the texel rect.** The rect lives on the material and
 * `sprite_lit.frag` maps the corner into it and divides by `textureSize`, so a frame
 * change is one `setLitSpriteUv` rather than new geometry and nothing CPU-side has to know
 * the file's dimensions.
 */
[[nodiscard]] MeshData quadMesh(const QuadDesc& desc);

/// Everything about one lit sprite that a game states rather than computes.
struct LitSpriteDesc {
    /// From `Engine::images()`. A handle the table cannot resolve draws the font atlas,
    /// which is `ImageTable`'s stated fallback rather than undefined data.
    gfx::ImageId image;
    /// Texels: x, y, width, height. A zero width or height means the whole image, resolved
    /// in the fragment shader for the reason `SpriteDesc::uv` gives.
    glm::vec4 uv{0.0f, 0.0f, 0.0f, 0.0f};
    /// World units.
    glm::vec2 size{1.0f, 1.0f};
    /// A fraction of `size`, from the top-left. `{0.5, 1.0}` is a character's feet.
    glm::vec2 pivot{0.5f, 0.5f};
    /// Where the pivot is, in world space. Three components, because a lit sprite lives in
    /// a 3D scene and its depth is what occludes it.
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    /// Radians, anticlockwise about the quad's own +Z axis.
    float rotation = 0.0f;
    /// Multiplied into the texel. `baseColorFactor`, so it is the same field every other
    /// material tints with; the alpha multiplies into the cutout test.
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    /// Alpha below this discards the fragment.
    float cutoff = 0.5f;
    /// Matte by default: fully rough and non-metallic, which stops a painted sheet picking
    /// up a specular highlight nobody drew.
    float roughness = 1.0f;
    float metallic = 0.0f;
    /// Multiplied into the texel, like the tint. Non-zero makes the sprite cast no shadow --
    /// the rule `shadow.frag` applies to every emissive surface.
    glm::vec3 emissive{0.0f, 0.0f, 0.0f};
    bool flipX = false;
    bool flipY = false;
    /// The transform may change between frames, which is what makes the velocity pass write
    /// a real motion vector rather than the reproject-from-depth sentinel.
    bool dynamic = false;
};

} // namespace scene
