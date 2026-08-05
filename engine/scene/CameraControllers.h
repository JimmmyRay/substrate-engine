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
 * virtuals filled in. Nothing here is installed by default; a game says
 * `e.setCamera(&flyCam)`.
 *
 * All four are installable into a `gfx::ViewTable` view as well, but **the engine drives
 * none of them there**: it calls neither `activate` nor `update` on a view camera, because
 * there is one `InputMap` and the engine has no notion of whose input a second view is
 * showing. A game that wants a view camera to read input calls `update` on it itself, from
 * `Game::frameUpdate`.
 */

/**
 * @brief Orbit-and-fly, on nine actions the game opts into by installing it.
 *
 * Dragging turns the camera where it stands -- the eye is held and the focus swings around
 * it -- scrolling dollies toward the focus, and WASD translates the focus in the view
 * plane. Swinging the eye around the focus instead is the same four numbers read the other
 * way round, and feels like rotating the scene rather than the camera.
 */
class FlyCamera : public Camera {
  public:
    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take the engine's `camera.*` feel rows -- move speed, orbit sensitivity, zoom step.
    /// The game calls it, because the engine holds a `Camera&` and these three are not on
    /// it. Call it before `Engine::setCamera`, and after any `settings.set` that moves a row.
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
 * given back in `deactivate`. The engine applies the ask after `update`, so a panel opening
 * still takes the pointer back without this camera having to know.
 *
 * It declares no movement actions. In a first-person game the *character* moves under the
 * solver and the camera follows it; a camera that also walked would fight the controller
 * and would need collision of its own.
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

    /// One action, and it ships **unbound**: an unbound row means look continuously, and
    /// binding something to it makes the same row hold-to-look. The grab is not conditional
    /// on it either way, because unbounded deltas are what a first-person view turns on.
    struct Actions {
        core::input::ActionId look = core::input::kInvalidAction;
    } actions;
};

/**
 * @brief Follow a node, orbit around it, scroll to pull in and out.
 *
 * Holds a target and **reads its world transform itself, during `update`**. It has to:
 * `update` runs before `Game::frameUpdate` so a game reading `camera().yaw` to resolve
 * "forward" gets this frame's yaw, which means a position the game pushed in would always
 * be one frame behind.
 *
 * `focus = targetWorld + heightOffset`. **The camera writes `focus` and never `yaw`**: a
 * camera that followed a character *and* aimed itself from the character's heading is two
 * integrators feeding each other, and the basis a game's movement resolves against would
 * then depend on the movement.
 *
 * No spring arm and no collision, so it clips through geometry between it and the target.
 */
class ThirdPersonCamera : public Camera {
  public:
    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take `camera.orbitSensitivity` and `camera.zoomStep`.
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
    /// `--camera` both write `distance` directly, and clamping every frame would move a
    /// camera nobody had touched.
    float minDistance = 1.5f;
    float maxDistance = 12.0f;
    /// Grabbed mouselook with no held button, as `FirstPersonCamera` does it. Off by
    /// default, because a grab that started at `activate` takes the pointer off every panel.
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
 * **Scroll moves `orthoHeight` rather than `distance`**, which is what a controller written
 * for perspective gets wrong here: a parallel projection does not care how far away the eye
 * is, so dollying it changes nothing on screen.
 *
 * Pitch is fixed at the true isometric angle, where a unit cube's three visible faces
 * project to equal areas. Yaw snaps in quarter turns, rounded to a turn count at the moment
 * of the press rather than accumulated, so eight presses land exactly back where they
 * started and `frameBounds` and `--camera` still choose where this camera begins.
 */
class IsometricCamera : public Camera {
  public:
    IsometricCamera();

    void activate(core::input::InputMap& map) override;
    void deactivate(core::input::InputMap& map) override;
    void update(const core::input::InputMap& in, float dt) override;

    /// Take `camera.moveSpeedScale` as the pan scale and `camera.zoomStep` as the zoom.
    void applySettings(const core::settings::Settings& settings);

    /// `-atan(1/sqrt(2))`, 35.264 degrees below the horizon: the angle at which a unit
    /// cube's three visible faces have equal area. Negative because `pitch` is signed and
    /// this camera looks down.
    float fixedPitch = -0.6154797f;
    float panSpeedScale = 1.0f; ///< multiplies the orthoHeight-derived pan speed
    float zoomStep = 0.9f;      ///< orthoHeight multiplier per scroll notch
    /// As on `ThirdPersonCamera`, these bound the *scroll* and not the pose `frameBounds`
    /// worked out from the scene.
    float minOrthoHeight = 1.0f;
    float maxOrthoHeight = 200.0f;
    /// World units per pixel of drag, per unit of `orthoHeight`. Moving the ground exactly
    /// under the pointer wants the viewport height, which a camera does not have, so this is
    /// the reciprocal of a nominal 500-pixel half-height and is a feel row, not a derivation.
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
