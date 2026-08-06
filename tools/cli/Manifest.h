#pragma once

#include <rapidjson/fwd.h>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * @file tools/cli/Manifest.h
 * @brief The transitive closure of every file a packaged game needs.
 *
 * The pieces below are public because the unit suite links this translation unit and drives
 * them over temporary trees. `cmdManifest` is the command; everything else is what it is
 * made of, and what a test can reach without staging a whole game.
 */
namespace tool {

int cmdManifest(const std::vector<std::string>& args);

namespace manifest {

namespace fs = std::filesystem;

/// The first non-redistributable path component of `name`, or empty.
std::string restrictedPart(const std::string& name);

/// The JSON of a `.gltf` or the JSON chunk of a `.glb`. False if it is neither.
bool readGltf(const fs::path& path, rapidjson::Document& out);

/// Every file a glTF names, resolved against the document holding it.
///
/// Mirrors what the loader actually opens: fastgltf reads buffers and images relative to the
/// document, `GltfScene::ktx2CachePath` looks for a sibling `.ktx2`, and
/// `parseSceneAudioSources` reads `substrate_audio.file` the same way.
std::vector<fs::path> gltfReferences(const rapidjson::Document& doc, const fs::path& scene);

/// `res:/` names in the string literals of a C++ translation unit, comments and printf
/// format strings excluded.
std::set<std::string> resNamesInSource(const std::string& text);

/// `res:/` lookup, in the order `Resources::Resources` uses: game tree, then engine.
///
/// Also knows where each tree lands in a package. Those destinations mirror the source tree's
/// own shape rather than being flattened, because a glTF's references are relative to the
/// document and the composite scenes reach across into the other tree; the two have to stay
/// the same distance apart in the package as they are here.
struct Resolver {
    fs::path gameRoot;
    fs::path engineRoot;
    std::string gamePrefix = "game/demo/assets";
    std::string enginePrefix = "engine/assets";

    std::string prefix(const std::string& tree) const;

    /// False when neither tree holds the name.
    bool resolve(const std::string& name, fs::path& path, std::string& tree) const;

    std::string rootsDescription() const;

    /// (tree, destination within it) for a file on disk, or false if it is under neither.
    ///
    /// A document-relative reference is not confined to the tree its document was found in --
    /// the composite scenes graft props onto glTFs in the *other* tree, so they reach across
    /// with `../../../engine/assets/...`. The loader follows that happily; a package has to
    /// work out which root the file ends up under so it can be staged somewhere the same
    /// relative walk still lands on it.
    bool relocate(const fs::path& path, std::string& tree, std::string& dest) const;
};

struct Closure {
    /// Absolute source path to package-relative destination.
    std::map<fs::path, std::string> staged;
    std::vector<std::string> missing;
    std::set<std::string> restricted;
    std::set<std::string> cold;
};

/// Close over every seed.
///
/// `required` are names the packaged config asks for; one that is missing fails the build.
/// `optional` are source-literal defaults the packaged config overrides, so a missing one is
/// not an error -- the package never reaches it.
///
/// `cold` collects images whose `.ktx2` sidecar was never built. Empty unless `requireCache`,
/// because an absent sidecar is the normal case in a source tree and an error only in a
/// release: shipping one means shipping the decode path to someone who cannot rebuild it.
Closure build(const Resolver& resolver, const std::set<std::string>& required,
              const std::set<std::string>& optional, bool requireCache);

} // namespace manifest
} // namespace tool
