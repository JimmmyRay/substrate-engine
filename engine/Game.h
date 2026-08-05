#pragma once

#include "core/SaveFile.h"
#include "core/Settings.h"
#include "core/DebugView.h"
#include "gfx/Light.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class Engine;

namespace ui {
class Context;
}

/**
 * @brief What a game authors, handed to the engine before it builds anything.
 *
 * These are properties of the program, not of the person running it, which is why they are
 * C++ rather than `substrate.json` rows -- see principles.md rule 7. Every field has a
 * working default, so a game that overrides nothing still runs.
 *
 * **Grouped by the subsystem each row configures** (D21). `setup.occlusion` was audio and did
 * not say so, while the engine spends the same word on `Source::occlusion` and on
 * `render.occlusionCulling`, which is a depth test with nothing to do with either; it is
 * `setup.audio.occlusion.enabled` now. Nothing was renamed and nothing changed meaning -- only
 * the address.
 *
 * **There is no `scene` field, and no budgets.** A game composes its world in `init` out of
 * imported assets (C41), and every pool sizes itself and grows past what a game asks for
 * (C40), so neither is a thing to author.
 */
struct GameSetup {
    /// The program's name, and the window's title.
    std::string name = "Substrate";

    /// Copies of the scene's first skinned mesh, each its own animator character.
    uint32_t characters = 1;

    /// What the game looks like: the lighting it authors and the curve that balances it.
    struct Look {
        /**
         * @brief Lights the game authors, appended to whatever its scene shipped.
         *
         * ```cpp
         * setup.look.lights = {gfx::makeDirectionalLight({-0.3f, 0.9f, 0.25f}, {1.0f, 0.95f, 0.86f}, 3.2f)};
         * ```
         *
         * The **first directional light wins and becomes the sun**, whether it came from here
         * or from the scene, because the cascades are fitted to one direction and the shader
         * routes every directional light through them -- a second would be lit correctly and
         * shadowed wrongly. `Engine::initLights` says which one it took.
         */
        std::vector<gfx::GpuLight> lights;

        /**
         * @brief A flat ambient added to every surface, as radiance -- colour and magnitude
         *        in one vec3. Black by default, which is no ambient at all.
         *
         * A placeholder for indirect light with no direction, no bounce and no knowledge of
         * the room. Authored per game because the right value is the colour of the stone in
         * the room, not a constant.
         */
        glm::vec3 ambientColor{0.0f, 0.0f, 0.0f};

        /// A look decision, authored with the lighting it balances.
        float exposure = 1.0f;

        /// The tonemap curve. `--tonemap <name>` overrides it for one run. F11 in the demo
        /// cycles the renderer's own field and does not write back here.
        core::TonemapOperator tonemap = core::TonemapOperator::Aces;

        /**
         * @brief Shadow bias, in world units.
         *
         * Depth bias moves the comparison along the light ray, normal bias moves the sample
         * along the surface normal. Too little is acne, too much is peter-panning, and where
         * the line falls depends on how thin the game's walls are and how far its shadow box
         * reaches -- so it is per game rather than a constant.
         */
        float shadowDepthBias = 0.02f;
        float shadowNormalBias = 0.04f;
    } look;

    /// How the frame reaches the window.
    struct Present {
        /**
         * @brief Render at this size and present it at the largest integer scale that fits,
         *        letterboxed. `{0, 0}` -- the default -- renders at the window.
         *
         * ```cpp
         * setup.present.virtualResolution = {320, 180};
         * ```
         *
         * The scale is an integer and **floors**: a window that is not a multiple of this
         * pays the remainder in bars, never in a fractional resample. A window too small for
         * even 1x crops to the middle rather than shrinking. `--virtual-resolution WxH`
         * overrides it per invocation. See rendering.md, "Presentation: virtual resolution
         * and integer scale".
         */
        glm::uvec2 virtualResolution{0, 0};

        /**
         * @brief Whether the HUD, the UI and the debug text are drawn *inside* the virtual
         *        target, and so magnified with the world.
         *
         * False draws them after the presentation blit, at the window's own resolution, over
         * a letterboxed image. The engine maps the cursor into whichever space the UI was
         * laid out in, so a hit test is correct either way without a game doing arithmetic.
         */
        bool uiInsideVirtual = true;

        /**
         * @brief Take the frame's remaining sub-texel machinery out of the 2D path.
         *
         * Not just a flag -- it writes three other values as `Source::Game`: `render.taa`
         * off (which takes the jitter with it), `render.tonemap` to `clamp`, and the
         * overlay's image array onto a nearest sampler. The command line still wins over all
         * three.
         *
         * Independent of `virtualResolution` in both directions.
         */
        bool pixelExact = false;
    } present;

    /// What the simulation *is*. The step in particular is load-bearing for determinism:
    /// changing it changes every golden image.
    struct Sim {
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        float physicsStep = 1.0f / 60.0f;
    } sim;

    /// The mix graph, the ears, and the filter that puts a wall between them and a source.
    struct Audio {
        /// Named gain stages. Empty gets music, sfx and ambience, so a scene authored with
        /// `"bus": "sfx"` works out of the box.
        struct Bus {
            std::string name;
            float volume = 1.0f;
            std::string duckedBy;
            float duckAmount = 1.0f;
            float duckAttack = 0.05f;
            float duckRelease = 0.4f;
        };
        std::vector<Bus> buses;

        /// Sources placed in code rather than by a glTF node, as `decals` are: there is no
        /// audio content in Sponza.
        struct Source {
            std::string name;
            std::string file;
            glm::vec3 position{0.0f};
            std::string bus;
            float volume = 1.0f;
            bool spatial = true;
            bool loop = true;
            float minDistance = 1.0f;
            float maxDistance = 0.0f;
            std::string load = "auto"; ///< auto | stream | decode
            bool occlusion = true;
        };
        std::vector<Source> sources;

        /// Pairs of ears the engine comes up with (C28). Two is split screen; more than one
        /// is what makes `listenerFollowsCamera` a question rather than a formality.
        uint32_t listeners = 1;

        /**
         * @brief Whether the engine keeps listener 0 on the camera every frame.
         *
         * On, which is what every scene in this tree wants and what the camera-is-the-ears
         * binding has always been. Off is for the three shapes it is wrong for: a
         * first-person game that wants the ears at the character's head rather than at a
         * camera that has moved for a cutscene, a top-down game whose camera hovers tens of
         * metres up so every sound is distant and unpanned, and split screen, where the
         * camera is not any one player's.
         *
         * **A switch rather than an ordering change.** The engine writes the listener in
         * `beginFrame`, before `Game::frameUpdate`, so a game that wrote its own would have
         * it overwritten before it was heard; moving the engine's write after the game's
         * would overwrite the game's instead. Neither order is right for both, so a game
         * says which of the two owns the ears and the other one does not write.
         */
        bool listenerFollowsCamera = true;

        /// Six constants tuned against one scene's geometry.
        struct Occlusion {
            bool enabled = true;
            float cutoffHz = 700.0f;
            float gain = 0.45f;
            float attack = 0.08f;
            float release = 0.25f;
            float rayMargin = 0.3f;
        } occlusion;
    } audio;

    /// Where a tool writes. Not a preference, so not a setting -- but still something a
    /// project may want to point somewhere else, and the flags override both.
    struct Tools {
        std::string capturePath = "debug_frames/capture.png";
        std::string rdocCapturePath = "debug_frames/rdoc/frame";
    } tools;

    /// One projected decal. Placed in code because there is no decal content in this
    /// repository -- Sponza ships none.
    struct Decal {
        glm::vec3 position{0.0f};
        glm::vec3 size{1.0f};
        float rotationY = 0.0f;
        uint32_t texture = 0;
        glm::vec3 tint{1.0f};
        float opacity = 1.0f;
        float edgeFade = 0.25f;
    };
    std::vector<Decal> decals;
};

/**
 * @file engine/Game.h
 * @brief The one interface the engine defines.
 *
 * > **`Game` is the only base class the engine defines, and it sits at the outermost
 * > edge.** A second one appearing inside `engine/gfx/` is still the thing to stop.
 * > See principles.md, "No abstraction layers over Vulkan".
 *
 * Every method has an empty default, so the smallest possible game is a class with no
 * members and no methods. There are no capability flags -- an override that does nothing
 * costs a call whose body is a return.
 */
class Game {
  public:
    Game() = default;
    virtual ~Game() = default;
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    /**
     * @brief Once, before `substrate.json` is read: add this game's own settings rows.
     *
     * ```cpp
     * void MyGame::declareSettings(core::settings::Settings& s) {
     *     difficulty = s.declare("mygame.difficulty", 2, "Difficulty", 1, 3);
     *     subtitleSize = s.declare("mygame.subtitleSize", 18.0f, "Subtitle size", 8.0f, 48.0f);
     * }
     * ```
     *
     * Keep the handles. They are typed as the engine's `core::options::` constants are --
     * only the *id* is assigned at run time -- so `s.set(difficulty, true)` on an integer
     * row still does not compile.
     *
     * **The schema is frozen the moment this returns.** `declareSettings` may only add
     * rows, `configure` may only write them, and a `declare` from `configure` is refused.
     * Nothing is available here -- no `GameSetup`, no engine, no file, no flags -- because
     * a schema that differed between runs would break the dump,
     * `--write-default-config` and the panel.
     *
     * A game owns every module the engine does not name; `render.`, `audio.`, `engine.`
     * and the rest are refused.
     */
    virtual void declareSettings(core::settings::Settings&) {}

    /**
     * @brief Once, before any subsystem exists: the authored values.
     *
     * `substrate.json` has been read by the time this runs and **the command line has
     * not**, and nothing has been created yet -- no window, no device, no scene. Both
     * halves of that ordering are load-bearing: the engine needs gravity and the mix graph
     * before `init` below, and a game reading a setting from here sees the user's file but
     * not the flags.
     *
     * ```cpp
     * s.setDefault(core::options::window::vsync, true);   // my answer unless you said otherwise
     * s.set(core::options::window::vsync, true);          // my answer regardless
     * ```
     *
     * `setDefault` writes only where nothing has claimed the row and is the one to reach
     * for; `set` is for a game that must force a value over the file. Both land as
     * `Source::Game`, and both lose to the command line, applied after this returns.
     */
    virtual void configure(GameSetup&, core::settings::Settings&) {}

    /// Once, before the first frame, and after every subsystem is up. This is where a
    /// game declares its actions -- `Engine::run` applies the config's rebinds
    /// immediately afterwards, so an action declared here is one a config can name.
    virtual void init(Engine&) {}

    /// Once per rendered frame, with the wall-clock delta. The camera and the input map
    /// are already resolved for this frame when it runs.
    virtual void frameUpdate(Engine&, float /*dt*/) {}

    /// Once per fixed simulation step, *before* the engine's movers run it: game intent,
    /// then simulate. A frame may run this zero times, once, or up to the per-frame cap.
    virtual void fixedUpdate(Engine&, float /*step*/) {}

    /// Once per rendered frame, and only while `Engine::uiVisible()`. Immediate mode:
    /// every widget both draws and answers, and the value on screen is the variable.
    virtual void drawUi(Engine&, ui::Context&) {}

    /// Once, after the last frame and before the engine tears anything down.
    virtual void shutdown(Engine&) {}

    /**
     * @brief Write whatever the game owns into the save.
     *
     * Called by `Engine::saveGame` after the engine has written its own section. Open a
     * section first, with a version the matching `load` will check:
     *
     * ```cpp
     * void DemoGame::save(Engine&, core::SaveWriter& out) {
     *     out.beginSection("demo", 1);
     *     out.u32(score);
     *     out.text(playerName);
     * }
     * ```
     *
     * A game that writes nothing produces a save the engine can still load.
     */
    virtual void save(Engine&, core::SaveWriter&) {}

    /**
     * @brief Read back what `save` wrote.
     *
     * Called by `Engine::loadGame` only after the engine's own section applied cleanly, so
     * a game reading this is looking at a world already restored.
     *
     * ```cpp
     * void DemoGame::load(Engine&, core::SaveReader& in) {
     *     if (!in.section("demo", 1)) return;  // absent, or newer than this build reads
     *     score = in.u32();
     *     playerName = in.text();
     *     if (!in.ok()) score = 0;             // truncated; the game decides what that means
     * }
     * ```
     *
     * **Refusal is the game's to make atomic.** `section()` returns false before a byte is
     * consumed, and `ok()` goes false and stays false the moment a read runs off the end.
     */
    virtual void load(Engine&, core::SaveReader&) {}
};
