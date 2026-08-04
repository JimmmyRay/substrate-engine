#pragma once

#include "core/Handle.h"
#include "gfx/DebugLines.h"
#include "scene/Cloth.h"
#include "scene/Collider.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace scene {

/**
 * @file Physics.h
 * @brief The fixed-step clock and the rigid-body world.
 *
 * Not a wrapper written so a second solver could be dropped in. It owns what a Jolt world
 * needs to exist -- the layer tables, the job system, the temp allocator, the body list
 * and the two interpolation snapshots -- and there is exactly one of it.
 *
 * ## Determinism
 *
 * The golden images rest on frame N being the same image on every run, and three decisions
 * defend that here:
 *
 * - The step is fixed and the accumulator is `FixedClock`'s, so the number of steps taken
 *   before frame N is a function of the frame index and not of the machine.
 * - **`workerThreads` defaults to zero**, selecting Jolt's single-threaded job system.
 *   Jolt is deterministic for a *fixed* thread count, and zero is the count that is fixed
 *   on every machine. It also keeps the unit suite clean under ThreadSanitizer.
 * - **Bodies are created in the order the file declared them and never reordered**, so the
 *   `BodyID`s the solver's island ordering depends on are a function of the scene.
 *
 * Cross-platform determinism is a separate promise and Jolt's
 * `CROSS_PLATFORM_DETERMINISTIC` is deliberately off.
 */

/// Everything the world needs to exist, in one struct because `Config` hands it over in
/// one piece. Defaults are the engine's, not Jolt's, where the two disagree.
struct PhysicsConfig {
    /// The simulation step. Shared with animation and particles: three subsystems
    /// stepping at three rates would make "frame 60" mean three different times.
    float step = 1.0f / 60.0f;
    /// Steps a single frame may run before the rest of the accumulated time is dropped.
    /// Past this, time is discarded, counted and reported rather than chased into a frame
    /// that never ends.
    uint32_t maxStepsPerFrame = 4;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Bodies the world can hold. Zero means "size it from the scene", which is what
    /// makes this a budget rather than a capacity -- see `PhysicsWorld::init`.
    uint32_t bodyBudget = 0;
    /// Zero selects Jolt's single-threaded job system. Any other value is a thread pool
    /// of that size; the count is data because determinism depends on it being fixed,
    /// not on it being one.
    uint32_t workerThreads = 0;
    /// Collision sub-steps inside one `step`. Jolt's own default is 1.
    uint32_t collisionSteps = 1;
};

/**
 * @brief The accumulator that turns a variable frame rate into a fixed step.
 *
 * Belongs to the application rather than to the physics world -- animation and particles
 * step on it too, and a scene with no colliders still has one.
 *
 * **The locked clock is not a second path through this.** Feeding `accumulate` exactly
 * `step` lands the accumulator on exactly zero, in float, so one step runs and `alpha()`
 * is zero. Feeding it the frame's wall-clock `dt` is the realtime path. One code path, and
 * the selector is data.
 */
class FixedClock {
  public:
    explicit FixedClock(float step = 1.0f / 60.0f, uint32_t maxStepsPerFrame = 4)
        : stepSeconds(step > 0.0f ? step : 1.0f / 60.0f), maxSteps(maxStepsPerFrame) {}

    /// Add a frame's worth of time and begin a new frame's budget of steps. Negative
    /// time is ignored rather than run backwards, which a clock stepped across a system
    /// time change can otherwise be handed.
    void accumulate(float dt);

    /// True when another step should run this frame. Consumes it.
    [[nodiscard]] bool consume();

    /// How far into the next step the render time sits, in [0, 1). Exactly zero under a
    /// locked clock.
    [[nodiscard]] float alpha() const;

    /**
     * @brief Scale time before it is accumulated. 0 is paused, 1 is normal.
     *
     * **Pause is a time scale rather than a second concept.** An `isPaused` flag beside a
     * `timeScale` float raises the question of what `setTimeScale(1.0f)` does while paused,
     * and every answer is a rule somebody has to remember.
     *
     * Applied to what the accumulator *receives*, so animation, particles, physics and
     * audio occlusion all inherit it from where they already inherit their step. Rendering,
     * input and the UI sit outside the step and keep running.
     *
     * **Audio sources keep playing**: miniaudio owns that clock and this one cannot reach
     * it. Silence during a pause is `AudioEngine::setMuted`.
     *
     * Negative values clamp to zero, at the setter rather than one call later.
     */
    void setTimeScale(float scale) { timeScaleValue = scale > 0.0f ? scale : 0.0f; }
    [[nodiscard]] float timeScale() const { return timeScaleValue; }
    /// True when the scale has stopped time.
    [[nodiscard]] bool paused() const { return timeScaleValue == 0.0f; }

    [[nodiscard]] float step() const { return stepSeconds; }
    /// Steps taken since construction. The simulation's own frame counter.
    [[nodiscard]] uint64_t stepCount() const { return total; }
    /// Whole steps discarded by the per-frame cap. Reported when it changes, never
    /// silently -- time the simulation did not run is a fact a game needs.
    [[nodiscard]] uint32_t droppedSteps() const { return dropped; }
    [[nodiscard]] uint32_t stepsThisFrame() const { return thisFrame; }

  private:
    float stepSeconds;
    uint32_t maxSteps;
    float timeScaleValue = 1.0f;
    float accumulator = 0.0f;
    uint32_t thisFrame = 0;
    uint32_t dropped = 0;
    uint64_t total = 0;
};

/// Position and orientation of one body at one instant. Two of these per body -- before
/// the last step and after it -- are what the render state interpolates between.
struct PhysicsState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// The rendered transform `alpha` of the way from `a` to `b`. Slerped rather than
/// lerped-and-normalised: a body spinning fast enough for the two to differ visibly is
/// exactly the body a debug view is being pointed at.
[[nodiscard]] glm::mat4 interpolateState(const PhysicsState& a, const PhysicsState& b, float alpha);

/// Forward-declared and defined in the .cpp. The engine's one derived type, because
/// `JPH::DebugRendererSimple` is how Jolt hands out the wireframe of a convex hull or a
/// triangle mesh -- neither of which a procedural box outline can draw.
struct PhysicsDebugRenderer;

/// Tags that make a body handle and a character handle unrelated types. Declared, never
/// defined -- see `core/Handle.h`.
struct BodyTag;
struct PhysicsCharacterTag;

/// A rigid body in the physics world.
using BodyId = core::Handle<BodyTag>;
/// A `CharacterVirtual`. A different kind of thing from a body -- it is not solved with
/// the world, it reads it -- which is why it is a different handle type and not a body
/// with a flag. Named for the subsystem because `SceneAnimator` also has characters, and
/// they are not the same characters.
using PhysicsCharacterId = core::Handle<PhysicsCharacterTag>;

/**
 * @brief What a character is standing on, or not.
 *
 * **Three answers rather than two**, and the third is why this type exists: a face steeper
 * than the collider's `maxSlopeAngle` is neither ground nor mid-air. As `OnGround` a
 * character walks up a cliff; as `InAir` -- what a bool gave -- a game plays a fall clip
 * for something the solver is sliding down a surface.
 *
 * `Sliding` also covers Jolt's `NotSupported`, touching a body that cannot hold the
 * character up. Same answer to the same question, and no game acts on the two differently.
 */
enum class CharacterGround : uint8_t {
    InAir,    ///< Touching nothing.
    OnGround, ///< Standing. The only state a jump launches from, coyote window aside.
    Sliding,  ///< Touching a face too steep to stand on, or a body that cannot support it.
};

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
    void init(const PhysicsConfig& cfg, uint32_t expectedBodies);
    void shutdown();

    /// True until `init()` has been called, and for a scene that declared no collider --
    /// which is what makes the whole subsystem cost nothing rather than an empty step.
    ///
    /// **Cloth counts.** A scene whose only physics is a hanging curtain has no rigid body
    /// and no character, and a world reporting itself empty there returns out of `step()`
    /// before the solver runs.
    [[nodiscard]] bool empty() const { return bodies.empty() && characters.empty() && clothes.empty(); }

    /**
     * @brief Create one body. Returns an invalid handle when it was refused.
     *
     * **Two create verbs rather than one**, against principles.md rule 8's table: this
     * class makes two kinds of thing with two handle types, and a single `create` cannot
     * overload on its return type. `destroy` *is* overloaded.
     *
     * **A `desc.motion == Character` is refused rather than routed to `createCharacter`**,
     * which would return a handle the caller could not tell apart from a body's.
     */
    BodyId createBody(const ColliderDesc& desc, uint64_t userData = 0);

    /// Create a `CharacterVirtual`. Invalid handle when it was refused.
    PhysicsCharacterId createCharacter(const ColliderDesc& desc, uint64_t userData = 0);

    /**
     * @brief Retire a body or a character. A second destroy of the same handle is a no-op.
     *
     * **The slot is not reused immediately.** Jolt cannot have a body removed from under a
     * step in progress, so the generation moves *now* -- the handle goes stale on the call
     * -- while the slot returns to the free list only once the Jolt body is gone, at the
     * top of the next `step()`.
     */
    void destroy(BodyId id);
    void destroy(PhysicsCharacterId id);

    /// The handle currently occupying a slot, for a caller iterating `0..bodyCount()`.
    /// Invalid for an empty slot, which is what makes the walk skip retired ones.
    [[nodiscard]] BodyId bodyAt(uint32_t slot) const {
        if (slot >= bodies.size() || !bodies[slot].live) return {};
        return BodyId{slot, bodies[slot].generation};
    }
    [[nodiscard]] PhysicsCharacterId characterAt(uint32_t slot) const {
        if (slot >= characters.size() || !characters[slot].live) return {};
        return PhysicsCharacterId{slot, characters[slot].generation};
    }

    /// Does this handle still name a live body? False for a destroyed one, for a handle
    /// from a different world, and for a default-constructed one.
    [[nodiscard]] bool valid(BodyId id) const {
        return id.valid() && id.index < bodies.size() && bodies[id.index].generation == id.generation &&
               bodies[id.index].live;
    }
    [[nodiscard]] bool valid(PhysicsCharacterId id) const {
        return id.valid() && id.index < characters.size() && characters[id.index].generation == id.generation &&
               characters[id.index].live;
    }

    /// Tell the broad phase that the static set is final. Cheap to skip and expensive to
    /// have skipped: without it every static body sits in the tree the way it was
    /// inserted, and a scene's floor is one long thin box the whole world overlaps.
    void finalize();

    /// Advance exactly one step. The accumulator is the caller's; this is what it calls.
    void step(float dt);

    // ---------------------------------------------------------------- bodies
    [[nodiscard]] uint32_t bodyCount() const { return static_cast<uint32_t>(bodies.size()); }
    /// Bounds-checked, like every accessor below: storing what a refused `createBody`
    /// returned is the ordinary way to use it, so a caller can reach these with one.
    [[nodiscard]] uint64_t bodyUserData(BodyId id) const { return valid(id) ? bodies[id.index].userData : 0u; }
    /// True for a body the solver moves, so a caller can skip pushing a static one into an
    /// instance every frame. False for no such body, which makes the skip correct too.
    [[nodiscard]] bool bodyMoves(BodyId id) const { return valid(id) && bodies[id.index].moves; }
    /// True for a body that moves but is not solved for -- a platform, a door, a lift. A
    /// dynamic body's transform is the solver's to report; a kinematic one's is something
    /// else's to write, which is the distinction `bodyMoves` cannot make.
    [[nodiscard]] bool bodyKinematic(BodyId id) const { return valid(id) && bodies[id.index].kinematic; }
    /// World transform, interpolated `alpha` of the way through the step in flight.
    [[nodiscard]] glm::mat4 bodyTransform(BodyId id, float alpha) const;

    /**
     * @brief Put a body where something outside the solver says it is.
     *
     * Dynamic and kinematic both -- a respawn, a level reset and a portal are the same
     * operation. **Static is refused**: a static body's place in the broad phase was
     * decided when it was inserted, and `finalize()` builds that tree once.
     *
     * **It teleports rather than sweeps.** Jolt's `MoveKinematic` derives a velocity from a
     * target and a time, which needs the step length, and this is called from a frame. So
     * it sets position, rotation, **and both interpolation snapshots** -- a body that moved
     * without the solver moving it has no previous state worth interpolating from, and
     * leaving one smears it back for a frame.
     *
     * **A dynamic body keeps its velocity.** Zeroing it would make a portal impossible;
     * the other way round is one extra `setLinearVelocity(id, {})`.
     *
     * Scale is dropped -- a Jolt body has no scale, and the node's went into the shape when
     * the body was created.
     */
    void setBodyTransform(BodyId id, const glm::mat4& transform);

    // ------------------------------------------------------------------------- motion
    /**
     * @brief Push a body, in kilogram-metres per second.
     *
     * Applied at the centre of mass, so it accelerates without spinning. An off-centre push
     * is a different call and there is not one yet.
     *
     * **Dynamic only; a non-dynamic body is refused with a reason rather than ignored.** An
     * impulse divided by an infinite mass is zero, so a kinematic or static body absorbing
     * one silently is a game whose crate does not move and whose log says nothing -- which
     * is what Jolt's own `AddImpulse` does and this does not.
     *
     * Wakes the body, or the impulse would arrive whenever something else woke it.
     */
    void addImpulse(BodyId id, const glm::vec3& impulse);

    /**
     * @brief Set a body's linear velocity outright, in metres per second.
     *
     * Where `addImpulse` accumulates and scales by mass, this replaces -- the call for
     * anything whose speed is a decision rather than a consequence.
     *
     * **Kinematic bodies too**: a kinematic body *is* moved by its velocity, and it is how
     * a lift carries what stands on it rather than shearing through it. Static is refused.
     *
     * A component the body's `freedom` does not allow is dropped by the solver rather than
     * by this call, so asking a `Plane2D` body to move along Z is a no-op and not a warning.
     *
     * Clamped to Jolt's maximum linear velocity rather than asserted against it, so a
     * caller multiplying by a bad `dt` gets a fast body instead of an abort.
     */
    void setLinearVelocity(BodyId id, const glm::vec3& velocity);

    /// What the body is moving at now, in metres per second. Read from the solver rather
    /// than from an interpolation snapshot -- there is no half-step velocity, and the two
    /// snapshots hold positions. Zero for a static body, a stale handle, and a world that
    /// has not been stepped.
    [[nodiscard]] glm::vec3 linearVelocity(BodyId id) const;

    // -------------------------------------------------------------------------- cloth
    /// What `createCloth` returns when it refused.
    static constexpr uint32_t kNoCloth = 0xFFFFFFFFu;

    /**
     * @brief Add a welded cloth to the world as a Jolt soft body.
     *
     * **A plain index rather than a `core::Handle`**: a generation exists to make a
     * *destroyed* thing's id report staleness, and nothing destroys one cloth -- it lives
     * from scene placement to world teardown. `kNoCloth` is the only invalid value.
     *
     * **Nothing about it is stepped by this class.** The body goes into the same
     * `PhysicsSystem` every rigid body is in, so `PhysicsSystem::Update` inside `step()`
     * solves it, it collides with every collider `Collider.h` can author, and it inherits
     * the determinism argument above unchanged.
     *
     * @return `kNoCloth` for an uninitialised world, a topology with no faces, or a body
     *         Jolt refused. Counted against the same budget bodies are.
     */
    uint32_t createCloth(const ClothTopology& topology);

    /// How many soft bodies the world holds.
    [[nodiscard]] uint32_t clothCount() const { return static_cast<uint32_t>(clothes.size()); }
    /// Particles in one cloth, or zero for an index that names none. What a caller sizes
    /// its readback span from.
    [[nodiscard]] uint32_t clothParticleCount(uint32_t cloth) const;
    /**
     * @brief The solved particle positions, in world space.
     *
     * Written into the caller's storage, up to `out.size()`, so reading a pose costs no
     * allocation.
     *
     * **World space, not relative to the centre of mass.** Jolt stores a soft body's
     * vertices relative to the body's centre of mass and moves the body under them; this
     * adds the body transform back, because a cloth instance's transform is identity and
     * the renderer wants vertices it can draw without one.
     */
    void clothPositions(uint32_t cloth, std::span<glm::vec3> out) const;

    /// Bodies refused past the budget since init(). Non-zero means the scene declared
    /// more colliders than the world was sized for.
    [[nodiscard]] uint32_t refusedBodies() const { return refused; }
    /// What `init` settled on, after the budget and the scene were both considered.
    [[nodiscard]] uint32_t bodyCapacity() const { return capacity; }

    // ------------------------------------------------------------ characters
    [[nodiscard]] uint32_t characterCount() const { return static_cast<uint32_t>(characters.size()); }
    [[nodiscard]] uint64_t characterUserData(PhysicsCharacterId id) const {
        return valid(id) ? characters[id.index].userData : 0u;
    }
    [[nodiscard]] glm::mat4 characterTransform(PhysicsCharacterId id, float alpha) const;
    /**
     * @brief Put a character where something outside the solver says it is.
     *
     * A respawn, a checkpoint, a portal and a loaded save -- the cases `setBodyTransform`
     * already names for rigid bodies. **It teleports**: the velocity is zeroed, both
     * interpolation snapshots are written so no frame draws the character sliding in from
     * where it was, and the ground state is recomputed *here* rather than at the next step,
     * because `step()` reads that state before it sweeps. The jump windows start over as
     * they do for a character that has just been created, so a placement into mid-air
     * carries no coyote time across from wherever it was standing. Scale is dropped.
     */
    void setCharacterTransform(PhysicsCharacterId id, const glm::mat4& transform);
    /// Where the character is being asked to go, in world space, and whether it is being
    /// asked to jump. Set once per frame from input; consumed by the next step.
    void setCharacterInput(PhysicsCharacterId id, const glm::vec3& moveDirection, bool jump);
    /// Horizontal speed in metres per second after the last step. What a locomotion state
    /// machine takes as its `speed` parameter.
    [[nodiscard]] float characterSpeed(PhysicsCharacterId id) const;
    /**
     * @brief The horizontal velocity `characterSpeed` is the length of, in m/s. Y is zero.
     *
     * **Relative to the ground it is standing on, which is the whole reason this exists.**
     * The obvious way to find out where a character is going is to difference
     * `characterTransform` across a frame, and that answer includes whatever is carrying it:
     * a character standing still on a moving platform reads as walking at the platform's
     * speed, in the platform's direction. A game turning a mesh to face its heading gets a
     * character that pirouettes with the lift. This has the carry already taken out.
     *
     * Still what the solver *did* rather than what it was asked for -- a ramp is climbed at
     * the speed the ramp allows and a wall slides the direction along it -- so it is a
     * heading, not a restatement of `setCharacterInput`. That comes from differencing the
     * swept position: **Jolt's `GetLinearVelocity` is the request**, unchanged by the sweep,
     * and reading it back is the edit that makes this sentence false again.
     */
    [[nodiscard]] glm::vec3 characterVelocity(PhysicsCharacterId id) const;
    /// This character's top speed, from the `ColliderDesc` that made it. What
    /// `characterSpeed` is divided by to get a machine's normalised `speed` -- exposed
    /// because that division used to be a constant a game guessed, and the number it was
    /// guessing at is here (G15).
    [[nodiscard]] float characterMoveSpeed(PhysicsCharacterId id) const;
    /// **Standing**, and only standing -- a character sliding down a face steeper than its
    /// `maxSlopeAngle` answers false here. `characterGround` is the call that tells that
    /// case apart from mid-air.
    [[nodiscard]] bool characterOnGround(PhysicsCharacterId id) const;
    /// The three-valued answer. A caller that only wants "can it act" wants
    /// `characterOnGround`; one that has to *animate* the difference wants this.
    [[nodiscard]] CharacterGround characterGround(PhysicsCharacterId id) const;
    /**
     * @brief The upward normal of whatever the character is standing on or sliding down.
     *
     * Straight up for a character in the air, which is the only answer that is not a stale
     * face from wherever it last stood. A slope lean, a ski, a wall run and "is this ramp
     * too steep to bother animating" all start here, and none of them could be written
     * against `characterGround`'s three values alone.
     */
    [[nodiscard]] glm::vec3 characterGroundNormal(PhysicsCharacterId id) const;
    /**
     * @brief What the character is standing on, or a falsy handle.
     *
     * A moving platform, a conveyor and an enemy's head are the same `OnGround` to
     * `characterGround` and are three different things to a game. Falsy in the air, and
     * falsy for geometry this class handed out no handle for.
     */
    [[nodiscard]] BodyId characterGroundBody(PhysicsCharacterId id) const;
    /**
     * @brief Did the last step actually launch this character?
     *
     * **A game cannot derive this and must not try.** `pressed(jump) && characterOnGround`
     * matches the controller's decision only until the launch gains a coyote window and a
     * buffer -- a press in the air launches, and a press on the ground can be delayed by a
     * step or two. This reports what the solver did, one step after it did it.
     */
    [[nodiscard]] bool characterJumped(PhysicsCharacterId id) const;

    // ------------------------------------------------------------------------ queries
    /**
     * @brief What a query hit, or a falsy value for a miss.
     *
     * One record shared by every query below, so a caller that handles a raycast handles a
     * sphere cast. `explicit operator bool` lets
     * `if (const auto hit = physics.raycast(...))` read as a question.
     *
     * **It tests `distance`, not `body`.** A query can strike geometry this class handed
     * out no handle for -- a character's internal body is the case that exists -- and that
     * is a real hit with a real point and normal. Keying the test on `body` would report
     * those as misses, which for `segmentBlocked` means a sound audible through a
     * character.
     */
    struct RayHit {
        BodyId body;
        /// Where the surface was struck, in world space.
        glm::vec3 point{0.0f};
        /// The surface normal there, pointing out of the body that was hit.
        glm::vec3 normal{0.0f};
        /// Metres from `from` to `point`, so a caller comparing two hits needs no
        /// subtraction. **Negative for a miss**, which is what makes a default-constructed
        /// `RayHit` falsy without a second flag to keep in step.
        float distance = -1.0f;

        [[nodiscard]] explicit operator bool() const { return distance >= 0.0f; }
    };

    /**
     * @brief The closest thing between `from` and `to`.
     *
     * Interaction, picking, ground checks, weapon traces and AI perception are all this
     * call. `segmentBlocked` is implemented over it and documents `ignoreBody`.
     *
     * @return a falsy `RayHit` for a miss, an empty world, or a zero-length segment.
     */
    [[nodiscard]] RayHit raycast(const glm::vec3& from, const glm::vec3& to, BodyId ignoreBody = {}) const;

    /**
     * @brief The closest thing a sphere of `radius` sweeping from `from` to `to` touches.
     *
     * A ray with thickness, which is what a ground check and a character probe want: a
     * zero-width ray falls through the gap between two floor tiles and a sphere does not.
     *
     * **A sphere rather than an arbitrary shape.** A general `shapeCast` would need a
     * `ColliderDesc`, which carries mesh data a query has no business building per call.
     */
    [[nodiscard]] RayHit sphereCast(const glm::vec3& from, const glm::vec3& to, float radius,
                                    BodyId ignoreBody = {}) const;

    /**
     * @brief Every body overlapping a sphere, into the caller's storage.
     *
     * @param out where the body indices go. Filled up to its size and no further.
     * @return **how many bodies overlapped, not how many were written**, so a result
     *         greater than `out.size()` says the answer was truncated. Returning the number
     *         written is indistinguishable from having found exactly that many.
     */
    [[nodiscard]] uint32_t overlapSphere(const glm::vec3& center, float radius, std::span<BodyId> out) const;

    /**
     * @brief Is anything solid between `from` and `to`?
     *
     * Deliberately the narrowest query that answers audio occlusion -- a boolean, not a hit
     * record. Against every layer, including the moving one: a closed door occludes, and a
     * door is kinematic.
     *
     * @param ignoreBody a body the segment passes through as if it were not there.
     *        **Without it a sound bolted to a crate reports itself permanently occluded by
     *        the crate**, because the source sits at the crate's centre and the ray drawn
     *        to it enters the crate first. Trimming the ray at both ends does not fix it:
     *        the trim is a constant and the crate is whatever size the scene made it.
     *
     * @return true when the segment is blocked. False for an empty world, so a caller
     *         needs no second test.
     */
    [[nodiscard]] bool segmentBlocked(const glm::vec3& from, const glm::vec3& to,
                                      BodyId ignoreBody = {}) const;

    // ----------------------------------------------------------------------- contacts
    /**
     * @brief Two bodies that **began** to touch during a step.
     *
     * Began, not touching. A settled stack persists a dozen contacts every step forever,
     * and a game diffing them frame over frame would be rebuilding bookkeeping the solver
     * already keeps. A parting contact is not reported either: Jolt hands it over with the
     * bodies unreadable and possibly already destroyed, so there is no point, normal or
     * speed to put in these fields.
     *
     * **`a` is always the lower slot of the pair and `normal` points out of it**, so a game
     * matching a pair writes one test rather than two and the stream can be ordered by a
     * property of the collision rather than of the job thread that found it.
     */
    struct Contact {
        /// The lower of the two slots. Both are live when `contacts()` is first readable,
        /// and either may be destroyed mid-walk -- see `contacts()`.
        BodyId a;
        BodyId b;
        /// Where they met, in world space: the **centroid** of the manifold rather than its
        /// first point, because a box landing flat produces four and the solver's ordering
        /// among them is arbitrary.
        glm::vec3 point{0.0f};
        /// Out of `a`, toward `b`.
        glm::vec3 normal{0.0f};
        /**
         * @brief How fast the two were closing along `normal`, in metres per second.
         *
         * **Read before the solver resolved the contact**, which makes it the impact rather
         * than the rebound -- a value taken afterwards is the velocity the response chose,
         * and is near zero for exactly the contacts that were loudest.
         *
         * Clamped at zero: a speculative contact can be detected while the two are still
         * separating.
         */
        float speed = 0.0f;
    };

    /**
     * @brief What the last `step()` found. Valid until the next one.
     *
     * Recorded during the step and handed over afterwards -- **nothing dispatches into game
     * code mid-step**, where every body is locked, the thread is one the caller did not
     * create, and creating or destroying a body deadlocks.
     *
     * A game reads this from `Game::fixedUpdate`, which runs *before* the engine's movers,
     * so what it sees is the previous step's contacts. One step of latency, and every
     * step's contacts are drained exactly once.
     *
     * **Destroying a body named here is safe from inside the walk.** `destroy` moves the
     * generation immediately, so remaining contacts naming it stop validating on that call,
     * and the slot cannot be reused until `reclaim()` runs at the top of the next `step()`
     * -- which is also where this list is cleared.
     *
     * **Ordered by `a`, then `b`, then position**, so the stream is a function of the scene
     * rather than of `physics.workerThreads`.
     */
    [[nodiscard]] std::span<const Contact> contacts() const { return stepContacts; }

    // -------------------------------------------------------------------------- debug
    /// Append every body's wireframe, and the contacts of the last step, to `out`.
    /// A no-op when the world is empty, so the caller needs no second test.
    void drawDebug(std::vector<gfx::DebugLineVertex>& out, const glm::vec3& cameraPosition);

    /// Draw contact points as well as shapes. Off by default: on a settled stack it is
    /// four crosses per box and it hides the shapes underneath.
    bool debugContacts = false;

  private:
    /// The lifetime pair every slot in this class carries: a generation that moves on every
    /// destroy, and the flag saying whether the slot holds anything now. It starts at 1
    /// because `Handle::valid()` reserves generation 0 for "never issued".
    struct Slot {
        uint32_t generation = 1;
        bool live = true;
    };

    struct Body : Slot {
        uint32_t id = 0; ///< JPH::BodyID's raw value, so this header needs no Jolt
        uint64_t userData = 0;
        bool moves = false;
        /// Moves, but not because the solver moved it. Beside `moves` rather than replacing
        /// it -- "does this need pushing anywhere" has the same answer for both kinds.
        bool kinematic = false;
    };

    struct Character : Slot {
        uint64_t userData = 0;
        glm::vec3 moveDirection{0.0f};
        bool jump = false;
        /// Horizontal and ground-relative, written at the end of `step`. `characterSpeed` is
        /// its length rather than a second field, so the two cannot come to disagree.
        glm::vec3 velocity{0.0f};

        // The tuning, copied out of the `ColliderDesc` that made it -- see that struct for
        // what each one means. Copied rather than referenced because the desc is the
        // scene's and does not outlive the load.
        float moveSpeed = 4.0f;
        float jumpSpeed = 4.5f;
        float acceleration = 10.0f;
        float deceleration = 40.0f;
        float airControl = 0.35f;
        float stepHeight = 0.35f;
        uint32_t jumpBufferSteps = 10;
        uint32_t coyoteSteps = 6;
        /// The capsule radius the supporting-volume plane was built from. Kept because
        /// `grow()` rebuilds every character against a new system and Jolt hands back the
        /// shape, the mass and the slope angle but not that plane -- see `createCharacter`.
        float capsuleRadius = 0.3f;

        // ------------------------------------------------- the two windows, in steps
        /// Consecutive steps not standing, saturating. Zero while the character stands, so
        /// `airSteps <= coyoteSteps` is the whole coyote test.
        uint32_t airSteps = 0;
        /// Steps a latched press has left to find ground, zero when nothing is pending.
        uint32_t jumpBuffer = 0;
        /// A launch spends the coyote window; standing again refills it. Without this a
        /// press held across the window is a second jump out of thin air.
        bool coyoteSpent = false;
        /// What the last step did. Read by `characterJumped`, and read again at the top of
        /// the next step: the ground state lags the launch by one sweep, so the step after
        /// a jump still reports standing and would otherwise refill both windows.
        bool launched = false;
    };

    /// The handle for a Jolt body, given the raw `BodyID` value `Body::id` stores. Invalid
    /// for geometry this class did not create -- a character's internal body is the case
    /// that exists. Takes a raw `uint32_t` so this header still needs no Jolt.
    [[nodiscard]] BodyId handleFor(uint32_t joltRawId) const;

    /// Reclaim the slots whose Jolt objects `destroy` retired. Called at the top of
    /// `step()`, which is the only point at which removing a body from the system is
    /// safe -- see `destroy`.
    void reclaim();

    /// Build `impl` and its Jolt system at `bodyCapacity`. Shared by `init` and `grow`
    /// rather than copied into both: the two must construct an identical world or a growth
    /// silently changes the simulation, and a second copy is where that drift would start.
    void createSystem(uint32_t bodyCapacity);

    /**
     * @brief Rebuild the world so it holds at least `needed` bodies, carrying everything
     *        across. False only when Jolt refuses the new system.
     *
     * **Jolt fixes its body count at `Init` and has no resize path** -- `BodyManager::Init`
     * reserves the body array outright so a `Body*` held by a solver job across a step can
     * never be invalidated. Growing is therefore a rebuild, and this is where the engine
     * absorbs that rather than making a game state a ceiling it cannot know.
     *
     * Every handle survives: an id is an index and a generation into this class's own
     * vectors, and only the Jolt raw id inside each slot changes. Characters are recreated
     * rather than moved, because a `CharacterVirtual` holds the system it queries.
     *
     * **Between steps only.** Called from the create verbs, which is the same window
     * `reclaim` runs in and for the same reason.
     */
    bool grow(uint32_t needed);

    /// Turn what the contact listener recorded during the step into `stepContacts`: Jolt's
    /// body ids become handles, the pair is canonicalised and the list is ordered. Once,
    /// after the step, on the stepping thread -- which keeps the work *inside* the step
    /// down to a few flops and a `push_back`.
    void collectContacts();

    /// Read every body's position and orientation into `current`. One pass, after the
    /// step, rather than a query per body at draw time: the caller reads all of them.
    void snapshot();

    PhysicsConfig config;
    uint32_t capacity = 0;
    uint32_t refused = 0;

    /// One soft body. `particles` is cached because reading it back means asking Jolt for a
    /// motion-properties pointer, and the count never changes after creation.
    struct ClothBody {
        uint32_t id = 0;
        uint32_t particles = 0;
    };

    std::vector<Body> bodies;
    std::vector<Character> characters;
    /// Never compacted and never reordered: the index is what `ClothSystem` stores, and the
    /// solver's island ordering depends on the `BodyID`s being a function of the scene.
    std::vector<ClothBody> clothes;

    /// Slots whose object is gone and which the next create may reuse. Only reached
    /// through `reclaim()`, so a slot is never handed out while Jolt still holds
    /// something in it.
    std::vector<uint32_t> freeBodySlots;
    std::vector<uint32_t> freeCharacterSlots;
    /// Destroyed between steps and not yet removed from Jolt. Drained by `reclaim()`.
    std::vector<uint32_t> pendingBodyRemoval;
    std::vector<uint32_t> pendingCharacterRemoval;
    /**
     * @brief Two snapshots per body and two per character, in **four arrays rather than
     *        two**, so a slot's index never depends on how many of the other kind exist.
     *
     * It was one pair, bodies first and characters after them. That layout made a body
     * created between two steps shift every character along, so `characterTransform` ran off
     * the end of the array and answered identity until the next `snapshot()` rebuilt it --
     * a character that teleported to the origin for one frame because something else spawned.
     * `snapshot()` repaired the layout afterwards, which is a frame too late for the game
     * that read it.
     *
     * Growth made that the ordinary case rather than a corner: creating a body at runtime is
     * now something the engine encourages, so the layout has to be one a create cannot
     * disturb. Each pair is still always the same length as its partner, which is what every
     * accessor relies on -- bounds-check the current, then index both.
     */
    std::vector<PhysicsState> previous;
    std::vector<PhysicsState> current;
    std::vector<PhysicsState> previousCharacters;
    std::vector<PhysicsState> currentCharacters;

    /// What the last `step()` found. Cleared rather than freed at the top of the next one,
    /// so a world with a busy contact count allocates once.
    std::vector<Contact> stepContacts;

    /// Everything Jolt owns, in one holder defined in the .cpp.
    ///
    /// **This is ownership, not portability.** There is no second solver behind it. The
    /// pointer is here because the three layer tables, the temp allocator and the job
    /// system take constructor arguments only `init()` knows and several hold references
    /// to each other; the alternative is five `unique_ptr` members and Jolt's headers on
    /// the include path of every file that touches a scene.
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace scene
