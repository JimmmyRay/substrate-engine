#include "scene/CameraControllers.h"

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

constexpr float kPitchLimit = 1.55334f; // just under pi/2, so up never degenerates

/// The slowest this controller ever flies, in units/second. The speed below is a fraction
/// of the distance to the focus, and a camera pressed right up against something would
/// otherwise stop being able to back away from it.
constexpr float kMinSpeed = 0.25f;

/// A quarter turn in radians, which is the only yaw `IsometricCamera` ever holds.
constexpr float kQuarterTurn = 1.57079633f;

} // namespace

void FlyCamera::activate(core::input::InputMap& map) {
    // The pad half is bound alongside the keyboard half rather than added later: an
    // action with two sources is what stops the gamepad becoming a second input path,
    // and writing it down here is the cheapest moment to prove that works.
    actions.forward = map.declare("Camera.Forward", "W Pad.LeftY-");
    actions.back = map.declare("Camera.Back", "S Pad.LeftY+");
    actions.left = map.declare("Camera.Left", "A Pad.LeftX-");
    actions.right = map.declare("Camera.Right", "D Pad.LeftX+");
    // E and Q, and Space belongs to neither. `update` reads `up` unconditionally while a
    // game reads its jump only when a player character exists -- so Space on both meant
    // one press flew the camera *and* jumped, in a scene whose own HUD said "space=jump".
    // `InputMap::conflicts` is what said so; the asymmetry with `down`, which never had a
    // second key, is what it had looked like beforehand.
    actions.up = map.declare("Camera.Up", "E Pad.RightTrigger+");
    actions.down = map.declare("Camera.Down", "Q Pad.LeftTrigger+");
    actions.fast = map.declare("Camera.Fast", "LeftShift RightShift Pad.RightBumper");
    actions.slow = map.declare("Camera.Slow", "LeftControl RightControl Pad.LeftBumper");
    actions.orbit = map.declare("Camera.Orbit", "Mouse.Middle");
}

void FlyCamera::deactivate(core::input::InputMap& map) {
    // Retired, not cleared: a cleared row still shows in the binding menu and can still be
    // rebound, for a controller that is no longer running. `declare` is idempotent by name
    // and revives a retired row with whatever the player had put on it, so re-installing
    // this camera costs them nothing.
    for (const core::input::ActionId id : {actions.forward, actions.back, actions.left, actions.right, actions.up,
                                           actions.down, actions.fast, actions.slow, actions.orbit}) {
        map.retire(id);
    }
    actions = Actions{};
    // The drag cannot outlive the controller that started it. Without this a camera
    // swapped out mid-drag leaves the pointer captured with nothing reading it.
    core::input::mouseRelease();
}

void FlyCamera::update(const core::input::InputMap& in, float dt) {
    // ------------------------------------------------------------------- the pointer
    // **The ask, not the platform call.** `Engine` applies the cursor mode from
    // `mouseGrabbed()` *after* this runs, so a camera that takes no input grabs nothing --
    // which is the whole reason the engine no longer asks a camera for its orbit action.
    if (in.held(actions.orbit)) {
        core::input::mouseGrab();
    } else {
        core::input::mouseRelease();
    }

    // ------------------------------------------------------------------- look
    // Skipped on the frame the button went down: the pointer's travel while the button
    // was up is a position change, not a drag, and applying it snaps the view.
    if (in.held(actions.orbit) && !in.pressed(actions.orbit)) {
        // The eye is held still and the focus swings around it -- a camera turning on
        // the spot, not one flying around the focus. The state is still a focus, a
        // distance and two angles, so this is where the two readings of that state part
        // company: orbiting it rotates the scene in front of you, and everything nearer
        // than the focus then parallaxes the wrong way, which is what made both axes
        // feel inverted rather than only the one that was.
        const glm::vec3 eye = position();

        // Mouse right turns right, mouse down looks down. Screen-right is
        // cross(forward, up), which is where the negation on yaw comes from; pitch is
        // negated because the cursor's Y grows downward and looking up does not.
        yaw -= static_cast<float>(in.cursorDeltaX()) * orbitSensitivity;
        pitch = std::clamp(pitch - static_cast<float>(in.cursorDeltaY()) * orbitSensitivity, -kPitchLimit, kPitchLimit);

        focus = eye + forward() * distance;
    }

    // ------------------------------------------------------------------- zoom
    // Multiplicative, so zoom feels the same at every scale and never crosses zero.
    if (const double scroll = in.scrollDelta(); scroll != 0.0) {
        distance *= std::pow(zoomStep, static_cast<float>(scroll));
        distance = std::max(distance, nearPlane * 2.0f);
    }

    // ------------------------------------------------------------------- move
    // **Speed is the distance to the focus, not a field the scene sized.** `frameBounds`
    // is the base's and knows how big the world is; this controller is not, and the
    // distance it is already holding is the one number that says how big the thing being
    // looked at is -- so crossing that gap takes about a second at full deflection, at any
    // scale and after any dolly. `camera.moveSpeedScale` is the ratio a user has an
    // opinion about, over the derived speed rather than as a second spelling of it.
    float speed = std::max(distance, kMinSpeed) * moveSpeedScale;
    if (in.held(actions.fast)) speed *= 4.0f;
    if (in.held(actions.slow)) speed *= 0.25f;

    const glm::vec3 fwd = forward();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    // Values rather than held flags, so a stick at a third of its travel moves at a
    // third of the speed. A key reads back as exactly 1, which is what makes the
    // keyboard path identical to what it was.
    glm::vec3 delta(0.0f);
    delta += fwd * (in.value(actions.forward) - in.value(actions.back));
    delta += right * (in.value(actions.right) - in.value(actions.left));
    delta += up * (in.value(actions.up) - in.value(actions.down));

    const float magnitude = glm::length(delta);
    if (magnitude > 0.0f) {
        // Clamped rather than normalised: normalising would push a stick held gently
        // to full speed, and leaving it alone would make a keyboard diagonal 1.41x
        // faster than a straight line.
        focus += delta / std::max(magnitude, 1.0f) * speed * dt;
    }
}

void FlyCamera::applySettings(const core::settings::Settings& settings) {
    moveSpeedScale = settings.get(core::options::camera::moveSpeedScale);
    orbitSensitivity = settings.get(core::options::camera::orbitSensitivity);
    zoomStep = settings.get(core::options::camera::zoomStep);
}

// ================================================================== FirstPersonCamera

void FirstPersonCamera::activate(core::input::InputMap& map) {
    // Unbound on purpose -- see the row on `Actions::look`. `declare` is idempotent by
    // name, so a player who put a button on it keeps it across a re-install.
    actions.look = map.declare("Camera.Look");
    // **The whole of the grab, and it is not conditional on anything.** A first-person view
    // has nowhere for a cursor to be, and the deltas it turns on have to be able to run
    // further than the screen is wide. `Engine` applies the ask after `update`, and the UI
    // hands the pointer back on its own while a panel is open.
    core::input::mouseGrab();
}

void FirstPersonCamera::deactivate(core::input::InputMap& map) {
    map.retire(actions.look);
    actions = Actions{};
    core::input::mouseRelease();
}

void FirstPersonCamera::update(const core::input::InputMap& in, float /*dt*/) {
    // Not gated on an action being *held* -- there is none by default -- so it is gated on
    // this controller having been activated at all. Without that, a camera a game holds but
    // never installed would still turn with the pointer.
    if (actions.look == core::input::kInvalidAction) return;

    // The eye is the focus. Written every frame rather than in the constructor because
    // `Camera::frameBounds` derives a distance from the scene and would otherwise back the
    // eye out of the head this camera is meant to be inside.
    distance = 0.0f;

    // An unbound row looks continuously; a bound one is hold-to-look. The frame the button
    // went down is skipped for the reason `FlyCamera` skips it: the pointer's travel while
    // it was up is a position change, not a turn, and applying it snaps the view.
    const bool gated = !in.bindings(actions.look).empty();
    if ((!gated || in.held(actions.look)) && !in.pressed(actions.look)) {
        // Mouse right turns right, mouse down looks down -- the same two negations
        // `FlyCamera::update` derives, and for the same two reasons.
        yaw -= static_cast<float>(in.cursorDeltaX()) * lookSensitivity;
        pitch =
            std::clamp(pitch - static_cast<float>(in.cursorDeltaY()) * lookSensitivity, -kPitchLimit, kPitchLimit);
    }

    if (tree != nullptr && tree->valid(target)) {
        focus = glm::vec3(tree->worldTransform(target)[3]) + glm::vec3(0.0f, eyeHeight, 0.0f);
    }
}

void FirstPersonCamera::applySettings(const core::settings::Settings& settings) {
    lookSensitivity = settings.get(core::options::camera::orbitSensitivity);
}

void FirstPersonCamera::follow(const Scene* tree, NodeId node) {
    // `this->` because the parameter carries the name the header documents, and the
    // obvious alternative -- calling it `scene` -- shadows the namespace this lives in.
    this->tree = tree;
    target = node;
}

// ================================================================== ThirdPersonCamera

void ThirdPersonCamera::activate(core::input::InputMap& map) {
    // The same name and the same default as `FlyCamera`'s, because it is the same verb.
    // One row means a player who moved it keeps it across a switch between the two, which
    // is what `declare` being idempotent by name is for.
    //
    // **Middle, and not left or right.** Both of those are a game's to spend: a pointer over
    // a 3D world is how a game selects, orders and attacks, and a camera that took the two
    // obvious buttons would leave nothing for it. Left is also `Ui.Click`, and while
    // `setPointerMode` makes that overlap correct rather than broken, correct-by-suppression
    // is a worse answer than not colliding. `IsometricCamera`'s `Camera.Pan` still lists
    // Middle; the two are never live together, because two camera controllers never are.
    actions.orbit = map.declare("Camera.Orbit", "Mouse.Middle");
    if (continuousLook) core::input::mouseGrab();
}

void ThirdPersonCamera::deactivate(core::input::InputMap& map) {
    map.retire(actions.orbit);
    actions = Actions{};
    // The drag cannot outlive the controller that started it, and neither can a
    // continuous-look grab.
    core::input::mouseRelease();
}

void ThirdPersonCamera::update(const core::input::InputMap& in, float /*dt*/) {
    // ------------------------------------------------------------------- the pointer
    const bool looking = continuousLook || in.held(actions.orbit);
    if (looking) {
        core::input::mouseGrab();
    } else {
        core::input::mouseRelease();
    }

    // ------------------------------------------------------------------- orbit
    // **The focus is held and the eye swings around it**, which is the opposite of
    // `FlyCamera` and is what a camera pointed at a character has to do: the thing being
    // looked at is the thing that must not move. Nothing has to be written for that --
    // `position()` already derives the eye from the focus and the two angles -- so the
    // whole difference between the two controllers is that this one does not put `focus`
    // back afterwards.
    if (looking && !in.pressed(actions.orbit)) {
        yaw -= static_cast<float>(in.cursorDeltaX()) * orbitSensitivity;
        pitch =
            std::clamp(pitch - static_cast<float>(in.cursorDeltaY()) * orbitSensitivity, -kPitchLimit, kPitchLimit);
    }

    // ------------------------------------------------------------------- zoom
    // Multiplicative, so a notch feels the same at every distance, and clamped only here
    // -- see `minDistance`.
    if (const double scroll = in.scrollDelta(); scroll != 0.0) {
        distance = std::clamp(distance * std::pow(zoomStep, static_cast<float>(scroll)), minDistance, maxDistance);
    }

    // ------------------------------------------------------------------- follow
    // **Read here rather than pushed in by the game**, because this runs before
    // `Game::frameUpdate` and a pushed position would be a frame stale. `focus` is written
    // and `yaw` never is; see the class comment for why that direction is the whole design.
    if (tree != nullptr && tree->valid(target)) {
        focus = glm::vec3(tree->worldTransform(target)[3]) + glm::vec3(0.0f, heightOffset, 0.0f);
    }
}

void ThirdPersonCamera::applySettings(const core::settings::Settings& settings) {
    orbitSensitivity = settings.get(core::options::camera::orbitSensitivity);
    zoomStep = settings.get(core::options::camera::zoomStep);
}

void ThirdPersonCamera::follow(const Scene* tree, NodeId node) {
    this->tree = tree;
    target = node;
}

// ==================================================================== IsometricCamera

IsometricCamera::IsometricCamera() {
    // On the instance rather than asserted in `update`, because the projection mode is the
    // camera's own state: installing another controller installs its mode with it and there
    // is nothing to restore.
    projectionMode = Projection::Orthographic;
}

void IsometricCamera::activate(core::input::InputMap& map) {
    // The pan shares `FlyCamera`'s four names and their defaults -- it is the same verb on
    // the same keys, moving the focus in a plane rather than in the view.
    actions.forward = map.declare("Camera.Forward", "W Pad.LeftY-");
    actions.back = map.declare("Camera.Back", "S Pad.LeftY+");
    actions.left = map.declare("Camera.Left", "A Pad.LeftX-");
    actions.right = map.declare("Camera.Right", "D Pad.LeftX+");
    // Q and E, which `FlyCamera` spends on up and down. Two controllers are never live at
    // once, so the keys are free to mean whichever one is running.
    actions.rotateLeft = map.declare("Camera.RotateLeft", "Q Pad.LeftBumper");
    actions.rotateRight = map.declare("Camera.RotateRight", "E Pad.RightBumper");
    // Its own name rather than `Camera.Orbit`: this drag translates and that one turns, and
    // a player who rebound one has said nothing about the other.
    actions.pan = map.declare("Camera.Pan", "Mouse.Middle Mouse.Right");
}

void IsometricCamera::deactivate(core::input::InputMap& map) {
    for (const core::input::ActionId id : {actions.forward, actions.back, actions.left, actions.right,
                                           actions.rotateLeft, actions.rotateRight, actions.pan}) {
        map.retire(id);
    }
    actions = Actions{};
    core::input::mouseRelease();
}

void IsometricCamera::update(const core::input::InputMap& in, float dt) {
    // ------------------------------------------------------------------- the pointer
    if (in.held(actions.pan)) {
        core::input::mouseGrab();
    } else {
        core::input::mouseRelease();
    }

    // Held rather than clamped: this camera has one pitch and the rest of the class is
    // written against it. A game wanting a different one moves `fixedPitch`.
    pitch = fixedPitch;

    // ------------------------------------------------------------------- rotate
    // Rounded off `yaw` at the moment of the press rather than accumulated, so whatever
    // framing or `--camera` chose is where the first press counts from, and normalised
    // through the integer rather than through the angle -- `remainder` on a float pi/2
    // leaves a residue that eight presses turn into a visible tilt.
    if (const int step = (in.pressed(actions.rotateRight) ? 1 : 0) - (in.pressed(actions.rotateLeft) ? 1 : 0);
        step != 0) {
        const int turns = static_cast<int>(std::lround(yaw / kQuarterTurn)) + step;
        yaw = static_cast<float>(((turns % 4) + 4) % 4) * kQuarterTurn;
    }

    // ------------------------------------------------------------------- zoom
    // **`orthoHeight`, not `distance`, and that is the whole of what an orthographic
    // controller is.** A parallel projection ignores how far away the eye is, so a
    // controller written for perspective dollies here and nothing on screen changes size.
    if (const double scroll = in.scrollDelta(); scroll != 0.0) {
        orthoHeight = std::clamp(orthoHeight * std::pow(zoomStep, static_cast<float>(scroll)), minOrthoHeight,
                                 maxOrthoHeight);
    }

    // ------------------------------------------------------------------- pan
    // The ground-plane basis, taken off `yaw` alone. Not `forward()`: this camera is pitched
    // 35 degrees down, so its forward has a large Y and normalising the flattened version of
    // it is one more step for the same two numbers.
    const glm::vec3 ahead(std::sin(yaw), 0.0f, std::cos(yaw));
    const glm::vec3 side = glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f));

    // Speed from the box the camera is showing rather than from a field a scene sized, for
    // the reason `FlyCamera` derives its speed from `distance`: `orthoHeight` is the one
    // number here that says how big the thing being looked at is, so crossing a screen
    // takes about a second at any zoom.
    const float speed = std::max(orthoHeight, 1.0f) * panSpeedScale;
    glm::vec3 delta = ahead * (in.value(actions.forward) - in.value(actions.back)) +
                      side * (in.value(actions.right) - in.value(actions.left));
    if (const float magnitude = glm::length(delta); magnitude > 0.0f) {
        // Clamped rather than normalised, so a stick at a third of its travel pans at a
        // third of the speed and a keyboard diagonal is not 1.41x faster than a straight
        // line.
        focus += delta / std::max(magnitude, 1.0f) * speed * dt;
    }

    // The drag moves the ground under the pointer, so the focus goes the other way on the
    // screen's right axis and the same way on its up axis -- the cursor's Y grows downward,
    // which is what leaves `ahead` un-negated here and negated in a look.
    if (in.held(actions.pan) && !in.pressed(actions.pan)) {
        const float scale = orthoHeight * dragScale;
        focus -= side * static_cast<float>(in.cursorDeltaX()) * scale;
        focus += ahead * static_cast<float>(in.cursorDeltaY()) * scale;
    }
}

void IsometricCamera::applySettings(const core::settings::Settings& settings) {
    panSpeedScale = settings.get(core::options::camera::moveSpeedScale);
    zoomStep = settings.get(core::options::camera::zoomStep);
}

} // namespace scene
