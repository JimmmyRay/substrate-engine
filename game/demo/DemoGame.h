#pragma once

#include "DemoWorld.h"
#include "Engine.h"
#include "nav/NavModule.h"
#include "ai/Planner.h"
#include "Game.h"
#include "scene/CameraControllers.h"
#include "ui/Inspector.h"
#include "ui/SettingsUi.h"

#include <string>
#include <vector>

/**
 * @file game/demo/DemoGame.h
 * @brief The demo, and the engine's first consumer: its toggles, key meanings, panel and
 *        character driving. None of it is engine policy.
 */

/// Every action this game acts on, with the key it ships bound to.
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
    /// Starts and stops a recording; splitting it into two keys is two ways to get the
    /// start and the stop out of step.
    core::input::ActionId record = core::input::kInvalidAction;
    core::input::ActionId dumpProfile = core::input::kInvalidAction;
    core::input::ActionId save = core::input::kInvalidAction;
    core::input::ActionId load = core::input::kInvalidAction;
    /// Path the player character to where the camera is standing.
    core::input::ActionId navGo = core::input::kInvalidAction;
    /// Re-stream the current scene on a worker thread.
    core::input::ActionId streamScene = core::input::kInvalidAction;
    /// Append another glTF into the live scene, and give the last one back.
    core::input::ActionId addModel = core::input::kInvalidAction;
    /// Append a *skinned* glTF into the live scene.
    core::input::ActionId addRig = core::input::kInvalidAction;
    core::input::ActionId dropModel = core::input::kInvalidAction;
    /// Pick the torch up, and put it back.
    core::input::ActionId carry = core::input::kInvalidAction;
    /// Hand the character a *goal* rather than a movement.
    core::input::ActionId fetch = core::input::kInvalidAction;
    /// Build a cube in code and drop it in front of the camera.
    core::input::ActionId spawnCube = core::input::kInvalidAction;
    /// Put the character on top of the sliding platform.
    /// `scripts/locomotion.sh`'s platform arm drives this action; retiring it breaks that run.
    core::input::ActionId ridePlatform = core::input::kInvalidAction;
    /// Swap the third-person camera for the free-fly one and back.
    core::input::ActionId cameraToggle = core::input::kInvalidAction;

    void declare(core::input::InputMap& map);
};

/**
 * @brief The character controller's slice of the action map.
 *
 * Declared whether or not the scene has a character in it: making the declaration
 * conditional makes the binding menu's contents a property of which file was loaded.
 */
struct PlayerActions {
    core::input::ActionId forward = core::input::kInvalidAction;
    core::input::ActionId back = core::input::kInvalidAction;
    core::input::ActionId left = core::input::kInvalidAction;
    core::input::ActionId right = core::input::kInvalidAction;
    core::input::ActionId jump = core::input::kInvalidAction;
    /// The gait modifier. Drop it and every request is full travel, so the band between
    /// the walk and run thresholds is never occupied and the machine's `walk` state
    /// becomes unreachable.
    core::input::ActionId run = core::input::kInvalidAction;

    /// Takes W, A, S, D, Shift and Space. Nothing has to give them up first: the free-fly
    /// camera that also wants WASD is never running while these are being read, so moving
    /// either off its keys to deconflict them is a fix for a collision that cannot happen.
    void declare(core::input::InputMap& map);

    /// Where the player is asking to go, in world space. The vector's length is the
    /// fraction of the character's top speed being asked for, zero for a standstill.
    ///
    /// Takes the camera itself, not its yaw. Rebuilding a heading from a yaw disagrees
    /// with `Camera::forward()` by a sign, and the two agree only where `cos(yaw)` is
    /// zero -- which is exactly the yaw `frameBounds` picks for the showcase scene, so a
    /// run against it is the one run that cannot catch the error.
    [[nodiscard]] glm::vec3 moveDirection(const core::input::InputMap& in, const scene::Camera& camera) const;
};

/**
 * @brief What the character actually did, as opposed to what it was asked to do.
 *
 * Every number here is read from the far end of the chain -- `SceneAnimator::currentState`,
 * `PhysicsWorld::characterTransform` -- and none from the input map. Sourcing any of them
 * from the keypress instead makes the trace agree with itself whatever the engine did.
 */
struct LocomotionTrace {
    bool active = false;
    glm::vec3 start{0.0f};
    glm::vec3 previous{0.0f};
    /// Horizontal path length in metres, summed per step. Straight-line displacement
    /// instead would pass for a character that only fell.
    float travelled = 0.0f;
    float peakRise = 0.0f;
    /**
     * @brief Metres of per-step displacement projected onto a direction taken at that step,
     *        so each divided by `travelled` is a ratio a run can assert on.
     *
     * `alongCamera` is signed against the camera's horizontal forward, 1 for a character
     * walking where the camera points; `acrossCamera` is unsigned against the camera's
     * right, so a basis that swapped its axes rather than flipping one lands here instead;
     * `alongFacing` is signed against the **scene node's** forward and not the angle the
     * game wrote, because a facing set on a node the sweep overwrites is one that never
     * happened and nothing else in the chain would say so.
     *
     * The camera is sampled per step; sampling it once makes a run that turns the camera
     * mid-walk report a ratio that is about the turn rather than about the character.
     */
    float alongCamera = 0.0f;
    float acrossCamera = 0.0f;
    float alongFacing = 0.0f;
    /**
     * @brief How far the *pose* carried the character's root, in metres, worst over the run.
     *        Expected to be zero in a demo whose controller owns travel.
     *
     * Horizontal, and measured against the first step's value rather than the bind pose.
     * The hold keeps Y on purpose -- a rig binds in a T-pose and animates with bent knees --
     * so widening this to all three axes measures the hips' authored bob and reports it as
     * a defect.
     */
    float poseDrift = 0.0f;
    glm::vec3 poseRoot{0.0f};
    bool posed = false;
    /**
     * @brief The largest yaw the drawn character ever swung through, in radians, measured
     *        from the heading it started the run with.
     *
     * Deliberately divides by nothing. The three ratios above all divide by `travelled`,
     * which is world displacement, so a character standing still on the sliding platform
     * feeds them exactly as a walking one does and a heading following the platform reads
     * as a heading following the character.
     *
     * Off the first `mesh` child only; a part left behind while the others turn is what
     * `alongFacing` catches by taking the worst.
     */
    float turned = 0.0f;
    float startYaw = 0.0f;
    bool aimed = false;
    /// Set by the placement action, cleared by the next trace step. A distance measured
    /// across a teleport is the gap, not the run.
    bool placed = false;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t changes = 0;
    /// Every state entered, in order, `a > b > c`. Capped: a long unscripted run would
    /// otherwise grow this without limit.
    std::string path;
};

/// What the settings panel edits, and the two strings it needs to remember.
struct PanelState {
    std::string capturePath = "debug_frames/panel.png";
    uint32_t debugView = 0;
    std::vector<std::string> debugViewNames;
    /// Invalid until `init` loads it, and stays invalid in a tree without the generated
    /// asset -- so the panel must check before drawing the row rather than get a
    /// missing-texture square.
    gfx::ImageId uiImage;
    /// What the last `Save settings` press did, kept until the next one.
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
    /// Drain the last step's contacts and fire a one-shot at each one worth hearing.
    void playImpacts(Engine& e);

    /// Put one of the two cameras in and take the other out, carrying the pose across.
    /// `Engine::setCamera` copies no pose, so a swap that skips the carry is silent: the
    /// incoming camera starts wherever it was constructed.
    void installCamera(Engine& e, bool fly);

    /// One fixed step of the locomotion machine's parameters, and the trace of what came
    /// back out. Takes the step because a turn rate is degrees per second.
    void driveLocomotion(Engine& e, float step);

    /**
     * @brief The two settings rows this game declares, as the typed handles `declare`
     *        returned.
     *
     * Initialised to `Id::None` so a refused declaration reads and writes nothing;
     * a default-constructed handle would alias `window.width`.
     */
    core::settings::Setting<float> impactVolume{core::settings::Id::None};
    core::settings::Setting<bool> impactDust{core::settings::Id::None};

    /// Where this game's panel sits and how big it is, in unscaled UI units.
    core::settings::Setting<float> panelX{core::settings::Id::None};
    core::settings::Setting<float> panelY{core::settings::Id::None};
    core::settings::Setting<float> panelWidth{core::settings::Id::None};
    core::settings::Setting<float> panelHeight{core::settings::Id::None};

    /**
     * @brief The demo's two cameras, owned here and one of them installed in `init`.
     *
     * The engine holds a non-owning pointer to whichever is active, so `shutdown` must give
     * it back before either dies.
     *
     * `followCamera` takes the player node as its target rather than a position pushed each
     * frame: `Camera::update` runs *before* `frameUpdate`, so a pushed position is always a
     * frame stale.
     */
    scene::FlyCamera flyCamera;
    scene::ThirdPersonCamera followCamera;
    /// Which of the two is installed. The demo drives its character only while this is
    /// false, which is what lets both control schemes ship on W, A, S and D.
    bool flying = false;

    AppActions actions;
    PlayerActions playerActions;
    PanelState panelState;
    ui::InspectorState inspectorState;
    ui::NodeInspectorState nodeInspectorState;
    bool inspectorOpen = false;
    uint32_t saveCount = 0;
    nav::PathFollower navFollower;
    /// Appended models, newest last.
    std::vector<scene::GltfScene::ModelId> loadedModels;
    /// The node carrying one of the scene's lights.
    scene::NodeId torchNode;

    ai::Planner planner;
    ai::Agent fetcher;
    uint32_t propNearTorch = ai::kNoProperty;
    uint32_t propCarrying = ai::kNoProperty;
    uint32_t actWalkToTorch = 0;
    uint32_t actPickUpTorch = 0;
    /// Whether a goal has been handed over at all. Planning from the first frame instead
    /// makes eleven golden cases walk somewhere.
    bool fetching = false;
    /// The action reported last, so the log says what changed rather than what is.
    uint32_t fetchStep = ai::Agent::kNoAction;
    uint32_t torchLight = 0;
    bool carrying = false;
    /// The material every spawned cube shares, created once on the first spawn.
    uint32_t cubeMaterial = 0xFFFFFFFFu;
    std::vector<scene::GltfScene::ModelId> spawnedCubes;
    /// The variant `game/demo/shaders/hologram.frag` is registered as, and the index the
    /// cube material puts in its `shader` field.
    uint32_t hologramVariant = 0;
    /// What a collision sounds like, built once in `init` and varied per impact. Its
    /// `file` is empty in a tree that never ran `fetch_assets.sh`, and callers must treat
    /// that as "stay silent" rather than warn per contact.
    scene::AudioSourceDesc impactSound;
    /// Everything the demo authors in code, and the state the platform needs between steps.
    DemoWorld world;
    LocomotionTrace locomotion;
    /**
     * @brief The nodes the character's meshes hang off, all turned together.
     *
     * Not `playerNode()`. The sweep writes `CharacterVirtual`'s transform straight into that
     * node's world matrix with no decomposition, so a local rotation set on it is overwritten
     * every step and silently does nothing. Its children are ordinary nodes whose TRS is
     * composed onto an already-final parent.
     *
     * A list, because `Engine::bindPhysicsToScene` makes one child named `mesh` per placement
     * bound to the body -- the showcase rig is two -- and turning only the first leaves the
     * rest pointing wherever the file authored them.
     *
     * Re-resolved when it empties: `Scene::setParent` puts the torch at the head of the same
     * child list, and appending a model replaces the whole tree.
     */
    std::vector<scene::NodeId> facingNodes;
    /**
     * @brief The rig node whose translation is root motion, or `kNoNode`.
     *
     * `Hips` on the showcase rig, resolved by name: a joint index is a property of the file
     * and hardcoding one is a number nobody can check. Re-resolved from `kNoNode` after the
     * scene is swapped.
     *
     * Kept even in a build that does not apply the root-motion hold, because
     * `LocomotionTrace::poseDrift` measures this node and a measurement that disappears with
     * the fix cannot fail without it.
     */
    uint32_t rootJoint = scene::SceneAnimator::kNoNode;
};
