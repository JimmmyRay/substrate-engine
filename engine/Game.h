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
 * Properties of the program, not of the person running it -- a row belonging to the latter
 * goes in `core::settings`; see principles.md rule 7.
 *
 * A field added without a working default breaks every game that does not set it.
 */
struct GameSetup {
    /// The program's name, and the window's title.
    std::string name = "Substrate";

    /// Copies of the scene's first skinned mesh, each its own animator character.
    uint32_t characters = 1;

    struct Look {
        /**
         * @brief Lights the game authors, appended to whatever its scene shipped.
         *
         * The **first directional light wins and becomes the sun**, from here or from the
         * scene. The cascades are fitted to one direction and every directional light is
         * routed through them, so a second is lit correctly and shadowed wrongly.
         */
        std::vector<gfx::GpuLight> lights;

        /// A flat ambient added to every surface, as radiance -- colour and magnitude in one
        /// vec3. Black is no ambient at all.
        glm::vec3 ambientColor{0.0f, 0.0f, 0.0f};

        float exposure = 1.0f;

        /// The tonemap curve. `--tonemap <name>` overrides it for one run; the runtime cycle
        /// key moves the renderer's own field and never writes back here.
        core::TonemapOperator tonemap = core::TonemapOperator::Aces;

        /// Shadow bias, in world units: depth along the light ray, normal along the surface.
        /// Too little is acne, too much is peter-panning, and where the line falls depends on
        /// the game's wall thickness and shadow-box reach.
        float shadowDepthBias = 0.02f;
        float shadowNormalBias = 0.04f;
    } look;

    struct Present {
        /// Render at this size, presented at the largest integer scale that fits,
        /// letterboxed. `{0, 0}` renders at the window; `--virtual-resolution WxH` overrides
        /// it. Integer scale, floored -- see rendering.md, "Presentation: virtual resolution
        /// and integer scale".
        glm::uvec2 virtualResolution{0, 0};

        /// Whether the HUD, the UI and the debug text are drawn *inside* the virtual target,
        /// and so magnified with the world. The engine maps the cursor into whichever space
        /// the UI was laid out in, so a game scaling hit-test coordinates itself doubles it.
        bool uiInsideVirtual = true;

        /**
         * @brief Take the frame's remaining sub-texel machinery out of the 2D path.
         *
         * Writes three other settings as `Source::Game`: `render.taa` off (which takes the
         * jitter with it), `render.tonemap` to `clamp`, and the overlay's image array onto a
         * nearest sampler. A game setting any of those three itself is overwritten here; the
         * command line still wins over all three.
         */
        bool pixelExact = false;
    } present;

    /// The step is load-bearing for determinism: changing it changes every golden image.
    struct Sim {
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        float physicsStep = 1.0f / 60.0f;
    } sim;

    struct Audio {
        /// Named gain stages. Leaving this empty yields music, sfx and ambience; naming even
        /// one bus replaces that set, and a scene's `"bus": "sfx"` then resolves to nothing.
        struct Bus {
            std::string name;
            float volume = 1.0f;
            std::string duckedBy;
            float duckAmount = 1.0f;
            float duckAttack = 0.05f;
            float duckRelease = 0.4f;
        };
        std::vector<Bus> buses;

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

        /// Pairs of ears.
        uint32_t listeners = 1;

        /**
         * @brief Whether the engine keeps listener 0 on the camera every frame.
         *
         * The engine writes the listener in `beginFrame`, before `Game::frameUpdate`. Leave
         * this on and a game placing listener 0 itself is silently overwritten every frame;
         * turn it off and nothing places it at all.
         */
        bool listenerFollowsCamera = true;

        struct Occlusion {
            bool enabled = true;
            float cutoffHz = 700.0f;
            float gain = 0.45f;
            float attack = 0.08f;
            float release = 0.25f;
            float rayMargin = 0.3f;
        } occlusion;
    } audio;

    /// Where a tool writes. The matching command-line flags override both.
    struct Tools {
        std::string capturePath = "debug_frames/capture.png";
        std::string rdocCapturePath = "debug_frames/rdoc/frame";
    } tools;

    /// One projected decal.
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
 * One of the three base classes the engine is allowed -- see principles.md, "No abstraction
 * layers over Vulkan".
 *
 * A method added here without an empty default breaks every existing game.
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
     * Keep the handles `declare` returns -- they carry the row's type, so a later `set` of
     * the wrong type fails to compile rather than at run time.
     *
     * **The schema is frozen the moment this returns**, and a `declare` from `configure` is
     * refused: a schema that differed between runs would break the dump,
     * `--write-default-config` and the panel. Prefixes the engine owns -- `render.`,
     * `audio.`, `engine.` -- are refused here.
     */
    virtual void declareSettings(core::settings::Settings&) {}

    /**
     * @brief Once, before any subsystem exists: the authored values.
     *
     * `substrate.json` has been read by the time this runs and **the command line has not**,
     * so a setting read here reflects the user's file but none of the flags.
     *
     * `setDefault` writes only where nothing has claimed the row; `set` overrides the user's
     * file. Both land as `Source::Game` and both lose to the command line.
     */
    virtual void configure(GameSetup&, core::settings::Settings&) {}

    /// Once, before the first frame and after every subsystem is up. `Engine::run` applies
    /// the config's rebinds immediately afterwards, so an action declared any later than
    /// this is one no config can name.
    virtual void init(Engine&) {}

    /// Once per rendered frame, with the wall-clock delta. The camera and the input map are
    /// already resolved for this frame.
    virtual void frameUpdate(Engine&, float /*dt*/) {}

    /// Once per fixed simulation step, *before* the engine's movers run it. A frame may run
    /// this zero times, so anything that must happen every frame does not belong here.
    virtual void fixedUpdate(Engine&, float /*step*/) {}

    /// Once per rendered frame, and only while `Engine::uiVisible()`.
    virtual void drawUi(Engine&, ui::Context&) {}

    /// Once, after the last frame and before the engine tears anything down.
    virtual void shutdown(Engine&) {}

    /**
     * @brief Write whatever the game owns into the save.
     *
     * Called after the engine has written its own section. Open a section with
     * `beginSection` before writing a field, and bump its version whenever the field order
     * changes -- the matching `load` has no other way to reject an older layout.
     */
    virtual void save(Engine&, core::SaveWriter&) {}

    /**
     * @brief Read back what `save` wrote.
     *
     * Called only after the engine's own section applied cleanly, so the world is already
     * restored by the time a game reads here.
     *
     * **Refusal is the game's to make atomic.** `section()` returns false before a byte is
     * consumed, and `ok()` goes false and stays false once a read runs off the end -- a game
     * that checks neither commits half a save over a live world.
     */
    virtual void load(Engine&, core::SaveReader&) {}
};
