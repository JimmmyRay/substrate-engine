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

/// `true` and `false` are input conveniences and are second, so `--dump-settings` and a
/// save both report `on` and `off` however the value was spelled coming in.
constexpr core::Named<Tristate> kTristates[] = {
    {"auto", Tristate::Auto}, {"on", Tristate::On},   {"true", Tristate::On},
    {"off", Tristate::Off},   {"false", Tristate::Off},
};
static_assert(core::namesEveryValue(kTristates), "a tristate reachable from the enum and from no name");

// ------------------------------------------------------------- names, and the refusal

/**
 * @brief Say what was wrong with a name, and what is standing instead.
 *
 * **The value is refused; the run is not.** A hard exit over a typo in a hand-edited
 * config file is a different kind of damage from the one being fixed, and the argument the
 * old `lookup` made -- *"refusing to start over a typo in a benchmark sweep costs more
 * than the typo"* -- is right about the *run* and was wrong about the *value*: a sweep
 * that silently measured ACES because `reinhardt` fell back to it produces a number nobody
 * can tell is wrong. Refusing the value and leaving the previous one standing costs the
 * sweep nothing and tells the truth, so both doors do exactly that and neither exits.
 *
 * `error` rather than `warn`, and one rung above the unknown *flag* beside it: an unknown
 * flag is a word the program does not know, and this is an instruction it understood and
 * could not carry out.
 *
 * @param kept named in the message on purpose. A refusal that leaves a value behind has to
 *        say which one, or the next question is what the program is actually running.
 */
void refuse(const char* where, std::string_view text, const std::string& legal, std::string_view kept) {
    Logger::error(LogCategory::Core, "%s: `%s` is not one of %s -- keeping `%s`", where, std::string(text).c_str(),
                  legal.c_str(), std::string(kept).c_str());
}

/**
 * @brief Assign a flag's named value into the field it names, or refuse it and leave the
 *        field alone.
 *
 * The whole of D12's refusal, now that every name-valued setting is a *field of its enum's
 * own type* rather than a `std::string` row. `kNamedRows` -- seven entries erased to two
 * function pointers, because a table of rows had to hold four different enums -- is gone
 * with the rows it named: a template over `Names<E>` is what the same job looks like when
 * the destination is typed, and it does the parse once at the door instead of on every
 * read. Six flags reach it, which is the Rule of Threes met twice over for a helper that
 * stays local to this file.
 *
 * @return whether the value was taken, for the two flags that also have to record *that a
 *         flag spoke* -- `--tonemap` over the game's own choice.
 */
template <typename E>
bool setName(const char* flag, std::string_view text, Names<E> names, E& out) {
    if (const auto value = core::parseName(names, text)) {
        out = *value;
        return true;
    }
    refuse(flag, text, core::legalNames(names), core::nameOf(names, out));
    return false;
}

// ------------------------------------------------------------------ command line

/// A flag that assigns one boolean and nothing else. About twenty of these were twenty
/// `else if` branches with one statement each; the table is the same information without
/// the ceremony, and adding a flag is now a row rather than a branch.
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
    // One flag for every traced path, deliberately -- there is no per-feature split.
    // The reflection pass shades its hits with the same shadow queries the lighting
    // pass runs, so a partial combination would let a surface and its own reflection
    // shadow the same light two different ways.
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
    // The one switch left here that is not a render row. Muting is a preference -- a
    // player legitimately wants silence -- where skipping the physics world or the mixer's
    // device is a measurement, which is why `--no-physics` and `--audio-null` write fields
    // below instead.
    {"--no-audio", Id::audio_enabled, false},
};

/**
 * @brief A flag that takes a number.
 *
 * Three of them, and the shortness is the point (D13). Eleven more lived here to assign one
 * preference row apiece and are gone: a row with a JSON key reaches the command line
 * through `--set <key>=<value>`, and a named flag is now correct only for a control that
 * has *no* key -- something a measurement script drives. `--frames` was the fourth and is
 * a branch below now, because D14 made `benchmark.exitAfterFrames` a `Config` field: this
 * table writes settings rows, and there is no row left for it to write.
 *
 * **The `scale` column went with them**, and that is a small win of its own. It existed so
 * `--bloom-strength 5` could mean 0.05, because *"150% is a scale nobody would type as 1.5
 * twice"* -- so one setting had two representations, the file's and the flag's.
 * `--set render.bloomStrength=0.05` is the number the file holds and the dump prints.
 */
struct NumberFlag {
    const char* flag;
    Id id;
};

constexpr NumberFlag kNumberFlags[] = {
    {"--msaa", Id::render_msaaSamples},
    // The window, from the command line, because P2 made the window size a thing a check
    // has to *pin* rather than inherit: the presentation scale is derived from it, so a
    // readback case that took whatever `substrate.json` happened to say would be asserting
    // a scale it did not choose. Same argument `--locked` makes about the clock.
    {"--width", Id::window_width},
    {"--height", Id::window_height},
};

// `kStringFlags` is gone with D14. All four of its entries -- `--trace`, `--log-level`,
// `--validation` and `--tonemap` -- assigned a `std::string` row that is now a typed field,
// so each is a branch below that parses its name where it arrives rather than a row that
// carries a spelling around and resolves it on every read.

/// Write a number onto whichever of the five numeric row types the row actually is, so the
/// flag table can hold one entry per flag rather than one per flag and type.
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

// ============================================================================ load

bool Config::loadFromFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        // Not fatal: running without a config file is legitimate, and the defaults are the
        // configuration in that case.
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

    // Every scalar, in one call. It also walks the file and accounts for every key it
    // does not claim -- a moved key gets the sentence saying where it went, a typo gets
    // told it is one, and the one aggregate below is known to it and stays quiet.
    //
    // The capture-apply-settle dance over `kNamedRows` that used to follow this call is
    // gone with D14: not one row of the table holds a name any more, so there is nothing
    // for the file to spell wrongly and nothing to canonicalise on the way back out. The
    // refusal itself did not go anywhere -- it moved to the six flags that now carry those
    // names, where `setName` applies it before the field is written.
    settings.loadJson(&doc, path.string());

    // ------------------------------------------------------- the one aggregate left
    if (const Value* in = json::member(doc, "input"); in != nullptr) {
        if (const Value* b = json::member(*in, "bindings"); b != nullptr && b->IsObject()) {
            input.bindings.clear();
            for (auto it = b->MemberBegin(); it != b->MemberEnd(); ++it) {
                if (!it->name.IsString()) continue;

                // A single binding is a string and several are an array, because
                // `"Camera.Forward": "W"` is what someone writes by hand and
                // `["W", "Pad.LeftY-"]` is what the save path writes back.
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

// ==================================================================== command line

bool Config::applyCommandLine(int argc, char** argv, int& exitCode) {
    exitCode = 0;

    // Whether the overlay was named on the command line, either way. A capture run turns
    // the overlay off below, and that has to be a decision about the *default* rather
    // than an override of an instruction: someone who typed --overlay next to --capture
    // wants the overlay in the capture, and is entitled to it however little sense it
    // makes for a golden image. Local rather than a field because nothing outside this
    // function has any business knowing which way a value was arrived at.
    bool overlayNamed = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextString = [&](const std::string& fallback) -> std::string {
            return i + 1 < argc ? std::string(argv[++i]) : fallback;
        };
        /**
         * @brief The next argument as a number, or `fallback` if there isn't one.
         *
         * Two things here are deliberate and both were defects before S4.
         *
         * **The fallback is what the setting already holds**, not a literal restating the
         * default. Six of the fifteen numeric flags restated it and nine did not, so
         * `--msaa` at the end of a line kept whatever was configured and another in the
         * same position silently reset to a literal.
         *
         * **A token that is not a number is not consumed.** `--msaa --frames` used to read
         * the second flag as zero *and swallow it*, so one missing value quietly changed
         * two settings and dropped a third instruction on the floor.
         */
        const auto nextNumber = [&](double fallback) -> double {
            if (i + 1 >= argc) return fallback;
            char* end = nullptr;
            const double parsed = std::strtod(argv[i + 1], &end);
            if (end == argv[i + 1] || *end != '\0') return fallback;
            ++i;
            return parsed;
        };

        // ------------------------------------------------------------- the tables
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

        // ------------------------------------------------- the ones that are not one line
        if (arg == "--config") {
            // Handled by the caller before this runs; skip its value here.
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
            // The arm profiling.md's *"the whole profiler costs 0.023 ms a frame"* figure
            // is measured against. It was `"profiler": {"enabled": false}` in the config
            // file, which meant a measurement whose control lived in the tester's own
            // settings rather than on the command line that produced it.
            profiler.enabled = false;
        } else if (arg == "--record") {
            // The one flag that takes an *optional* number, which is why it is here and
            // not in kNumberFlags: `--record` on its own means the default window, and
            // `--record 60` means a minute. `nextNumber` only consumes a token that
            // parses as one, so `--record --panel` sets neither wrongly.
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
            // The aggregate, and the one flag here that takes a list. `all` is the
            // wildcard and is what an empty value means, so `--log-categories` with
            // nothing after it is not a way to silence the log by accident.
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
            // The one name here that overrides a *game's* choice rather than a built-in,
            // so a refused spelling must not look like an override either -- which is why
            // `tonemapNamed` follows the parse instead of the flag.
            render.tonemapNamed |=
                setName(arg.c_str(), nextString(core::tonemapKey(render.tonemap)), core::tonemapNames(), render.tonemap);
        } else if (arg == "--debug-view") {
            // The one named value with no row behind it, so it is refused here rather than
            // through a row. It is parsed by the flag rather than carried as a
            // string for the reason `--camera` and `--input-script` are: a name that is not
            // one is refused while the person who typed it is still looking, instead of
            // being resolved to `lit` by a reader three subsystems away.
            const std::string text = nextString(core::debugViewKey(render.debugView));
            if (const auto view = core::parseName(core::debugViewNames(), text)) {
                render.debugView = *view;
            } else {
                refuse(arg.c_str(), text, core::legalNames(core::debugViewNames()),
                       core::debugViewKey(render.debugView));
            }
        } else if (arg == "--sync-validation") {
            // Implies the layer, because asking for the expensive checks and getting
            // silence because the layer was never loaded is the worst of both.
            render.syncValidation = true;
            render.validation = Tristate::On;
        } else if (arg == "--headless") {
            window.headless = true;
        } else if (arg == "--camera") {
            // Parsed here rather than in main so a malformed one is refused where every
            // other malformed flag is. All six or none: five numbers is a typo, and
            // starting from a camera that is five-sixths of what was asked for would
            // reproduce something other than the thing being reported.
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
            // Degrees per *frame*, not per second, and refused rather than rounded if it
            // is not a number: the flag exists to make a run repeatable, so a typo that
            // silently span at the default would defeat the only thing it is for.
            const std::string spec = nextString("");
            float degrees = 0.0f;
            if (std::sscanf(spec.c_str(), "%f", &degrees) == 1) {
                camera.spinDegreesPerFrame = degrees;
            } else {
                Logger::warn(LogCategory::Core,
                             "--camera-spin wants degrees of yaw per frame, got '%s'; not spinning", spec.c_str());
            }
        } else if (arg == "--input-script") {
            // Parsed here rather than carried as a string and parsed later, for the reason
            // --camera is parsed here: a malformed one is refused where every other
            // malformed flag is, and at the moment the person who typed it is still
            // looking. `parse` logs which step it choked on and leaves the script empty,
            // so the run continues driving nothing rather than driving half a scenario.
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
            // The counterpart to --realtime, and the reason --locked exists is not
            // symmetry: a tool that depends on frame N being a function of N has to *pin*
            // the clock rather than inherit it. The golden suite learned this once already
            // about the scene path -- eight baselines that depended on an unstated config
            // value failed the day the default scene changed, for a reason with nothing to
            // do with the renderer. golden.sh and baseline.py pass --locked.
            physics.clock = arg == "--locked" ? "locked" : "realtime";
        } else if (arg == "--physics-contacts") {
            physics.debugDraw = true;
            physics.debugContacts = true;
        } else if (arg == "--inspector") {
            // Implies the settings panel, because the inspector is placed to the right
            // of it and a run that asked for one panel's worth of furniture should get
            // both rather than an inspector hanging in space where nothing put it.
            ui.panel = true;
            ui.inspector = true;
        } else if (arg == "--no-occlusion") {
            // The occlusion *parameters* are the game's (see `GameSetup`), because six
            // constants tuned against one scene's geometry are game feel rather than
            // taste. Turning the raycast off is still a per-invocation control: it is how
            // a run isolates the cost of the rays from the cost of the mix.
            //
            // **Unreachable, and known to be** (found by D13, not caused by it):
            // `kBoolFlags` claims `--no-occlusion` for `render.occlusionCulling` and
            // `continue`s, so audio occlusion has had no way to be turned off since the
            // flag tables landed. Two subsystems took the same word for two ideas. Left
            // alone here rather than resolved in passing, because whichever of the two
            // gets renamed is a published flag changing meaning, which is a card of its
            // own -- and `--help` documents only the one that actually fires.
            audio.occlusionOff = true;
        } else if (arg == "--audio-null") {
            // Not the same thing as --no-audio, and the difference is the point: this
            // runs the whole mixer and sends it nowhere, which is what a machine with no
            // sound card and a run that must not make noise both want.
            audio.backend = core::AudioBackend::Null;
        } else if (arg == "--capture") {
            benchmark.capturePath = nextString(benchmark.capturePath);
            // A frame index is required for a capture to mean anything, but demanding
            // two flags to take one screenshot is friction. 60 is past the load hitch
            // and inside every default profiler window.
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
            // Same reasoning as --capture: the readback needs a stated frame, and
            // demanding two flags for one image is friction.
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
            // Parsed here rather than carried as a string, for the reason --camera and
            // --input-script are parsed here: a malformed one is refused where every other
            // malformed flag is, while the person who typed it is still looking. `native`
            // is spelled out because "0x0" is not something anybody would guess.
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
            // A readback is a capture plus an expectation, so it implies the capture the
            // way --golden does rather than making a caller pass three flags to run one
            // check. The path is only defaulted where nothing named one.
            if (benchmark.capturePath.empty()) benchmark.capturePath = "debug_frames/readback/capture.png";
            if (benchmark.captureFrame == 0) benchmark.captureFrame = 60;
        } else if (arg == "--readback-expected") {
            benchmark.readbackExpectedPath = nextString(benchmark.readbackExpectedPath);
        } else if (arg == "--readback-sprite") {
            benchmark.readbackSprite = true;
        } else if (arg == "--readback-sheet-fps") {
            // A sheet is a sprite, so this implies the sprite path rather than making a
            // caller pass both -- the same courtesy --readback pays --capture.
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
            // Same reasoning as --capture: a path with no frame index captures nothing,
            // and asking for two flags to take one capture is friction.
            if (benchmark.rdocCaptureFrame == 0) benchmark.rdocCaptureFrame = 60;
        } else if (arg == "--set") {
            /*
             * The door every row of the table has (D13), and it parses nothing on purpose:
             * splitting on the first `=` is the whole of the branch. `setFromString`
             * already refuses an unknown key, an `engine.` key, a value of the wrong type
             * and an `initOnly` row after the freeze, each with the message it has -- and
             * answers a *moved* key with the sentence saying where it went.
             *
             * The first `=` rather than the only one, because a value may contain one:
             * `--set profiler.outputFile=a=b.json` is a path, not a second assignment.
             */
            const std::string assignment = nextString("");
            const size_t eq = assignment.find('=');
            if (eq == 0 || eq == std::string::npos) {
                // `error` rather than `warn`, by the same distinction a refused name is:
                // this is an instruction the program understood and could not carry out,
                // where an unknown flag is a word it does not know.
                Logger::error(LogCategory::Core, "--set wants <key>=<value>, got `%s` -- setting nothing",
                              assignment.c_str());
                continue;
            }
            const std::string key = assignment.substr(0, eq);
            const std::string text = assignment.substr(eq + 1);
            // One call, and D14 is what made it one. The `namedRowFor` detour this branch
            // used to take existed for the seven rows of the table whose value was a name;
            // none of them is a row any more, so there is no row type left that `--set`
            // has to hold an opinion about and the whole branch is the split and this line.
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
            // The five name lists are printed from the same tables that parse them (D12),
            // rather than spelled here a second time. `--help` was the last place an enum
            // had two lists free to disagree, and a name that only this text knew about
            // was a name the parser would have refused.
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
                "  --headless               create the window unmapped (pairs with --frames)\n"
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

    // A run that captures a frame does not draw the overlay unless it was asked to. The
    // overlay is a frame counter and a millisecond figure that change every frame, so
    // leaving it on by default would make every golden capture differ from every other
    // one in the same corner regardless of what the renderer did -- and a regression
    // suite that always reports a difference reports nothing.
    if (benchmark.captureFrame != 0 && !overlayNamed) render.debugOverlay = false;

    return true;
}

// ================================================================== derived values

/*
 * D12 left a residue here and recorded it: five accessors resolved a name out of a row
 * with a `value_or` that neither door could reach, covering only a third door -- the
 * generated panel's text field over a `String` row -- and unable to log about it because
 * `Engine::endFrame` polled one of them once a frame. **D14 closed it by removing the
 * rows**, which is what that note said it would. `logging.level`, `logging.output`,
 * `render.tonemap`, `render.validation`, `render.rayQuery`, `render.shaderHotReload` and
 * `audio.backend` are fields of their own enum types now, parsed once at the flag that
 * carries them, so there is no spelling to resolve, no fallback to pick and no third door.
 * What is left below is three `Tristate` resolutions, and none of them can fail.
 */
uint32_t Config::logCategoryMask() const {
    uint32_t mask = 0;
    for (const std::string& entry : logging.categories) {
        // `all` is a wildcard over every category rather than one of them, so it is not in
        // the list and is answered here.
        if (core::namesEqual(entry, "all")) return AllLogCategories;
        if (const auto category = core::parseName(logCategoryNames(), entry)) {
            mask |= static_cast<uint32_t>(*category);
            continue;
        }
        // The aggregate's own shape of refusal: there is no previous value to keep for one
        // entry of a list, so the entry is dropped and the rest of the list still applies.
        Logger::error(LogCategory::Core, "--log-categories: `%s` is not one of %s or all -- ignoring it", entry.c_str(),
                      core::legalNames(logCategoryNames()).c_str());
    }
    // Everything unrecognised is the same as saying nothing, and saying nothing must not
    // mean "log nothing" -- a typo that silenced the log would be invisible.
    return mask != 0 ? mask : AllLogCategories;
}

bool Config::validationEnabled(bool debugBuild) const {
    return enabled(render.validation, debugBuild);
}

bool Config::shaderHotReloadEnabled(bool debugBuild) const {
    return enabled(render.shaderHotReload, debugBuild);
}

bool Config::rayQueryAllowed() const {
    // `auto` means "wherever the device offers them", so the only value that turns it off
    // here is an explicit off -- which is what makes `whenAuto` an argument of `enabled`
    // rather than something `Tristate` could have decided for itself, and why there is a
    // `--no-ray-query` and no `--ray-query`: `on` would be indistinguishable from `auto`.
    return enabled(render.rayQuery, true);
}

bool Config::physicsRealtimeClock() const {
    // Not one of D12's enums, deliberately. `physics.clock` has no config key and no name
    // a person types: `--locked` and `--realtime` are the only writers and both assign a
    // canonical spelling, so there is no door for a typo to arrive through and a list of
    // two names would have no second consumer.
    if (core::namesEqual(physics.clock, "locked") || core::namesEqual(physics.clock, "fixed")) return false;
    // A typo falls back to the *default* rather than to the deterministic value, which is
    // the change that matters here: a misspelled clock used to silently run the whole
    // simulation at the frame rate, and "everything is ten times too fast" is a much
    // harder symptom to trace back than a warning is.
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

// ============================================================== the default config

bool writeDefaultConfig(const settings::Settings& table, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::fprintf(stderr, "error: cannot write %s\n", path.string().c_str());
        return false;
    }

    /*
     * The module a row belongs to, in first-appearance order, and every row of a module
     * written under one heading.
     *
     * D16, and it was a real defect rather than tidiness. This used to open a section
     * whenever the module changed while walking rows *in list order*, and the list
     * interleaves -- `render.msaaSamples`, then `scene.characters`, then `render.validation`
     * -- so it wrote `"render"` twice. The round trip passed because rapidjson tolerates a
     * duplicate member and the loader walks all of them; every other JSON reader in the
     * world rejects the file. A declared row can interleave further still, so grouping is
     * what the writer needs regardless.
     */
    const auto moduleOf = [](const settings::Row& r) {
        const std::string_view key = r.key;
        return std::string(key.substr(0, key.find('.')));
    };

    std::vector<std::string> modules;
    for (uint16_t i = 0; i < table.rowCount(); ++i) {
        const settings::Row& r = table.row(static_cast<settings::Id>(i));
        // `engine.` rows are engine-owned live state. Writing them into a file that
        // implies they can be edited would be exactly the confusion the prefix prevents.
        if ((r.flags & settings::kEngine) != 0) continue;
        if (const std::string module = moduleOf(r); std::find(modules.begin(), modules.end(), module) == modules.end()) {
            modules.push_back(module);
        }
    }

    // The aggregate the table cannot hold, emitted at the end of the section it belongs to
    // so the written file is a complete example rather than one missing a key. There was a
    // second, `logging.categories`, and D14 took it out of the file with the three
    // `logging` rows beside it: leaving one aggregate behind would have left the module
    // unclaimed by any row and therefore free for a game to declare into, while the engine
    // was still parsing a key out of it.
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

            // The row's own built-in (D16), rather than the value a throwaway `Settings`
            // was constructed to hold. One less table per invocation, and the default is
            // read where it is written down.
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
