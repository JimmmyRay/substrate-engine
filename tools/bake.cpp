#include "core/Logger.h"
#include "scene/MeshLod.h"
#include "scene/SceneData.h"
#include "scene/SceneParse.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @file tools/bake.cpp
 * @brief `substrate-bake` -- the offline writer of the C15 scene sidecar (D9).
 *
 * ## Why this is a program and not a flag
 *
 * The bake used to be `--bake-scene`, handled inside `GltfScene::loadCpu`, which meant two
 * things that D9 exists to stop. A packaged game shipped the writer and the flag that
 * reached it, so a player who typed it wrote a `.scene` into the install directory. And
 * baking meant launching the renderer: `build_release.sh` stood up a Vulkan device, a
 * swapchain and every texture upload to produce a CPU-side artifact that touches none of
 * them, so the one step of a package that has no use for a GPU was the step that could not
 * run without one.
 *
 * This links `SUBSTRATE_HOSTED_SOURCES` -- the translation units that pull in neither
 * Vulkan nor a window -- plus the two the runtime does not get: `scene/SceneParse.cpp`,
 * which is the CPU half of a load, and `scene/SceneCacheWrite.cpp`, which is the writer.
 * The boundary is the linker's, exactly as it is for the unit suite and for `game/`: a
 * device dependency reaching this file is a link error rather than a code review.
 *
 * ## What it does, in the order the runtime used to
 *
 * Parse the document, build C17's LOD chains, write the sidecar. Same functions, same
 * structs, same bytes -- the writer shares `Vertex` and `Primitive` with the reader, which
 * is C13's rule and the reason this is C++ rather than a Python script that would have to
 * encode the vertex layout a second time with no compiler to keep the two in step.
 *
 * ## Who runs it
 *
 * `build_release.sh`, once per scene the manifest resolves, after `scripts/ktx2.py` and
 * before `manifest.py --require-cache`. Nothing else, and deliberately not `build.sh`: the
 * development loop parses the document, which is what makes a stale sidecar impossible to
 * hide behind. C15's stamp is the belt -- editing a glTF invalidates its own cache without
 * anyone having to remember the cache exists -- and this tool is the braces.
 *
 * There is no `--force` and no up-to-date check, because the output is reproducible: no
 * duration measured by the run that baked survives into the file, so baking a scene twice
 * produces the same bytes and re-baking one that was already current costs time and
 * nothing else.
 */
namespace {

void usage() {
    std::fputs("substrate-bake -- write the scene sidecar a game reads at load time\n"
               "\n"
               "usage: substrate-bake [--quiet] <scene.gltf> [<scene.gltf> ...]\n"
               "\n"
               "Writes <scene>.scene beside each document: the parsed scene and its LOD\n"
               "chains, in the form scene::readSceneCache takes. Nothing rewrites the glTF,\n"
               "and deleting a sidecar restores the original behaviour exactly.\n"
               "\n"
               "  --quiet   only report failures\n"
               "\n"
               "build_release.sh runs this once per scene the manifest resolves. A\n"
               "development build parses the document instead, on purpose.\n",
               stderr);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> scenes;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "substrate-bake: unknown option '%s'\n\n", arg.c_str());
            usage();
            return 2;
        }
        scenes.emplace_back(arg);
    }

    if (scenes.empty()) {
        usage();
        return 2;
    }

    core::Logger::setLevel(quiet ? core::LogLevel::Warn : core::LogLevel::Status);

    size_t failed = 0;
    for (const std::filesystem::path& scene : scenes) {
        if (!std::filesystem::exists(scene)) {
            core::Logger::error(core::LogCategory::GLTF, "%s: no such file", scene.string().c_str());
            ++failed;
            continue;
        }

        scene::SceneData data;
        scene::EmbeddedImages embedded;
        if (!scene::parseSceneData(scene, data, embedded)) {
            // parseSceneData has already said what was wrong with the document.
            ++failed;
            continue;
        }

        // C17's chains are built here and nowhere else. Simplifying a scene is seconds of
        // work per launch whose answer never changes between them, which is precisely what
        // the sidecar exists to move offline. Ordered before the write, because the chain
        // is part of what gets written.
        const uint32_t chains = scene::buildLodChains(data);
        (void)chains;

        if (!scene::writeSceneCache(scene, data)) {
            // The embedded-image refusal says so itself, loudly, and it is the one failure
            // here a person can act on: run scripts/ktx2.py first.
            ++failed;
            continue;
        }
    }

    if (failed != 0) {
        std::fprintf(stderr, "substrate-bake: %zu of %zu scene(s) could not be baked\n", failed, scenes.size());
        return 1;
    }
    return 0;
}
