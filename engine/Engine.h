#pragma once

#include "Game.h"
#include "core/Config.h"
#include "core/Input.h"
#include "core/Profiler.h"
#include "gfx/Renderer.h"
#include "gfx/Resources.h"
#include "gfx/VulkanContext.h"
#include "scene/Animation.h"
#include "scene/Audio.h"
#include "scene/Camera.h"
#include "scene/GltfScene.h"
#include "scene/SceneLoader.h"
#include "scene/InstanceTable.h"
#include "scene/LitSprite.h"
#include "scene/Locomotion.h"
#include "scene/SpatialIndex.h"
#include "scene/ParticleSystem.h"
#include "scene/Physics.h"
#include "scene/Scene.h"
#include "scene/Simulation.h"
#include "scene/SpriteTable.h"
#include "ui/BindingMenu.h"
#include "ui/Ui.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct GLFWwindow;



/**
 * @file engine/Engine.h
 * @brief Everything a game does not want to own, and the loop that drives it.
 *
 * Moving the run modes -- `--capture`, `--frames`, `--overlay`, the resize drive -- into the
 * game layer breaks `scripts/baseline.py` and `scripts/golden.sh`, which drive them with no
 * game loaded.
 */
namespace nav {
class NavMesh;
struct NavBuildParams;
} // namespace nav

class Engine {
  public:
    /// **Both defined in `Engine.cpp`, not defaulted here.** Inline, the destructor
    /// destroys twenty-odd by-value members in *the caller's* translation unit, so every
    /// object file that lets an `Engine` go out of scope carries an undefined
    /// `~PhysicsWorld`, `~AudioEngine` and `~SceneLoader` that nothing in it mentions.
    Engine();
    /// The backstop for the path `main` does not cover: `run()` calls into arbitrary game
    /// code, and if that unwinds, `Entry.cpp` never reaches `shutdown()` and the window,
    /// the device and the audio graph are left open. `teardown()` is idempotent, so
    /// whichever arrives first wins.
    ///
    /// Does *not* cover `Logger::critical`, which ends in `std::exit` and so runs no
    /// destructor of any automatic object.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * @brief Read the config, ask the game what it is, open a window and a device, load
     *        the scene.
     *
     * Takes the `Game&` because `Game::configure` has to run after the config file and
     * before the first subsystem: gravity, the scene path and the mix graph are the game's,
     * and the engine builds from all three.
     *
     * @return false when the process should stop before running a frame. Everything built
     *         so far is already torn down, so pairing it with `shutdown()` tears the same
     *         things down twice; return `exitCode()` and nothing else.
     */
    [[nodiscard]] bool init(int argc, char** argv, Game& game);

    [[nodiscard]] int exitCode() const { return exitCodeValue; }

    /// `Game::init`, then the loop, then `Game::shutdown`. Returns the golden-image verdict
    /// as an exit code, which is the only part a shell loop can read.
    int run(Game& game);

    void shutdown();

    /// False when the window has closed or a `--frames` limit was reached, which is the
    /// loop's condition.
    [[nodiscard]] bool beginFrame();

    [[nodiscard]] float dt() const { return frameDelta; }

    /// True while another fixed step should run this frame. Consumes it.
    [[nodiscard]] bool consumeStep();

    [[nodiscard]] float step() const { return simClock.step(); }

    /// Scale simulated time. `0` pauses, `0.25` is bullet time, `1` resumes. Rendering,
    /// input and the UI are outside the fixed step and keep running at any scale, and
    /// **audio sources keep playing** -- silencing them is `audio().setMuted`.
    void setTimeScale(float scale) { simClock.setTimeScale(scale); }
    [[nodiscard]] float timeScale() const { return simClock.timeScale(); }
    [[nodiscard]] bool paused() const { return simClock.paused(); }

    /// Animation, particles, physics and audio, in that order, on the fixed step.
    void simulate(float stepSeconds);

    /// Grow the particle pool to what its live emitters need, resizing the system and the
    /// renderer's buffers together -- resizing either alone leaves the two disagreeing about
    /// how many particles the next dispatch may write.
    void growParticles();

    void endFrame();

    /**
     * @brief Apply `input.bindings` over every declared action, and log the result.
     *
     * **Called by `run()` after `Game::init`, not at the end of `init()`**: a config can
     * only rebind an action that already exists. A hand-driven loop that skips this gets the
     * shipped defaults and silently ignores the user's rebinds.
     */
    void applyBindings();

    core::Config& config() { return configData; }
    core::settings::Settings& settingsTable() { return configData.settings; }
    /// What the game authored, as it was authored -- the world was already built from it, so
    /// nothing here is answerable for what is on screen after `init`.
    const GameSetup& gameSetup() const { return setup; }
    gfx::VulkanContext& context() { return ctx; }
    gfx::Uploader& uploader() { return uploaderData; }
    gfx::Renderer& renderer() { return render; }
    /**
     * @brief The hierarchy. Where a game says that one thing follows another.
     *
     * Updated once a frame, in `endFrame`, after the animation writeback and before the
     * draw -- so a node moved anywhere in a game's frame reaches its instance, its light,
     * its sound and its emitter in that same frame.
     */
    scene::Scene& scene() { return sceneTree; }

    /// The *file* -- meshes, materials, images, rigs. Not `scene()`, which is where things
    /// are rather than what they are made of.
    scene::GltfScene& gltfScene() { return sceneData; }
    scene::InstanceTable& instances() { return instanceTable; }

    /// Images a game loaded, by handle. **The table is the lifetime** -- the renderer holds
    /// only the residency behind each slot, so an image released through it leaks the slot.
    gfx::ImageTable& images() { return imageTable; }

    /**
     * @brief Views the frame renders and does not present: a mirror, a monitor, a minimap.
     *
     * **The table is the lifetime**, as with `images()`; the renderer reconciles against it.
     *
     * A view costs a full pass chain -- see `gfx/ViewTable.h` for the measured number and
     * for what it shares with the presenting view rather than owning.
     */
    gfx::ViewTable& views() { return viewTable; }

    /**
     * @brief Layers and sprites, for a game that is flat.
     *
     * Textures come from `images()` and the projection from `camera()`'s orthographic mode,
     * not a second camera. The pass runs after the tonemap, so a sprite reaches the
     * swapchain as the texels it was authored with.
     */
    scene::SpriteTable& sprites() { return spriteTable; }

    /// The BVH over the instance table, rebuilt when the table changes shape and refitted
    /// when anything moved, once per frame before `Game::frameUpdate` -- so a query made
    /// from a game is against the world as it stands this frame, not last.
    [[nodiscard]] const scene::SpatialIndex& spatialIndex() const { return sceneIndex; }

    /// The walkable surface, baked from the scene's static mesh colliders, and empty when
    /// the scene authored none.
    ///
    /// **Defined in `nav/NavModule.cpp`, and naming it is what links navigation** -- a game
    /// that never calls this or `bakeNavMesh` links no navmesh at all. See that header.
    [[nodiscard]] const nav::NavMesh& navMesh() const;
    /// Bake again with different agent parameters, into `out`. The engine's own navmesh is
    /// left as it was.
    void bakeNavMesh(nav::NavMesh& out, const nav::NavBuildParams& params) const;

    /**
     * @brief Start parsing a scene on a worker thread.
     *
     * Returns immediately; the frame loop keeps running. The result is applied at a frame
     * boundary once it arrives -- never mid-frame, because swapping the scene out from
     * under a recorded command buffer is not something a barrier can fix.
     *
     * @return false when a load is already in flight. One at a time; see `SceneLoader`.
     */
    bool beginLoadScene(const std::filesystem::path& path);
    [[nodiscard]] bool sceneLoadBusy() const { return sceneLoader.busy(); }

    /**
     * @brief Load another glTF into the live scene and instantiate it at `transform`.
     *
     * Not a second scene: the geometry is sub-allocated out of the buffers the renderer
     * already binds. See `GltfScene::appendModel` for what that costs and what it refuses.
     *
     * Colliders are carried into `gltfScene().colliders()` and are bound to bodies by
     * `initPhysics`, so an import authoring them after the world starts gets none.
     *
     * @return `GltfScene::kNoModel` on failure, which is logged with the reason.
     */
    scene::GltfScene::ModelId addModel(const std::filesystem::path& path,
                                       const glm::mat4& transform = glm::mat4(1.0f));

    /**
     * @brief How many world units one authored metre is. Set it **before the first import**.
     *
     * Every `addModel` resizes what it reads by this, so a prop appended at 1x into a 4x
     * world is a doll's house. Scaling the import's node instead is not the same operation:
     * `scene::scaleSceneData` holds rigs and dynamic colliders at their authored size and
     * carries light ranges, intensities and audio falloff with the factor.
     *
     * Loading a scene sets it from `--scene-scale`, so a game writing this afterwards
     * overrides what the run asked for. Non-positive is ignored.
     */
    void setWorldScale(float scale) {
        if (scale > 0.0f) sceneData.sceneScale = scale;
    }
    [[nodiscard]] float worldScale() const { return sceneData.sceneScale; }

    /**
     * @brief Build a mesh in code and put it in the world.
     *
     * Freed by `removeModel`, out of the same allocator as a loaded one.
     *
     * @return `GltfScene::kNoModel` on failure, which is logged with the reason.
     */
    scene::GltfScene::ModelId createMesh(scene::MeshData data);

    /**
     * @brief One more copy of a mesh the scene already holds, at a transform.
     *
     * Building the row by hand instead skips two things this does: a slot past the
     * renderer's instance capacity is written past the end of a mapped staging buffer, and
     * an instance absent from the acceleration structure until something rebuilds it does
     * not appear in a ray-traced reflection.
     *
     * @param model     A model `createMesh` or `addModel` returned. Its **first** primitive
     *                  is the one copied; a multi-primitive model wants `addModel`.
     * @param material  Index into the scene's material table, as `GltfScene::createMaterial`
     *                  returns.
     * @return The new instance, or an invalid handle if `model` names nothing.
     */
    scene::InstanceId addInstance(scene::GltfScene::ModelId model, uint32_t material, const glm::mat4& transform,
                                  scene::InstanceMotion motion);

    /// Destroy a model's instances and give its geometry back.
    void removeModel(scene::GltfScene::ModelId id);

    /**
     * @brief A sprite that goes through the shading path, as one quad and one material.
     *
     * Not `sprites()`, which draws after the tonemap and is bit-identical to its source
     * file; this one is geometry, so it is occluded, shadowed, fogged, reflected and
     * tonemapped. See `scene/LitSprite.h`.
     *
     * Freed by `removeModel`.
     *
     * **Each call takes a material slot and material slots are never reclaimed** -- see
     * `GltfScene::unloadModel`. A scene's headroom is 64.
     *
     * @return `GltfScene::kNoModel` when the material table is full or the mesh will not
     *         fit, both of which are logged with the reason.
     */
    scene::GltfScene::ModelId createLitSprite(const scene::LitSpriteDesc& desc);

    /// Point a lit sprite at a different texel rectangle of its image. Zero width or height
    /// means the whole image, as `LitSpriteDesc::uv` does.
    void setLitSpriteUv(scene::GltfScene::ModelId id, const glm::vec4& uv);

    /// The shader variant a lit sprite's material names, registered on the first call. A
    /// game building its own lit-sprite material needs this for `GpuMaterial::shader`.
    uint32_t litSpriteShader();

    /**
     * @brief The instance slots a model's placements landed in.
     *
     * Empty for an id this scene does not hold, which is also what a model gives back after
     * `removeModel`. Invalidated by the next `addModel`, `createMesh` or scene swap, so a
     * span held across one points at slots that have moved.
     */
    [[nodiscard]] std::span<const scene::InstanceId> instancesOf(scene::GltfScene::ModelId id) const;

    /**
     * @brief The animator character driving a morphed mesh `createMesh` made, which is where
     *        `SceneAnimator::setMorphWeight` drives the weights from.
     *
     * Invalid for a model with no morph targets, for one this scene does not hold, and for
     * one that has been through `removeModel` -- which retires the character, and so
     * invalidates any handle taken before it.
     */
    [[nodiscard]] scene::AnimatorId morphCharacterOf(scene::GltfScene::ModelId id) const;

    /**
     * @brief Write the engine's state and the game's into one file.
     *
     * Two sections, `engine` then `game`, each with its own version. The engine's is keyed
     * by instance slot, so it carries transforms and flags but no rigid body -- a game that
     * needs its bodies back rebuilds them from its own section.
     *
     * @return false if the file could not be written; the reason is logged.
     */
    [[nodiscard]] bool saveGame(const std::filesystem::path& path);

    /**
     * @brief Read a save back, refusing rather than partially applying.
     *
     * **The engine's section is read whole and checked against the world it is being loaded
     * into** -- same scene, same slot count -- before a single transform is written, so a
     * save from another scene is refused instead of scattering one scene's transforms over
     * another's. `Game::load` runs only once the engine's half applied.
     *
     * @return false if the save could not be read or did not apply, with the reason logged.
     */
    [[nodiscard]] bool loadGame(const std::filesystem::path& path);

    /// The version of the `engine` section this build writes and reads. A save carrying a
    /// higher number is refused; a lower one is read in the shape that number described.
    static constexpr uint32_t kEngineSaveVersion = 1;
    scene::SceneAnimator& animator() { return sceneAnimator; }

    /**
     * @brief The controller-to-rig wiring the engine drives every step.
     *
     * A `CharacterVirtual` the scene authored is paired automatically. One a game spawned
     * itself is not, and stays unpaired until the game calls `pair()`.
     */
    scene::LocomotionDriver& locomotion() { return locomotionDriver; }
    scene::ParticleSystem& particles() { return particleSystem; }
    scene::PhysicsWorld& physics() { return physicsWorld; }
    scene::AudioEngine& audio() { return audioEngine; }
    core::input::InputMap& input() { return inputMap; }
    core::input::TextInput& text() { return textInput; }
    /// The camera the frame renders through, the listener follows and `--camera`
    /// reproduces. **Never null**: with nothing installed this is the engine's own base
    /// `Camera`.
    scene::Camera& camera() { return *activeCamera; }

    /**
     * @brief The world ray under the mouse cursor. One half of picking; `physics().raycast`
     *        is the other.
     *
     * Unprojecting the cursor at the call site instead gets both halves wrong: the cursor
     * arrives in *window* pixels while the scene is drawn into a virtual target that may be
     * scaled and letterboxed inside that window, and the aspect the frame was projected at
     * is the render target's, not the window's.
     *
     * Under a cursor the window has never seen this is the ray through the top-left pixel,
     * which is what `InputMap` reports until the first motion event.
     */
    [[nodiscard]] scene::Ray cursorRay() const;

    /**
     * @brief Install a camera, or `nullptr` to go back to the engine's own.
     *
     * The only door, so `deactivate` and `activate` cannot be mis-sequenced: the outgoing
     * camera retires its bindings before the incoming one declares any, which is what stops
     * two cameras owning `Camera.Forward` at once.
     *
     * **Non-owning: the engine never deletes this.** A game that destroys an installed
     * camera calls `setCamera(nullptr)` first, or the engine is left updating freed memory
     * on the next frame.
     *
     * **The *presenting* camera only.** A view's camera goes to `views().setCamera`, and the
     * engine calls neither `activate` nor `update` on it -- installing one here instead puts
     * a second claimant on every binding it declares.
     */
    void setCamera(scene::Camera* c);

    /**
     * @brief The UI context, with this frame's input already fed to it.
     *
     * **Begun lazily**, on the first call in a frame, and ended by `endFrame()`. A panel
     * opened by a game action this frame is drawn this frame only if nothing begun or
     * skipped the context before that action ran.
     */
    ui::Context& ui();

    /// Whether the game wants its panel drawn. Seeded from `ui.panel` and never written by
    /// the engine, which only reads it to decide whether the binding menu may own the
    /// keyboard.
    [[nodiscard]] bool uiVisible() const { return uiOpen; }
    void setUiVisible(bool on) { uiOpen = on; }

    /// The DPI scale everything laid out in logical units multiplies by. Read once, at
    /// init, because the font is baked at a pixel height and the layout has to agree with
    /// it from the first frame.
    [[nodiscard]] float uiScale() const { return uiScaleValue; }

    /// Index of the step `consumeStep()` has just handed out. Only meaningful inside the
    /// step loop; a game reading it outside one is asking about a step that is over.
    [[nodiscard]] uint64_t stepIndex() const { return simClock.stepCount() - 1; }

    /**
     * @brief Every `Character` collider a loaded file declared, in the order they were walked.
     *
     * Which of them anybody is driving is the game's to decide -- see limitations.md, "The
     * engine does not know which character is a player".
     *
     * `node` is the scene node the character was attached to, and what a game parents
     * something to when it wants it carried.
     *
     * **The collider walk's output, not a census of the physics world.** A character made
     * with `PhysicsWorld::createCharacter` is not here, and neither is one a streamed scene
     * brought -- `applyPendingScene` does not re-walk colliders.
     */
    struct AuthoredCharacter {
        scene::PhysicsCharacterId character;
        scene::NodeId node;
    };
    [[nodiscard]] const std::vector<AuthoredCharacter>& authoredCharacters() const { return authored; }


    void requestQuit();

    /// The CPU profiler *and* the renderer's GPU timings.
    void dumpProfile();

    /**
     * @brief Start the session recording, whatever `record.enabled` said at startup.
     *
     * Drives three subsystems -- the renderer's frame tee, the audio capture tap, and the
     * encoders. `renderer().startRecording` alone gives a silent video.
     *
     * `path` empty means the configured `record.file`, so a key-driven recording lands
     * where `--record` would have put it.
     *
     * Refuses a headless run. Already recording is `true` and no second encoder.
     *
     * @return false with the reason logged: headless, no ffmpeg, an unwritable path, or a
     *         swapchain that cannot be read back.
     */
    bool startRecording(const std::filesystem::path& path = {});

    /**
     * @brief Stop it, finish the file, and give back the tap.
     *
     * **Order matters**: the renderer drains its readback slots first because that needs
     * the device, then the encoder is joined, and only then does the audio tap go -- the
     * worker is still reading it until the join returns.
     *
     * @return the file written, or an empty path if nothing was recording or the mux
     *         failed. Safe to call when nothing is.
     */
    std::filesystem::path stopRecording();

    /// Whether `endFrame()` fills `renderer().debugLines` from the physics world and from
    /// the audio sources. Seeded from `physics.debugDraw` and `audio.debugDraw`.
    bool physicsDebugDraw = false;
    bool audioDebugDraw = false;

  private:
    /**
     * @brief What `Scene::add<scene::Model>` calls. Installed as the tree's importer in `init`.
     *
     * The node's world transform is the placement, and every instance the import produced is
     * attached under `node`.
     *
     * @return `GltfScene::kNoModel` on failure, logged with the reason, leaving `node` bare.
     */
    scene::GltfScene::ModelId importModel(scene::NodeId node, const std::filesystem::path& path);

    bool initWindow();
    void initRenderer();
    void loadScene();
    void initAudio();
    void initRecording();
    void initPhysics();

    /// A collider that became a body or character and drives something, passed between the
    /// two halves below.
    struct DrivenBody {
        scene::BodyId body;
        scene::PhysicsCharacterId character;
        uint32_t node;
        /// The collider's name, which is the glTF node's.
        std::string name;
    };
    /// A placement's instance, keyed by the *collider* node rather than the placing node.
    /// The two coincide for a collider authored on the node carrying the mesh and differ for
    /// a rig, where the capsule sits several levels above the skinned meshes -- so keying by
    /// the placing node works until the first rig. See `Placement::colliderNode`.
    struct DrivenSlot {
        uint32_t colliderNode;
        scene::InstanceId instance;
    };

    /**
     * @brief Turn a range of colliders into bodies and characters, and bind sounds to them.
     *
     * **Bodies, then `PhysicsWorld::finalize`, then `bindDrivenNodes` -- in that order and
     * never the obvious one-loop version.** `finalize` takes the world's snapshot, and a
     * rest transform read before it is the identity, so every driven instance carries an
     * offset equal to its own placement and renders at twice its distance from the origin.
     *
     * Appends to `out`; rebuilding or extending `sourceBody` and `authored` is the caller's.
     */
    void createColliderBodies(uint32_t firstCollider, uint32_t colliderCount, std::vector<DrivenBody>& out);

    /// A scene node per driven body, with the meshes and sounds authored on its glTF node
    /// hanging off it. Call only after `PhysicsWorld::finalize`.
    /// @return How many nodes it made.
    uint32_t bindDrivenNodes(const std::vector<DrivenBody>& added, const std::vector<DrivenSlot>& slots);

    /// Pair a controller with the animator character its bound skinned mesh names.
    /// A no-op for an instance that no character deforms, which is most of them.
    void pairLocomotion(scene::PhysicsCharacterId character, scene::InstanceId instance);
    /// Build a soft body for every `FABRIC_` placement. Called from inside `initPhysics`,
    /// not after it -- a cloth built outside the world it collides with falls through it.
    void initCloth();
    void initLights();
    /// Field of view, the scene framing and `--camera`, applied to whatever camera is
    /// installed. **Called by `run()` after `Game::init`, not from `init()`**, for the
    /// reason `applyBindings()` is: a game installs its camera in `Game::init`, and
    /// framing the one it replaced would leave its own pose at the built-in default.
    void applyCameraConfig();
    void initInput();
    void teardown();

    /// The pose an attachment on `node` follows -- the rig that animates it, and character 0
    /// for a node nothing animates.
    [[nodiscard]] const std::vector<glm::mat4>& poseFor(uint32_t node) const;

    static void onFramebufferResize(GLFWwindow* window, int w, int h);
    static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void onChar(GLFWwindow* window, unsigned int codepoint);
    static void onMouseButton(GLFWwindow* window, int button, int action, int mods);
    static void onCursorPos(GLFWwindow* window, double x, double y);
    static void onScroll(GLFWwindow* window, double xoffset, double yoffset);
    static void onWindowFocus(GLFWwindow* window, int focused);

    // **Declaration order is load-bearing here.** Members are destroyed in reverse
    // declaration order, and `Settings::bindLive` parks raw addresses of `render`'s fields
    // inside `configData`. With `configData` declared first, `render` dies first and leaves
    // those pointers dangling for the rest of `~Engine`.
    /// What the game authored, filled by `Game::configure` before anything is built.
    GameSetup setup;
    /// What `--scene-scale` asked for, read by `loadScene`.
    float loadedSceneScale = 1.0f;
    std::filesystem::path configPath = "substrate.json";
    GLFWwindow* window = nullptr;
    gfx::VulkanContext ctx;
    gfx::Uploader uploaderData;
    gfx::Renderer render;
    core::Config configData;
    scene::GltfScene sceneData;
    scene::InstanceTable instanceTable;
    gfx::ImageTable imageTable;
    gfx::ViewTable viewTable;
    /**
     * @brief Everything a step moves, and the only place the order of a step is written.
     *
     * Its position among these members is the teardown order of every subsystem it holds --
     * moving the declaration changes when all of them are destroyed relative to the
     * renderer and the device.
     */
    scene::Simulation sim;
    scene::SpriteTable& spriteTable = sim.sprites;
    scene::SceneAnimator& sceneAnimator = sim.animator;
    scene::LocomotionDriver& locomotionDriver = sim.locomotion;
    scene::ParticleSystem& particleSystem = sim.particles;
    scene::PhysicsWorld& physicsWorld = sim.physics;
    scene::ClothSystem& clothSystem = sim.cloth;
    scene::AudioEngine& audioEngine = sim.audio;
    scene::FixedClock& simClock = sim.clock;
    scene::Scene& sceneTree = sim.tree;
    std::vector<scene::BodyId>& sourceBody = sim.sourceBody;

    scene::SceneLoader sceneLoader;
    /// Instances created per appended model, so `removeModel` can destroy exactly those.
    std::vector<std::vector<scene::InstanceId>> modelInstances;
    /// The animator character driving a code-made morphed mesh, parallel to
    /// `modelInstances` and invalid for every model that has no morph target.
    std::vector<scene::AnimatorId> modelCharacters;
    /// The lit-sprite variant's index, or `kNoLitSpriteShader` until something asks.
    /// Registered once and never re-registered -- a second registration is a second index
    /// for one shader, and materials would disagree about which is theirs.
    static constexpr uint32_t kNoLitSpriteShader = 0xFFFFFFFFu;
    uint32_t litSpriteVariant = kNoLitSpriteShader;
    std::filesystem::path pendingScenePath;
    /// Applied at the top of a frame, never inside one.
    void applyPendingScene();
    scene::SpatialIndex sceneIndex;
    Game* activeGame = nullptr;
    /// The table revision `sceneIndex` was last refitted against.
    uint64_t indexRevision = 0;
    /// Owns the encoders and the worker thread.
    core::Recorder recorder;
    core::input::InputMap inputMap;
    core::input::TextInput textInput;
    ui::BindingMenu bindingMenu;
    ui::Context uiContext;
    /// The UI's own click. **The one action exempted from pointer mode**: everything else on
    /// the mouse goes quiet while the UI has it, and this does not.
    core::input::ActionId uiClickAction = core::input::kInvalidAction;
    /// Also the null camera, which is what keeps `activeCamera` never null.
    scene::Camera nullCamera;
    scene::Camera* activeCamera = &nullCamera;

    /// Characters the collider walk made, with the nodes it attached them to. Rebuilt by
    /// `initPhysics` and appended to by `addModel`, which are the only two places colliders
    /// are walked.
    std::vector<AuthoredCharacter> authored;

    std::optional<core::ProfileScope> frameScope;

    /// Frame 0's `Frame` zone, opened at the top of `init` and closed once `Game::init` has
    /// run. It has to span both calls: a scope opened with the thread stack empty records at
    /// depth 0 as a *sibling* of `Frame`, so everything a game builds at startup lands with
    /// no path to attribute it by.
    std::optional<core::ProfileScope> startupFrameScope;
    std::chrono::steady_clock::time_point lastTime{};
    float frameDelta = 0.0f;
    float uiScaleValue = 1.0f;
    bool realtimeClock = false;
    bool uiOpen = false;
    /// Whether the platform cursor is captured, and where it was grabbed so it can be put
    /// back.
    bool cursorCaptured = false;
    double cursorBeforeCapture[2]{};
    bool uiBegun = false;
    bool closed = false;
    /// Whether `teardown()` has a scene to unload. The `--capture-target list` path in
    /// `init()` tears down before the scene is loaded.
    bool sceneLoaded = false;
    /// Whether `teardown()` has already run. `shutdown()`, `init()` and `~Engine` all reach
    /// it, and only the first to arrive may do anything.
    bool tornDown = false;
    uint32_t droppedStepsReported = 0;
    /// Which of the resize drive's two window sizes is next, and the frame the last one was
    /// asked for on. **`lastResizeFrame` is load-bearing**: a frame that resizes draws
    /// nothing, so `frameCount()` does not advance across it, and pacing off the count
    /// alone re-requests on the very next iteration forever.
    bool resizeSmall = false;
    uint64_t lastResizeFrame = 0;
    int exitCodeValue = 0;
};
