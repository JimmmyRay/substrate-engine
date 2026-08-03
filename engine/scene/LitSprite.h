#pragma once

#include "gfx/ImageTable.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

namespace scene {

/**
 * @file engine/scene/LitSprite.h
 * @brief A sprite that goes through the shading path: a quad, a material, an instance (P6).
 *
 * ```cpp
 * void MyGame::init(Engine& e) {
 *     hero = e.images().load("res:/hero.png");
 *     card = e.createLitSprite({
 *         .image    = hero,
 *         .uv       = {0.0f, 0.0f, 16.0f, 16.0f},   // texels, not normalised
 *         .size     = {1.0f, 2.0f},                 // world units
 *         .pivot    = {0.5f, 1.0f},                 // feet
 *         .position = {0.0f, 0.0f, 0.0f},
 *     });
 * }
 *
 * void MyGame::frameUpdate(Engine& e, float dt) {
 *     e.setLitSpriteUv(card, e.sprites().frameUv(sheet, cell));   // P5's slicing, reused
 * }
 * ```
 *
 * ## The trade, stated here because it is the opposite of P4's
 *
 * `scene::SpriteTable` draws **after the tonemap**, into the virtual-resolution target,
 * with no depth test -- the only place in this frame where a texel reaches the swapchain
 * unaltered, which is what makes `scripts/readback.sh` a proof. What it gives up is stated
 * on that header: no occlusion by 3D geometry, no bloom, no SSR, no fog.
 *
 * This is the other half of the same sentence. A lit sprite is geometry: it writes depth
 * and velocity, it is culled, shadowed, ambient-occluded, reflected, fogged, temporally
 * resolved and tonemapped, and **it is therefore not bit-identical to its source file**.
 * That is the point rather than a defect. The two cannot both be true of one draw, so they
 * are two paths rather than one with a flag -- see the card for why a `bool lit` would have
 * been two subsystems wearing one struct.
 *
 * ## What it is made of, all of which already existed
 *
 * G4's `createMesh` and mutable materials, G5's `ShaderVariant`, P1's `ImageTable`, C10's
 * `unloadModel`. The row adds a quad, two small shaders and the arithmetic below. There is
 * no lit-sprite table, no lit-sprite pass and no lit-sprite pipeline: a lit sprite is a
 * `GltfScene::ModelId`, freed by `Engine::removeModel` like any other model, so this
 * introduces no sixth lifetime model -- which is what the P6-vs-C1 rule in
 * `docs/kanban/arcs.md` asks of it.
 *
 * ## Cutout, not blend
 *
 * ALPHA_MODE MASK. `arcs.md` predicted a *blended* lit sprite and therefore predicted that
 * it would be excluded from `InstanceTable::dynamicCount()` and get no TAA motion
 * correction. A cutout is what pixel art has, it costs one `discard`, and it buys all of
 * that back. A blended lit sprite is deferred; see the card for the trigger.
 */

/// How a quad is laid out, in the terms `SpriteDesc` already uses (P6).
struct QuadDesc {
    /// Into the scene's material table, from `GltfScene::createMaterial`.
    uint32_t material = 0;
    /// World units, across and up. The quad lies in the XY plane and faces +Z.
    glm::vec2 size{1.0f, 1.0f};
    /// The point `transform` places, as a fraction of `size` measured from the quad's
    /// **top-left** -- the same convention `SpriteDesc::pivot` uses, so `{0.5, 1.0}` is a
    /// character's feet in both paths rather than in neither.
    glm::vec2 pivot{0.5f, 0.5f};
    /// Mirror the image about the middle of its UV rect. On the *UVs* rather than on the
    /// positions: a negative scale would flip the winding and the normal with it, and the
    /// convention every 2D tool uses is that a flip does not move the anchor.
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
 * A free function rather than a method because it is stateless and its callers span
 * modules -- `Engine::createLitSprite` and the unit suite -- which is the rung the
 * Rule of Threes' scope table names for exactly that shape.
 *
 * **The UVs are the 0..1 corner, not the texel rect.** The rect lives on the material, in
 * texels, and `sprite_lit.frag` maps the corner into it and divides by `textureSize`. That
 * is what makes a frame change one `setLitSpriteUv` rather than new geometry, and it is
 * the only arrangement in which nothing CPU-side has to know the file's dimensions -- the
 * constraint P1 imposed by holding no `VkImage` and P4 already answered the same way.
 *
 * Bounds are stated rather than left to be derived, because they are exact here and the
 * derivation would walk four vertices to reach the same answer.
 */
[[nodiscard]] MeshData quadMesh(const QuadDesc& desc);

/// Everything about one lit sprite that a game states rather than computes (P6).
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
    /// Where the pivot is, in world space. Three components rather than two: a lit sprite
    /// lives in a 3D scene and its depth is what occludes it.
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    /// Radians, anticlockwise about the quad's own +Z axis.
    float rotation = 0.0f;
    /// Multiplied into the texel. `baseColorFactor`, so it is the same field every other
    /// material tints with; the alpha multiplies into the cutout test.
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    /// Below this the fragment is discarded. The whole of what "cutout" means.
    float cutoff = 0.5f;
    /// A sprite is matte by default: fully rough and non-metallic, which is what a painted
    /// sheet is and what stops it picking up a specular highlight nobody drew.
    float roughness = 1.0f;
    float metallic = 0.0f;
    /// Multiplied into the texel, like the tint. Non-zero makes the sprite cast no shadow,
    /// which is the rule `shadow.frag` already applies to every emissive surface.
    glm::vec3 emissive{0.0f, 0.0f, 0.0f};
    bool flipX = false;
    bool flipY = false;
    /// The transform may change between frames, which is what makes the velocity pass write
    /// a real motion vector for it rather than the reproject-from-depth sentinel.
    bool dynamic = false;
};

} // namespace scene
