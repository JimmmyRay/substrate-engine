#include "physics/PhysicsWorld.h"

#include "core/Logger.h"
#include "core/Profiler.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/RegisterTypes.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace physics {

namespace {

/// Object layers. The tables below take a count, so a third layer is this number plus the
/// `EnableCollision` pairs that layer takes part in.
constexpr JPH::ObjectLayer kLayerStatic = 0;
constexpr JPH::ObjectLayer kLayerMoving = 1;
constexpr uint32_t kObjectLayerCount = 2;

/// Where `Character::airSteps` saturates. It is only ever compared against a coyote window,
/// so the count past that window carries no information and must not be allowed to wrap.
constexpr uint32_t kAirStepsMax = 1u << 20;

/// Broad-phase layers. Separate from the object layers -- many object layers can share one
/// broad-phase tree -- and only incidentally the same count.
constexpr JPH::BroadPhaseLayer kBroadPhaseStatic(0);
constexpr JPH::BroadPhaseLayer kBroadPhaseMoving(1);
constexpr uint32_t kBroadPhaseLayerCount = 2;

/// Headroom over the colliders a scene declared, when no budget names a ceiling. Enough for a
/// game to spawn a magazine of debris without a rebuild, small enough that a scene with four
/// colliders does not allocate for a thousand.
constexpr uint32_t kBodyHeadroom = 256;

/// Jolt asks for these at init and they scale with the body count. The floors are Jolt's own
/// sample values, the smallest numbers known to behave.
uint32_t bodyPairsFor(uint32_t maxBodies) { return std::max(1024u, maxBodies * 8u); }
uint32_t contactConstraintsFor(uint32_t maxBodies) { return std::max(1024u, maxBodies * 4u); }

/// Jolt's temp allocator is a stack, sized once and held for the process. 16 MB is Jolt's own
/// sample figure and covers a few thousand bodies.
constexpr uint32_t kTempAllocatorBytes = 16u * 1024u * 1024u;

glm::vec3 toGlm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat toGlm(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }
JPH::Vec3 toJolt(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJolt(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

/// Route Jolt's diagnostics into the engine's log, not stdout, where a headless golden run
/// would mix them into the capture script's output.
///
/// The formatted message goes to the `std::string` overload, never through `"Jolt: %s"`, so a
/// diagnostic containing a percent sign is not formatted twice.
void traceToLogger(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string msg = core::Logger::vformat(format, args);
    va_end(args);
    core::Logger::debug(core::LogCategory::Scene, "Jolt: " + msg);
}

/**
 * @brief Jolt's process-wide setup, done once and undone at exit.
 *
 * A static local rather than a call in `PhysicsWorld::init`: the tests construct several
 * worlds and `RegisterTypes` is not idempotent. The destructor matters as much: without it the
 * factory is a real leak ASan reports on every run of the suite.
 */
struct JoltRuntime {
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = traceToLogger;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRuntime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
};

void ensureJoltRuntime() { static JoltRuntime runtime; }

/// Split a world matrix for a body. Scale comes back separately because it belongs to the
/// shape, not the body, and dropping it silently shrinks or grows a collider.
void decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    translation = glm::vec3(m[3]);
    scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
    glm::mat3 r(m);
    // Substituting 1 for a zero axis keeps the quaternion finite; dividing by the raw scale
    // makes it NaN and every transform built from it after that.
    for (int i = 0; i < 3; ++i) {
        const float s = scale[i] > 1e-8f ? scale[i] : 1.0f;
        r[i] /= s;
    }
    rotation = glm::normalize(glm::quat_cast(r));
}

} // namespace

glm::mat4 interpolateState(const PhysicsState& a, const PhysicsState& b, float alpha) {
    const glm::vec3 p = glm::mix(a.position, b.position, alpha);
    const glm::quat r = glm::slerp(a.rotation, b.rotation, alpha);
    return glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r);
}

#ifdef JPH_DEBUG_RENDERER
/**
 * @brief Jolt's debug output, as line vertices.
 *
 * Deriving is the only way to get a convex hull's or triangle mesh's wireframe: those shapes
 * carry no parameters a procedural outline could be built from. `DrawLine` and `DrawText3D`
 * are the base's two pure virtuals; `DrawTriangle` already falls back to three `DrawLine`s.
 */
struct PhysicsDebugRenderer final : public JPH::DebugRendererSimple {
    std::vector<gfx::DebugLineVertex>* out = nullptr;

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override {
        if (out == nullptr) return;
        // Jolt's Color is already 0xAABBGGRR in memory, the layout `DebugLineVertex`
        // documents; swizzling here would double-convert it.
        const uint32_t packed = color.mU32;
        out->push_back({glm::vec3(from.GetX(), from.GetY(), from.GetZ()), packed});
        out->push_back({glm::vec3(to.GetX(), to.GetY(), to.GetZ()), packed});
    }

    /// Stubbed: pure in the base, and `recordOverlay` draws the engine's own glyphs.
    void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override {}
};
#else
struct PhysicsDebugRenderer {};
#endif

namespace {

/**
 * @brief Records what touched during a step, and does nothing else.
 *
 * Jolt calls this from inside `Update`, on job threads, with every body in the world locked.
 * Creating a body, destroying one or taking a body lock from here deadlocks the step, so the
 * callback may only write a POD into a vector; `PhysicsWorld::collectContacts` turns that into
 * something a game can act on once the step is over.
 *
 * Resolving `JPH::BodyID` to a slot belongs in that drain, not here -- it is a hash lookup per
 * contact, and doing it here puts it on the locked path and on N threads for no saving, since
 * the drain walks the list anyway.
 */
struct ContactRecorder final : public JPH::ContactListener {
    /// One manifold, in Jolt's terms.
    struct Raw {
        uint32_t body1 = 0; ///< JPH::BodyID's raw value
        uint32_t body2 = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float speed = 0.0f;
    };

    /// Held unconditionally, not only when `workerThreads` is non-zero: a lock that appears
    /// when a config key changes is a lock nobody ever tests. Uncontended at the default,
    /// where the callbacks arrive on the stepping thread.
    std::mutex lock;
    std::vector<Raw> found;

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        const auto count = static_cast<uint32_t>(manifold.mRelativeContactPointsOn1.size());
        if (count == 0) return;

        // The centroid, not point zero: a box landing flat gives four points in the clipper's
        // order, so the first one moves between two runs of the same scene. The relative
        // points are summed before the base offset is added, which is what stays exact when
        // the world origin is far away.
        JPH::Vec3 sum = JPH::Vec3::sZero();
        for (uint32_t i = 0; i < count; ++i) sum += manifold.mRelativeContactPointsOn1[i];
        const JPH::RVec3 point = manifold.mBaseOffset + sum / static_cast<float>(count);

        // Velocities as they were before the solver ran; this callback is the only place they
        // can be read. A static body reports zero, so a crate hitting a floor gets the crate's
        // speed rather than nothing.
        const JPH::Vec3 relative = body1.GetPointVelocity(point) - body2.GetPointVelocity(point);

        Raw raw;
        raw.body1 = body1.GetID().GetIndexAndSequenceNumber();
        raw.body2 = body2.GetID().GetIndexAndSequenceNumber();
        raw.point = toGlm(JPH::Vec3(point));
        raw.normal = toGlm(manifold.mWorldSpaceNormal);
        // Jolt's normal points from body 1 to body 2, so closing gives a positive dot.
        // Negative is a speculative contact caught while the two still separate: a real
        // contact whose impact is zero, not a value to drop.
        raw.speed = std::max(0.0f, relative.Dot(manifold.mWorldSpaceNormal));

        const std::lock_guard<std::mutex> held(lock);
        found.push_back(raw);
    }
};

} // namespace

/// Declaration order is teardown order reversed: `system` is destroyed last, after the layer
/// tables, the allocator and the job system it holds references to.
struct PhysicsWorld::Impl {
    JPH::PhysicsSystem system;
    std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> broadPhase;
    std::unique_ptr<JPH::ObjectLayerPairFilterTable> objectPairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystem> jobs;
    /// `CharacterVirtual` is reference counted, and holding a `Ref` is how Jolt expects
    /// one to be owned.
    std::vector<JPH::Ref<JPH::CharacterVirtual>> characters;
    std::unique_ptr<PhysicsDebugRenderer> debug;
    bool initialised = false;

    /// `JPH::BodyID`'s raw value to the index this class hands out. A map rather than a scan
    /// of `bodies`, which would be O(bodies) *per hit* on a surface built to be called. Jolt's
    /// own body user data cannot serve: `addBody` already stores the caller's `userData` there.
    std::unordered_map<uint32_t, uint32_t> bodyIndexById;

    /// By value, because the system stores a bare pointer to it and this holder outlives the
    /// system it sits beside.
    ContactRecorder contacts;
};

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() { shutdown(); }

void PhysicsWorld::init(const scene::PhysicsConfig& cfg, uint32_t expectedBodies) {
    auto zone = core::Profiler::scope("PhysicsWorld::init");
    shutdown();
    ensureJoltRuntime();

    config = cfg;
    if (config.step <= 0.0f) config.step = 1.0f / 60.0f;
    if (config.collisionSteps == 0) config.collisionSteps = 1;

    // A floor, not a ceiling: `grow()` rebuilds the world when a create outruns this, so the
    // number decides only how much is allocated first. A stated budget is taken at its word
    // and raised only to what the scene already declares; the headroom is for the derived
    // case, where guessing tight means rebuilding during load.
    createSystem(config.bodyBudget != 0 ? std::max(config.bodyBudget, expectedBodies)
                                        : expectedBodies + kBodyHeadroom);
}

void PhysicsWorld::createSystem(uint32_t bodyCapacity) {
    capacity = std::max(bodyCapacity, 1u);

    impl = std::make_unique<Impl>();

    impl->broadPhase = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(kObjectLayerCount, kBroadPhaseLayerCount);
    impl->broadPhase->MapObjectToBroadPhaseLayer(kLayerStatic, kBroadPhaseStatic);
    impl->broadPhase->MapObjectToBroadPhaseLayer(kLayerMoving, kBroadPhaseMoving);

    impl->objectPairs = std::make_unique<JPH::ObjectLayerPairFilterTable>(kObjectLayerCount);
    // Static against static stays disabled: two things that never move cannot begin to
    // overlap, so enabling it buys pairs that can never fire.
    impl->objectPairs->EnableCollision(kLayerStatic, kLayerMoving);
    impl->objectPairs->EnableCollision(kLayerMoving, kLayerMoving);

    impl->objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        *impl->broadPhase, kBroadPhaseLayerCount, *impl->objectPairs, kObjectLayerCount);

    impl->temp = std::make_unique<JPH::TempAllocatorImpl>(kTempAllocatorBytes);
    if (config.workerThreads == 0) {
        // The determinism default -- see the header.
        impl->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    } else {
        // Default-constructed then `Init`ed, not built with the convenience constructor: that
        // one starts the threads, and `SetThreadInitFunction` has to be in place before they
        // run or Jolt's workers never name their profiler tracks.
        auto pool = std::make_unique<JPH::JobSystemThreadPool>();
        pool->SetThreadInitFunction([](int) { core::Profiler::nameThread("physics job"); });
        pool->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(config.workerThreads));
        impl->jobs = std::move(pool);
    }

    impl->system.Init(capacity, 0, bodyPairsFor(capacity), contactConstraintsFor(capacity), *impl->broadPhase,
                      *impl->objectVsBroadPhase, *impl->objectPairs);
    impl->system.SetGravity(toJolt(config.gravity));
    // Installed unconditionally. Gating it on a "does anybody want contacts" flag costs a game
    // that sets the flag after `init` every contact, silently; the listener costs a scene
    // nobody listens to one `push_back` per new manifold.
    impl->system.SetContactListener(&impl->contacts);
    impl->initialised = true;
}

void PhysicsWorld::shutdown() {
    bodies.clear();
    characters.clear();
    freeBodySlots.clear();
    freeCharacterSlots.clear();
    pendingBodyRemoval.clear();
    pendingCharacterRemoval.clear();
    previous.clear();
    current.clear();
    previousCharacters.clear();
    currentCharacters.clear();
    stepContacts.clear();
    clothes.clear();
    refused = 0;
    capacity = 0;
    impl.reset();
}

namespace {

/**
 * Solver settings for a soft body, stated rather than defaulted. All three guard the same
 * failure -- fabric that bounces, twitches and swings forever -- and the envelope property in
 * `tests/ClothTests.cpp` is what catches a regression in any of them.
 *
 * - Iterations above Jolt's default of 5: too few leaves corrections that never converge.
 * - Damping applied to the *velocity*. A velocity threshold instead buys a quiet cloth at the
 *   price of one that never starts falling.
 * - A vertex radius, so a particle sits slightly off a surface. Jolt's default of zero
 *   z-fights against whatever the cloth rests on.
 */
constexpr uint32_t kClothSolverIterations = 10;
constexpr float kClothLinearDamping = 2.0f;
constexpr float kClothVertexRadius = 0.01f;

} // namespace

uint32_t PhysicsWorld::createCloth(const scene::ClothTopology& topology) {
    if (impl == nullptr || !impl->initialised) return kNoCloth;
    if (topology.positions.empty() || topology.faces.size() < 3) return kNoCloth;
    // Soft bodies share the rigid bodies' budget, so the held count must include both.
    if (const uint32_t held = static_cast<uint32_t>(bodies.size() + clothes.size());
        held >= capacity && !grow(held + 1)) {
        ++refused;
        return kNoCloth;
    }

    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    shared->mVertices.reserve(topology.positions.size());
    for (size_t i = 0; i < topology.positions.size(); ++i) {
        const glm::vec3& p = topology.positions[i];
        JPH::SoftBodySharedSettings::Vertex v;
        v.mPosition = JPH::Float3(p.x, p.y, p.z);
        // `ClothVertex::invMass` is already Jolt's convention, zero pinned -- see
        // `clothInvMass`, which is where the authoring weight is converted.
        v.mInvMass = topology.invMasses[i];
        shared->mVertices.push_back(v);
    }
    for (size_t i = 0; i + 2 < topology.faces.size(); i += 3) {
        JPH::SoftBodySharedSettings::Face f;
        f.mVertex[0] = topology.faces[i];
        f.mVertex[1] = topology.faces[i + 1];
        f.mVertex[2] = topology.faces[i + 2];
        shared->AddFace(f);
    }

    // One `VertexAttributes` for the whole sheet: Jolt repeats the last element when the list
    // is shorter than the vertex array, so a uniform fabric needs exactly one.
    JPH::SoftBodySharedSettings::VertexAttributes attributes;
    // Zero compliance is inextensible, which is what fabric is at this scale; a curtain that
    // visibly stretches under its own weight is rubber.
    attributes.mCompliance = 0.0f;
    attributes.mShearCompliance = 0.0f;
    // `FLT_MAX` is bend constraints *off*, and it is Jolt's default. Zero -- the obvious
    // reading of "stiff everywhere" -- makes them infinitely stiff and turns a nine-by-nine
    // sheet into a plate the Gauss-Seidel solver cannot satisfy: it hangs in the right place
    // and twitches two millimetres a step forever, and raising the iteration count makes it
    // worse, because more iterations of an unsatisfiable system is more energy.
    attributes.mBendCompliance = FLT_MAX;
    // The long-range attachment constraint, measured along the edges, caps how far a vertex
    // may get from the nearest pinned one. Without it a correction propagates one edge per
    // iteration, so the far corner of a sheet learns about its pin several frames late and
    // travels while it waits.
    attributes.mLRAType = JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance;
    attributes.mLRAMaxDistanceMultiplier = 1.0f;
    shared->CreateConstraints(&attributes, 1);
    shared->Optimize();

    // Identity, because `weldCloth` already put the vertices in world space. Placing the body
    // as well would apply the node transform twice.
    JPH::SoftBodyCreationSettings settings(shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), kLayerMoving);
    settings.mNumIterations = kClothSolverIterations;
    settings.mLinearDamping = kClothLinearDamping;
    settings.mVertexRadius = kClothVertexRadius;
    // The body's own position must not drift out from under vertices that are already
    // world-space; `clothPositions` adds this origin back and assumes it has not moved.
    settings.mUpdatePosition = false;
    settings.mMakeRotationIdentity = false;

    JPH::BodyInterface& bi = impl->system.GetBodyInterface();
    JPH::Body* body = bi.CreateSoftBody(settings);
    if (body == nullptr) {
        ++refused;
        return kNoCloth;
    }
    bi.AddBody(body->GetID(), JPH::EActivation::Activate);

    const uint32_t index = static_cast<uint32_t>(clothes.size());
    clothes.push_back({body->GetID().GetIndexAndSequenceNumber(), static_cast<uint32_t>(topology.positions.size())});
    return index;
}

uint32_t PhysicsWorld::clothParticleCount(uint32_t cloth) const {
    return cloth < clothes.size() ? clothes[cloth].particles : 0u;
}

void PhysicsWorld::clothPositions(uint32_t cloth, std::span<glm::vec3> out) const {
    if (impl == nullptr || cloth >= clothes.size() || out.empty()) return;

    // A read lock: the vertices are motion-property state, and no `BodyInterface` call hands
    // them over. Safe only between steps, which is where the caller runs.
    const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(clothes[cloth].id));
    if (!lock.Succeeded()) return;
    const JPH::Body& body = lock.GetBody();
    if (!body.IsSoftBody()) return;

    const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    const JPH::RVec3 origin = body.GetCenterOfMassPosition();
    const size_t n = std::min(out.size(), static_cast<size_t>(motion->GetVertices().size()));
    for (size_t i = 0; i < n; ++i) {
        // Jolt keeps a soft body's vertices relative to its centre of mass, so the origin has
        // to be added back even though `mUpdatePosition` is false and it never moves.
        const JPH::Vec3 p = motion->GetVertex(static_cast<uint32_t>(i)).mPosition;
        out[i] = glm::vec3(static_cast<float>(origin.GetX()) + p.GetX(), static_cast<float>(origin.GetY()) + p.GetY(),
                           static_cast<float>(origin.GetZ()) + p.GetZ());
    }
}

namespace {

// GCC 12 reports `result.GetError()` below as reading an uninitialised `Ref<Shape>`. False
// positive: `JPH::Result` is a tagged union whose members share storage, the error string is
// read only after `HasError()`, and the compiler tracks the offset rather than the tag. Keep
// the push/pop around these two functions only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

/// Build the shape a collider describes, in node space and before the node's scale.
/// Returns null when the description cannot produce one, having said why.
JPH::RefConst<JPH::Shape> makeShape(const scene::ColliderDesc& desc) {
    JPH::ShapeSettings::ShapeResult result;

    switch (desc.resolvedShape()) {
    case scene::ColliderShape::Box: {
        const glm::vec3 he = glm::max(desc.halfExtent, glm::vec3(1e-3f));
        // The convex radius has to fit inside the box or Jolt refuses the shape, and its
        // default of 0.05 is larger than a small crate's half-extent.
        const float radius = std::min(0.05f, std::min({he.x, he.y, he.z}) * 0.5f);
        result = JPH::BoxShapeSettings(toJolt(he), radius).Create();
        break;
    }
    case scene::ColliderShape::Sphere:
        result = JPH::SphereShapeSettings(std::max(desc.radius, 1e-3f)).Create();
        break;
    case scene::ColliderShape::Capsule:
        result = JPH::CapsuleShapeSettings(std::max(desc.halfHeight, 1e-3f), std::max(desc.radius, 1e-3f)).Create();
        break;
    case scene::ColliderShape::Cylinder:
        result = JPH::CylinderShapeSettings(std::max(desc.halfHeight, 1e-3f), std::max(desc.radius, 1e-3f)).Create();
        break;
    case scene::ColliderShape::Hull: {
        if (desc.points.empty()) {
            core::Logger::warn(core::LogCategory::Scene, "Collider '%s': a hull needs the node's mesh, and there is none",
                         desc.name.c_str());
            return nullptr;
        }
        JPH::Array<JPH::Vec3> points;
        points.reserve(desc.points.size());
        for (const glm::vec3& p : desc.points) points.push_back(toJolt(p));
        result = JPH::ConvexHullShapeSettings(points).Create();
        break;
    }
    case scene::ColliderShape::Mesh: {
        if (desc.points.empty() || desc.indices.size() < 3) {
            core::Logger::warn(core::LogCategory::Scene, "Collider '%s': a mesh shape needs the node's triangles, and there are none",
                         desc.name.c_str());
            return nullptr;
        }
        JPH::VertexList vertices;
        vertices.reserve(desc.points.size());
        for (const glm::vec3& p : desc.points) vertices.push_back(JPH::Float3(p.x, p.y, p.z));

        JPH::IndexedTriangleList triangles;
        triangles.reserve(desc.indices.size() / 3);
        for (size_t i = 0; i + 2 < desc.indices.size(); i += 3) {
            triangles.push_back(JPH::IndexedTriangle(desc.indices[i], desc.indices[i + 1], desc.indices[i + 2], 0));
        }
        result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
        break;
    }
    case scene::ColliderShape::Auto:
        // Unreachable: `resolvedShape()` never returns it. Listed rather than defaulted so a
        // shape added to the enum fails to compile here instead of falling through.
        return nullptr;
    }

    if (result.HasError()) {
        core::Logger::warn(core::LogCategory::Scene, "Collider '%s': %s", desc.name.c_str(), result.GetError().c_str());
        return nullptr;
    }

    JPH::RefConst<JPH::Shape> shape = result.Get();

    if (desc.offset != glm::vec3(0.0f)) {
        JPH::ShapeSettings::ShapeResult offsetResult =
            JPH::RotatedTranslatedShapeSettings(toJolt(desc.offset), JPH::Quat::sIdentity(), shape).Create();
        if (offsetResult.HasError()) {
            core::Logger::warn(core::LogCategory::Scene, "Collider '%s': %s", desc.name.c_str(), offsetResult.GetError().c_str());
            return nullptr;
        }
        shape = offsetResult.Get();
    }
    return shape;
}

/// Apply a node's scale to a shape, correcting one the shape cannot take. Jolt refuses a
/// non-uniformly scaled sphere outright, so not correcting it is a body that fails to exist.
JPH::RefConst<JPH::Shape> scaleShape(const JPH::RefConst<JPH::Shape>& shape, const glm::vec3& scale,
                                     const std::string& name) {
    if (std::abs(scale.x - 1.0f) < 1e-5f && std::abs(scale.y - 1.0f) < 1e-5f && std::abs(scale.z - 1.0f) < 1e-5f) {
        return shape;
    }

    JPH::Vec3 s = toJolt(scale);
    if (!shape->IsValidScale(s)) {
        const JPH::Vec3 fixed = shape->MakeScaleValid(s);
        core::Logger::warn(core::LogCategory::Scene,
                     "Collider '%s': scale (%.3f %.3f %.3f) is not one this shape can take -- using (%.3f %.3f %.3f)",
                     name.c_str(), scale.x, scale.y, scale.z, fixed.GetX(), fixed.GetY(), fixed.GetZ());
        s = fixed;
    }

    JPH::ShapeSettings::ShapeResult result = JPH::ScaledShapeSettings(shape, s).Create();
    if (result.HasError()) {
        core::Logger::warn(core::LogCategory::Scene, "Collider '%s': %s", name.c_str(), result.GetError().c_str());
        return shape;
    }
    return result.Get();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace

scene::BodyId PhysicsWorld::createBody(const scene::ColliderDesc& desc, uint64_t userData) {
    if (impl == nullptr || !impl->initialised) return {};
    // Refused, not routed to `createCharacter`: that would hand back a `scene::BodyId` naming
    // something that is not a body.
    if (desc.motion == scene::ColliderMotion::Character) return {};

    // Grown rather than refused, which is what keeps `refused` counting only what Jolt itself
    // would not give -- a shape it could not build, or its own index ceiling.
    if (bodies.size() >= capacity && !grow(static_cast<uint32_t>(bodies.size()) + 1)) {
        ++refused;
        return {};
    }

    JPH::RefConst<JPH::Shape> shape = makeShape(desc);
    if (shape == nullptr) return {};

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(desc.transform, translation, rotation, scale);
    shape = scaleShape(shape, scale, desc.name);

    const bool moves = desc.motion != scene::ColliderMotion::Static;
    const JPH::EMotionType motion = desc.motion == scene::ColliderMotion::Dynamic  ? JPH::EMotionType::Dynamic
                                    : desc.motion == scene::ColliderMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                               : JPH::EMotionType::Static;

    JPH::BodyCreationSettings settings(shape, toJolt(translation), toJolt(rotation), motion,
                                       moves ? kLayerMoving : kLayerStatic);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linearDamping;
    settings.mAngularDamping = desc.angularDamping;
    settings.mGravityFactor = desc.gravityFactor;
    settings.mUserData = userData;
    // Jolt zeroes the disallowed rows of the inverse mass and inertia, so a confined body is
    // never solved off its plane and there is nothing to correct afterwards. A static body has
    // no motion properties to hold it and Jolt ignores it there.
    settings.mAllowedDOFs =
        desc.freedom == scene::ColliderFreedom::Plane2D ? JPH::EAllowedDOFs::Plane2D : JPH::EAllowedDOFs::All;
    if (desc.mass > 0.0f) {
        // `CalculateInertia`, so the tensor still comes from the shape; overriding both would
        // need an author to write a 3x3 matrix by hand.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    }

    JPH::BodyInterface& bi = impl->system.GetBodyInterface();
    JPH::Body* body = bi.CreateBody(settings);
    if (body == nullptr) {
        ++refused;
        return {};
    }
    bi.AddBody(body->GetID(), moves ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

    const uint32_t raw = body->GetID().GetIndexAndSequenceNumber();

    // A retired slot before a new one. Slots are never compacted -- callers key off a body
    // index -- so reuse is what keeps a world that spawns and despawns bounded.
    uint32_t index;
    if (!freeBodySlots.empty()) {
        index = freeBodySlots.back();
        freeBodySlots.pop_back();
        Body& slot = bodies[index];
        // `destroy()` already moved the generation; bumping it again here would burn a second
        // generation per lifetime for no extra staleness detection.
        slot.live = true;
        slot.id = raw;
        slot.userData = userData;
        slot.moves = moves;
        slot.kinematic = desc.motion == scene::ColliderMotion::Kinematic;
    } else {
        index = static_cast<uint32_t>(bodies.size());
        Body slot;
        slot.id = raw;
        slot.userData = userData;
        slot.moves = moves;
        slot.kinematic = desc.motion == scene::ColliderMotion::Kinematic;
        bodies.push_back(slot);
    }

    impl->bodyIndexById[raw] = index;
    return scene::BodyId{index, bodies[index].generation};
}

scene::PhysicsCharacterId PhysicsWorld::createCharacter(const scene::ColliderDesc& desc, uint64_t userData) {
    if (impl == nullptr || !impl->initialised) return {};

    // No budget check: `capacity` is the body count handed to `PhysicsSystem::Init`, and a
    // `CharacterVirtual` is not tracked by the system at all -- it moves by collision queries
    // against a shape it owns. Adding one turns characters away over headroom they never use.
    JPH::RefConst<JPH::Shape> shape = makeShape(desc);
    if (shape == nullptr) return {};

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(desc.transform, translation, rotation, scale);
    shape = scaleShape(shape, scale, desc.name);

    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape;
    settings.mMaxSlopeAngle = desc.maxSlopeAngle;
    settings.mMass = desc.mass > 0.0f ? desc.mass : 70.0f;
    // The plane a supporting contact must sit below to count as ground. Jolt's own value, but
    // expressed against the capsule radius so a character authored at any size behaves alike.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -std::max(desc.radius, 1e-3f));

    // Adopted by the `Ref` on the line it is made: Jolt hands back a raw pointer at refcount
    // zero, and holding one across the container growths below leaks it if either throws.
    JPH::Ref<JPH::CharacterVirtual> character =
        new JPH::CharacterVirtual(&settings, toJolt(translation), toJolt(rotation), &impl->system);

    uint32_t index;
    if (!freeCharacterSlots.empty()) {
        index = freeCharacterSlots.back();
        freeCharacterSlots.pop_back();
        const uint32_t generation = characters[index].generation;
        characters[index] = Character{};
        characters[index].generation = generation;
        impl->characters[index] = character;
    } else {
        index = static_cast<uint32_t>(characters.size());
        impl->characters.emplace_back(character);
        characters.emplace_back();
    }

    Character& c = characters[index];
    c.live = true;
    c.userData = userData;
    c.moveSpeed = desc.moveSpeed;
    c.jumpSpeed = desc.jumpSpeed;
    c.acceleration = desc.acceleration;
    c.deceleration = desc.deceleration;
    c.airControl = desc.airControl;
    c.stepHeight = desc.stepHeight;
    c.jumpBufferSteps = desc.jumpBufferSteps;
    c.coyoteSteps = desc.coyoteSteps;
    // Kept only so `grow()` can rebuild the supporting-volume plane, which Jolt does not
    // hand back. Everything else about this character is either read off the
    // `CharacterVirtual` or lives in this slot already.
    c.capsuleRadius = desc.radius;
    // Not standing until it has been swept, which is what `characterGround` reports and
    // what the coyote window has to start from: a character that began the run in the air
    // must not be able to jump out of it.
    c.airSteps = kAirStepsMax;
    c.coyoteSpent = true;
    return scene::PhysicsCharacterId{index, c.generation};
}

void PhysicsWorld::destroy(scene::BodyId id) {
    if (!valid(id)) return;

    // Stale now, gone later. The generation moves here so that every handle a caller is
    // holding stops validating on the call they made, and the slot only reaches the free
    // list once Jolt has actually let go of the body -- which cannot happen from inside a
    // step. `reclaim()` is where the two meet.
    Body& slot = bodies[id.index];
    slot.live = false;
    ++slot.generation;
    pendingBodyRemoval.push_back(id.index);
}

void PhysicsWorld::destroy(scene::PhysicsCharacterId id) {
    if (!valid(id)) return;

    Character& slot = characters[id.index];
    slot.live = false;
    ++slot.generation;
    slot.moveDirection = glm::vec3(0.0f);
    slot.jump = false;
    slot.velocity = glm::vec3(0.0f);
    pendingCharacterRemoval.push_back(id.index);
}

bool PhysicsWorld::grow(uint32_t needed) {
    if (impl == nullptr || !impl->initialised) return false;
    if (needed <= capacity) return true;

    // **Doubled rather than grown to fit.** A world that spawns one body per frame would
    // otherwise rebuild once per frame, and the rebuild is the expensive half by orders of
    // magnitude. Jolt's own index ceiling is the only hard stop there is.
    uint64_t target = std::max<uint64_t>(capacity, 1u);
    while (target < needed) target *= 2u;
    target = std::min<uint64_t>(target, JPH::BodyID::cMaxBodyIndex);
    if (target < needed) {
        core::Logger::error(core::LogCategory::Scene,
                            "Physics: %u bodies is past Jolt's ceiling of %u -- this one is refused", needed,
                            static_cast<uint32_t>(JPH::BodyID::cMaxBodyIndex));
        return false;
    }

    auto zone = core::Profiler::scope("PhysicsWorld::grow");

    // Shapes are refcounted, so a `RefConst` held here outlives the system it came from --
    // which is what lets the world be captured and rebuilt without re-deriving a shape from a
    // `scene::ColliderDesc` that is long gone.
    struct SavedBody {
        uint32_t slot = 0;
        JPH::RefConst<JPH::Shape> shape;
        JPH::RVec3 position{JPH::RVec3::sZero()};
        JPH::Quat rotation{JPH::Quat::sIdentity()};
        JPH::Vec3 linear{JPH::Vec3::sZero()};
        JPH::Vec3 angular{JPH::Vec3::sZero()};
        JPH::EMotionType motion = JPH::EMotionType::Static;
        JPH::ObjectLayer layer = kLayerStatic;
        JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
        float friction = 0.0f;
        float restitution = 0.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        float gravityFactor = 1.0f;
        uint64_t userData = 0;
        bool active = false;
    };
    struct SavedCloth {
        uint32_t slot = 0;
        JPH::RefConst<JPH::SoftBodySharedSettings> shared;
        std::vector<JPH::Vec3> positions;
        std::vector<JPH::Vec3> velocities;
        std::vector<float> inverseMass;
    };
    struct SavedCharacter {
        uint32_t slot = 0;
        JPH::RefConst<JPH::Shape> shape;
        JPH::RVec3 position{JPH::RVec3::sZero()};
        JPH::Quat rotation{JPH::Quat::sIdentity()};
        JPH::Vec3 linear{JPH::Vec3::sZero()};
        float mass = 70.0f;
        float maxSlopeAngle = 0.0f;
    };

    std::vector<SavedBody> savedBodies;
    std::vector<SavedCloth> savedCloths;
    std::vector<SavedCharacter> savedCharacters;
    savedBodies.reserve(bodies.size());

    {
        const JPH::BodyLockInterface& locks = impl->system.GetBodyLockInterfaceNoLock();
        for (uint32_t i = 0; i < bodies.size(); ++i) {
            if (!bodies[i].live) continue;
            const JPH::BodyLockRead lock(locks, JPH::BodyID(bodies[i].id));
            if (!lock.Succeeded()) continue;
            const JPH::Body& b = lock.GetBody();

            SavedBody s;
            s.slot = i;
            s.shape = b.GetShape();
            s.position = b.GetPosition();
            s.rotation = b.GetRotation();
            s.linear = b.GetLinearVelocity();
            s.angular = b.GetAngularVelocity();
            s.motion = b.GetMotionType();
            s.layer = b.GetObjectLayer();
            s.friction = b.GetFriction();
            s.restitution = b.GetRestitution();
            s.userData = b.GetUserData();
            s.active = b.IsActive();
            if (const JPH::MotionProperties* mp = b.GetMotionPropertiesUnchecked(); mp != nullptr) {
                s.linearDamping = mp->GetLinearDamping();
                s.angularDamping = mp->GetAngularDamping();
                s.gravityFactor = mp->GetGravityFactor();
                s.dofs = mp->GetAllowedDOFs();
            }
            savedBodies.push_back(std::move(s));
        }

        // A cloth's state is per particle: the shared settings carry the topology, the
        // vertices carry where the solve had got to. Saving only the first snaps every hanging
        // fabric back to the pose it was welded in.
        for (uint32_t i = 0; i < clothes.size(); ++i) {
            const JPH::BodyLockRead lock(locks, JPH::BodyID(clothes[i].id));
            if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) continue;
            const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
            SavedCloth s;
            s.slot = i;
            s.shared = motion->GetSettings();
            s.positions.reserve(motion->GetVertices().size());
            for (const JPH::SoftBodyVertex& v : motion->GetVertices()) {
                s.positions.push_back(v.mPosition);
                s.velocities.push_back(v.mVelocity);
                s.inverseMass.push_back(v.mInvMass);
            }
            savedCloths.push_back(std::move(s));
        }
    }

    // A `CharacterVirtual` is not in the system, so nothing above reaches it, and it holds a
    // `PhysicsSystem*` for its own queries, so it cannot come across intact. Only its pose and
    // velocity need saving -- the windows in `Character` are this class's, not Jolt's.
    for (uint32_t i = 0; i < characters.size(); ++i) {
        if (!characters[i].live || impl->characters[i] == nullptr) continue;
        const JPH::CharacterVirtual& c = *impl->characters[i];
        SavedCharacter s;
        s.slot = i;
        s.shape = c.GetShape();
        s.position = c.GetPosition();
        s.rotation = c.GetRotation();
        s.linear = c.GetLinearVelocity();
        s.mass = c.GetMass();
        s.maxSlopeAngle = std::acos(std::clamp(c.GetCosMaxSlopeAngle(), -1.0f, 1.0f));
        savedCharacters.push_back(std::move(s));
    }

    const uint32_t was = capacity;

    // `createSystem` replaces `impl` wholesale, taking the old system, its broad phase and
    // every body in it. Nothing captured above may point into it: the shapes are refs and the
    // rest is by value.
    createSystem(static_cast<uint32_t>(target));

    JPH::BodyInterface& bi = impl->system.GetBodyInterface();
    for (const SavedBody& s : savedBodies) {
        JPH::BodyCreationSettings settings(s.shape, s.position, s.rotation, s.motion, s.layer);
        settings.mFriction = s.friction;
        settings.mRestitution = s.restitution;
        settings.mLinearDamping = s.linearDamping;
        settings.mAngularDamping = s.angularDamping;
        settings.mGravityFactor = s.gravityFactor;
        settings.mAllowedDOFs = s.dofs;
        settings.mUserData = s.userData;
        // The shape already carries the mass properties it was created with; overriding from
        // the old body's mass here would recompute an inertia tensor that is already correct.
        JPH::Body* body = bi.CreateBody(settings);
        if (body == nullptr) {
            core::Logger::error(core::LogCategory::Scene, "Physics: grow lost a body at slot %u", s.slot);
            continue;
        }
        bi.AddBody(body->GetID(), s.active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        if (s.motion != JPH::EMotionType::Static) {
            bi.SetLinearAndAngularVelocity(body->GetID(), s.linear, s.angular);
        }

        // The slot keeps its index and its generation and only the raw id moves, which is what
        // makes a growth invisible to a game holding a `scene::BodyId`.
        bodies[s.slot].id = body->GetID().GetIndexAndSequenceNumber();
        impl->bodyIndexById[bodies[s.slot].id] = s.slot;
    }

    for (const SavedCloth& s : savedCloths) {
        JPH::SoftBodyCreationSettings settings(const_cast<JPH::SoftBodySharedSettings*>(s.shared.GetPtr()),
                                               JPH::RVec3::sZero(), JPH::Quat::sIdentity(), kLayerMoving);
        settings.mNumIterations = kClothSolverIterations;
        settings.mLinearDamping = kClothLinearDamping;
        settings.mVertexRadius = kClothVertexRadius;
        settings.mUpdatePosition = false;
        settings.mMakeRotationIdentity = false;
        JPH::Body* body = bi.CreateSoftBody(settings);
        if (body == nullptr) {
            core::Logger::error(core::LogCategory::Scene, "Physics: grow lost a cloth at slot %u", s.slot);
            continue;
        }
        bi.AddBody(body->GetID(), JPH::EActivation::Activate);
        clothes[s.slot].id = body->GetID().GetIndexAndSequenceNumber();

        auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
        const size_t n = std::min(s.positions.size(), static_cast<size_t>(motion->GetVertices().size()));
        for (size_t v = 0; v < n; ++v) {
            JPH::SoftBodyVertex& vert = motion->GetVertices()[static_cast<JPH::uint>(v)];
            vert.mPosition = s.positions[v];
            vert.mPreviousPosition = s.positions[v];
            vert.mVelocity = s.velocities[v];
            vert.mInvMass = s.inverseMass[v];
        }
    }

    impl->characters.assign(characters.size(), nullptr);
    for (const SavedCharacter& s : savedCharacters) {
        JPH::CharacterVirtualSettings settings;
        settings.mShape = s.shape;
        settings.mMaxSlopeAngle = s.maxSlopeAngle;
        settings.mMass = s.mass;
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -std::max(characters[s.slot].capsuleRadius, 1e-3f));
        JPH::Ref<JPH::CharacterVirtual> character =
            new JPH::CharacterVirtual(&settings, s.position, s.rotation, &impl->system);
        character->SetLinearVelocity(s.linear);
        // A fresh character has never swept and believes it is in mid-air, so without this a
        // fighter standing on the floor reports airborne for a step because something else
        // spawned -- restarting the coyote window and the jump buffer with it.
        character->RefreshContacts(impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                                   impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);
        impl->characters[s.slot] = character;
    }

    // The broad phase was built from nothing, so it needs the same optimise a load does.
    impl->system.OptimizeBroadPhase();

    core::Logger::status(core::LogCategory::Scene,
                         "Physics: grew %u -> %u bodies (%zu carried, %zu characters, %zu cloths)", was, capacity,
                         savedBodies.size(), savedCharacters.size(), savedCloths.size());
    return true;
}

void PhysicsWorld::reclaim() {
    if (impl == nullptr) return;

    if (!pendingBodyRemoval.empty()) {
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        for (const uint32_t index : pendingBodyRemoval) {
            const JPH::BodyID jolt(bodies[index].id);
            impl->bodyIndexById.erase(bodies[index].id);
            bi.RemoveBody(jolt);
            bi.DestroyBody(jolt);
            bodies[index].id = 0;
            // Only now, with nothing in Jolt referring to the slot, may it be handed out.
            freeBodySlots.push_back(index);
        }
        pendingBodyRemoval.clear();
    }

    for (const uint32_t index : pendingCharacterRemoval) {
        impl->characters[index] = nullptr;
        freeCharacterSlots.push_back(index);
    }
    pendingCharacterRemoval.clear();
}

void PhysicsWorld::finalize() {
    if (impl == nullptr || !impl->initialised) return;
    impl->system.OptimizeBroadPhase();
    // Both snapshots start equal, or a frame rendered before the first step interpolates
    // between a state and zero and draws every body sliding in from the origin.
    snapshot();
    previous = current;
    previousCharacters = currentCharacters;
}

void PhysicsWorld::snapshot() {
    if (impl == nullptr) return;
    current.resize(bodies.size());
    currentCharacters.resize(characters.size());

    const JPH::BodyInterface& bi = impl->system.GetBodyInterfaceNoLock();
    for (size_t i = 0; i < bodies.size(); ++i) {
        // A retired slot has no Jolt body to read; its stale state is unreachable because
        // every accessor goes through `valid()` first.
        if (!bodies[i].live) continue;
        const JPH::BodyID id(bodies[i].id);
        JPH::Vec3 position;
        JPH::Quat rotation;
        bi.GetPositionAndRotation(id, position, rotation);
        current[i].position = toGlm(position);
        current[i].rotation = toGlm(rotation);
    }

    for (size_t i = 0; i < characters.size(); ++i) {
        if (!characters[i].live) continue;
        PhysicsState& s = currentCharacters[i];
        s.position = toGlm(impl->characters[i]->GetPosition());
        s.rotation = toGlm(impl->characters[i]->GetRotation());
    }

    // Each pair must stay the same length: every accessor bounds-checks the current array
    // alone and then indexes both. A slot appended after load starts equal to `current`, so it
    // costs one uninterpolated step; zero-filling would draw it in from the origin.
    const auto extend = [](std::vector<PhysicsState>& older, const std::vector<PhysicsState>& newer) {
        if (older.size() == newer.size()) return;
        const size_t from = std::min(older.size(), newer.size());
        older.resize(newer.size());
        for (size_t i = from; i < newer.size(); ++i) older[i] = newer[i];
    };
    extend(previous, current);
    extend(previousCharacters, currentCharacters);
}

void PhysicsWorld::step(float dt) {
    auto s = core::Profiler::scope("PhysicsWorld::step");
    if (impl == nullptr || !impl->initialised) return;

    // Before anything else, and outside `Update` rather than inside it: this is the one moment
    // at which removing a body from the system is safe. `destroy` only ever queues.
    reclaim();

    // Cleared in the same breath as the slots their handles name, which is what makes the
    // whole gap between two steps a window in which a game may read them *and* destroy what
    // they name. Before the `empty()` return, so a world whose last body went away stops
    // reporting what it collided with.
    stepContacts.clear();
    impl->contacts.found.clear();

    if (empty()) return;

    previous = current;
    previousCharacters = currentCharacters;

    // Characters before `system.Update`, as in Jolt's own samples: a `CharacterVirtual` reads
    // the world rather than being solved with it, so sweeping it after the step would give it
    // contacts from a world its own sweep never saw.
    if (!characters.empty()) {
        const JPH::Vec3 gravity = toJolt(config.gravity);

        for (size_t i = 0; i < characters.size(); ++i) {
            Character& c = characters[i];
            if (!c.live) continue;
            JPH::CharacterVirtual& cv = *impl->characters[i];

            // The coyote window and the jump buffer are counted in steps, and this loop is the
            // only thing that advances them. Keeping either in seconds against an accumulator
            // makes its size move with the clock: sixty additions of `1.0f/60.0f` land just
            // under a second, so the window would be off by a step at random.
            const bool launchedLastStep = c.launched;
            c.launched = false;

            const JPH::CharacterBase::EGroundState ground = cv.GetGroundState();
            const bool standing = ground == JPH::CharacterBase::EGroundState::OnGround;

            // The sweep that would show a launch has not run yet, so the step after one still
            // reports standing. Dropping `launchedLastStep` hands back the coyote time the
            // launch just spent, which is a second jump out of thin air.
            if (standing && !launchedLastStep) {
                c.airSteps = 0;
                c.coyoteSpent = false;
            } else if (c.airSteps < kAirStepsMax) {
                ++c.airSteps;
            }

            // `+ 1` so that a window of zero still allows the jump on the step the press
            // reaches, which is the only thing "no buffer" can mean.
            if (c.jump) c.jumpBuffer = c.jumpBufferSteps + 1u;
            c.jump = false;

            const bool mayLaunch = !c.coyoteSpent && (standing || c.airSteps <= c.coyoteSteps);
            bool launch = false;
            if (c.jumpBuffer > 0u) {
                if (mayLaunch) {
                    launch = true;
                    c.jumpBuffer = 0u;
                } else {
                    --c.jumpBuffer;
                }
            }

            // The horizontal velocity ramps toward the request rather than being assigned it,
            // and the ramp is written against the velocity *relative to the ground*, so it is
            // a ramp against a moving platform rather than against the world. An acceleration
            // large enough to close the gap in one step reproduces a plain assignment.
            //
            // Steep ground takes the airborne branch, which is what makes gravity accumulate
            // and the character slide down it.
            JPH::Vec3 velocity = cv.GetLinearVelocity();
            if (!standing) velocity += gravity * dt;

            // The ground's own velocity is the base, or a character standing on a moving
            // platform is left behind by it.
            const JPH::Vec3 base = standing ? cv.GetGroundVelocity() : JPH::Vec3::sZero();
            JPH::Vec3 relative(velocity.GetX() - base.GetX(), 0.0f, velocity.GetZ() - base.GetZ());

            const JPH::Vec3 target = JPH::Vec3(c.moveDirection.x, 0.0f, c.moveDirection.z) * c.moveSpeed;
            // The rate is chosen on whether the request is *faster* than the current motion,
            // not on whether it is zero: that is what makes turning around at full speed a
            // deceleration through the turn and an acceleration out of it.
            const float rate = (target.Length() >= relative.Length() ? c.acceleration : c.deceleration) *
                               (standing ? 1.0f : c.airControl);
            JPH::Vec3 delta = target - relative;
            const float limit = std::max(rate, 0.0f) * dt;
            const float want = delta.Length();
            if (want > limit && want > 1e-6f) delta = delta * (limit / want);
            relative += delta;

            float vertical = standing ? base.GetY() : velocity.GetY();
            if (launch) {
                // Added to the ground's own vertical while standing, so a jump off a rising
                // platform is the platform's speed plus the character's. A coyote launch
                // replaces the fall it was in -- a jump taken late must not be a weaker jump.
                vertical = (standing ? base.GetY() : 0.0f) + c.jumpSpeed;
                c.coyoteSpent = true;
                c.launched = true;
            }
            cv.SetLinearVelocity(JPH::Vec3(base.GetX() + relative.GetX(), vertical, base.GetZ() + relative.GetZ()));

            // Per character, because `stepHeight` is authored per collider and Jolt's defaults
            // are absolute metres. The ratio is what matters: the step-down has to reach
            // further than the step-up, or a character that walked up one stair hovers off the
            // next.
            JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
            updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, c.stepHeight, 0.0f);
            updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -c.stepHeight * 1.25f, 0.0f);

            // Taken before the sweep, because the sweep is the only thing that knows what the
            // request survived. `ExtendedUpdate` slides the shape and leaves `mLinearVelocity`
            // exactly as set, so reading that back afterwards answers with the request: a
            // character pressed into a wall reports a full-speed run while the key is held.
            const JPH::RVec3 before = cv.GetPosition();

            cv.ExtendedUpdate(dt, gravity, updateSettings, impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                              impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);

            // What the sweep did, relative to the ground rather than the world: a locomotion
            // machine blends on gait, so standing still on a platform moving at 2 m/s must
            // read 0. The ground is re-read after `ExtendedUpdate` rather than reusing `base`,
            // because what the character stands on is one of the things that call can change.
            //
            // Not fed back into the ramp above, whose state is the request: a fighter leaning
            // on a column for a second still has its speed when the column moves away.
            const JPH::Vec3 moved = JPH::Vec3(cv.GetPosition() - before) / dt;
            const JPH::Vec3 under = cv.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround
                                        ? cv.GetGroundVelocity()
                                        : JPH::Vec3::sZero();
            c.velocity = glm::vec3(moved.GetX() - under.GetX(), 0.0f, moved.GetZ() - under.GetZ());
        }
    }

    impl->system.Update(dt, static_cast<int>(config.collisionSteps), impl->temp.get(), impl->jobs.get());
    collectContacts();
    snapshot();
}

void PhysicsWorld::collectContacts() {
    const std::vector<ContactRecorder::Raw>& found = impl->contacts.found;
    if (found.empty()) return;
    stepContacts.reserve(found.size());

    for (const ContactRecorder::Raw& raw : found) {
        Contact contact;
        contact.a = handleFor(raw.body1);
        contact.b = handleFor(raw.body2);
        // A manifold this class cannot name both ends of is dropped rather than reported with
        // an invalid handle: an event a caller can ask no questions about is one it would have
        // to filter itself.
        if (!contact.a.valid() || !contact.b.valid()) continue;

        contact.point = raw.point;
        contact.normal = raw.normal;
        contact.speed = raw.speed;

        // `a` is the lower slot. Jolt sorts the pair by `BodyID`, which stops matching this
        // class's slot order the moment a slot is reused. The normal is defined out of `a`, so
        // swapping the pair must negate it.
        if (contact.b.index < contact.a.index) {
            std::swap(contact.a, contact.b);
            contact.normal = -contact.normal;
        }
        stepContacts.push_back(contact);
    }

    // The order has to be the scene's, not the thread pool's. Position breaks the tie between
    // two manifolds on one pair -- a compound shape resting on two of its children -- because
    // after the pair it is the only remaining property of the collision rather than of which
    // job finished first.
    std::sort(stepContacts.begin(), stepContacts.end(), [](const Contact& l, const Contact& r) {
        if (l.a.index != r.a.index) return l.a.index < r.a.index;
        if (l.b.index != r.b.index) return l.b.index < r.b.index;
        if (l.point.x != r.point.x) return l.point.x < r.point.x;
        if (l.point.y != r.point.y) return l.point.y < r.point.y;
        return l.point.z < r.point.z;
    });
}

glm::mat4 PhysicsWorld::bodyTransform(scene::BodyId id, float alpha) const {
    if (!valid(id) || id.index >= current.size()) return glm::mat4(1.0f);
    return interpolateState(previous[id.index], current[id.index], alpha);
}

void PhysicsWorld::setBodyTransform(scene::BodyId id, const glm::mat4& transform) {
    if (impl == nullptr || !valid(id)) return;
    if (!bodies[id.index].moves) {
        core::Logger::warn(core::LogCategory::Scene,
                           "Physics: a static body cannot be placed after finalize(); ignoring");
        return;
    }

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(transform, translation, rotation, scale);

    JPH::BodyInterface& bi = impl->system.GetBodyInterface();

    // A kinematic body is *moved*, not placed, and the difference is whether anything can
    // stand on it. `SetPositionAndRotation` teleports, so the body keeps the velocity it had
    // -- zero, for a platform driven from a scene node -- and
    // `CharacterVirtual::GetGroundVelocity` then hands its rider zero and leaves it standing
    // still while the platform slides out from under it. `MoveKinematic` sets the velocity
    // that arrives at the target in one step, which is the velocity the character reads, and
    // still ends the step exactly on target.
    //
    // The config's step, not a measured delta: that is the clock the solver runs on. A frame
    // running two steps asks for the whole frame's travel in one step's time and overshoots by
    // one step, which the next sweep corrects.
    if (bodies[id.index].kinematic) {
        bi.MoveKinematic(JPH::BodyID(bodies[id.index].id), toJolt(translation), toJolt(rotation),
                         std::max(config.step, 1e-6f));
    } else {
        bi.SetPositionAndRotation(JPH::BodyID(bodies[id.index].id), toJolt(translation), toJolt(rotation),
                                  JPH::EActivation::Activate);
    }

    // Both snapshots. A body placed rather than solved has no previous state worth
    // interpolating from, and leaving the old one smears it back for the rest of the frame.
    if (id.index < current.size() && id.index < previous.size()) {
        current[id.index].position = translation;
        current[id.index].rotation = rotation;
        previous[id.index] = current[id.index];
    }
}

void PhysicsWorld::addImpulse(scene::BodyId id, const glm::vec3& impulse) {
    if (impl == nullptr || !valid(id)) return;
    const Body& body = bodies[id.index];
    if (!body.moves || body.kinematic) {
        // Jolt's own `AddImpulse` checks `IsDynamic()` and returns without a word, so removing
        // this leaves a crate that does not move and a log that says nothing.
        core::Logger::warn(core::LogCategory::Scene, "Physics: a %s body takes no impulse; ignoring",
                           body.kinematic ? "kinematic" : "static");
        return;
    }
    // The body interface wakes it; the raw `Body::AddImpulse` does not, and the impulse would
    // then arrive whenever something else woke it.
    impl->system.GetBodyInterface().AddImpulse(JPH::BodyID(body.id), toJolt(impulse));
}

void PhysicsWorld::setLinearVelocity(scene::BodyId id, const glm::vec3& velocity) {
    if (impl == nullptr || !valid(id)) return;
    const Body& body = bodies[id.index];
    if (!body.moves) {
        core::Logger::warn(core::LogCategory::Scene, "Physics: a static body has no velocity to set; ignoring");
        return;
    }
    // The clamped form. The unclamped one asserts against `mMaxLinearVelocity` with Jolt's
    // assertions on, turning a caller's bad arithmetic into an abort in debug and a working
    // game in release.
    impl->system.GetBodyInterface().SetLinearVelocity(JPH::BodyID(body.id), toJolt(velocity));
}

glm::vec3 PhysicsWorld::linearVelocity(scene::BodyId id) const {
    if (impl == nullptr || !valid(id)) return glm::vec3(0.0f);
    return toGlm(impl->system.GetBodyInterface().GetLinearVelocity(JPH::BodyID(bodies[id.index].id)));
}

glm::mat4 PhysicsWorld::characterTransform(scene::PhysicsCharacterId id, float alpha) const {
    if (!valid(id) || id.index >= currentCharacters.size()) return glm::mat4(1.0f);
    return interpolateState(previousCharacters[id.index], currentCharacters[id.index], alpha);
}

void PhysicsWorld::setCharacterTransform(scene::PhysicsCharacterId id, const glm::mat4& transform) {
    if (impl == nullptr || !valid(id)) return;

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(transform, translation, rotation, scale);

    JPH::CharacterVirtual& cv = *impl->characters[id.index];
    cv.SetPosition(toJolt(translation));
    cv.SetRotation(toJolt(rotation));
    cv.SetLinearVelocity(JPH::Vec3::sZero());

    // The ground state is stale the instant the character moves, and `step()` reads it before
    // it sweeps. Without this a character placed over a pit reports standing for one step --
    // long enough to spend a jump, refill the coyote window and ramp its horizontal velocity
    // against a platform that is no longer under it.
    cv.RefreshContacts(impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                       impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);

    Character& c = characters[id.index];
    c.velocity = glm::vec3(0.0f);
    c.jump = false;
    c.jumpBuffer = 0u;
    c.launched = false;
    // Emptied, not left: a placement into mid-air must not be able to jump out of it on the
    // strength of ground the character left behind. The next step refills both from the
    // contacts refreshed above.
    c.airSteps = kAirStepsMax;
    c.coyoteSpent = true;

    // Both snapshots, as `setBodyTransform` writes both: leaving the previous one draws the
    // character crossing the map for the rest of the frame.
    if (id.index < currentCharacters.size() && id.index < previousCharacters.size()) {
        currentCharacters[id.index].position = translation;
        currentCharacters[id.index].rotation = rotation;
        previousCharacters[id.index] = currentCharacters[id.index];
    }
}

void PhysicsWorld::setCharacterInput(scene::PhysicsCharacterId id, const glm::vec3& moveDirection, bool jump) {
    if (!valid(id)) return;
    characters[id.index].moveDirection = moveDirection;
    // Latched, not assigned: a jump pressed between two simulation steps is otherwise
    // overwritten by the next frame's `false` and never reaches the solver.
    characters[id.index].jump = characters[id.index].jump || jump;
}

float PhysicsWorld::characterSpeed(scene::PhysicsCharacterId id) const {
    return valid(id) ? glm::length(characters[id.index].velocity) : 0.0f;
}

glm::vec3 PhysicsWorld::characterVelocity(scene::PhysicsCharacterId id) const {
    return valid(id) ? characters[id.index].velocity : glm::vec3(0.0f);
}

float PhysicsWorld::characterMoveSpeed(scene::PhysicsCharacterId id) const {
    return valid(id) ? characters[id.index].moveSpeed : 0.0f;
}

bool PhysicsWorld::characterOnGround(scene::PhysicsCharacterId id) const {
    return characterGround(id) == scene::CharacterGround::OnGround;
}

scene::CharacterGround PhysicsWorld::characterGround(scene::PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id)) return scene::CharacterGround::InAir;
    switch (impl->characters[id.index]->GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround:
        return scene::CharacterGround::OnGround;
    // Two of Jolt's four states give one answer: something is under the character and it is
    // going down anyway.
    case JPH::CharacterBase::EGroundState::OnSteepGround:
    case JPH::CharacterBase::EGroundState::NotSupported:
        return scene::CharacterGround::Sliding;
    case JPH::CharacterBase::EGroundState::InAir:
        break;
    }
    return scene::CharacterGround::InAir;
}

glm::vec3 PhysicsWorld::characterGroundNormal(scene::PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id) || characterGround(id) == scene::CharacterGround::InAir) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    // Jolt keeps the last ground it found, so the air case must be filtered above: without it
    // this reports the normal of a face the character has already left.
    return toGlm(impl->characters[id.index]->GetGroundNormal());
}

scene::BodyId PhysicsWorld::characterGroundBody(scene::PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id) || characterGround(id) == scene::CharacterGround::InAir) return {};
    const JPH::BodyID under = impl->characters[id.index]->GetGroundBodyID();
    return under.IsInvalid() ? scene::BodyId{} : handleFor(under.GetIndexAndSequenceNumber());
}

bool PhysicsWorld::characterJumped(scene::PhysicsCharacterId id) const {
    return valid(id) && characters[id.index].launched;
}

bool PhysicsWorld::characterMotion(uint64_t controller, scene::CharacterMotion* out) const {
    const scene::PhysicsCharacterId id = core::unpackHandle<scene::PhysicsCharacterTag>(controller);
    if (!valid(id)) return false;
    out->speed = characterSpeed(id);
    out->topSpeed = characterMoveSpeed(id);
    out->onGround = characterOnGround(id);
    out->jumped = characterJumped(id);
    return true;
}

scene::BodyId PhysicsWorld::handleFor(uint32_t joltRawId) const {
    const auto it = impl->bodyIndexById.find(joltRawId);
    if (it == impl->bodyIndexById.end()) return {};
    const uint32_t slot = it->second;
    if (slot >= bodies.size() || !bodies[slot].live) return {};
    return scene::BodyId{slot, bodies[slot].generation};
}

// Every query below leaves the two layer filters at their defaults, so every layer is solid:
// a wall and a closed door are in different ones and both have to stop a bullet and a sound.
PhysicsWorld::RayHit PhysicsWorld::raycast(const glm::vec3& from, const glm::vec3& to, scene::BodyId ignoreBody) const {
    RayHit hit;
    if (impl == nullptr || !impl->initialised) return hit;

    const glm::vec3 segment = to - from;
    const JPH::Vec3 origin = toJolt(from);
    const JPH::Vec3 delta = toJolt(segment);
    // Checked here rather than left to Jolt, which normalises the direction and would divide
    // by zero.
    if (delta.LengthSq() < 1e-8f) return hit;

    JPH::RayCastResult result;
    const JPH::RRayCast ray{origin, delta};
    const JPH::BroadPhaseLayerFilter broadPhaseFilter;
    const JPH::ObjectLayerFilter layerFilter;
    bool found = false;
    if (!valid(ignoreBody)) {
        found = impl->system.GetNarrowPhaseQuery().CastRay(ray, result);
    } else {
        const JPH::IgnoreSingleBodyFilter bodyFilter{JPH::BodyID(bodies[ignoreBody.index].id)};
        found = impl->system.GetNarrowPhaseQuery().CastRay(ray, result, broadPhaseFilter, layerFilter, bodyFilter);
    }
    if (!found) return hit;

    hit.body = handleFor(result.mBodyID.GetIndexAndSequenceNumber());
    hit.point = from + segment * result.mFraction;
    hit.distance = glm::length(segment) * result.mFraction;

    // Reading a body means holding its lock -- the system may be mid-step on another thread.
    // `Succeeded()` is not a formality: a body destroyed between the cast and the lock is
    // exactly the case it catches.
    const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        hit.normal = toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, toJolt(hit.point)));
    }

    return hit;
}

PhysicsWorld::RayHit PhysicsWorld::sphereCast(const glm::vec3& from, const glm::vec3& to, float radius,
                                              scene::BodyId ignoreBody) const {
    RayHit hit;
    if (impl == nullptr || !impl->initialised || radius <= 0.0f) return hit;

    const glm::vec3 segment = to - from;
    if (glm::dot(segment, segment) < 1e-8f) return hit;

    const JPH::SphereShape sphere(radius);
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(toJolt(from)),
                               toJolt(segment));

    JPH::ShapeCastSettings settings;
    // A character probing downward starts overlapping the floor it stands on; reporting those
    // initial overlaps makes every ground check hit at fraction zero.
    settings.mReturnDeepestPoint = false;

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const JPH::BroadPhaseLayerFilter broadPhaseFilter;
    const JPH::ObjectLayerFilter layerFilter;
    if (!valid(ignoreBody)) {
        impl->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector);
    } else {
        const JPH::IgnoreSingleBodyFilter bodyFilter{JPH::BodyID(bodies[ignoreBody.index].id)};
        impl->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector,
                                                     broadPhaseFilter, layerFilter, bodyFilter);
    }
    if (!collector.HadHit()) return hit;

    const JPH::ShapeCastResult& r = collector.mHit;
    hit.body = handleFor(r.mBodyID2.GetIndexAndSequenceNumber());
    hit.point = toGlm(r.mContactPointOn2);
    // Negated because Jolt's penetration axis points *into* body 2, and `RayHit::normal` is
    // documented as pointing out of the body that was hit.
    hit.normal = -glm::normalize(toGlm(r.mPenetrationAxis));
    hit.distance = glm::length(segment) * r.mFraction;
    return hit;
}

uint32_t PhysicsWorld::overlapSphere(const glm::vec3& center, float radius, std::span<scene::BodyId> out) const {
    if (impl == nullptr || !impl->initialised || radius <= 0.0f) return 0;

    const JPH::SphereShape sphere(radius);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl->system.GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f),
                                                    JPH::RMat44::sTranslation(toJolt(center)),
                                                    JPH::CollideShapeSettings{}, JPH::RVec3::sZero(), collector);

    // `found` counts overlaps, not writes, which is what lets a caller detect truncation.
    // Bodies this class did not add are skipped entirely, so the count never includes one the
    // caller could ask nothing about.
    uint32_t found = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const scene::BodyId id = handleFor(r.mBodyID2.GetIndexAndSequenceNumber());
        if (!id.valid()) continue;
        if (found < out.size()) out[found] = id;
        ++found;
    }
    return found;
}

bool PhysicsWorld::segmentBlocked(const glm::vec3& from, const glm::vec3& to, scene::BodyId ignoreBody) const {
    // Over `raycast`, not beside it: a second cast of the same ray with the same filters is
    // one waiting to disagree with the other.
    return static_cast<bool>(raycast(from, to, ignoreBody));
}

void PhysicsWorld::drawDebug(std::vector<gfx::DebugLineVertex>& out, const glm::vec3& cameraPosition) {
    if (impl == nullptr || !impl->initialised) return;

    // Drawn from `stepContacts` rather than through Jolt's own contact drawing: that happens
    // inside the step, into `DebugRenderer::sInstance`, and the renderer below is attached to
    // an output vector only for the duration of this call, so its lines go nowhere.
    if (debugContacts) {
        for (const Contact& contact : stepContacts) {
            // White for a graze, red for a hit. 6 m/s is roughly a one-and-a-half metre fall,
            // the speed at which a thing sounds like it landed.
            const float hardness = std::min(contact.speed / 6.0f, 1.0f);
            const uint32_t color = gfx::packDebugColor({1.0f, 1.0f - hardness, 1.0f - hardness, 1.0f});
            // A cross, because a line list cannot draw a point and three axis-aligned segments
            // read as a marker from angles a normal-aligned pair foreshortens to nothing.
            constexpr float kArm = 0.06f;
            for (int axis = 0; axis < 3; ++axis) {
                glm::vec3 arm(0.0f);
                arm[axis] = kArm;
                out.push_back({contact.point - arm, color});
                out.push_back({contact.point + arm, color});
            }
            out.push_back({contact.point, color});
            out.push_back({contact.point + contact.normal * (0.1f + 0.4f * hardness), color});
        }
    }

#ifdef JPH_DEBUG_RENDERER
    if (empty()) return;
    if (impl->debug == nullptr) impl->debug = std::make_unique<PhysicsDebugRenderer>();

    impl->debug->out = &out;
    impl->debug->SetCameraPos(JPH::RVec3(cameraPosition.x, cameraPosition.y, cameraPosition.z));

    JPH::BodyManager::DrawSettings settings;
    settings.mDrawShape = true;
    settings.mDrawShapeWireframe = true;
    impl->system.DrawBodies(settings, impl->debug.get());
    impl->system.DrawConstraints(impl->debug.get());

    // A character is not a body, so `DrawBodies` does not reach it.
    for (const JPH::Ref<JPH::CharacterVirtual>& c : impl->characters) {
        c->GetShape()->Draw(impl->debug.get(), c->GetCenterOfMassTransform(), JPH::Vec3::sOne(),
                            JPH::Color::sGreen, false, true);
    }

    impl->debug->out = nullptr;
#else
    (void)cameraPosition;
#endif
}

} // namespace physics
