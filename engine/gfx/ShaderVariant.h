#pragma once

#include "gfx/Pipeline.h"

#include <string>
#include <vector>

namespace gfx {

/**
 * @file engine/gfx/ShaderVariant.h
 * @brief One material *behaviour*, as a set of shaders the renderer binds for it.
 *
 * A game writes GLSL for a surface the standard glTF material cannot describe, registers it
 * with `Renderer::addShaderVariant`, and points a material's `shader` field at the index
 * that comes back. Pipelines are created the first time a draw needs one, so declaring
 * forty variants costs the two a level uses.
 *
 * The attachments, descriptor set layouts, depth state and sample count belong to the
 * engine -- `engine/shaders/gbuffer_contract.glsl` is that fixture, and a variant's GLSL
 * includes it rather than restating it. `Renderer::verifyShaderBindings` reflects the
 * SPIR-V and aborts in Debug naming a binding or `constant_id` that disagrees, so a
 * violation is a named abort rather than a black screen.
 */
struct ShaderVariant {
    /// For logs and for the object name a capture shows. Not an identity: the index
    /// `addShaderVariant` returns is what a material stores.
    std::string name;

    /// The opaque pair, drawn into the G-buffer. An overriding vertex stage must still
    /// write the five varyings `gbuffer_contract.glsl` names, and the shadow and velocity
    /// passes do not follow it -- geometry moved here moves in neither.
    std::string vertexShader = "gbuffer.vert";
    std::string fragmentShader = "gbuffer.frag";

    /// Fragment only: the shadow pass has its own vertex stage, writing the projected
    /// position and the UV an alpha cutout needs.
    std::string shadowFragment = "shadow.frag";

    /// Drawn by the forward pass, for materials the loader marked ALPHA_MODE BLEND.
    ///
    /// **`layout(set = 4, binding = 0) uniform sampler2D sceneDepth`** is the opaque depth
    /// behind the fragment. Reverse-Z, so `viewDistance()` in `frame.glsl` has to turn both
    /// it and `gl_FragCoord.z` into metres before they are subtracted.
    std::string forwardFragment = "forward.frag";

    /**
     * @brief The variant's own specialisation constants, supplied from `constant_id` 8.
     *
     * Ids 0..7 are the engine's in every id space, so `layout(constant_id = 8)` gets
     * `constants[0]` and cannot collide with a constant the engine adds later. Supplied to
     * all three pipelines, so a variant has one id space rather than three.
     */
    std::vector<uint32_t> constants;

    /// Face culling for the opaque pair only -- the *shadow* pass ignores this and always
    /// draws both faces, to match the ray-traced path. See `createPipelines`.
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;

    /// How the *forward* pipeline composites; the opaque pair writes into four attachments
    /// nothing blends into, so this does not reach it.
    GraphicsPipelineDesc::Blend blend = GraphicsPipelineDesc::Blend::AlphaOver;
};

/// @brief Which of a variant's three pipelines a pass is asking for.
enum class VariantPass : uint32_t {
    GBuffer,
    Shadow,
    Forward,
};

/// Where a variant's own specialisation constants start. The engine reserves every id
/// below it in all three id spaces, so lowering this collides with an engine constant.
inline constexpr uint32_t kVariantConstantBase = 8;

} // namespace gfx
