#include "Harness.h"

#include "Process.h"
#include "Repo.h"

#include <cstdio>
#include <fstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr int kFrame = 60;
constexpr int kFrames = 90;
constexpr int kTimeout = 120;
constexpr const char* kImage = "res:/readback.png";

struct Case {
    const char* name;
    int width;
    int height;
    std::vector<std::string> flags;
};

/// Nine cases, each presenting a known PNG and comparing the swapchain bit-exactly against
/// the source expanded by the integer scale.
///
/// The two sheet cases land mid-cell rather than on a boundary, and the slack is the point:
/// an assertion on a cell boundary would make this a question about float accumulation
/// rather than about frame selection, and it would pass and fail at random.
std::vector<Case> cases() {
    return {
        {"native-inside", 960, 540, {"--virtual-resolution", "native"}},
        {"native-outside", 960, 540, {"--virtual-resolution", "native", "--ui-outside-virtual"}},
        {"scale3-inside", 960, 540, {"--virtual-resolution", "320x180"}},
        {"scale3-outside", 960, 540, {"--virtual-resolution", "320x180", "--ui-outside-virtual"}},
        {"letterbox", 1000, 600, {"--virtual-resolution", "320x180"}},
        {"sprite", 960, 540, {"--virtual-resolution", "320x180", "--readback-sprite"}},
        {"sprite-letterbox", 1000, 600,
         {"--virtual-resolution", "320x180", "--readback-sprite", "--ui-outside-virtual"}},
        {"sheet-cell1", 960, 540,
         {"--virtual-resolution", "320x180", "--readback-sheet-fps", "1.5",
          "--readback-sheet-frame", "1"}},
        {"sheet-cell2", 1000, 600,
         {"--virtual-resolution", "320x180", "--readback-sheet-fps", "2.5",
          "--readback-sheet-frame", "2"}},
    };
}

/// **The one case whose expectation is not a value.** Every case above holds a texel against
/// the file it came from. A *lit* sprite goes through the G-buffer, the lighting pass and the
/// tonemapper, so its value is corrected four times over -- bit-exactness is definitionally
/// unavailable to it, and a snapped reference is the standard this arc exists not to use.
///
/// So the claim moves from the value to the **coverage**, which lighting cannot change. Two
/// runs differing in one number: `--readback-lit-cutoff 2` puts the cutoff above every alpha
/// there is, so every fragment discards and the sprite disappears while the material, the
/// instance, the indirect command and the pipeline stay exactly as they were. That is a
/// stronger control than omitting the sprite, which would also change the draw list.
constexpr const char* kLitImage = "res:/cutout.png";

std::vector<std::string> litFlags() {
    // The effects that bleed across a silhouette are off, and that is not a dodge: bloom,
    // SSAO and SSR legitimately change pixels outside a sprite's outline. What is asserted
    // is where the sprite is, not what a post pass does with it.
    return {"--virtual-resolution", "320x180", "--readback-lit-sprite",
            "--no-bloom", "--no-ssao", "--no-ssr", "--no-rt"};
}

/// The verdict line, from `token` to the end of it.
///
/// The engine logs two `Readback:` lines -- the setup and then the comparison -- so the token
/// the verdict opens with is part of the pattern. Matching the label alone reports "drawing
/// slot 1 at 1:1" as the result of the comparison, which is a different sentence that also
/// looks like an answer.
std::string verdict(const std::string& log, const std::string& label, const std::string& token) {
    const size_t at = log.find(label + token);
    if (at == std::string::npos) return {};
    const size_t from = at + label.size();
    const size_t end = log.find('\n', from);
    std::string value = log.substr(from, end == std::string::npos ? end : end - from);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    return value;
}

void writeLog(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

RunResult launch(Config config, const std::vector<std::string>& extra, int timeout) {
    std::vector<std::string> command{selfPath().string(), "run", name(config), "--",
                                     "--headless", "--locked", "--audio-null"};
    command.insert(command.end(), extra.begin(), extra.end());

    RunOptions options;
    options.cwd = repoRoot();
    options.timeoutSeconds = timeout;
    return run(command, options);
}

} // namespace

int cmdReadback(const std::vector<std::string>& args) {
    Config config = Config::Release;
    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate readback [config]\n"
                       "\n"
                       "Presents a known PNG and compares the swapchain bit-exactly against the\n"
                       "source expanded by the integer scale. The expectation is computed, not\n"
                       "snapped -- nothing here is a baseline anyone captured.\n",
                       stderr);
            return 0;
        }
        if (const std::optional<Config> parsed = parseConfig(arg)) {
            config = *parsed;
            continue;
        }
        std::fprintf(stderr, "error: unknown argument '%s' (want: %s)\n", arg.c_str(),
                     configList().c_str());
        return 1;
    }

    const fs::path dir = repoRoot() / "debug_frames" / "readback";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::vector<Case> all = cases();
    int failures = 0;

    for (const Case& item : all) {
        const std::string name = item.name;
        const fs::path log = dir / (name + ".log");

        std::vector<std::string> extra{
            "--frames", std::to_string(kFrames), "--width", std::to_string(item.width),
            "--height", std::to_string(item.height), "--readback", kImage,
            "--capture", (dir / (name + ".png")).string(),
            "--capture-frame", std::to_string(kFrame),
            "--readback-expected", (dir / (name + ".expected.png")).string(),
            "--diff", (dir / (name + ".diff.png")).string()};
        extra.insert(extra.end(), item.flags.begin(), item.flags.end());

        const RunResult result = launch(config, extra, kTimeout);
        const std::string output = stripAnsi(result.out + result.err);
        writeLog(log, output);

        if (result.ok()) {
            // The engine exits 0 on a match and says so in a line worth surfacing: it names
            // the scale, so a case that silently presented at 1x when it meant 3x is visible
            // here rather than only in the log.
            std::printf("ok    %s -- %s\n", name.c_str(),
                        verdict(output, "Readback: ", "bit-identical").c_str());
            continue;
        }
        std::printf("FAIL  %s -- see %s (%s)\n", name.c_str(),
                    (dir / (name + ".diff.png")).generic_string().c_str(),
                    log.generic_string().c_str());
        if (const std::string why = verdict(output, "Readback: ", "MISMATCH"); !why.empty()) {
            std::printf("      %s\n", why.c_str());
        }
        ++failures;
    }

    // The lit silhouette: a control run whose cutoff discards every fragment, then the real
    // one compared against it.
    {
        const std::vector<std::string> shared{"--frames", std::to_string(kFrames), "--width",
                                              "960", "--height", "540", "--readback", kLitImage,
                                              "--capture-frame", std::to_string(kFrame)};
        std::vector<std::string> background = shared;
        background.insert(background.end(), {"--capture", (dir / "lit-sprite.bg.png").string(),
                                             "--readback-lit-cutoff", "2"});
        for (const std::string& flag : litFlags()) background.push_back(flag);

        const RunResult bg = launch(config, background, kTimeout);
        writeLog(dir / "lit-sprite.bg.log", stripAnsi(bg.out + bg.err));

        RunResult lit = bg;
        if (bg.ok()) {
            std::vector<std::string> foreground = shared;
            foreground.insert(foreground.end(),
                              {"--capture", (dir / "lit-sprite.png").string(),
                               "--readback-background", (dir / "lit-sprite.bg.png").string(),
                               "--diff", (dir / "lit-sprite.diff.png").string()});
            for (const std::string& flag : litFlags()) foreground.push_back(flag);
            lit = launch(config, foreground, kTimeout);
        }

        const std::string output = stripAnsi(lit.out + lit.err);
        writeLog(dir / "lit-sprite.log", output);

        if (bg.ok() && lit.ok()) {
            std::printf("ok    lit-sprite -- %s\n", verdict(output, "Silhouette: ", "exact").c_str());
        } else {
            std::printf("FAIL  lit-sprite -- see %s (%s)\n",
                        (dir / "lit-sprite.diff.png").generic_string().c_str(),
                        (dir / "lit-sprite.log").generic_string().c_str());
            if (const std::string why = verdict(output, "Silhouette: ", "MISMATCH"); !why.empty()) {
                std::printf("      %s\n", why.c_str());
            }
            ++failures;
        }
    }

    // Not a case, because it has no expected image: the scale is derived from the window, so
    // it changes under the user's hands. A letterbox correct at 960x540 and a validation
    // error at 1366x768 is the expected failure, and `--resize-every` is what drives it.
    std::puts("resize soak (validation layer is the verdict; run in a debug build to see it)");
    {
        const RunResult soak =
            launch(config, {"--frames", "240", "--virtual-resolution", "320x180",
                            "--resize-every", "20"}, kTimeout);
        const std::string output = stripAnsi(soak.out + soak.err);
        writeLog(dir / "resize.log", output);

        if (!soak.ok()) {
            std::printf("FAIL  resize -- the run did not complete (%s)\n",
                        (dir / "resize.log").generic_string().c_str());
            ++failures;
        } else if (output.find("[ERROR]") != std::string::npos) {
            // Any error line at all. Validation messages arrive through Logger::error under
            // the Vulkan category, so this is the layer's verdict where layers are on and a
            // cheap smoke test where they are not.
            std::printf("FAIL  resize -- error output in %s\n",
                        (dir / "resize.log").generic_string().c_str());
            ++failures;
        } else {
            std::puts("ok    resize -- 240 frames across 12 swapchains, no error output");
        }
    }

    if (failures == 0) {
        std::printf("all %zu readback cases bit-identical, plus the lit silhouette\n", all.size());
        return 0;
    }
    std::printf("%d case(s) failed\n", failures);
    return 1;
}

} // namespace tool
