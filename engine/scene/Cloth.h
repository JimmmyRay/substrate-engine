#pragma once

#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/**
 * @file engine/scene/Cloth.h
 * @brief The authoring convention cloth arrives by, and the mesh maths around the solver
 *        (C19).
 *
 * ## What is here and what is not
 *
 * The solver is not here. A soft body is a body in the physics world -- that is the whole
 * reason C19 chose Jolt's over porting a compute shader across, because a cloth that
 * collides with the scene has to be *in* the thing that solves the scene -- so it lives in
 * `PhysicsWorld`, is created by `createCloth`, and is stepped by `PhysicsSystem::Update`
 * inside `PhysicsWorld::step` with no stepping code of its own. Splitting it into a second
 * translation unit would mean either promoting `PhysicsWorld::Impl` into a header, which
 * puts Jolt on the include path of everything that touches a scene and is exactly what
 * `Physics.h` refuses, or a type-erased hook, which is indirection over Vulkan's neighbour
 * for no gain.
 *
 * What is here is everything about cloth that is *not* the solver, and all of it is
 * arithmetic:
 *
 * - **The convention**, spelled once. `scripts/check_pins.py` holds the same two strings
 *   and the same threshold on the authoring side of the exporter and says in its comments
 *   that this file owns them.
 * - **The topology build.** A glTF mesh is not a simulation mesh, and `weldCloth` is the
 *   difference -- see below, it is the finding this row did not expect.
 * - **The normal recompute**, because Jolt returns positions and a G-buffer needs a basis.
 * - **`ClothSystem`**, the per-frame bookkeeping between the two: which instance a cloth
 *   drives, where its solved vertices go, and the one array the renderer copies.
 *
 * None of it names Jolt or Vulkan, so this translation unit joins
 * `SUBSTRATE_HOSTED_SOURCES` and runs under all four sanitizers -- which is the strongest
 * verification this board has and what a GPU solver would have forfeited.
 */
namespace scene {

class PhysicsWorld;

// ---------------------------------------------------------------- the convention
/**
 * @brief The mesh-name prefix that makes geometry cloth, and the attribute that pins it.
 *
 * **Spelled once, here, and read from here by everything in the engine that cares.** This
 * is the single thing C19 set out to copy from Tethered by *not* copying it: over there
 * the same predicate existed four times, three translation units plus a private copy in
 * the tests, so the tests tested their own copy and a change to the real one would not
 * have failed anything.
 *
 * `scripts/check_pins.py` carries the second copy and cannot avoid it -- it is a different
 * language on the other side of the exporter -- so its constants name this file, and the
 * two producers are checked against each other by `ClothTwoProducersAgree` in
 * `tests/ClothTests.cpp` rather than by anyone remembering.
 *
 * A name prefix runs against this engine's grain and that is a decision rather than an
 * oversight. Substrate's four authoring schemas are all glTF `extras`, read by one
 * rapidjson pass; `extras` is a per-node dictionary and has nowhere at all to put a value
 * per vertex, which is what a pin weight is. So cloth authors through a name and an
 * attribute, and `systems.md` records it beside the four as the exception it is.
 */
inline constexpr std::string_view kFabricPrefix = "FABRIC_";
/// The per-vertex float attribute, on the vertex domain, 1 pinned and 0 free. The leading
/// underscore is load-bearing: it is what makes Blender's stock glTF exporter pass a
/// custom mesh attribute through verbatim, and it is why no exporter patch is needed.
inline constexpr std::string_view kPinAttribute = "_PIN_WEIGHT";

/**
 * @brief Is this mesh fabric? The one predicate, and the tests call this one.
 *
 * `starts_with`, not equality and not "prefix plus something" -- because
 * `scripts/check_pins.py` uses Python's `str.startswith` and a bare `FABRIC_` is fabric
 * to it. Two producers of one convention disagreeing about the empty suffix is precisely
 * the kind of drift that convention exists to prevent, so the cheaper rule wins.
 *
 * Reads `asset.meshes[i].name` -- the glTF **mesh** name, which comes from Blender's mesh
 * data-block, not from the object. That distinction is not pedantic: renaming the object
 * and not the data-block is the trap `check_pins.py` gives its own refusal to, because
 * every other check passes and the engine still sees no cloth.
 */
[[nodiscard]] constexpr bool isFabricMesh(std::string_view meshName) {
    return meshName.substr(0, kFabricPrefix.size()) == kFabricPrefix;
}

/**
 * @brief The authoring weight a `_PIN_WEIGHT` attribute carries, as an inverse mass.
 *
 * `JPH::SoftBodySharedSettings::Vertex::mInvMass` is an inverse mass with zero meaning
 * pinned; the attribute is a weight with one meaning pinned. This is the one place the two
 * vocabularies meet, so nothing downstream has to remember which it is holding.
 *
 * **`>= 0.999` is pinned rather than `== 1.0f`**, and the slack is not superstition: a
 * weight that has been through a float attribute, an exporter and a base64 round trip is
 * not going to compare equal to one, and a curtain whose top edge came back as
 * `0.99999994` is a curtain on the floor. Anything below the threshold maps to
 * `1 - weight`, so a weight of zero is a vertex of unit inverse mass and needs no special
 * case, and a weight of 0.5 is a vertex that is heavy but mobile.
 *
 * Outside [0, 1] is clamped rather than refused, and NaN maps to free. `check_pins.py` is
 * what refuses a bad file, before the engine ever opens it; by the time the bytes are
 * here the useful thing to produce is a cloth.
 */
[[nodiscard]] constexpr float clothInvMass(float pinWeight) {
    if (!(pinWeight > 0.0f)) return 1.0f; // catches NaN too, which must not pin
    if (pinWeight >= 0.999f) return 0.0f;
    return 1.0f - pinWeight;
}

// ---------------------------------------------------------------- the topology
/**
 * @brief One `FABRIC_` primitive, as the loader read it, ready to become a soft body.
 *
 * Spans rather than vectors: every one of these views something the scene already owns,
 * and a copy per cloth of geometry that is not going to change would be a second copy of
 * the rest pose for nobody.
 */
struct ClothDesc {
    /// Rest pose, in the primitive's object space. `transform` is what puts it in the
    /// world.
    std::span<const Vertex> vertices;
    /// **Zero-based into `vertices`**, not the scene's absolute indices. The caller
    /// rebases; doing it here would mean this struct had to carry `baseVertex` as well as
    /// the span it describes, which is a value that can disagree with another.
    std::span<const ClothVertex> masses;
    std::span<const uint32_t> indices;
    /// Where the node hierarchy placed it. Baked into the vertices at build time -- see
    /// `ClothSystem::add`, which states why the node transform is ignored afterwards.
    glm::mat4 transform{1.0f};
};

/**
 * @brief A simulation mesh, welded, with the map back to the render mesh.
 *
 * **This is the thing C19's card did not predict, and without it the feature does not
 * work on a real asset.** A glTF vertex is a *shading* vertex: the exporter duplicates one
 * wherever a UV seam, a smoothing split or a material boundary needs two normals or two
 * texture coordinates at one place. A soft body built from those vertices has two
 * particles at the same point with no constraint between them, so the fabric tears itself
 * open along every seam on the first step -- silently, and looking exactly like a solver
 * bug.
 *
 * Blender's exporter splits a curtain's front and back faces the moment either has a
 * different normal, so this is not an exotic case; it is the ordinary one. The generated
 * `cloth.gltf` has no duplicates at all, which is precisely why it could not have caught
 * this and why the weld is tested directly instead.
 *
 * The weld is by **position only**, quantised, because that is the property that decides
 * whether two shading vertices are one piece of fabric. Normals and UVs deliberately do
 * not participate: two vertices at one point with opposite normals are the two sides of
 * one sheet, and they must move together.
 */
struct ClothTopology {
    /// One entry per welded particle: where it starts, in world space.
    std::vector<glm::vec3> positions;
    /// One entry per welded particle. The *minimum* inverse mass of the shading vertices
    /// that merged into it -- so a seam where one side was pinned and the other was not
    /// stays pinned. The alternative, averaging, invents a half-pinned vertex the author
    /// did not write.
    std::vector<float> invMasses;
    /// Triangles, as welded indices. Degenerate faces -- which a weld can create where the
    /// source had a sliver -- are dropped, because Jolt asserts on them.
    std::vector<uint32_t> faces;
    /// Render vertex -> welded particle. `positions[remap[i]]` is where render vertex `i`
    /// went, which is the whole of how a solved pose gets back onto a shading mesh.
    std::vector<uint32_t> remap;

    [[nodiscard]] bool empty() const { return positions.empty() || faces.empty(); }
};

/**
 * @brief Weld a primitive into a simulation mesh.
 *
 * Positions are transformed into world space *before* welding, not after, and the order
 * matters: two vertices that coincide in object space coincide in world space under any
 * transform, but the quantisation grid is a world-space grid, so welding first would give
 * a different answer for a scaled mesh than for the same mesh unscaled.
 *
 * @param desc the primitive. A `masses` span shorter than `vertices` leaves the remainder
 *        free, which is what a malformed `_PIN_WEIGHT` accessor comes to after the loader
 *        has clamped it -- and a cloth pinned nowhere is caught by `check_pins.py` at
 *        authoring time and by `ClothSystem::add` at load time, not here.
 */
[[nodiscard]] ClothTopology weldCloth(const ClothDesc& desc);

/**
 * @brief Recompute normals and re-orthogonalise tangents over a deformed mesh, in place.
 *
 * Jolt returns positions and nothing else, so this is the cost C19 predicted would be the
 * larger half and it is the reason the row was sized L. Area-weighted face normals
 * accumulated per vertex and normalised, which is the standard and is what makes a folded
 * curtain shade as a fold rather than as a crease.
 *
 * **Tangents are re-orthogonalised rather than rebuilt.** The rest-pose tangent came from
 * the UVs and the UVs do not move, so the useful thing to preserve is its direction:
 * Gram-Schmidt against the new normal, handedness in `w` untouched. Rebuilding one from an
 * arbitrary perpendicular would rotate every normal-mapped detail on the cloth as it swung.
 *
 * **`restTangents` is what it orthogonalises *from*, and passing the vertices' own current
 * tangents instead is a bug this row shipped and then caught.** Gram-Schmidt applied to its
 * own previous output is a path: a frame that ran one step and a frame that ran four reach
 * the same positions and *different* tangents, so the shading of a cloth would depend on
 * the frame rate. Against a fixed rest pose the answer is a pure function of the current
 * positions, which is what the fixed step promises everywhere else. An empty span means
 * "use whatever the vertices carry", which is only correct for a one-shot call.
 *
 * Runs once per cloth per *frame*, not per fixed step -- see `ClothSystem::update`. A
 * frame that ran four steps still shades one pose, so doing this inside the step would be
 * three-quarters wasted.
 */
void recomputeClothNormals(std::span<Vertex> vertices, std::span<const uint32_t> indices,
                           std::span<const glm::vec4> restTangents = {});

// ---------------------------------------------------------------- the bookkeeping
/**
 * @brief Every cloth in the scene, and the vertices the renderer copies to the GPU.
 *
 * A dense array with no handles, unlike `PhysicsWorld` and `InstanceTable`, and the reason
 * is that nothing creates or destroys a cloth after load: a `FABRIC_` primitive becomes
 * one soft body when the scene is placed and stops being one when the scene is unloaded.
 * A generation counter would be lifetime machinery guarding a lifetime nothing varies.
 */
class ClothSystem {
  public:
    /// What one cloth needs the renderer to know. Public because `Renderer` reads it every
    /// frame and a getter per field would be four getters for one struct.
    struct Cloth {
        /// The instance this cloth deforms. `Renderer::skinDestBase[slot]` is where its
        /// vertices land in the deformed vertex buffer.
        uint32_t instance = 0xFFFFFFFFu;
        /// The primitive it was built from, for the log line and for the inspector.
        uint32_t primitive = 0xFFFFFFFFu;
        /// The soft body in the physics world, as a raw slot rather than a `ClothId`, so
        /// this header needs no `Physics.h` -- the same property `Physics.h` keeps against
        /// Jolt, one layer out.
        uint32_t body = 0xFFFFFFFFu;
        /// The render mesh, updated in place every frame. Rest pose until the first
        /// `update`, which is what a cloth drawn on frame zero looks like.
        std::vector<Vertex> vertices;
        /// Render-mesh indices, zero-based, kept for the normal recompute.
        std::vector<uint32_t> indices;
        /// The rest pose's tangents, in world space, kept so that every frame's
        /// Gram-Schmidt starts from the same place -- see `recomputeClothNormals`. Sixteen
        /// bytes a vertex against a pose that would otherwise depend on the frame rate.
        std::vector<glm::vec4> restTangents;
        /// Welded particle per render vertex.
        std::vector<uint32_t> remap;
        /// World-space bounds of the last solved pose. **Computed and not yet used**: it
        /// is what `ClothTests.cpp`'s envelope property reads, and it is what a later row
        /// would cull on. Cloth inherits the infinite culling box every deformed instance
        /// has, and giving it a finite one means deciding what a skinned character's
        /// bounds are too -- a different problem, and a different card.
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        /// Largest distance any vertex moved in the last `update`, in metres. The
        /// convergence property is written against this, and it is also the cheapest
        /// possible answer to "is this cloth still moving".
        float lastMaxDisplacement = 0.0f;
    };

    /**
     * @brief Create a soft body for one primitive and start tracking it.
     *
     * @return false, having created nothing, when the primitive cannot be cloth: no
     *         welded faces, or no vertex pinned. **The second is refused rather than
     *         simulated**, and loudly, because a cloth pinned nowhere falls out of the
     *         world on frame one and looks like an engine bug rather than an authoring
     *         one. `check_pins.py` refuses the same case before the export leaves Blender;
     *         this is the same refusal for a file that never went through it.
     */
    bool add(PhysicsWorld& world, uint32_t instance, uint32_t primitive, const ClothDesc& desc);

    /**
     * @brief Read the solved pose back and reshade it. Once per frame, after the steps.
     *
     * **Not once per fixed step**, and that is a deliberate saving rather than an
     * approximation: a frame runs between zero and four steps, only the last one's pose is
     * ever drawn, and the normal recompute is the expensive half. A read per step would
     * pay for three poses nothing looks at.
     *
     * A no-op for an empty system, so the caller needs no test.
     */
    void update(const PhysicsWorld& world);

    /// Drop every cloth. The soft bodies belong to the world and go with it; this is the
    /// bookkeeping only, which is why it takes no world.
    void clear() { clothes.clear(); }

    [[nodiscard]] bool empty() const { return clothes.empty(); }
    [[nodiscard]] uint32_t count() const { return static_cast<uint32_t>(clothes.size()); }
    [[nodiscard]] const Cloth& at(uint32_t i) const { return clothes[i]; }
    /// Total vertices across every cloth. What the renderer sizes its staging buffer from.
    [[nodiscard]] uint32_t vertexCount() const;

  private:
    std::vector<Cloth> clothes;
    /// Solved particle positions, read out of the world once per cloth per frame. A member
    /// rather than a local because a local is an allocation per cloth per frame, and the
    /// pose-resolve row measured exactly that shape at 179 ns and 2.4% of the step. Grown
    /// to the largest cloth and never shrunk.
    std::vector<glm::vec3> scratch;
};

} // namespace scene
