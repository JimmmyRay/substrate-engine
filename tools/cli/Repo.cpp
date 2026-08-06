#include "Repo.h"

#include "Process.h"

#include <algorithm>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tool {
namespace {

namespace fs = std::filesystem;

bool looksLikeRoot(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_regular_file(dir / "CMakeLists.txt", ec)) return false;
    std::ifstream in(dir / "CMakeLists.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("project(Substrate") != std::string::npos) return true;
    }
    return false;
}

fs::path findRoot() {
    std::error_code ec;
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        if (looksLikeRoot(dir)) return dir;
        if (dir == dir.parent_path()) break;
    }
#ifdef SUBSTRATE_SOURCE_DIR
    if (looksLikeRoot(SUBSTRATE_SOURCE_DIR)) return fs::path(SUBSTRATE_SOURCE_DIR);
#endif
    return {};
}

} // namespace

const char* name(Config config) {
    switch (config) {
    case Config::Debug: return "debug";
    case Config::Release: return "release";
    case Config::Asan: return "asan";
    case Config::Tsan: return "tsan";
    }
    return "debug";
}

std::optional<Config> parseConfig(std::string_view token) {
    if (token == "debug") return Config::Debug;
    if (token == "release") return Config::Release;
    if (token == "asan") return Config::Asan;
    if (token == "tsan") return Config::Tsan;
    return std::nullopt;
}

std::string configList() { return "debug|release|asan|tsan"; }

std::string configUnsupported(Config config) {
#ifdef _WIN32
    if (config == Config::Asan || config == Config::Tsan) {
        return std::string(name(config)) +
               " needs a sanitizer runtime MinGW does not ship. Use a Linux checkout, or "
               "release for a build you can run here.";
    }
#else
    (void)config;
#endif
    return {};
}

const fs::path& repoRoot() {
    static const fs::path root = findRoot();
    return root;
}

fs::path pythonExe() {
    for (const char* name : {"python3", "python"}) {
        if (const fs::path found = which(name); !found.empty()) return found;
    }
    return {};
}

const fs::path& selfPath() {
    static const fs::path self = [] {
        std::error_code ec;
#ifdef _WIN32
        wchar_t buffer[32768];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, std::size(buffer));
        if (length > 0) return fs::path(std::wstring(buffer, length));
#else
        const fs::path link = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) return link;
#endif
        return fs::path();
    }();
    return self;
}

fs::path buildDir(Config config) { return repoRoot() / "build" / name(config); }

std::string executableName(std::string_view stem) {
#ifdef _WIN32
    return std::string(stem) + ".exe";
#else
    return std::string(stem);
#endif
}

bool isGame(std::string_view candidate) {
    if (candidate.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(repoRoot() / "game" / candidate / "CMakeLists.txt", ec);
}

std::vector<std::string> games() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(repoRoot() / "game", ec)) {
        if (!entry.is_directory()) continue;
        if (fs::is_regular_file(entry.path() / "CMakeLists.txt", ec)) {
            out.push_back(entry.path().filename().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void printGames(std::FILE* stream) {
    const std::vector<std::string> found = games();
    if (found.empty()) {
        std::fputs("  (none -- game/<name>/CMakeLists.txt is what makes one)\n", stream);
        return;
    }
    for (const std::string& game : found) std::fprintf(stream, "  %s\n", game.c_str());
}

std::string cacheValue(const fs::path& cache, std::string_view key) {
    std::ifstream in(cache);
    if (!in) return {};

    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        const size_t colon = line.find(':', key.size());
        if (colon != key.size()) continue;
        const size_t equals = line.find('=', colon);
        if (equals == std::string::npos) continue;
        return line.substr(equals + 1);
    }
    return {};
}

std::string cachedGame(Config config) {
    return cacheValue(buildDir(config) / "CMakeCache.txt", "SUBSTRATE_GAME");
}

Launch sanitizerLaunch(Config config) {
    Launch launch;
    switch (config) {
    case Config::Asan:
        // halt_on_error=0 so a run surfaces every finding, not just the first.
        launch.env.emplace_back("ASAN_OPTIONS",
                                "detect_leaks=1:halt_on_error=0:abort_on_error=0");
        launch.env.emplace_back("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=0");
        break;
    case Config::Tsan:
        launch.env.emplace_back("TSAN_OPTIONS", "halt_on_error=0:second_deadlock_stack=1");
        if (!which("setarch").empty()) {
            launch.prefix = {"setarch", "-R"};
        } else {
            std::fputs("warning: setarch not found; TSan will likely abort before main()\n",
                       stderr);
        }
        break;
    default:
        break;
    }
    return launch;
}

bool parseInvocation(const std::vector<std::string>& args, Invocation& out, int& exitCode,
                     std::string_view usage) {
    size_t i = 0;
    for (; i < args.size() && args[i] != "--"; ++i) {
        const std::string& token = args[i];

        if (token == "-h" || token == "--help" || token == "help") {
            std::fprintf(stderr, "%s\n", std::string(usage).c_str());
            std::fputs("games:\n", stderr);
            printGames(stderr);
            exitCode = 0;
            return false;
        }
        if (token == "--list" || token == "list") {
            printGames(stdout);
            exitCode = 0;
            return false;
        }
        if (const std::optional<Config> config = parseConfig(token)) {
            out.config = *config;
            continue;
        }
        if (isGame(token)) {
            out.game = token;
            out.namedGame = true;
            continue;
        }

        std::fprintf(stderr, "error: '%s' is neither a configuration nor a game.\n",
                     token.c_str());
        std::fprintf(stderr, "       configurations: %s\n", configList().c_str());
        std::fputs("       Everything meant for the binary goes after '--'.\n", stderr);
        std::fputs("games:\n", stderr);
        printGames(stderr);
        exitCode = 1;
        return false;
    }

    if (i < args.size() && args[i] == "--") ++i;
    out.rest.assign(args.begin() + static_cast<long>(i), args.end());
    return true;
}

} // namespace tool
