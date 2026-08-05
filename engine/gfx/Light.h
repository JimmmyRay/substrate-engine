#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace gfx {

/// One punctual light, matching the KHR_lights_punctual model. Field for field the std430
/// layout of the light storage buffer the shaders read -- see `lights.glsl`.
struct GpuLight {
    /// xyz world position (unused for directional), w range -- 0 means unbounded.
    glm::vec4 position;
    /// xyz the direction the light points (unused for point), w unused. The shader
    /// negates this to get L, the direction *toward* the light.
    glm::vec4 direction;
    /// rgb colour, w intensity.
    glm::vec4 color;
    /// x cos(inner cone), y cos(outer cone), z type, w unused. Reclaiming `w` for
    /// something else breaks the four-`vec4` std430 alignment the static_assert holds.
    glm::vec4 params;
};
static_assert(sizeof(GpuLight) % 16 == 0, "GpuLight must stay std430-aligned");

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

/**
 * @param towardLight Points **at** the light -- the opposite sign from `makeSpotLight`'s
 *        `direction`, matching `render.sunDirection` and `updateCascades`. glTF points
 *        every punctual light down local -Z, so an importer negates on the way in.
 */
GpuLight makeDirectionalLight(const glm::vec3& towardLight, const glm::vec3& color, float intensity);
/// `range` is where the light reaches zero; the falloff is inverse-square windowed to
/// arrive there smoothly rather than being clipped.
GpuLight makePointLight(const glm::vec3& position, float range, const glm::vec3& color, float intensity);
/// Cone angles in radians, per the glTF spec: inner is where falloff begins, outer
/// where it reaches zero.
GpuLight makeSpotLight(const glm::vec3& position, const glm::vec3& direction, float range, float innerAngle,
                       float outerAngle, const glm::vec3& color, float intensity);

/**
 * @brief How much this light is worth spending a budget slot on: luminance over squared
 *        distance to `viewPosition`, and infinity for a directional light.
 *
 * An approximation of on-screen contribution, not a measure of it -- neither a spot's
 * cone orientation nor whether the range reaches `viewPosition` is modelled, both because
 * a light can illuminate what is on screen from outside the camera's neighbourhood.
 */
float lightImportance(const GpuLight& light, const glm::vec3& viewPosition);

/**
 * @brief The six planes of a view frustum, inward-facing, in world space.
 *
 * Each `vec4` is `(nx, ny, nz, d)` with the normal pointing *into* the frustum, so a
 * point is inside when `dot(n, p) + d >= 0` for all six. Extracted by Gribb-Hartmann
 * from the rows of a view-projection matrix.
 */
struct Frustum {
    glm::vec4 planes[6];
};

/// Pull the six planes out of a view-projection matrix. Pass the *unjittered* one: a
/// sub-pixel TAA offset cannot change what is visible, and using the jittered matrix
/// would make culling decisions flicker on the jitter period.
[[nodiscard]] Frustum extractFrustum(const glm::mat4& viewProj);

/**
 * @brief Could this light affect anything inside the frustum?
 *
 * **Must stay conservative**: false only when the light's volume provably cannot reach any
 * visible surface, so culling by it cannot change a shaded pixel and the golden set stays
 * byte-identical. Directional lights and lights of range 0 (unbounded, per
 * `GpuLight::position.w`) are therefore never culled, and a spot is bounded by its sphere
 * rather than its cone.
 */
[[nodiscard]] bool lightVisible(const GpuLight& light, const Frustum& frustum);

} // namespace gfx
