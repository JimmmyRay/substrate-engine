#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace gfx {

/// One punctual light, matching the KHR_lights_punctual model. Mirrors the std430
/// layout of the light storage buffer.
///
/// Deliberately one flat struct rather than a type hierarchy: directional, point and
/// spot differ in which fields they read, not in what they are, and the shader loop
/// branches on `params.z` in four lines.
///
/// This lives in its own header rather than in Renderer.h only so `GltfScene` can
/// produce lights without including the renderer. It is a struct and three factory
/// functions -- not the ten-struct shadow-and-RT schema the roadmap warns against
/// lifting from Tethered.
struct GpuLight {
    /// xyz world position (unused for directional), w range -- 0 means unbounded.
    glm::vec4 position;
    /// xyz the direction the light points (unused for point), w unused. The shader
    /// negates this to get L, the direction *toward* the light.
    glm::vec4 direction;
    /// rgb colour, w intensity.
    glm::vec4 color;
    /// x cos(inner cone), y cos(outer cone), z type, w unused.
    ///
    /// `w` held the light's first punctual shadow-atlas layer, and a negative value
    /// meant it had none. Nothing reads it now: the shadow system was ripped out whole,
    /// and `frame.glsl` records the slot as vacant on the shader side. It keeps its
    /// place rather than being reclaimed because the struct is std430-aligned around
    /// four `vec4`s and a shadow layer is what it will hold again.
    glm::vec4 params;
};
static_assert(sizeof(GpuLight) % 16 == 0, "GpuLight must stay std430-aligned");

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

/**
 * @param towardLight Points **at** the light, the opposite of `makeSpotLight`'s
 *        `direction`. That asymmetry is deliberate and is the one thing about these
 *        three functions worth reading twice.
 *
 * A sun is authored by saying where it is in the sky -- `render.sunDirection` and
 * `updateCascades` both mean "toward the light" -- while a spot is authored by saying
 * where it is aimed. Making both take the same sign would force one of the two callers
 * to negate at every site, and a negation at a call site is exactly the thing that
 * gets dropped. glTF sides with the spot: a KHR_lights_punctual light of any type
 * points down local -Z, so the importer negates once, in one place, with a comment.
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
 * @brief How much this light is worth spending a budget slot on (0.9, 0.10).
 *
 * Luminance over squared distance -- the irradiance the light would deliver to a
 * surface at `viewPosition`, up to a constant. The shading budget is the only budget
 * that ranks by it now; it ranked the punctual shadow atlas too until that went, and
 * the single metric is what kept the two from disagreeing about which lights matter.
 *
 * **Directional lights are infinitely important**, which is not a hedge: a sun has no
 * position for a distance to be measured from, and it lights the whole scene rather
 * than a neighbourhood. It is never the light a budget should drop.
 *
 * Two things it deliberately does not model, because a policy that is stated has to be
 * the policy that runs:
 *
 * - **A spot's cone.** A spot aimed away from the camera scores the same as one aimed
 *   at it. It is still lighting whatever it points at, and that may be most of what is
 *   on screen; discounting by the angle between the cone and the *viewer* would drop
 *   the light illuminating the wall you are looking at.
 * - **Range.** A light whose range does not reach `viewPosition` is not zeroed. The
 *   geometry it lights can be on screen while the camera stands outside its falloff.
 *
 * Both make the metric an approximation of on-screen contribution rather than a
 * measure of it. The honest version needs the light's screen-space extent, which is
 * 3.8's tiled assignment -- and 3.8 is delegated. This is what the engine does
 * until then, and it says so.
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
 * @brief Could this light affect anything inside the frustum? (C8)
 *
 * **Conservative and exact**, which is the property the whole row rests on: this returns
 * false only when the light's own volume provably cannot reach any visible surface, so
 * culling by it cannot change a single shaded pixel. That is what lets C8 be verified by
 * the golden set being byte-identical rather than by looking at it.
 *
 * Three cases, and the first two are the ones that matter:
 *
 * - A **directional** light is always visible. A sun has no position and no range; it
 *   lights everything.
 * - A light with **range 0 is unbounded** by the convention `GpuLight::position.w`
 *   states, and is likewise never culled. Getting this wrong would delete lights from a
 *   scene that authored them without a range.
 * - Everything else is a sphere of `range` about `position`, tested against all six
 *   planes. A spot is culled by that sphere rather than by its cone: the sphere bounds
 *   the cone, so the test stays conservative, and a cone-versus-frustum test is a good
 *   deal easier to get subtly wrong than it is to profit from.
 *
 * What this fixes is not a bug in `lightImportance` but the question it was answering.
 * The budget used to rank *every light in the scene* and keep the top N, so lights behind
 * the camera cost slots that lights in front of it wanted. Culling first makes the budget
 * a cap on lights that can affect this view, which is the semantics anyone would have
 * assumed it already had.
 */
[[nodiscard]] bool lightVisible(const GpuLight& light, const Frustum& frustum);

// `LightOverride` and `parseSceneLightOverrides` were here, reading one boolean --
// `castsShadows` -- out of `nodes[i].extras.substrate_light`. Both went with the shadow
// system, because the boolean's only effect was to write `kLightNeverShadowed` into a
// `params.w` that nothing reads any more, and a scene key that parses and does nothing
// is worse than one that is absent.
//
// The case it existed for is not fixed and will come back with shadows: **a point light
// inside the emissive mesh that represents it**. The mesh encloses the light, so every
// cube face records it at near-zero distance, the light illuminates nothing, and the
// mesh throws a large soft blob across the room. It reads as a shadow bug and is really
// a missing sentence in the schema. `scripts/make_composite_scene.py` authored it on the
// showcase orb for exactly that reason; restoring shadows means restoring both ends.

} // namespace gfx
