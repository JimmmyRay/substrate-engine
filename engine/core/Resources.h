#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace core {

/**
 * @file engine/core/Resources.h
 * @brief An asset named rather than located. Unrelated to `engine/gfx/Resources.h`, which
 * is GPU buffers and images.
 *
 *     Resources("res:/character.gltf")  ->  <abs>/game/demo/assets/character.gltf
 *     Resources("res:/Sponza/glTF/Sponza.gltf")
 *                                       ->  <abs>/engine/assets/Sponza/glTF/Sponza.gltf
 *
 * The game's tree is searched *first*, so a game shipping its own version of an engine
 * asset under the same name gets it -- the rule `readShaderBinary` follows for GLSL.
 *
 * Both roots are absolute, so a `res:/` name resolves from any working directory. Anything
 * without the scheme is passed through unchanged and stays relative to the working
 * directory, which is what keeps `scripts/golden.sh` naming its scenes directly.
 */
class Resources {
  public:
    /// Resolves against the trees this build was configured with.
    explicit Resources(std::string_view uri);

    /// Explicit roots. An empty `gameRoot` means no game is configured, which is the
    /// state `scripts/build.sh` alone leaves the library in.
    Resources(std::string_view uri, const std::filesystem::path& gameRoot,
              const std::filesystem::path& engineRoot);

    /// Implicit, so a `Resources` drops into a signature taking a path. The reference is to
    /// a member and lives exactly as long as the `Resources` does: binding it to a
    /// `const path&` that outlives the full expression dangles.
    operator const std::filesystem::path&() const { return resolved; }

    [[nodiscard]] const std::filesystem::path& path() const { return resolved; }
    [[nodiscard]] std::string string() const { return resolved.string(); }

    /// False when neither tree held it. Not an error here: the loader that wanted the asset
    /// reports the failure with the context this has none of.
    [[nodiscard]] bool found() const { return exists; }

  private:
    std::filesystem::path resolved;
    bool exists = false;
};

} // namespace core
