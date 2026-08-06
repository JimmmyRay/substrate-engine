#include "Bench.h"

#include "Process.h"
#include "Repo.h"
#include "Trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <set>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// Columns of the emitted table, in the order the frame records them. An explicit list
/// rather than "whatever the trace contains", so a zone that stops being recorded shows up
/// as a missing column instead of a silently shorter table.
const char* const kTableZones[] = {"Cull",     "Shadows", "PunctualShadows", "GBuffer", "SSAO",
                                   "Lighting", "SSR",     "Bloom",           "Tonemap"};

constexpr const char* kUsage =
    "usage: substrate bench [options] -- [engine args]\n"
    "\n"
    "  --config <debug|release>  default release; a Debug baseline is not a baseline\n"
    "  --samples N [N ...]       MSAA counts to sweep. Default 1 2 4 8\n"
    "  --runs N                  runs per sample count. Default 3\n"
    "  --frames N                frames per run. Default 900\n"
    "  --timeout N               seconds per run. Default 300\n"
    "  --zones                   one row per zone with median/min/max, not the sweep table\n"
    "  --startup                 frame 0 alone, with no medians\n"
    "  --read <trace.json>       report a trace that already exists, running nothing\n";

struct Options {
    Config config = Config::Release;
    std::vector<int> samples{1, 2, 4, 8};
    int runs = 3;
    int frames = 900;
    int timeout = 300;
    bool zones = false;
    bool startup = false;

    /// Report a trace that already exists instead of producing one. The parse is the part
    /// worth re-running: a trace outlives the machine state that produced it.
    fs::path read;
    std::vector<std::string> engineArgs;
};

/// The executable a build directory holds. `build-game <name>` records the name in the CMake
/// cache, which is why this reads it rather than taking a game argument: the choice of game
/// is a property of the build directory, not a flag on every tool.
fs::path configuredGame(Config config) {
    const std::string game = cachedGame(config);
    if (game.empty()) {
        std::fprintf(stderr,
                     "error: build/%s holds no game -- `substrate build` produces a library and\n"
                     "       a test binary, and nothing to run. Run: substrate build-game <name> %s\n",
                     name(config), name(config));
        return {};
    }
    const fs::path binary = buildDir(config) / executableName(game);
    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        std::fprintf(stderr, "error: %s not built. Run: substrate build-game %s %s\n",
                     binary.generic_string().c_str(), game.c_str(), name(config));
        return {};
    }
    return binary;
}

/// One run, into a trace this deletes on the way out.
bool runOnce(const Options& options, int samples, Trace& out) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("substrate-bench-" + std::to_string(samples));
    fs::create_directories(dir, ec);
    const fs::path trace = dir / "trace.json";
    fs::remove(trace, ec);

    // --locked is pinned rather than inherited: the engine ships a realtime clock, which
    // steps the simulation from wall-clock time, so the number of animation, particle and
    // physics steps in a frame would become a function of how fast the frame rendered and a
    // per-pass table would be measuring its own frame rate.
    //
    // --headless unless the caller asked for a window. A sweep is twelve runs, and twelve
    // windows mapping in turn take the keyboard from whoever is working while it measures.
    // The window is unmapped rather than absent, so the surface, swapchain and present path
    // are the ones a visible run uses.
    std::vector<std::string> extra;
    bool windowed = false;
    for (const std::string& arg : options.engineArgs) {
        if (arg == "--windowed") {
            windowed = true;
            continue;
        }
        extra.push_back(arg);
    }

    std::vector<std::string> command{"run", name(options.config), "--", "--locked", "--audio-null"};
    if (!windowed) command.push_back("--headless");
    command.insert(command.end(), {"--msaa", std::to_string(samples), "--frames",
                                   std::to_string(options.frames), "--trace", trace.string()});
    command.insert(command.end(), extra.begin(), extra.end());

    // Re-entering this binary rather than a shell script, so a harness and a hand-run
    // command take exactly the same path into the engine.
    std::vector<std::string> argv{selfPath().string()};
    argv.insert(argv.end(), command.begin(), command.end());

    RunOptions runOptions;
    runOptions.cwd = repoRoot();
    runOptions.timeoutSeconds = options.timeout;
    const RunResult result = run(argv, runOptions);

    if (!fs::exists(trace, ec)) {
        // A failed run must not quietly become a missing sample the median then papers over.
        std::fprintf(stderr, "  run failed: no trace written (exit %d)\n", result.exitCode);
        return false;
    }

    std::string error;
    const bool read = readTrace(trace, out, error);
    fs::remove_all(dir, ec);
    if (!read) {
        std::fprintf(stderr, "  run failed: %s\n", error.c_str());
        return false;
    }
    if (out.empty()) {
        std::fputs("  run failed: trace holds no frames past frame 0\n", stderr);
        return false;
    }
    return true;
}

void pool(std::map<std::string, std::vector<double>>& into,
          const std::map<std::string, std::vector<double>>& from) {
    for (const auto& [key, values] : from) {
        std::vector<double>& target = into[key];
        target.insert(target.end(), values.begin(), values.end());
    }
}

size_t widestKey(const std::map<std::string, std::vector<double>>& table, size_t floor) {
    size_t width = floor;
    for (const auto& [key, values] : table) width = std::max(width, key.size());
    return width;
}

void printZones(const Options& options, int samples, const Trace& trace) {
    const int width = static_cast<int>(widestKey(trace.cpu, 18));
    std::string flags;
    for (const std::string& arg : options.engineArgs) flags += (flags.empty() ? "" : " ") + arg;
    if (flags.empty()) flags = "engine defaults";

    std::printf("\n%dx MSAA, %zu frames, %s, %s\n", samples, trace.wallMs.size(),
                name(options.config), flags.c_str());
    std::printf("%-*s %8s %8s %8s\n", width, "GPU zone", "median", "min", "max");

    // `Frame` last, so the zones it contains read as a list and then their total.
    std::vector<std::string> gpuNames;
    for (const auto& [zone, values] : trace.gpu) gpuNames.push_back(zone);
    std::sort(gpuNames.begin(), gpuNames.end(), [](const std::string& a, const std::string& b) {
        return std::make_pair(a == "Frame", a) < std::make_pair(b == "Frame", b);
    });
    for (const std::string& zone : gpuNames) {
        const std::vector<double>& v = trace.gpu.at(zone);
        std::printf("%-*s %8.3f %8.3f %8.3f\n", width, zone.c_str(), median(v), minimum(v),
                    maximum(v));
    }

    // The row that says whether the list above is complete, and the only honest way to ask
    // it. **Do not answer this by subtracting the sum of the medians above from `Frame`** --
    // that difference is about half a millisecond on a scene where every timestamp says the
    // frame is fully attributed. See unnamedGpuMs.
    if (!trace.unnamedMs.empty()) {
        std::printf("%-*s %8.3f %8.3f %8.3f\n", width, "unnamed", median(trace.unnamedMs),
                    minimum(trace.unnamedMs), maximum(trace.unnamedMs));
    }

    // `total/frame` is the pooled sum rather than a per-frame one: a zone that records twice
    // in a frame -- the G-buffer's two phases, the two cull dispatches -- would otherwise
    // report a median that understates what the frame paid for it by half.
    std::printf("\n%-*s %8s %8s %8s %12s\n", width, "CPU zone", "median", "min", "max",
                "total/frame");
    const double traced = static_cast<double>(trace.wallMs.size());
    for (const auto& [zone, v] : trace.cpu) {
        const double total = std::accumulate(v.begin(), v.end(), 0.0);
        std::printf("%-*s %8.3f %8.3f %8.3f %12.3f\n", width, zone.c_str(), median(v), minimum(v),
                    maximum(v), traced > 0 ? total / traced : 0.0);
    }
    std::printf("%-*s %8.3f %8.3f %8.3f\n", width, "wall frame", median(trace.wallMs),
                minimum(trace.wallMs), maximum(trace.wallMs));
    std::printf("%-*s %8.3f %8.3f %8.3f\n", width, "CPU busy", median(trace.busyMs),
                minimum(trace.busyMs), maximum(trace.busyMs));

    // Quantities, not durations, so they get their own block and no total: summing a
    // draw-call count over the window says nothing anybody wants to know.
    if (!trace.counters.empty()) {
        std::printf("\n%-*s %8s %8s %8s\n", width, "counter", "median", "min", "max");
        for (const auto& [counter, v] : trace.counters) {
            std::printf("%-*s %8.1f %8.1f %8.1f\n", width, counter.c_str(), median(v), minimum(v),
                        maximum(v));
        }
    }
}

void printStartup(const Options& options, int samples, const Trace& trace) {
    if (trace.startup.empty()) {
        std::fprintf(stderr, "\n%dx MSAA, %s: frame 0 holds no zones\n", samples,
                     name(options.config));
        return;
    }

    size_t width = 26;
    for (const auto& [path, zone] : trace.startup) width = std::max(width, path.size());

    const auto frame = trace.startup.find("Frame");
    const double total = frame == trace.startup.end() ? 0.0 : frame->second.totalMs;

    std::string flags;
    for (const std::string& arg : options.engineArgs) flags += (flags.empty() ? "" : " ") + arg;
    if (flags.empty()) flags = "engine defaults";

    // Indented by path depth and sorted by path, so a step sits under the one that called it
    // and a level sums against its parent by eye. **No medians**: there is exactly one
    // startup per run by construction, and a median column would be a median of one.
    std::printf("\n%dx MSAA, %s, %s -- startup (frame 0)\n", samples, name(options.config),
                flags.c_str());
    std::printf("%-*s %9s %4s %13s\n", static_cast<int>(width), "zone", "ms", "n", "% of frame 0");

    double named = 0.0;
    for (const auto& [path, zone] : trace.startup) {
        const double share = total > 0.0 ? 100.0 * zone.totalMs / total : 0.0;
        const size_t depth = static_cast<size_t>(std::count(path.begin(), path.end(), '/'));
        const size_t lastSlash = path.rfind('/');
        const std::string leaf = lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
        std::printf("%-*s %9.3f %4d %12.1f%%\n", static_cast<int>(width),
                    (std::string(depth * 2, ' ') + leaf).c_str(), zone.totalMs, zone.count, share);

        // Direct children of Frame only, so the residual is Frame's own time rather than
        // Frame less every level of the tree counted once per level.
        if (path.rfind("Frame/", 0) == 0 && path.find('/', 6) == std::string::npos) {
            named += zone.totalMs;
        }
    }
    const double share = total > 0.0 ? 100.0 * (total - named) / total : 0.0;
    std::printf("%-*s %9.3f %4s %12.1f%%\n", static_cast<int>(width), "unnamed", total - named, "",
                share);
}

void printTable(const Options& options, const std::map<int, Trace>& results) {
    std::string header = "| MSAA";
    for (const char* zone : kTableZones) header += std::string(" | ") + zone;
    header += " | GPU frame | wall | CPU busy | FPS | VRAM |";
    std::printf("%s\n", header.c_str());

    const size_t columns = std::size(kTableZones) + 6;
    std::string rule = "|";
    for (size_t i = 0; i < columns; ++i) rule += "---|";
    std::printf("%s\n", rule.c_str());

    for (int samples : options.samples) {
        const auto found = results.find(samples);
        if (found == results.end()) continue;
        const Trace& trace = found->second;

        std::string row = "| " + std::to_string(samples) + "x";
        char cell[64];
        for (const char* zone : kTableZones) {
            const auto values = trace.gpu.find(zone);
            std::snprintf(cell, sizeof(cell), "%.3f",
                          values == trace.gpu.end() ? 0.0 : median(values->second));
            row += std::string(" | ") + cell;
        }
        const auto frame = trace.gpu.find("Frame");
        std::snprintf(cell, sizeof(cell), "%.3f",
                      frame == trace.gpu.end() ? 0.0 : median(frame->second));
        row += std::string(" | ") + cell;

        const double wallMs = median(trace.wallMs);
        std::snprintf(cell, sizeof(cell), "%.3f", wallMs);
        row += std::string(" | ") + cell;
        std::snprintf(cell, sizeof(cell), "%.3f", median(trace.busyMs));
        row += std::string(" | ") + cell;

        // FPS comes from wall time, the only one of the three that is a frame rate: CPU busy
        // would report the rate of a machine with an infinite GPU.
        if (wallMs > 0.0) {
            std::snprintf(cell, sizeof(cell), "%.0f", 1000.0 / wallMs);
        } else {
            std::snprintf(cell, sizeof(cell), "-");
        }
        row += std::string(" | ") + cell;

        const auto vram = trace.counters.find("vramMiB");
        std::snprintf(cell, sizeof(cell), "%.1f",
                      vram == trace.counters.end() ? 0.0 : median(vram->second));
        row += std::string(" | ") + cell + " |";
        std::printf("%s\n", row.c_str());
    }

    std::string flags;
    for (const std::string& arg : options.engineArgs) flags += (flags.empty() ? "" : " ") + arg;
    if (flags.empty()) flags = "engine defaults";
    std::printf("\nAll figures in ms. Medians over every traced frame of %d runs of %d frames, "
                "%s build, %s. VRAM in MiB.\n",
                options.runs, options.frames, name(options.config), flags.c_str());
    std::printf("`wall` is frame-to-frame time and paces FPS; `CPU busy` is wall less the frame's "
                "waitFence, acquire and\npresent. Where they are far apart the GPU is the limiter "
                "and the CPU has that much headroom.\n");
}

bool parseOptions(const std::vector<std::string>& args, Options& out) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            out.engineArgs.assign(args.begin() + static_cast<long>(i) + 1, args.end());
            return true;
        }
        if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stderr);
            return false;
        }
        if (arg == "--read" && i + 1 < args.size()) {
            out.read = args[++i];
        } else if (arg == "--zones") {
            out.zones = true;
        } else if (arg == "--startup") {
            out.startup = true;
        } else if (arg == "--config" && i + 1 < args.size()) {
            const std::optional<Config> config = parseConfig(args[++i]);
            if (!config) {
                std::fprintf(stderr, "error: unknown config '%s' (want: %s)\n", args[i].c_str(),
                             configList().c_str());
                return false;
            }
            out.config = *config;
        } else if (arg == "--runs" && i + 1 < args.size()) {
            out.runs = std::atoi(args[++i].c_str());
        } else if (arg == "--frames" && i + 1 < args.size()) {
            out.frames = std::atoi(args[++i].c_str());
        } else if (arg == "--timeout" && i + 1 < args.size()) {
            out.timeout = std::atoi(args[++i].c_str());
        } else if (arg == "--samples") {
            out.samples.clear();
            while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) {
                out.samples.push_back(std::atoi(args[++i].c_str()));
            }
            if (out.samples.empty()) {
                std::fputs("error: --samples needs at least one count\n", stderr);
                return false;
            }
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
            std::fputs("       Everything meant for the engine goes after '--'.\n", stderr);
            return false;
        }
    }
    return true;
}

} // namespace

int cmdBench(const std::vector<std::string>& args) {
    Options options;
    if (!parseOptions(args, options)) return args.empty() ? 0 : 1;

    if (!options.read.empty()) {
        Trace trace;
        std::string error;
        if (!readTrace(options.read, trace, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        const int samples = options.samples.empty() ? 0 : options.samples.front();
        if (options.startup) {
            printStartup(options, samples, trace);
        } else {
            printZones(options, samples, trace);
        }
        return 0;
    }

    if (configuredGame(options.config).empty()) return 1;

    std::map<int, Trace> results;
    for (int samples : options.samples) {
        Trace pooled;
        for (int attempt = 0; attempt < options.runs; ++attempt) {
            Trace one;
            if (!runOnce(options, samples, one)) continue;

            pool(pooled.gpu, one.gpu);
            pool(pooled.cpu, one.cpu);
            pool(pooled.counters, one.counters);
            pooled.wallMs.insert(pooled.wallMs.end(), one.wallMs.begin(), one.wallMs.end());
            pooled.busyMs.insert(pooled.busyMs.end(), one.busyMs.begin(), one.busyMs.end());
            pooled.unnamedMs.insert(pooled.unnamedMs.end(), one.unnamedMs.begin(),
                                    one.unnamedMs.end());
            // Last run wins rather than pooled: there is one startup per run, and averaging
            // two of them would hide that the second was warm.
            if (!one.startup.empty()) pooled.startup = one.startup;
        }
        if (pooled.wallMs.empty()) {
            std::fprintf(stderr, "error: every run at %dx failed\n", samples);
            return 1;
        }
        std::fprintf(stderr, "    %dx: %zu frames\n", samples, pooled.wallMs.size());
        results.emplace(samples, std::move(pooled));
    }

    for (int samples : options.samples) {
        const auto found = results.find(samples);
        if (found == results.end()) continue;
        if (options.startup) {
            printStartup(options, samples, found->second);
        } else if (options.zones) {
            printZones(options, samples, found->second);
        }
    }
    if (!options.startup && !options.zones) printTable(options, results);
    return 0;
}

} // namespace tool
