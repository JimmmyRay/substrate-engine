#include "core/Resources.h"

#include "core/Logger.h"
#include "core/Paths.h"

namespace core {

// The two asset trees, absolute and baked at build time, as SUBSTRATE_SHADER_DIR is. A root
// resolved against the working directory would make a `res:/` name mean "wherever you were
// standing". They must stay in this .cpp: an absolute build-time path in a public header is
// what the out-of-tree check forbids.
#ifndef SUBSTRATE_ENGINE_ASSET_DIR
#define SUBSTRATE_ENGINE_ASSET_DIR "engine/assets"
#endif
// Empty when no game is configured. Defined on this translation unit alone -- on the target
// it would recompile the whole library on a `build_game.sh` toggle.
#ifndef SUBSTRATE_GAME_ASSET_DIR
#define SUBSTRATE_GAME_ASSET_DIR ""
#endif

namespace {

constexpr std::string_view kScheme = "res:";

/// A build-time root, anchored to the executable. `operator/` discards its left operand for
/// an absolute root, so a development build is unaffected and a package built with the
/// relative fallbacks finds its trees beside the binary.
///
/// The empty check is load-bearing: `path / ""` is *not* empty, and the constructor below
/// reads a non-empty `gameRoot` as "a game is configured", so every lookup would search the
/// executable's own directory first and match whatever sat there.
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

    // An empty name must stay an empty path: `render.debugFont` defaults to "" meaning "the
    // embedded bitmap font", and `Font::init` decides that by asking whether the path is
    // empty. Absolute-ising "" makes it try to read the working directory as a TTF.
    if (uri.empty()) return;

    const std::string_view name = schemeName(uri);
    if (name.empty()) {
        // No scheme: a filesystem path, used as given. `absolute` only prepends the working
        // directory, so the string still names the same file.
        std::error_code ec;
        resolved = fs::absolute(fs::path(uri), ec);
        if (ec) resolved = fs::path(uri);
        exists = fs::exists(resolved, ec);
        return;
    }

    // Game tree, then engine tree; the order is what lets a game override an engine asset.
    // The `gameRoot.empty()` guard is required: `empty() / name` is a bare relative name
    // that spuriously matches the working directory.
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

    // Which roots were searched is the one thing a caller's "cannot open <path>" cannot
    // say. The engine candidate is still returned, so the caller's own error handling runs
    // and names a file.
    Logger::warn(LogCategory::Asset, "res:/%.*s not found in \"%s\" or \"%s\"",
                 static_cast<int>(name.size()), name.data(),
                 gameRoot.empty() ? "<no game>" : gameRoot.string().c_str(), engineRoot.string().c_str());
    resolved = std::move(candidate);
    exists = false;
}

} // namespace core
