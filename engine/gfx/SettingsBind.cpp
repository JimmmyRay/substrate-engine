#include "core/Settings.h"

#include "gfx/Renderer.h"

/**
 * @file engine/gfx/SettingsBind.cpp
 * @brief Where each renderer-backed setting lives, and nothing else (S4.2, G2).
 *
 * This file is the deletion of the 34 `renderer.x = configData.render.x` assignments that
 * used to sit in `Engine::initRenderer`. It looks like the same list, and the difference
 * is the one that matters: those were *copies*, made once at startup, after which the
 * config said one thing and the renderer held another the moment anything toggled a
 * feature. These are **bindings**. After `bindRenderer`, the table's value and the
 * renderer's field are the same storage -- `settings.get(core::options::render::ssao)` reads what
 * F8 last set, and `--dump-settings` reports the live frame rather than the startup one.
 *
 * It is its own translation unit because `Settings.cpp` is in `SUBSTRATE_HOSTED_SOURCES`,
 * the sources that pull in neither Vulkan nor a window, which is what lets the unit suite
 * run under ThreadSanitizer at all. Nothing else belongs here.
 *
 * ## What is deliberately *not* bound
 *
 * **One** row has no plain field to point at, and it did not get machinery for it. It is
 * **polled instead**, by one line at the end of `Engine::endFrame`, which is the same
 * refresh mechanism `featureKey()` already is -- compared once a frame, no dispatch, and
 * nothing to subscribe to: `render.msaaSamples`, because `Renderer::setSampleCount`
 * rebuilds render targets, and it returns early unless the clamped count actually differs.
 *
 * It was applied by hand at startup until the generated panel arrived, and the panel is
 * what made that untenable: a slider over a row nothing applies moves, reports success and
 * changes nothing, which is the failure this whole table exists to remove. A game may
 * still call `setSampleCount` directly, and should not -- the table would then say 4 while
 * the image was 8, and the dump would report the table.
 *
 * `render.tonemap` was the second, and D14 removed it rather than polled it: a curve is a
 * look decision the game's author made, so it is `GameSetup::look.tonemap` with `--tonemap` as
 * the per-invocation override, and `Engine::initRenderer` writes it once. Four more rows
 * left this file with the same card -- `render.debugOverlay`, `shadowDepthBias`,
 * `shadowNormalBias` and `lightBudget`. The first is a developer readout behind
 * `--overlay`; the other three are authored, and `GameSetup` carries them.
 */
namespace core::settings {

void bindRenderer(Settings& s, gfx::Renderer& r) {
    s.bindLive(Id::render_ssao, &r.ssaoEnabled);
    // D11. Two world-unit lengths that had no row at all until that card, so the only way
    // to correct a hemisphere sized for a 30 m interior was to recompile the engine. Both
    // reach the shader as push constants, so both take effect on the next frame and
    // nothing recomputes them behind the write -- which is the property that makes them
    // rows rather than a switch that reverts.
    s.bindLive(Id::render_ssaoRadius, &r.ssaoRadius);
    s.bindLive(Id::render_ssaoBias, &r.ssaoBias);
    // Only `render.shadows` goes through featureKey() -- it is a specialisation constant,
    // so flipping it rebuilds the lighting pipeline. The three tuning rows below reach the
    // shader through the frame UBO and take effect on the next frame.
    s.bindLive(Id::render_shadows, &r.shadowsEnabled);
    s.bindLive(Id::render_punctualShadows, &r.punctualShadowsEnabled);
    s.bindLive(Id::render_rtShadows, &r.rtShadowsEnabled);
    s.bindLive(Id::render_rtShadowMask, &r.rtShadowMaskEnabled);
    s.bindLive(Id::render_shadowCache, &r.shadowCacheEnabled);
    s.bindLive(Id::render_shadowDistance, &r.shadowDistance);
    s.bindLive(Id::render_lightCutoff, &r.lightCutoff);
    s.bindLive(Id::render_bloom, &r.bloomEnabled);
    // All three reach the GPU as push constants (and bloomStrength as frame.params.w),
    // so none of them needs a pipeline rebuild -- live in the true sense, unlike the
    // feature toggles above that go through featureKey().
    s.bindLive(Id::render_bloomThreshold, &r.bloomThreshold);
    s.bindLive(Id::render_bloomSoftKnee, &r.bloomSoftKnee);
    s.bindLive(Id::render_bloomStrength, &r.bloomStrength);
    s.bindLive(Id::render_edgeMsaa, &r.edgeMsaaEnabled);
    s.bindLive(Id::render_culling, &r.cullingEnabled);
    s.bindLive(Id::render_occlusionCulling, &r.occlusionCullingEnabled);
    s.bindLive(Id::render_meshLod, &r.meshLodEnabled);
    s.bindLive(Id::render_lodThreshold, &r.meshLodThreshold);
    // Live like the three culling rows above it, and not through featureKey(): the shader
    // reads the tile stride out of the frame UBO rather than a specialisation constant, so
    // flipping this costs the next frame and no pipeline rebuild.
    s.bindLive(Id::render_lightTiles, &r.lightTilesEnabled);
    s.bindLive(Id::render_rt, &r.rtEnabled);
    s.bindLive(Id::render_ssr, &r.ssrEnabled);
    s.bindLive(Id::render_ssrRoughnessCutoff, &r.ssrRoughnessCutoff);
    s.bindLive(Id::render_ssrIntensity, &r.ssrIntensity);
    // Bound like the rest even though it resizes an image, which `render.msaaSamples`
    // above could not be. The difference is where the comparison lives: the renderer
    // already holds the extent this produces, so it can notice the change itself in the
    // frame, and nothing outside has to poll for it.
    s.bindLive(Id::render_ssrScale, &r.ssrScale);
    s.bindLive(Id::render_ssrMaxDistance, &r.ssrMaxDistance);
    s.bindLive(Id::render_ssrThickness, &r.ssrThickness);
    s.bindLive(Id::render_rtMaxDistance, &r.rtMaxDistance);
    s.bindLive(Id::render_fog, &r.fogEnabled);
    s.bindLive(Id::render_fogDensity, &r.fogDensity);
    s.bindLive(Id::render_fogAnisotropy, &r.fogAnisotropy);
    s.bindLive(Id::render_fogMaxDistance, &r.fogMaxDistance);
    s.bindLive(Id::render_fogHeightFalloff, &r.fogHeightFalloff);
    s.bindLive(Id::render_particles, &r.particlesEnabled);
    s.bindLive(Id::render_particleSort, &r.particleSortEnabled);
    s.bindLive(Id::render_taa, &r.taaEnabled);
    s.bindLive(Id::render_taaBlend, &r.taaBlend);
}

} // namespace core::settings
