#include "scene/CameraControllers.h"

#include "core/Input.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace core;

using namespace scene;

using namespace input;

/**
 * @file tests/CameraTests.cpp
 * @brief The camera's half of S1.1 -- movement resolved from actions, not keys.
 *
 * The camera became testable *because* of this item. It used to call `glfwGetKey`
 * against a window handle, so exercising it meant opening a window; it now reads an
 * InputMap, which is a struct anyone can fill. That is the reason `Camera.cpp` moved
 * into `SUBSTRATE_HOSTED_SOURCES`, and these are the assertions that were impossible
 * before.
 */

namespace {

/// A flycam framed on a known box, with its actions declared into `map`.
FlyCamera framed(InputMap& map) {
    FlyCamera camera;
    camera.activate(map);
    camera.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));
    return camera;
}

/// One frame with `key` held for the whole of it.
void holdFrame(InputMap& map, Camera& camera, Key key, float dt) {
    map.onKey(key, true);
    map.beginFrame();
    camera.update(map, dt);
    map.onKey(key, false);
}

/// A camera framed on the same box, in the given projection.
Camera framedAs(Camera::Projection mode) {
    Camera camera;
    camera.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));
    camera.projectionMode = mode;
    return camera;
}

/// The aspect ratio these tests project through. Any value does: nothing asserted below
/// is about x, and the two rows `depthLinear()` reads do not contain it.
constexpr float kAspect = 1.6f;

/// The reverse-Z depth a point `distance` in front of the camera lands on, taken from the
/// matrix itself rather than from a formula that could agree with the wrong one.
float depthAt(const Camera& camera, float distance) {
    const glm::vec4 clip = camera.projection(kAspect) * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
    return clip.z / clip.w;
}

/// **The expression `viewDistance()` in `engine/shaders/frame.glsl` evaluates**, term for
/// term and in the same order, so what these tests pin is that function and not a
/// paraphrase of it. The shader has no unit suite of its own; this is the closest thing
/// available, and it is only worth anything while the two stay written the same way.
float viewDistance(const glm::vec4& c, float depth) {
    return (depth * c.x - c.y) / (depth * c.z - c.w);
}

} // namespace

TEST(CameraTest, DeclaringActionsNamesThemRatherThanTheKeysBehindThem) {
    InputMap map;
    const FlyCamera camera = framed(map);
    EXPECT_NEAR(glm::length(camera.forward()), 1.0f, 1e-5f);

    ASSERT_NE(map.find("Camera.Forward"), kInvalidAction);
    EXPECT_EQ(map.bindingList(map.find("Camera.Forward")), "W Pad.LeftY-");
    EXPECT_EQ(map.bindingList(map.find("Camera.Orbit")), "Mouse.Middle");
    EXPECT_EQ(map.actionCount(), 9u) << "nine actions, and not one key mentioned anywhere else";
}

TEST(CameraTest, ForwardMovesAlongTheViewDirection) {
    InputMap map;
    FlyCamera camera = framed(map);

    const glm::vec3 before = camera.position();
    const glm::vec3 forward = camera.forward();
    holdFrame(map, camera, Key::W, 0.5f);

    const glm::vec3 moved = camera.position() - before;
    ASSERT_GT(glm::length(moved), 0.0f);
    EXPECT_GT(glm::dot(glm::normalize(moved), forward), 0.999f);
}

TEST(CameraTest, ARebindMovesTheCameraWithoutTheCameraKnowing) {
    InputMap map;
    FlyCamera camera = framed(map);
    map.setBindings(map.find("Camera.Forward"), "K");

    const glm::vec3 before = camera.position();
    holdFrame(map, camera, Key::W, 0.5f);
    EXPECT_EQ(camera.position(), before) << "W is not a camera key any more";

    holdFrame(map, camera, Key::K, 0.5f);
    EXPECT_NE(camera.position(), before);
}

TEST(CameraTest, AStickHeldHalfwayMovesAtHalfSpeed) {
    InputMap pad;
    pad.gamepadDeadzone = 0.0f;
    FlyCamera stickCamera = framed(pad);
    const glm::vec3 stickStart = stickCamera.position();

    GamepadState state;
    state.connected = true;
    state.axes[static_cast<int>(PadAxis::LeftY)] = -0.5f;
    pad.setGamepad(state);
    pad.beginFrame();
    stickCamera.update(pad, 1.0f);
    const float halfway = glm::length(stickCamera.position() - stickStart);

    InputMap keyboard;
    FlyCamera keyCamera = framed(keyboard);
    const glm::vec3 keyStart = keyCamera.position();
    keyboard.onKey(Key::W, true);
    keyboard.beginFrame();
    keyCamera.update(keyboard, 1.0f);
    const float held = glm::length(keyCamera.position() - keyStart);

    ASSERT_GT(held, 0.0f);
    EXPECT_NEAR(halfway, held * 0.5f, 1e-4f) << "a key reads back as exactly 1, a stick as what it is";
}

TEST(CameraTest, ADiagonalIsNotFasterThanAStraightLine) {
    InputMap map;
    FlyCamera camera = framed(map);

    const glm::vec3 start = camera.position();
    map.onKey(Key::W, true);
    map.beginFrame();
    camera.update(map, 1.0f);
    const float straight = glm::length(camera.position() - start);
    map.onKey(Key::W, false);

    InputMap two;
    FlyCamera diagonal = framed(two);
    const glm::vec3 diagonalStart = diagonal.position();
    two.onKey(Key::W, true);
    two.onKey(Key::D, true);
    two.beginFrame();
    diagonal.update(two, 1.0f);
    const float across = glm::length(diagonal.position() - diagonalStart);

    EXPECT_NEAR(across, straight, 1e-4f) << "unclamped, two keys would be 1.41x one";
}

TEST(CameraTest, TheFrameADragStartsOnDoesNotApplyTheDelta) {
    InputMap map;
    FlyCamera camera = framed(map);

    // The pointer moved a long way with the button up; that travel is a position
    // change, not a drag, and applying it snaps the view a quarter turn.
    map.onCursorPos(100.0, 100.0);
    map.beginFrame();
    map.onCursorPos(700.0, 400.0);
    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();

    const glm::vec3 before = camera.forward();
    camera.update(map, 0.016f);
    EXPECT_EQ(camera.forward(), before);

    map.onCursorPos(720.0, 400.0);
    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_NE(camera.forward(), before) << "the second frame of the drag is a drag";
}

TEST(CameraTest, PitchStopsShortOfThePoles) {
    InputMap map;
    FlyCamera camera = framed(map);

    map.onCursorPos(0.0, 0.0);
    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();
    camera.update(map, 0.016f);

    for (int i = 0; i < 50; ++i) {
        map.onCursorPos(0.0, static_cast<double>(i + 1) * 100.0);
        map.beginFrame();
        camera.update(map, 0.016f);
    }

    // Straight up would make the view matrix's up vector degenerate, and lookAt
    // produces NaNs rather than a clamped camera.
    const glm::vec3 forward = camera.forward();
    EXPECT_LT(forward.y, 1.0f);
    EXPECT_FALSE(std::isnan(forward.x));
    const glm::mat4 view = camera.view();
    EXPECT_EQ(view[3][3], view[3][3]) << "a NaN is the only value that is not equal to itself";
}

TEST(CameraTest, ScrollDolliesAndNeverReachesZero) {
    InputMap map;
    FlyCamera camera = framed(map);

    const float startDistance = glm::length(camera.position() - glm::vec3(0.0f, 1.0f, 0.0f));
    for (int i = 0; i < 200; ++i) {
        map.onScroll(1.0);
        map.beginFrame();
        camera.update(map, 0.016f);
    }

    const float endDistance = glm::length(camera.position() - glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_LT(endDistance, startDistance);
    EXPECT_GT(endDistance, 0.0f) << "multiplicative zoom must never cross the focus point";
}

TEST(CameraTest, ACameraWhoseActionsWereNeverDeclaredSimplyDoesNotMove) {
    InputMap map;
    FlyCamera camera; // never activated
    camera.frameBounds(glm::vec3(-1.0f), glm::vec3(1.0f));

    const glm::vec3 before = camera.position();
    map.onKey(Key::W, true);
    map.beginFrame();
    camera.update(map, 1.0f);
    EXPECT_EQ(camera.position(), before) << "kInvalidAction answers \"not held\", which is the safe answer";
}

TEST(CameraTest, TheBaseTypeIsAlsoTheNullCameraAndTakesNoInput) {
    // The engine holds one of these when a game installs nothing, so what it does with a
    // full keyboard is the whole of what "no camera" costs a game that has its own.
    InputMap map;
    FlyCamera declaring;
    declaring.activate(map); // the nine rows exist and are bound

    Camera base;
    base.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));
    const glm::vec3 before = base.position();
    const float yawBefore = base.yaw;

    map.onKey(Key::W, true);
    map.onCursorPos(0.0, 0.0);
    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();
    map.onCursorPos(300.0, 120.0);
    map.beginFrame();
    base.update(map, 1.0f);

    EXPECT_EQ(base.position(), before);
    EXPECT_EQ(base.yaw, yawBefore);
}

TEST(CameraTest, ADeactivatedControllerRetiresItsRowsRatherThanClearingThem) {
    InputMap map;
    FlyCamera camera;
    camera.activate(map);
    const ActionId forward = map.find("Camera.Forward");
    ASSERT_NE(forward, kInvalidAction);

    camera.deactivate(map);
    // Gone from the live map, so a binding menu walking it lists no `Camera.*` row and a
    // frame cannot resolve one...
    EXPECT_EQ(map.find("Camera.Forward"), kInvalidAction);
    EXPECT_FALSE(map.actionLive(forward));
    // ...and still declared, so a player's rebind survives the camera going away. Cleared
    // bindings would have lost it and left the row visible, which is both halves wrong.
    EXPECT_EQ(map.findDeclared("Camera.Forward"), forward);
    EXPECT_EQ(map.bindingList(forward), "W Pad.LeftY-");

    // And the controller stops moving with them, because its ids went back to invalid.
    camera.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));
    const glm::vec3 before = camera.position();
    holdFrame(map, camera, Key::W, 1.0f);
    EXPECT_EQ(camera.position(), before);
}

TEST(CameraTest, TheControllerAsksForThePointerAndTheBaseNeverDoes) {
    mouseGrabReset();
    InputMap map;
    FlyCamera camera = framed(map);

    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_TRUE(mouseGrabbed()) << "the drag is the camera's ask, not the engine's";

    // Handing the camera back mid-drag gives the pointer back with it: `Engine` applies
    // the platform's cursor mode from this, and nothing else would ever clear it.
    camera.deactivate(map);
    EXPECT_FALSE(mouseGrabbed());
    mouseGrabReset();
}

// ---------------------------------------------------------------- projections

TEST(CameraTest, BothProjectionsPutTheNearPlaneAtDepthOne) {
    const Camera perspective = framedAs(Camera::Projection::Perspective);
    const Camera ortho = framedAs(Camera::Projection::Orthographic);

    EXPECT_NEAR(depthAt(perspective, perspective.nearPlane), 1.0f, 1e-5f);
    EXPECT_NEAR(depthAt(ortho, ortho.nearPlane), 1.0f, 1e-5f);

    // And the far end at 0. The perspective one has no far plane to name, so it is
    // approached rather than reached -- which is the whole reason it is called infinite.
    EXPECT_NEAR(depthAt(ortho, ortho.orthoFar), 0.0f, 1e-5f);
    EXPECT_LT(depthAt(perspective, 1.0e6f), 1e-5f);

    // A forward-Z ortho -- glm::ortho, the obvious call -- would answer these the other
    // way round, and every `depth > FAR_DEPTH` test in the shaders reads that as sky.
    EXPECT_GT(depthAt(ortho, ortho.nearPlane), depthAt(ortho, ortho.orthoFar));
}

TEST(CameraTest, TheOrthographicProjectionIsParallelAndTheOtherIsNot) {
    const Camera ortho = framedAs(Camera::Projection::Orthographic);
    const glm::mat4 p = ortho.projection(kAspect);

    // The same point, twice as far away. Under a parallel projection it lands on the
    // same pixel; that is what "no eye point" means, and it is why the skybox ray cannot
    // be built by subtracting the camera position.
    const glm::vec4 close = p * glm::vec4(1.0f, 1.0f, -5.0f, 1.0f);
    const glm::vec4 distant = p * glm::vec4(1.0f, 1.0f, -10.0f, 1.0f);
    EXPECT_FLOAT_EQ(close.w, 1.0f);
    EXPECT_FLOAT_EQ(distant.w, 1.0f);
    EXPECT_FLOAT_EQ(close.x / close.w, distant.x / distant.w);
    EXPECT_FLOAT_EQ(close.y / close.w, distant.y / distant.w);

    const glm::mat4 q = framedAs(Camera::Projection::Perspective).projection(kAspect);
    const glm::vec4 pNear = q * glm::vec4(1.0f, 1.0f, -5.0f, 1.0f);
    const glm::vec4 pFar = q * glm::vec4(1.0f, 1.0f, -10.0f, 1.0f);
    EXPECT_NE(pNear.x / pNear.w, pFar.x / pFar.w);
}

TEST(CameraTest, TheOrthographicBoxIsOrthoHeightTall) {
    Camera camera = framedAs(Camera::Projection::Orthographic);
    camera.orthoHeight = 8.0f;
    const glm::mat4 p = camera.projection(kAspect);

    // Half the height reaches the top edge of clip space, which is -1 under Vulkan's
    // downward Y -- the same negation the perspective branch applies.
    EXPECT_FLOAT_EQ((p * glm::vec4(0.0f, 4.0f, -5.0f, 1.0f)).y, -1.0f);
    EXPECT_FLOAT_EQ((p * glm::vec4(4.0f * kAspect, 0.0f, -5.0f, 1.0f)).x, 1.0f);
}

TEST(CameraTest, DepthLinearInvertsWhicheverProjectionBuiltTheDepth) {
    for (const Camera::Projection mode : {Camera::Projection::Perspective, Camera::Projection::Orthographic}) {
        const Camera camera = framedAs(mode);
        const glm::vec4 c = camera.depthLinear();

        for (const float distance : {0.5f, 1.0f, 7.5f, 30.0f}) {
            const float depth = depthAt(camera, distance);
            EXPECT_NEAR(viewDistance(c, depth), distance, distance * 1e-4f)
                << "distance " << distance << " under mode " << static_cast<int>(mode);
        }
    }
}

TEST(CameraTest, UnderPerspectiveTheFourCoefficientsAreNearOverDepthToTheBit) {
    const Camera camera = framedAs(Camera::Projection::Perspective);
    const glm::vec4 c = camera.depthLinear();

    // The claim P3 rests on: three shaders gave up `pc.nearPlane / depth` for a general
    // expression, and the perspective path had to come out byte-identical. It does
    // because two of the four coefficients are exactly zero and one is exactly -1, so
    // the numerator folds to -near and the denominator to -depth, and IEEE division of
    // two negated operands is the negation of neither. EXPECT_EQ rather than
    // EXPECT_FLOAT_EQ is the point of the test.
    EXPECT_EQ(c.x, 0.0f);
    EXPECT_EQ(c.z, -1.0f);
    EXPECT_EQ(c.w, 0.0f);
    EXPECT_EQ(c.y, camera.nearPlane);

    for (const float distance : {0.5f, 1.0f, 7.5f, 30.0f, 3700.0f}) {
        const float depth = depthAt(camera, distance);
        EXPECT_EQ(viewDistance(c, depth), camera.nearPlane / depth) << "distance " << distance;
    }
}

TEST(CameraTest, DepthLinearComesOffTheMatrixRatherThanFromTheFieldsBesideIt) {
    Camera camera = framedAs(Camera::Projection::Orthographic);
    const glm::vec4 before = camera.depthLinear();

    // Moving the far plane has to move the linearization, because the two are the same
    // statement. A copy rebuilt from `nearPlane` and `orthoFar` by hand could satisfy
    // this and still disagree with the matrix; reading the matrix is what cannot.
    camera.orthoFar *= 2.0f;
    EXPECT_NE(camera.depthLinear(), before);
    EXPECT_NEAR(viewDistance(camera.depthLinear(), depthAt(camera, 12.0f)), 12.0f, 1e-3f);

    // And the aspect ratio has to leave it alone, which is why depthLinear() takes none.
    const glm::mat4 wide = camera.projection(4.0f);
    const glm::mat4 tall = camera.projection(0.5f);
    EXPECT_EQ(wide[2][2], tall[2][2]);
    EXPECT_EQ(wide[3][2], tall[3][2]);
}

TEST(CameraTest, FramingASceneSizesTheOrthographicBoxAsWellAsTheNearPlane) {
    Camera camera;
    const glm::mat4 unframed = camera.projection(kAspect);
    camera.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));

    // The perspective branch is untouched by the two new fields -- it reads neither, and
    // the only thing framing moves in it is the near plane it always moved.
    const glm::mat4 framed = camera.projection(kAspect);
    EXPECT_EQ(framed[0][0], unframed[0][0]);
    EXPECT_EQ(framed[1][1], unframed[1][1]);
    EXPECT_EQ(framed[2][3], unframed[2][3]);
    EXPECT_EQ(framed[3][2], camera.nearPlane);

    // A camera switched to Orthographic afterwards must not first need two numbers
    // picked by hand: framing a scene sizes its box too.
    EXPECT_GT(camera.orthoHeight, 1.0f);
    EXPECT_GT(camera.orthoFar, camera.orthoHeight);

    camera.projectionMode = Camera::Projection::Orthographic;
    EXPECT_NEAR(depthAt(camera, camera.nearPlane), 1.0f, 1e-5f);
    EXPECT_NEAR(depthAt(camera, camera.orthoFar), 0.0f, 1e-5f);
}

// ------------------------------------------------------- the other three controllers

namespace {

/// A one-node tree with the node at `at`, swept so its world transform is current. The
/// sweep is what the follow cameras read, and a tree that never had one holds identity.
Scene treeAt(NodeId& node, const glm::vec3& at) {
    Scene tree;
    node = tree.create("player");
    tree.setLocalPosition(node, at);
    tree.update({});
    return tree;
}

/// Two frames: `key` down for the first and up for the second. **The release needs a frame
/// of its own** -- `pressed` is `held && !heldLast`, so a key released after `beginFrame`
/// and pressed again before the next one never comes back up as far as the map is concerned
/// and the second tap reports no edge at all.
void tapFrame(InputMap& map, Camera& camera, Key key) {
    map.onKey(key, true);
    map.beginFrame();
    camera.update(map, 0.016f);
    map.onKey(key, false);
    map.beginFrame();
    camera.update(map, 0.016f);
}

/// One frame of pointer travel with `button` already down, which is the second frame of a
/// drag and therefore the first one that turns anything.
void dragFrame(InputMap& map, Camera& camera, double x, double y) {
    map.onCursorPos(x, y);
    map.beginFrame();
    camera.update(map, 0.016f);
}

/// Every live action whose name starts with `Camera.`, in declaration order. What a
/// binding menu would list, which is the question "are there stale rows" is really about.
std::vector<std::string> liveCameraRows(const InputMap& map) {
    std::vector<std::string> rows;
    for (ActionId id = 0; id < map.actionCount(); ++id) {
        if (map.actionLive(id) && map.actionName(id).rfind("Camera.", 0) == 0) rows.push_back(map.actionName(id));
    }
    return rows;
}

} // namespace

TEST(CameraTest, FirstPersonLooksWithNoButtonHeldAndHoldsThePointerForItsWholeLife) {
    mouseGrabReset();
    InputMap map;
    FirstPersonCamera camera;

    EXPECT_FALSE(mouseGrabbed());
    camera.activate(map);
    EXPECT_TRUE(mouseGrabbed()) << "activation is the grab -- there is no button to hold";

    map.onCursorPos(0.0, 0.0);
    map.beginFrame();
    camera.update(map, 0.016f);
    const float yawBefore = camera.yaw;

    // Nothing is pressed on this frame and the view turns anyway, which is the whole
    // difference from every drag-to-look controller above.
    dragFrame(map, camera, 200.0, 0.0);
    EXPECT_LT(camera.yaw, yawBefore) << "the pointer moving right turns the camera right";
    EXPECT_TRUE(mouseGrabbed());

    camera.deactivate(map);
    EXPECT_FALSE(mouseGrabbed());
    EXPECT_TRUE(liveCameraRows(map).empty());
    mouseGrabReset();
}

TEST(CameraTest, FirstPersonDeclaresOneRowAndNoneOfThemWalks) {
    mouseGrabReset();
    InputMap map;
    FirstPersonCamera camera;
    camera.activate(map);

    // One row, and it is not a movement. The camera that flies is FlyCamera; this one is
    // carried by whatever the solver is moving.
    EXPECT_EQ(liveCameraRows(map), (std::vector<std::string>{"Camera.Look"}));
    EXPECT_EQ(map.find("Camera.Forward"), kInvalidAction);

    camera.focus = glm::vec3(1.0f, 2.0f, 3.0f);
    for (const Key key : {Key::W, Key::A, Key::S, Key::D, Key::Q, Key::E, Key::Space}) {
        holdFrame(map, camera, key, 1.0f);
    }
    EXPECT_EQ(camera.focus, glm::vec3(1.0f, 2.0f, 3.0f)) << "a whole second on every key moves nothing";
    mouseGrabReset();
}

TEST(CameraTest, FirstPersonPitchHoldsAtBothPoles) {
    mouseGrabReset();
    InputMap map;
    FirstPersonCamera camera;
    camera.activate(map);
    map.onCursorPos(0.0, 0.0);
    map.beginFrame();
    camera.update(map, 0.016f);

    // Straight down, then straight up, from the same start -- one pole apiece, because a
    // clamp written with one comparison passes a test that only pushes one way.
    for (int i = 0; i < 60; ++i) dragFrame(map, camera, 0.0, static_cast<double>(i + 1) * 200.0);
    EXPECT_GT(camera.pitch, -glm::half_pi<float>());
    EXPECT_LT(camera.forward().y, 1.0f);
    EXPECT_GT(camera.forward().y, -1.0f);
    const float lowest = camera.pitch;

    for (int i = 0; i < 240; ++i) dragFrame(map, camera, 0.0, 12000.0 - static_cast<double>(i + 1) * 200.0);
    EXPECT_LT(camera.pitch, glm::half_pi<float>());
    EXPECT_NEAR(camera.pitch, -lowest, 1e-4f) << "the clamp is symmetric or one pole is unreachable";

    // And the view matrix is still a matrix at both ends: straight up degenerates lookAt's
    // up vector and produces NaNs rather than a stopped camera.
    const glm::mat4 view = camera.view();
    EXPECT_EQ(view[3][3], view[3][3]);
    mouseGrabReset();
}

TEST(CameraTest, FirstPersonPutsTheEyeOnItsTargetAtEyeHeight) {
    mouseGrabReset();
    InputMap map;
    FirstPersonCamera camera;
    camera.activate(map);
    camera.frameBounds(glm::vec3(-10.0f, 0.0f, -2.0f), glm::vec3(10.0f, 4.0f, 2.0f));
    ASSERT_GT(camera.distance, 0.0f) << "framing gives every camera a distance, including this one";

    NodeId player;
    Scene tree = treeAt(player, glm::vec3(3.0f, 0.0f, -4.0f));
    camera.follow(&tree, player);
    camera.eyeHeight = 1.7f;

    map.beginFrame();
    camera.update(map, 0.016f);
    // The eye *is* the focus -- a distance left over from framing would back the camera out
    // of the head it is meant to be inside.
    EXPECT_EQ(camera.distance, 0.0f);
    EXPECT_EQ(camera.position(), camera.focus);
    EXPECT_EQ(camera.focus, glm::vec3(3.0f, 1.7f, -4.0f));

    // And a camera told to follow nothing hands `focus` back to the game.
    camera.follow(nullptr, NodeId{});
    camera.focus = glm::vec3(9.0f);
    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_EQ(camera.focus, glm::vec3(9.0f));
    mouseGrabReset();
}

TEST(CameraTest, ThirdPersonReadsItsTargetItselfRatherThanBeingToldWhereItIs) {
    mouseGrabReset();
    InputMap map;
    ThirdPersonCamera camera;
    camera.activate(map);

    NodeId player;
    Scene tree = treeAt(player, glm::vec3(0.0f));
    camera.follow(&tree, player);
    camera.heightOffset = 1.2f;

    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_EQ(camera.focus, glm::vec3(0.0f, 1.2f, 0.0f));

    // The target moves and the camera has it on the *same* update, with nobody pushing a
    // position in. That is the whole reason the target is held rather than the position:
    // `update` runs before `Game::frameUpdate`, so a pushed position is a frame stale.
    tree.setLocalPosition(player, glm::vec3(5.0f, 0.0f, -2.0f));
    tree.update({});
    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_EQ(camera.focus, glm::vec3(5.0f, 1.2f, -2.0f));
    mouseGrabReset();
}

TEST(CameraTest, ThirdPersonOrbitsTheTargetAndNeverWritesYaw) {
    mouseGrabReset();
    InputMap map;
    ThirdPersonCamera camera;
    camera.activate(map);
    camera.distance = 4.0f;

    NodeId player;
    Scene tree = treeAt(player, glm::vec3(0.0f));
    camera.follow(&tree, player);

    map.onCursorPos(0.0, 0.0);
    map.onMouseButton(MouseButton::Middle, true);
    map.beginFrame();
    camera.update(map, 0.016f);
    const glm::vec3 focusBefore = camera.focus;

    dragFrame(map, camera, 120.0, 0.0);
    // **The focus is held and the eye swings**, which is the opposite of FlyCamera and what
    // a camera pointed at a character has to do: the thing being looked at must not move.
    EXPECT_EQ(camera.focus, focusBefore);
    EXPECT_NE(camera.yaw, 0.0f);
    EXPECT_NEAR(glm::length(camera.position() - camera.focus), 4.0f, 1e-4f);

    // And the coupling only runs one way. The target walks a long way in both horizontal
    // axes and the yaw the game resolves "forward" against does not move a bit -- two
    // integrators feeding each other is the chase problem, and this is its absence.
    const float yaw = camera.yaw;
    map.onMouseButton(MouseButton::Middle, false);
    for (int i = 1; i <= 20; ++i) {
        tree.setLocalPosition(player, glm::vec3(static_cast<float>(i), 0.0f, static_cast<float>(-i)));
        tree.update({});
        map.beginFrame();
        camera.update(map, 0.016f);
    }
    EXPECT_EQ(camera.yaw, yaw);
    EXPECT_EQ(camera.focus, glm::vec3(20.0f, 1.2f, -20.0f));
    mouseGrabReset();
}

TEST(CameraTest, ThirdPersonScrollStaysBetweenItsBoundsAndOnlyTheScrollIsBounded) {
    mouseGrabReset();
    InputMap map;
    ThirdPersonCamera camera;
    camera.activate(map);
    camera.minDistance = 1.5f;
    camera.maxDistance = 12.0f;

    // Framing, or `--camera`, writes a distance outside the band and this controller does
    // not overrule it: clamping every frame would move a camera nobody had touched.
    camera.distance = 40.0f;
    map.beginFrame();
    camera.update(map, 0.016f);
    EXPECT_EQ(camera.distance, 40.0f);

    for (int i = 0; i < 100; ++i) {
        map.onScroll(1.0);
        map.beginFrame();
        camera.update(map, 0.016f);
    }
    EXPECT_FLOAT_EQ(camera.distance, 1.5f);

    for (int i = 0; i < 100; ++i) {
        map.onScroll(-1.0);
        map.beginFrame();
        camera.update(map, 0.016f);
    }
    EXPECT_FLOAT_EQ(camera.distance, 12.0f);
    mouseGrabReset();
}

TEST(CameraTest, IsometricYawSnapsToQuarterTurnsAcrossTheSeam) {
    mouseGrabReset();
    InputMap map;
    IsometricCamera camera;
    camera.activate(map);
    camera.yaw = 0.0f;

    // Sixteen presses one way. Every one of them has to land on a multiple of pi/2 -- an
    // angle accumulated by adding pi/2 to a float drifts, and a view a degree off axis is a
    // camera that has stopped being isometric.
    for (int i = 1; i <= 16; ++i) {
        tapFrame(map, camera, Key::E);
        const float turns = camera.yaw / glm::half_pi<float>();
        EXPECT_NEAR(turns, std::round(turns), 1e-5f) << "after " << i << " presses, yaw " << camera.yaw;
        EXPECT_GE(camera.yaw, 0.0f);
        EXPECT_LT(camera.yaw, glm::two_pi<float>());
    }
    // Four presses is a full turn, so sixteen is exactly where it started -- checked as an
    // equality because the count is the state and the angle is derived from it.
    EXPECT_EQ(camera.yaw, 0.0f);

    // And the seam is crossed the other way too: one press left from zero is three quarter
    // turns, not a negative angle that the next `lround` would have to guess about.
    tapFrame(map, camera, Key::Q);
    EXPECT_FLOAT_EQ(camera.yaw, 3.0f * glm::half_pi<float>());
    tapFrame(map, camera, Key::Q);
    EXPECT_FLOAT_EQ(camera.yaw, glm::pi<float>());

    // The pitch is the true isometric angle and is held there, not clamped toward it.
    EXPECT_FLOAT_EQ(camera.pitch, camera.fixedPitch);
    EXPECT_NEAR(glm::degrees(camera.pitch), -35.264f, 1e-3f);
    mouseGrabReset();
}

TEST(CameraTest, IsometricScrollMovesTheBoxAndNotTheDistance) {
    mouseGrabReset();
    InputMap map;
    IsometricCamera camera;
    camera.activate(map);
    EXPECT_EQ(camera.projectionMode, Camera::Projection::Orthographic);

    camera.orthoHeight = 20.0f;
    camera.distance = 30.0f;
    map.onScroll(1.0);
    map.beginFrame();
    camera.update(map, 0.016f);

    // **This is the thing a controller written for perspective gets wrong.** A parallel
    // projection ignores how far away the eye is, so dollying changes nothing on screen.
    EXPECT_EQ(camera.distance, 30.0f);
    EXPECT_LT(camera.orthoHeight, 20.0f);

    for (int i = 0; i < 200; ++i) {
        map.onScroll(1.0);
        map.beginFrame();
        camera.update(map, 0.016f);
    }
    EXPECT_FLOAT_EQ(camera.orthoHeight, camera.minOrthoHeight);
    mouseGrabReset();
}

TEST(CameraTest, IsometricPansTheFocusInTheGroundPlane) {
    mouseGrabReset();
    InputMap map;
    IsometricCamera camera;
    camera.activate(map);
    camera.yaw = 0.0f;
    camera.orthoHeight = 10.0f;
    camera.focus = glm::vec3(0.0f, 3.0f, 0.0f);

    holdFrame(map, camera, Key::W, 0.1f);
    // Along +Z at yaw zero, and the height it was panning at is untouched -- a pan that
    // used `forward()` would drag the focus into the floor at this pitch.
    EXPECT_GT(camera.focus.z, 0.0f);
    EXPECT_FLOAT_EQ(camera.focus.x, 0.0f);
    EXPECT_FLOAT_EQ(camera.focus.y, 3.0f);

    camera.focus = glm::vec3(0.0f, 3.0f, 0.0f);
    holdFrame(map, camera, Key::D, 0.1f);
    EXPECT_LT(camera.focus.x, 0.0f) << "screen-right at yaw zero is cross(+Z, +Y), which is -X";
    EXPECT_FLOAT_EQ(camera.focus.y, 3.0f);
    mouseGrabReset();
}

TEST(CameraTest, IsometricRendersParallelEdgesAsParallel) {
    mouseGrabReset();
    InputMap map;
    IsometricCamera camera;
    camera.activate(map);
    camera.focus = glm::vec3(0.0f);
    camera.yaw = 0.0f;
    camera.distance = 10.0f;
    camera.orthoHeight = 40.0f;
    camera.orthoFar = 200.0f;
    map.beginFrame();
    camera.update(map, 0.016f); // which is what puts the pitch on the isometric angle

    // Two world-space edges that are parallel to each other and **run away from the
    // camera**, eight metres apart. That last part is the whole of the check: two edges
    // parallel to the screen's X stay parallel under a perspective projection too, so a
    // pair chosen that way would pass for either matrix. These converge on a vanishing
    // point under one and not under the other.
    const auto skew = [&](const Camera& c) {
        const glm::mat4 vp = c.viewProjection(kAspect);
        const auto project = [&](const glm::vec3& p) {
            const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
            return glm::vec2(clip) / clip.w;
        };
        const glm::vec2 left = glm::normalize(project({-4.0f, 0.0f, 20.0f}) - project({-4.0f, 0.0f, 5.0f}));
        const glm::vec2 right = glm::normalize(project({4.0f, 0.0f, 20.0f}) - project({4.0f, 0.0f, 5.0f}));
        return std::abs(left.x * right.y - left.y * right.x);
    };

    EXPECT_NEAR(skew(camera), 0.0f, 1e-5f) << "a parallel projection has no vanishing point";

    camera.projectionMode = Camera::Projection::Perspective;
    EXPECT_GT(skew(camera), 1e-3f) << "the check is capable of failing";
    mouseGrabReset();
}

TEST(CameraTest, SwitchingControllersLeavesNoStaleRowsAndKeepsARebind) {
    mouseGrabReset();
    InputMap map;
    FlyCamera fly;
    ThirdPersonCamera follow;

    fly.activate(map);
    const ActionId forward = map.find("Camera.Forward");
    ASSERT_NE(forward, kInvalidAction);
    EXPECT_EQ(liveCameraRows(map).size(), 9u);
    // A player moves one before the switch.
    map.setBindings(forward, "Up");

    fly.deactivate(map);
    follow.activate(map);
    // **The menu lists what is running and nothing else.** Eight of the nine are gone and
    // the ninth is the drag, which both controllers spell the same way and therefore share.
    EXPECT_EQ(liveCameraRows(map), (std::vector<std::string>{"Camera.Orbit"}));
    EXPECT_EQ(map.find("Camera.Forward"), kInvalidAction);

    follow.deactivate(map);
    fly.activate(map);
    EXPECT_EQ(liveCameraRows(map).size(), 9u);
    EXPECT_EQ(map.find("Camera.Forward"), forward) << "the same id, so anything holding one still means it";
    EXPECT_EQ(map.bindingList(forward), "Up") << "retire keeps bindings; declare revives them";
    EXPECT_FALSE(map.isDefault(forward));
    mouseGrabReset();
}

// ------------------------------------------------------------------ picking (rayThrough)
//
// The arithmetic that turns a cursor into something `PhysicsWorld::raycast` can take. It is
// tested here rather than through `Engine::cursorRay` because that one needs a swapchain to
// say what the render extent is, and none of what can be got wrong below is about a device:
// it is the Y convention, the reverse-Z convention, and the far plane at infinity.

/// A camera at a known pose looking down -Z, which is what yaw 0 and pitch 0 give.
Camera atOrigin() {
    Camera camera;
    camera.focus = glm::vec3(0.0f);
    camera.distance = 10.0f;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    return camera;
}

TEST(CameraRayTest, TheCentrePixelLooksAlongTheCameraForward) {
    const Camera camera = atOrigin();
    const Ray ray = rayThrough(camera, {800.0f, 450.0f}, {1600.0f, 900.0f});

    EXPECT_NEAR(glm::length(ray.direction), 1.0f, 1e-5f);
    EXPECT_NEAR(glm::dot(ray.direction, camera.forward()), 1.0f, 1e-4f);
    // On the near plane, not at the eye: the origin is where the ray enters what was drawn.
    EXPECT_NEAR(glm::distance(ray.origin, camera.position()), camera.nearPlane, 1e-3f);
}

TEST(CameraRayTest, TheTopLeftPixelIsUpAndLeftOfCentre) {
    const Camera camera = atOrigin();
    const Ray corner = rayThrough(camera, {0.0f, 0.0f}, {1600.0f, 900.0f});

    // **This is the Y convention and it is the whole reason this test exists.** The cursor's
    // origin is the top-left corner with y down, and the projection negates Y for Vulkan's
    // downward clip space -- so pixel y 0 must come out as +Y in the world. Flipped
    // anywhere in the chain, picking is mirrored about the horizon and still looks
    // plausible until something is clicked.
    EXPECT_GT(corner.direction.y, 0.0f) << "the top of the screen looks up";

    // Against the camera's own right vector rather than against a world axis: at yaw 0 this
    // camera looks down **+Z**, so screen-right is -X and asserting a sign on `direction.x`
    // pins the yaw convention instead of the handedness this is about.
    const glm::vec3 right = glm::normalize(glm::cross(camera.forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
    EXPECT_LT(glm::dot(corner.direction, right), 0.0f) << "the left of the screen looks left";
}

TEST(CameraRayTest, EveryRayIsFiniteUnderTheInfiniteProjection) {
    const Camera camera = atOrigin();
    for (const glm::vec2 pixel : {glm::vec2(0.0f, 0.0f), glm::vec2(1600.0f, 900.0f), glm::vec2(800.0f, 0.0f)}) {
        const Ray ray = rayThrough(camera, pixel, {1600.0f, 900.0f});
        // The perspective matrix puts the far plane at infinity, so unprojecting depth 0
        // divides by a zero w. Sampling any depth strictly inside the range instead is what
        // keeps this finite, and a NaN direction is a raycast that silently never hits.
        EXPECT_TRUE(std::isfinite(ray.direction.x) && std::isfinite(ray.direction.y) &&
                    std::isfinite(ray.direction.z));
        EXPECT_NEAR(glm::length(ray.direction), 1.0f, 1e-5f);
    }
}

TEST(CameraRayTest, AnOrthographicViewGivesParallelRaysFromDifferentOrigins) {
    Camera camera = atOrigin();
    camera.projectionMode = Camera::Projection::Orthographic;
    camera.orthoHeight = 20.0f;

    const Ray centre = rayThrough(camera, {800.0f, 450.0f}, {1600.0f, 900.0f});
    const Ray corner = rayThrough(camera, {0.0f, 0.0f}, {1600.0f, 900.0f});

    // What an eye position plus a field-of-view direction cannot express, and the reason
    // both ends are unprojected: under a parallel projection the rays do not share a point.
    EXPECT_NEAR(glm::dot(centre.direction, corner.direction), 1.0f, 1e-4f);
    EXPECT_GT(glm::distance(centre.origin, corner.origin), 1.0f);
}

TEST(CameraRayTest, AZeroExtentYieldsAZeroDirectionRatherThanANaN) {
    const Ray ray = rayThrough(atOrigin(), {0.0f, 0.0f}, {0.0f, 0.0f});
    EXPECT_EQ(ray.direction, glm::vec3(0.0f));
}
