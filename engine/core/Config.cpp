#include "core/Config.h"

#include "core/Json.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace core {

namespace {

using rapidjson::Value;
using settings::Id;
using settings::Source;

/// `true` and `false` must stay second: the first name for a value is the one a save and
/// `--dump-settings` write back, so promoting them changes what lands in the file.
constexpr core::Named<Tristate> kTristates[] = {
    {"auto", Tristate::Auto}, {"on", Tristate::On},   {"true", Tristate::On},
    {"off", Tristate::Off},   {"false", Tristate::Off},
};
static_assert(core::namesEveryValue(kTristates), "a tristate reachable from the enum and from no name");

/**
 * @brief Say what was wrong with a name, and what is standing instead.
 *
 * The value is refused; the run is not. Falling back to a default instead would let a sweep
 * silently measure ACES because `reinhardt` was misspelled, and produce a number nobody can
 * tell is wrong.
 *
 * `error` rather than `warn`: this is an instruction the program understood and could not
 * carry out, one rung above the unknown flag beside it. `kept` is in the message because a
 * refusal that leaves a value behind has to say which one.
 */
void refuse(const char* where, std::string_view text, const std::string& legal, std::string_view kept) {
    Logger::error(LogCategory::Core, "%s: `%s` is not one of %s -- keeping `%s`", where, std::string(text).c_str(),
                  legal.c_str(), std::string(kept).c_str());
}

/// @brief Assign a flag's named value into the field it names, or refuse it and leave the
///        field alone.
///
/// Returns whether the value was taken, which is what `--tonemap` needs to record that a
/// flag spoke over the game's own choice.
template <typename E>
bool setName(const char* flag, std::string_view text, Names<E> names, E& out) {
    if (const auto value = core::parseName(names, text)) {
        out = *value;
        return true;
    }
    refuse(flag, text, core::legalNames(names), core::nameOf(names, out));
    return false;
}

/// A flag that assigns one boolean settings row and nothing else.
struct BoolFlag {
    const char* flag;
    Id id;
    bool value;
};

constexpr BoolFlag kBoolFlags[] = {
    {"--no-ssao", Id::render_ssao, false},
    {"--no-bloom", Id::render_bloom, false},
    {"--no-edge-msaa", Id::render_edgeMsaa, false},
    {"--no-cull", Id::render_culling, false},
    // One switch for every traced path. Splitting it per feature lets a surface and its own
    // reflection shadow the same light two different ways, because the reflection pass
    // shades its hits with the shadow queries the lighting pass runs.
    {"--rt", Id::render_rt, true},
    {"--no-rt", Id::render_rt, false},
    {"--no-rt-shadows", Id::render_rtShadows, false},
    {"--rt-shadow-mask", Id::render_rtShadowMask, true},
    {"--no-ssr", Id::render_ssr, false},
    {"--no-occlusion", Id::render_occlusionCulling, false},
    {"--no-lod", Id::render_meshLod, false},
    {"--fog", Id::render_fog, true},
    {"--no-particles", Id::render_particles, false},
    {"--no-particle-sort", Id::render_particleSort, false},
    {"--taa", Id::render_taa, true},
    // Muting is a preference and belongs on a row; skipping the physics world or the mixer's
    // device is a measurement, so `--no-physics` and `--audio-null` write fields instead.
    {"--no-audio", Id::audio_enabled, false},
};

/// @brief A flag that takes a number and writes it to a settings row.
///
/// A row with a JSON key already reaches the command line through `--set <key>=<value>`, so
/// a new entry here is only correct for a control a measurement script has to pin.
struct NumberFlag {
    const char* flag;
    Id id;
};

constexpr NumberFlag kNumberFlags[] = {
    {"--msaa", Id::render_msaaSamples},
    // The presentation scale is derived from the window size, so a readback case that
    // inherited whatever `substrate.json` said would assert a scale it did not choose.
    {"--width", Id::window_width},
    {"--height", Id::window_height},
};

/// Write a number onto whichever numeric type the row actually is, so the flag tables hold
/// one entry per flag rather than one per flag and type.
void setNumber(settings::Settings& s, Id id, double value, const char* flag) {
    switch (s.row(id).type) {
        case settings::Type::Int: (void)s.setValue(id, static_cast<int>(value), Source::Cli, flag); return;
        case settings::Type::Uint: (void)s.setValue(id, static_cast<uint32_t>(value), Source::Cli, flag); return;
        case settings::Type::Uint64: (void)s.setValue(id, static_cast<uint64_t>(value), Source::Cli, flag); return;
        case settings::Type::Float: (void)s.setValue(id, static_cast<float>(value), Source::Cli, flag); return;
        default: Logger::error(LogCategory::Core, "%s does not take a number", flag); return;
    }
}

} // namespace

Names<Tristate> tristateNames() {
    return kTristates;
}

bool Config::loadFromFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::warn(LogCategory::Core, "No config at %s; using built-in defaults", path.string().c_str());
        return true;
    }

    rapidjson::IStreamWrapper stream(file);
    rapidjson::Document doc;
    doc.ParseStream(stream);

    if (doc.HasParseError()) {
        Logger::error(LogCategory::Core, "%s: JSON parse error at offset %zu: %s", path.string().c_str(),
                      doc.GetErrorOffset(), rapidjson::GetParseError_En(doc.GetParseError()));
        return false;
    }
    if (!doc.IsObject()) {
        Logger::error(LogCategory::Core, "%s: top level must be an object", path.string().c_str());
        return false;
    }

    sourcePath = path;

    // Also accounts for every key it does not claim, so a new aggregate parsed below has to
    // be added to its known-keys list or the file reports it as a typo.
    settings.loadJson(&doc, path.string());

    if (const Value* in = json::member(doc, "input"); in != nullptr) {
        if (const Value* b = json::member(*in, "bindings"); b != nullptr && b->IsObject()) {
            input.bindings.clear();
            for (auto it = b->MemberBegin(); it != b->MemberEnd(); ++it) {
                if (!it->name.IsString()) continue;

                // Both shapes have to stay accepted: a bare string is what a hand-edited
                // file holds, an array is what `input::saveBindings` writes back.
                std::string list;
                if (it->value.IsString()) {
                    list = it->value.GetString();
                } else if (it->value.IsArray()) {
                    for (const auto& entry : it->value.GetArray()) {
                        if (!entry.IsString()) continue;
                        if (!list.empty()) list += ' ';
                        list += entry.GetString();
                    }
                } else {
                    continue;
                }
                input.bindings.emplace_back(it->name.GetString(), std::move(list));
            }
        }
    }

    return true;
}

bool Config::applyCommandLine(int argc, char** argv, int& exitCode) {
    exitCode = 0;

    // Whether the overlay was named either way, so the capture default below overrides the
    // default and not an instruction.
    bool overlayNamed = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextString = [&](const std::string& fallback) -> std::string {
            return i + 1 < argc ? std::string(argv[++i]) : fallback;
        };
        /**
         * @brief The next argument as a number, or `fallback` if there isn't one.
         *
         * Callers must pass what the setting already holds, not a literal restating the
         * default, or `--msaa` at the end of a line silently resets rather than keeps.
         *
         * A token that does not parse is *not* consumed: consuming it makes `--msaa
         * --frames` read the second flag as zero and swallow it, changing two settings and
         * dropping a third instruction.
         */
        const auto nextNumber = [&](double fallback) -> double {
            if (i + 1 >= argc) return fallback;
            char* end = nullptr;
            const double parsed = std::strtod(argv[i + 1], &end);
            if (end == argv[i + 1] || *end != '\0') return fallback;
            ++i;
            return parsed;
        };

        if (const auto* f = std::find_if(std::begin(kBoolFlags), std::end(kBoolFlags),
                                         [&](const BoolFlag& b) { return arg == b.flag; });
            f != std::end(kBoolFlags)) {
            (void)settings.setValue(f->id, f->value, Source::Cli, f->flag);
            continue;
        }
        if (const auto* f = std::find_if(std::begin(kNumberFlags), std::end(kNumberFlags),
                                         [&](const NumberFlag& n) { return arg == n.flag; });
            f != std::end(kNumberFlags)) {
            const double current = std::stod(settings.valueString(f->id));
            setNumber(settings, f->id, nextNumber(current), f->flag);
            continue;
        }

        if (arg == "--config") {
            // Handled by the caller before this runs; the value is skipped so it cannot be
            // mistaken for a positional scene path below.
            (void)nextString("");
        } else if (arg == "--scene") {
            scene.path = nextString(scene.path.string());
        } else if (arg == "--characters") {
            scene.characters = static_cast<uint32_t>(nextNumber(scene.characters));
        } else if (arg == "--frames") {
            benchmark.exitAfterFrames = static_cast<uint64_t>(nextNumber(static_cast<double>(benchmark.exitAfterFrames)));
        } else if (arg == "--trace") {
            profiler.outputFile = nextString(profiler.outputFile);
        } else if (arg == "--no-profiler") {
            profiler.enabled = false;
        } else if (arg == "--record") {
            // Takes an *optional* number, which is why it cannot be a `kNumberFlags` row:
            // `--record` alone means the default window. `nextNumber` consumes only a token
            // that parses, so `--record --panel` sets neither wrongly.
            record.enabled = true;
            record.seconds = static_cast<float>(nextNumber(static_cast<double>(record.seconds)));
        } else if (arg == "--record-file") {
            record.file = nextString(record.file);
        } else if (arg == "--log-file") {
            logging.file = nextString(logging.file);
        } else if (arg == "--log-level") {
            (void)setName(arg.c_str(), nextString(core::nameOf(logLevelNames(), logging.level)), logLevelNames(),
                          logging.level);
        } else if (arg == "--log-output") {
            (void)setName(arg.c_str(), nextString(core::nameOf(logOutputNames(), logging.output)), logOutputNames(),
                          logging.output);
        } else if (arg == "--log-categories") {
            // An empty value has to mean `all`, or `--log-categories` with nothing after it
            // silences the log by accident.
            const std::string spec = nextString("all");
            logging.categories.clear();
            for (size_t start = 0; start <= spec.size();) {
                const size_t comma = std::min(spec.find(',', start), spec.size());
                if (comma > start) logging.categories.emplace_back(spec.substr(start, comma - start));
                start = comma + 1;
            }
            if (logging.categories.empty()) logging.categories.emplace_back("all");
        } else if (arg == "--validation") {
            (void)setName(arg.c_str(), nextString(core::nameOf(tristateNames(), render.validation)), tristateNames(),
                          render.validation);
        } else if (arg == "--hot-reload") {
            (void)setName(arg.c_str(), nextString(core::nameOf(tristateNames(), render.shaderHotReload)),
                          tristateNames(), render.shaderHotReload);
        } else if (arg == "--tonemap") {
            // `tonemapNamed` follows the *parse*, not the flag: setting it unconditionally
            // makes a refused spelling override the game's choice with the old value.
            render.tonemapNamed |=
                setName(arg.c_str(), nextString(core::tonemapKey(render.tonemap)), core::tonemapNames(), render.tonemap);
        } else if (arg == "--debug-view") {
            // Parsed at the flag rather than carried as a string: a name that is not one is
            // refused here instead of resolving to `lit` in a reader three subsystems away.
            const std::string text = nextString(core::debugViewKey(render.debugView));
            if (const auto view = core::parseName(core::debugViewNames(), text)) {
                render.debugView = *view;
            } else {
                refuse(arg.c_str(), text, core::legalNames(core::debugViewNames()),
                       core::debugViewKey(render.debugView));
            }
        } else if (arg == "--sync-validation") {
            // Implies the layer: without it the expensive checks are requested and the
            // layer never loads, which is silent rather than an error.
            render.syncValidation = true;
            render.validation = Tristate::On;
        } else if (arg == "--headless") {
            window.headless = true;
        } else if (arg == "--windowed") {
            window.windowed = true;
        } else if (arg == "--camera") {
            // All six or none: accepting five would start from a camera five-sixths of what
            // was asked for and reproduce something other than the thing being reported.
            const std::string spec = nextString("");
            float f[6]{};
            if (std::sscanf(spec.c_str(), "%f,%f,%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) == 6) {
                camera.startFocus = glm::vec3(f[0], f[1], f[2]);
                camera.startYawDegrees = f[3];
                camera.startPitchDegrees = f[4];
                camera.startDistance = f[5];
                camera.startSet = true;
            } else {
                Logger::warn(LogCategory::Core,
                             "--camera wants six comma-separated numbers "
                             "(focus x,y,z then yaw,pitch in degrees then distance), got '%s'; "
                             "framing the scene instead",
                             spec.c_str());
            }
        } else if (arg == "--camera-spin") {
            // Degrees per *frame*, not per second. Refused rather than defaulted: a typo
            // that silently span at some default defeats the repeatability this is for.
            const std::string spec = nextString("");
            float degrees = 0.0f;
            if (std::sscanf(spec.c_str(), "%f", &degrees) == 1) {
                camera.spinDegreesPerFrame = degrees;
            } else {
                Logger::warn(LogCategory::Core,
                             "--camera-spin wants degrees of yaw per frame, got '%s'; not spinning", spec.c_str());
            }
        } else if (arg == "--input-script") {
            // `parse` leaves the script empty on failure, so a malformed one drives nothing
            // rather than half a scenario.
            (void)input.script.parse(nextString(""));
        } else if (arg == "--overlay" || arg == "--no-overlay") {
            render.debugOverlay = arg == "--overlay";
            overlayNamed = true;
        } else if (arg == "--no-ray-query") {
            render.rayQuery = Tristate::Off;
        } else if (arg == "--no-physics") {
            physics.enabled = false;
        } else if (arg == "--physics-debug") {
            physics.debugDraw = true;
        } else if (arg == "--panel") {
            ui.panel = true;
        } else if (arg == "--audio-debug") {
            audio.debugDraw = true;
        } else if (arg == "--realtime" || arg == "--locked") {
            // A tool that depends on frame N being a function of N has to pin the clock
            // rather than inherit it; golden.sh and baseline.py pass --locked for that.
            physics.clock = arg == "--locked" ? "locked" : "realtime";
        } else if (arg == "--physics-contacts") {
            physics.debugDraw = true;
            physics.debugContacts = true;
        } else if (arg == "--inspector") {
            // Implies the settings panel: the inspector is positioned to the right of it,
            // so without the panel it hangs in space.
            ui.panel = true;
            ui.inspector = true;
        } else if (arg == "--no-occlusion") {
            // Unreachable: `kBoolFlags` claims `--no-occlusion` for
            // `render.occlusionCulling` and `continue`s, so audio occlusion cannot be
            // turned off. Renaming either is a published flag changing meaning.
            audio.occlusionOff = true;
        } else if (arg == "--audio-null") {
            audio.backend = core::AudioBackend::Null;
        } else if (arg == "--capture") {
            benchmark.capturePath = nextString(benchmark.capturePath);
            // 60: past the load hitch and inside every default profiler window. A capture
            // with no frame index writes nothing.
            if (benchmark.captureFrame == 0) benchmark.captureFrame = 60;
        } else if (arg == "--capture-frame") {
            benchmark.captureFrame = static_cast<uint64_t>(nextNumber(static_cast<double>(benchmark.captureFrame)));
        } else if (arg == "--golden") {
            benchmark.goldenPath = nextString(benchmark.goldenPath);
            if (benchmark.captureFrame == 0) benchmark.captureFrame = 60;
        } else if (arg == "--diff") {
            benchmark.diffPath = nextString(benchmark.diffPath);
        } else if (arg == "--compare-tolerance") {
            benchmark.compareTolerance = static_cast<uint32_t>(nextNumber(benchmark.compareTolerance));
        } else if (arg == "--compare-max-pixels") {
            benchmark.compareMaxPixels = static_cast<uint64_t>(nextNumber(static_cast<double>(benchmark.compareMaxPixels)));
        } else if (arg == "--capture-target") {
            benchmark.captureTarget = nextString(benchmark.captureTarget);
            if (benchmark.captureFrame == 0) benchmark.captureFrame = 60;
        } else if (arg == "--capture-target-path") {
            benchmark.captureTargetPath = nextString(benchmark.captureTargetPath);
        } else if (arg == "--capture-target-mip") {
            benchmark.captureTargetMip = static_cast<uint32_t>(nextNumber(0));
        } else if (arg == "--capture-target-layer") {
            benchmark.captureTargetLayer = static_cast<uint32_t>(nextNumber(0));
        } else if (arg == "--resize-every") {
            benchmark.resizeEveryFrames = static_cast<uint64_t>(nextNumber(1));
        } else if (arg == "--virtual-resolution") {
            // `native` is spelled out because nobody would guess "0x0" for it.
            const std::string spec = nextString("");
            unsigned w = 0;
            unsigned h = 0;
            if (spec == "native" || spec == "0") {
                render.virtualWidth = 0;
                render.virtualHeight = 0;
            } else if (std::sscanf(spec.c_str(), "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
                render.virtualWidth = w;
                render.virtualHeight = h;
            } else {
                Logger::warn(LogCategory::Core,
                             "--virtual-resolution wants WxH or 'native', got '%s'; rendering at the window",
                             spec.c_str());
            }
        } else if (arg == "--ui-outside-virtual") {
            render.uiOutsideVirtualNamed = true;
        } else if (arg == "--readback") {
            benchmark.readbackImage = nextString("");
            // Only defaulted where nothing named one, so an earlier `--capture` still wins.
            if (benchmark.capturePath.empty()) benchmark.capturePath = "debug_frames/readback/capture.png";
            if (benchmark.captureFrame == 0) benchmark.captureFrame = 60;
        } else if (arg == "--readback-expected") {
            benchmark.readbackExpectedPath = nextString(benchmark.readbackExpectedPath);
        } else if (arg == "--readback-sprite") {
            benchmark.readbackSprite = true;
        } else if (arg == "--readback-sheet-fps") {
            benchmark.readbackSheetFps = static_cast<float>(nextNumber(0.0));
            benchmark.readbackSprite = true;
        } else if (arg == "--readback-lit-sprite") {
            benchmark.readbackLitSprite = true;
        } else if (arg == "--readback-lit-cutoff") {
            benchmark.readbackLitCutoff = static_cast<float>(nextNumber(0.5));
        } else if (arg == "--readback-background") {
            benchmark.readbackBackground = nextString("");
        } else if (arg == "--readback-sheet-frame") {
            benchmark.readbackSheetFrame = static_cast<uint32_t>(nextNumber(0));
        } else if (arg == "--sprites") {
            benchmark.spriteStress = static_cast<uint32_t>(nextNumber(0));
        } else if (arg == "--sprite-image") {
            benchmark.spriteStressImage = nextString(benchmark.spriteStressImage);
        } else if (arg == "--sprites-move") {
            benchmark.spriteStressMove = true;
        } else if (arg == "--rdoc-capture-frame") {
            benchmark.rdocCaptureFrame =
                static_cast<uint64_t>(nextNumber(static_cast<double>(benchmark.rdocCaptureFrame)));
        } else if (arg == "--rdoc-capture-path") {
            benchmark.rdocCapturePath = nextString(benchmark.rdocCapturePath);
            if (benchmark.rdocCaptureFrame == 0) benchmark.rdocCaptureFrame = 60;
        } else if (arg == "--set") {
            // Split on the *first* `=`, not the only one: a value may contain one, and
            // `--set profiler.outputFile=a=b.json` is a path rather than two assignments.
            const std::string assignment = nextString("");
            const size_t eq = assignment.find('=');
            if (eq == 0 || eq == std::string::npos) {
                Logger::error(LogCategory::Core, "--set wants <key>=<value>, got `%s` -- setting nothing",
                              assignment.c_str());
                continue;
            }
            const std::string key = assignment.substr(0, eq);
            const std::string text = assignment.substr(eq + 1);
            (void)settings.setFromString(key, text, Source::Cli, "--set");
        } else if (arg == "--dump-settings") {
            dumpSettings = Dump::Table;
        } else if (arg == "--dump-settings=json") {
            dumpSettings = Dump::Json;
        } else if (arg == "--write-default-config") {
            const std::string out = nextString("substrate.json");
            exitCode = writeDefaultConfig(settings, out) ? 0 : 1;
            return false;
        } else if (arg == "--help" || arg == "-h") {
            // The five name lists are `%s`, filled from the same tables that parse them.
            // Spelling one out here gives the enum a second list free to disagree.
            std::printf(
                "usage: substrate [scene.gltf] [options]\n"
                "\n"
                "Every setting in substrate.json can be set for one run, by its JSON key:\n"
                "\n"
                "  --set <key>=<value>      e.g. --set render.fogHeightFalloff=12\n"
                "                                --set window.vsync=true --set ui.scale=2\n"
                "                           --dump-settings prints every key there is, its\n"
                "                           value, and where that value came from.\n"
                "\n"
                "The flags below are the ones --set cannot replace: a developer control with\n"
                "no JSON key at all, or a switch a runtime key already spells (--no-ssao\n"
                "beside F8 is one idea spelled twice for one reason). Anything else is a row\n"
                "in the table and reaches the command line through --set -- which is the test\n"
                "a new flag has to pass.\n"
                "\n"
                "  --config <path>          config file (default: substrate.json)\n"
                "  --dump-settings          print the whole settings table and exit\n"
                "  --dump-settings=json     the same, as JSON, for a bug report\n"
                "  --write-default-config [path]   write a fully-populated config and exit\n"
                "  --scene <path>           scene to load, overriding the game's own\n"
                "  --msaa <1|2|4|8>         MSAA sample count\n"
                "  --characters <N>         copies of the scene's skinned mesh, one character each\n"
                "  --frames <N>             exit after N frames (0 = run until closed)\n"
                "  --trace <path>           Chrome Tracing output\n"
                "  --no-profiler            no CPU scopes, no GPU queries, no trace\n"
                "  --debug-view <name>      %s\n"
                "  --log-file <path>        where the log goes (default debug_frames/substrate.log)\n"
                "  --log-level <name>       %s\n"
                "  --log-output <name>      %s\n"
                "  --log-categories <a,b>   which subsystems speak; 'all' is the default\n"
                "  --validation <name>      %s\n"
                "  --hot-reload <name>      recompile changed shaders in place (auto = on in Debug)\n"
                "  --tonemap <name>         %s -- overrides GameSetup::look.tonemap\n"
                "  --no-occlusion           disable two-pass Hi-Z occlusion culling (C11)\n"
                "  --no-lod                 draw every mesh at LOD 0 (C17)\n"
                "  --sync-validation        add synchronization validation (implies --validation on)\n"
                "  --headless               create the window unmapped\n"
                "  --windowed               map it anyway: --frames implies --headless without this\n"
                "  --camera <fx,fy,fz,yaw,pitch,dist>  start where the overlay's cam line says\n"
                "  --camera-spin <degrees>       yaw this many degrees per frame, for repeatable motion\n"
                "  --input-script <steps>   press actions on stated frames, e.g.\n"
                "                           60:Game.Save,90:Camera.Forward+,150:Camera.Forward-\n"
                "                           '+' presses, '-' releases, neither taps; --frames must\n"
                "                           outlast the last one\n"
                "  --overlay                draw the frame stats overlay (on by default, F6 toggles)\n"
                "  --no-overlay             start without the frame stats overlay\n"
                "  --no-ssao                disable SSAO (F8 toggles)\n"
                "  --no-bloom               disable bloom (F10 toggles)\n"
                "  --no-edge-msaa           shade every MSAA sample always (M toggles)\n"
                "  --no-cull                submit every draw regardless of the frustum (C toggles)\n"
                "  --no-ray-query           do not request the RT extensions (needed under ASan)\n"
                "  --no-rt                  screen-space reflection march instead of traced reflections\n"
                "                           (Y toggles; on by default wherever the device supports ray\n"
                "                           query)\n"
                "  --rt                     force ray tracing back on over a config that turned it off\n"
                "  --no-rt-shadows          raster shadow maps under a traced frame, for attribution\n"
                "  --rt-shadow-mask         trace shadow rays once per distinct fragment rather\n"
                "                           than once per MSAA sample; approximate at silhouettes\n"
                "  --no-ssr                 disable screen-space reflections (R toggles)\n"
                "  --fog                    volumetric fog (G toggles)\n"
                "  --no-particles           disable the particle passes (N toggles)\n"
                "  --no-particle-sort       draw particles unsorted; measures the sort itself\n"
                "  --taa                    temporal antialiasing (T toggles)\n"
                "  --no-physics             skip the physics world entirely\n"
                "  --realtime               step the simulation from wall-clock time and\n"
                "                           interpolate the render state (the default)\n"
                "  --locked                 one fixed step per frame, so frame N is a function\n"
                "                           of N alone -- what golden images and benchmarks need\n"
                "  --physics-debug          draw every collision shape (B toggles)\n"
                "  --physics-contacts       and the contact points with them\n"
                "  --panel                  open the settings panel at startup (I toggles)\n"
                "  --inspector              open the instance inspector at startup (O toggles)\n"
                "  --no-audio               skip the mixer, the decode and every source\n"
                "  --audio-null             mix without a playback device: every decoder, filter and\n"
                "                           bus still runs, nothing reaches a speaker\n"
                "  --audio-debug            draw a line to each source, coloured by occlusion (K toggles)\n"
                "  --record [seconds]       record the session to an mp4, keeping the last N seconds\n"
                "  --record-file <path>     where it goes (default debug_frames/session.mp4)\n"
                "  --capture <path>         write a PNG of one frame (F12 captures live)\n"
                "  --capture-frame <N>      which frame to capture (default 60)\n"
                "  --golden <path>          compare the capture against this PNG; exit 1 on mismatch\n"
                "  --diff <path>            where the difference image goes\n"
                "  --compare-tolerance <N>  per-channel difference a pixel may have (default 2)\n"
                "  --compare-max-pixels <N> pixels allowed to exceed it (default 0)\n"
                "  --capture-target <name>  read an intermediate render target back to a PNG;\n"
                "                           'list' prints the names this build offers\n"
                "  --capture-target-path <p>  where it goes (default debug_frames/targets/target.png)\n"
                "  --resize-every <N>       resize the window every N frames (0.4's swapchain drive)\n"
                "  --virtual-resolution <WxH>  render here and present at the largest integer\n"
                "                           scale that fits, letterboxed; 'native' for the window\n"
                "  --ui-outside-virtual     draw the HUD and UI after the scale, at window size\n"
                "  --readback <res:/img>    draw it at 1:1 and compare the capture against the\n"
                "                           same file expanded by the presentation scale (P2)\n"
                "  --readback-expected <p>  where that expanded expectation is written\n"
                "  --readback-sprite        run that same check through the sprite pass (P4)\n"
                "  --readback-sheet-fps <f> cut it into four cells, play them at f, and hold\n"
                "                           the capture against one cell (P5)\n"
                "  --readback-sheet-frame <n> the cell the animation must have reached by the\n"
                "                           capture frame -- stated, never derived (P5)\n"
                "  --sprites <N>            draw N sprites through an orthographic camera,\n"
                "                           for scripts/baseline.py's before/after (P4)\n"
                "  --sprite-image <res:/i>  what --sprites draws (default res:/readback.png)\n"
                "  --sprites-move           move every one of them every frame, which is the\n"
                "                           arm the upload's revision gate is measured against\n"
                "  --width <N> --height <N> the window, pinned rather than inherited\n"
                "  --capture-target-mip <N>   one mip instead of all of them\n"
                "  --capture-target-layer <N> one array layer instead of all of them\n"
                "  --rdoc-capture-frame <N> RenderDoc capture at frame N (F11 captures live);\n"
                "                           needs ENABLE_VULKAN_RENDERDOC_CAPTURE=1 -- use scripts/rdoc.sh\n"
                "  --rdoc-capture-path <p>  .rdc path prefix (default debug_frames/rdoc/frame)\n"
                "  -h, --help\n",
                core::legalNames(core::debugViewNames()).c_str(), core::legalNames(logLevelNames()).c_str(),
                core::legalNames(logOutputNames()).c_str(), core::legalNames(tristateNames()).c_str(),
                core::legalNames(core::tonemapNames()).c_str());
            return false;
        } else if (!arg.empty() && arg[0] != '-') {
            scene.path = arg;
        } else {
            Logger::warn(LogCategory::Core, "Unknown option '%s' (try --help)", arg.c_str());
        }
    }

    // The overlay draws a frame counter and a millisecond figure that change every frame,
    // so leaving it on makes every golden capture differ in the same corner regardless of
    // what the renderer did.
    if (benchmark.captureFrame != 0 && !overlayNamed) render.debugOverlay = false;

    // A frame budget already sets `GLFW_FOCUSED` and `GLFW_FOCUS_ON_SHOW` false, and that
    // does not bind a window manager configured to focus whatever it maps -- so a harness
    // run took the keyboard whenever a caller forgot `--headless`, and every caller had to
    // remember it separately. Unmapping on the same signal is what removes the discipline.
    // `--record` is the one thing a frame budget can legitimately want a swapchain for,
    // since `startRecording` refuses a headless run.
    if (benchmark.exitAfterFrames != 0 && !window.windowed && !record.enabled) window.headless = true;

    return true;
}

uint32_t Config::logCategoryMask() const {
    uint32_t mask = 0;
    for (const std::string& entry : logging.categories) {
        // `all` is a wildcard rather than a category, so it is not in `logCategoryNames`.
        if (core::namesEqual(entry, "all")) return AllLogCategories;
        if (const auto category = core::parseName(logCategoryNames(), entry)) {
            mask |= static_cast<uint32_t>(*category);
            continue;
        }
        Logger::error(LogCategory::Core, "--log-categories: `%s` is not one of %s or all -- ignoring it", entry.c_str(),
                      core::legalNames(logCategoryNames()).c_str());
    }
    // An empty mask must fall back to everything: returning 0 makes a typo that silenced
    // the whole log invisible.
    return mask != 0 ? mask : AllLogCategories;
}

bool Config::validationEnabled(bool debugBuild) const {
    return enabled(render.validation, debugBuild);
}

bool Config::shaderHotReloadEnabled(bool debugBuild) const {
    return enabled(render.shaderHotReload, debugBuild);
}

bool Config::rayQueryAllowed() const {
    // `whenAuto` is true: `auto` means "wherever the device offers them", so only an
    // explicit `off` turns it off here.
    return enabled(render.rayQuery, true);
}

bool Config::physicsRealtimeClock() const {
    if (core::namesEqual(physics.clock, "locked") || core::namesEqual(physics.clock, "fixed")) return false;
    // Falls back to realtime, the default, rather than to the deterministic value: a
    // misspelled clock running the simulation at the frame rate presents as "everything is
    // ten times too fast", which is far harder to trace back than this warning.
    if (!core::namesEqual(physics.clock, "realtime") && !core::namesEqual(physics.clock, "variable")) {
        Logger::warn(LogCategory::Core, "physics clock '%s' is neither 'locked' nor 'realtime' -- using realtime",
                     physics.clock.c_str());
    }
    return true;
}

void Config::logSummary() const {
    Logger::status(LogCategory::Core, "Config: %s",
                   sourcePath.empty() ? "<built-in defaults>" : sourcePath.string().c_str());
    Logger::status(LogCategory::Core, "  window %dx%d vsync=%s%s | msaa=%ux | view=%s", settings.get(options::window::width),
                   settings.get(options::window::height), settings.get(options::window::vsync) ? "on" : "off",
                   window.headless ? " headless" : "", settings.get(options::render::msaaSamples),
                   core::debugViewKey(render.debugView));
    if (settings.get(options::audio::enabled)) {
        Logger::status(LogCategory::Core, "  audio %s %u Hz %u ch | stream past %.1fs",
                       core::nameOf(core::audioBackendNames(), audio.backend), settings.get(options::audio::sampleRate),
                       settings.get(options::audio::channels),
                       static_cast<double>(settings.get(options::audio::streamThresholdSeconds)));
    }
}

bool writeDefaultConfig(const settings::Settings& table, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::fprintf(stderr, "error: cannot write %s\n", path.string().c_str());
        return false;
    }

    // Rows are grouped by module rather than emitted in list order, because the list
    // interleaves modules and a section opened per change writes `"render"` twice. rapidjson
    // tolerates the duplicate member so the round trip still passes; other readers reject
    // the file.
    const auto moduleOf = [](const settings::Row& r) {
        const std::string_view key = r.key;
        return std::string(key.substr(0, key.find('.')));
    };

    std::vector<std::string> modules;
    for (uint16_t i = 0; i < table.rowCount(); ++i) {
        const settings::Row& r = table.row(static_cast<settings::Id>(i));
        // `engine.` rows are engine-owned live state; writing them implies a file can edit
        // them.
        if ((r.flags & settings::kEngine) != 0) continue;
        if (const std::string module = moduleOf(r); std::find(modules.begin(), modules.end(), module) == modules.end()) {
            modules.push_back(module);
        }
    }

    // The aggregate no row can hold, so the written file is a complete example. An
    // aggregate emitted for a module with no rows leaves that module unclaimed by the table
    // and free for a game to declare into while the engine still parses a key out of it.
    const auto closeSection = [&](const std::string& name) {
        if (name == "input") out << ",\n    \"bindings\": {}";
        out << "\n  }";
    };

    out << "{\n";
    for (size_t m = 0; m < modules.size(); ++m) {
        if (m > 0) out << ",\n\n";
        out << "  \"" << modules[m] << "\": {\n";

        bool anyMember = false;
        for (uint16_t i = 0; i < table.rowCount(); ++i) {
            const settings::Row& r = table.row(static_cast<settings::Id>(i));
            if ((r.flags & settings::kEngine) != 0) continue;
            if (moduleOf(r) != modules[m]) continue;

            if (anyMember) out << ",\n";
            anyMember = true;

            // The row's own built-in default, not its current value -- `table` is the live
            // one, so `r.value` here would write the running configuration out as defaults.
            const std::string_view key = r.key;
            out << "    \"" << key.substr(key.find('.') + 1) << "\": ";
            if (r.type == settings::Type::String) {
                out << '"' << settings::defaultString(r) << '"';
            } else {
                out << settings::defaultString(r);
            }
        }
        closeSection(modules[m]);
    }
    out << "\n}\n";

    std::printf("wrote %s\n", path.string().c_str());
    return true;
}

} // namespace core
