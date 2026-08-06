#include "Manifest.h"

#include "Repo.h"

#include <rapidjson/document.h>

#include <cstdint>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// Content this repository does not own and cannot redistribute. Sponza is (c) Crytek under
/// the CryEngine Limited License Agreement, which is incompatible with Apache-2.0.
///
/// Packaged by default and merely reported, because a local build is not distribution.
/// `--strict` turns every one into an error, and is what a build that goes out to other
/// people should pass -- the point of tracking it at all is that the day the answer changes,
/// the list already exists instead of having to be reconstructed.
const char* const kRestricted[] = {"Sponza"};

/// glTF URIs are percent-encoded. Spaces as %20 are the case that actually occurs.
std::string unquote(const std::string& uri) {
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
            continue;
        }
        out += uri[i];
    }
    return out;
}

} // namespace

namespace manifest {

/// `Logger::warn(..., "res:/%.*s not found in ...")` in Resources.cpp is a format string, not
/// an asset. Anything carrying a printf conversion is the engine talking about a name rather
/// than naming one.
bool looksLikeFormat(const std::string& name) {
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '%') continue;
        size_t j = i + 1;
        while (j < name.size() && std::strchr("-+ #0123456789.*", name[j])) ++j;
        if (j < name.size() && std::isalpha(static_cast<unsigned char>(name[j]))) return true;
    }
    return false;
}

/// Every `res:/<name>` in `text`. The name runs to the first quote, backslash or space.
void collectResNames(const std::string& text, std::set<std::string>& into) {
    for (size_t at = text.find("res:/"); at != std::string::npos; at = text.find("res:/", at + 1)) {
        size_t i = at + 4;
        while (i < text.size() && text[i] == '/') ++i;
        const size_t start = i;
        while (i < text.size() && text[i] != '"' && text[i] != '\'' && text[i] != '\\' &&
               !std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i > start) into.insert(text.substr(start, i - start));
        at = i > at ? i - 1 : at;
    }
}

/// Every double-quoted literal in a C++ translation unit, comments excluded.
///
/// Both halves matter and neither is optional. Scanning raw text finds the examples in
/// Resources.h's own file comment -- `Resources("res:/showcase.gltf")` is prose about how the
/// scheme works, and treating it as a dependency had this demanding a scene the package never
/// asks for. Scanning quotes without skipping comments finds them too, because the examples
/// are quoted. Skipping comments without tracking strings would cut a literal containing `//`
/// in half.
std::vector<std::string> stringLiterals(const std::string& text) {
    std::vector<std::string> out;
    const size_t n = text.size();

    for (size_t i = 0; i < n;) {
        if (text[i] == '"') {
            std::string buffer;
            size_t j = i + 1;
            while (j < n && text[j] != '"') {
                if (text[j] == '\\') {
                    buffer.append(text, j, 2);
                    j += 2;
                    continue;
                }
                if (text[j] == '\n') break; // unterminated; not ours to diagnose
                buffer += text[j];
                ++j;
            }
            out.push_back(buffer);
            i = j + 1;
        } else if (text.compare(i, 2, "//") == 0) {
            const size_t end = text.find('\n', i);
            if (end == std::string::npos) break;
            i = end;
        } else if (text.compare(i, 2, "/*") == 0) {
            const size_t end = text.find("*/", i);
            if (end == std::string::npos) break;
            i = end + 2;
        } else if (text[i] == '\'') {
            i += text.compare(i + 1, 1, "\\") == 0 ? 4 : 3;
        } else {
            ++i;
        }
    }
    return out;
}

std::set<std::string> resNamesInSource(const std::string& text) {
    std::set<std::string> names;
    for (const std::string& literal : stringLiterals(text)) {
        std::set<std::string> found;
        collectResNames(literal, found);
        for (const std::string& name : found) {
            if (!looksLikeFormat(name)) names.insert(name);
        }
    }
    return names;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// `res:/` names appearing in string literals in source files under `roots`.
std::set<std::string> scanLiterals(const std::vector<fs::path>& roots) {
    std::set<std::string> names;
    std::error_code ec;

    for (const fs::path& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".c") continue;

            for (const std::string& literal : stringLiterals(readFile(it->path()))) {
                std::set<std::string> found;
                collectResNames(literal, found);
                for (const std::string& name : found) {
                    if (!looksLikeFormat(name)) names.insert(name);
                }
            }
        }
    }
    return names;
}

/// Every `res:/` name anywhere in a decoded JSON document.
///
/// Walks the whole tree rather than reading `scene.path` and the rest by name: those are
/// simply the fields that use the scheme today, and a config that grows a fourth should not
/// need this edited to notice.
void scanJson(const rapidjson::Value& value, std::set<std::string>& into) {
    if (value.IsString()) {
        collectResNames(value.GetString(), into);
    } else if (value.IsObject()) {
        for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
            scanJson(it->value, into);
        }
    } else if (value.IsArray()) {
        for (const rapidjson::Value& item : value.GetArray()) scanJson(item, into);
    }
}

/// The JSON of a `.gltf` or the JSON chunk of a `.glb`. False if it is neither.
bool readGltf(const fs::path& path, rapidjson::Document& out) {
    const std::string data = readFile(path);
    if (data.size() >= 4 && data.compare(0, 4, "glTF") == 0) {
        // 12-byte header, then chunks of (length, type, payload). The first chunk is JSON.
        if (data.size() < 20) return false;
        uint32_t length = 0;
        uint32_t kind = 0;
        std::memcpy(&length, data.data() + 12, 4);
        std::memcpy(&kind, data.data() + 16, 4);
        if (kind != 0x4E4F534A) return false; // 'JSON'
        if (20 + static_cast<size_t>(length) > data.size()) return false;
        out.Parse(data.data() + 20, length);
        return !out.HasParseError();
    }
    out.Parse(data.c_str());
    return !out.HasParseError();
}

/// Every file a glTF names, resolved against the document holding it.
///
/// Mirrors what the loader actually opens: fastgltf reads buffers and images relative to the
/// document, `GltfScene::ktx2CachePath` looks for a sibling `.ktx2`, and
/// `parseSceneAudioSources` reads `substrate_audio.file` the same way.
std::vector<fs::path> gltfReferences(const rapidjson::Document& doc, const fs::path& scene) {
    const fs::path base = scene.parent_path();
    std::vector<fs::path> out;

    for (const char* kind : {"buffers", "images"}) {
        if (!doc.HasMember(kind) || !doc[kind].IsArray()) continue;
        const auto items = doc[kind].GetArray();
        for (rapidjson::SizeType i = 0; i < items.Size(); ++i) {
            const bool isImage = std::strcmp(kind, "images") == 0;
            if (!items[i].IsObject() || !items[i].HasMember("uri") || !items[i]["uri"].IsString()) {
                // Embedded in a buffer view. Nothing to stage for the payload, but the
                // texture cache still gets a name -- ktx2CachePath falls back to
                // `<stem>.image<N>.ktx2` exactly so an embedded image can have one.
                if (isImage) {
                    out.push_back(base / (scene.stem().string() + ".image" +
                                          std::to_string(i) + ".ktx2"));
                }
                continue;
            }
            const std::string uri = items[i]["uri"].GetString();
            if (uri.rfind("data:", 0) == 0) continue;

            out.push_back(base / unquote(uri));
            if (isImage) out.push_back(base / (unquote(uri) + ".ktx2"));
        }
    }

    // The scene sidecar, beside the document and named for the whole of it. Absent is the
    // normal case in a source tree, exactly as a `.ktx2` is.
    out.push_back(base / (scene.filename().string() + ".scene"));

    // Audio lives in node extras, not in a glTF-standard array.
    if (doc.HasMember("nodes") && doc["nodes"].IsArray()) {
        for (const rapidjson::Value& node : doc["nodes"].GetArray()) {
            if (!node.IsObject() || !node.HasMember("extras")) continue;
            const rapidjson::Value& extras = node["extras"];
            if (!extras.IsObject() || !extras.HasMember("substrate_audio")) continue;
            const rapidjson::Value& audio = extras["substrate_audio"];
            if (audio.IsObject() && audio.HasMember("file") && audio["file"].IsString()) {
                out.push_back(base / unquote(audio["file"].GetString()));
            }
        }
    }
    return out;
}

/// `res:/` lookup, in the order `Resources::Resources` uses: game tree, then engine.
///
/// Also knows where each tree lands in a package. Those destinations mirror the source tree's
/// own shape rather than being flattened, because a glTF's references are relative to the
/// document and the composite scenes reach across into the other tree; the two have to stay
/// the same distance apart in the package as they are here.
std::string Resolver::prefix(const std::string& tree) const {
    return tree == "game" ? gamePrefix : enginePrefix;
}

bool Resolver::resolve(const std::string& name, fs::path& path, std::string& tree) const {
    std::error_code ec;
    if (!gameRoot.empty() && fs::exists(gameRoot / name, ec)) {
        path = gameRoot / name;
        tree = "game";
        return true;
    }
    if (fs::exists(engineRoot / name, ec)) {
        path = engineRoot / name;
        tree = "engine";
        return true;
    }
    return false;
}

std::string Resolver::rootsDescription() const {
    return "\"" + (gameRoot.empty() ? std::string("<no game>") : gameRoot.string()) +
           "\" or \"" + engineRoot.string() + "\"";
}

    /// (tree, destination within it) for a file on disk, or false if it is under neither.
    ///
    /// A document-relative reference is not confined to the tree its document was found in --
    /// the composite scenes graft props onto glTFs in the *other* tree, so they reach across
    /// with `../../../engine/assets/...`. The loader follows that happily; a package has to
bool Resolver::relocate(const fs::path& path, std::string& tree, std::string& dest) const {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(path, ec);
    for (const auto& [label, root] : {std::pair{std::string("game"), gameRoot},
                                      std::pair{std::string("engine"), engineRoot}}) {
        if (root.empty()) continue;
        const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
        const fs::path relative = fs::relative(resolved, canonicalRoot, ec);
        if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0) continue;
        tree = label;
        dest = relative.generic_string();
        return true;
    }
    return false;
}

std::string restrictedPart(const std::string& name) {
    for (const char* part : kRestricted) {
        const std::string needle = std::string("/") + part + "/";
        if (name == part || name.rfind(std::string(part) + "/", 0) == 0 ||
            name.find(needle) != std::string::npos) {
            return part;
        }
    }
    return {};
}

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
              const std::set<std::string>& optional, bool requireCache) {
    Closure out;
    std::set<std::pair<std::string, std::string>> seen;

    struct Item {
        fs::path path;
        std::string tree;
        std::string dest;
    };
    std::vector<Item> queue;

    for (const std::string& name : required) {
        fs::path path;
        std::string tree;
        if (!resolver.resolve(name, path, tree)) {
            out.missing.push_back(name);
            continue;
        }
        queue.push_back({path, tree, name});
    }
    for (const std::string& name : optional) {
        fs::path path;
        std::string tree;
        // A compiled-in default the packaged config overrides. Not being there is the normal
        // case on a machine that never fetched it.
        if (resolver.resolve(name, path, tree)) queue.push_back({path, tree, name});
    }

    std::error_code ec;
    while (!queue.empty()) {
        const Item item = queue.back();
        queue.pop_back();

        if (!seen.insert({item.path.string(), item.tree}).second) continue;

        // Normalised, because a reference that reached across trees got here through `../../`
        // and the raw form is unreadable in build output -- and because two spellings of one
        // file would otherwise stage it twice.
        const std::string packaged = resolver.prefix(item.tree) + "/" + item.dest;
        out.staged[fs::weakly_canonical(item.path, ec)] = packaged;
        if (!restrictedPart(item.dest).empty()) out.restricted.insert(packaged);

        std::string ext = item.path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".gltf" && ext != ".glb") continue;

        rapidjson::Document doc;
        if (!readGltf(item.path, doc)) continue;

        for (const fs::path& ref : gltfReferences(doc, item.path)) {
            if (!fs::exists(ref, ec)) {
                std::string refExt = ref.extension().string();
                std::transform(refExt.begin(), refExt.end(), refExt.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (requireCache && refExt == ".ktx2") {
                    out.cold.insert(ref.filename().string() + " (referenced by " +
                                    item.path.filename().string() + ")");
                }
                continue;
            }

            std::string refTree;
            std::string refDest;
            if (!resolver.relocate(ref, refTree, refDest)) {
                // Under neither asset root, so there is nowhere in the package it could go and
                // still be found by the same relative walk.
                out.missing.push_back(ref.generic_string() + " (referenced by " +
                                      item.path.filename().string() +
                                      ", outside both asset trees)");
                continue;
            }
            queue.push_back({ref, refTree, refDest});
        }
    }
    return out;
}

/// Names somebody wrote down precisely because nothing else would find them.
std::set<std::string> readExtraNames(const fs::path& path) {
    std::set<std::string> names;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return names;

    std::istringstream lines(readFile(path));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.rfind("res:/", 0) == 0) line = line.substr(5);
        if (!line.empty()) names.insert(line);
    }
    return names;
}

} // namespace manifest

namespace {

using namespace manifest;

void reportList(const char* verb, size_t count, const char* what,
                const std::set<std::string>& items, const char* tail) {
    std::fprintf(stderr, "%s: %zu %s:\n", verb, count, what);
    size_t shown = 0;
    for (const std::string& item : items) {
        if (shown++ == 8) break;
        std::fprintf(stderr, "  %s\n", item.c_str());
    }
    if (items.size() > 8) {
        std::fprintf(stderr, "  ... and %zu more\n", items.size() - 8);
    }
    std::fputs(tail, stderr);
}

} // namespace

int cmdManifest(const std::vector<std::string>& args) {
    std::string game;
    fs::path configPath;
    bool json = false;
    bool strict = false;
    bool requireCache = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--json") {
            json = true;
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--require-cache") {
            requireCache = true;
        } else if (arg == "--config" && i + 1 < args.size()) {
            configPath = args[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate manifest <game> [--config P] [--json] [--strict] "
                       "[--require-cache]\n"
                       "\n"
                       "Every file a packaged game needs, as `src -> dest` lines.\n",
                       stderr);
            return 0;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
            return 2;
        } else if (game.empty()) {
            game = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    if (game.empty()) {
        std::fputs("error: no game named. Usage: substrate manifest <game>\n", stderr);
        std::fputs("games:\n", stderr);
        printGames(stderr);
        return 2;
    }
    if (!isGame(game)) {
        std::fprintf(stderr, "error: game/%s/CMakeLists.txt does not exist\n", game.c_str());
        return 1;
    }

    const fs::path gameDir = repoRoot() / "game" / game;
    const fs::path gameRoot = gameDir / "assets";
    const fs::path engineRoot = repoRoot() / "engine" / "assets";

    // Checked before anything else, and separately, because "the tree is not there" and "the
    // tree is there and is missing one file" want different messages.
    std::error_code ec;
    for (const fs::path& root : {engineRoot, gameRoot}) {
        if (!fs::is_directory(root, ec)) {
            std::fprintf(stderr,
                         "error: %s does not exist, so there is nothing to package.\n"
                         "       The fetched assets are not committed. Run: substrate "
                         "fetch-assets\n",
                         root.generic_string().c_str());
            return 1;
        }
    }

    if (configPath.empty()) configPath = repoRoot() / "substrate.json";
    if (!fs::is_regular_file(configPath, ec)) {
        std::fprintf(stderr, "error: %s does not exist\n", configPath.generic_string().c_str());
        return 1;
    }
    rapidjson::Document config;
    config.Parse(readFile(configPath).c_str());
    if (config.HasParseError()) {
        std::fprintf(stderr, "error: %s is not valid JSON\n", configPath.generic_string().c_str());
        return 1;
    }

    // Three seeds, and which of them may be missing is not uniform. The **game's own source
    // literals are required**, because since the scene path moved out of the config and into
    // `GameSetup` that is where a game names what it loads -- treating them as optional would
    // let a package build with no scene in it and open on a black screen. The **engine's
    // literals stay optional**: those are defaults for a game that names nothing.
    std::set<std::string> required;
    scanJson(config, required);
    for (const std::string& name : readExtraNames(gameDir / "package.txt")) required.insert(name);
    for (const std::string& name : scanLiterals({gameDir})) required.insert(name);

    std::set<std::string> optional;
    for (const std::string& name : scanLiterals({repoRoot() / "engine"})) {
        if (!required.count(name)) optional.insert(name);
    }

    Resolver resolver{gameRoot, engineRoot, "game/" + game + "/assets", "engine/assets"};
    const Closure closure = build(resolver, required, optional, requireCache);

    if (!closure.restricted.empty()) {
        reportList(strict ? "error" : "note", closure.restricted.size(),
                   "packaged file(s) cannot be redistributed", closure.restricted,
                   "       Sponza is (c) Crytek under the CryEngine Limited License "
                   "Agreement,\n"
                   "       which is incompatible with this repository's Apache-2.0. Fine for "
                   "a\n       local build; not for one that goes to other people.\n");
        if (strict) return 1;
    }

    if (!closure.cold.empty()) {
        reportList("error", closure.cold.size(), "packaged image(s) have no .ktx2 sidecar",
                   closure.cold,
                   "       A cold cache is fine in a source tree and wrong in a package -- "
                   "it\n       ships the decode path to someone who cannot rebuild it.\n"
                   "       Run: substrate ktx2\n");
        return 1;
    }

    if (!closure.missing.empty()) {
        std::fprintf(stderr, "error: %zu reference(s) resolved to nothing.\n",
                     closure.missing.size());
        std::fprintf(stderr, "       searched %s\n", resolver.rootsDescription().c_str());
        std::vector<std::string> sorted = closure.missing;
        std::sort(sorted.begin(), sorted.end());
        for (const std::string& name : sorted) std::fprintf(stderr, "  res:/%s\n", name.c_str());
        std::fprintf(stderr,
                     "\n       Run substrate fetch-assets if the tree is only missing what "
                     "is\n       fetched, or add the name to game/%s/package.txt if it is\n"
                     "       built another way.\n",
                     game.c_str());
        return 1;
    }

    if (json) {
        std::printf("{\n  \"game\": \"%s\",\n  \"files\": [\n", game.c_str());
        size_t index = 0;
        for (const auto& [src, dest] : closure.staged) {
            std::printf("    {\n      \"src\": \"%s\",\n      \"dest\": \"%s\"\n    }%s\n",
                        src.generic_string().c_str(), dest.c_str(),
                        ++index == closure.staged.size() ? "" : ",");
        }
        std::printf("  ]\n}\n");
    } else {
        for (const auto& [src, dest] : closure.staged) {
            std::printf("%s -> %s\n", src.generic_string().c_str(), dest.c_str());
        }
    }
    return 0;
}

} // namespace tool
