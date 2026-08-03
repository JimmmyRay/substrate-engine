#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace core {

/**
 * @file engine/core/Resources.h
 * @brief An asset named rather than located.
 *
 * Not to be confused with `engine/gfx/Resources.h`, which is GPU buffers and images.
 * That one is memory on a device; this one is a file on a disk, and the two never meet.
 *
 * There are two asset trees -- the engine's and the configured game's -- and before this
 * every path naming something in one of them spelled out which tree and how deep:
 * `game/demo/assets/character.gltf` in `substrate.json`,
 * `engine/assets/Sponza/glTF/Sponza.gltf` spelled out again by `run.sh`. A `res:/` name
 * says what the asset is and lets the lookup say where it lives:
 *
 *     Resources("res:/character.gltf")  ->  <abs>/game/demo/assets/character.gltf
 *     Resources("res:/Sponza/glTF/Sponza.gltf")
 *                                       ->  <abs>/engine/assets/Sponza/glTF/Sponza.gltf
 *
 * The game's tree is searched first, so a game can ship its own version of an engine
 * asset under the same name and get it, while every other game still gets the engine's.
 * That is the rule `readShaderBinary` already follows for GLSL, and it is deliberately
 * the same rule -- two trees with the game in front is now how this engine answers
 * "where does this come from".
 *
 * Both roots are absolute, so a `res:/` name resolves from any working directory -- which
 * is the point of naming a thing rather than locating it, and what shaders already had.
 *
 * **Anything without the scheme is passed through as a filesystem path**, and stays
 * relative to the working directory, because that is what a path means. Every string that
 * worked before this existed still works, which is what lets call sites move over one at
 * a time and what keeps `scripts/golden.sh` naming its scenes directly.
 *
 * **A glTF's own `uri` entries need nothing.** Buffers and images are relative to the
 * document, which is what the format specifies and what fastgltf and
 * `GltfScene::decodeImage` already do; resolving the document to an absolute path anchors
 * everything inside it for free.
 */
class Resources {
  public:
    /// Resolves against the trees this build was configured with.
    explicit Resources(std::string_view uri);

    /// Explicit roots. An empty `gameRoot` means no game is configured, which is the
    /// state `./build.sh` alone leaves the library in.
    Resources(std::string_view uri, const std::filesystem::path& gameRoot,
              const std::filesystem::path& engineRoot);

    /// Implicit, so a Resources drops into the signatures that already take a path
    /// instead of every call site growing a `.path()`. The reference is to a member, so
    /// it lives exactly as long as the Resources does: fine for
    /// `load(Resources("res:/main.gltf"))`, where the temporary outlives the call, and
    /// wrong if you bind it to a `const path&` that outlives the full expression.
    operator const std::filesystem::path&() const { return resolved; }

    [[nodiscard]] const std::filesystem::path& path() const { return resolved; }
    [[nodiscard]] std::string string() const { return resolved.string(); }

    /// False when neither tree held it. Deliberately not an error here: a missing scene,
    /// font or sound is each handled by the loader that wanted it, and each of those
    /// reports the failure better than this could.
    [[nodiscard]] bool found() const { return exists; }

  private:
    std::filesystem::path resolved;
    bool exists = false;
};

} // namespace core
