#pragma once

#include "gfx/Pipeline.h"

#include <string>
#include <vector>

namespace gfx {

/**
 * @file engine/gfx/ShaderVariant.h
 * @brief One material *behaviour*, as a set of shaders the renderer binds for it (G5).
 *
 * A game that wants a surface the standard glTF material cannot describe -- a hologram, a
 * force field, terrain blended from four layers, water -- writes GLSL for it, registers it
 * here with `Renderer::addShaderVariant`, and points a material's `shader` field at the
 * index that comes back. Nothing else changes: the geometry goes into the same buffers,
 * the same instance table, the same indirect commands and the same passes.
 *
 * **Not a material class hierarchy, and deliberately not a permutation cache.** The
 * renderer holds a flat array of these and creates each one's pipelines the first time a
 * draw actually needs one -- so a game may declare forty variants and pay for the two a
 * level uses. What is refused is the enumerating kind: nothing here takes a cross product
 * of feature flags, because nine booleans is 512 pipelines and that is how a variant
 * system turns into a compile farm.
 *
 * ## What a variant may change, and what it may not
 *
 * The three named shaders are the three ways one surface has to be drawn: into the
 * G-buffer when it is opaque, into a shadow map when it occludes, and over the lit scene
 * when it is blended. Each is optional and falls back to the engine's, so a variant that
 * only changes opaque shading names one file and inherits the rest.
 *
 * Everything else about the pipelines is the engine's: the attachments, the descriptor set
 * layouts, the depth state, the sample count. `engine/shaders/gbuffer_contract.glsl` is
 * that fixture written down, and a variant's GLSL includes it rather than restating it.
 * A variant that violates it does not render wrongly -- `Renderer::verifyShaderBindings`
 * reflects the SPIR-V and aborts in Debug naming the binding or the `constant_id` that
 * disagrees, which is the failure a game developer would otherwise meet as a black screen.
 */
struct ShaderVariant {
    /// For logs and for the object name a capture shows. Not an identity: the index
    /// `addShaderVariant` returns is what a material stores.
    std::string name;

    /// The opaque pair, drawn into the G-buffer. The vertex stage is overridable because
    /// a variant may need varyings the default does not compute; see the contract header
    /// for the five it must still write, and for why the shadow and velocity passes do
    /// *not* follow a vertex shader that moves geometry.
    std::string vertexShader = "gbuffer.vert";
    std::string fragmentShader = "gbuffer.frag";

    /// Drawn into the sun's cascade and into the punctual atlas. Fragment only -- the
    /// shadow pass has its own vertex stage, writing only the position it projects and
    /// the UV an alpha cutout needs. A variant that cuts its silhouette out differently
    /// (an animated dissolve, say) is what this is for.
    std::string shadowFragment = "shadow.frag";

    /// Drawn by the forward pass, for materials the loader marked ALPHA_MODE BLEND. Reads
    /// the same set 1 the G-buffer pair does but shades and composites in one go, because
    /// a blended surface never reaches the deferred pass.
    ///
    /// **`layout(set = 4, binding = 0) uniform sampler2D sceneDepth`** is the opaque depth
    /// behind the fragment, and is the one thing this pass offers that the G-buffer pair
    /// cannot: an intersection highlight, a soft particle or a water line is a comparison
    /// against what was already drawn. Reverse-Z, so `viewDistance()` in `frame.glsl` turns
    /// both it and `gl_FragCoord.z` into metres before they are subtracted.
    std::string forwardFragment = "forward.frag";

    /**
     * @brief The variant's own specialisation constants, supplied from `constant_id` 8.
     *
     * Ids 0..7 are the engine's -- `ENABLE_GSAA` in the G-buffer's id space, the seven
     * shading constants in the forward pass's -- so a variant declaring
     * `layout(constant_id = 8)` gets `constants[0]` and cannot collide with a constant the
     * engine adds later. Supplied to all three of its pipelines, since a constant nobody
     * reads costs nothing and two id spaces per variant would be one to get wrong.
     *
     * Empty is the common case, and the case that costs exactly what the engine's own
     * pipelines cost.
     */
    std::vector<uint32_t> constants;

    /// Face culling for the opaque pair. The engine's default is `BACK_BIT`, which makes
    /// the G-buffer pass 2.4x cheaper on correctly-wound geometry; `NONE` is what a
    /// single-sided sheet -- a leaf card, a banner -- needs. The *shadow* pass ignores
    /// this and always draws both faces, which is a parity decision with the ray-traced
    /// path rather than a quality one; see `createPipelines`.
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;

    /// How the *forward* pipeline composites. Meaningless for the opaque pair, which
    /// writes into four attachments nothing blends into. `AlphaOver` is the engine's
    /// blended surface; `Additive` is what a flame or a glow wants, and it is the reason
    /// this is a field rather than a constant.
    GraphicsPipelineDesc::Blend blend = GraphicsPipelineDesc::Blend::AlphaOver;
};

/**
 * @brief Which of a variant's three pipelines a pass is asking for.
 *
 * Passed to `Renderer::drawSceneIndirect` so one draw loop serves the G-buffer pass and
 * both shadow passes: what differs between them is which pipeline a variant resolves to,
 * and everything else about walking the command list is the same.
 */
enum class VariantPass : uint32_t {
    GBuffer,
    Shadow,
    Forward,
};

/// Where a variant's own specialisation constants start. The engine reserves the ids
/// below it in every id space a variant can be compiled into, so one number answers
/// "what do I write in `layout(constant_id = ...)`" for all three pipelines.
inline constexpr uint32_t kVariantConstantBase = 8;

} // namespace gfx
