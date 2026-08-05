#pragma once

#include "core/Input.h"

#include <glm/glm.hpp>

namespace scene {

/**
 * @brief A pose and a projection, and a controller hook that defaults to taking no input.
 *
 * The state is a focus point, a distance and yaw/pitch, which is what `view()` is built
 * from and what `--camera` reproduces. The empty virtuals are what make this type also the
 * null camera; `scene::FlyCamera` in `CameraControllers.h` is the free-fly one.
 *
 * Projection is **reverse-Z**: near maps to depth 1 and the far plane to 0, under both
 * modes, and the perspective far plane is at infinity. That distribution of float precision
 * against the depth-buffer hyperbola is what keeps distant geometry stable across Sponza's
 * ~3700-unit extent.
 */
class Camera {
  public:
    virtual ~Camera() = default;

    /// Declare whatever actions this camera consumes. Called by `Engine::setCamera` when
    /// this camera becomes the active one, and never for a view camera.
    ///
    /// **On activation rather than in a constructor**, so a game can hold every camera it
    /// will use: two held cameras declaring `Camera.Forward` in their constructors would
    /// duel, and `InputMap::conflicts()` would be right to say so.
    virtual void activate(core::input::InputMap& /*map*/) {}

    /// Retire what `activate` declared. Retire, not clear: a cleared row is still visible
    /// in the binding menu and still rebindable, for a camera that is not running.
    virtual void deactivate(core::input::InputMap& /*map*/) {}

    /// Apply this frame's actions. Called once per frame with the frame delta, on the
    /// active camera only.
    virtual void update(const core::input::InputMap& /*in*/, float /*dt*/) {}

    /// Which family of matrix `projection()` builds. **Both are hand-built and both are
    /// reverse-Z**, because the depth clear, the `GREATER` compare ops and every
    /// `FAR_DEPTH` test in the shaders are written for that convention. `glm::ortho` is
    /// forward-Z and inverts all three at once.
    enum class Projection { Perspective, Orthographic };

    /// Point the camera at a scene, and scale its near plane and orthographic box to it.
    /// Writes the pose and the projection only -- a controller's feel is the controller's.
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

    /// Which projection `projection()` builds. Downstream reads it through `depthLinear()`
    /// and one flag in the frame uniforms; the skybox ray is the only thing that branches
    /// on the mode itself.
    Projection projectionMode = Projection::Perspective;

    float nearPlane = 0.1f;
    float fovYRadians = 1.0472f;    ///< 60 degrees, perspective only
    /// World units the view spans vertically under `Orthographic`; width follows aspect.
    float orthoHeight = 10.0f;
    /// The orthographic far plane. There is no perspective equivalent: that projection is
    /// infinite, which is what `cull.comp` and the cascade fit are both written against.
    float orthoFar = 1000.0f;

    /// The pose, and the camera's whole reproducible state -- the overlay prints these four
    /// and `--camera` writes them back. Set them **after** frameBounds(), which derives the
    /// near plane and the box from the scene and overwrites them.
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
 *        `Engine::cursorRay` is this with the window-to-render-target transform applied.
 *
 * **Both ends are unprojected rather than one end plus a forward vector.** Composing the
 * eye position with a direction built from the field of view restates the projection
 * convention a third time, and it is wrong outright under `Orthographic`, where every ray
 * starts somewhere different and they are all parallel.
 *
 * @param pixel  a point on the render target, `(0,0)` at the top-left corner and y down --
 *               the cursor's convention and Vulkan's clip-space Y, so nothing in here
 *               flips.
 * @param extent the render target's size in pixels. A zero extent yields a zero direction.
 */
[[nodiscard]] Ray rayThrough(const Camera& camera, glm::vec2 pixel, glm::vec2 extent);

} // namespace scene
