/**
 * Shader feature constants for the shading paths.
 *
 * Specialisation constants, not uniforms. The driver folds each one at pipeline
 * creation and dead-strips the branch behind it, so a disabled feature costs nothing
 * at runtime -- no fetch, no branch, and none of the registers its live version would
 * have held. A uniform flag cannot do that: the code is still there, still allocating
 * registers, and on a divergent path still executed.
 *
 * Three things this buys beyond the obvious saving:
 *
 * - A per-feature A/B without editing a shader, which is how a per-pass cost gets
 *   *attributed* rather than merely measured. F7..F10 flip these live.
 * - The mechanism roughness clamping (1.9) and edge-detect hybrid MSAA (3.6) both
 *   want, since each is a variant of an existing shader rather than a new one.
 * - How an optional *hardware* capability gets handled. When ray query lands (3.9),
 *   a device without it simply gets the variant where ENABLE_RT_* are false, rather
 *   than no build at all.
 *
 * ## The id space
 *
 * The index into `GraphicsPipelineDesc::constants` *is* the `constant_id`, so these
 * ids and that vector's order are the same fact written twice. In Debug they are
 * checked against each other: `Renderer::verifyShaderBindings` reflects every module
 * and aborts if it declares a `SpecId` the pipeline does not supply a value for.
 *
 * This file is one id space, shared by lighting.frag, lighting1x.frag and forward.frag
 * because they gate the same features. gbuffer.frag and tonemap.frag declare their own
 * constants locally: they have one and two respectively, and a shared header for that
 * would be indirection rather than sharing.
 *
 * ## What earns a constant
 *
 * A feature that is genuinely optional *and* whose branch has a measured cost. Where
 * a flag is neither, a uniform branch is cheaper than a second variant. Where several
 * flags are mutually exclusive, one non-boolean constant beats several booleans --
 * see TONEMAP_OPERATOR in tonemap.frag, which is three flags' worth of states in one
 * value, and one variant per operator instead of eight.
 */

/// Baked in so the per-sample resolve loop unrolls. Not a feature toggle: it is the
/// G-buffer's actual sample count, and a variant per MSAA level already existed.
layout(constant_id = 0) const uint SAMPLE_COUNT = 1;

/// Screen-space ambient occlusion. Gates the *read*: with the pass off the buffer is
/// never written, so sampling it would fold undefined data into the ambient term.
layout(constant_id = 1) const bool ENABLE_SSAO = true;

/// Id 2 held ENABLE_IBL and is vacant. The environment term it gated was removed with
/// the split-sum lookup -- the lighting pass has no image-based term left to switch off,
/// and a constant whose only `if` had gone kept a command-line flag, a settings row, an
/// F9 binding and a golden case alive for a feature that no longer existed. Vacant rather
/// than renumbered, for the reason ibl.glsl's bindings 0 and 1 are vacant: the ids below
/// are the same fact as `GraphicsPipelineDesc::constants`' order, and closing a gap moves
/// every one of them. Whatever brings indirect light back declares its own id here.

/// Edge-detect hybrid MSAA (3.6). Shade once and broadcast wherever every sample in
/// the pixel carries the same G-buffer fragment, and per-sample only where they do
/// not. Does nothing at SAMPLE_COUNT 1, where there is one sample to shade either way.
layout(constant_id = 3) const bool ENABLE_EDGE_MSAA = true;

/// Cascaded shadow maps for the sun.
///
/// The *non-traced* path, and only that. Where `render.rt` is on the lighting pass is
/// compiled from lighting_rt.frag, which traces a ray per light and never reads this --
/// ray tracing is one switch covering shadows and reflections together, so there is no
/// combination in which a surface is traced and its own reflection is not.
///
/// This is what a device without VK_KHR_ray_query gets, and what `--no-rt` selects on one
/// that has it. Cheaper than tracing and worse: cascade seams, a depth bias to tune, and
/// contact shadows only as good as the texel that carries them.
layout(constant_id = 4) const bool ENABLE_SHADOWS = true;

/**
 * Shadows for point and spot lights, from the atlas at set 2 binding 1. Follows
 * updateLights(): it assigns layers only when both this and render.shadows are on, so a
 * constant saying otherwise would have the shader reading layers nothing ever wrote.
 *
 * Worth its own constant rather than sharing ENABLE_SHADOWS because the costs are not
 * comparable -- the sun is one lookup, this is one per punctual light that reaches the
 * pixel -- and because an interior lit only by punctual lights has all of its shadows
 * here and none in the sun's map.
 */
layout(constant_id = 5) const bool ENABLE_PUNCTUAL_SHADOWS = true;

/**
 * Shadow rays in the traced lighting pass -- the counterpart of the two above, read only
 * by the ray-query variants.
 *
 * A measurement toggle before it is a feature one, and that is the honest description.
 * `render.rt` selects a shader *file*, so switching it measures the raster path against
 * the traced one rather than saying what the rays inside the traced path cost. This gates
 * just the `lightShadow` call, so the delta across it is the shadow-ray cost alone.
 *
 * Deliberately *not* read by shadeRayHit in raytrace.glsl, which the SSR pipelines compile
 * and which is not supplied this id space. Reflection-hit rays are a separate cost in a
 * separate zone, and `render.ssr` already switches them off.
 */
layout(constant_id = 6) const bool ENABLE_RT_SHADOWS = true;

/**
 * Read the traced shadow bits shadowmask.frag left for this sample instead of tracing
 * them again -- the ENABLE_SSAO pattern, gating *the read* rather than the binding,
 * because with the pass off the mask holds whatever the last frame's contents were.
 *
 * Only the per-sample half of the resolve reads it. A pixel whose samples agree collapses
 * to one shading evaluation and one ray per light either way, so the mask covers only the
 * pixels that shade per sample -- which is also the only place a saving exists.
 *
 * The renderer supplies false at 1x, where there is one sample and nothing to share a ray
 * between, and wherever `render.rt` or `render.rtShadows` is off, where there are no rays.
 */
layout(constant_id = 7) const bool ENABLE_SHADOW_MASK = true;
