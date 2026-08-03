#pragma once

#include "core/Settings.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

namespace scene {

/**
 * @file engine/scene/CameraControllers.h
 * @brief The camera controllers the engine ships: free-fly, first-person, third-person
 *        and isometric.
 *
 * A controller is a `Camera` subclass, so it is a pose and a projection with the three
 * virtuals filled in. Nothing here is installed by default -- `Engine` starts on the base
 * type, which takes no input and declares nothing, and a game that wants fly controls
 * says so with `e.setCamera(&flyCam)`.
 *
 * All four are installable into a `gfx::ViewTable` view as well, which is what a
 * picture-in-picture inset or a second player's half of the screen wants. **The engine
 * drives none of them there**: it calls neither `activate` nor `update` on a view camera,
 * because there is one `InputMap` and the engine has no notion of whose input a second
 * view is showing. A game that wants a view camera to read input calls `update` on it
 * itself, from `Game::frameUpdate`.
 *
 * What is deliberately not here is policy. None of these decides what a character does,
 * which camera is active, or which key switches between them.
 */

/**
 * @brief Orbit-and-fly, on nine actions the game opts into by installing it.
 *
 * Dragging turns the camera where it stands -- the eye is held and the focus swings
 * around it -- scrolling dollies toward the focus, and WASD translates the focus in the
 * view plane, so looking and flying are the same state rather than two modes. Dragging
 * used to swing the eye around the focus instead, which is the same four numbers read the
 * other way round and feels like rotating the scene rather than the camera.
 *
 * Movement reads *actions*, not keys (S1.1). The controller declares the ones it needs
 * with their default bindings and then never mentions a key again, which is what lets a
 * stick drive it and a rebind stick.
 *
 * ```cpp
 * class MyGame : public Game {
 *     scene::FlyCamera flyCam;                    // the game owns it
 *     void init(Engine& e) override {
 *         flyCam.applySettings(e.settingsTable());
 *         e.setCamera(&flyCam);
 *     }
 * };
 * ```
 */
class FlyCamera : public Camera {
  public:
    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take the engine's `camera.*` feel rows -- move speed, orbit sensitivity, zoom step.
    ///
    /// The game does this rather than the engine, because the engine holds a `Camera&` and
    /// these three are not on it. Call it before `Engine::setCamera`, or after any
    /// `settings.set` that moves one of the rows.
    void applySettings(const core::settings::Settings& settings);

    float moveSpeedScale = 1.0f;     ///< multiplies the distance-derived move speed
    float orbitSensitivity = 0.005f; ///< radians per pixel of drag
    float zoomStep = 0.9f;           ///< distance multiplier per scroll notch

  private:
    /// This controller's slice of the action map. Filled by `activate`; every one is
    /// `kInvalidAction` until then, and the map answers "not held" for that, so a
    /// controller nobody activated simply does not move.
    struct Actions {
        core::input::ActionId forward = core::input::kInvalidAction;
        core::input::ActionId back = core::input::kInvalidAction;
        core::input::ActionId left = core::input::kInvalidAction;
        core::input::ActionId right = core::input::kInvalidAction;
        core::input::ActionId up = core::input::kInvalidAction;
        core::input::ActionId down = core::input::kInvalidAction;
        core::input::ActionId fast = core::input::kInvalidAction;
        core::input::ActionId slow = core::input::kInvalidAction;
        core::input::ActionId orbit = core::input::kInvalidAction;
    } actions;
};

/**
 * @brief Look, and deliberately not walk. `distance` is zero, so the eye is the focus.
 *
 * **Continuous mouselook with no held button**: the pointer is grabbed in `activate` and
 * given back in `deactivate`, which is the case a declarative "this action grabs the
 * pointer" flag on the binding could not have expressed, and the reason grabbing is a
 * verb. The engine applies the ask after `update`, so a panel opening still takes the
 * pointer back without this camera having to know.
 *
 * **It declares no movement actions and that is the design.** In a first-person game the
 * *character* moves under the solver and the camera follows it; a camera that also walked
 * would fight the controller and would need collision of its own. The camera that flies is
 * `FlyCamera`.
 *
 * Position comes from `follow()` -- a node in a tree, plus `eyeHeight` -- read during
 * `update`. With no target the game writes `focus` itself and this only turns.
 */
class FirstPersonCamera : public Camera {
  public:
    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take `camera.orbitSensitivity` as the look sensitivity. See `FlyCamera::applySettings`
    /// for why a game does this rather than the engine.
    void applySettings(const core::settings::Settings& settings);

    /**
     * @brief Put the eye on a node, `eyeHeight` above its origin. Both non-owning.
     *
     * The transform is read during `update`, which runs *before* `Game::frameUpdate` -- so a
     * position pushed in by the game would always be a frame stale. A null tree stops the
     * follow and hands `focus` back to the game.
     */
    void follow(const Scene* tree, NodeId node);

    float eyeHeight = 1.7f;          ///< metres above the target node's origin
    float lookSensitivity = 0.005f;  ///< radians per pixel of pointer travel

  private:
    const Scene* tree = nullptr;
    NodeId target;

    /// One action, and it ships **unbound**: an unbound row means look continuously, which
    /// is what the pointer grab is for. Bind something to it and the same row becomes
    /// hold-to-look, which is the only way one action expresses both -- a `bool` on the
    /// class would leave a declared row nothing ever reads. The grab is not conditional on
    /// it either way, because unbounded deltas are what a first-person view turns on.
    struct Actions {
        core::input::ActionId look = core::input::kInvalidAction;
    } actions;
};

/**
 * @brief Follow a node, orbit around it, scroll to pull in and out.
 *
 * Holds a target and **reads its world transform itself, during `update`**. It has to:
 * `update` runs before `Game::frameUpdate` so that a game reading `camera().yaw` to resolve
 * "forward" gets this frame's yaw rather than last frame's, and a position the game pushed
 * in would therefore always be one frame behind. The ordering is not the thing to change.
 *
 * `focus = targetWorld + heightOffset`. **The camera writes `focus` and never `yaw`**, which
 * is the whole answer to the chase problem: a camera that followed a character *and* aimed
 * itself from the character's heading is two integrators feeding each other. Yaw comes from
 * the pointer and from nothing else, so the basis a game's movement resolves against does
 * not depend on the movement.
 *
 * **No spring arm and no collision.** Pulling the camera in when a wall comes between it and
 * the target needs physics queries and a policy for when there is nowhere to go; a camera
 * that clips through geometry is a stated gap here rather than a discovery later.
 */
class ThirdPersonCamera : public Camera {
  public:
    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take `camera.orbitSensitivity` and `camera.zoomStep`. There is no move-speed row to
    /// take: this controller translates nothing, it follows.
    void applySettings(const core::settings::Settings& settings);

    /// Follow `node` in `tree`, both non-owning. A null tree stops the follow and leaves
    /// `focus` to the game.
    void follow(const Scene* tree, NodeId node);

    /// Chest height on a 1.8 m rig, which is what stops a character filling the bottom of
    /// the frame while the camera studies the floor.
    float heightOffset = 1.2f;
    float orbitSensitivity = 0.005f; ///< radians per pixel of drag
    float zoomStep = 0.9f;           ///< distance multiplier per scroll notch
    /// **Only the scroll is clamped to these, not the pose.** `Camera::frameBounds` and
    /// `--camera` both write `distance` directly and neither is this controller's business
    /// to overrule; clamping every frame would move a camera nobody had touched.
    float minDistance = 1.5f;
    float maxDistance = 12.0f;
    /// Grabbed mouselook with no held button, as `FirstPersonCamera` does it. Off by
    /// default: drag-to-orbit is what a third-person camera over a cursor-driven UI wants,
    /// and a grab that started at `activate` would take the pointer off every panel.
    bool continuousLook = false;

  private:
    const Scene* tree = nullptr;
    NodeId target;

    struct Actions {
        core::input::ActionId orbit = core::input::kInvalidAction;
    } actions;
};

/**
 * @brief Orthographic, fixed pitch, quarter-turn yaw, and a focus that pans.
 *
 * The first controller to drive `Projection::Orthographic`, which the engine has carried
 * since P3 with nothing but a lambda setting it. **Scroll moves `orthoHeight` rather than
 * `distance`** -- that is the difference the projection makes and the thing a controller
 * written for perspective gets wrong: a parallel projection does not care how far away the
 * eye is, so dollying it changes nothing on screen.
 *
 * Pitch is fixed at the true isometric angle, where a unit cube's three visible faces
 * project to equal areas. Yaw snaps in quarter turns, rounded to a turn count at the moment
 * of the press rather than accumulated -- so eight presses land exactly back where they
 * started, and `frameBounds` and `--camera` still choose where this camera begins.
 *
 * It does **not** replace `pixelPerfectCamera`. That is a 2D pixel-exact setup -- one world
 * unit per texel, origin centred, yaw pi -- and conflating the two would produce a camera
 * bad at both.
 */
class IsometricCamera : public Camera {
  public:
    IsometricCamera();

    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take `camera.moveSpeedScale` as the pan scale and `camera.zoomStep` as the zoom.
    void applySettings(const core::settings::Settings& settings);

    /// 35.264 degrees below the horizon, negated because `pitch` is signed and this camera
    /// looks down. `atan(1/sqrt(2))`: the angle at which a unit cube's three visible faces
    /// have equal area, which is what "isometric" means before it means a look.
    float fixedPitch = -0.6154797f;
    float panSpeedScale = 1.0f; ///< multiplies the orthoHeight-derived pan speed
    float zoomStep = 0.9f;      ///< orthoHeight multiplier per scroll notch
    /// As on `ThirdPersonCamera`, these bound the *scroll* and not the pose `frameBounds`
    /// worked out from the scene.
    float minOrthoHeight = 1.0f;
    float maxOrthoHeight = 200.0f;
    /// World units per pixel of drag, per unit of `orthoHeight`. A drag should move the
    /// ground under the pointer, and the exact ratio wants the viewport height, which a
    /// camera does not have -- so this is the reciprocal of a nominal 500-pixel half-height
    /// and is a feel row rather than a derivation.
    float dragScale = 0.002f;

  private:
    struct Actions {
        core::input::ActionId forward = core::input::kInvalidAction;
        core::input::ActionId back = core::input::kInvalidAction;
        core::input::ActionId left = core::input::kInvalidAction;
        core::input::ActionId right = core::input::kInvalidAction;
        core::input::ActionId rotateLeft = core::input::kInvalidAction;
        core::input::ActionId rotateRight = core::input::kInvalidAction;
        core::input::ActionId pan = core::input::kInvalidAction;
    } actions;
};

} // namespace scene
