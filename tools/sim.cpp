#include "core/Logger.h"
#include "scene/SceneData.h"
#include "scene/SceneParse.h"
#include "scene/Simulation.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

/**
 * @file tools/sim.cpp
 * @brief Step a scene with no Vulkan device and no window (C27).
 *
 * A dedicated server, a CI regression run on a GPU-less box and a batch tuning job all want
 * the same thing: the simulation, advanced, with nothing on screen. `--headless` is not that
 * and never claimed to be -- it unmaps the window and still creates a real surface against a
 * real driver, which is why CI can run the unit suite and not one frame of the engine.
 *
 * This links `SUBSTRATE_HOSTED_SOURCES` plus `scene/SceneParse.cpp`, so it holds no Vulkan
 * symbol at all. **The boundary is the linker's**, exactly as it is for `substrate-bake` and
 * the unit suite: a device dependency reaching `scene/Simulation.cpp` stops this target
 * linking rather than being caught in review.
 *
 * The step order is not repeated here. `scene::Simulation::step` is the same call the drawn
 * engine makes, which is the whole point of the row -- a headless loop with its own copy of
 * the order is a loop that will disagree with the rendered one on the frame it matters.
 */
namespace {

void usage() {
    std::fputs("substrate-sim -- step a scene with no device\n"
               "\n"
               "usage: substrate-sim <scene.gltf> [--steps N] [--gravity G] [--quiet]\n"
               "\n"
               "Builds a physics world from the colliders the document authored, advances it\n"
               "N fixed steps, and prints what it ended up with. No window, no driver, no\n"
               "shaders -- so this runs in a container and on a build machine.\n"
               "\n"
               "  --steps N     fixed steps to run. Default 600, which is ten seconds at 60 Hz\n"
               "  --gravity G   metres per second squared, negative Y. Default 9.81\n"
               "  --quiet       the summary only, with no per-body lines\n",
               stderr);
}

/// A number a CI run can diff. Positions rather than a hash of them, folded so that a body
/// that moved a millimetre changes it -- the point is to notice a divergence, not to identify
/// which body diverged, which the per-body lines are for.
double checksum(const scene::PhysicsWorld& physics) {
    double sum = 0.0;
    for (uint32_t slot = 0; slot < physics.bodyCount(); ++slot) {
        const scene::BodyId id = physics.bodyAt(slot);
        if (!id.valid()) continue;
        const glm::mat4 m = physics.bodyTransform(id, 0.0f);
        sum += static_cast<double>(m[3].x) + static_cast<double>(m[3].y) + static_cast<double>(m[3].z);
    }
    return sum;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path scenePath;
    uint32_t steps = 600;
    float gravity = 9.81f;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--steps" && i + 1 < argc) {
            steps = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--gravity" && i + 1 < argc) {
            gravity = std::strtof(argv[++i], nullptr);
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "substrate-sim: unknown option %s\n", arg.c_str());
            usage();
            return 2;
        } else if (scenePath.empty()) {
            scenePath = arg;
        } else {
            std::fputs("substrate-sim: more than one scene named\n", stderr);
            return 2;
        }
    }

    if (scenePath.empty()) {
        usage();
        return 2;
    }

    scene::SceneData data;
    scene::EmbeddedImages embedded;
    if (!scene::parseSceneData(scenePath, data, embedded)) {
        // parseSceneData has already said what was wrong with the document.
        return 1;
    }

    scene::Simulation sim;

    scene::PhysicsConfig cfg;
    cfg.gravity = glm::vec3(0.0f, -gravity, 0.0f);
    sim.physics.init(cfg, static_cast<uint32_t>(data.colliders.size()));

    // The collider walk, and it is **not** the engine's. `Engine::initPhysics` builds scene
    // nodes, binds audio sources to bodies and pairs rigs while it walks the same table; none
    // of that is reachable from a hosted translation unit today, so this is the short version
    // and the divergence is on its own card. What is shared is the thing that had to be:
    // the step below.
    uint32_t characters = 0;
    for (const scene::ColliderDesc& desc : data.colliders) {
        if (desc.motion == scene::ColliderMotion::Character) {
            if (sim.physics.createCharacter(desc).valid()) ++characters;
        } else {
            sim.physics.createBody(desc);
        }
    }
    sim.physics.finalize();

    core::Logger::status(core::LogCategory::Scene, "substrate-sim: %s -- %u bodies, %u characters, %u steps",
                         scenePath.filename().string().c_str(), sim.physics.bodyCount(), characters, steps);

    const float step = cfg.step;
    for (uint32_t i = 0; i < steps; ++i) sim.step(step);

    if (!quiet) {
        for (uint32_t slot = 0; slot < sim.physics.bodyCount(); ++slot) {
            const scene::BodyId id = sim.physics.bodyAt(slot);
            if (!id.valid()) continue;
            const glm::mat4 m = sim.physics.bodyTransform(id, 0.0f);
            std::printf("body %3u  %8.3f %8.3f %8.3f\n", slot, m[3].x, m[3].y, m[3].z);
        }
        for (uint32_t slot = 0; slot < sim.physics.characterCount(); ++slot) {
            const scene::PhysicsCharacterId id = sim.physics.characterAt(slot);
            if (!id.valid()) continue;
            const glm::mat4 m = sim.physics.characterTransform(id, 0.0f);
            std::printf("char %3u  %8.3f %8.3f %8.3f  %s\n", slot, m[3].x, m[3].y, m[3].z,
                        sim.physics.characterOnGround(id) ? "grounded" : "airborne");
        }
    }

    std::printf("steps %u  bodies %u  characters %u  checksum %.6f\n", steps, sim.physics.bodyCount(),
                sim.physics.characterCount(), checksum(sim.physics));
    return 0;
}
