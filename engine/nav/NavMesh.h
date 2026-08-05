#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/nav/NavMesh.h
 * @brief Walkable surface, path through it, and steering along the path (C12).
 *
 * ## What this is
 *
 * A **triangle navmesh**: the scene's triangles, filtered to the ones an agent could stand
 * on, welded into a connected surface with edge adjacency, searched with A*, and pulled
 * straight with the funnel algorithm. Three stages, one file, no classes beyond the mesh
 * itself -- `bake`, `findPath` and `steer` are the whole surface.
 *
 * ## What it is not, and why the difference is worth stating
 *
 * It is **not** a voxel navmesh. Recast's answer -- rasterise the world into height
 * spans, erode by the agent radius, extract contours, triangulate -- is the industry
 * standard and it buys two things this does not have:
 *
 * - **Overhangs and clearance.** A voxel field knows there is a ceiling 1.2 m above a
 *   floor and refuses to walk a 1.8 m agent under it. A triangle mesh has no idea; the
 *   floor is walkable and the ceiling is simply another surface that failed the slope
 *   test. `agentHeight` is therefore **not** a parameter here, because accepting one
 *   would be a promise this cannot keep.
 * - **True radius erosion.** Voxelisation shrinks the walkable region by the agent radius
 *   before the mesh exists, so every point on it is legal. Here the radius is applied
 *   during string-pulling, by insetting each portal (see `NavBuildParams::agentRadius`).
 *   That keeps a path off the walls it passes *through* a portal, and does nothing about
 *   a wall it merely passes *beside*.
 *
 * Those are real limits and they are written down rather than discovered. What the
 * triangle approach buys is that it is exact where it applies: no rasterisation error, no
 * cell size to tune, and a path whose vertices lie on the source geometry rather than on a
 * quantised approximation of it. For a scene authored with its walkable surfaces as actual
 * floors -- which is every scene this engine loads -- it is the right first answer, and the
 * voxel field is the row that follows it if a game needs clearance.
 *
 * ## What stands on a floor is cut out of it
 *
 * A slope filter on its own answers a question nobody asked. A floor authored as one large
 * quad with columns resting on it passes that filter whole, so the columns contribute
 * nothing but their tops and **every route across the floor is a straight line through all
 * of them** -- while the triangle count, the region count and the word "baked" all look
 * healthy. So `bake` reads the geometry it is about to throw away: a triangle too steep to
 * walk that reaches above a walkable surface and touches or crosses it is *standing on* it,
 * and where it stands the surface is cut and whatever ends up enclosed is dropped.
 *
 * **Standing on and above are different questions, and only the first one cuts.** A bridge
 * over a floor takes nothing away from it -- that is the clearance limit above rather than a
 * second one -- and neither does the underside of the floor itself. Asking about contact
 * instead of about headroom is what lets this need no agent height, so `agentHeight` stays
 * absent for exactly the reason it always was.
 *
 * The hole is the footprint and not a quantisation of it: the pieces stay convex and the
 * splitting is arithmetic, so a cut costs triangles and introduces no cell size and no
 * rasterisation error -- which is the property the triangle approach is here for.
 *
 * ## Coordinates
 *
 * The solver works in Y up. "Slope" is the angle between a triangle's normal and +Y, and
 * every triangle that survives the filter is wound so its normal points up -- which is what
 * lets the funnel treat a shared edge's two vertices as left and right without asking which
 * triangle it came from.
 *
 * **The world it is baked from need not be** (D18). `NavBuildParams::up` says which way is
 * up out there, `bake` rotates the triangles into the frame above, and every query rotates
 * back -- so a flat world in XY, which is where `ColliderFreedom::Plane2D` puts a 2D game's
 * bodies, gets a navmesh in the same plane as the bodies that walk it. +Y is the identity
 * and performs no arithmetic at all.
 */
namespace nav {

/// What `bake` is allowed to decide. Every field is a property of the *agent*, not of the
/// scene, which is why one scene can carry several navmeshes and none of this lives in the
/// glTF.
struct NavBuildParams {
    /// Steepest surface an agent will stand on, in degrees from horizontal. 45 is the
    /// conventional default and the one every authored ramp is built under.
    float walkableSlopeDegrees = 45.0f;

    /**
     * @brief How far a path is kept from a portal's endpoints, in metres.
     *
     * Applied during string-pulling rather than during the bake -- see the file comment.
     * A portal narrower than twice this is not discarded; it is inset to its midpoint,
     * because a corridor an agent cannot quite fit through is still better described as a
     * tight squeeze than as a wall. **The bake does not remove triangles for it**, so a
     * navmesh baked at radius 0.4 and queried at 0.4 is the same mesh as one baked at 0.
     */
    float agentRadius = 0.4f;

    /// Vertices within this distance of each other become one. What turns a triangle soup
    /// -- where two adjacent floor tiles have four coincident-but-distinct corners -- into
    /// a surface with adjacency. Too small and the mesh is islands; too large and a
    /// doorway welds shut.
    float weldEpsilon = 0.01f;

    /// Connected regions with less total area than this are dropped. A scene bakes dozens
    /// of one-triangle scraps -- a windowsill, the top of a crate -- and each is a region
    /// an agent can never reach and `nearest` can always snap to.
    float minRegionArea = 1.0f;

    /**
     * @brief Which way is up in the world these triangles came from (D18).
     *
     * `+Y` for a 3D scene, which is every scene this engine loads. **`+Z` is what a flat
     * world wants**, because the plane a 2D game is played in is XY --
     * `ColliderFreedom::Plane2D` says so, gravity is -Y, the orthographic camera looks
     * down -Z and a sprite's layer is its depth. Without this the only navmesh a game
     * could bake was XZ, so a 2D game's bodies and its navmesh lived in perpendicular
     * planes and one of the two had to be rewritten by the game.
     *
     * It rotates rather than swizzles. A permutation of the axes flips handedness, and the
     * funnel reads a portal's left and right off a winding that only holds in a
     * right-handed basis -- so the bake turns the world into the solver's own frame with a
     * rotation whose determinant is 1, and every query turns back. `+Y` is the identity and
     * takes no arithmetic at all, so a 3D scene is bit-for-bit what it was.
     */
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

/**
 * @brief One walkable triangle and how it connects.
 *
 * `neighbour[i]` is across the edge from `v[i]` to `v[(i + 1) % 3]`, so an index means the
 * same thing in both arrays and walking a corridor never needs a search to find which edge
 * two triangles share.
 */
struct NavTriangle {
    uint32_t v[3]{};
    uint32_t neighbour[3]{};
    /// Connected component. Two triangles with different regions have no path between
    /// them, which is what makes `reachable` a comparison rather than a search.
    uint32_t region = 0;
};

/**
 * @brief A position on the mesh: which triangle, and where in it.
 *
 * Carried as a pair rather than as a bare `vec3` because every query that follows needs
 * the triangle, and re-deriving it from the point is the expensive half. Falsy when
 * nothing was found.
 */
struct NavPoint {
    static constexpr uint32_t kNoTriangle = 0xFFFFFFFFu;

    uint32_t triangle = kNoTriangle;
    glm::vec3 position{0.0f};

    explicit operator bool() const { return triangle != kNoTriangle; }
};

/**
 * @brief The baked surface, and every query over it.
 *
 * Not a base class and not an interface. A game that wants a different navmesh builds a
 * different one; there is nothing here for a second implementation to share.
 */
class NavMesh {
  public:
    static constexpr uint32_t kNoTriangle = NavPoint::kNoTriangle;

    /**
     * @brief Build from a world-space triangle soup.
     *
     * @param positions One vertex each, already transformed into world space. The caller
     *                  does the transforming because it is the only one that knows which
     *                  instances count as level geometry -- a navmesh over the scene's
     *                  moving crates is a navmesh that is wrong every frame.
     * @param indices   Three per triangle.
     *
     * Discards everything: a second `bake` on the same object leaves no trace of the
     * first.
     */
    void bake(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices,
              const NavBuildParams& params = {});

    [[nodiscard]] bool empty() const { return tris.empty(); }
    [[nodiscard]] uint32_t triangleCount() const { return static_cast<uint32_t>(tris.size()); }
    [[nodiscard]] uint32_t vertexCount() const { return static_cast<uint32_t>(verts.size()); }
    [[nodiscard]] const NavTriangle& triangle(uint32_t i) const { return tris[i]; }
    /// By value rather than by reference since D18: the solver holds its vertices in its
    /// own frame, and what a caller wants is the world it baked from.
    [[nodiscard]] glm::vec3 vertex(uint32_t i) const { return toWorld(verts[i]); }

    /// Which way was up in the world this was baked from.
    [[nodiscard]] const glm::vec3& up() const { return build.up; }
    /// World to the solver's frame and back. Public because `PathFollower` walks a path in
    /// world space and a debug draw wants the mesh where the level is; the identity when
    /// `up` is +Y, which is every 3D scene.
    [[nodiscard]] glm::vec3 toNav(const glm::vec3& world) const { return rotated ? navRotation * world : world; }
    [[nodiscard]] glm::vec3 toWorld(const glm::vec3& nav) const {
        return rotated ? glm::conjugate(navRotation) * nav : nav;
    }
    /// How many connected components survived `minRegionArea`. One is the happy case; more
    /// than one means an agent's reachable set depends on where it starts.
    [[nodiscard]] uint32_t regionCount() const { return regions; }

    /**
     * @brief The closest point on the mesh to `p`, or a falsy `NavPoint`.
     *
     * @param maxDistance Search radius. A query further than this from any walkable
     *                    surface returns nothing rather than snapping across the level,
     *                    which is the difference between "the agent stepped off the mesh"
     *                    and "the agent teleported home".
     */
    [[nodiscard]] NavPoint nearest(const glm::vec3& p, float maxDistance = 4.0f) const;

    /// Whether any path exists, in constant time. Worth asking first: A* over a
    /// disconnected pair explores the whole of the start's region before failing.
    [[nodiscard]] bool reachable(const NavPoint& from, const NavPoint& to) const;

    /**
     * @brief A* through the triangle graph, then the funnel.
     *
     * @param out Straightened world-space waypoints, `from.position` first and
     *            `to.position` last. Cleared before anything is written, so a failed
     *            search leaves it empty rather than stale.
     * @return false when either end is falsy or the two are in different regions.
     *
     * The corridor A* finds is a list of triangles, which is not a path -- walking their
     * centroids produces the zig-zag that gives navmeshes their reputation. `out` is what
     * the funnel makes of it: the shortest polyline through the same corridor, with a
     * vertex only where the path actually turns.
     */
    [[nodiscard]] bool findPath(const NavPoint& from, const NavPoint& to, std::vector<glm::vec3>& out) const;

    /// The corridor itself, for a caller that wants it -- a debug draw, or a game keeping a
    /// path valid as the agent walks. Same failure rules as `findPath`.
    [[nodiscard]] bool findCorridor(const NavPoint& from, const NavPoint& to, std::vector<uint32_t>& out) const;

    /// Height of the mesh directly below `p`, within `maxDrop`. What puts a spawned agent
    /// on the floor. Falsy when nothing is under it.
    [[nodiscard]] NavPoint dropToFloor(const glm::vec3& p, float maxDrop = 4.0f) const;

    /**
     * @brief Can an agent walk the straight line from `from` to `to` without leaving the
     *        mesh?
     *
     * Walks the triangle strip the segment passes through, crossing at each shared edge
     * until it either contains `to` or reaches an edge with nothing on the other side. No
     * BVH and no distance test: the walk is O(triangles crossed), which for the "can I see
     * that" question a game actually asks is a handful.
     *
     * Used by `findPath` to straighten what the funnel could not, and worth having on its
     * own -- navmesh line-of-sight is the cheap approximation of visibility that most AI
     * wants, and it answers "can I charge straight at the player" without a physics query.
     */
    [[nodiscard]] bool raycast(const NavPoint& from, const glm::vec3& to) const;

    /// The same question for an agent with width: the centre line and both edges of a
    /// `radius`-wide band must all stay on the mesh. `radius` at or below zero is exactly
    /// `raycast`.
    [[nodiscard]] bool corridorClear(const glm::vec3& from, const glm::vec3& to, float radius) const;

    [[nodiscard]] const NavBuildParams& params() const { return build; }

  private:
    /// Interior node of the private BVH over triangle boxes. The second BVH in the tree
    /// and deliberately not shared with `SpatialIndex`: that one indexes *instances* and
    /// refits against an `InstanceTable`, this one indexes triangles and never moves. Two
    /// occurrences are a coincidence -- the abstraction waits for a third.
    struct BvhNode {
        glm::vec3 boundsMin{0.0f};
        uint32_t firstTri = 0;
        glm::vec3 boundsMax{0.0f};
        uint32_t triCount = 0;
    };

    /// The world-space wrappers above delegate to these, and so does every internal caller.
    /// Splitting them is what keeps a rotation from being applied twice: `corridorClear`
    /// asks `nearest` and `raycast`, and `findPath` asks `findCorridor` and `corridorClear`.
    [[nodiscard]] NavPoint nearestNav(const glm::vec3& p, float maxDistance) const;
    [[nodiscard]] NavPoint dropToFloorNav(const glm::vec3& p, float maxDrop) const;
    [[nodiscard]] bool raycastNav(const NavPoint& from, const glm::vec3& to) const;
    [[nodiscard]] bool corridorClearNav(const glm::vec3& from, const glm::vec3& to, float radius) const;
    [[nodiscard]] bool findPathNav(const NavPoint& from, const NavPoint& to, std::vector<glm::vec3>& out) const;
    [[nodiscard]] bool findCorridorNav(const NavPoint& from, const NavPoint& to, std::vector<uint32_t>& out) const;

    [[nodiscard]] glm::vec3 closestOnTriangle(uint32_t tri, const glm::vec3& p) const;
    [[nodiscard]] glm::vec3 centroid(uint32_t tri) const;
    void buildBvh();
    uint32_t buildBvhRange(uint32_t first, uint32_t count, uint32_t depth);
    void labelRegions(float minRegionArea);

    std::vector<glm::vec3> verts;
    std::vector<NavTriangle> tris;
    /// Indirection so the BVH may reorder triangles without invalidating the adjacency
    /// indices inside `NavTriangle`.
    std::vector<uint32_t> bvhOrder;
    std::vector<BvhNode> nodes;
    NavBuildParams build;
    uint32_t regions = 0;
    /// World to the solver's frame, taking `build.up` onto +Y by the shortest arc.
    glm::quat navRotation{1.0f, 0.0f, 0.0f, 0.0f};
    /// False whenever that rotation is the identity, which is the whole of what makes a
    /// 3D scene's numbers unchanged: not "close enough", but no arithmetic performed.
    bool rotated = false;
};

/**
 * @brief Where an agent is along a path, and nothing else.
 *
 * A plain struct with the path in it rather than an `Agent` class that also owns a
 * position, a velocity and a state machine: those belong to whatever the game already has,
 * and a navigation system that starts owning them is one that ends up owning movement.
 */
struct PathFollower {
    std::vector<glm::vec3> path;
    /// The waypoint being walked towards. Advanced by `steer`.
    size_t waypoint = 0;
    /// How close counts as arrived at an intermediate waypoint. Bigger than it looks
    /// necessary on purpose -- a follower that must hit each corner exactly stalls against
    /// its own arrival test at low frame rates.
    float waypointRadius = 0.4f;
    /// How close counts as arrived at the *last* one, and where slowing begins.
    float arriveRadius = 0.5f;
    /// The axis `steer` drops, which is the mesh's `up()` (D18). Held here rather than
    /// taken from the navmesh because a follower outlives the search that filled it and
    /// `steer` deliberately knows nothing about a `NavMesh`.
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    [[nodiscard]] bool done() const { return waypoint >= path.size(); }
    void reset(std::vector<glm::vec3> newPath) {
        path = std::move(newPath);
        waypoint = 0;
    }
};

/**
 * @brief Desired velocity for this frame, in world space.
 *
 * Horizontal: Y is dropped from the direction and from the arrival distance, because the
 * follower is walking a floor and a ramp's rise must not slow it down. Returns a zero
 * vector once the path is done.
 *
 * Advances `follower.waypoint` past every waypoint already reached, so a single call
 * covers a frame long enough to have crossed two of them -- which is what stops a slow
 * frame from leaving an agent orbiting a corner it already passed.
 *
 * @param maxSpeed Full speed, in metres per second. Scaled down inside `arriveRadius` of
 *                 the final waypoint so the agent stops rather than oscillating across it.
 */
[[nodiscard]] glm::vec3 steer(PathFollower& follower, const glm::vec3& position, float maxSpeed);

} // namespace nav
