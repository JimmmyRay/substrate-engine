#include "Harness.h"

#include "Process.h"
#include "Repo.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr int kFrame = 60;
constexpr int kFrames = 90;
constexpr int kTimeout = 120;

/// Every case names its scene explicitly, including the ones that are Sponza. They used to
/// rely on `scene.path` from substrate.json, which made those baselines depend on a config
/// value -- so the day the default scene changed they would have failed for a reason with
/// nothing to do with the renderer.
constexpr const char* kSponza = "engine/assets/Sponza/glTF/Sponza.gltf";

struct Case {
    const char* name;
    std::vector<std::string> flags;
};

/// One entry per configuration worth pinning. Kept small on purpose: each is a full run of
/// the engine, and a suite nobody waits for is a suite nobody runs.
std::vector<Case> cases() {
    return {
        {"lit", {kSponza}},
        {"albedo", {kSponza, "--debug-view", "albedo"}},
        {"normal", {kSponza, "--debug-view", "normal"}},
        {"depth", {kSponza, "--debug-view", "depth"}},
        // The AO buffer, upsampled into the full-resolution swapchain by the debug view
        // rather than captured at its own size -- so this stays diffable against its
        // baseline if the pass ever changes resolution, which a target dump would not.
        {"ssao", {kSponza, "--debug-view", "ssao"}},
        {"msaa1", {kSponza, "--msaa", "1"}},
        {"no-rt", {kSponza, "--no-rt"}},
        // A light inside the emissive mesh that represents it. Emissive geometry is built
        // non-opaque in the BLAS so a shadow ray passes through it; a regression here takes
        // the ground from lit to black.
        {"emissive", {"engine/assets/emissive.gltf"}},
        // Pins the particle subsystem's determinism, which is not free: the sort is a fixed
        // comparison network and the slot allocator runs on the CPU precisely so that frame
        // 60 is the same frame 60 on every run.
        {"particles", {"engine/assets/particles.gltf"}},
        // The only case with a skinned caster. Without it the suite could not see a bug that
        // corrupted every skinned cascade shadow: the shadow pass drew the whole command
        // list with the scene's vertex buffer bound, while the skinned half of that list
        // carries a vertexOffset into the skinned buffer. Sponza has no skin, so every other
        // case agreed on a frame that was wrong everywhere it mattered.
        {"skin", {"engine/assets/skin.gltf"}},
        // The only case with a solver in it. A settling stack is the most sensitive thing in
        // this suite to a step order or a thread count changing, and it would drift silently
        // rather than break.
        {"physics", {"engine/assets/physics.gltf"}},
        // The only two cases with a smooth surface, and the reason they are two is that the
        // reflection pass is two algorithms rather than one setting. `no-rt` above renders
        // Sponza, which has no smooth surface anywhere, so the pass it is named for
        // contributes almost nothing to the image it pins.
        {"mirror", {"engine/assets/mirror.gltf"}},
        {"mirror-no-rt", {"engine/assets/mirror.gltf", "--no-rt"}},
    };
}

/// The first capture of `pattern` in the log, or empty.
std::string scrape(const std::string& log, const std::string& prefix, char stop) {
    const size_t at = log.find(prefix);
    if (at == std::string::npos) return {};
    const size_t from = at + prefix.size();
    size_t to = from;
    while (to < log.size() && log[to] != '\n' && log[to] != stop) ++to;
    std::string value = log.substr(from, to - from);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

/// Substrate logs the device it selected. Check it rather than trust it, because the failure
/// this catches is otherwise silent: anything that costs the discrete card its presentation
/// support makes VulkanContext fall through to a software rasteriser, and a suite of
/// CPU-rendered pixels compared against CPU-rendered baselines passes while testing nothing.
///
/// Returns the offending device name, or empty when the device is real.
std::string softwareDevice(const std::string& log) {
    const std::string device = scrape(log, "Device: ", '(');
    if (device.empty()) return "no device line";

    static const char* const kSoftware[] = {"llvmpipe", "lavapipe", "softpipe", "SwiftShader",
                                            "Software Rasterizer"};
    for (const char* needle : kSoftware) {
        if (device.find(needle) != std::string::npos) return device;
    }
    return {};
}

/// The counterpart to softwareDevice, for the same class of mistake: a device without the
/// ray-query extensions runs a ray-traced case through the raster fallback and produces a
/// baseline that looks fine and tests nothing -- worse than a failure, because it passes.
bool rayQueryMissing(const std::string& log) {
    const std::string state = scrape(log, "Ray query: ", '\0');
    return state.rfind("available", 0) != 0;
}

/// The last `count` lines, indented, for a harness failure's context.
std::string tail(const std::string& text, size_t count) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::string out;
    for (size_t i = lines.size() > count ? lines.size() - count : 0; i < lines.size(); ++i) {
        out += "         | " + lines[i] + "\n";
    }
    return out;
}

/// **Kept, because the next run overwrites them.** A failing case writes its actual, diff and
/// log into the same paths the next run uses, so re-running to "see if it happens again"
/// destroys the only evidence of the run that failed.
///
/// The directory name is the run's own sequence number rather than a timestamp, so this needs
/// no clock and sorts in the order the failures happened.
fs::path keepArtifacts(const fs::path& dir, const std::vector<std::string>& names) {
    std::error_code ec;
    const fs::path keep = dir / "failed";
    int n = 1;
    while (fs::exists(keep / std::to_string(n), ec)) ++n;
    const fs::path into = keep / std::to_string(n);
    fs::create_directories(into, ec);

    for (const std::string& name : names) {
        fs::copy_file(dir / (name + ".log"), into / (name + ".log"),
                      fs::copy_options::overwrite_existing, ec);
        for (const char* suffix : {".actual.png", ".diff.png"}) {
            fs::copy_file(dir / (name + suffix), into / (name + suffix),
                          fs::copy_options::overwrite_existing, ec);
        }
        fs::copy_file(dir / (name + ".png"), into / (name + ".expected.png"),
                      fs::copy_options::overwrite_existing, ec);
    }
    return into;
}

} // namespace

int cmdGolden(const std::vector<std::string>& args) {
    std::string mode = "check";
    Config config = Config::Release;

    for (const std::string& arg : args) {
        if (arg == "snap" || arg == "check") {
            mode = arg;
        } else if (const std::optional<Config> parsed = parseConfig(arg)) {
            config = *parsed;
        } else if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate golden [snap|check] [config]\n"
                       "\n"
                       "  snap    capture the current build as the baseline\n"
                       "  check   capture again and compare against it\n"
                       "\n"
                       "Exit 1 is an image difference, 2 is a harness failure. They are not the\n"
                       "same news and only one of them is about the change under test.\n",
                       stderr);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s' (want: snap|check, %s)\n",
                         arg.c_str(), configList().c_str());
            return 2;
        }
    }

    const fs::path dir = repoRoot() / "debug_frames" / "golden";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::vector<Case> all = cases();
    std::vector<std::string> failed;
    std::vector<std::string> harness;

    for (const Case& item : all) {
        const std::string name = item.name;
        const fs::path golden = dir / (name + ".png");
        const fs::path log = dir / (name + ".log");
        const bool rayTraced = name.rfind("rt-", 0) == 0;

        std::vector<std::string> command{selfPath().string(), "run", tool::name(config), "--",
                                         "--headless", "--locked", "--audio-null", "--frames",
                                         std::to_string(kFrames), "--capture-frame",
                                         std::to_string(kFrame)};

        if (mode == "snap") {
            command.insert(command.end(), {"--capture", golden.string()});
        } else {
            if (!fs::is_regular_file(golden, ec)) {
                std::printf("MISS  %s (no baseline; run: substrate golden snap)\n", name.c_str());
                failed.push_back(name);
                continue;
            }
            // Nothing from a previous run may survive into this one. A case that never draws
            // leaves the last run's images at these paths, and the keep step then files them
            // under this run's number -- so an intermittent failure gets investigated by
            // diffing two images from a run that rendered correctly.
            fs::remove(dir / (name + ".actual.png"), ec);
            fs::remove(dir / (name + ".diff.png"), ec);
            fs::remove(log, ec);

            command.insert(command.end(), {"--capture", (dir / (name + ".actual.png")).string(),
                                           "--golden", golden.string(), "--diff",
                                           (dir / (name + ".diff.png")).string()});
        }
        command.insert(command.end(), item.flags.begin(), item.flags.end());

        RunOptions options;
        options.cwd = repoRoot();
        options.timeoutSeconds = kTimeout;
        const RunResult result = run(command, options);

        // One stream, because the shell redirected both into one file and every predicate
        // below was written against that.
        const std::string output = stripAnsi(result.out + result.err);
        std::ofstream(log, std::ios::binary) << output;

        if (mode == "snap") {
            if (!result.ok()) {
                std::fputs(output.c_str(), stderr);
                std::fprintf(stderr, "snap %s failed\n", name.c_str());
                return 1;
            }
            if (const std::string device = softwareDevice(output); !device.empty()) {
                std::fprintf(stderr, "snap %s ran on %s -- refusing to baseline it\n",
                             name.c_str(), device.c_str());
                return 1;
            }
            if (rayTraced && rayQueryMissing(output)) {
                std::fprintf(stderr, "snap %s has no ray query on this device -- refusing to "
                                     "baseline the raster path\n", name.c_str());
                return 1;
            }
            std::printf("snap  %s\n", name.c_str());
            continue;
        }

        if (!result.ok()) {
            // An image that changed and a run that never happened are different failures, and
            // only one of them is a regression. Every comparison the engine performs logs a
            // `Compare:` verdict, so a non-zero exit without one came from upstream of the
            // comparison -- a vanished binary, a timeout, a scene that would not load.
            if (output.find("Compare: MISMATCH") != std::string::npos ||
                output.find("Compare: size mismatch") != std::string::npos) {
                std::printf("FAIL  %s -- see %s (%s)\n", name.c_str(),
                            (dir / (name + ".diff.png")).generic_string().c_str(),
                            log.generic_string().c_str());
                failed.push_back(name);
                continue;
            }
            std::printf("HARNESS  %s -- the run did not reach a comparison (%s)\n", name.c_str(),
                        log.generic_string().c_str());
            std::fputs(tail(output, 3).c_str(), stdout);
            harness.push_back(name);
            // Stop rather than carry on. Whatever stopped this case will stop the rest for the
            // same reason, and ten more identical failures read as a total regression instead
            // of as one broken harness.
            break;
        }

        // After the comparison, not instead of it: a match on the wrong device is still wrong.
        if (const std::string device = softwareDevice(output); !device.empty()) {
            std::printf("FAIL  %s -- ran on %s, not the GPU (%s)\n", name.c_str(), device.c_str(),
                        log.generic_string().c_str());
            failed.push_back(name);
            continue;
        }
        if (rayTraced && rayQueryMissing(output)) {
            std::printf("FAIL  %s -- no ray query on this device, so this compared the raster "
                        "path (%s)\n", name.c_str(), log.generic_string().c_str());
            failed.push_back(name);
            continue;
        }

        std::printf("ok    %s\n", name.c_str());
    }

    if (mode != "check") return 0;

    if (!harness.empty()) {
        std::vector<std::string> keep = harness;
        keep.insert(keep.end(), failed.begin(), failed.end());
        const fs::path into = keepArtifacts(dir, keep);

        std::string names;
        for (const std::string& name : harness) names += (names.empty() ? "" : " ") + name;
        std::printf("harness failure: %s -- the suite did not run to completion\n", names.c_str());
        std::printf("kept: %s\n", into.generic_string().c_str());
        // Distinct from 1 so a caller can tell "the renderer changed" from "the suite never
        // rendered". Only one of them is stop-the-line for the change under test.
        return 2;
    }

    if (failed.empty()) {
        std::printf("all %zu cases match\n", all.size());
        return 0;
    }

    const fs::path into = keepArtifacts(dir, failed);
    std::string names;
    for (const std::string& name : failed) names += (names.empty() ? "" : " ") + name;
    std::printf("%zu of %zu cases differ\n", failed.size(), all.size());
    std::printf("kept: %s (%s)\n", into.generic_string().c_str(), names.c_str());
    return 1;
}

} // namespace tool
