#include "core/Settings.h"

#include "gfx/Renderer.h"

/**
 * @file engine/gfx/SettingsBind.cpp
 * @brief Where each renderer-backed setting lives, and nothing else.
 *
 * These are **bindings**, not copies: after `bindRenderer` the table's value and the
 * renderer's field are the same storage, so `--dump-settings` reports the live frame. An
 * assignment written here instead would let the two disagree the moment anything toggled
 * a feature.
 *
 * Its own translation unit because `Settings.cpp` is in `SUBSTRATE_HOSTED_SOURCES`, which
 * pull in neither Vulkan nor a window -- putting this list anywhere in that set would take
 * the unit suite's ThreadSanitizer run with it. Nothing else belongs here.
 *
 * `render.msaaSamples` is **polled** at the end of `Engine::endFrame` rather than bound,
 * because `Renderer::setSampleCount` rebuilds render targets. A game calling that directly
 * leaves the table saying 4 while the image is 8, and the dump reports the table.
 */
namespace core::settings {

void bindRenderer(Settings& s, gfx::Renderer& r) {
    s.bindLive(Id::render_ssao, &r.ssaoEnabled);
    // World units. Both reach the shader as push constants, so nothing recomputes them
    // behind the write.
    s.bindLive(Id::render_ssaoRadius, &r.ssaoRadius);
    s.bindLive(Id::render_ssaoBias, &r.ssaoBias);
    // A specialisation constant, so flipping this rebuilds the lighting pipeline; the
    // tuning rows below reach the shader through the frame UBO instead.
    s.bindLive(Id::render_shadows, &r.shadowsEnabled);
    s.bindLive(Id::render_punctualShadows, &r.punctualShadowsEnabled);
    s.bindLive(Id::render_rtShadows, &r.rtShadowsEnabled);
    s.bindLive(Id::render_rtShadowMask, &r.rtShadowMaskEnabled);
    s.bindLive(Id::render_shadowCache, &r.shadowCacheEnabled);
    s.bindLive(Id::render_shadowDistance, &r.shadowDistance);
    s.bindLive(Id::render_lightCutoff, &r.lightCutoff);
    s.bindLive(Id::render_bloom, &r.bloomEnabled);
    // Push constants (and `bloomStrength` as `frame.params.w`), so no pipeline rebuild.
    s.bindLive(Id::render_bloomThreshold, &r.bloomThreshold);
    s.bindLive(Id::render_bloomSoftKnee, &r.bloomSoftKnee);
    s.bindLive(Id::render_bloomStrength, &r.bloomStrength);
    s.bindLive(Id::render_edgeMsaa, &r.edgeMsaaEnabled);
    s.bindLive(Id::render_culling, &r.cullingEnabled);
    s.bindLive(Id::render_occlusionCulling, &r.occlusionCullingEnabled);
    s.bindLive(Id::render_meshLod, &r.meshLodEnabled);
    s.bindLive(Id::render_lodThreshold, &r.meshLodThreshold);
    // Not through featureKey(): the shader reads the tile stride out of the frame UBO, so
    // flipping this needs no pipeline rebuild.
    s.bindLive(Id::render_lightTiles, &r.lightTilesEnabled);
    s.bindLive(Id::render_rt, &r.rtEnabled);
    s.bindLive(Id::render_ssr, &r.ssrEnabled);
    s.bindLive(Id::render_ssrRoughnessCutoff, &r.ssrRoughnessCutoff);
    s.bindLive(Id::render_ssrIntensity, &r.ssrIntensity);
    // Bound rather than polled despite resizing an image: the renderer already holds the
    // extent this produces and notices the change itself.
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
