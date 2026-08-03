#include "core/Resources.h"

#include "core/Logger.h"
#include "core/Paths.h"

namespace core {

// Where the two asset trees are. **Absolute**, baked at build time, exactly as
// SUBSTRATE_SHADER_DIR is -- and for the reason the whole scheme exists: a `res:/` name
// says what an asset is instead of where it is, and a root resolved against the working
// directory would put "where" straight back, as "the directory you happened to be standing
// in". Shaders already loaded from any cwd; assets do too.
//
// The out-of-tree delegation keeps "no absolute build-time path baked into a *public
// header*" as a checked property, and it still holds: these appear only here, in a .cpp,
// each behind a relative fallback -- the same shape SUBSTRATE_SHADER_DIR has.
//
// A packaged build configures the fallbacks instead, and `anchored()` below is what makes
// that work: it puts executableDir() in front, which an absolute root discards and a
// relative one does not. So the tradeoff the shader paths took -- a source tree moved
// after the build loses its assets -- still applies to a development build and does not
// apply to a package, where the assets ship beside the binary.
#ifndef SUBSTRATE_ENGINE_ASSET_DIR
#define SUBSTRATE_ENGINE_ASSET_DIR "engine/assets"
#endif
// Empty when no game is configured. Set on this translation unit alone, because it is the
// only path here that varies with the configured game and a define on the target would
// recompile the whole library on a `build_game.sh` toggle.
#ifndef SUBSTRATE_GAME_ASSET_DIR
#define SUBSTRATE_GAME_ASSET_DIR ""
#endif

namespace {

constexpr std::string_view kScheme = "res:";

/// A build-time root, anchored to the executable. Absolute roots come back unchanged --
/// that is `operator/` discarding its left operand -- so a development build resolves
/// exactly as it did before this existed, and a package built with the relative fallbacks
/// finds its trees beside the binary.
///
/// The empty check is load-bearing rather than an optimisation: an unconfigured game
/// leaves SUBSTRATE_GAME_ASSET_DIR empty, `path / ""` is *not* empty, and the constructor
/// below reads a non-empty gameRoot as "a game is configured" -- which would have every
/// lookup search the executable's own directory first and match whatever happened to sit
/// there.
std::filesystem::path anchored(const char* root) {
    if (root == nullptr || *root == '\0') return {};
    return executableDir() / root;
}

/// The name after `res:`, with leading slashes dropped so `res:/x` and `res://x` are the
/// same asset. Empty view means the string carried no scheme at all.
std::string_view schemeName(std::string_view uri) {
    if (uri.substr(0, kScheme.size()) != kScheme) return {};
    uri.remove_prefix(kScheme.size());
    while (!uri.empty() && (uri.front() == '/' || uri.front() == '\\')) uri.remove_prefix(1);
    return uri;
}

}  // namespace

Resources::Resources(std::string_view uri)
    : Resources(uri, anchored(SUBSTRATE_GAME_ASSET_DIR), anchored(SUBSTRATE_ENGINE_ASSET_DIR)) {}

Resources::Resources(std::string_view uri, const std::filesystem::path& gameRoot,
                     const std::filesystem::path& engineRoot) {
    namespace fs = std::filesystem;

    // An empty name stays an empty path, and this is load-bearing rather than tidiness:
    // `render.debugFont` defaults to "" meaning "use the embedded bitmap font", and
    // Font::init decides that by asking whether the path is empty. Absolute-ising "" into
    // the working directory would have it try to read a directory as a TTF.
    if (uri.empty()) return;

    const std::string_view name = schemeName(uri);
    if (name.empty()) {
        // No scheme: a filesystem path, used as given. `absolute` only prepends the
        // working directory, so what the string means does not change -- an absolute path
        // is returned untouched and a relative one still names the same file.
        std::error_code ec;
        resolved = fs::absolute(fs::path(uri), ec);
        if (ec) resolved = fs::path(uri);
        exists = fs::exists(resolved, ec);
        return;
    }

    // Game tree, then engine tree. Two roots are two checks rather than a list and a
    // loop; a third tree is what would make this worth generalising, and there is no
    // third tree. An unconfigured game leaves gameRoot empty, and `empty() / name` would
    // otherwise produce a bare relative name that spuriously matches the working
    // directory -- hence the guard rather than a root that happens to be "".
    std::error_code ec;
    if (!gameRoot.empty()) {
        if (fs::path candidate = fs::absolute(gameRoot / name, ec); !ec && fs::exists(candidate, ec)) {
            resolved = std::move(candidate);
            exists = true;
            return;
        }
    }

    fs::path candidate = fs::absolute(engineRoot / name, ec);
    if (ec) candidate = engineRoot / name;
    if (fs::exists(candidate, ec)) {
        resolved = std::move(candidate);
        exists = true;
        return;
    }

    // Logged here and nowhere else: which roots were searched is the one thing a caller's
    // "cannot open <path>" cannot tell you. The engine candidate is still returned, so
    // that caller's own error handling runs and names a file.
    Logger::warn(LogCategory::Asset, "res:/%.*s not found in \"%s\" or \"%s\"",
                 static_cast<int>(name.size()), name.data(),
                 gameRoot.empty() ? "<no game>" : gameRoot.string().c_str(), engineRoot.string().c_str());
    resolved = std::move(candidate);
    exists = false;
}

} // namespace core
