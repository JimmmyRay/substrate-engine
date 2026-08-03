#pragma once

#include "core/Input.h"

#include <glm/glm.hpp>

namespace scene {

/**
 * @brief A pose and a projection, and a controller hook that defaults to taking no input.
 *
 * The state is a focus point, a distance and yaw/pitch, which is what `view()` is built
 * from and what `--camera` reproduces. Nothing here reads the keyboard: the three virtuals
 * are how a controller attaches, and their empty defaults are what makes **this type also
 * the null camera** -- "looks at the scene and takes no input" is both its definition and
 * what a null object has to be. `scene::FlyCamera` in `CameraControllers.h` is the free-fly
 * one, and a game installs it by name.
 *
 * Projection is **reverse-Z**: near maps to depth 1 and the far plane to 0, under both
 * modes. For the perspective one the far plane is at infinity, and distributing float
 * precision against the depth-buffer hyperbola is what keeps distant geometry stable
 * across Sponza's ~3700-unit extent.
 */
class Camera {
  public:
    virtual ~Camera() = default;

    /// Declare whatever actions this camera consumes. Called by `Engine::setCamera` when
    /// this camera becomes the active one, and never for a view camera.
    ///
    /// **On activation rather than in a constructor**, which is what lets a game hold every
    /// camera it will use and pay input surface only for the one that is running: two held
    /// cameras declaring `Camera.Forward` in their constructors would duel, and
    /// `InputMap::conflicts()` would be right to say so.
    virtual void activate(core::input::InputMap& /*map*/) {}

    /// Retire what `activate` declared. `retire`, not `clearBindings`: a cleared row is
    /// still visible in the binding menu and still rebindable, for a camera that is not
    /// running.
    virtual void deactivate(core::input::InputMap& /*map*/) {}

    /// Apply this frame's actions. Called once per frame with the frame delta, on the
    /// active camera only.
    virtual void update(const core::input::InputMap& /*in*/, float /*dt*/) {}

    /// Which family of matrix `projection()` builds. **Both are hand-built and both are
    /// reverse-Z**, because the depth clear, the `GREATER` compare ops and every
    /// `FAR_DEPTH` test in the shaders are written for that convention and nothing else.
    /// `glm::ortho` is forward-Z and would invert all three at once, which is why the
    /// obvious library call is the wrong move here.
    enum class Projection { Perspective, Orthographic };

    /// Point the camera at a scene, and scale its near plane and orthographic box to it.
    ///
    /// **Sets the pose and the projection and nothing else.** A controller's feel -- how
    /// fast it flies, how far a drag turns it -- is the controller's, and `FlyCamera`
    /// derives its speed from `distance` rather than from a field this would have written.
    void frameBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax);

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
    glm::mat4 viewProjection(float aspect) const { return projection(aspect) * view(); }
    glm::vec3 position() const;
    /// Normalised direction from the eye toward the focus point.
    glm::vec3 forward() const;

    /// The four coefficients that turn a reverse-Z depth sample back into a distance
    /// along the view axis, in the order `viewDistance()` in `frame.glsl` reads them.
    /// **Read off the matrix rather than rebuilt from near and far**, so a projection
    /// and its inverse cannot drift apart; the derivation is on that function.
    glm::vec4 depthLinear() const;

    /// Which projection `projection()` builds. The rest of the engine reads it through
    /// `depthLinear()` and one flag in the frame uniforms rather than branching on it:
    /// culling tests clip-space inequalities, `worldFromDepth` goes through
    /// `invViewProj`, the TAA jitter is a clip-space post-multiply, and the sun's shadow
    /// box never depended on the camera. The skybox ray is the one thing that does.
    Projection projectionMode = Projection::Perspective;

    float nearPlane = 0.1f;
    float fovYRadians = 1.0472f;    ///< 60 degrees, perspective only
    /// World units the view spans vertically under `Orthographic`; width follows aspect.
    float orthoHeight = 10.0f;
    /// The orthographic far plane. There is no perspective equivalent and there should
    /// not be one -- that projection is infinite, which is what `cull.comp` and the
    /// cascade fit are both written against.
    float orthoFar = 1000.0f;

    /// The pose, and public because it is the camera's whole reproducible state: the
    /// overlay prints these four and `--camera` writes them back, which is what turns
    /// "it does this when I stand about here" into a run that can be repeated. Set them
    /// after frameBounds(), which derives the near plane and the box from the scene
    /// and would otherwise overwrite them.
    glm::vec3 focus{0.0f};
    float distance = 5.0f;
    float yaw = 0.0f;   ///< radians, around +Y
    float pitch = 0.0f; ///< radians, clamped short of the poles
};

/// A world-space ray. `direction` is normalised, and zero for a degenerate view.
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f};

    /// Where the ray is after `t` metres.
    [[nodiscard]] glm::vec3 at(float t) const { return origin + direction * t; }
};

/**
 * @brief The world ray through a point on the render target -- the picking primitive.
 *
 * `PhysicsWorld::raycast` names picking as one of the five things it is for, and until this
 * there was no way to get it a ray: turning a cursor position into one needs the inverse
 * view-projection, and a game holding a camera cannot build the aspect the frame was drawn
 * at. `Engine::cursorRay` is that, with the window-to-render-target transform applied first.
 *
 * A free function beside `Camera` rather than a method on it, for the reason `gfx::decalAt`
 * is one: it is arithmetic over a camera and two vectors, it belongs to no instance, and
 * here it is testable without a device.
 *
 * **Both ends are unprojected rather than one end plus a forward vector.** Composing the
 * eye position with a direction built from the field of view is a third place the
 * projection convention would have to be restated, and it is wrong outright under
 * `Orthographic`, where every ray starts somewhere different and they are all parallel.
 * Two unprojected points are correct under both without branching on which one it got.
 *
 * @param pixel  a point on the render target, `(0,0)` at the top-left corner and y down --
 *               which is the cursor's convention and Vulkan's clip-space Y, so no flip
 *               happens anywhere in here.
 * @param extent the render target's size in pixels. A zero extent yields a zero direction.
 */
[[nodiscard]] Ray rayThrough(const Camera& camera, glm::vec2 pixel, glm::vec2 extent);

} // namespace scene
