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
 * @brief What a glTF file says about collision, before any solver sees it.
 *
 * Free of Jolt, and has to stay that way: `GltfScene` places a collider exactly as it places
 * a light, and a `JPH::` type here would put Jolt's headers on the include path of every
 * translation unit that touches a scene. The solver half is `Physics.h`.
 *
 * The extras key is `nodes[i].extras.substrate_collider`, read by a targeted rapidjson pass
 * rather than through fastgltf, which reaches extras only through a `simdjson::dom::object`
 * -- a header fastgltf downloads into its own `deps/` at configure time.
 */

/// What shape stands in for the node.
enum class ColliderShape : uint32_t {
    /// Derived from the motion type and the node's mesh -- see `resolvedShape`. The one case
    /// it cannot serve, a node with a collider and no mesh, is reported.
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
    /// A `CharacterVirtual` rather than a rigid body: a capsule tracked by the character
    /// solver, which can climb a step and refuse a slope where a pushed capsule cannot.
    Character,
};

/**
 * @brief Which degrees of freedom the solver may move a body through.
 *
 * Reaches Jolt's `EAllowedDOFs`, which zeroes the disallowed components of the inverse mass
 * and inertia: a confined body is never solved off its plane rather than corrected back onto
 * it. Ignored by a static body, which Jolt gives no motion properties to hold it.
 */
enum class ColliderFreedom : uint32_t {
    /// Three translations and three rotations. Jolt's default.
    All,
    /// **X and Y translation, Z rotation.** The one 2D plane, chosen to agree with the
    /// conventions that already exist: gravity is -Y, an orthographic camera looks down -Z,
    /// and a sprite's layer is its depth. A game wanting another rotates its world.
    Plane2D,
};

/**
 * @brief One collider, as `nodes[i].extras.substrate_collider` authored it.
 *
 * Every key is optional and every field has a working default, so a collider that names two
 * properties gets two properties and the engine's defaults for the rest.
 *
 * Placed by its glTF node, like a light and an emitter: the same collider under two
 * nodes is two bodies, and only the node knows where each one is.
 */
struct ColliderDesc {
    /// An alias of `scene::kNoNode` (scene/Node.h), for callers that spell it
    /// `ColliderDesc::kNoNode`.
    static constexpr uint32_t kNoNode = scene::kNoNode;

    std::string name;
    /// World placement, written by the scene's node walk. Translation and rotation become
    /// the body's; **scale reaches the shape**, because a Jolt body has none of its own.
    glm::mat4 transform{1.0f};
    /// glTF node that placed it, or `kNoNode` for a collider built in code. Also what binds
    /// a body to the instance it drives: the placements from this node are the ones whose
    /// transform it pushes.
    uint32_t node = kNoNode;

    ColliderShape shape = ColliderShape::Auto;
    ColliderMotion motion = ColliderMotion::Static;
    /// Which axes the solver may move it along, and around. Independent of `motion` -- a
    /// fifth motion value would make a kinematic 2D platform inexpressible.
    ColliderFreedom freedom = ColliderFreedom::All;

    /// Box half-extents.
    glm::vec3 halfExtent{0.5f};
    /// Sphere, capsule and cylinder radius.
    float radius = 0.5f;
    /// Half the length of a capsule's or cylinder's straight section, **excluding the
    /// caps** -- Jolt's convention, and the one place this schema follows the solver rather
    /// than an author's intuition. Authored as half the total height it is a radius too tall.
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
    /// m/s^2 toward a larger request and back down from it. Two rates because the forces
    /// differ -- traction limits getting going, friction limits stopping, and friction is
    /// the larger. A huge pair degenerates to assigning the velocity outright.
    float acceleration = 10.0f;
    float deceleration = 40.0f;
    /// The fraction of both rates that applies while the character is not standing. Below
    /// one, a jump carries the speed it launched with instead of dropping it.
    float airControl = 0.35f;
    /// **Fixed steps, never frames** -- a window counted in frames is a window whose length
    /// depends on the frame rate.
    ///
    /// `jumpBufferSteps` holds a press that arrived before the character could act on it;
    /// `coyoteSteps` keeps the launch available that long after the ground went away. The
    /// buffer is the longer on purpose: it only *delays* a jump the player asked for, where
    /// the coyote window *grants* one the world did not offer.
    uint32_t jumpBufferSteps = 10; ///< 1/6 s
    uint32_t coyoteSteps = 6;      ///< 1/10 s

    // Filled by the scene loader, never by the extras parser: the JSON says *which* shape,
    // the mesh says what it is made of. Both stay empty for the primitive shapes, so only a
    // hull or a mesh collider pays for a CPU copy of its geometry.

    /// Node-space vertex positions, for `Hull` and `Mesh`.
    std::vector<glm::vec3> points;
    /// Triangle indices into `points`, for `Mesh`. Empty for a hull, which needs none.
    std::vector<uint32_t> indices;

    /// What `Auto` means for this collider: a capsule for a character, a triangle mesh for
    /// anything that does not move, a convex hull for anything that does. A concave mesh has
    /// no inertia tensor and a hull is a poor floor, so neither default serves the other's
    /// case. One place, so the parser, the loader and the world cannot disagree.
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
 * The transform is left at identity and `node` carries the node index: placing a collider is
 * the scene's job, because the same collider under two nodes is two bodies.
 *
 * @return false when the bytes are not a glTF document at all. A document with no collider
 *         in it is not a failure.
 */
[[nodiscard]] bool parseSceneColliders(const rapidjson::Value& nodesArray, std::vector<ColliderDesc>& out);

/**
 * @brief The node-name suffix that authors collision without touching extras.
 *
 * A name convention rather than an `extras` key, because every DCC spells a custom property
 * differently and none of them round-trips it through a format conversion, where a node name
 * survives every exporter there is.
 *
 * A suffixed node is **not rendered**. Nothing anywhere loads a mesh and declines to draw
 * it, so the suppression is done by never making the placement -- see the node walk in
 * SceneParse.cpp.
 */
inline constexpr std::string_view kColliderSuffix = ".collider";

/**
 * @brief Does this node name author collision? The one predicate; the tests call this one.
 *
 * Reads the glTF **node** name, not the mesh name -- the opposite of `isFabricMesh`. A
 * collider is placed by its node because the same mesh under two nodes is two bodies, so the
 * node is the only thing that can carry the answer. A bare `.collider` counts.
 */
[[nodiscard]] constexpr bool isColliderNode(std::string_view nodeName) {
    return nodeName.size() >= kColliderSuffix.size() &&
           nodeName.substr(nodeName.size() - kColliderSuffix.size()) == kColliderSuffix;
}

/// The name each value carries in the JSON.
[[nodiscard]] const char* colliderShapeName(ColliderShape shape);
[[nodiscard]] const char* colliderMotionName(ColliderMotion motion);
[[nodiscard]] const char* colliderFreedomName(ColliderFreedom freedom);

} // namespace scene
