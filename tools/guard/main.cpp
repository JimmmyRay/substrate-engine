#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @file tools/guard/main.cpp
 * @brief `substrate-guard ascii|layers` -- the two checks CMake runs on every build.
 *
 * This links nothing. Not the engine, not the hosted sources, not a submodule -- only the
 * standard library. That is what lets `ascii_guard` and `layer_guard` be `ALL` targets that
 * every other target depends on: a guard built out of the tree it guards cannot run before
 * the tree builds.
 *
 * What a module is, and why the tiers are what they are: architecture/README.md, "A module
 * is what `root` cannot reach".
 */
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- shared

/// Directory names skipped by both guards wherever they appear.
///
/// `assets` and `__pycache__` are here for the same reason and `.git` for a worse one: a
/// zlib-deflated object store is the most NUL-dense thing in the tree, and every game but
/// the demo is its own repository, so `game/<name>/.git` is the normal arrangement.
bool skipDir(const std::string& name) {
    return name == "assets" || name == "__pycache__" || name == ".git";
}

std::vector<fs::path> sourceFiles(const fs::path& root, bool codeOnly) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;

    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (skipDir(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        if (codeOnly) {
            const std::string ext = it->path().extension().string();
            if (ext != ".h" && ext != ".cpp" && ext != ".hpp") continue;
        }
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Forward slashes on every platform, so a reported path and an ACCEPTED key match on
/// Windows as well as they do here.
std::string generic(const fs::path& p) { return p.generic_string(); }

// ---------------------------------------------------------------- the ASCII guard

/// Every byte outside 0x00-0x7F is a violation, with one exception: U+2014 EM DASH is house
/// style in comments across the whole codebase, and rejecting it would make this guard a
/// rewrite rather than a check. Smart quotes, ellipses, non-Latin scripts and mojibake fail.
int checkAscii(const std::vector<std::string>& dirs) {
    static constexpr char kEmDash[] = "\xe2\x80\x94";

    std::vector<std::string> hits;

    for (const std::string& dir : dirs) {
        for (const fs::path& file : sourceFiles(dir, false)) {
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;

            std::string line;
            int number = 0;
            while (std::getline(in, line)) {
                ++number;
                if (!line.empty() && line.back() == '\r') line.pop_back();

                bool bad = false;
                for (size_t i = 0; i < line.size(); ++i) {
                    if (static_cast<unsigned char>(line[i]) < 0x80) continue;
                    if (line.compare(i, 3, kEmDash) == 0) {
                        i += 2;
                        continue;
                    }
                    bad = true;
                    break;
                }
                if (bad) hits.push_back(generic(file) + ":" + std::to_string(number) + ":" + line);
            }
        }
    }

    if (hits.empty()) return 0;

    std::fputs("error: non-ASCII bytes in source (only U+2014 EM DASH is allowed):\n", stderr);
    for (const std::string& hit : hits) std::fprintf(stderr, "%s\n", hit.c_str());
    return 1;
}

// ---------------------------------------------------------------- the layer guard

// Two lists rather than a row per directory, because two lists is what the definition
// actually is. A directory in neither, and not `core`, is an error rather than a pass, so a
// new directory under engine/ has to be classified deliberately.
const char* const kCluster[] = {"gfx", "scene", "ui", "sim", "root"};
const char* const kModules[] = {"ai", "nav", "particles", "physics", "audio", "anim"};

/// Directories under engine/ that hold no translation unit and are not code.
const char* const kNotAModule[] = {"shaders", "assets"};

/// Edges that predate the guard, as `<path>:<from>-><to>`. **The guard fails on anything not
/// in this list**, which is the property worth having on every build: the count can fall and
/// cannot rise.
///
/// No line number: an accepted edge survives the file being reformatted, and pinning it to a
/// line would turn every unrelated edit into a failure here.
const char* const kAccepted[] = {nullptr};

bool contains(const char* const* set, size_t count, std::string_view name) {
    for (size_t i = 0; i < count; ++i) {
        if (set[i] && name == set[i]) return true;
    }
    return false;
}

enum class Tier { Core, Engine, Module, Unknown };

/// Every member of the engine cluster answers to one name, so an edge inside it is a
/// self-edge rather than a question.
Tier tierOf(std::string_view dir) {
    if (contains(kCluster, std::size(kCluster), dir)) return Tier::Engine;
    if (contains(kModules, std::size(kModules), dir)) return Tier::Module;
    if (dir == "core") return Tier::Core;
    return Tier::Unknown;
}

const char* tierName(Tier t) {
    switch (t) {
    case Tier::Core: return "core";
    case Tier::Engine: return "engine";
    case Tier::Module: return "module";
    case Tier::Unknown: return "unknown";
    }
    return "unknown";
}

/// What a tier may include, as tiers.
std::vector<Tier> allowedTiers(Tier from) {
    switch (from) {
    case Tier::Core: return {};
    case Tier::Engine: return {Tier::Core, Tier::Engine};
    case Tier::Module: return {Tier::Core, Tier::Engine};
    case Tier::Unknown: return {};
    }
    return {};
}

std::string allowedNames(Tier from) {
    std::string out;
    for (Tier t : allowedTiers(from)) {
        if (!out.empty()) out += ' ';
        out += tierName(t);
    }
    return out;
}

/// A cycle written into `allowedTiers` would make every include legal and this guard would
/// pass by saying nothing, so the table is checked before a single file is read. The engine
/// cluster is one node here, which is the whole reason gfx <-> scene is not a cycle.
bool tierTableIsAcyclic(std::string& cycle) {
    enum class Colour { White, Grey, Black };
    std::unordered_map<int, Colour> colour;

    // Recursion over three nodes, so the visitor is a lambda rather than a worklist.
    auto visit = [&](auto&& self, Tier node) -> bool {
        colour[static_cast<int>(node)] = Colour::Grey;
        for (Tier next : allowedTiers(node)) {
            if (next == node) continue;
            const Colour seen = colour.count(static_cast<int>(next)) ? colour[static_cast<int>(next)]
                                                                    : Colour::White;
            if (seen == Colour::Grey) {
                cycle = std::string(tierName(node)) + " -> " + tierName(next);
                return false;
            }
            if (seen == Colour::White && !self(self, next)) {
                cycle = std::string(tierName(node)) + " -> " + cycle;
                return false;
            }
        }
        colour[static_cast<int>(node)] = Colour::Black;
        return true;
    };

    for (Tier t : {Tier::Core, Tier::Engine, Tier::Module}) {
        const bool unvisited = !colour.count(static_cast<int>(t));
        if (unvisited && !visit(visit, t)) return false;
    }
    return true;
}

/// The quoted target of `#include "..."`, or empty for any other line.
///
/// Only quoted includes: <glm/glm.hpp> and every other angle-bracket form is a dependency
/// rather than a directory of ours.
std::string includeTarget(const std::string& line) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] != '#') return {};
    i = line.find_first_not_of(" \t", i + 1);
    if (i == std::string::npos || line.compare(i, 7, "include") != 0) return {};
    i = line.find_first_not_of(" \t", i + 7);
    if (i == std::string::npos || line[i] != '"') return {};
    const size_t end = line.find('"', i + 1);
    if (end == std::string::npos) return {};
    return line.substr(i + 1, end - i - 1);
}

/// An include with no slash names a header at the root of engine/, which is `root`.
std::string dirOf(const std::string& rel) {
    const size_t slash = rel.find('/');
    return slash == std::string::npos ? "root" : rel.substr(0, slash);
}

int checkLayers(bool table) {
    if (table) {
        std::string cluster, modules;
        for (const char* d : kCluster) cluster += (cluster.empty() ? "" : " ") + std::string(d);
        for (const char* d : kModules) modules += (modules.empty() ? "" : " ") + std::string(d);
        std::printf("%-34s -> %s\n", "core", "(nothing)");
        std::printf("%-34s -> %s\n", cluster.c_str(), "core, and each other");
        std::printf("%-34s -> %s\n", modules.c_str(), "core, the engine cluster");
        return 0;
    }

    std::string cycle;
    if (!tierTableIsAcyclic(cycle)) {
        std::fprintf(stderr, "error: the tier table has a cycle: %s\n", cycle.c_str());
        return 2;
    }

    std::unordered_set<std::string> accepted;
    for (const char* key : kAccepted) {
        if (key) accepted.insert(key);
    }
    std::unordered_set<std::string> seen;
    int edges = 0;

    for (const fs::path& file : sourceFiles("engine", true)) {
        const std::string path = generic(file);
        const std::string from = dirOf(path.substr(std::strlen("engine/")));
        if (contains(kNotAModule, std::size(kNotAModule), from)) continue;

        const Tier fromTier = tierOf(from);
        if (fromTier == Tier::Unknown) {
            std::fprintf(stderr,
                         "error: %s is in engine/%s/, which is in neither CLUSTER nor MODULES\n",
                         path.c_str(), from.c_str());
            return 2;
        }

        std::ifstream in(file);
        if (!in) continue;

        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const std::string target = includeTarget(line);
            if (target.empty()) continue;

            const std::string to = dirOf(target);
            // A directory always reaches itself, and a vendored path reached through a quoted
            // include is not an edge in this graph at all.
            if (to == from) continue;
            const Tier toTier = tierOf(to);
            if (toTier == Tier::Unknown) continue;

            std::string why;
            if (fromTier == Tier::Module && toTier == Tier::Module) {
                // Two modules are peers, so one naming the other is a violation even though
                // both sit at the same tier. Checked before the tier test, which would pass it.
                why = "a module may not name another module";
            } else {
                const std::vector<Tier> allow = allowedTiers(fromTier);
                if (std::find(allow.begin(), allow.end(), toTier) != allow.end()) continue;
                const std::string names = allowedNames(fromTier);
                why = names.empty() ? std::string(tierName(fromTier)) + " may include nothing"
                                    : std::string(tierName(fromTier)) + " may include: " + names;
            }

            const std::string key = path + ":" + from + "->" + to;
            if (accepted.count(key)) {
                seen.insert(key);
                continue;
            }
            std::fprintf(stderr, "%s:%d: %s -> %s (%s)\n", path.c_str(), number, from.c_str(),
                         to.c_str(), why.c_str());
            ++edges;
        }
    }

    // An accepted edge that matched nothing is an exception outliving what it excused.
    int stale = 0;
    for (const std::string& key : accepted) {
        if (!seen.count(key)) {
            std::fprintf(stderr, "error: accepted edge no longer exists, delete it from "
                                 "kAccepted: %s\n",
                         key.c_str());
            ++stale;
        }
    }

    if (edges > 0 || stale > 0) {
        if (edges > 0) {
            std::fprintf(stderr, "layer guard: %d upward include%s\n", edges, edges == 1 ? "" : "s");
        }
        return 1;
    }

    if (!accepted.empty()) {
        std::printf("layer guard: clean (%zu accepted edge%s remain; see kAccepted in "
                    "tools/guard/main.cpp)\n",
                    accepted.size(), accepted.size() == 1 ? "" : "s");
    }
    return 0;
}

void usage() {
    std::fputs("substrate-guard -- the checks CMake runs on every build\n"
               "\n"
               "usage: substrate-guard ascii [dir ...]     defaults to engine game tests tools\n"
               "       substrate-guard layers [--table]    --table prints the graph and exits\n"
               "\n"
               "Run from the repository root. Exit 1 is a violation, 2 is a broken check.\n",
               stderr);
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "ascii") {
        std::vector<std::string> dirs(argv + 2, argv + argc);
        if (dirs.empty()) dirs = {"engine", "game", "tests", "tools"};
        return checkAscii(dirs);
    }
    if (mode == "layers") {
        const bool table = argc > 2 && std::strcmp(argv[2], "--table") == 0;
        return checkLayers(table);
    }

    usage();
    return mode.empty() || mode == "-h" || mode == "--help" ? 0 : 2;
}
