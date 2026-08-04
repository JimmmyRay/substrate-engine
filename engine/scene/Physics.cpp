#include "scene/Physics.h"

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

namespace scene {

namespace {

/// Object layers. Two is all this engine has needed: things that never move, and things
/// that do. A third would be a game's decision (triggers, projectiles, a player layer),
/// and the tables below take a count rather than an enumeration, so adding one is a
/// number and two `EnableCollision` calls.
constexpr JPH::ObjectLayer kLayerStatic = 0;
constexpr JPH::ObjectLayer kLayerMoving = 1;
constexpr uint32_t kObjectLayerCount = 2;

/// Where `Character::airSteps` stops counting (C20). It is only ever compared against a
/// coyote window, so the count past that window carries no information -- and a character
/// that has been falling for two years is not a case worth wrapping a counter for.
constexpr uint32_t kAirStepsMax = 1u << 20;

/// Broad-phase layers, one per object layer here. They are separate concepts -- many
/// object layers can share a broad-phase tree -- and keeping them parallel while there
/// are two of each is the Rule of Threes rather than an oversight.
constexpr JPH::BroadPhaseLayer kBroadPhaseStatic(0);
constexpr JPH::BroadPhaseLayer kBroadPhaseMoving(1);
constexpr uint32_t kBroadPhaseLayerCount = 2;

/// Headroom over the colliders a scene declared, when no budget names a ceiling. A
/// stated figure rather than a guess at a working set: it is enough for a game to spawn
/// a magazine of debris without re-initialising the world, and small enough that a scene
/// with four colliders does not allocate for a thousand.
constexpr uint32_t kBodyHeadroom = 256;

/// Jolt asks for these at init and they scale with the body count rather than with
/// anything a caller would know. The floors are Jolt's own sample values, which are the
/// smallest numbers known to behave.
uint32_t bodyPairsFor(uint32_t maxBodies) { return std::max(1024u, maxBodies * 8u); }
uint32_t contactConstraintsFor(uint32_t maxBodies) { return std::max(1024u, maxBodies * 4u); }

/// Jolt's temp allocator is a stack, sized once. 16 MB is its own sample figure and is
/// what a few thousand bodies need; it is a single allocation held for the process.
constexpr uint32_t kTempAllocatorBytes = 16u * 1024u * 1024u;

glm::vec3 toGlm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat toGlm(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }
JPH::Vec3 toJolt(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJolt(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

/// Route Jolt's own diagnostics into the engine's log rather than onto stdout, where a
/// headless golden run would mix them into the capture script's output.
///
/// `Logger::vformat` rather than a buffer here, which is the reuse the old version most
/// obviously wanted: it truncated into `char[1024]` and then handed the result to
/// `Logger::debug`, the very thing that would have sized it correctly. The formatted
/// message goes to the `std::string` overload rather than through `"Jolt: %s"`, so a
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
 * worlds, and `RegisterTypes` is not idempotent. The destructor matters as much as the
 * constructor -- without it the factory is a leak ASan reports on every run of the suite,
 * which is a real report about a real allocation and would train everyone to ignore it.
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

/// Split a world matrix into the two things a body carries and the one thing it does
/// not. A Jolt body has a position and an orientation; scale belongs to the shape, which
/// is why it comes back separately rather than being quietly dropped.
void decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    translation = glm::vec3(m[3]);
    scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
    glm::mat3 r(m);
    // A zero axis is a degenerate node transform. Substituting one keeps the quaternion
    // finite; the shape it scales is already flat and will collide with nothing, which
    // is the honest outcome for geometry with no thickness.
    for (int i = 0; i < 3; ++i) {
        const float s = scale[i] > 1e-8f ? scale[i] : 1.0f;
        r[i] /= s;
    }
    rotation = glm::normalize(glm::quat_cast(r));
}

} // namespace

// ---------------------------------------------------------------------- FixedClock

void FixedClock::accumulate(float dt) {
    thisFrame = 0;
    // Scaled here rather than at any consumer, so everything downstream of the step
    // inherits the pause without knowing there is one. At the default scale this is a
    // multiplication by exactly 1.0f, which is exact in float and is what keeps a locked
    // clock bit-identical to the engine before C4.
    if (dt > 0.0f) accumulator += dt * timeScaleValue;
}

bool FixedClock::consume() {
    if (accumulator < stepSeconds) return false;

    if (thisFrame >= maxSteps) {
        // Everything still owed is discarded in one go rather than carried into the next
        // frame, which would only defer the same overrun and hide that it happened.
        const auto skipped = static_cast<uint32_t>(accumulator / stepSeconds);
        dropped += skipped;
        accumulator -= static_cast<float>(skipped) * stepSeconds;
        return false;
    }

    accumulator -= stepSeconds;
    ++thisFrame;
    ++total;
    return true;
}

float FixedClock::alpha() const {
    const float a = accumulator / stepSeconds;
    return a < 0.0f ? 0.0f : (a >= 1.0f ? 0.99999994f : a);
}

glm::mat4 interpolateState(const PhysicsState& a, const PhysicsState& b, float alpha) {
    const glm::vec3 p = glm::mix(a.position, b.position, alpha);
    const glm::quat r = glm::slerp(a.rotation, b.rotation, alpha);
    return glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r);
}

// ------------------------------------------------------------------ debug renderer

#ifdef JPH_DEBUG_RENDERER
/**
 * @brief The engine's one derived type, and the whole of what S4 spends on inheritance.
 *
 * It is here because Jolt's API is how a convex hull's or a triangle mesh's wireframe is
 * obtained at all -- those shapes have no parameters a procedural outline could be built
 * from, so drawing them any other way means re-deriving the hull. `DrawLine` and
 * `DrawText3D` are the two the base class leaves pure; `DrawTriangle` already falls back
 * to three `DrawLine`s, which is exactly what a wireframe wants.
 */
struct PhysicsDebugRenderer final : public JPH::DebugRendererSimple {
    std::vector<gfx::DebugLineVertex>* out = nullptr;

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override {
        if (out == nullptr) return;
        // Jolt's Color is already 0xAABBGGRR in memory, which is the layout
        // DebugLineVertex documents, so this is a read rather than a conversion.
        const uint32_t packed = color.mU32;
        out->push_back({glm::vec3(from.GetX(), from.GetY(), from.GetZ()), packed});
        out->push_back({glm::vec3(to.GetX(), to.GetY(), to.GetZ()), packed});
    }

    /// Deliberately empty. Text over a body is the one part of Jolt's debug output this
    /// engine already has a better answer for -- `recordOverlay` draws real glyphs -- and
    /// stubbing it beats rasterising strings into a line list.
    void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override {}
};
#else
struct PhysicsDebugRenderer {};
#endif

// --------------------------------------------------------------- contacts (G7)

namespace {

/**
 * @brief The second derived type in `engine/`, and it is allowed for the first one's reason.
 *
 * `PhysicsDebugRenderer` above exists because `JPH::DebugRendererSimple` is how Jolt hands
 * out a hull's wireframe; this exists because `JPH::ContactListener` is how Jolt hands out
 * the fact that two things touched. Both are a dependency's requirement rather than an
 * abstraction this engine chose, which is the line `principles.md` draws: `Game` is still
 * the only base class here that no library demanded.
 *
 * **It records and does nothing else.** Jolt calls this from inside `Update`, on job
 * threads, with every body in the world locked -- a caller that created a body, destroyed
 * one or took a body lock from here would deadlock the step, and there is no way to hand
 * that constraint to a game and expect it to hold. So the callback writes a POD into a
 * vector and `PhysicsWorld::collectContacts` turns it into something a game can act on
 * after the step is over.
 *
 * The lookup from `JPH::BodyID` to this engine's slot deliberately does *not* happen here.
 * It is a hash lookup per contact, and moving it out of the callback moves it off the
 * locked path, off N threads and onto one -- which costs nothing, because the drain has to
 * walk the list anyway.
 */
struct ContactRecorder final : public JPH::ContactListener {
    /// One manifold, in Jolt's terms, because that is all the callback can cheaply say.
    struct Raw {
        uint32_t body1 = 0; ///< JPH::BodyID's raw value
        uint32_t body2 = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float speed = 0.0f;
    };

    /// Uncontended in the configuration this engine ships -- `workerThreads` defaults to
    /// zero and the callbacks then arrive on the stepping thread -- so this is an atomic
    /// exchange per new contact. It is here rather than conditional on the thread count
    /// because a lock that appears when a config key changes is a lock nobody ever tests.
    std::mutex lock;
    std::vector<Raw> found;

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        const auto count = static_cast<uint32_t>(manifold.mRelativeContactPointsOn1.size());
        if (count == 0) return;

        // The centroid rather than point zero. A box landing flat gives four points whose
        // order is the clipper's, so taking the first would put the impact at whichever
        // corner Jolt happened to emit first and move it between two runs of the same
        // scene. Relative points summed before the base offset is added, which is the one
        // arrangement that stays exact when the world origin is far away.
        JPH::Vec3 sum = JPH::Vec3::sZero();
        for (uint32_t i = 0; i < count; ++i) sum += manifold.mRelativeContactPointsOn1[i];
        const JPH::RVec3 point = manifold.mBaseOffset + sum / static_cast<float>(count);

        // Velocities as they were *before* the solver ran, which is what Jolt's own
        // documentation says this callback is the place to read for exactly this purpose.
        // A static body reports zero, so a crate hitting a floor gets the crate's speed
        // rather than nothing.
        const JPH::Vec3 relative = body1.GetPointVelocity(point) - body2.GetPointVelocity(point);

        Raw raw;
        raw.body1 = body1.GetID().GetIndexAndSequenceNumber();
        raw.body2 = body2.GetID().GetIndexAndSequenceNumber();
        raw.point = toGlm(JPH::Vec3(point));
        raw.normal = toGlm(manifold.mWorldSpaceNormal);
        // Jolt's normal points from body 1 to body 2, so a body 1 moving toward body 2
        // gives a positive dot. Negative is a speculative contact detected while the two
        // are still separating; it is a real contact and its impact is zero.
        raw.speed = std::max(0.0f, relative.Dot(manifold.mWorldSpaceNormal));

        const std::lock_guard<std::mutex> held(lock);
        found.push_back(raw);
    }
};

} // namespace

// ------------------------------------------------------------------------- Impl

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

    /// `JPH::BodyID`'s raw value to the index this class hands out, so a query can report
    /// the body it hit in the caller's terms rather than Jolt's.
    ///
    /// A map rather than a scan of `bodies`, which is what the obvious version does. The
    /// scan is O(bodies) *per hit*, and a query surface exists to be called -- a few
    /// perception checks per agent per frame across a large scene is exactly the shape
    /// that turns an invisible constant into a profile. Jolt's own body user data is not
    /// available for this: `addBody` already stores the caller's `userData` there.
    std::unordered_map<uint32_t, uint32_t> bodyIndexById;

    /// `PhysicsWorld::handleFor` is what reads this; the generation that completes the
    /// handle lives in `bodies`, which Impl cannot see.

    /// Registered with the system at init and held for its whole life (G7). By value
    /// rather than behind a pointer, because the system stores a bare pointer to it and
    /// this holder already outlives the system it is a member beside.
    ContactRecorder contacts;
};

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() { shutdown(); }

void PhysicsWorld::init(const PhysicsConfig& cfg, uint32_t expectedBodies) {
    // Jolt's runtime registration, the broad phase, and the job pool.
    auto zone = core::Profiler::scope("PhysicsWorld::init");
    shutdown();
    ensureJoltRuntime();

    config = cfg;
    if (config.step <= 0.0f) config.step = 1.0f / 60.0f;
    if (config.collisionSteps == 0) config.collisionSteps = 1;

    // **A floor rather than a ceiling** (C40). `grow()` rebuilds the world when a create
    // outruns this, so the number decides only how much is allocated before that first
    // happens -- never whether a body exists.
    //
    // A stated budget is taken at its word and only raised to what the scene already
    // declares; the headroom is for the *derived* case, where nothing has said anything and
    // guessing tight would mean rebuilding during load. A game that states a small number is
    // saying it wants a small world, which is now a cost question rather than a ceiling.
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
    // Static against static is left disabled, which is not an optimisation but the
    // definition: two things that never move cannot begin to overlap.
    impl->objectPairs->EnableCollision(kLayerStatic, kLayerMoving);
    impl->objectPairs->EnableCollision(kLayerMoving, kLayerMoving);

    impl->objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        *impl->broadPhase, kBroadPhaseLayerCount, *impl->objectPairs, kObjectLayerCount);

    impl->temp = std::make_unique<JPH::TempAllocatorImpl>(kTempAllocatorBytes);
    if (config.workerThreads == 0) {
        // The determinism default. Jolt reproduces a run for a *fixed* thread count, and
        // zero is the count that is fixed on every machine -- see the header.
        impl->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    } else {
        // Default-constructed and `Init`ed separately, because the convenience constructor
        // starts the threads and `SetThreadInitFunction` has to be set before they run.
        // The hook is what lets Jolt's workers name their own tracks -- they are threads
        // that can reach a contact callback, and a contact callback can reach ours.
        auto pool = std::make_unique<JPH::JobSystemThreadPool>();
        pool->SetThreadInitFunction([](int) { core::Profiler::nameThread("physics job"); });
        pool->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(config.workerThreads));
        impl->jobs = std::move(pool);
    }

    impl->system.Init(capacity, 0, bodyPairsFor(capacity), contactConstraintsFor(capacity), *impl->broadPhase,
                      *impl->objectVsBroadPhase, *impl->objectPairs);
    impl->system.SetGravity(toJolt(config.gravity));
    // Installed unconditionally rather than behind a "does anybody want contacts" flag
    // (G7). What it costs a scene nobody listens to is a `push_back` per *new* manifold,
    // into storage that stops growing after the first busy step; what a flag would cost is
    // a game whose contacts are silently missing because it set the flag after `init`.
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

// ------------------------------------------------------------------------- cloth (C19)

namespace {

/**
 * Solver settings for a soft body, in one place and taken rather than defaulted.
 *
 * C19's card names over-reaction as the failure this row would ship -- fabric that
 * bounces, twitches and swings far more than it should, forever -- and Tethered lost that
 * fight in four places, with four hard-coded constants each added against the same
 * symptom. **Jolt's advantage is not incidental: over-reactivity is what its substep and
 * iteration counts exist to control, and they are settings rather than shader constants.**
 * So there are three numbers here, all of them Jolt's own knobs, all of them stated:
 *
 * - `mNumIterations` is the solver's, and 8 rather than the default 5. Too few iterations
 *   means corrections that never converge, which is one of the four causes of the
 *   over-reaction above; the extra three are the cheapest available answer to it and they
 *   are what the envelope property in `tests/ClothTests.cpp` passes on.
 * - `mLinearDamping` at 0.2 rather than 0.1. Damping applied to the *velocity*, which is
 *   the right quantity -- Tethered's velocity threshold froze a slow vertex instead, and
 *   bought a quiet cloth at the price of one that never starts falling. C19's card names
 *   that specific hack as the thing not to reproduce, and this is what it is not.
 * - `mVertexRadius`, so a particle sits slightly off a surface rather than exactly on it.
 *   Zero is Jolt's default and it z-fights against whatever the cloth is resting on.
 *
 * There is deliberately no per-cloth tuning surface. A `substrate_cloth` extras block
 * exposing these would be the fifth authoring schema C19 refused, for the same reason:
 * `extras` cannot carry the per-vertex value the feature actually needs, so half the
 * convention would be in a name and half in a dictionary.
 */
constexpr uint32_t kClothSolverIterations = 10;
constexpr float kClothLinearDamping = 2.0f;
constexpr float kClothVertexRadius = 0.01f;

} // namespace

uint32_t PhysicsWorld::createCloth(const ClothTopology& topology) {
    if (impl == nullptr || !impl->initialised) return kNoCloth;
    if (topology.positions.empty() || topology.faces.size() < 3) return kNoCloth;
    // As `createBody`: the world grows rather than turning a fabric away (C40).
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
        // Zero is pinned, which is the representation `ClothVertex::invMass` already
        // holds -- so nothing is being adapted here, only copied.
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

    /*
     * Edge, shear *and* bend constraints, which is the thing Jolt gives that Tethered's
     * ported solver did not: it generated structural edges only, so its cloth resisted
     * stretching and nothing else -- a curtain that could fold along any diagonal for free.
     *
     * One `VertexAttributes` for every vertex rather than one per vertex. Jolt repeats the
     * last element when the list is shorter than the vertex array, which is documented on
     * `CreateConstraints` itself, so a uniform fabric needs exactly one. Per-vertex
     * compliance is the beginning of a cloth *material*, and C19 lists that among the
     * things this row must not grow.
     *
     * Zero compliance is inextensible, which is what fabric is at this scale; a curtain
     * that visibly stretches under its own weight is rubber. The bend type is Jolt's
     * distance default -- dihedral is the alternative and it is for volumetric bodies.
     */
    JPH::SoftBodySharedSettings::VertexAttributes attributes;
    // Inextensible, in-plane. Fabric does not stretch and it does not shear into a
    // parallelogram, and zero compliance is what says so.
    attributes.mCompliance = 0.0f;
    attributes.mShearCompliance = 0.0f;
    // **Bend constraints off, which is Jolt's own default and is what makes this cloth
    // rather than sheet metal.** Passing zero here -- which is what the obvious reading of
    // "stiff everywhere" produces -- makes the bend constraints infinitely stiff and turns
    // a nine-by-nine sheet into a rigid plate held by an over-constrained system the
    // Gauss-Seidel solver cannot satisfy. That was measured, and it is the whole of the
    // over-reaction C19's card predicted this row would ship: the curtain hung in the right
    // *place* and twitched two millimetres a step forever, and raising the iteration count
    // from 8 to 30 made it worse rather than better, because more iterations of an
    // unsatisfiable system is more energy. Limp fabric is both the correct model and the
    // stable one.
    attributes.mBendCompliance = FLT_MAX;
    /*
     * **The envelope, enforced by the solver rather than asserted after it.** A long-range
     * attachment constraint caps how far a vertex may get from the nearest pinned one, and
     * `GeodesicDistance` measures that along the edges -- so the cap is "no further than
     * the fabric between them is long", which is exactly the property `ClothTests.cpp`
     * checks and exactly what a stretching cloth violates.
     *
     * It is worth saying what this is *not*. It is not a clamp bolted on to hide a solver
     * that overshoots; it is a constraint the solver satisfies alongside the others, it is
     * Jolt's own, and it is the standard answer to the one artefact inextensible cloth
     * still has after everything else is right -- a long chain of edge constraints
     * propagates a correction one link per iteration, so the far corner of a sheet learns
     * about its pin several frames late and travels while it waits.
     */
    attributes.mLRAType = JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance;
    attributes.mLRAMaxDistanceMultiplier = 1.0f;
    shared->CreateConstraints(&attributes, 1);
    shared->Optimize();

    // Identity position and rotation, because `weldCloth` already put the vertices in
    // world space -- the node transform is baked in at load and ignored thereafter. A
    // soft body has no rigid transform to push down a node hierarchy, so there is nothing
    // for a second placement to mean.
    JPH::SoftBodyCreationSettings settings(shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), kLayerMoving);
    settings.mNumIterations = kClothSolverIterations;
    settings.mLinearDamping = kClothLinearDamping;
    settings.mVertexRadius = kClothVertexRadius;
    // The body's own position must not drift out from under vertices that are already
    // world-space, and Jolt is more accurate with a soft body's rotation left at identity.
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

    // A read lock rather than `GetBodyInterface`, because the vertices are motion-property
    // state rather than a transform and there is no interface call that hands them over.
    // Taken and released here, between steps, which is the only time this is called.
    const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(clothes[cloth].id));
    if (!lock.Succeeded()) return;
    const JPH::Body& body = lock.GetBody();
    if (!body.IsSoftBody()) return;

    const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    const JPH::RVec3 origin = body.GetCenterOfMassPosition();
    const size_t n = std::min(out.size(), static_cast<size_t>(motion->GetVertices().size()));
    for (size_t i = 0; i < n; ++i) {
        // Jolt keeps a soft body's vertices relative to its centre of mass. `mUpdatePosition`
        // is false so that origin does not move, but adding it back is still what makes
        // these world-space rather than nearly so.
        const JPH::Vec3 p = motion->GetVertex(static_cast<uint32_t>(i)).mPosition;
        out[i] = glm::vec3(static_cast<float>(origin.GetX()) + p.GetX(), static_cast<float>(origin.GetY()) + p.GetY(),
                           static_cast<float>(origin.GetZ()) + p.GetZ());
    }
}

namespace {

// GCC 12 reports `result.GetError()` below as reading an uninitialised
// `Ref<Shape>`. It is a false positive about `JPH::Result`, which is a tagged union:
// the two members share storage, the error string is only ever read after
// `HasError()` says the error member is the live one, and the compiler is tracking
// the offset rather than the tag. Scoped to these two functions rather than silenced
// for the file, and narrower still would mean not calling GetError() at all -- which
// would trade Jolt's account of why a shape was refused for "a shape was refused".
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

/// Build the shape a collider describes, in node space and before the node's scale.
/// Returns null when the description cannot produce one, having said why.
JPH::RefConst<JPH::Shape> makeShape(const ColliderDesc& desc) {
    JPH::ShapeSettings::ShapeResult result;

    switch (desc.resolvedShape()) {
    case ColliderShape::Box: {
        const glm::vec3 he = glm::max(desc.halfExtent, glm::vec3(1e-3f));
        // The convex radius has to fit inside the box or Jolt refuses it, and its
        // default of 0.05 is larger than a small crate's half-extent. Scaling it to the
        // shape is what lets the same schema describe a wall and a die.
        const float radius = std::min(0.05f, std::min({he.x, he.y, he.z}) * 0.5f);
        result = JPH::BoxShapeSettings(toJolt(he), radius).Create();
        break;
    }
    case ColliderShape::Sphere:
        result = JPH::SphereShapeSettings(std::max(desc.radius, 1e-3f)).Create();
        break;
    case ColliderShape::Capsule:
        result = JPH::CapsuleShapeSettings(std::max(desc.halfHeight, 1e-3f), std::max(desc.radius, 1e-3f)).Create();
        break;
    case ColliderShape::Cylinder:
        result = JPH::CylinderShapeSettings(std::max(desc.halfHeight, 1e-3f), std::max(desc.radius, 1e-3f)).Create();
        break;
    case ColliderShape::Hull: {
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
    case ColliderShape::Mesh: {
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
    case ColliderShape::Auto:
        // Unreachable: resolvedShape() never returns it. Listed rather than defaulted so
        // a shape added to the enum fails to compile here instead of silently falling
        // through to whatever the default arm did.
        return nullptr;
    }

    if (result.HasError()) {
        core::Logger::warn(core::LogCategory::Scene, "Collider '%s': %s", desc.name.c_str(), result.GetError().c_str());
        return nullptr;
    }

    JPH::RefConst<JPH::Shape> shape = result.Get();

    // The offset moves the shape within the node, which is what lets a capsule be
    // authored on a node at the character's feet rather than at its waist.
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

/// Apply a node's scale to a shape, correcting a scale the shape cannot take. A sphere
/// under a non-uniform scale is the case that matters: Jolt refuses it outright, and the
/// alternative to correcting it is a body that silently fails to exist.
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

BodyId PhysicsWorld::createBody(const ColliderDesc& desc, uint64_t userData) {
    if (impl == nullptr || !impl->initialised) return {};
    // Refused rather than routed. The old addBody() sent a Character motion to
    // addCharacter() and returned an index the caller could not tell apart from a body's;
    // with two handle types that ambiguity is a compile error waiting to be made, so the
    // caller picks the verb that matches what it is making.
    if (desc.motion == ColliderMotion::Character) return {};

    // Grown rather than refused (C40). `refused` now counts only what Jolt itself would not
    // give -- a shape it could not build, or a world past its own index ceiling -- so a
    // non-zero count is a defect again rather than a budget somebody guessed low.
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

    const bool moves = desc.motion != ColliderMotion::Static;
    const JPH::EMotionType motion = desc.motion == ColliderMotion::Dynamic  ? JPH::EMotionType::Dynamic
                                    : desc.motion == ColliderMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                               : JPH::EMotionType::Static;

    JPH::BodyCreationSettings settings(shape, toJolt(translation), toJolt(rotation), motion,
                                       moves ? kLayerMoving : kLayerStatic);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linearDamping;
    settings.mAngularDamping = desc.angularDamping;
    settings.mGravityFactor = desc.gravityFactor;
    settings.mUserData = userData;
    // The 2D constraint, and it is the solver's rather than ours (P7). Jolt zeroes the
    // disallowed rows of the inverse mass and inertia, so a confined body is never solved
    // off its plane -- there is nothing to correct afterwards and nothing to drift. A
    // static body has no motion properties to hold this and Jolt ignores it there, which
    // is why nothing here refuses the combination.
    settings.mAllowedDOFs =
        desc.freedom == ColliderFreedom::Plane2D ? JPH::EAllowedDOFs::Plane2D : JPH::EAllowedDOFs::All;
    if (desc.mass > 0.0f) {
        // The inertia tensor still comes from the shape; only the mass is overridden.
        // Overriding both would need an author to write a 3x3 matrix, which is not a
        // thing anyone does by hand.
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

    // A retired slot before a new one. Slots are never compacted -- four consumers key off
    // a body index the way they key off an instance slot -- so reuse is what keeps a world
    // that spawns and despawns from growing without bound.
    uint32_t index;
    if (!freeBodySlots.empty()) {
        index = freeBodySlots.back();
        freeBodySlots.pop_back();
        Body& slot = bodies[index];
        // The generation was already moved by destroy(). Reusing it here rather than
        // bumping it again is what makes exactly one stale-handle boundary per lifetime.
        slot.live = true;
        slot.id = raw;
        slot.userData = userData;
        slot.moves = moves;
        slot.kinematic = desc.motion == ColliderMotion::Kinematic;
    } else {
        index = static_cast<uint32_t>(bodies.size());
        Body slot;
        slot.id = raw;
        slot.userData = userData;
        slot.moves = moves;
        slot.kinematic = desc.motion == ColliderMotion::Kinematic;
        bodies.push_back(slot);
    }

    // The reverse direction, for queries. Written here rather than rebuilt on demand
    // because this is the only place a body is ever added.
    impl->bodyIndexById[raw] = index;
    return BodyId{index, bodies[index].generation};
}

PhysicsCharacterId PhysicsWorld::createCharacter(const ColliderDesc& desc, uint64_t userData) {
    if (impl == nullptr || !impl->initialised) return {};

    // No budget check here, deliberately, and not an oversight to be corrected: `capacity` is
    // the body count handed to `PhysicsSystem::Init`, and a CharacterVirtual is not tracked by
    // the system at all -- it moves by collision queries against a shape it owns. Refusing one
    // against the body budget would turn characters away over headroom they never consume.
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
    // The plane a supporting contact has to be below for the character to be considered
    // standing on it. Jolt's own value, expressed against the capsule's radius so a
    // character authored at any size gets the same behaviour.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -std::max(desc.radius, 1e-3f));

    // Adopted by the Ref on the same line it is made. Jolt hands back a raw pointer with a
    // refcount of zero, and holding one across the two container growths below meant an
    // allocation failure in either would drop the character on the floor. `Ref` takes the
    // first reference here; the assignments below take their own.
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
    return PhysicsCharacterId{index, c.generation};
}

void PhysicsWorld::destroy(BodyId id) {
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

void PhysicsWorld::destroy(PhysicsCharacterId id) {
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

    // --------------------------------------------------- read everything out of the old world
    //
    // Shapes are refcounted, so a `RefConst` held here outlives the system it came from --
    // which is what makes a capture-then-rebuild possible without re-deriving a single shape
    // from the `ColliderDesc` that is long gone.
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

        // Cloth is a soft body in the same system and rebuilds the same way, except that its
        // state is per particle: the shared settings carry the topology, and the vertices
        // carry where the solve had got to. Dropping the second would snap every hanging
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

    // A `CharacterVirtual` is not in the system, so nothing above reaches it -- and it holds
    // a `PhysicsSystem*` for its own collision queries, which is exactly why it cannot come
    // across intact. Its pose and velocity are read here and every window in `Character`
    // survives untouched, because that state was always this class's rather than Jolt's.
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

    // --------------------------------------------------------------------- the new world
    // `createSystem` replaces `impl` wholesale, which takes the old system, its broad phase
    // and every body in it. Nothing captured above points into it: the shapes are refs and
    // the rest is by value.
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
        // The shape already carries the mass properties it was created with, so nothing is
        // overridden here: re-deriving from `ColliderDesc::mass` would need the desc, and
        // overriding from the old body's mass would recompute an inertia tensor that is
        // already correct.
        JPH::Body* body = bi.CreateBody(settings);
        if (body == nullptr) {
            core::Logger::error(core::LogCategory::Scene, "Physics: grow lost a body at slot %u", s.slot);
            continue;
        }
        bi.AddBody(body->GetID(), s.active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        if (s.motion != JPH::EMotionType::Static) {
            bi.SetLinearAndAngularVelocity(body->GetID(), s.linear, s.angular);
        }

        // **The slot keeps its index and its generation, and only the raw id moves.** That
        // is the whole reason a growth is invisible to a game: a `BodyId` names a slot in
        // this class, never a `JPH::BodyID`.
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
        // **A fresh character has never swept, so it believes it is in mid-air.** Everything
        // the coyote window and the jump buffer are built on would restart -- a fighter
        // standing on the floor would report airborne for a step because something else
        // spawned. This is the same refresh `setCharacterTransform` does after a teleport,
        // and for the same reason.
        character->RefreshContacts(impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                                   impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);
        impl->characters[s.slot] = character;
    }

    // The broad phase is built from nothing, so it wants the same optimise a load does.
    impl->system.OptimizeBroadPhase();

    // Reported rather than silent: an allocation that doubled is a fact about the frame it
    // happened in, and a game tuning its floor needs to see where the world settled.
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
            // Only now, when nothing in Jolt refers to the slot, may it be handed out.
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
    // Both snapshots start equal, so a frame rendered before the first step interpolates
    // between a state and itself rather than between a state and zero.
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
        // A retired slot has no Jolt body to read. Its state is left as it was, which
        // nothing reads -- every accessor goes through valid() first.
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

    /**
     * **Each pair has to be the same length, and this block is what keeps them so.** Every
     * accessor interpolates `previous[slot]` against `current[slot]` behind a bounds check on
     * the current one alone, because `finalize()` starts them equal and `step()` assigns one
     * to the other.
     *
     * A thing created *after* load appends, and its earlier state does not exist. The new
     * slot therefore starts equal to `current`, which costs it one step without
     * interpolation on the step it was created. That is the honest answer: zero-filling
     * would draw it in from the origin.
     *
     * Since the four-array split there is nothing here about one kind shifting the other:
     * a body appended to `current` cannot move a character's slot, so the arrays are only
     * ever grown at the tail.
     */
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

    // Before anything else in the step, and outside it rather than inside: this is the
    // one moment at which removing a body from the system is safe. `destroy` only ever
    // queues.
    reclaim();

    // The last step's contacts stop being current here, in the same breath as the slots
    // their handles name -- which is what makes the whole gap between two steps a window
    // in which a game may read them *and* destroy what they name. Cleared before the
    // `empty()` return as well, so a world whose last body went away does not keep
    // reporting what it collided with.
    stepContacts.clear();
    impl->contacts.found.clear();

    if (empty()) return;

    previous = current;
    previousCharacters = currentCharacters;

    // Characters first, matching Jolt's own samples: a `CharacterVirtual` is not tracked
    // by the physics system, so it reads the world rather than being solved with it, and
    // reading it before the step is what makes its contacts agree with the state its
    // own sweep found.
    if (!characters.empty()) {
        const JPH::Vec3 gravity = toJolt(config.gravity);

        for (size_t i = 0; i < characters.size(); ++i) {
            Character& c = characters[i];
            if (!c.live) continue;
            JPH::CharacterVirtual& cv = *impl->characters[i];

            /*
             * ------------------------------------------------- the two windows (C20)
             *
             * Both are counted in *steps*, and this loop is the only thing that advances
             * them, which is what makes them independent of the frame rate. A window kept
             * in seconds and compared against an accumulator would be a window whose size
             * moved with the clock -- and G12 already found that sixty additions of
             * `1.0f/60.0f` land just under a second, so the seconds version would have
             * been off by a step at random as well.
             */
            const bool launchedLastStep = c.launched;
            c.launched = false;

            const JPH::CharacterBase::EGroundState ground = cv.GetGroundState();
            const bool standing = ground == JPH::CharacterBase::EGroundState::OnGround;

            // The sweep that would show a launch has not run yet, so the step *after* one
            // still reports standing. Refilling the windows there would hand back the
            // coyote time the launch just spent.
            if (standing && !launchedLastStep) {
                c.airSteps = 0;
                c.coyoteSpent = false;
            } else if (c.airSteps < kAirStepsMax) {
                ++c.airSteps;
            }

            // A press latched since the last step opens the buffer; it is not consumed
            // here, because the whole point is that it survives the steps in which it
            // cannot be acted on. `+ 1` so that a window of *zero* still allows the jump on
            // the step the press reaches, which is the only thing "no buffer" can mean.
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

            /*
             * ------------------------------------------------- the motion model (C20)
             *
             * The horizontal velocity *ramps* toward the request rather than being assigned
             * it. Written against the velocity relative to whatever the character is
             * standing on, so a ramp on a moving platform is a ramp against the platform
             * and not against the world.
             *
             * A rate large enough to cover the whole gap in one step reproduces the
             * assignment this replaced exactly, which is what makes "responsive" a number a
             * scene can author rather than a line a game had to edit `engine/` to change.
             *
             * Steep ground takes the airborne branch, and that is the answer to "what
             * happens on a face too steep to stand on": gravity accumulates and the
             * character slides down it. What changed is that it is now *visible* --
             * `characterGround` says `Sliding` where a bool said mid-air.
             */
            JPH::Vec3 velocity = cv.GetLinearVelocity();
            if (!standing) velocity += gravity * dt;

            // The ground's own velocity is the base, so a character standing on a moving
            // platform moves with it rather than being left behind by it.
            const JPH::Vec3 base = standing ? cv.GetGroundVelocity() : JPH::Vec3::sZero();
            JPH::Vec3 relative(velocity.GetX() - base.GetX(), 0.0f, velocity.GetZ() - base.GetZ());

            const JPH::Vec3 target = JPH::Vec3(c.moveDirection.x, 0.0f, c.moveDirection.z) * c.moveSpeed;
            // Which of the two rates applies is decided by whether the request is *faster*
            // than the current motion, not by whether it is zero: turning around at full
            // speed is a deceleration through the turn and an acceleration out of it, and
            // asking the question this way gets that for free.
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
                // platform is the platform's speed plus the character's. A coyote launch has
                // no ground under it to add to, and *replaces* the fall it was in -- a jump
                // taken late must not be a weaker jump.
                vertical = (standing ? base.GetY() : 0.0f) + c.jumpSpeed;
                c.coyoteSpent = true;
                c.launched = true;
            }
            cv.SetLinearVelocity(JPH::Vec3(base.GetX() + relative.GetX(), vertical, base.GetZ() + relative.GetZ()));

            // Per character rather than once for the loop, because `stepHeight` is
            // authored per collider and Jolt's defaults are absolute metres -- 0.4 up
            // against 0.5 down, sitting two hundred lines from an `mSupportingVolume` the
            // same header does scale to the capsule. The ratio between the pair is what
            // matters and is kept: the step-down has to reach further than the step-up, or
            // a character that walked up one stair hovers off the next.
            JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
            updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, c.stepHeight, 0.0f);
            updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -c.stepHeight * 1.25f, 0.0f);

            // **Taken before the sweep, because the sweep is the only thing that knows what
            // the request survived.** `ExtendedUpdate` slides the shape and leaves
            // `mLinearVelocity` exactly as it was set, so reading it back afterwards answers
            // with the line above rather than with the result -- a character pressed into a
            // wall reported a full-speed run for as long as the key was held.
            const JPH::RVec3 before = cv.GetPosition();

            cv.ExtendedUpdate(dt, gravity, updateSettings, impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                              impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);

            // **What the sweep did, relative to the ground rather than to the world.** This is
            // the number a locomotion state machine blends on, and gait is what the legs do,
            // not where the character ends up: standing still on a platform moving at 2 m/s is
            // `0`, and walking backwards along it at its own speed is `moveSpeed`, not
            // stillness. The ground is read after `ExtendedUpdate` rather than reusing `base`
            // from above, because the ground the character is on is one of the things that call
            // can change.
            //
            // The displacement carries every case the request cannot: a wall takes the
            // component into it away, a ramp is climbed at the speed the ramp allows, and a
            // stair-step arrives with the horizontal `WalkStairs` actually moved. It is *not*
            // fed back into the ramp above -- that integrator is C20's motion model and its
            // state is the request, so a fighter that leans on a column for a second still has
            // its speed when the column stops being in the way.
            //
            // Kept as a vector rather than collapsed to a length here: the direction is the
            // same measurement and a caller that wants a heading has nowhere else to get one
            // -- differencing `characterTransform` gets the carry back.
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
        // A manifold this class cannot name both ends of is dropped rather than reported
        // with an invalid handle, and the reasoning is `overlapSphere`'s: an event a caller
        // can ask no questions about is one it would have to filter out itself. Nothing
        // reaches this today -- every rigid body in the world came from `createBody`, and a
        // `CharacterVirtual` is not in the broad phase at all -- so it is the honest
        // treatment of a case rather than one that fires.
        if (!contact.a.valid() || !contact.b.valid()) continue;

        contact.point = raw.point;
        contact.normal = raw.normal;
        contact.speed = raw.speed;

        // Canonical: `a` is the lower slot. Jolt sorts the pair by `BodyID`, which is a
        // different order from this class's slots the moment a slot has been reused, so a
        // caller matching "did the player touch the door" cannot rely on Jolt's. The normal
        // is defined out of `a`, so swapping the pair has to negate it.
        if (contact.b.index < contact.a.index) {
            std::swap(contact.a, contact.b);
            contact.normal = -contact.normal;
        }
        stepContacts.push_back(contact);
    }

    // The order is the scene's, not the thread pool's. Two manifolds between the same pair
    // in one step -- a compound shape resting on two of its children -- are separated by
    // where they are, because after the pair that is the only thing left which is a
    // property of the collision rather than of which job finished first.
    std::sort(stepContacts.begin(), stepContacts.end(), [](const Contact& l, const Contact& r) {
        if (l.a.index != r.a.index) return l.a.index < r.a.index;
        if (l.b.index != r.b.index) return l.b.index < r.b.index;
        if (l.point.x != r.point.x) return l.point.x < r.point.x;
        if (l.point.y != r.point.y) return l.point.y < r.point.y;
        return l.point.z < r.point.z;
    });
}

glm::mat4 PhysicsWorld::bodyTransform(BodyId id, float alpha) const {
    if (!valid(id) || id.index >= current.size()) return glm::mat4(1.0f);
    return interpolateState(previous[id.index], current[id.index], alpha);
}

void PhysicsWorld::setBodyTransform(BodyId id, const glm::mat4& transform) {
    if (impl == nullptr || !valid(id)) return;
    if (!bodies[id.index].moves) {
        // Static only, since P7. A dynamic body being placed from outside used to be
        // refused here on the grounds that the solver owns its transform; it owns how the
        // body *moves*, and a respawn is not movement.
        core::Logger::warn(core::LogCategory::Scene,
                           "Physics: a static body cannot be placed after finalize(); ignoring");
        return;
    }

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(transform, translation, rotation, scale);

    JPH::BodyInterface& bi = impl->system.GetBodyInterface();

    /**
     * **A kinematic body is *moved*, not placed, and the difference is whether anything can
     * stand on it.** `SetPositionAndRotation` teleports: the body arrives with the velocity
     * it already had, which for a platform driven from a scene node is zero, for ever.
     * `CharacterVirtual::GetGroundVelocity` reads the velocity of the body under the
     * character's feet, so a character on a teleporting platform is handed a ground velocity
     * of zero and stands still while the platform slides out from under it -- which is
     * exactly what a rider on the demo's platform did.
     *
     * `MoveKinematic` sets the velocity that arrives at the target in one step instead, and
     * that velocity is the one the character reads. The body still ends the step exactly on
     * the target, so the mesh riding the same node is where the collider is.
     *
     * The step is the config's rather than a measured delta because that is the clock the
     * solver actually runs on. A frame that runs two steps therefore asks for the whole
     * frame's travel in one step's time and overshoots by one step, which the next sweep
     * corrects; a frame that runs none asks for no travel at all. Both are visible only
     * below the step rate, and the alternative -- threading a per-frame delta through a verb
     * whose whole point is that it is called from outside the step loop -- buys a wobble at
     * 30 fps with a parameter every caller has to get right.
     */
    if (bodies[id.index].kinematic) {
        bi.MoveKinematic(JPH::BodyID(bodies[id.index].id), toJolt(translation), toJolt(rotation),
                         std::max(config.step, 1e-6f));
    } else {
        bi.SetPositionAndRotation(JPH::BodyID(bodies[id.index].id), toJolt(translation), toJolt(rotation),
                                  JPH::EActivation::Activate);
    }

    // Both snapshots, not just the current one. A body placed rather than solved has no
    // previous state worth interpolating from, and leaving the old one would smear it
    // back to where it was for the remainder of the frame.
    if (id.index < current.size() && id.index < previous.size()) {
        current[id.index].position = translation;
        current[id.index].rotation = rotation;
        previous[id.index] = current[id.index];
    }
}

// ----------------------------------------------------------------------- motion (P7)

void PhysicsWorld::addImpulse(BodyId id, const glm::vec3& impulse) {
    if (impl == nullptr || !valid(id)) return;
    const Body& body = bodies[id.index];
    if (!body.moves || body.kinematic) {
        // Jolt's own `AddImpulse` checks `IsDynamic()` and returns without a word. Wrapping
        // it means the check happens once, here, with a name and a reason -- an impulse
        // absorbed silently is a crate that does not move and a log that says nothing.
        core::Logger::warn(core::LogCategory::Scene, "Physics: a %s body takes no impulse; ignoring",
                           body.kinematic ? "kinematic" : "static");
        return;
    }
    // Wakes it, which the body interface does and the raw `Body::AddImpulse` does not.
    impl->system.GetBodyInterface().AddImpulse(JPH::BodyID(body.id), toJolt(impulse));
}

void PhysicsWorld::setLinearVelocity(BodyId id, const glm::vec3& velocity) {
    if (impl == nullptr || !valid(id)) return;
    const Body& body = bodies[id.index];
    if (!body.moves) {
        core::Logger::warn(core::LogCategory::Scene, "Physics: a static body has no velocity to set; ignoring");
        return;
    }
    // The clamped form. The unclamped one asserts against `mMaxLinearVelocity` in a build
    // with Jolt's assertions on, which turns a caller's bad arithmetic into an abort in
    // debug and a working game in release -- the worst pair of behaviours to choose between.
    impl->system.GetBodyInterface().SetLinearVelocity(JPH::BodyID(body.id), toJolt(velocity));
}

glm::vec3 PhysicsWorld::linearVelocity(BodyId id) const {
    if (impl == nullptr || !valid(id)) return glm::vec3(0.0f);
    // Zero for a static body is the body interface's own answer, so there is no second test
    // for it here.
    return toGlm(impl->system.GetBodyInterface().GetLinearVelocity(JPH::BodyID(bodies[id.index].id)));
}

glm::mat4 PhysicsWorld::characterTransform(PhysicsCharacterId id, float alpha) const {
    if (!valid(id) || id.index >= currentCharacters.size()) return glm::mat4(1.0f);
    return interpolateState(previousCharacters[id.index], currentCharacters[id.index], alpha);
}

void PhysicsWorld::setCharacterTransform(PhysicsCharacterId id, const glm::mat4& transform) {
    if (impl == nullptr || !valid(id)) return;

    glm::vec3 translation;
    glm::quat rotation;
    glm::vec3 scale;
    decompose(transform, translation, rotation, scale);

    JPH::CharacterVirtual& cv = *impl->characters[id.index];
    cv.SetPosition(toJolt(translation));
    cv.SetRotation(toJolt(rotation));
    cv.SetLinearVelocity(JPH::Vec3::sZero());

    // **The ground state is stale the instant the character moves, and `step()` reads it
    // before it sweeps.** Without this a character placed over a pit reports standing for
    // one step -- long enough to spend a jump, refill the coyote window and ramp its
    // horizontal velocity against a platform that is no longer under it. Jolt does the
    // collision detection here instead, at the position just written.
    cv.RefreshContacts(impl->system.GetDefaultBroadPhaseLayerFilter(kLayerMoving),
                       impl->system.GetDefaultLayerFilter(kLayerMoving), {}, {}, *impl->temp);

    Character& c = characters[id.index];
    c.velocity = glm::vec3(0.0f);
    c.jump = false;
    c.jumpBuffer = 0u;
    c.launched = false;
    // What `createCharacter` leaves a character that has not been swept, and for its
    // reason: a placement into mid-air must not be able to jump out of it on the strength
    // of ground the character left behind. The step after this one refills both from the
    // contacts refreshed above.
    c.airSteps = kAirStepsMax;
    c.coyoteSpent = true;

    // Both snapshots, as `setBodyTransform` writes both: a character that moved without the
    // solver moving it has no previous state worth interpolating from, and leaving the old
    // one draws it crossing the map for the remainder of the frame.
    if (id.index < currentCharacters.size() && id.index < previousCharacters.size()) {
        currentCharacters[id.index].position = translation;
        currentCharacters[id.index].rotation = rotation;
        previousCharacters[id.index] = currentCharacters[id.index];
    }
}

void PhysicsWorld::setCharacterInput(PhysicsCharacterId id, const glm::vec3& moveDirection, bool jump) {
    if (!valid(id)) return;
    characters[id.index].moveDirection = moveDirection;
    // Latched rather than assigned: a jump pressed between two simulation steps would
    // otherwise be overwritten by the next frame's `false` and never reach the solver.
    characters[id.index].jump = characters[id.index].jump || jump;
}

float PhysicsWorld::characterSpeed(PhysicsCharacterId id) const {
    return valid(id) ? glm::length(characters[id.index].velocity) : 0.0f;
}

glm::vec3 PhysicsWorld::characterVelocity(PhysicsCharacterId id) const {
    return valid(id) ? characters[id.index].velocity : glm::vec3(0.0f);
}

float PhysicsWorld::characterMoveSpeed(PhysicsCharacterId id) const {
    return valid(id) ? characters[id.index].moveSpeed : 0.0f;
}

bool PhysicsWorld::characterOnGround(PhysicsCharacterId id) const {
    return characterGround(id) == CharacterGround::OnGround;
}

CharacterGround PhysicsWorld::characterGround(PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id)) return CharacterGround::InAir;
    switch (impl->characters[id.index]->GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround:
        return CharacterGround::OnGround;
    // Two of Jolt's four states are one answer here: a face past `maxSlopeAngle`, and a
    // body that is there but cannot hold the character up. Both mean the same thing to a
    // game -- something is under it and it is going down anyway.
    case JPH::CharacterBase::EGroundState::OnSteepGround:
    case JPH::CharacterBase::EGroundState::NotSupported:
        return CharacterGround::Sliding;
    case JPH::CharacterBase::EGroundState::InAir:
        break;
    }
    return CharacterGround::InAir;
}

glm::vec3 PhysicsWorld::characterGroundNormal(PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id) || characterGround(id) == CharacterGround::InAir) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    // Jolt keeps the last ground it found, so the air case is filtered above rather than
    // reported: a normal from a face the character left is worse than no normal at all.
    return toGlm(impl->characters[id.index]->GetGroundNormal());
}

BodyId PhysicsWorld::characterGroundBody(PhysicsCharacterId id) const {
    if (impl == nullptr || !valid(id) || characterGround(id) == CharacterGround::InAir) return {};
    const JPH::BodyID under = impl->characters[id.index]->GetGroundBodyID();
    // `handleFor` answers falsy for geometry this class did not create, which is the honest
    // report for another character's internal body.
    return under.IsInvalid() ? BodyId{} : handleFor(under.GetIndexAndSequenceNumber());
}

bool PhysicsWorld::characterJumped(PhysicsCharacterId id) const {
    return valid(id) && characters[id.index].launched;
}

// ------------------------------------------------------------------------ queries (C2)

BodyId PhysicsWorld::handleFor(uint32_t joltRawId) const {
    const auto it = impl->bodyIndexById.find(joltRawId);
    if (it == impl->bodyIndexById.end()) return {};
    const uint32_t slot = it->second;
    if (slot >= bodies.size() || !bodies[slot].live) return {};
    return BodyId{slot, bodies[slot].generation};
}

//
// Every one of these leaves the two layer filters at their defaults, on purpose and for
// the reason `segmentBlocked` has always given: every layer is solid to a query, because a
// wall and a closed door are in different ones and both stop a bullet as well as a sound.
// A game that wants to trace against one layer is the trigger to widen this, and widening
// it means a filter parameter rather than a second set of functions.

PhysicsWorld::RayHit PhysicsWorld::raycast(const glm::vec3& from, const glm::vec3& to, BodyId ignoreBody) const {
    RayHit hit;
    if (impl == nullptr || !impl->initialised) return hit;

    const glm::vec3 segment = to - from;
    const JPH::Vec3 origin = toJolt(from);
    const JPH::Vec3 delta = toJolt(segment);
    // A zero-length segment has nothing between its ends. Checked rather than left to
    // Jolt, which normalises the direction.
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

    // The normal needs the body, and reading a body means holding its lock -- the physics
    // system is free to be mid-step on another thread. `Succeeded()` is not a formality:
    // a body destroyed between the cast and the lock is exactly what the lock is for.
    const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        hit.normal = toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, toJolt(hit.point)));
    }

    // A hit on something this class did not add -- a character's internal body -- reports
    // its geometry honestly and `kNoBody` for the index, rather than pretending to a slot
    // the caller could look up. It is still a hit: `operator bool` reads `distance`, which
    // was set above, for exactly this case.
    return hit;
}

PhysicsWorld::RayHit PhysicsWorld::sphereCast(const glm::vec3& from, const glm::vec3& to, float radius,
                                              BodyId ignoreBody) const {
    RayHit hit;
    if (impl == nullptr || !impl->initialised || radius <= 0.0f) return hit;

    const glm::vec3 segment = to - from;
    if (glm::dot(segment, segment) < 1e-8f) return hit;

    const JPH::SphereShape sphere(radius);
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(toJolt(from)),
                               toJolt(segment));

    JPH::ShapeCastSettings settings;
    // The sweep starts inside whatever it starts inside -- a character probing downward
    // begins overlapping the floor it is standing on. Reporting those would make every
    // ground check hit at fraction zero.
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
    // Jolt's penetration axis points *into* body 2. A surface normal points out of it, and
    // out of it is what every caller means by "the normal I bounced off".
    hit.normal = -glm::normalize(toGlm(r.mPenetrationAxis));
    hit.distance = glm::length(segment) * r.mFraction;
    return hit;
}

uint32_t PhysicsWorld::overlapSphere(const glm::vec3& center, float radius, std::span<BodyId> out) const {
    if (impl == nullptr || !impl->initialised || radius <= 0.0f) return 0;

    const JPH::SphereShape sphere(radius);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl->system.GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f),
                                                    JPH::RMat44::sTranslation(toJolt(center)),
                                                    JPH::CollideShapeSettings{}, JPH::RVec3::sZero(), collector);

    // Counted separately from what is written, because the return value is the count and
    // the span is the storage. A body this class did not add is skipped rather than
    // reported as kNoBody: a caller iterating the results would have to filter it, and
    // "how many did you find" should not include ones it cannot ask about.
    uint32_t found = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const BodyId id = handleFor(r.mBodyID2.GetIndexAndSequenceNumber());
        if (!id.valid()) continue;
        if (found < out.size()) out[found] = id;
        ++found;
    }
    return found;
}

bool PhysicsWorld::segmentBlocked(const glm::vec3& from, const glm::vec3& to, BodyId ignoreBody) const {
    // Implemented over `raycast` rather than beside it. The boolean is still the right
    // return for the one caller it was written for -- audio occlusion asks whether, not
    // what -- but two casts of the same ray with the same filters was one of them waiting
    // to disagree with the other.
    return static_cast<bool>(raycast(from, to, ignoreBody));
}

void PhysicsWorld::drawDebug(std::vector<gfx::DebugLineVertex>& out, const glm::vec3& cameraPosition) {
    if (impl == nullptr || !impl->initialised) return;

    // `--physics-contacts` has been in `--help` since S4.5 and drew nothing at all, which
    // is what a flag with no reader looks like from the outside. Jolt's own contact drawing
    // happens *inside* the step, into `DebugRenderer::sInstance`, and the renderer below is
    // only attached to an output vector for the duration of this call -- so every line it
    // produced went into a null pointer. G7's stream is a record that outlives the step, so
    // there is finally something here to draw.
    if (debugContacts) {
        for (const Contact& contact : stepContacts) {
            // White for a graze, red for a hit. 6 m/s is roughly a one-and-a-half metre
            // fall, which is the speed at which a thing sounds like it landed.
            const float hardness = std::min(contact.speed / 6.0f, 1.0f);
            const uint32_t color = gfx::packDebugColor({1.0f, 1.0f - hardness, 1.0f - hardness, 1.0f});
            // A cross rather than a point, because a line list cannot draw a point, and
            // three axis-aligned segments read as a marker from any angle a normal-aligned
            // pair would foreshorten to nothing.
            constexpr float kArm = 0.06f;
            for (int axis = 0; axis < 3; ++axis) {
                glm::vec3 arm(0.0f);
                arm[axis] = kArm;
                out.push_back({contact.point - arm, color});
                out.push_back({contact.point + arm, color});
            }
            // And the normal, scaled by the impact, so the direction and the force are one
            // thing to look at rather than two.
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

} // namespace scene
