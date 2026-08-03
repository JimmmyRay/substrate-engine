#pragma once

#include "core/Input.h"
#include "core/Logger.h"
#include "core/Names.h"
#include "core/Profiler.h"
#include "core/Settings.h"
#include "gfx/DebugView.h"
#include "scene/AudioBackend.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace core {

/**
 * @brief `auto | on | off`, for a switch whose third state is "you decide".
 *
 * It carries `enabled(v, whenAuto)` rather than resolving `auto` itself because **what
 * `auto` means differs per caller** -- the build type for validation and hot reload, the
 * device's offer for ray query. A `bool` with a default could not hold that.
 */
enum class Tristate : uint32_t {
    Auto = 0,
    On = 1,
    Off = 2,
    Count = 3,
};

/// Every spelling the three tristate rows accept, canonical first: `true` and `false` are
/// input conveniences for `on` and `off` and are never written back.
[[nodiscard]] Names<Tristate> tristateNames();

/// What the switch resolves to, given what `auto` means to this caller.
[[nodiscard]] constexpr bool enabled(Tristate v, bool whenAuto) {
    return v == Tristate::Auto ? whenAuto : v == Tristate::On;
}

/**
 * @brief What `substrate.json` and the command line put into the program.
 *
 * Every scalar preference is a row in `settings`, not a field here. What is left in this
 * struct is the three kinds of thing a row cannot be:
 *
 * 1. **Aggregates** -- a map of action name to binding list, a list of log categories.
 * 2. **Per-invocation developer controls with no JSON key at all**, driven by
 *    `scripts/golden.sh` and `scripts/baseline.py`. Nearly every field below is one.
 * 3. **A default a game supplies** -- `capturePath` starts empty here and is filled from
 *    `GameSetup` unless a flag named one.
 *

 * Authored decisions -- the scene, the sun, gravity, exposure -- are in `GameSetup`
 * instead. Every key that used to be here is in `settings::removedKeys` with a sentence
 * saying where it went, so a stale file gets that sentence rather than silence.
 *
 * See principles.md rule 7 for which of the three a new value is.
 */
struct Config {
    /// The settings table. Loaded from JSON, overridden by flags, seeded by the game, and
    /// dumpable in full with `--dump-settings`.
    settings::Settings settings;

    /// Path the config was loaded from, or empty if defaults were used.
    std::filesystem::path sourcePath;

    // ------------------------------------------------- per-invocation, no JSON key

    struct Window {
        /// Create the window unmapped. A golden run needs pixels, not a display, and the
        /// capture is read back off the swapchain image either way.
        bool headless = false;
    } window;

    struct Scene {
        /// A scene named on the command line, which wins over the game's own. Empty means
        /// the game's choice stands. Every golden case names its own scene this way rather
        /// than inheriting one.
        std::filesystem::path path;
        /// `--characters N`. Zero means the game's choice stands.
        uint32_t characters = 0;
    } scene;

    struct Render {
        /// `--validation auto|on|off`, `--sync-validation`. Layers are chosen when the
        /// Vulkan instance is created; `auto` follows the build type.
        Tristate validation = Tristate::Auto;
        bool syncValidation = false;

        /// `--no-ray-query`. `auto` is "wherever the device offers them", so `on` and
        /// `auto` behave identically and there is no flag for `on`.
        Tristate rayQuery = Tristate::Auto;

        /// `--hot-reload auto|on|off`. A shader recompile loop: `auto` is on in Debug.
        Tristate shaderHotReload = Tristate::Auto;

        /// `--overlay` / `--no-overlay`, and F6 toggles the renderer's own field after
        /// startup. `--capture` turns it off unless a flag named it.
        bool debugOverlay = true;

        /// `--tonemap <name>`, overriding `GameSetup::look.tonemap` for one run.
        /// `tonemapNamed` is what says a flag spoke.
        gfx::TonemapOperator tonemap = gfx::TonemapOperator::Aces;
        bool tonemapNamed = false;

        /// `--debug-view <name>`, parsed to the value at the flag rather than stored as a
        /// string. Storing the spelling let an unrecognised name reach the reader, which
        /// then silently decided it meant `lit`.
        gfx::DebugView debugView = gfx::DebugView::Lit;

        /// `--virtual-resolution WxH`, which wins over `GameSetup::present.virtualResolution`.
        /// Zero means the game's choice stands.
        uint32_t virtualWidth = 0;
        uint32_t virtualHeight = 0;
        /// `--ui-outside-virtual`. Only meaningful beside a virtual resolution, and it is
        /// here rather than as a bool with a default because "the game did not say" and
        /// "the game said true" have to stay distinguishable.
        bool uiOutsideVirtualNamed = false;
    } render;

    struct InputCfg {
        /// Action name to space-separated binding list, in file order. Applied over
        /// whatever defaults each action was declared with, so a file that names three
        /// actions rebinds three and leaves the rest alone. A name no action claims is
        /// warned about and dropped for good, since the next save writes the live map.
        ///
        /// This is the *reader* half only -- the writer is `input::saveBindings`, which
        /// has to preserve every key this struct never looked at.
        std::vector<std::pair<std::string, std::string>> bindings;

        /// `--input-script <frame>:<action>[+|-],...`, and no JSON key. A feed of presses
        /// addressed by frame index, resolved through the binding table exactly as a
        /// device would be. An empty script feeds nothing.
        input::Script script;
    } input;

    struct CameraCfg {
        /// Where to start, from `--camera fx,fy,fz,yaw,pitch,distance` -- the six numbers
        /// the overlay prints, in the order it prints them, angles in degrees. False
        /// leaves the framing `Camera::frameBounds` derives from the scene.
        bool startSet = false;
        glm::vec3 startFocus{0.0f};
        float startYawDegrees = 0.0f;
        float startPitchDegrees = 0.0f;
        float startDistance = 0.0f;

        /// Degrees of yaw per frame, from `--camera-spin D`. Zero leaves the camera
        /// wherever it is. It gives the golden suite its only moving camera, so that a
        /// cache that never invalidates fails something.
        ///
        /// **A fixed step per frame, never wall-clock dt** -- frame 60 has to be the same
        /// frame 60 on every run.
        float spinDegreesPerFrame = 0.0f;
    } camera;

    struct AudioCfg {
        /// `--no-occlusion`, which isolates the cost of the rays from the cost of the mix.
        /// The occlusion *parameters* are the game's, in `GameSetup`.
        bool occlusionOff = false;

        /// `--audio-null`. `auto` takes the device if there is one. `null` is a test mode:
        /// every decoder, filter and bus still runs and nothing reaches a speaker, which
        /// is not the same thing as the `audio.enabled` row muting the output.
        scene::AudioBackend backend = scene::AudioBackend::Auto;

        /// `--audio-debug`, and K toggles it after startup. A line to each source,
        /// coloured by occlusion.
        bool debugDraw = false;
    } audio;

    struct PhysicsCfg {
        /// `realtime` feeds the accumulator the frame's wall-clock delta and interpolates
        /// the render state. `locked` feeds it exactly one step per frame, which makes
        /// every frame a function of the frame index and nothing else.
        ///
        /// **A tool depending on determinism must pin this rather than inherit it** --
        /// `scripts/golden.sh` and `scripts/baseline.py` pass `--locked` explicitly.
        std::string clock = "realtime"; ///< realtime | locked

        /// `--no-physics`: skip the world, to attribute frame time to the solver.
        bool enabled = true;
        /// `--physics-debug` (B toggles), and `--physics-contacts`, which implies it.
        bool debugDraw = false;
        bool debugContacts = false;
    } physics;

    /// The two debug windows. `--panel` (I toggles) and `--inspector` (O toggles, and
    /// implies the panel). A game shipping an options menu draws its own.
    struct Ui {
        bool panel = false;
        bool inspector = false;
    } ui;

    /// The profiler's own struct, filled by `--trace` and `--no-profiler`. The path is
    /// stated *here* rather than in `ProfilerConfig`, whose own default is empty --
    /// "write nothing to disk" is right for a caller that did not ask for a trace.
    ProfilerConfig profiler{.outputFile = "debug_frames/profile.json"};

    /// The session recorder: `--record [seconds]` and `--record-file <path>`.
    /// `Engine::startRecording` is public, so a game that ships clip capture binds a key
    /// to it and holds its own settings.
    struct Record {
        bool enabled = false;
        float seconds = 30.0f;
        uint32_t fps = 30;
        std::string file = "debug_frames/session.mp4";
    } record;

    /// Where the log goes, how loud it is and which subsystems speak. No JSON keys, so
    /// `logging` is unclaimed by the settings table and a game may declare into it.
    struct Logging {
        std::string file = "debug_frames/substrate.log"; ///< `--log-file`
        LogLevel level = LogLevel::Status;               ///< `--log-level`
        LogOutput output = LogOutput::Both;              ///< `--log-output`
        std::vector<std::string> categories{"all"};      ///< `--log-categories a,b,c`
    } logging;

    /// The capture and comparison block, driven by `scripts/golden.sh`,
    /// `scripts/baseline.py` and `scripts/rdoc.sh`. No JSON keys; flags only.
    struct Benchmark {
        /// `--frames N`. Exit after N frames, or 0 to run until closed.
        uint64_t exitAfterFrames = 0;

        /// Frame index to screenshot, or 0 for never. An index rather than "the last
        /// one": the frame a run happens to end on is not a property of the render.
        uint64_t captureFrame = 0;
        /// Empty until a flag names one, at which point `GameSetup::tools.capturePath` no longer
        /// applies. The default itself is the game's.
        std::string capturePath;

        /// Golden image to compare the capture against. Empty captures without comparing.
        /// A mismatch makes the process exit non-zero.
        std::string goldenPath;
        /// Where the difference image goes. Empty writes none.
        std::string diffPath = "debug_frames/diff.png";
        /// Per-channel absolute difference below which a pixel matches, 0..255.
        uint32_t compareTolerance = 2;
        /// How many pixels may exceed that tolerance before the comparison fails.
        uint64_t compareMaxPixels = 0;

        /// Frame index to hand to RenderDoc, or 0 for never. A stated frame, as
        /// `captureFrame` is.
        uint64_t rdocCaptureFrame = 0;
        /// Path *prefix*, not a filename -- RenderDoc appends `_frameNNNN.rdc`. Empty
        /// until a flag names one; the default is the game's.
        std::string rdocCapturePath;

        /// Render target to read back by name, or empty for none. "list" prints the names
        /// this configuration offers and exits.
        std::string captureTarget;
        std::string captureTargetPath = "debug_frames/targets/target.png";
        /// UINT32_MAX means every mip / every layer, which is what makes a bloom chain or
        /// a cascade array one request instead of five.
        uint32_t captureTargetMip = UINT32_MAX;
        uint32_t captureTargetLayer = UINT32_MAX;

        /// Resize the window every N frames, or 0 for never, alternating between the
        /// configured size and one 160x90 smaller (0.4's swapchain drive).
        uint64_t resizeEveryFrames = 0;

        /**
         * @brief `--readback <res:/image.png>`: draw a known image and check it survives.
         *
         * Drawn at 1:1 in the top-left of the overlay's surface, then compared against
         * **that same file expanded by the integer presentation scale**, bit-exact, at the
         * letterbox offset. The expectation is computed from the input rather than snapped
         * from a previous run, so a failure has nothing to re-snap against.
         *
         * **The source has to be opaque.** A translucent one is composited in linear space
         * by the blend unit, and reproducing that on the CPU would put a rounding step
         * between the file and the expectation.
         */
        std::string readbackImage;
        /// Where the expanded expectation is written, so a failure can be looked at beside
        /// the capture and the diff rather than only counted.
        std::string readbackExpectedPath = "debug_frames/readback/expected.png";

        /**
         * @brief `--readback-sprite`: run the same check through the sprite pass.
         *
         * Drawn as one sprite rather than an overlay quad, through an orthographic camera
         * in which one world unit is one texel of the virtual target, top-left corner on
         * texel (0, 0). The comparison is unchanged, so what this adds over the overlay
         * cases is the projection, the texel-to-normalised divide and the premultiplied
         * blend.
         */
        bool readbackSprite = false;

        /**
         * @brief `--readback-sheet-fps <f>`: run the check through a sprite *sheet*.
         *
         * Non-zero turns the sheet case on. The readback image is cut into a two-by-two
         * grid of its own quarters, a four-cell looping clip is played over it at this
         * rate, and the sprite is sized to **one cell** -- so the capture is held against
         * a quarter of the source file rather than the whole of it.
         *
         * A flag rather than a constant so that two cases can run the same number of fixed
         * steps at two rates and expect two different cells; a frame index that came from
         * anywhere but the clock cannot satisfy both.
         */
        float readbackSheetFps = 0.0f;

        /**
         * @brief `--readback-sheet-frame <n>`: which cell the animation must have reached.
         *
         * **Stated by the caller, never derived from the animation.** The expected image is
         * the source file cropped to cell `n`; cropping to whatever cell the animation
         * selected would compare the frame selection against itself and pass for any
         * selection at all. The shell script computes it from the fixed step, the capture
         * frame and the rate above.
         */
        uint32_t readbackSheetFrame = 0;

        /**
         * @brief `--readback-lit-sprite`: run the check through the *lit* path.
         *
         * The same image, camera and corner as `--readback-sprite`, drawn as a quad
         * through the G-buffer instead of as a sprite after the tonemap. **Its value
         * cannot be bit-exact** -- exposure, a BRDF, shadowing and a tonemap curve are
         * applied to it. What survives lighting is *coverage*, which is what
         * `--readback-background` holds it to.
         */
        bool readbackLitSprite = false;

        /**
         * @brief `--readback-lit-cutoff <f>`: the lit sprite's alpha cutoff.
         *
         * A flag rather than a constant because it is what makes the pair of runs a
         * control: **above 1 nothing passes the test and the sprite disappears**, while
         * the material, the instance, the indirect command and the pipeline stay exactly
         * as they were. Omitting the sprite instead would also change the instance count
         * and the draw list.
         */
        float readbackLitCutoff = 0.5f;

        /**
         * @brief `--readback-background <png>`: the frame this one is held against.
         *
         * Turns on the silhouette comparison. Two properties, both computed from the
         * source file rather than snapped: every pixel *outside* the expected silhouette
         * must be bit-identical to this image, and the bounding box of the pixels that do
         * differ must be exactly the silhouette's. See `gfx::compareSilhouette`.
         */
        std::string readbackBackground;

        /// `--sprites <N>`: set the readback's orthographic camera and tile N sprites
        /// across the visible area, so two arms of a before/after differ in the sprite
        /// count and nothing else. Engine-side so `scripts/baseline.py` can drive it
        /// without a game being written for it.
        uint32_t spriteStress = 0;
        /// What `--sprites` draws. Any image the asset trees hold; the readback image is
        /// the default because it is generated rather than fetched.
        std::string spriteStressImage = "res:/readback.png";
        /**
         * @brief `--sprites-move`: nudge every stressed sprite every frame.
         *
         * The arm that measures the upload the revision gate skips. `--sprites N` alone is
         * a static screen that uploads once, which proves the gate fires but not what it
         * saves; this moves the table's revision every frame so the pass pays the whole
         * copy.
         *
         * A sub-texel offset, so the two arms draw the same overdraw at the same overlap
         * and differ in the upload and nothing visible.
         */
        bool spriteStressMove = false;
    } benchmark;

    /// What `--dump-settings` asked for. Serviced by `Engine::init` *after* the config
    /// file, the game's `configure` and the command line have all been applied -- earlier
    /// and it reports the wrong provenance for the thing being debugged.
    enum class Dump : uint8_t { None, Table, Json };
    Dump dumpSettings = Dump::None;

    // -------------------------------------------------------------------- lifetime

    /// Load from `path`. Missing file is not an error -- defaults are used and a warning
    /// logged; the defaults *are* the configuration in that case. False means the file
    /// exists but could not be parsed.
    [[nodiscard]] bool loadFromFile(const std::filesystem::path& path);

    /// Apply command-line overrides. Returns false if the program should exit
    /// (`--help`, `--write-default-config`), with `exitCode` set.
    bool applyCommandLine(int argc, char** argv, int& exitCode);

    // ------------------------------------------------------------- derived values
    //
    // Only what no field can answer for itself: a `Tristate` resolved against what `auto`
    // means to this caller. Everything else is parsed once where the flag arrives.

    [[nodiscard]] uint32_t logCategoryMask() const;
    [[nodiscard]] bool validationEnabled(bool debugBuild) const;
    [[nodiscard]] bool shaderHotReloadEnabled(bool debugBuild) const;
    [[nodiscard]] bool rayQueryAllowed() const;
    /// True for `physics.clock == "realtime"`. Anything unrecognised **warns** and reads
    /// as realtime -- silently, a typo here makes every golden image a function of the
    /// frame rate.
    [[nodiscard]] bool physicsRealtimeClock() const;

    void logSummary() const;
};

/// Written to `path` with every setting `table` holds, generated from the table itself so
/// it cannot drift from the defaults it documents.
///
/// It takes the live table rather than building a throwaway one because a default lives on
/// its row -- only a table that has grown knows every row there is.
bool writeDefaultConfig(const settings::Settings& table, const std::filesystem::path& path);

} // namespace core
