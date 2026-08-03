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
#include "scene/NavMesh.h"
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
 * ## It wraps nothing
 *
 * Every accessor hands out a reference to the concrete type. `renderer()` is a
 * `gfx::Renderer&`, not a facade, and there is no `Engine::setExposure` because that is
 * `renderer().exposure`.
 *
 * > **`Engine` gets no method that merely forwards to one subsystem.**
 *
 * `dumpProfile()` is allowed because it spans two, `startRecording()` because it spans
 * three, and `requestQuit()` because the alternative is handing a game the GLFW window.
 *
 * ## The engine acts on its own config; the engine binds no keys
 *
 * `--capture`, `--frames`, `--overlay` and the resize drive are run *modes* that
 * `scripts/baseline.py` and `scripts/golden.sh` depend on working with no game involved,
 * so they stay here. **Not one key is bound here and not one panel is drawn here** --
 * debug capabilities are exposed as calls, and which key reaches them is the game's
 * decision. That is what keeps exactly one owner of the keyboard.
 *
 * ## The loop
 *
 * `run(Game&)` is the loop, and it is also the documentation for the hand-driven form,
 * which stays public because tools want to interleave work between the phases:
 *
 * ```cpp
 * while (engine.beginFrame()) {                     // poll, input, camera, the accumulator
 *     game.frameUpdate(engine, engine.dt());
 *     if (engine.uiVisible()) game.drawUi(engine, engine.ui());
 *     while (engine.consumeStep()) {
 *         game.fixedUpdate(engine, engine.step());  // game intent, then simulate
 *         engine.simulate(engine.step());           // animation, particles, physics, audio
 *     }
 *     engine.endFrame();                            // writeback, record, submit, present
 * }
 * ```
 */
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

    // ------------------------------------------------------------------ lifetime

    /**
     * @brief Read the config, ask the game what it is, open a window and a device, load
     *        the scene.
     *
     * The `Game&` is here rather than only in `run()` because of one ordering: the scene
     * path, gravity and the mix graph are the *game's*, and the engine needs all three
     * before it has built anything to hand a game. So `Game::configure` runs after the
     * config file and before the first subsystem.
     *
     * @return false when the process should stop before running a frame -- a malformed
     *         config, a `--help`, `--write-default-config` or `--dump-settings` that has
     *         done its job, or `--capture-target list`. Everything built so far has
     *         already been torn down by the time it returns, so a caller returns
     *         `exitCode()` and nothing else.
     */
    [[nodiscard]] bool init(int argc, char** argv, Game& game);

    [[nodiscard]] int exitCode() const { return exitCodeValue; }

    /// `Game::init`, then the loop, then `Game::shutdown`. Returns the golden-image
    /// verdict as an exit code, which is the only part a shell loop can read.
    int run(Game& game);

    void shutdown();

    // --------------------------------------------------------------------- a frame

    /// Poll, resolve every action once, update the camera and the listener, and open the
    /// simulation's budget of steps. False when the window has closed or a `--frames`
    /// limit was reached, which is the loop's condition.
    [[nodiscard]] bool beginFrame();

    [[nodiscard]] float dt() const { return frameDelta; }

    /// True while another fixed step should run this frame. Consumes it.
    [[nodiscard]] bool consumeStep();

    [[nodiscard]] float step() const { return simClock.step(); }

    /// Scale simulated time. `0` pauses, `0.25` is bullet time, `1` resumes. Rendering,
    /// input and the UI are outside the fixed step and keep running at any scale.
    /// **Audio sources keep playing** -- silencing them is `audio().setMuted`. See
    /// `FixedClock::setTimeScale`.
    void setTimeScale(float scale) { simClock.setTimeScale(scale); }
    [[nodiscard]] float timeScale() const { return simClock.timeScale(); }
    [[nodiscard]] bool paused() const { return simClock.paused(); }

    /// Animation, particles, physics and audio, in that order, on the fixed step.
    void simulate(float stepSeconds);

    /// Grow the particle pool to what its live emitters need, resizing the system and the
    /// renderer's buffers together (C40). Called after each step; a game never calls either
    /// half, and never states a budget.
    void growParticles();

    /// Interpolated writeback, the debug line lists, the config-driven capture and resize
    /// drives, and the recorded frame.
    void endFrame();

    /**
     * @brief Apply `input.bindings` over every declared action, and log the result.
     *
     * **Called by `run()` after `Game::init`, not at the end of `init()`**: a config can
     * only rebind an action that already exists, and a game's actions do not exist until it
     * has declared them. A hand-driven loop that skips this gets the shipped defaults.
     */
    void applyBindings();

    // ---------------------------------------------------------- what a game reaches

    core::Config& config() { return configData; }
    /// The settings table, which is `config().settings`, reachable directly so a game
    /// reading `e.settingsTable().get(core::options::render::ssao)` need not know that.
    core::settings::Settings& settingsTable() { return configData.settings; }
    /// What the game said it was. Read-only after `init`: this is the *authored* block,
    /// and a value that changed after the world was built from it would be a lie.
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
     *
     * The engine builds nodes for what the *file* declared moves. Everything else here is
     * a game's to create.
     */
    scene::Scene& scene() { return sceneTree; }

    /// The *file* -- meshes, materials, images, rigs. `scene()` is where things are; this
    /// is what they are made of, which is why the two names differ.
    scene::GltfScene& gltfScene() { return sceneData; }
    scene::InstanceTable& instances() { return instanceTable; }

    /**
     * @brief Images a game loaded, by handle, with a `destroy` that works.
     *
     * ```cpp
     * hero = e.images().load("res:/sprites/hero.png");   // in init
     * e.images().destroy(hero);                          // in shutdown
     * ```
     *
     * One growable bindless array. The renderer holds the `VkImage` behind each slot and
     * reconciles once a frame; **the table is the lifetime**, which is why it is here
     * rather than behind `renderer()`.
     */
    gfx::ImageTable& images() { return imageTable; }

    /**
     * @brief Views the frame renders and does not present: a mirror, a monitor, a minimap.
     *
     * ```cpp
     * mirror = e.views().create(e.images());          // in init
     * e.views().camera(mirror)->focus = wallPoint;    // in update, every frame
     * mirrorArt = e.views().image(mirror);            // an ImageId like any other
     * ```
     *
     * Here rather than behind `renderer()` for the reason `images()` is: **the table is the
     * lifetime**, and the renderer is the residency that reconciles against it. Taking the
     * image table by argument is what keeps a view's destination the same kind of thing as
     * a loaded texture instead of a second way to name one.
     *
     * A view costs a full pass chain -- see `gfx/ViewTable.h` for the measured number and
     * for the three things it shares with the presenting view rather than owning.
     */
    gfx::ViewTable& views() { return viewTable; }

    /**
     * @brief Layers and sprites, for a game that is flat.
     *
     * ```cpp
     * actors = e.sprites().createLayer({.order = 0});
     * hero   = e.sprites().create(actors, {.image = heroImage, .uv = {0, 0, 16, 16},
     *                                      .size = {16, 16}, .pivot = {0.5f, 1.0f}});
     * e.sprites().setPosition(hero, at);
     * ```
     *
     * Sprites take their textures from `images()` and their projection from `camera()` --
     * the orthographic mode, not a second camera. The pass runs after the tonemap, so a
     * sprite reaches the swapchain as the texels it was authored with.
     */
    scene::SpriteTable& sprites() { return spriteTable; }

    /**
     * @brief The BVH over the instance table, maintained for you.
     *
     * Rebuilt when the table changes shape and refitted when anything moved, once per
     * frame, before the game's `frameUpdate` -- so a query made from a game is against the
     * world as it stands this frame, not last.
     *
     * **Const because the maintenance is the engine's**: a game that refitted it mid-frame
     * would be answering half its own queries against a different instant.
     */
    [[nodiscard]] const scene::SpatialIndex& spatialIndex() const { return sceneIndex; }

    /// The walkable surface, baked from the scene's static mesh colliders. Empty when the
    /// scene authored none, rather than a navmesh over render geometry an agent was never
    /// meant to stand on.
    [[nodiscard]] const scene::NavMesh& navMesh() const { return navigation; }
    /// Bake again with different agent parameters. A game with two body sizes needs two
    /// navmeshes and this is where the second one comes from -- it returns the mesh rather
    /// than replacing the engine's, because the engine has no idea which is "the" agent.
    void bakeNavMesh(scene::NavMesh& out, const scene::NavBuildParams& params) const;

    // ---------------------------------------------------------------------- streaming

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
    /// A background load is in flight.
    [[nodiscard]] bool sceneLoadBusy() const { return sceneLoader.busy(); }

    /**
     * @brief Load another glTF into the live scene and instantiate it.
     *
     * Not a second scene: the geometry lands in the buffers the renderer already binds,
     * sub-allocated out of them. See `GltfScene::appendModel` for what that costs and what
     * it refuses.
     *
     * @return `GltfScene::kNoModel` on failure, which is logged with the reason.
     */
    ///
    /// `transform` places the import in the world (C21). Since C21 this also brings what
    /// the file has besides geometry -- its lights, its emitters and its sounds -- so a
    /// scene can be composed out of several documents instead of one a script baked.
    /// Colliders are carried into `gltfScene().colliders()` and are bound to bodies by
    /// `initPhysics`, so an import that authors them is one made before the world starts.
    scene::GltfScene::ModelId addModel(const std::filesystem::path& path,
                                       const glm::mat4& transform = glm::mat4(1.0f));

    /**
     * @brief How many world units one authored metre is. Set it **before the first import**.
     *
     * Every `addModel` resizes what it reads by this, which is what keeps a composed world in
     * one set of units: a prop appended at 1x into a 4x world is a doll's house. It is not the
     * same operation as scaling the node an import lands on -- `scene::scaleSceneData` holds
     * rigs and dynamic colliders at their authored size and carries light ranges, intensities
     * and audio falloff with the factor, and a placement matrix does none of that.
     *
     * Loading a scene sets it (`--scene-scale`), so a game that overwrites it afterwards is
     * overriding what the run asked for. Non-positive is ignored.
     */
    void setWorldScale(float scale) {
        if (scale > 0.0f) sceneData.sceneScale = scale;
    }
    [[nodiscard]] float worldScale() const { return sceneData.sceneScale; }

    /**
     * @brief Build a mesh in code and put it in the world.
     *
     * The same bookkeeping `addModel` does -- the device wait, the instance, the two
     * renderer calls, the spatial index -- over geometry a game made rather than a file it
     * named. Here rather than in a game because it spans the scene, the instance table and
     * the renderer, and doing it by hand means knowing that `setInstances` resizes the
     * indirect buffer.
     *
     * Freed by `removeModel`, out of the same allocator as a loaded one.
     *
     * @return `GltfScene::kNoModel` on failure, which is logged with the reason.
     */
    scene::GltfScene::ModelId createMesh(scene::MeshData data);

    /**
     * @brief One more copy of a mesh the scene already holds, at a transform (G14).
     *
     * `createMesh` makes a model, a primitive and exactly one instance. A second copy does
     * not want a second model — the geometry is already in the buffer — so what it wants is
     * one more row in the instance table, built out of the `Primitive` the scene holds.
     *
     * Here rather than in a game for the reason `createMesh` is: creating the row is seven
     * field copies, and *knowing what has to happen afterwards* is the part a game cannot
     * be expected to have. A slot past the renderer's instance capacity is written past the
     * end of a mapped staging buffer, and a new instance is absent from the acceleration
     * structure until something rebuilds it, so a ray-traced reflection does not show it.
     * Both are this method's problem, not the caller's.
     *
     * @param model     A model `createMesh` or `addModel` returned. Its **first** primitive
     *                  is the one copied; a multi-primitive model wants `addModel`.
     * @param material  Index into the scene's material table, which is what
     *                  `GltfScene::createMaterial` returns.
     * @param motion    Not defaulted, and not a bool — see `scene::InstanceMotion`.
     * @return The new instance, or an invalid handle if `model` names nothing.
     */
    scene::InstanceId addInstance(scene::GltfScene::ModelId model, uint32_t material, const glm::mat4& transform,
                                  scene::InstanceMotion motion);

    /// Destroy a model's instances and give its geometry back.
    void removeModel(scene::GltfScene::ModelId id);

    // -------------------------------------------------------------------- lit sprites

    /**
     * @brief A sprite that goes through the shading path, as one quad and one material.
     *
     * The other half of `sprites()`. That one draws after the tonemap and is bit-identical
     * to its source file; this one is geometry, so it is occluded, shadowed, fogged,
     * reflected and tonemapped -- and therefore is not. See `scene/LitSprite.h`.
     *
     * A `ModelId` rather than a handle of its own, so it is freed by `removeModel` out of
     * the same allocator as every other model and adds no new lifetime model.
     *
     * **Each call takes a material slot and material slots are never reclaimed** -- see
     * `GltfScene::unloadModel`. A scene's headroom is 64.
     *
     * @return `GltfScene::kNoModel` when the material table is full or the mesh will not
     *         fit, both of which are logged with the reason.
     */
    scene::GltfScene::ModelId createLitSprite(const scene::LitSpriteDesc& desc);

    /**
     * @brief Point a lit sprite at a different texel rectangle of its image.
     *
     * The whole of what animating one takes, and why the rect lives on the material rather
     * than in the quad's vertices: `e.setLitSpriteUv(card, e.sprites().frameUv(sheet,
     * cell))` drives it off the sheet slicing without rebuilding geometry.
     *
     * Zero width or height means the whole image, exactly as `LitSpriteDesc::uv` does.
     */
    void setLitSpriteUv(scene::GltfScene::ModelId id, const glm::vec4& uv);

    /**
     * @brief The shader variant a lit sprite's material names.
     *
     * Registered on the first call rather than at start-up, so a game with no lit sprites
     * pays nothing.
     *
     * Public because a game wanting its own lit-sprite material -- several sprites sharing
     * one, which avoids the material-slot cost above -- needs the index for
     * `GpuMaterial::shader`.
     */
    uint32_t litSpriteShader();

    /**
     * @brief The instance slots a model's placements landed in.
     *
     * What a game needs when it builds a mesh and then wants a body to drive it or a node
     * to carry it. Working it out from the table instead means reproducing the walk in
     * `addPlacementInstances` and agreeing with it forever.
     *
     * Empty for an id this scene does not hold, which is also what a model gives back after
     * `removeModel`. Invalidated by the next `addModel`, `createMesh` or scene swap.
     */
    [[nodiscard]] std::span<const scene::InstanceId> instancesOf(scene::GltfScene::ModelId id) const;

    /**
     * @brief The animator character driving a morphed mesh `createMesh` made.
     *
     * `MeshData::morphTargets` is the shape; this is where the *weights* are driven from --
     * `e.animator().setMorphWeight(e.morphCharacterOf(banner), 0, w)` in `fixedUpdate` is
     * the whole of making one wave. `createMesh` creates and attaches the character,
     * because the instance has to name it before the renderer sizes the weight buffer.
     *
     * Invalid for a model with no morph targets, for one this scene does not hold, and
     * for one that has been through `removeModel` -- which retires the character, and
     * therefore invalidates any handle taken before it.
     */
    [[nodiscard]] scene::AnimatorId morphCharacterOf(scene::GltfScene::ModelId id) const;

    // -------------------------------------------------------------------- persistence

    /**
     * @brief Write the engine's state and the game's into one file.
     *
     * Two sections, `engine` then `game`, each with its own version. The engine's holds
     * what it owns and can put back: the scene it was loaded from, every live instance's
     * transform and flags, and the simulation clock. The game's is whatever
     * `Game::save` streams.
     *
     * **It does not hold a rigid body.** Not because nothing could put one back --
     * `setBodyTransform` and `setLinearVelocity` can -- but because a body has no identity
     * in the file: this section is keyed by instance slot and a body is not an instance. A
     * game that needs its bodies back rebuilds them from its own section.
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
     * another's.
     *
     * `Game::load` is called only once the engine's half applied. A game's own atomicity is
     * a game's to keep.
     *
     * @return false if the save could not be read or did not apply, with the reason logged.
     */
    [[nodiscard]] bool loadGame(const std::filesystem::path& path);

    /// The version of the `engine` section this build writes and reads. A save carrying a
    /// higher number is refused; a lower one is read in the shape that number described.
    static constexpr uint32_t kEngineSaveVersion = 1;
    scene::SceneAnimator& animator() { return sceneAnimator; }

    /**
     * @brief The controller-to-rig wiring the engine drives every step (G15).
     *
     * Paired automatically wherever the engine can see the pairing: a `CharacterVirtual`
     * the scene authored drives whichever animator character its own skinned meshes name.
     * A game with a rig the engine cannot pair -- one it spawned itself -- calls
     * `pair()`; a game whose rig spells its parameters differently calls `setParameters`.
     * Neither is something the demo needs, which is the point.
     */
    scene::LocomotionDriver& locomotion() { return locomotionDriver; }
    scene::ParticleSystem& particles() { return particleSystem; }
    scene::PhysicsWorld& physics() { return physicsWorld; }
    scene::AudioEngine& audio() { return audioEngine; }
    core::input::InputMap& input() { return inputMap; }
    core::input::TextInput& text() { return textInput; }
    /// The camera the frame renders through, the listener follows and `--camera`
    /// reproduces. **Never null**: with nothing installed this is the engine's own base
    /// `Camera`, which is a pose and a projection that takes no input.
    scene::Camera& camera() { return *activeCamera; }

    /**
     * @brief The world ray under the mouse cursor. One half of picking; `physics().raycast`
     *        is the other.
     *
     * ```cpp
     * const scene::Ray ray = e.cursorRay();
     * if (const auto hit = e.physics().raycast(ray.origin, ray.at(100.0f))) { ... }
     * ```
     *
     * A game cannot assemble this from what it holds. The cursor arrives in *window* pixels
     * and the scene is drawn into a virtual target that may be scaled and letterboxed inside
     * that window, so the transform between them is the renderer's; the aspect the frame was
     * projected at is the render target's and not the window's. Both are applied here.
     *
     * Under a cursor the window has never seen the ray is the one through the top-left
     * pixel, which is what `InputMap` reports until the first motion event.
     */
    [[nodiscard]] scene::Ray cursorRay() const;

    /**
     * @brief Install a camera, or `nullptr` to go back to the engine's own.
     *
     * The only door, so `deactivate` and `activate` cannot be forgotten or mis-sequenced:
     * the outgoing camera retires its bindings before the incoming one declares any, which
     * is what stops two cameras owning `Camera.Forward` at once.
     *
     * ```cpp
     * class MyGame : public Game {
     *     scene::FlyCamera flyCam;
     *     void init(Engine& e) override {
     *         flyCam.applySettings(e.settingsTable());
     *         e.setCamera(&flyCam);
     *     }
     *     void shutdown(Engine& e) override { e.setCamera(nullptr); }
     * };
     * ```
     *
     * **Non-owning: the engine never deletes this.** A game that destroys an installed
     * camera calls `setCamera(nullptr)` first, or the engine is left updating freed memory
     * on the next frame.
     *
     * **This is the *presenting* camera and nothing else.** A view's camera is installed
     * with `views().setCamera` and the engine calls neither `activate` nor `update` on it;
     * there is one `InputMap`, and two cameras declaring `Camera.Forward` is the collision
     * this pair exists to prevent.
     */
    void setCamera(scene::Camera* c);

    /**
     * @brief The UI context, with this frame's input already fed to it.
     *
     * **Begun lazily**, on the first call in a frame, and ended by `endFrame()`. The action
     * that opens a panel belongs to the *game* and runs after `beginFrame()`, so a panel
     * opened this frame is drawn this frame only if the context was not begun or skipped
     * before then.
     */
    ui::Context& ui();

    /// Whether the game wants its panel drawn. Seeded from `ui.panel`; the engine reads
    /// it to decide whether the binding menu may own the keyboard, and never sets it.
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
     * The engine names no player. This is the loader's answer to "what did the file author",
     * and which of them anybody is driving is the game's to decide -- an RTS drives none, a
     * party game drives four, a possession game changes its mind. See limitations.md, "The
     * engine does not know which character is a player".
     *
     * `node` is the scene node the engine attached the character to -- what a game parents
     * something to when it wants it carried, since `Scene::setParent` keeps the world
     * transform.
     *
     * **This is the collider walk's output, not a census of the physics world.** A character
     * a game made with `PhysicsWorld::createCharacter` is not here, because no file authored
     * it; and a streamed scene adds nothing, because `applyPendingScene` does not re-walk
     * colliders -- the same C10 limitation that leaves it the rig the first scene brought.
     */
    struct AuthoredCharacter {
        scene::PhysicsCharacterId character;
        scene::NodeId node;
    };
    [[nodiscard]] const std::vector<AuthoredCharacter>& authoredCharacters() const { return authored; }


    void requestQuit();

    /// The CPU profiler *and* the renderer's GPU timings. Two subsystems, which is why
    /// this is a method here rather than two calls at the call site.
    void dumpProfile();

    /**
     * @brief Start the session recording, whatever `record.enabled` said at startup.
     *
     * Three subsystems, which is what earns it a place here: the renderer tees presented
     * frames, the audio engine opens a capture tap, and the `Recorder` that owns the
     * encoders is the engine's. `renderer().startRecording` alone would give a silent
     * video.
     *
     * `path` empty means the configured `record.file`, so a key-driven recording lands
     * where `--record` would have put it.
     *
     * Refuses a headless run -- there is no swapchain to read back. Already recording is
     * `true` and no second encoder.
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
    /// the audio sources. Seeded from `physics.debugDraw` and `audio.debugDraw`; public
    /// because they are plain flags with no derived state.
    bool physicsDebugDraw = false;
    bool audioDebugDraw = false;

  private:
    /**
     * @brief What `Scene::add<scene::Model>` calls. Installed as the tree's importer in `init`.
     *
     * Private because the node is the API: a game says `scene().add<scene::Model>(node, {path})`
     * and never reaches this. The node's world transform is the placement, every instance the
     * import produced is attached under `node`, and the `Model` component the caller gets back
     * records the `ModelId`.
     *
     * @return `GltfScene::kNoModel` on failure, logged with the reason, leaving `node` bare.
     */
    scene::GltfScene::ModelId importModel(scene::NodeId node, const std::filesystem::path& path);

    bool initWindow();
    void initRenderer();
    void loadScene();
    void initAudio();
    /// The `record.enabled` gate, and nothing else -- everything it would have done is in
    /// `startRecording()`, because a game keying a recording does exactly the same work.
    void initRecording();
    void initPhysics();

    /// A collider that became a body or character and drives something, between the two
    /// halves below. Static colliders are absent: nothing has to push their transform
    /// anywhere, because the instance already holds the one the loader placed it at.
    struct DrivenBody {
        scene::BodyId body;
        scene::PhysicsCharacterId character;
        uint32_t node;
        /// The collider's name, which is the glTF node's. Carried so the scene node it
        /// becomes is findable by the name an author wrote rather than by an index.
        std::string name;
    };
    /// A placement's instance, keyed by the collider node that drives it. Keyed by the
    /// *collider* node rather than the placing node: for a collider authored on the node
    /// that carries the mesh those are the same value, and they differ for a rig, where a
    /// capsule goes on the node an author can see and the skinned meshes hang several
    /// levels below it. See `Placement::colliderNode`.
    struct DrivenSlot {
        uint32_t colliderNode;
        scene::InstanceId instance;
    };

    /**
     * @brief Turn a range of colliders into bodies and characters, and bind sounds to them.
     *
     * Half of one walk, and the split is where the two callers genuinely differ:
     * `initPhysics` runs `initCloth()` between this and `finalize()`, and `addModel` must
     * not. **Bodies, then `PhysicsWorld::finalize`, then `bindDrivenNodes` -- in that order
     * and never the obvious one-loop version.** `finalize` takes the world's snapshot, and a
     * rest transform read before it is the identity, so every driven instance would carry an
     * offset equal to its own placement and render at twice its distance from the origin.
     *
     * Appends to `out`; the caller decides whether `sourceBody` and `authored` are rebuilt
     * or extended, which is the other thing the two do differently.
     */
    void createColliderBodies(uint32_t firstCollider, uint32_t colliderCount, std::vector<DrivenBody>& out);

    /// The other half: a scene node per driven body, with the meshes and sounds authored on
    /// its glTF node hanging off it. Call only after `PhysicsWorld::finalize`.
    /// @return How many nodes it made.
    uint32_t bindDrivenNodes(const std::vector<DrivenBody>& added, const std::vector<DrivenSlot>& slots);

    /// Pair a controller with the animator character its bound skinned mesh names (G15).
    /// A no-op for an instance that no character deforms, which is most of them.
    void pairLocomotion(scene::PhysicsCharacterId character, scene::InstanceId instance);
    /// Build a soft body for every `FABRIC_` placement. Called from `initPhysics` rather
    /// than beside it, because a cloth has to be in the world it collides with.
    void initCloth();
    void initNavigation();
    void initLights();
    /// Field of view, the scene framing and `--camera`, applied to whatever camera is
    /// installed. **Called by `run()` after `Game::init`, not from `init()`**, for the
    /// reason `applyBindings()` is: a game installs its camera in `Game::init`, and
    /// framing the one it replaced would leave its own pose at the built-in default.
    void applyCameraConfig();
    void initInput();
    void teardown();

    /// The pose an attachment on `node` follows -- the rig that animates it, resolved by
    /// `SceneAnimator::characterForNode`, and character 0 for a node nothing animates. Two
    /// sweeps in two different functions ask this, which is why it is a method and not a
    /// lambda in either of them.
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
    /// What `--scene-scale` asked for. Read by `loadScene`; a game states its own with
    /// `setWorldScale` instead.
    float loadedSceneScale = 1.0f;
    std::filesystem::path configPath = "substrate.json";
    GLFWwindow* window = nullptr;
    gfx::VulkanContext ctx;
    gfx::Uploader uploaderData;
    gfx::Renderer render;
    core::Config configData;
    scene::GltfScene sceneData;
    scene::InstanceTable instanceTable;
    /// Owned here rather than by the renderer, as `instanceTable` is: the lifetime is the
    /// game's, and what the renderer keeps is the residency behind it.
    gfx::ImageTable imageTable;
    gfx::ViewTable viewTable;
    /**
     * @brief Everything a step moves, and the only place the order of a step is written.
     *
     * Declared exactly where `spriteTable` used to be, which is not cosmetic: members are
     * destroyed in reverse declaration order, and every subsystem this absorbed was declared
     * after that point. Put it anywhere else and the teardown order changes.
     *
     * The references below are aliases onto its members, so the ~200 uses of
     * `physicsWorld`, `sceneAnimator` and the rest read as they always did. They are
     * references rather than accessors because an accessor would have made every one of
     * those a call site to rewrite for no gain.
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

    scene::NavMesh navigation;
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
    /// Retained from `init` so `saveGame` can reach `Game::save` without every caller
    /// passing the game back to the engine that already has it.
    Game* activeGame = nullptr;
    /// The table revision the index was last refitted against, so a frame in which nothing
    /// moved costs nothing at all rather than an O(n) walk that changes no box.
    uint64_t indexRevision = 0;
    /// Owns the encoders and the worker thread. Declared here rather than in `Renderer`
    /// because it is not a graphics object -- it takes bytes from the swapchain readback
    /// and samples from the audio tap, and neither half belongs to the other.
    core::Recorder recorder;
    core::input::InputMap inputMap;
    core::input::TextInput textInput;
    ui::BindingMenu bindingMenu;
    ui::Context uiContext;
    /// The UI's own click, declared here because the thing that consumes an action is what
    /// names it. **The one action exempted from pointer mode**: everything else on the
    /// mouse goes quiet while the UI has it, and this does not.
    core::input::ActionId uiClickAction = core::input::kInvalidAction;
    /// The engine's own camera, and **the null camera as well** -- "looks at the scene and
    /// takes no input" is simultaneously what the base type is and what a null object has
    /// to be, so there is no second type here and `activeCamera` is never null.
    scene::Camera nullCamera;
    scene::Camera* activeCamera = &nullCamera;

    /// Characters the collider walk made, with the nodes it attached them to. Rebuilt by
    /// `initPhysics` and appended to by `addModel`, which are the only two places colliders
    /// are walked.
    std::vector<AuthoredCharacter> authored;

    std::optional<core::ProfileScope> frameScope;

    /// Frame 0's `Frame` zone, opened at the top of `init` and closed once `Game::init`
    /// has run. A member rather than a local because it has to span two calls: `init` and
    /// the first part of `run`. Everything a game builds at startup -- meshes, materials,
    /// bodies -- happens in the second, and a scope opened with the thread stack empty
    /// records at depth 0 as a *sibling* of `Frame` with no path to attribute it by.
    std::optional<core::ProfileScope> startupFrameScope;
    std::chrono::steady_clock::time_point lastTime{};
    float frameDelta = 0.0f;
    float uiScaleValue = 1.0f;
    bool realtimeClock = false;
    bool uiOpen = false;
    /// Whether the platform cursor is currently captured, and where it was grabbed so it
    /// can be put back. See `Engine::frame`, where the two are set from
    /// `core::input::mouseGrabbed()`.
    bool cursorCaptured = false;
    double cursorBeforeCapture[2]{};
    bool uiBegun = false;
    bool closed = false;
    /// Whether `destroy()` has anything to destroy. `init()` tears itself down on the
    /// `--capture-target list` path, which runs before the scene is loaded.
    bool sceneLoaded = false;
    /// Whether `teardown()` has already run. Three callers -- `shutdown()`, the
    /// `--capture-target list` path in `init()`, and `~Engine` -- and only the first to
    /// arrive should do anything.
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
