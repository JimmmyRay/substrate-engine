#include "Setup.h"

#include "Process.h"
#include "Repo.h"

#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr const char* kUpstream = "https://github.com/KhronosGroup/glTF-Sample-Assets.git";
constexpr const char* kModelPath = "Models/Sponza";

std::string substitute(std::string text, const std::string& game, const std::string& className) {
    for (const auto& [token, value] : {std::pair{"@NAME@", game}, std::pair{"@CLASS@", className}}) {
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at)) {
            text.replace(at, std::strlen(token), value);
            at += value.size();
        }
    }
    return text;
}

/// Written to a temp and moved into place, so a failure part-way leaves no half-scaffolded
/// directory for the next run to refuse.
bool render(const fs::path& source, const fs::path& target, const std::string& game,
            const std::string& className) {
    std::ifstream in(source);
    if (!in) {
        std::fprintf(stderr, "error: missing template %s\n", source.generic_string().c_str());
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            std::fprintf(stderr, "error: cannot write %s\n", temp.generic_string().c_str());
            return false;
        }
        out << substitute(buffer.str(), game, className);
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    return !ec;
}

size_t countFiles(const fs::path& dir) {
    size_t count = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec)) ++count;
    }
    return count;
}

/// The grafted scenes, which are generated because the files they graft onto cannot be
/// committed. Skipped with a warning when python3 is absent, for the reason the transcode
/// below is: python is not a build dependency of the engine, and refusing here would make
/// it one.
void generateScenes() {
    const fs::path generator = repoRoot() / "scripts" / "make_composite_scene.py";
    std::error_code ec;
    if (!fs::is_regular_file(generator, ec)) return;

    const fs::path python = pythonExe();
    if (python.empty()) {
        std::fputs("warning: no python interpreter found; skipping scene generation\n", stderr);
        std::fputs("         run scripts/make_composite_scene.py by hand\n", stderr);
        return;
    }
    std::puts("Generating grafted scenes ...");
    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    run({python.string(), generator.string()}, options);
}

/// Cut a looping fire bed out of the recording beside it.
///
/// This transcodes rather than invents, and the engine is why it has to: miniaudio decodes
/// WAV, FLAC and MP3, and the recording is AAC in an MP4 container. Everything about the cut
/// is stated rather than defaulted -- eight seconds from ten seconds in because the opening
/// is quieter than the body; mono, because four braziers place it in space and a stereo image
/// would fight the panning; `volume` rather than `loudnorm`, because single-pass loudnorm on
/// a source this quiet (mean -41 dB) returns silence.
///
/// A missing ffmpeg or recording deletes any stale output rather than leaving it: the demo
/// tests for the file before building the source, so the braziers simply burn silently. A
/// demo missing a sound is right; one that plays static is broken.
void transcodeFireCrackle() {
    const fs::path dir = repoRoot() / "game" / "demo" / "assets" / "audio";
    const fs::path source = dir / "fire_crackle.m4a";
    const fs::path dest = dir / "fire_crackle.wav";

    std::error_code ec;
    if (which("ffmpeg").empty() || !fs::is_regular_file(source, ec)) {
        fs::remove(dest, ec);
        std::printf("skipped %s (needs ffmpeg and fire_crackle.m4a)\n",
                    dest.generic_string().c_str());
        return;
    }

    RunOptions options;
    options.cwd = repoRoot();
    run({"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", "10", "-t", "8", "-i",
         source.string(), "-ac", "1", "-ar", "48000", "-af",
         "volume=8dB,afade=t=in:st=0:d=0.25,afade=t=out:st=7.75:d=0.25", "-c:a", "pcm_s16le",
         "-fflags", "+bitexact", "-flags:a", "+bitexact", dest.string()},
        options);
    std::printf("wrote %s\n", dest.generic_string().c_str());
}

} // namespace

int cmdFetchAssets(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate fetch-assets\n"
                       "\n"
                       "Sparse-clones Sponza into engine/assets/, generates the grafted scenes\n"
                       "and transcodes the fire bed. Idempotent.\n",
                       stderr);
            return 0;
        }
        std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
        return 1;
    }

    const fs::path assetDir = repoRoot() / "engine" / "assets";
    const fs::path target = assetDir / "Sponza";
    const fs::path staging = assetDir / ".sponza-staging";

    std::error_code ec;
    if (fs::is_regular_file(target / "glTF" / "Sponza.gltf", ec)) {
        std::printf("Sponza already present at %s\n", target.generic_string().c_str());
        generateScenes();
        transcodeFireCrackle();
        return 0;
    }

    fs::create_directories(assetDir, ec);

    // Blobless and sparse: fetch only Models/Sponza (~41 MB) instead of the whole
    // multi-gigabyte sample-asset repository.
    std::printf("Fetching Sponza from %s ...\n", kUpstream);
    fs::remove_all(staging, ec);

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    if (const RunResult r = run({"git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
                                 kUpstream, staging.string()},
                                options);
        !r.ok()) {
        return 1;
    }
    if (const RunResult r = run({"git", "-C", staging.string(), "sparse-checkout", "set",
                                 kModelPath},
                                options);
        !r.ok()) {
        return 1;
    }

    if (!fs::is_regular_file(staging / kModelPath / "glTF" / "Sponza.gltf", ec)) {
        std::fprintf(stderr, "error: expected %s/glTF/Sponza.gltf in the upstream clone\n",
                     kModelPath);
        return 1;
    }

    fs::rename(staging / kModelPath, target, ec);
    if (ec) {
        // Across a filesystem boundary rename fails where a copy succeeds, and on Windows
        // the temporary and the asset tree are routinely on different volumes.
        fs::copy(staging / kModelPath, target,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    fs::remove_all(staging, ec);

    std::puts("Sponza ready:");
    std::printf("  %s\n", (target / "glTF" / "Sponza.gltf").generic_string().c_str());
    std::printf("  %zu files\n", countFiles(target));

    generateScenes();
    transcodeFireCrackle();
    return 0;
}

int cmdNewGame(const std::vector<std::string>& args) {
    const std::string game = args.empty() ? "" : args[0];

    if (game == "-h" || game == "--help" || game == "help") {
        std::fputs("usage: substrate new-game <name>\n", stderr);
        std::fputs("games:\n", stderr);
        printGames(stderr);
        return 0;
    }
    if (game == "--list" || game == "list") {
        printGames(stdout);
        return 0;
    }
    if (game.empty()) {
        std::fputs("error: no name given. Usage: substrate new-game <name>\n", stderr);
        return 1;
    }

    // A game's name reaches three places that constrain it: a directory, a CMake target, and
    // -- through the class -- a C++ identifier. Checked here rather than discovered as a
    // CMake error three commands later, which is the whole difference between a rejected
    // name and a broken build directory.
    bool valid = !game.empty() && std::islower(static_cast<unsigned char>(game[0]));
    for (char c : game) {
        const unsigned char u = static_cast<unsigned char>(c);
        valid = valid && (std::islower(u) || std::isdigit(u) || c == '_');
    }
    if (!valid) {
        std::fprintf(stderr,
                     "error: '%s' must be lowercase, start with a letter, and hold only letters,\n"
                     "       digits and underscores -- it becomes a directory, a CMake target and\n"
                     "       part of a C++ class name.\n",
                     game.c_str());
        return 1;
    }

    std::error_code ec;
    const fs::path dir = repoRoot() / "game" / game;
    if (fs::exists(dir, ec)) {
        std::fprintf(stderr, "error: game/%s already exists\n", game.c_str());
        return 1;
    }

    // mygame -> MygameGame. Ugly for one-word names and unambiguous for all of them, which is
    // the trade a generated identifier should make: nothing here has to guess where a word
    // boundary was.
    std::string className = game;
    className[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(className[0])));
    className += "Game";

    const fs::path templates = repoRoot() / "scripts" / "template" / "game";
    fs::create_directories(dir, ec);

    const std::pair<const char*, fs::path> files[] = {
        {"Game.h.in", dir / (className + ".h")},
        {"Game.cpp.in", dir / (className + ".cpp")},
        {"CMakeLists.txt.in", dir / "CMakeLists.txt"},
        {"README.md.in", dir / "README.md"},
    };
    for (const auto& [source, destination] : files) {
        if (!render(templates / source, destination, game, className)) return 1;
    }

    std::printf("created game/%s/\n", game.c_str());
    std::printf("  %s.h  %s.cpp  CMakeLists.txt  README.md\n", className.c_str(),
                className.c_str());
    std::printf("\nnext:\n  substrate build-game %s\n  substrate run %s\n", game.c_str(),
                game.c_str());
    return 0;
}

} // namespace tool
