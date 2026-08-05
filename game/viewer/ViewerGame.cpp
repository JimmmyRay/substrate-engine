#include "ViewerGame.h"

#include "Engine.h"
#include "Entry.h"
#include "anim/AnimModule.h"
#include "audio/AudioModule.h"
#include "particles/ParticlesModule.h"

namespace {

void placeLights(gfx::Renderer& renderer, const glm::vec3& boundsMin, const glm::vec3& boundsMax) {
    const glm::vec3 centre = (boundsMin + boundsMax) * 0.5f;
    const glm::vec3 extent = boundsMax - boundsMin;
    const float radius = glm::length(extent) * 0.35f;
    const float height = boundsMin.y + extent.y * 0.25f;

    // Scaled to the sun and never to a constant of its own: each fill delivers about a third of
    // the sun's irradiance at half its reach, and an interior lit at three times key by its fill
    // has no shadows left to read. The distance squared is the inverse-square falloff, stated at
    // a distance that scales with the scene so a bigger one keeps the same ratio.
    const float fillDistance = radius * 0.5f;
    const float intensity = renderer.sunIntensity * 0.35f * fillDistance * fillDistance;
    const glm::vec3 warm(1.0f, 0.72f, 0.42f);
    const glm::vec3 pale(1.0f, 0.85f, 0.65f);

    renderer.lights = {
        gfx::makePointLight({centre.x, height, centre.z}, radius, pale, intensity),
        gfx::makePointLight({centre.x - extent.x * 0.25f, height, centre.z}, radius, warm, intensity),
        gfx::makePointLight({centre.x + extent.x * 0.25f, height, centre.z}, radius, warm, intensity),
    };

    // The only spot in this set: take it out and nothing exercises the spot path.
    renderer.lights.push_back(gfx::makeSpotLight({centre.x, boundsMin.y + extent.y * 0.8f, centre.z},
                                                 {0.0f, -1.0f, 0.15f}, radius * 1.5f, glm::radians(18.0f),
                                                 glm::radians(32.0f), pale, intensity * 2.5f));
}

/**
 * @brief Name every module the golden set runs through, which is what links them.
 *
 * **An include links nothing; calling the accessor is the undefined symbol that pulls the
 * archive member.** Every failure this prevents is silent and none of them fails a build:
 * emitters spawning into a pool of zero, `configure`'s buses never created under
 * `--audio-null`, a skinned mesh held in its bind pose. Every golden case is a viewer run,
 * so a line deleted here is a reference image that renders something else.
 */
void linkModules(Engine& e) {
    (void)e.animator();
    (void)e.audio();
    (void)e.particles();
}

} // namespace

void ViewerGame::configure(GameSetup& setup, core::settings::Settings& /*settings*/) {
    setup.name = "Substrate Viewer";

    // The golden baselines were captured through these three values. Changing one moves
    // thirteen images.
    setup.look.lights = {gfx::makeDirectionalLight({-0.35f, 0.85f, 0.4f}, {1.0f, 0.96f, 0.88f}, 3.0f)};
    setup.look.ambientColor = {0.0025f, 0.0021f, 0.0016f};
    setup.look.exposure = 1.0f;

    setup.audio.buses = {
        {"music", 1.0f, "sfx", 0.45f, 0.05f, 0.4f},
        {"sfx", 1.0f, "", 1.0f, 0.05f, 0.4f},
        {"ambience", 1.0f, "sfx", 0.7f, 0.05f, 0.4f},
    };
}

void ViewerGame::init(Engine& e) {
    quit = e.input().declare("App.Quit", "Escape");

    // **The pose is not taken from `e.camera()` and must not be.** `Engine::run` frames the
    // loaded scene into whichever camera is active after this returns, so a pose read here is
    // the default one.
    flyCamera.applySettings(e.settingsTable());
    e.setCamera(&flyCamera);

    if (e.gltfScene().lights().empty()) {
        placeLights(e.renderer(), e.gltfScene().boundsMin, e.gltfScene().boundsMax);
    }

    linkModules(e);
}

void ViewerGame::frameUpdate(Engine& e, float /*dt*/) {
    if (e.input().pressed(quit)) e.requestQuit();
}

void ViewerGame::shutdown(Engine& e) {
    // The engine holds a non-owning pointer to a member that is about to go away.
    e.setCamera(nullptr);
}

SUBSTRATE_GAME(ViewerGame)
