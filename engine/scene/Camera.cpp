#include "scene/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace scene {

void Camera::frameBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax) {
    const glm::vec3 extent = boundsMax - boundsMin;
    const float radius = glm::length(extent) * 0.5f;

    // Look along whichever horizontal axis is longest, from a point near that end,
    // at roughly eye height. Framing an interior from outside its bounding sphere
    // just shows the back of an exterior wall; framing it from the exact centre
    // buries the camera in whatever happens to be there.
    const bool longAxisIsX = extent.x >= extent.z;
    yaw = longAxisIsX ? 1.5708f : 0.0f; // +X or +Z
    pitch = -0.05f;

    focus = (boundsMin + boundsMax) * 0.5f;
    focus.y = boundsMin.y + extent.y * 0.25f;

    // A quarter of the long axis: far enough back to see down the space, close
    // enough that the camera does not start embedded in the end wall.
    const float longExtent = longAxisIsX ? extent.x : extent.z;
    distance = longExtent > 0.0f ? longExtent * 0.25f : 5.0f;

    // Scale clipping to the scene rather than hard-coding units: Sponza is ~30 units
    // across its longest axis once its 0.008 root scale is applied, a test cube is 1. A
    // near plane is a fraction of what is being looked at or it eats the geometry, which
    // is a derivation with no user answer and therefore no settings row -- one of the
    // ones D11 audited and kept.
    nearPlane = std::max(radius * 0.002f, 0.01f);

    // The orthographic half of the same scaling, set whichever mode is current: a camera
    // framed on a scene and then switched to Orthographic would otherwise need two
    // numbers picked by hand before it showed anything. A box the height of the bounding
    // sphere, and a far plane past the far side of it.
    orthoHeight = std::max(radius, 1.0f);
    orthoFar = std::max(radius * 4.0f, 1.0f);
}

glm::vec3 Camera::forward() const {
    return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)));
}

glm::vec3 Camera::position() const { return focus - forward() * distance; }

glm::mat4 Camera::view() const { return glm::lookAt(position(), focus, glm::vec3(0.0f, 1.0f, 0.0f)); }

glm::mat4 Camera::projection(float aspect) const {
    if (projectionMode == Projection::Orthographic) {
        // Reverse-Z, hand-built, and that is the whole reason this is not `glm::ortho`.
        // The library call is forward-Z: near at 0 and far at 1. Feeding it to this
        // renderer inverts every `depth > FAR_DEPTH` test, fights a depth buffer cleared
        // to 0 and fights the `GREATER` compare ops, and each of those three failures
        // looks like a different bug.
        //
        // Near maps to 1 and `orthoFar` to 0, exactly as the perspective branch does, so
        // nothing downstream of the matrix has to know which one it got. Y is negated for
        // Vulkan's downward clip-space Y.
        const float halfHeight = orthoHeight * 0.5f;
        const float invRange = 1.0f / (orthoFar - nearPlane);

        glm::mat4 p(0.0f);
        p[0][0] = 1.0f / (halfHeight * aspect);
        p[1][1] = -1.0f / halfHeight;
        p[2][2] = invRange;
        p[3][2] = orthoFar * invRange;
        p[3][3] = 1.0f;
        return p;
    }

    // Infinite reverse-Z. Near maps to 1, infinity to 0; Y is negated for Vulkan's
    // downward clip-space Y.
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);

    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = -f;
    p[2][3] = -1.0f;
    p[3][2] = nearPlane;
    return p;
}

glm::vec4 Camera::depthLinear() const {
    // Clip space is `clip.z = p[2][2] * z + p[3][2]` and `clip.w = p[2][3] * z + p[3][3]`
    // for a view-space z (w = 1). Solving `depth = clip.z / clip.w` for -z gives
    //
    //     distance = (depth * p[3][3] - p[3][2]) / (depth * p[2][3] - p[2][2])
    //
    // which is what `viewDistance()` in frame.glsl evaluates, and these are its four
    // coefficients in that order. Taken from the matrix rather than recomputed from
    // `nearPlane` and `orthoFar`, so the linearization cannot say something the
    // projection does not; `aspect` is 1 because neither of those two rows contains it.
    const glm::mat4 p = projection(1.0f);
    return glm::vec4(p[3][3], p[3][2], p[2][3], p[2][2]);
}

Ray rayThrough(const Camera& camera, glm::vec2 pixel, glm::vec2 extent) {
    if (extent.x <= 0.0f || extent.y <= 0.0f) return {};

    const glm::vec2 ndc = (pixel / extent) * 2.0f - 1.0f;
    const glm::mat4 inverseViewProj = glm::inverse(camera.viewProjection(extent.x / extent.y));

    const auto unproject = [&](float depth) {
        const glm::vec4 p = inverseViewProj * glm::vec4(ndc, depth, 1.0f);
        return glm::vec3(p) / p.w;
    };

    // 1 is the near plane under both projections -- reverse-Z, stated once on `Camera`.
    // **The second sample is not the far plane.** Under the perspective matrix that is
    // infinity, so `p.w` there is zero and the division is a NaN in every component; any
    // depth strictly inside the range gives the same direction, and 0.5 is one.
    const glm::vec3 near = unproject(1.0f);
    const glm::vec3 along = unproject(0.5f) - near;

    const float length = glm::length(along);
    return {near, length > 0.0f ? along / length : glm::vec3(0.0f)};
}

} // namespace scene
