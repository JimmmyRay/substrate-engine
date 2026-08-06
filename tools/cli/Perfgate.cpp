#include "Bench.h"

#include "Process.h"
#include "Repo.h"
#include "Trace.h"

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

/// The arm. Fixed on purpose: a gate whose scene or camera moved with the tree would compare
/// two different pictures and call the difference a regression. `--locked` pins the clock,
/// `--headless` never maps a window so a commit hook cannot steal focus, and the camera is
/// written out rather than left to `frameBounds`, which follows the scene's bounds and would
/// shift the moment an asset did.
const char* const kArm[] = {"--locked",  "--audio-null", "--headless", "--msaa", "4",
                            "--camera",  "0.00,1.16,1.80,63.6,-12.6,7.87"};

constexpr int kFrames = 300;
const char* const kZones[] = {"Lighting", "Frame"};

/// 12%. Wide enough that ordinary machine noise does not fail a commit, narrow enough that a
/// real regression in either gated zone does.
constexpr double kMargin = 1.12;

/// Per-frame **minimum**, not median: the cheapest frame is the one least contaminated by
/// whatever else the machine was doing, which is what makes this a gate rather than a
/// weather report.
bool measure(Config config, std::map<std::string, double>& out) {
    const fs::path binary = buildDir(config) / executableName("demo");
    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        std::fprintf(stderr, "perfgate: %s is not built. Run: substrate build-game demo %s\n",
                     binary.generic_string().c_str(), name(config));
        return false;
    }

    const fs::path dir = fs::temp_directory_path(ec) / "substrate-perfgate";
    fs::create_directories(dir, ec);
    const fs::path trace = dir / "trace.json";
    fs::remove(trace, ec);

    std::vector<std::string> argv{selfPath().string(), "run", name(config), "--"};
    for (const char* arg : kArm) argv.push_back(arg);
    argv.insert(argv.end(), {"--frames", std::to_string(kFrames), "--trace", trace.string()});

    RunOptions options;
    options.cwd = repoRoot();
    options.timeoutSeconds = 180;
    const RunResult result = run(argv, options);

    if (!fs::exists(trace, ec)) {
        std::fprintf(stderr, "perfgate: no trace written (exit %d); not measured\n",
                     result.exitCode);
        const size_t tail = result.err.size() > 2000 ? result.err.size() - 2000 : 0;
        std::fprintf(stderr, "%s\n", result.err.substr(tail).c_str());
        return false;
    }

    Trace parsed;
    std::string error;
    const bool read = readTrace(trace, parsed, error);
    fs::remove_all(dir, ec);
    if (!read) {
        std::fprintf(stderr, "perfgate: %s\n", error.c_str());
        return false;
    }

    for (const char* zone : kZones) {
        const auto found = parsed.gpu.find(zone);
        if (found == parsed.gpu.end()) continue;
        // A zone that appears in under a third of the frames is a zone whose pass did not
        // run for most of the trace -- reporting a minimum over six samples as this tree's
        // cost is how a gate ends up defending a number that means nothing.
        if (found->second.size() < static_cast<size_t>(kFrames / 3)) continue;
        out[zone] = minimum(found->second);
    }
    return true;
}

fs::path budgetPath() { return repoRoot() / "perf-budget.json"; }

void writeBudget(Config config, const std::map<std::string, double>& measured) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"config\": \"" << name(config) << "\",\n";
    json << "  \"frames\": " << kFrames << ",\n";
    json << "  \"arm\": [";
    for (size_t i = 0; i < std::size(kArm); ++i) {
        json << (i ? ", " : "") << '"' << kArm[i] << '"';
    }
    json << "],\n";
    json << "  \"marginPct\": " << static_cast<int>(std::lround((kMargin - 1.0) * 100)) << ",\n";
    json << "  \"note\": \"Per-frame minimum in ms on this machine. `substrate perfgate "
            "--update` rewrites it.\",\n";
    json << "  \"zones\": {\n";
    size_t index = 0;
    for (const auto& [zone, ms] : measured) {
        char cell[64];
        std::snprintf(cell, sizeof(cell), "%.4f", ms);
        json << "    \"" << zone << "\": " << cell
             << (++index == measured.size() ? "\n" : ",\n");
    }
    json << "  }\n}\n";

    std::ofstream out(budgetPath());
    out << json.str();
}

} // namespace

int cmdPerfgate(const std::vector<std::string>& args) {
    Config config = Config::Release;
    bool update = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--update") {
            update = true;
        } else if (args[i] == "--config" && i + 1 < args.size()) {
            const std::optional<Config> parsed = parseConfig(args[++i]);
            if (!parsed) {
                std::fprintf(stderr, "error: unknown config '%s' (want: %s)\n", args[i].c_str(),
                             configList().c_str());
                return 2;
            }
            config = *parsed;
        } else if (args[i] == "-h" || args[i] == "--help") {
            std::fputs("usage: substrate perfgate [--config <cfg>] [--update]\n", stderr);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", args[i].c_str());
            return 2;
        }
    }

    std::map<std::string, double> now;
    if (!measure(config, now)) return 2;
    if (now.empty()) {
        std::fputs("perfgate: the trace held none of the gated zones; not measured\n", stderr);
        return 2;
    }

    std::error_code ec;
    if (update || !fs::exists(budgetPath(), ec)) {
        writeBudget(config, now);
        std::printf("perfgate: %s perf-budget.json --", update ? "updated" : "created");
        for (const auto& [zone, ms] : now) std::printf("  %s %.3f", zone.c_str(), ms);
        std::printf("\n");
        return 0;
    }

    std::ifstream in(budgetPath());
    rapidjson::IStreamWrapper stream(in);
    rapidjson::Document budget;
    budget.ParseStream(stream);
    if (budget.HasParseError() || !budget.IsObject()) {
        std::fputs("perfgate: perf-budget.json is malformed; not comparable\n", stderr);
        return 2;
    }

    const std::string recordedConfig =
        budget.HasMember("config") ? budget["config"].GetString() : "";
    if (recordedConfig != name(config)) {
        std::fprintf(stderr, "perfgate: budget is for %s, measured %s; not comparable\n",
                     recordedConfig.c_str(), name(config));
        return 2;
    }
    if (!budget.HasMember("zones") || !budget["zones"].IsObject()) {
        std::fputs("perfgate: perf-budget.json records no zones; not comparable\n", stderr);
        return 2;
    }

    struct Over {
        std::string zone;
        double got = 0.0;
        double recorded = 0.0;
    };
    std::vector<Over> over;

    for (const auto& [zone, ms] : now) {
        const rapidjson::Value& zones = budget["zones"];
        if (!zones.HasMember(zone.c_str())) continue;
        const double recorded = zones[zone.c_str()].GetDouble();
        const double limit = recorded * kMargin;
        std::printf("  %-10s %7.3f ms   budget %.3f  limit %.3f  %s\n", zone.c_str(), ms,
                    recorded, limit, ms > limit ? "OVER" : "ok");
        if (ms > limit) over.push_back({zone, ms, recorded});
    }

    if (over.empty()) {
        std::puts("perfgate: inside budget");
        return 0;
    }

    std::fputs("\nperfgate: the frame got slower.\n", stderr);
    for (const Over& entry : over) {
        std::fprintf(stderr, "  %s %.3f ms against %.3f (%+.1f%%)\n", entry.zone.c_str(),
                     entry.got, entry.recorded, (entry.got / entry.recorded - 1.0) * 100.0);
    }
    std::fputs("\nThis is a minimum over 300 frames, so it is not machine load. Either the "
               "change\nmade the frame slower, or the cost is understood and intended -- in "
               "which case\nre-baseline on purpose with `substrate perfgate --update` and say "
               "why in the\ncommit. `git commit --no-verify` skips the gate and leaves no "
               "record that it did.\n",
               stderr);
    return 1;
}

} // namespace tool
