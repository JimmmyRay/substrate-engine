#pragma once

#include "core/Slot.h"
#include "gfx/DebugLines.h"
#include "scene/Body.h"
#include "scene/CharacterMotion.h"
#include "scene/Cloth.h"
#include "scene/Collider.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace physics {

/**
 * @file engine/physics/PhysicsWorld.h
 * @brief The rigid-body world, and the soft bodies solved alongside it.
 *
 * What a document authors -- colliders, the world's configuration, the handles a body is
 * named by -- is `scene/Collider.h` and `scene/Body.h`, where the loaders reach it.
 *
 * The golden images rest on frame N being the same image on every run, and three things here
 * hold that up. Breaking any one of them breaks the golden set on some machines and not
 * others:
 *
 * - The step is fixed and the accumulator is `scene::FixedClock`'s, so the step count before
 *   frame N is a function of the frame index, not of the machine.
 * - `workerThreads` defaults to zero, selecting Jolt's single-threaded job system. Jolt is
 *   deterministic for a *fixed* thread count, and zero is the only count fixed everywhere.
 * - Bodies are created in file order and never reordered, so the `BodyID`s the solver's
 *   island ordering depends on are a function of the scene.
 *
 * This header names no Jolt type; adding one puts Jolt on the include path of everything that
 * links physics.
 */

/// Position and orientation of one body at one instant. Two per body -- before the last step
/// and after it -- are what the render state interpolates between.
struct PhysicsState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// The rendered transform `alpha` of the way from `a` to `b`. Slerped, not
/// lerped-and-normalised: a body spinning fast enough for the two to differ visibly is
/// exactly the body a debug view is being pointed at.
[[nodiscard]] glm::mat4 interpolateState(const PhysicsState& a, const PhysicsState& b, float alpha);

/// Defined in the .cpp, where Jolt's headers are.
struct PhysicsDebugRenderer;

class PhysicsWorld {
  public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /**
     * @brief Create the world.
     *
     * @param expectedBodies how many colliders the scene declared. Jolt needs a fixed
     *        maximum at init, so the ceiling is that count plus stated headroom, unless
     *        `cfg.bodyBudget` names one. Past it, `createBody` refuses, counts and reports.
     */
    void init(const scene::PhysicsConfig& cfg, uint32_t expectedBodies);
    void shutdown();

    /// True until `init()`, and for a scene that declared no collider. `step()` returns on it,
    /// so cloth must stay in the test: a scene whose only physics is a hanging curtain has no
    /// rigid body and no character, and would never be solved.
    [[nodiscard]] bool empty() const { return bodies.empty() && characters.empty() && clothes.empty(); }

    /**
     * @brief Create one body. Returns an invalid handle when it was refused.
     *
     * A `desc.motion == Character` is refused rather than routed to `createCharacter`, which
     * would return a handle the caller could not tell apart from a body's.
     */
    scene::BodyId createBody(const scene::ColliderDesc& desc, uint64_t userData = 0);

    /// Create a `CharacterVirtual`. Invalid handle when it was refused.
    scene::PhysicsCharacterId createCharacter(const scene::ColliderDesc& desc, uint64_t userData = 0);

    /**
     * @brief Retire a body or a character. A second destroy of the same handle is a no-op.
     *
     * The generation moves now, so the handle goes stale on this call, but the slot returns to
     * the free list only at the top of the next `step()`: Jolt cannot have a body removed from
     * under a step in progress.
     */
    void destroy(scene::BodyId id);
    void destroy(scene::PhysicsCharacterId id);

    /// The handle currently occupying a slot, for a caller iterating `0..bodyCount()`.
    /// Invalid for a retired slot, which is what makes such a walk skip them.
    [[nodiscard]] scene::BodyId bodyAt(uint32_t slot) const {
        if (slot >= bodies.size() || !bodies[slot].live) return {};
        return scene::BodyId{slot, bodies[slot].generation};
    }
    [[nodiscard]] scene::PhysicsCharacterId characterAt(uint32_t slot) const {
        if (slot >= characters.size() || !characters[slot].live) return {};
        return scene::PhysicsCharacterId{slot, characters[slot].generation};
    }

    /// Does this handle still name a live body? False for a destroyed one, for a handle
    /// from a different world, and for a default-constructed one.
    [[nodiscard]] bool valid(scene::BodyId id) const {
        return id.valid() && id.index < bodies.size() && bodies[id.index].generation == id.generation &&
               bodies[id.index].live;
    }
    [[nodiscard]] bool valid(scene::PhysicsCharacterId id) const {
        return id.valid() && id.index < characters.size() && characters[id.index].generation == id.generation &&
               characters[id.index].live;
    }

    /// Tell the broad phase that the static set is final. Skipping it leaves every static body
    /// in the tree the way it was inserted, so a scene's floor is one long thin box the whole
    /// world overlaps.
    void finalize();

    /// Advance exactly one step. The accumulator is the caller's.
    void step(float dt);

    [[nodiscard]] uint32_t bodyCount() const { return static_cast<uint32_t>(bodies.size()); }
    /// Bounds-checked, as every accessor below is: passing a handle a refused `createBody`
    /// returned is ordinary use, not a caller error.
    [[nodiscard]] uint64_t bodyUserData(scene::BodyId id) const { return valid(id) ? bodies[id.index].userData : 0u; }
    /// True for a body the solver moves; false also for a stale handle, so a caller skipping
    /// static bodies skips those too.
    [[nodiscard]] bool bodyMoves(scene::BodyId id) const { return valid(id) && bodies[id.index].moves; }
    /// True for a body that moves but is not solved for -- a platform, a door, a lift. A
    /// dynamic body's transform is the solver's to report; a kinematic one's is something
    /// else's to write, which `bodyMoves` cannot distinguish.
    [[nodiscard]] bool bodyKinematic(scene::BodyId id) const { return valid(id) && bodies[id.index].kinematic; }
    /// World transform, interpolated `alpha` of the way through the step in flight.
    [[nodiscard]] glm::mat4 bodyTransform(scene::BodyId id, float alpha) const;

    /**
     * @brief Put a body where something outside the solver says it is.
     *
     * Dynamic and kinematic both. Static is refused: its place in the broad phase was fixed
     * when it was inserted and `finalize()` builds that tree once.
     *
     * Teleports rather than sweeps, and writes *both* interpolation snapshots -- leaving the
     * previous one smears the body back from where it was for a frame.
     *
     * A dynamic body keeps its velocity, which is what makes a portal possible; zeroing it is
     * one extra `setLinearVelocity(id, {})`. Scale is dropped: a Jolt body has none, and the
     * node's went into the shape at creation.
     */
    void setBodyTransform(scene::BodyId id, const glm::mat4& transform);

    /**
     * @brief Push a body, in kilogram-metres per second.
     *
     * Applied at the centre of mass, so it accelerates without spinning.
     *
     * Dynamic only, and a non-dynamic body is refused with a reason: an impulse divided by an
     * infinite mass is zero, so accepting one silently -- as Jolt's own `AddImpulse` does --
     * is a game whose crate does not move and whose log says nothing.
     *
     * Wakes the body, or the impulse arrives whenever something else does.
     */
    void addImpulse(scene::BodyId id, const glm::vec3& impulse);

    /**
     * @brief Set a body's linear velocity outright, in metres per second.
     *
     * Replaces, where `addImpulse` accumulates and scales by mass.
     *
     * Kinematic bodies too -- a kinematic body *is* moved by its velocity, which is how a lift
     * carries what stands on it rather than shearing through it. Static is refused.
     *
     * A component the body's `freedom` forbids is dropped by the solver, not here, so asking a
     * `Plane2D` body to move along Z is a silent no-op. Clamped to Jolt's maximum rather than
     * asserted against it, so a caller multiplying by a bad `dt` gets a fast body, not an abort.
     */
    void setLinearVelocity(scene::BodyId id, const glm::vec3& velocity);

    /// What the body is moving at now, in metres per second, read from the solver -- there is
    /// no half-step velocity to interpolate. Zero for a static body, a stale handle, and a
    /// world that has not been stepped.
    [[nodiscard]] glm::vec3 linearVelocity(scene::BodyId id) const;

    /// What `createCloth` returns when it refused.
    static constexpr uint32_t kNoCloth = 0xFFFFFFFFu;

    /**
     * @brief Add a welded cloth to the world as a Jolt soft body.
     *
     * A plain index, not a `core::Handle`: nothing destroys one cloth, so there is no stale
     * index to detect. `kNoCloth` is the only invalid value.
     *
     * The soft body goes into the same `PhysicsSystem` the rigid bodies are in, so `step()`
     * solves it with no code of its own here.
     *
     * @return `kNoCloth` for an uninitialised world, a topology with no faces, or a body Jolt
     *         refused. Counted against the same budget bodies are.
     */
    uint32_t createCloth(const scene::ClothTopology& topology);

    /// How many soft bodies the world holds.
    [[nodiscard]] uint32_t clothCount() const { return static_cast<uint32_t>(clothes.size()); }
    /// Particles in one cloth, or zero for an index that names none. What a caller sizes its
    /// readback span from.
    [[nodiscard]] uint32_t clothParticleCount(uint32_t cloth) const;
    /**
     * @brief The solved particle positions, in world space, into the caller's storage.
     *
     * World space, not Jolt's: Jolt stores a soft body's vertices relative to the centre of
     * mass and moves the body under them, and this adds the body transform back. A cloth
     * instance's transform is identity, so the renderer has nothing else to apply.
     */
    void clothPositions(uint32_t cloth, std::span<glm::vec3> out) const;

    /// Bodies refused past the budget since init(). Non-zero means the scene declared more
    /// colliders than the world was sized for.
    [[nodiscard]] uint32_t refusedBodies() const { return refused; }
    /// What `init` settled on, after the budget and the scene were both considered.
    [[nodiscard]] uint32_t bodyCapacity() const { return capacity; }

    [[nodiscard]] uint32_t characterCount() const { return static_cast<uint32_t>(characters.size()); }
    [[nodiscard]] uint64_t characterUserData(scene::PhysicsCharacterId id) const {
        return valid(id) ? characters[id.index].userData : 0u;
    }
    [[nodiscard]] glm::mat4 characterTransform(scene::PhysicsCharacterId id, float alpha) const;
    /**
     * @brief Put a character where something outside the solver says it is.
     *
     * Teleports: the velocity is zeroed and both interpolation snapshots are written, so no
     * frame draws the character sliding in from where it was. The ground state is recomputed
     * here rather than at the next step, because `step()` reads it before it sweeps. Both jump
     * windows reset, so a placement into mid-air carries no coyote time across from wherever
     * it was standing. Scale is dropped.
     */
    void setCharacterTransform(scene::PhysicsCharacterId id, const glm::mat4& transform);
    /// Where the character is being asked to go, in world space, and whether it is being
    /// asked to jump. Set once per frame from input; consumed by the next step.
    void setCharacterInput(scene::PhysicsCharacterId id, const glm::vec3& moveDirection, bool jump);
    /// Horizontal speed in metres per second after the last step.
    [[nodiscard]] float characterSpeed(scene::PhysicsCharacterId id) const;
    /**
     * @brief The horizontal velocity `characterSpeed` is the length of, in m/s. Y is zero.
     *
     * Relative to the ground it stands on, which is why differencing `characterTransform`
     * across a frame is not a substitute: that answer includes the carry, so a character
     * standing still on a lift reads as walking at the lift's speed and a mesh turned to face
     * its heading pirouettes.
     *
     * Comes from differencing the swept position, so it is what the solver did -- a ramp
     * climbed at the speed the ramp allows, a wall slid along. Jolt's `GetLinearVelocity` is
     * the *request*, unchanged by the sweep; reading it back here reintroduces that.
     */
    [[nodiscard]] glm::vec3 characterVelocity(scene::PhysicsCharacterId id) const;
    /// This character's top speed in m/s, from the `scene::ColliderDesc` that made it. What
    /// `characterSpeed` is divided by for a state machine's normalised `speed`.
    [[nodiscard]] float characterMoveSpeed(scene::PhysicsCharacterId id) const;
    /// Standing, and only standing: a character sliding down a face steeper than its
    /// `maxSlopeAngle` answers false. `characterGround` tells that case apart from mid-air.
    [[nodiscard]] bool characterOnGround(scene::PhysicsCharacterId id) const;
    /// The three-valued answer, for a caller that has to *animate* the difference.
    [[nodiscard]] scene::CharacterGround characterGround(scene::PhysicsCharacterId id) const;
    /// The upward normal of whatever the character stands on or slides down. Straight up in
    /// the air, rather than the stale face it last stood on.
    [[nodiscard]] glm::vec3 characterGroundNormal(scene::PhysicsCharacterId id) const;
    /// What the character is standing on. Falsy in the air, and falsy for geometry this class
    /// handed out no handle for.
    [[nodiscard]] scene::BodyId characterGroundBody(scene::PhysicsCharacterId id) const;
    /**
     * @brief Did the last step actually launch this character?
     *
     * A game cannot derive this: `pressed(jump) && characterOnGround` disagrees with the
     * controller wherever the coyote window and the jump buffer do their work -- a press in
     * the air launches, a press on the ground can be delayed a step or two.
     */
    [[nodiscard]] bool characterJumped(scene::PhysicsCharacterId id) const;

    /**
     * @brief The four readouts above, for a controller named by `core::packHandle`.
     *
     * @return false for a handle this world does not hold, which is what retires a pairing
     *         whose controller has been destroyed.
     *
     * A packed key rather than a `scene::PhysicsCharacterId` because the only caller reaches this
     * through a `core::Slot`, from a directory that may not name this one.
     */
    [[nodiscard]] bool characterMotion(uint64_t controller, scene::CharacterMotion* out) const;

    /**
     * @brief What a query hit, or a falsy value for a miss.
     *
     * The boolean conversion tests `distance`, never `body`: a query can strike geometry this
     * class handed out no handle for -- a character's internal body -- and that is a real hit
     * with a real point and normal. Keying it on `body` makes a sound audible through a
     * character.
     */
    struct RayHit {
        scene::BodyId body;
        /// Where the surface was struck, in world space.
        glm::vec3 point{0.0f};
        /// The surface normal there, pointing out of the body that was hit.
        glm::vec3 normal{0.0f};
        /// Metres from `from` to `point`. Negative for a miss, which is what makes a
        /// default-constructed `RayHit` falsy with no second flag to keep in step.
        float distance = -1.0f;

        [[nodiscard]] explicit operator bool() const { return distance >= 0.0f; }
    };

    /**
     * @brief The closest thing between `from` and `to`.
     *
     * `segmentBlocked` is implemented over it and documents `ignoreBody`.
     *
     * @return a falsy `RayHit` for a miss, an empty world, or a zero-length segment.
     */
    [[nodiscard]] RayHit raycast(const glm::vec3& from, const glm::vec3& to, scene::BodyId ignoreBody = {}) const;

    /**
     * @brief The closest thing a sphere of `radius` sweeping from `from` to `to` touches.
     *
     * A ray with thickness, which is what a ground check wants: a zero-width ray falls through
     * the gap between two floor tiles and a sphere does not.
     */
    [[nodiscard]] RayHit sphereCast(const glm::vec3& from, const glm::vec3& to, float radius,
                                    scene::BodyId ignoreBody = {}) const;

    /**
     * @brief Every body overlapping a sphere, into the caller's storage.
     *
     * @param out where the body indices go. Filled up to its size and no further.
     * @return how many bodies overlapped, not how many were written, so a result greater than
     *         `out.size()` says the answer was truncated. Returning the number written is
     *         indistinguishable from having found exactly that many.
     */
    [[nodiscard]] uint32_t overlapSphere(const glm::vec3& center, float radius, std::span<scene::BodyId> out) const;

    /**
     * @brief Is anything solid between `from` and `to`?
     *
     * Tests every layer including the moving one, because a closed door occludes and a door is
     * kinematic.
     *
     * @param ignoreBody a body the segment passes through as if it were not there. Without it
     *        a sound bolted to a crate reports itself permanently occluded by the crate, since
     *        the source sits at the crate's centre. Trimming the ray at both ends is not a
     *        substitute: the trim is a constant and the crate is whatever size the scene made
     *        it.
     *
     * @return true when the segment is blocked. False for an empty world.
     */
    [[nodiscard]] bool segmentBlocked(const glm::vec3& from, const glm::vec3& to,
                                      scene::BodyId ignoreBody = {}) const;

    /**
     * @brief Two bodies that *began* to touch during a step.
     *
     * Began, not touching: a settled stack would persist a dozen contacts every step forever.
     *
     * `a` is always the lower slot of the pair and `normal` points out of it, which is what
     * lets a game match a pair with one test and lets the stream be ordered by a property of
     * the collision rather than of the job thread that found it.
     */
    struct Contact {
        /// The lower of the two slots. Both live when `contacts()` is first readable; either
        /// may be destroyed mid-walk -- see `contacts()`.
        scene::BodyId a;
        scene::BodyId b;
        /// Where they met, in world space: the *centroid* of the manifold, because a box
        /// landing flat produces four points and the solver's ordering among them is arbitrary.
        glm::vec3 point{0.0f};
        /// Out of `a`, toward `b`.
        glm::vec3 normal{0.0f};
        /**
         * @brief How fast the two were closing along `normal`, in metres per second.
         *
         * Read before the solver resolves the contact, so it is the impact. Taken afterwards
         * it is the velocity the response chose, which is near zero for exactly the contacts
         * that were loudest.
         *
         * Clamped at zero: a speculative contact can be detected while the two still separate.
         */
        float speed = 0.0f;
    };

    /**
     * @brief What the last `step()` found. Valid until the next one.
     *
     * Recorded during the step and handed over afterwards. Nothing may dispatch into game code
     * mid-step: every body is locked, the thread is one the caller did not create, and
     * creating or destroying a body there deadlocks.
     *
     * A game reads this from `Game::fixedUpdate`, which runs before the engine's movers, so it
     * sees the previous step's contacts -- one step of latency, drained exactly once.
     *
     * Destroying a body named here is safe from inside the walk: `destroy` moves the
     * generation immediately, so remaining contacts naming it stop validating, and the slot
     * cannot be reused until `reclaim()` at the top of the next `step()`, which is also where
     * this list is cleared.
     *
     * Ordered by `a`, then `b`, then position, so the stream is a function of the scene rather
     * than of `physics.workerThreads`.
     */
    [[nodiscard]] std::span<const Contact> contacts() const { return stepContacts; }

    /// Append every body's wireframe, and the contacts of the last step, to `out`.
    void drawDebug(std::vector<gfx::DebugLineVertex>& out, const glm::vec3& cameraPosition);

    /// Draw contact points as well as shapes. Off by default: on a settled stack it is four
    /// crosses per box and it hides the shapes underneath.
    bool debugContacts = false;

  private:
    /// The lifetime pair every slot in this class carries. The generation starts at 1 because
    /// `Handle::valid()` reserves 0 for "never issued".
    struct Slot {
        uint32_t generation = 1;
        bool live = true;
    };

    struct Body : Slot {
        uint32_t id = 0; ///< JPH::BodyID's raw value, so this header needs no Jolt
        uint64_t userData = 0;
        bool moves = false;
        /// Moves, but not because the solver moved it. Both kinds set `moves`.
        bool kinematic = false;
    };

    struct Character : Slot {
        uint64_t userData = 0;
        glm::vec3 moveDirection{0.0f};
        bool jump = false;
        /// Horizontal and ground-relative, written at the end of `step`. `characterSpeed` is
        /// its length rather than a second field, so the two cannot disagree.
        glm::vec3 velocity{0.0f};

        // Copied out of the `scene::ColliderDesc` that made it -- see that struct. Copied, not
        // referenced: the desc is the scene's and does not outlive the load.
        float moveSpeed = 4.0f;
        float jumpSpeed = 4.5f;
        float acceleration = 10.0f;
        float deceleration = 40.0f;
        float airControl = 0.35f;
        float stepHeight = 0.35f;
        uint32_t jumpBufferSteps = 10;
        uint32_t coyoteSteps = 6;
        /// The capsule radius the supporting-volume plane was built from. Kept because `grow()`
        /// rebuilds every character and Jolt hands back the shape, the mass and the slope angle
        /// but not that plane -- see `createCharacter`.
        float capsuleRadius = 0.3f;

        /// Consecutive steps not standing, saturating. Zero while standing, so
        /// `airSteps <= coyoteSteps` is the whole coyote test.
        uint32_t airSteps = 0;
        /// Steps a latched press has left to find ground, zero when nothing is pending.
        uint32_t jumpBuffer = 0;
        /// A launch spends the coyote window; standing again refills it. Without it a press
        /// held across the window is a second jump out of thin air.
        bool coyoteSpent = false;
        /// What the last step did. Read by `characterJumped`, and again at the top of the next
        /// step: the ground state lags the launch by one sweep, so the step after a jump still
        /// reports standing and would otherwise refill both windows.
        bool launched = false;
    };

    /// The handle for a Jolt body, given the raw `BodyID` value `Body::id` stores. Invalid for
    /// geometry this class did not create, such as a character's internal body. Takes a raw
    /// `uint32_t` so this header still needs no Jolt.
    [[nodiscard]] scene::BodyId handleFor(uint32_t joltRawId) const;

    /// Reclaim the slots whose Jolt objects `destroy` retired. Must run at the top of `step()`:
    /// that is the only point at which removing a body from the system is safe.
    void reclaim();

    /// Build `impl` and its Jolt system at `bodyCapacity`. Shared by `init` and `grow`: a
    /// second copy is where a growth would start silently constructing a different world.
    void createSystem(uint32_t bodyCapacity);

    /**
     * @brief Rebuild the world so it holds at least `needed` bodies, carrying everything
     *        across. False only when Jolt refuses the new system.
     *
     * Jolt fixes its body count at `Init` and has no resize path -- `BodyManager::Init`
     * reserves the array outright so a `Body*` held by a solver job cannot be invalidated --
     * so growing is a rebuild.
     *
     * Every handle survives: an id indexes this class's own vectors, and only the Jolt raw id
     * inside each slot changes. Characters are recreated rather than moved, because a
     * `CharacterVirtual` holds the system it queries.
     *
     * Between steps only, the same window `reclaim` runs in and for the same reason.
     */
    bool grow(uint32_t needed);

    /// Turn what the contact listener recorded during the step into `stepContacts`. Runs once,
    /// after the step, on the stepping thread, which is what keeps the work inside the step to
    /// a few flops and a `push_back`.
    void collectContacts();

    /// Read every body's position and orientation into `current`, in one pass after the step.
    void snapshot();

    scene::PhysicsConfig config;
    uint32_t capacity = 0;
    uint32_t refused = 0;

    /// One soft body. `particles` is cached because reading it back means asking Jolt for a
    /// motion-properties pointer, and it never changes after creation.
    struct ClothBody {
        uint32_t id = 0;
        uint32_t particles = 0;
    };

    std::vector<Body> bodies;
    std::vector<Character> characters;
    /// Never compacted and never reordered: the index is what `ClothSystem` stores, and the
    /// solver's island ordering depends on the `BodyID`s being a function of the scene.
    std::vector<ClothBody> clothes;

    /// Slots whose object is gone and which the next create may reuse. Only ever filled by
    /// `reclaim()`, so a slot is never handed out while Jolt still holds something in it.
    std::vector<uint32_t> freeBodySlots;
    std::vector<uint32_t> freeCharacterSlots;
    /// Destroyed between steps and not yet removed from Jolt. Drained by `reclaim()`.
    std::vector<uint32_t> pendingBodyRemoval;
    std::vector<uint32_t> pendingCharacterRemoval;
    /**
     * @brief Two snapshots per body and two per character, in four arrays rather than two, so
     *        a slot's index never depends on how many of the other kind exist.
     *
     * Packing bodies and characters into one pair makes a body created between two steps shift
     * every character along, and `characterTransform` then answers identity until the next
     * `snapshot()` -- a character that teleports to the origin for one frame because something
     * else spawned.
     *
     * Each array is always the same length as its partner, which every accessor relies on:
     * bounds-check the current, then index both.
     */
    std::vector<PhysicsState> previous;
    std::vector<PhysicsState> current;
    std::vector<PhysicsState> previousCharacters;
    std::vector<PhysicsState> currentCharacters;

    /// What the last `step()` found. Cleared rather than freed at the top of the next one,
    /// so a world with a busy contact count allocates once.
    std::vector<Contact> stepContacts;

    /// Everything Jolt owns, in one holder defined in the .cpp. The members inside it take
    /// constructor arguments only `init()` knows and hold references to each other, so their
    /// declaration order there is load-bearing on teardown.
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * @brief `world` in the shape a locomotion driver reads it, keyed by packed controller.
 *
 * The slot holds a bare pointer, so `world` has to outlive every step that uses it. Taking
 * it fresh each step, as the module does, is what makes that true by construction.
 */
[[nodiscard]] inline core::Slot<bool(uint64_t, scene::CharacterMotion*)> characterMotionSource(PhysicsWorld& world) {
    return core::Slot<bool(uint64_t, scene::CharacterMotion*)>::bind<&PhysicsWorld::characterMotion>(&world);
}

} // namespace physics
