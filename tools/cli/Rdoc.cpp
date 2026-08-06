#include "Rdoc.h"

#include "Process.h"
#include "Repo.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// 60 and 90 match the golden suite: past the load hitch, inside the profiler window, and a
/// stated frame rather than whichever one the run ended on.
constexpr int kFrame = 60;
constexpr int kFrames = 90;
constexpr int kTimeout = 300;

constexpr const char* kUsage =
    "usage: substrate rdoc <command> [args]\n"
    "\n"
    "  capture [config] -- [flags]   run and write debug_frames/rdoc/frame*.rdc\n"
    "  passes <rdc|xml>              the pass tree\n"
    "  state <rdc|xml> <chunk>       the state bound at one chunk\n"
    "  barriers <rdc|xml>            the image barrier stream\n"
    "  resources <rdc|xml>           the ResourceId to name map\n"
    "  xml <rdc>                     convert only, and print the path\n"
    "  thumb <rdc>                   write the capture's thumbnail beside it\n";

/// RenderDoc is not packaged on most distributions and its official build is a tarball
/// unpacked wherever the user put it, so there is no path worth defaulting to. RDOC_ROOT
/// names that directory; failing that, an installed renderdoccmd on PATH is used.
fs::path renderdoccmd() {
    if (const char* root = std::getenv("RDOC_ROOT")) {
        const fs::path candidate = fs::path(root) / "bin" / executableName("renderdoccmd");
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return which("renderdoccmd");
}

bool requireRenderdoc(fs::path& out) {
    out = renderdoccmd();
    if (!out.empty()) return true;
    std::fputs("error: renderdoccmd not found.\n"
               "       Set RDOC_ROOT to the RenderDoc install directory, or put\n"
               "       renderdoccmd on PATH. RenderDoc: https://renderdoc.org/builds\n",
               stderr);
    return false;
}

bool needFile(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return true;
    std::fprintf(stderr, "error: no such capture: %s\n", path.generic_string().c_str());
    return false;
}

/// A .rdc is converted beside itself and the .xml reused. Empty on failure.
fs::path toXml(const fs::path& input) {
    if (input.extension() == ".xml") return input;

    fs::path cmd;
    if (!requireRenderdoc(cmd)) return {};

    fs::path out = input;
    out.replace_extension(".xml");

    std::error_code ec;
    const bool stale = !fs::exists(out, ec) ||
                       fs::last_write_time(input, ec) > fs::last_write_time(out, ec);
    if (stale) {
        std::fprintf(stderr, "==> converting %s\n", input.generic_string().c_str());
        RunOptions options;
        options.cwd = repoRoot();
        options.inherit = true;
        if (const RunResult r = run({cmd.string(), "convert", "-f", input.string(), "-o",
                                     out.string(), "-c", "xml"},
                                    options);
            !r.ok()) {
            return {};
        }
    }
    return out;
}

/// The analyser is Python and stays Python: it is an XML walk over a format RenderDoc owns,
/// and nothing in the engine shares a definition with it.
int analyse(const std::vector<std::string>& args) {
    const fs::path python = pythonExe();
    if (python.empty()) {
        std::fputs("error: no python interpreter found, and the capture analyser is Python.\n",
                   stderr);
        return 1;
    }
    std::vector<std::string> command{python.string(),
                                     (repoRoot() / "scripts" / "rdoc" / "analyse.py").string()};
    command.insert(command.end(), args.begin(), args.end());

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    return run(command, options).exitCode;
}

int capture(const std::vector<std::string>& args) {
    Config config = Config::Release;
    size_t first = 0;
    if (!args.empty() && args[0] != "--") {
        const std::optional<Config> parsed = parseConfig(args[0]);
        if (!parsed) {
            std::fprintf(stderr, "error: unknown config '%s' (want: %s)\n", args[0].c_str(),
                         configList().c_str());
            return 1;
        }
        config = *parsed;
        first = 1;
    }
    if (first < args.size() && args[first] == "--") ++first;

    const fs::path dir = repoRoot() / "debug_frames" / "rdoc";
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::vector<std::string> command{selfPath().string(), "run", name(config), "--",
                                     // --windowed against the rule that a frame budget unmaps
                                     // the window: RenderDoc hooks the present it is asked to
                                     // capture, and this is the one harness whose output is a
                                     // human opening the capture rather than a number.
                                     "--windowed", "--frames", std::to_string(kFrames),
                                     "--rdoc-capture-frame", std::to_string(kFrame),
                                     "--rdoc-capture-path", (dir / "frame").string()};
    command.insert(command.end(), args.begin() + static_cast<long>(first), args.end());

    RunOptions options;
    options.cwd = repoRoot();
    options.inherit = true;
    options.timeoutSeconds = kTimeout;
    // ENABLE_VULKAN_RENDERDOC_CAPTURE is what pulls the implicit capture layer into the
    // process; RenderDoc's layer json is gated on it. Without it the run succeeds and writes
    // no .rdc, which is the single easiest way to waste a capture.
    options.env.emplace_back("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1");
    run(command, options);

    fs::path latest;
    fs::file_time_type newest{};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".rdc") continue;
        const fs::file_time_type when = fs::last_write_time(entry.path(), ec);
        if (latest.empty() || when > newest) {
            latest = entry.path();
            newest = when;
        }
    }
    if (latest.empty()) {
        std::fputs("error: the run finished but wrote no .rdc.\n", stderr);
        std::fprintf(stderr,
                     "       --frames must exceed --rdoc-capture-frame (%d vs %d here).\n",
                     kFrames, kFrame);
        return 1;
    }
    std::printf("%s\n", latest.generic_string().c_str());
    return 0;
}

} // namespace

int cmdRdoc(const std::vector<std::string>& args) {
    const std::string mode = args.empty() ? "" : args[0];
    const std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());

    if (mode.empty() || mode == "-h" || mode == "--help" || mode == "help") {
        std::fputs(kUsage, stderr);
        return mode.empty() ? 1 : 0;
    }

    if (mode == "capture") return capture(rest);

    if (mode == "passes" || mode == "barriers" || mode == "resources" || mode == "state" ||
        mode == "xml" || mode == "thumb") {
        if (rest.empty()) {
            std::fprintf(stderr, "usage: substrate rdoc %s <rdc|xml>%s\n", mode.c_str(),
                         mode == "state" ? " <chunkIndex>" : "");
            return 1;
        }
        const fs::path input = rest[0];
        if (!needFile(input)) return 1;

        if (mode == "thumb") {
            fs::path cmd;
            if (!requireRenderdoc(cmd)) return 1;
            fs::path out = input;
            out.replace_extension(".jpg");
            RunOptions options;
            options.cwd = repoRoot();
            options.inherit = true;
            if (const RunResult r =
                    run({cmd.string(), "thumb", input.string(), "-o", out.string()}, options);
                !r.ok()) {
                return r.exitCode ? r.exitCode : 1;
            }
            std::printf("%s\n", out.generic_string().c_str());
            return 0;
        }

        const fs::path xml = toXml(input);
        if (xml.empty()) return 1;
        if (mode == "xml") {
            std::printf("%s\n", xml.generic_string().c_str());
            return 0;
        }
        if (mode == "state") {
            if (rest.size() < 2) {
                std::fputs("error: state needs a chunkIndex -- find one with: substrate rdoc "
                           "passes\n", stderr);
                return 1;
            }
            return analyse({"state", xml.string(), rest[1]});
        }
        return analyse({mode, xml.string()});
    }

    std::fprintf(stderr, "error: unknown command '%s'\n", mode.c_str());
    std::fputs(kUsage, stderr);
    return 1;
}

int cmdInstallHooks(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate install-hooks\n"
                       "\n"
                       "Writes a pre-commit hook that runs the perf gate when a rendering path\n"
                       "is staged. `git commit --no-verify` skips it.\n",
                       stderr);
            return 0;
        }
        std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
        return 1;
    }

    RunOptions options;
    options.cwd = repoRoot();
    const RunResult where = run({"git", "rev-parse", "--git-path", "hooks"}, options);
    if (!where.ok()) {
        std::fputs("error: not a git repository, so there is nowhere to install a hook.\n",
                   stderr);
        return 1;
    }
    std::string hooks = where.out;
    while (!hooks.empty() && (hooks.back() == '\n' || hooks.back() == '\r')) hooks.pop_back();

    std::error_code ec;
    const fs::path dir = repoRoot() / hooks;
    fs::create_directories(dir, ec);
    const fs::path hook = dir / "pre-commit";

    // The hook body stays POSIX shell: git runs hooks through its own shell on every
    // platform, and Git for Windows ships one.
    std::ofstream out(hook);
    if (!out) {
        std::fprintf(stderr, "error: cannot write %s\n", hook.generic_string().c_str());
        return 1;
    }
    out << "#!/usr/bin/env sh\n"
           "#\n"
           "# Refuse a commit that made the frame slower. Written by `substrate install-hooks`.\n"
           "#\n"
           "# Only when a rendering path is staged: the gate costs a 300-frame run, and a\n"
           "# commit that touches none of this cannot have moved the number.\n"
           "#\n"
           "if ! git diff --cached --name-only | grep -qE "
           "'^(engine/gfx/|engine/shaders/|engine/scene/)'; then\n"
           "    exit 0\n"
           "fi\n"
           "\n"
           "root=$(git rev-parse --show-toplevel)\n"
           "if [ ! -x \"$root/build/release/demo\" ] && [ ! -x \"$root/build/release/demo.exe\" ]; "
           "then\n"
           "    echo \"perfgate: build/release/demo is not built, so the frame was not measured.\" >&2\n"
           "    echo \"          Run scripts/build_game.sh demo release, or commit with "
           "--no-verify.\" >&2\n"
           "    exit 1\n"
           "fi\n"
           "\n"
           "\"$root/scripts/substrate.sh\" perfgate --config release\n";
    out.close();

    fs::permissions(hook,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    std::printf("installed %s\n", hook.generic_string().c_str());
    return 0;
}

} // namespace tool
