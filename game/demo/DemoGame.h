#pragma once

#include "DemoWorld.h"
#include "Engine.h"
#include "ai/Planner.h"
#include "Game.h"
#include "scene/CameraControllers.h"
#include "ui/Inspector.h"
#include "ui/SettingsUi.h"

#include <string>
#include <vector>

/**
 * @file game/demo/DemoGame.h
 * @brief The demo, and the engine's first consumer (G1).
 *
 * This is what the engine's pre-G1 `main.cpp` was: which toggles exist, what a key
 * means, what the panel says, and how a character is driven when nothing is holding a
 * movement key. None of it is engine policy, and the split is what makes that checkable
 * -- the engine builds and the unit suite runs with this directory absent from the tree.
 */

/**
 * @brief Every action this game acts on, with the key it ships bound to.
 *
 * The switch this replaced was the thing the engine/game separation argument called
 * actively rotting, and the reason was not its length: a key code written into a `case`
 * label is a binding nobody can change, serialise or show on screen. What is left is a
 * handler per feature -- which is irreducible, because each one does something different
 * -- over a binding table that is data.
 *
 * `Camera.*`, `Menu.*` and `Ui.Click` are absent on purpose. The camera, the binding menu
 * and the UI declare their own, because the thing that consumes an action is what should
 * name it -- and all three are the engine's.
 */
struct AppActions {
    core::input::ActionId quit = core::input::kInvalidAction;
    core::input::ActionId msaa[4]{};
    core::input::ActionId view[5]{};
    core::input::ActionId viewCycle = core::input::kInvalidAction;
    core::input::ActionId overlay = core::input::kInvalidAction;
    core::input::ActionId ssao = core::input::kInvalidAction;
    core::input::ActionId bloom = core::input::kInvalidAction;
    core::input::ActionId tonemap = core::input::kInvalidAction;
    core::input::ActionId culling = core::input::kInvalidAction;
    core::input::ActionId edgeMsaa = core::input::kInvalidAction;
    core::input::ActionId rt = core::input::kInvalidAction;
    core::input::ActionId rtShadowMask = core::input::kInvalidAction;
    core::input::ActionId ssr = core::input::kInvalidAction;
    core::input::ActionId fog = core::input::kInvalidAction;
    core::input::ActionId taa = core::input::kInvalidAction;
    core::input::ActionId particles = core::input::kInvalidAction;
    core::input::ActionId physicsDebug = core::input::kInvalidAction;
    core::input::ActionId audioMute = core::input::kInvalidAction;
    core::input::ActionId audioDebug = core::input::kInvalidAction;
    core::input::ActionId panel = core::input::kInvalidAction;
    core::input::ActionId inspector = core::input::kInvalidAction;
    core::input::ActionId screenshot = core::input::kInvalidAction;
    core::input::ActionId renderDoc = core::input::kInvalidAction;
    /// S7. One action for both edges: a recording is a thing that is running or is not,
    /// and two keys for that is two ways to get them out of step.
    core::input::ActionId record = core::input::kInvalidAction;
    core::input::ActionId dumpProfile = core::input::kInvalidAction;
    /// C6. Two actions rather than a menu, because what the demo is showing is that a save
    /// is one call each way -- the menu is a game's decision and this one has no menus.
    core::input::ActionId save = core::input::kInvalidAction;
    core::input::ActionId load = core::input::kInvalidAction;
    /// C12: path the player character to where the camera is standing.
    core::input::ActionId navGo = core::input::kInvalidAction;
    /// C10: re-stream the current scene on a worker thread.
    core::input::ActionId streamScene = core::input::kInvalidAction;
    /// C10: append another glTF into the live scene, and give the last one back.
    core::input::ActionId addModel = core::input::kInvalidAction;
    /// C22: append a *skinned* glTF into the live scene. Its own key rather than a second
    /// path on `addModel`, because the two demonstrate different things -- one that an
    /// import brings its colliders and lights, the other that it brings a skeleton.
    core::input::ActionId addRig = core::input::kInvalidAction;
    core::input::ActionId dropModel = core::input::kInvalidAction;
    /// G3: pick the torch up, and put it back.
    core::input::ActionId carry = core::input::kInvalidAction;
    /// C24: hand the character a *goal* rather than a movement. Nothing is pressed after
    /// this and the character walks over and picks the torch up on its own.
    core::input::ActionId fetch = core::input::kInvalidAction;
    /// G4: build a cube in code and drop it in front of the camera.
    core::input::ActionId spawnCube = core::input::kInvalidAction;
    /// C29: put the character on top of the sliding platform. The only way to get a rider
    /// onto it without walking one there, which is what `scripts/locomotion.sh`'s platform
    /// arm needs and what a respawn or a checkpoint would be.
    core::input::ActionId ridePlatform = core::input::kInvalidAction;
    /// C37: swap the third-person camera for the free-fly one and back. `App.` rather than
    /// `Camera.` and here rather than in a controller, which is the same distinction the
    /// note above draws: *which* camera is running is the application's decision, and the
    /// only `Camera.*` rows are the ones the running controller declared for itself.
    core::input::ActionId cameraToggle = core::input::kInvalidAction;

    void declare(core::input::InputMap& map);
};

/**
 * @brief The character controller's slice of the action map (S4.4).
 *
 * Its own struct rather than five more fields on `AppActions`, for the reason the camera
 * and the binding menu declare their own: the thing that consumes an action is what
 * should name it, and these are consumed by a character rather than by the application's
 * debug surface. Every one is rebindable from the config like all the rest.
 *
 * Declared even when the scene has no character in it. An action nobody reads costs a row
 * in a table; an action that appears and disappears with the scene would make the binding
 * menu's contents a property of which file was loaded.
 */
struct PlayerActions {
    core::input::ActionId forward = core::input::kInvalidAction;
    core::input::ActionId back = core::input::kInvalidAction;
    core::input::ActionId left = core::input::kInvalidAction;
    core::input::ActionId right = core::input::kInvalidAction;
    core::input::ActionId jump = core::input::kInvalidAction;
    /// G12. The gait modifier, and the reason the machine's `walk` state is reachable at
    /// all: without it every request is full travel, the controller assigns that speed
    /// outright, and the band between the walk and run thresholds is never occupied.
    core::input::ActionId run = core::input::kInvalidAction;

    /// Take W, A, S, D, Shift and Space, and nothing else. **Nothing has to give them up
    /// first** (C37): the camera the demo installs beside these declares only
    /// `Camera.Orbit`, and the free-fly one that wants WASD is not running while these are
    /// being read. Until C37 this call site was flanked by five `setDefaultBindings` that
    /// pushed the flycam onto the arrow keys.
    void declare(core::input::InputMap& map);

    /// Where the player is asking to go, in world space, and *how fast* -- the vector's
    /// length is the fraction of the character's top speed being asked for, zero for a
    /// standstill.
    ///
    /// Resolved against the camera, so "forward" means into the screen rather than down
    /// some axis the scene was authored along -- and flattened onto the ground plane,
    /// because a camera looking down at a character should not make it walk into the
    /// floor.
    ///
    /// **The camera itself, not its yaw** (G13). This took a yaw and rebuilt a heading
    /// out of it, and the rebuild disagreed with `Camera::forward()` by a sign: the two
    /// were the same vector only where `cos(yaw)` was zero, which is exactly the yaw
    /// `frameBounds` picks for the showcase scene and therefore the only yaw anything
    /// ever checked. There is now one expression of that basis in the tree and this
    /// derives from it.
    [[nodiscard]] glm::vec3 moveDirection(const core::input::InputMap& in, const scene::Camera& camera) const;
};

/**
 * @brief What the character actually did, as opposed to what it was asked to do (G12).
 *
 * The row's verification rests on this and on nothing else. A scripted run presses keys
 * and the engine answers with a position and a state; the only way to tell an animation
 * *driven* by the controller from one *announced* by the keypress is to report what the
 * machine settled in, at which step, beside how far the solver actually carried the
 * character. So all four numbers come from the far end of the chain --
 * `SceneAnimator::currentState`, `PhysicsWorld::characterTransform` -- and none of them
 * from the input map.
 *
 * Written every fixed step and reported twice: a line per transition, so the *order* and
 * the *timing* are checkable, and one summary at shutdown, so the distance is.
 */
struct LocomotionTrace {
    bool active = false;
    glm::vec3 start{0.0f};
    glm::vec3 previous{0.0f};
    /// Horizontal path length, summed per step. Horizontal because gravity contributes
    /// nothing to it -- a check on straight-line displacement would pass for a character
    /// that only fell.
    float travelled = 0.0f;
    float peakRise = 0.0f;
    /**
     * @brief The three metres-along-a-direction sums G13 added, and why a path length
     *        could not be one of them.
     *
     * `travelled` is deliberately the one quantity that is identical whichever way the
     * character walked, so nothing above it can tell forward from backward -- which is how
     * a heading error survived three checks. Each of these projects the *same* per-step
     * displacement onto a direction taken at that step, so all three are metres and all
     * three divide by `travelled` to give a ratio a run can assert on:
     *
     * - `alongCamera` -- signed, against the camera's own horizontal forward. 1 is a
     *   character walking where the camera points and -1 is one walking away from it.
     * - `acrossCamera` -- unsigned, against the camera's right. A basis that swapped its
     *   axes rather than flipping one lands here rather than in the sum above.
     * - `alongFacing` -- signed, against the **scene node's** forward rather than the
     *   angle the game wrote. Read back out of the tree for the reason every G12 number
     *   is: a facing set on a node the sweep overwrites is one that never happened, and
     *   nothing else in the chain would say so.
     *
     * The camera is sampled per step rather than once, so a run that turns the camera
     * while the character walks asserts the same ratio a straight one does.
     */
    float alongCamera = 0.0f;
    float acrossCamera = 0.0f;
    float alongFacing = 0.0f;
    /**
     * @brief How far the *pose* carried the character's root, in metres, worst over the run.
     *
     * The number that catches a clip moving the character a second time. Every other sum here
     * describes where the solver put the capsule; this one describes where the animation put
     * the mesh inside it, and the two are independent -- a locomotion clip authored with the
     * feet planted walks the rig forward whether or not anything asked it to, and the drawn
     * character then travels at the sum of the two speeds and snaps back to nothing the moment
     * the machine blends to a clip that stands still.
     *
     * Zero is the whole of the claim: `SceneAnimator::setRootNode` holds the named node at its
     * bind translation and hands the delta to the game, so a demo whose controller owns travel
     * expects the pose to contribute exactly none of it. Measured against the first step's
     * value rather than against the bind pose, so it is a claim about *motion* and not about
     * where the rig happens to sit.
     *
     * **Horizontal.** The hold keeps Y on purpose -- a rig binds in a T-pose and animates
     * with bent knees -- so a drift over all three axes measures the hips' authored bob and
     * calls it a defect. It read that way for as long as it existed and was red on six of
     * the eight arms; the axes it measures are now the axes the hold claims.
     */
    float poseDrift = 0.0f;
    glm::vec3 poseRoot{0.0f};
    bool posed = false;
    /**
     * @brief The largest yaw the drawn character ever swung through, in radians, measured
     *        from the heading it started the run with.
     *
     * **The one claim the three ratios above cannot make.** Every one of them divides by
     * `travelled`, and `travelled` is world displacement -- so a character standing still on
     * the sliding platform contributes to all three exactly as a walking one does, and a
     * heading that follows the platform reads as a heading that follows the character. This
     * number does not divide by anything and answers "did it turn", which for a rider who
     * pressed nothing is the whole assertion.
     *
     * Off the tree like `alongFacing`, and off the first `mesh` child only: a part left
     * behind while the others turn is what `alongFacing` catches by taking the worst, and
     * this asks a different question.
     */
    float turned = 0.0f;
    float startYaw = 0.0f;
    bool aimed = false;
    /// Set by the placement action, cleared by the next trace step. A teleport is not
    /// travel, and every distance measured across one is the gap rather than the run.
    bool placed = false;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t changes = 0;
    /// Every state entered, in order, `a > b > c`. Capped, because a long run of a game
    /// that is not being scripted would otherwise grow this without limit.
    std::string path;
};

/**
 * @brief What the settings panel edits, and the two strings it needs to remember (S6).
 *
 * Held by the game rather than by `ui::Context`, which is the immediate-mode bargain
 * stated as a struct: the UI owns *interaction* state -- what is hovered, held and
 * focused -- and the application owns every value on screen. A retained UI would own both
 * and then need a way to tell them apart.
 */
struct PanelState {
    std::string capturePath = "debug_frames/panel.png";
    uint32_t debugView = 0;
    std::vector<std::string> debugViewNames;
    /// C5. Invalid until `init` loads it, and the panel simply does not draw the row while
    /// it is -- which is also what a game shipping without the generated asset gets,
    /// rather than a missing-texture square.
    gfx::ImageId uiImage;
    /// What the last `Save settings` press did, kept until the next one. A save that
    /// reported only in the log would be a button with no visible effect, which is the
    /// one thing a settings panel must not be.
    std::string saveStatus;
};

class DemoGame : public Game {
  public:
    void declareSettings(core::settings::Settings& settings) override;
    void configure(GameSetup& setup, core::settings::Settings& settings) override;
    void init(Engine& e) override;
    void frameUpdate(Engine& e, float dt) override;
    void fixedUpdate(Engine& e, float step) override;
    void drawUi(Engine& e, ui::Context& ui) override;
    void shutdown(Engine& e) override;
    void save(Engine& e, core::SaveWriter& out) override;
    void load(Engine& e, core::SaveReader& in) override;

  private:
    /// G7. Drain the last step's contacts and fire a one-shot at each one worth hearing.
    /// A method rather than ten lines in `fixedUpdate` because it is the whole of what the
    /// row unblocks and it should be findable by name.
    void playImpacts(Engine& e);

    /// C37. Put one of the two cameras in and take the other out, carrying the pose across.
    /// A method because `init` and the `App.Camera` handler both do it and getting either
    /// half wrong is silent: `Engine::setCamera` copies no pose, so a camera swapped in
    /// cold starts wherever it was constructed.
    void installCamera(Engine& e, bool fly);

    /// G12. One fixed step of the locomotion machine's parameters, and the trace of what
    /// came back out of it. A method for the reason `playImpacts` is one: it is the whole
    /// of the row and the thing a reader will come looking for. G13 added the facing,
    /// which is why it now takes the step: a turn rate is degrees per second and this is
    /// the only place with a fixed one to multiply by.
    void driveLocomotion(Engine& e, float step);

    /**
     * @brief D17. The two rows this game declares, held as the typed handles `declare`
     *        returned.
     *
     * Initialised to `Id::None` so that a refused declaration reads and writes nothing
     * rather than quietly assigning `window.width` -- which is what the table promises a
     * refused handle does, and the promise is only worth anything if a game stores it.
     *
     * Both are preferences by principles.md section 7's own test: how loud this game's collisions
     * are, and whether it throws dust when something lands, are properties of the person
     * playing rather than of the program. Neither is `audio.masterVolume` or
     * `render.particles`, which move music and the whole particle subsystem with them.
     *
     * **What the demo deliberately does not declare is a difficulty.** It has no game to be
     * difficult, so a `demo.difficulty` here would be a row nothing reads -- a control that
     * moves, reports success and changes nothing, which is the exact failure the settings
     * table and the generated panel exist to remove. The mechanism is the same either way;
     * what a fair test of it needs is a game with thirty rows across four modules, and this
     * is not one.
     */
    core::settings::Setting<float> impactVolume{core::settings::Id::None};
    core::settings::Setting<bool> impactDust{core::settings::Id::None};

    /// Where this game's panel sits and how big it is, in unscaled UI units. They were
    /// `ui.panelX` and its three siblings until D14, and the reason they moved is the one
    /// D17 built the hook for: the engine draws no panel and reads none of these -- the
    /// *demo* does, in `drawUi`. A key in the engine's `ui` section for the geometry of a
    /// game's window was an engine row serving one game.
    core::settings::Setting<float> panelX{core::settings::Id::None};
    core::settings::Setting<float> panelY{core::settings::Id::None};
    core::settings::Setting<float> panelWidth{core::settings::Id::None};
    core::settings::Setting<float> panelHeight{core::settings::Id::None};

    /**
     * @brief The demo's two cameras, owned here and one of them installed in `init` (C37).
     *
     * The engine holds a non-owning pointer to whichever is active, which is why `shutdown`
     * gives it back before either dies.
     *
     * `followCamera` is what replaced G13's four-line follow rig, and it takes the player
     * node as its target rather than being handed a position every frame -- `Camera::update`
     * runs *before* `frameUpdate`, so a pushed position was always a frame stale. It is the
     * camera the demo installs whenever there is a character to drive; `flyCamera` is what
     * `App.Camera` swaps to, and the swap is the point rather than the feature. Two control
     * schemes really changing hands is what exercises `setCamera`'s deactivate-then-activate
     * and C36's retirement.
     */
    scene::FlyCamera flyCamera;
    scene::ThirdPersonCamera followCamera;
    /// Which of the two is installed. **The demo drives its character only while it is
    /// false**, which is what lets both control schemes ship on W, A, S and D: one of them
    /// is live at a time, so there is nothing left for `PlayerActions::declare` to move out
    /// of the way.
    bool flying = false;

    AppActions actions;
    PlayerActions playerActions;
    PanelState panelState;
    ui::InspectorState inspectorState;
    ui::NodeInspectorState nodeInspectorState;
    bool inspectorOpen = false;
    /// C6, and it is deliberately the least interesting state a game could have: what the
    /// demo demonstrates is the round trip, and a counter that goes back to what it was
    /// shows that as well as a hundred fields would and is readable in one line.
    uint32_t saveCount = 0;
    /// C12. Empty until `V` asks for a path, and emptied again the moment a movement key
    /// is touched -- a navigation system that fought manual input would be the demo
    /// demonstrating the wrong thing.
    scene::PathFollower navFollower;
    /// Appended models, newest last. C10.
    std::vector<scene::GltfScene::ModelId> loadedModels;
    /// G3. A node carrying one of the scene's lights, and whether it is being carried.
    /// Two fields rather than asking the tree, because "is it in my hand" is a game's
    /// question about its own state and not a property of the hierarchy.
    scene::NodeId torchNode;

    // ------------------------------------------------------------ the decision layer (C24)
    /**
     * @brief What the demo can want, and the two things it can do about it.
     *
     * Two actions and one derived route, which is the smallest thing that is a *planner*
     * rather than a lookup: the goal names a carried torch, nobody wrote "walk there first",
     * and `walk to the torch` is in the plan because `pick up the torch` needs what it
     * produces. Adding a third action -- unlock a door on the way -- would need no edit to
     * either of these, which is the property the whole layer is for.
     */
    ai::Planner planner;
    ai::Agent fetcher;
    uint32_t propNearTorch = ai::kNoProperty;
    uint32_t propCarrying = ai::kNoProperty;
    uint32_t actWalkToTorch = 0;
    uint32_t actPickUpTorch = 0;
    /// Whether a goal has been handed over at all. A demo that planned from the first frame
    /// would make eleven golden cases walk somewhere.
    bool fetching = false;
    /// The action reported last, so the log says what changed rather than what is.
    uint32_t fetchStep = ai::Agent::kNoAction;
    uint32_t torchLight = 0;
    bool carrying = false;
    /// G4. The material every spawned cube shares, created once on the first spawn. Its
    /// base colour is written every frame while any cube exists, which is what exercises
    /// the mutable material path -- and it does nothing at all until one is spawned, so a
    /// golden run never touches it.
    uint32_t cubeMaterial = 0xFFFFFFFFu;
    std::vector<scene::GltfScene::ModelId> spawnedCubes;
    /// G5. The variant `game/demo/shaders/hologram.frag` is registered as, and the index
    /// the cube material puts in its `shader` field. Registered in `init` whether or not
    /// a cube is ever spawned, because registration compiles nothing -- the pipelines are
    /// built on the first frame that actually draws one.
    uint32_t hologramVariant = 0;
    /// G7. What a collision sounds like, built once in `init` and varied per impact. Its
    /// `file` is empty when the generated asset is not in the tree, and that is the whole
    /// of the guard -- a clone that never ran `fetch_assets.sh` gets a silent demo rather
    /// than a warning per contact.
    scene::AudioSourceDesc impactSound;
    /// G9. Everything the demo authors in code, and the state the platform needs between
    /// steps. Built in `init` and only into the scene `configure` named -- see DemoWorld.h.
    DemoWorld world;
    LocomotionTrace locomotion;
    /**
     * @brief G13. The nodes the character's meshes hang off, all turned together.
     *
     * **Not `playerNode()`.** That node is the one the character controller drives, and the
     * sweep writes `CharacterVirtual`'s transform straight into its world matrix with no
     * decomposition -- so a local rotation set on it is overwritten every step and silently
     * does nothing. Its *children* are ordinary nodes: the sweep composes their TRS onto
     * a parent whose world transform is already final, which is a rotation about the
     * character's own origin without a matrix ever being decomposed.
     *
     * **A list rather than one node, because a character is routinely several meshes.**
     * `Engine::bindPhysicsToScene` makes one child per placement bound to the body and names
     * every one of them `mesh`; the Mixamo rig in the showcase scene is two, a body and a set
     * of joint caps. Holding only the first turned half a character and left the other half
     * pointing wherever the file authored it.
     *
     * Held rather than looked up per step, and re-resolved when it empties, because
     * `Scene::setParent` puts G3's torch at the head of the same child list and C10 replaces
     * the whole tree.
     */
    std::vector<scene::NodeId> facingNodes;
    /**
     * @brief The rig node whose translation is root motion, or `kNoNode`.
     *
     * `Hips` on the showcase rig, resolved by name because a joint index is a property of the
     * file and hardcoding one is a number nobody can check. Held rather than looked up per
     * step -- `findNode` is linear -- and re-resolved from `kNoNode` after C10 swaps the scene.
     *
     * Kept even in a build that does not apply the hold, because `LocomotionTrace::poseDrift`
     * measures this node and a measurement that disappears with the fix cannot fail without it.
     */
    uint32_t rootJoint = scene::SceneAnimator::kNoNode;
};
