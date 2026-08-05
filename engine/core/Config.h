#pragma once

#include "core/Input.h"
#include "core/Logger.h"
#include "core/Names.h"
#include "core/Profiler.h"
#include "core/Settings.h"
#include "core/DebugView.h"
#include "core/AudioBackend.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace core {

/// @brief `auto | on | off`. What `auto` resolves to differs per caller -- the build type
/// for validation, the device's offer for ray query -- so it is supplied at `enabled()`.
enum class Tristate : uint32_t {
    Auto = 0,
    On = 1,
    Off = 2,
    Count = 3,
};

/// Every spelling the tristate rows accept, canonical first. `true` and `false` are inputs
/// only; dropping a spelling here makes an existing config file fail to parse.
[[nodiscard]] Names<Tristate> tristateNames();

[[nodiscard]] constexpr bool enabled(Tristate v, bool whenAuto) {
    return v == Tristate::Auto ? whenAuto : v == Tristate::On;
}

/**
 * @brief What `substrate.json` and the command line put into the program.
 *
 * A new scalar preference belongs in `settings` as a row, not as a field here -- see
 * principles.md rule 7 for the three cases that cannot be rows. A JSON key retired from
 * here has to land in `settings::removedKeys`, or a stale file gets silence instead of a
 * sentence saying where the key went.
 */
struct Config {
    settings::Settings settings;

    /// Path the config was loaded from, or empty if defaults were used.
    std::filesystem::path sourcePath;

    struct Window {
        /// `--headless`: create the window unmapped.
        bool headless = false;
        /// `--windowed`: map the window even under a frame budget. Without it, `--frames`
        /// implies `--headless` -- so a harness that forgets the flag cannot take the
        /// keyboard off whoever is working while it runs.
        bool windowed = false;
    } window;

    struct Scene {
        /// `--scene <path>`, which wins over the game's own. Empty leaves the game's choice.
        std::filesystem::path path;
        /// `--characters N`. Zero means the game's choice stands.
        uint32_t characters = 0;
    } scene;

    struct Render {
        /// `--validation auto|on|off`, `--sync-validation`. Read when the Vulkan instance is
        /// created, so a later write has no effect; `auto` follows the build type.
        Tristate validation = Tristate::Auto;
        bool syncValidation = false;

        /// `--no-ray-query`. `auto` is "wherever the device offers them", which is why there
        /// is no flag for `on`.
        Tristate rayQuery = Tristate::Auto;

        /// `--hot-reload auto|on|off`. `auto` is on in Debug.
        Tristate shaderHotReload = Tristate::Auto;

        /// `--overlay` / `--no-overlay`; F6 toggles the renderer's own field afterwards.
        bool debugOverlay = true;

        /// `--tonemap <name>`, overriding `GameSetup::look.tonemap` for one run.
        core::TonemapOperator tonemap = core::TonemapOperator::Aces;
        /// Whether a flag spoke. Without it the game's tonemap cannot be told from a flag
        /// that happened to name the same operator.
        bool tonemapNamed = false;

        /// `--debug-view <name>`, parsed at the flag. Storing the spelling instead lets an
        /// unrecognised name reach the reader, which then silently reads as `lit`.
        core::DebugView debugView = core::DebugView::Lit;

        /// `--virtual-resolution WxH`, winning over `GameSetup::present.virtualResolution`.
        /// Zero means the game's choice stands.
        uint32_t virtualWidth = 0;
        uint32_t virtualHeight = 0;
        /// `--ui-outside-virtual`. A flag-spoke marker rather than a bool default, so "the
        /// game did not say" stays distinguishable from "the game said true".
        bool uiOutsideVirtualNamed = false;
    } render;

    struct InputCfg {
        /// Action name to space-separated binding list, in file order. The *reader* half
        /// only; the writer is `input::saveBindings`, which has to preserve every key this
        /// struct never looked at. A name no action claims is dropped for good, because the
        /// next save writes the live map back over the file.
        std::vector<std::pair<std::string, std::string>> bindings;

        /// `--input-script <frame>:<action>[+|-],...`. Presses addressed by frame index,
        /// resolved through the binding table exactly as a device would be.
        input::Script script;
    } input;

    struct CameraCfg {
        /// `--camera fx,fy,fz,yaw,pitch,distance` -- the six numbers the overlay prints, in
        /// that order, angles in degrees. False leaves `Camera::frameBounds`' framing.
        bool startSet = false;
        glm::vec3 startFocus{0.0f};
        float startYawDegrees = 0.0f;
        float startPitchDegrees = 0.0f;
        float startDistance = 0.0f;

        /// Degrees of yaw per frame, from `--camera-spin D`; zero holds the camera still.
        /// A fixed step per frame, never wall-clock dt -- scaling it by dt makes frame 60 a
        /// different frame on every run, and the golden suite's only moving camera useless.
        float spinDegreesPerFrame = 0.0f;
    } camera;

    struct AudioCfg {
        /// `--no-occlusion`. The occlusion *parameters* are the game's, in `GameSetup`.
        bool occlusionOff = false;

        /// `--audio-null`. `null` still runs every decoder, filter and bus, so it is not
        /// interchangeable with the `audio.enabled` row muting the output.
        core::AudioBackend backend = core::AudioBackend::Auto;

        /// `--audio-debug`; K toggles it after startup.
        bool debugDraw = false;
    } audio;

    struct PhysicsCfg {
        /// `realtime` feeds the accumulator wall-clock delta and interpolates; `locked`
        /// feeds exactly one step per frame. A tool depending on determinism has to pass
        /// `--locked` rather than inherit whatever this holds.
        std::string clock = "realtime"; ///< realtime | locked

        /// `--no-physics`: skip the world.
        bool enabled = true;
        /// `--physics-debug` (B toggles), and `--physics-contacts`, which implies it.
        bool debugDraw = false;
        bool debugContacts = false;
    } physics;

    /// `--panel` (I toggles) and `--inspector` (O toggles, and implies the panel).
    struct Ui {
        bool panel = false;
        bool inspector = false;
    } ui;

    /// `--trace` and `--no-profiler`. The path is defaulted here rather than in
    /// `ProfilerConfig`, whose empty default keeps a direct user of the profiler from
    /// writing a trace it never asked for.
    ProfilerConfig profiler{.outputFile = "debug_frames/profile.json"};

    /// `--record [seconds]` and `--record-file <path>`.
    struct Record {
        bool enabled = false;
        float seconds = 30.0f;
        uint32_t fps = 30;
        std::string file = "debug_frames/session.mp4";
    } record;

    /// No JSON keys, so `logging` stays unclaimed by the settings table and a game may
    /// declare rows into it.
    struct Logging {
        std::string file = "debug_frames/substrate.log"; ///< `--log-file`
        LogLevel level = LogLevel::Status;               ///< `--log-level`
        LogOutput output = LogOutput::Both;              ///< `--log-output`
        std::vector<std::string> categories{"all"};      ///< `--log-categories a,b,c`
    } logging;

    /// Driven by `scripts/golden.sh`, `scripts/baseline.py` and `scripts/rdoc.sh`. No JSON
    /// keys; flags only.
    struct Benchmark {
        /// `--frames N`. Exit after N frames, or 0 to run until closed.
        uint64_t exitAfterFrames = 0;

        /// Frame index to screenshot, or 0 for never. An index rather than "the last one":
        /// the frame a run happens to end on is not a property of the render.
        uint64_t captureFrame = 0;
        /// Empty until a flag names one, at which point `GameSetup::tools.capturePath` no
        /// longer applies.
        std::string capturePath;

        /// Golden image to compare the capture against; empty captures without comparing.
        /// A mismatch makes the process exit non-zero.
        std::string goldenPath;
        /// Where the difference image goes. Empty writes none.
        std::string diffPath = "debug_frames/diff.png";
        /// Per-channel absolute difference below which a pixel matches, 0..255.
        uint32_t compareTolerance = 2;
        /// How many pixels may exceed that tolerance before the comparison fails.
        uint64_t compareMaxPixels = 0;

        /// Frame index to hand to RenderDoc, or 0 for never.
        uint64_t rdocCaptureFrame = 0;
        /// Path *prefix*, not a filename -- RenderDoc appends `_frameNNNN.rdc`.
        std::string rdocCapturePath;

        /// Render target to read back by name, or empty for none. "list" prints the names
        /// this configuration offers and exits.
        std::string captureTarget;
        std::string captureTargetPath = "debug_frames/targets/target.png";
        /// UINT32_MAX means every mip / every layer, so a bloom chain or a cascade array is
        /// one request rather than five.
        uint32_t captureTargetMip = UINT32_MAX;
        uint32_t captureTargetLayer = UINT32_MAX;

        /// Resize the window every N frames, or 0 for never, alternating between the
        /// configured size and one 160x90 smaller.
        uint64_t resizeEveryFrames = 0;

        /// @brief `--readback <res:/image.png>`: draw a known image and check it survives.
        ///
        /// The source has to be opaque. A translucent one is composited in linear space by
        /// the blend unit, and reproducing that on the CPU puts a rounding step between the
        /// file and the expectation.
        std::string readbackImage;
        /// Where the expanded expectation is written, so a failure can be looked at beside
        /// the capture and the diff.
        std::string readbackExpectedPath = "debug_frames/readback/expected.png";

        /// `--readback-sprite`: run the same check through the sprite pass, adding the
        /// projection, the texel-to-normalised divide and the premultiplied blend.
        bool readbackSprite = false;

        /// @brief `--readback-sheet-fps <f>`: non-zero runs the check through a sprite
        /// sheet, the readback image cut into a two-by-two grid.
        ///
        /// A flag rather than a constant so two cases can run the same number of fixed
        /// steps at different rates and expect different cells.
        float readbackSheetFps = 0.0f;

        /// @brief `--readback-sheet-frame <n>`: which cell the animation must have reached.
        ///
        /// Stated by the caller, never derived from the animation: cropping the expectation
        /// to whatever cell the animation selected compares the frame selection against
        /// itself and passes for any selection at all.
        uint32_t readbackSheetFrame = 0;

        /// @brief `--readback-lit-sprite`: the same image and corner drawn through the
        /// G-buffer instead of after the tonemap.
        ///
        /// Its value cannot be bit-exact -- exposure, a BRDF, shadowing and a tonemap curve
        /// are applied. Only coverage survives, which is what `--readback-background` holds
        /// it to.
        bool readbackLitSprite = false;

        /// @brief `--readback-lit-cutoff <f>`: the lit sprite's alpha cutoff.
        ///
        /// A flag rather than a constant because above 1 the sprite disappears while the
        /// material, instance, indirect command and pipeline stay as they were -- omitting
        /// the sprite instead would also change the instance count and the draw list.
        float readbackLitCutoff = 0.5f;

        /// @brief `--readback-background <png>`: turns on the silhouette comparison.
        ///
        /// Both its properties are computed from the source file, not snapped: pixels
        /// outside the expected silhouette must be bit-identical to this image, and the
        /// bounding box of those that differ must be exactly the silhouette's. See
        /// `gfx::compareSilhouette`.
        std::string readbackBackground;

        /// `--sprites <N>`: tile N sprites across the readback's orthographic camera, so
        /// two arms of a before/after differ in the sprite count and nothing else.
        uint32_t spriteStress = 0;
        /// What `--sprites` draws. The readback image by default, because it is generated
        /// rather than fetched.
        std::string spriteStressImage = "res:/readback.png";
        /// @brief `--sprites-move`: nudge every stressed sprite every frame.
        ///
        /// Moves the sprite table's revision every frame so the pass pays the whole upload;
        /// `--sprites N` alone uploads once. The offset is sub-texel, so the two arms differ
        /// in the upload and in nothing visible.
        bool spriteStressMove = false;
    } benchmark;

    /// What `--dump-settings` asked for. Serviced by `Engine::init` after the config file,
    /// the game's `configure` and the command line have all been applied; earlier reports
    /// the wrong provenance.
    enum class Dump : uint8_t { None, Table, Json };
    Dump dumpSettings = Dump::None;

    /// Load from `path`. A missing file is not an error -- defaults stand, with a warning.
    /// False means the file exists but could not be parsed.
    [[nodiscard]] bool loadFromFile(const std::filesystem::path& path);

    /// Apply command-line overrides. Returns false if the program should exit
    /// (`--help`, `--write-default-config`), with `exitCode` set.
    bool applyCommandLine(int argc, char** argv, int& exitCode);

    [[nodiscard]] uint32_t logCategoryMask() const;
    [[nodiscard]] bool validationEnabled(bool debugBuild) const;
    [[nodiscard]] bool shaderHotReloadEnabled(bool debugBuild) const;
    [[nodiscard]] bool rayQueryAllowed() const;
    /// True for `physics.clock == "realtime"`. Anything unrecognised warns and reads as
    /// realtime; dropping the warning makes a typo here turn every golden image into a
    /// function of the frame rate, silently.
    [[nodiscard]] bool physicsRealtimeClock() const;

    void logSummary() const;
};

/// Writes `path` from `table` itself, so it cannot drift from the defaults it documents.
/// It must be the live table: a throwaway one is missing every row a game declared.
bool writeDefaultConfig(const settings::Settings& table, const std::filesystem::path& path);

} // namespace core
