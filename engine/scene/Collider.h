#pragma once

#include "scene/Node.h"
#include <rapidjson/fwd.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scene {

/**
 * @file Collider.h
 * @brief What a glTF file says about collision, before any solver sees it (S4.2).
 *
 * Deliberately free of Jolt. `GltfScene` has to place a collider by its node's world
 * transform exactly as it places a light or an emitter, and it has no business knowing
 * what a `JPH::BodyID` is -- so the schema lives here and the solver lives in
 * `Physics.h`. That split is also what keeps Jolt's headers off the include path of
 * every translation unit that touches a scene.
 *
 * The extras key is `nodes[i].extras.substrate_collider`, read by the same targeted
 * rapidjson pass `substrate_emitter` is, and for the same reason: fastgltf reaches
 * extras only through a `simdjson::dom::object`, which would put a header fastgltf
 * downloads into its own `deps/` at configure time on this engine's include path.
 */

/// What shape stands in for the node.
enum class ColliderShape : uint32_t {
    /// Derived from the motion type and the node's mesh: a convex hull for anything that
    /// moves, a triangle mesh for anything that does not. The default, because it is the
    /// right answer often enough that stating it every time would be noise -- and the
    /// one case it cannot serve, a node with a collider and no mesh, is reported.
    Auto,
    Box,
    Sphere,
    Capsule,
    Cylinder,
    /// The convex hull of the node's mesh vertices.
    Hull,
    /// The node's triangles, exactly. Static and kinematic only: Jolt cannot give a
    /// concave mesh an inertia tensor, and a dynamic one is a body that cannot rotate.
    Mesh,
};

/// How the body is driven.
enum class ColliderMotion : uint32_t {
    /// Never moves. Cheapest, and what a floor, a wall and a ramp are.
    Static,
    /// Moved by something other than the solver -- an animation, a lift, a door. It
    /// pushes dynamic bodies and is pushed by nothing.
    Kinematic,
    /// Moved by the solver.
    Dynamic,
    /**
     * @brief A `CharacterVirtual` rather than a rigid body (S4.4).
     *
     * Not a rigid-body motion type and it does not pretend to be one. It is here rather
     * than in a schema of its own because a character is placed, sized, named and
     * budgeted exactly like every other collider, and the second schema that said so
     * would drift from this one the first time either changed. What differs is only what
     * gets built from it: a capsule tracked by the character solver, which can climb a
     * step and refuse a slope, neither of which a pushed capsule can do.
     */
    Character,
};

/**
 * @brief Which degrees of freedom the solver may move a body through (P7).
 *
 * A 2D game in this engine is a 3D world with the third axis taken away, not a second
 * solver -- Jolt is the only one here and there is no room for a `Box2D` beside it. What
 * makes that honest rather than a compromise is that the constraint is the solver's own:
 * `EAllowedDOFs` zeroes the disallowed components of the inverse mass and inertia, so a
 * confined body is not corrected back onto its plane after the fact, it is never solved
 * off it in the first place. Contacts, stacking, friction and the query surface are the
 * ones the 3D world already had.
 *
 * Read by a body, ignored by a static one -- a body the solver never moves has no
 * degrees of freedom to take away, and Jolt gives it no motion properties to hold them.
 */
enum class ColliderFreedom : uint32_t {
    /// Three translations and three rotations. Jolt's default, and what every collider
    /// authored before this existed gets.
    All,
    /// **X and Y translation, Z rotation.** The plane a 2D game is played in, chosen to
    /// agree with everything else here that already has a plane: gravity is -Y, an
    /// orthographic camera (P3) looks down -Z, and a sprite's layer (P4) is its depth. A
    /// second plane would be a second convention for those three to disagree with, so
    /// there is one, and a game that wants another rotates its world rather than the
    /// engine growing a switch.
    Plane2D,
};

/**
 * @brief One collider, as `nodes[i].extras.substrate_collider` authored it.
 *
 * Every key is optional and every field has a working default, for the reason `Config`
 * and `ParticleEmitter` both give: a collider that names two properties should get two
 * properties and the engine's defaults for the rest.
 *
 * Placed by its glTF node, like a light and an emitter: the same collider under two
 * nodes is two bodies, and only the node knows where each one is.
 */
struct ColliderDesc {
    /// Kept as a member for the callers that spell it `ColliderDesc::kNoNode`; the
    /// declaration itself is in scene/Node.h now (D3).
    static constexpr uint32_t kNoNode = scene::kNoNode;

    std::string name;
    /// World placement, written by the scene's node walk. Translation and rotation
    /// become the body's; scale is applied to the shape, because a Jolt body has no
    /// scale of its own.
    glm::mat4 transform{1.0f};
    /// glTF node that placed it, or `kNoNode` for a collider built in code. Also what
    /// binds a body to the instance it drives: the placements that came from this node
    /// are the ones whose transform it pushes.
    uint32_t node = kNoNode;

    ColliderShape shape = ColliderShape::Auto;
    ColliderMotion motion = ColliderMotion::Static;
    /// Which axes the solver may move it along, and around. Beside `motion` rather than a
    /// fifth value of it: "moved by the solver" and "moved in a plane" are two independent
    /// questions, and folding them together would make a kinematic 2D platform
    /// inexpressible.
    ColliderFreedom freedom = ColliderFreedom::All;

    /// Box half-extents.
    glm::vec3 halfExtent{0.5f};
    /// Sphere, capsule and cylinder radius.
    float radius = 0.5f;
    /// Half the length of a capsule's or cylinder's straight section, excluding the
    /// caps -- which is how Jolt measures it, and the one place this schema follows the
    /// solver's convention rather than an author's intuition. Stated here because a
    /// capsule authored as "half of the total height" is a capsule a radius too tall.
    float halfHeight = 0.5f;
    /// Shape centre in node space, so a capsule can stand on a node at its feet.
    glm::vec3 offset{0.0f};

    /// Kilograms. Zero means "derive from the shape", which is what Jolt does from its
    /// own density and is right for anything not being tuned by hand.
    float mass = 0.0f;
    float friction = 0.2f;
    float restitution = 0.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    /// Multiplier on world gravity. Zero floats, negative rises.
    float gravityFactor = 1.0f;

    // ------------------------------------------------------- character (S4.4, C20)
    /// How high a step the character walks up rather than into. Reaches Jolt's
    /// `mWalkStairsStepUp`, and the step-*down* is derived from it -- see
    /// `PhysicsWorld::step`.
    float stepHeight = 0.35f;
    /// Steeper than this and the character slides rather than stands, in radians. A face
    /// on the far side of it reports `CharacterGround::Sliding`, not `InAir`, and refuses
    /// a jump.
    float maxSlopeAngle = 0.87266463f; ///< 50 degrees
    /// Metres per second on the ground, and the vertical speed a jump starts with.
    float moveSpeed = 4.0f;
    float jumpSpeed = 4.5f;
    /// m/s^2 toward a larger request and back down from it. The velocity *ramps* rather
    /// than arriving, which is what makes a speed between nothing and `moveSpeed` exist at
    /// all -- and the two rates are separate because the forces are: getting going is
    /// limited by traction, stopping is limited by friction, and friction is the larger.
    /// A huge pair reproduces the assignment this replaced.
    float acceleration = 10.0f;
    float deceleration = 40.0f;
    /// The fraction of both rates that applies while the character is not standing. Below
    /// one, a jump carries the speed it launched with instead of dropping it -- which is
    /// the "momentum instead" a game used to have to edit `engine/` for.
    float airControl = 0.35f;
    /// **Fixed steps, never frames.** A window measured in frames is a window whose size
    /// depends on the frame rate, which is the defect these two exist to remove.
    ///
    /// `jumpBufferSteps` holds a press that arrived before the character could act on it;
    /// `coyoteSteps` keeps the launch available for that many steps after the ground went
    /// away. The buffer is the longer of the two on purpose: it only ever *delays* a jump
    /// the player asked for, where the coyote window *grants* one the world did not offer.
    uint32_t jumpBufferSteps = 10; ///< 1/6 s
    uint32_t coyoteSteps = 6;      ///< 1/10 s

    // -------------------------------------------------------------- geometry
    // Filled by the scene loader, never by the extras parser: the JSON says *which*
    // shape, the mesh says what it is made of. Both stay empty for the primitive shapes,
    // so only a hull or a mesh collider pays for a CPU copy of its geometry.

    /// Node-space vertex positions, for `Hull` and `Mesh`.
    std::vector<glm::vec3> points;
    /// Triangle indices into `points`, for `Mesh`. Empty for a hull, which needs none.
    std::vector<uint32_t> indices;

    /// What `Auto` means for this collider: a capsule for a character, a triangle mesh
    /// for anything that does not move, a convex hull for anything that does. A concave
    /// mesh has no inertia tensor and a hull is a poor floor, so neither default serves
    /// the other's case. Resolved in one place so the parser, the loader and the world
    /// cannot disagree about it.
    [[nodiscard]] ColliderShape resolvedShape() const {
        if (shape != ColliderShape::Auto) return shape;
        if (motion == ColliderMotion::Character) return ColliderShape::Capsule;
        return motion == ColliderMotion::Static ? ColliderShape::Mesh : ColliderShape::Hull;
    }

    /// True when the loader must hand this collider its node's geometry. Asked of the
    /// *resolved* shape, so an `Auto` character -- which is a capsule and needs no mesh
    /// at all -- does not go looking for one.
    [[nodiscard]] bool needsGeometry() const {
        const ColliderShape s = resolvedShape();
        return s == ColliderShape::Hull || s == ColliderShape::Mesh;
    }
};

/**
 * @brief Read every `nodes[i].extras.substrate_collider` out of a glTF document.
 *
 * Takes the document's bytes rather than a path or a parsed asset, which is what makes
 * it testable without a device, a file or fastgltf. `.glb` is handled by unwrapping its
 * JSON chunk, so a caller does not have to know which it has.
 *
 * The transform is left at identity and `node` carries the node index: placing a
 * collider is the scene's job, because the same collider under two nodes is two bodies.
 *
 * @return false when the bytes are not a glTF document at all. A document with no
 *         collider in it is not a failure, it is Sponza.
 */
[[nodiscard]] bool parseSceneColliders(const rapidjson::Value& nodesArray, std::vector<ColliderDesc>& out);

/**
 * @brief The node-name suffix that authors collision without touching extras.
 *
 * A name convention runs against this engine's grain, and that is a decision rather than
 * an oversight -- the same one `Cloth.h`'s `FABRIC_` prefix records. `extras` is written
 * by a custom property per object, which every DCC spells differently and none of them
 * round-trips through a file format conversion; a node *name* survives every exporter
 * there is. Collision is the one authoring schema whose author is usually not the person
 * who knows what `substrate_collider` is, so it gets the door that needs no glossary.
 *
 * A suffixed node is **not rendered**: it is collision, and the visual mesh it duplicates
 * is a separate node. That half is not decoration -- there is no flag anywhere that loads
 * a mesh and declines to draw it, so it is done by never making the placement.
 */
inline constexpr std::string_view kColliderSuffix = ".collider";

/**
 * @brief Does this node name author collision? The one predicate; the tests call this one.
 *
 * Reads the glTF **node** name, not the mesh name -- the opposite of `isFabricMesh`, and
 * the difference is not pedantic. A collider is placed by its node because the same mesh
 * under two nodes is two bodies, so the node is the only thing that can carry the answer.
 *
 * A bare `.collider` with nothing in front of it is still collision. Requiring a stem
 * would make the empty case disagree with the obvious reading of the rule for no gain.
 */
[[nodiscard]] constexpr bool isColliderNode(std::string_view nodeName) {
    return nodeName.size() >= kColliderSuffix.size() &&
           nodeName.substr(nodeName.size() - kColliderSuffix.size()) == kColliderSuffix;
}

/// The name in the JSON for each shape, and the parse of it. Exposed because the test
/// that checks every spelling round-trips should not have to spell them twice.
[[nodiscard]] const char* colliderShapeName(ColliderShape shape);
[[nodiscard]] const char* colliderMotionName(ColliderMotion motion);
[[nodiscard]] const char* colliderFreedomName(ColliderFreedom freedom);

} // namespace scene
