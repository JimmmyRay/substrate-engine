#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

/**
 * @file engine/nav/NavMesh.h
 * @brief Walkable surface, path through it, and steering along the path.
 *
 * The solver works in Y up with every surviving triangle wound normal-up, which is what lets
 * the funnel read a shared edge's two vertices as left and right without asking which triangle
 * they came from. Drop the cut that removes what stands on a walkable surface and a floor
 * authored as one quad with columns resting on it routes straight through every column, while
 * the triangle count, the region count and the word "baked" all still look healthy.
 */
namespace nav {

struct NavBuildParams {
    /// Steepest surface an agent will stand on, in degrees from horizontal. Below 45 the
    /// ramps every scene is authored under stop being walkable.
    float walkableSlopeDegrees = 45.0f;

    /**
     * @brief How far a path is kept from a portal's endpoints, in metres.
     *
     * Applied when string-pulling, not when baking, so two navmeshes built at different radii
     * are the same mesh; a portal narrower than twice this is inset to its midpoint rather
     * than discarded.
     */
    float agentRadius = 0.4f;

    /// Vertices within this distance of each other become one. Too small and the mesh is
    /// islands with no adjacency; too large and a doorway welds shut.
    float weldEpsilon = 0.01f;

    /// Connected regions with less total area than this are dropped. At zero, `nearest` snaps
    /// agents onto the one-triangle scraps a scene bakes by the dozen -- a windowsill, a crate
    /// top -- each of them a region nothing can path out of.
    float minRegionArea = 1.0f;

    /**
     * @brief Which way is up in the world these triangles came from.
     *
     * `+Z` is what a flat world wants: `ColliderFreedom::Plane2D` plays in XY, so leaving this
     * at `+Y` puts a 2D game's bodies and its navmesh in perpendicular planes. It rotates
     * rather than swizzles -- a permutation of the axes flips handedness, and the funnel reads
     * a portal's left and right off a winding that only holds in a right-handed basis. `+Y` is
     * the identity and takes no arithmetic at all.
     */
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

/**
 * @brief One walkable triangle and how it connects.
 *
 * `neighbour[i]` is across the edge from `v[i]` to `v[(i + 1) % 3]` -- one index means the same
 * thing in both arrays, and every corridor walk depends on it.
 */
struct NavTriangle {
    uint32_t v[3]{};
    uint32_t neighbour[3]{};
    /// Connected component. Different regions means no path between them, which is what makes
    /// `reachable` a comparison rather than a search.
    uint32_t region = 0;
};

/**
 * @brief A position on the mesh: which triangle, and where in it.
 *
 * Falsy when nothing was found.
 */
struct NavPoint {
    static constexpr uint32_t kNoTriangle = 0xFFFFFFFFu;

    uint32_t triangle = kNoTriangle;
    glm::vec3 position{0.0f};

    explicit operator bool() const { return triangle != kNoTriangle; }
};

/// @brief The baked surface, and every query over it.
class NavMesh {
  public:
    static constexpr uint32_t kNoTriangle = NavPoint::kNoTriangle;

    /**
     * @brief Build from a world-space triangle soup.
     *
     * @param positions One vertex each, already in world space.
     * @param indices   Three per triangle.
     *
     * Discards everything: a second `bake` on the same object leaves no trace of the first.
     */
    void bake(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices,
              const NavBuildParams& params = {});

    [[nodiscard]] bool empty() const { return tris.empty(); }
    [[nodiscard]] uint32_t triangleCount() const { return static_cast<uint32_t>(tris.size()); }
    [[nodiscard]] uint32_t vertexCount() const { return static_cast<uint32_t>(verts.size()); }
    [[nodiscard]] const NavTriangle& triangle(uint32_t i) const { return tris[i]; }
    /// World space, not the solver frame the vertices are stored in.
    [[nodiscard]] glm::vec3 vertex(uint32_t i) const { return toWorld(verts[i]); }

    /// Which way was up in the world this was baked from.
    [[nodiscard]] const glm::vec3& up() const { return build.up; }
    /// World to the solver's frame and back; the identity when `up` is +Y.
    [[nodiscard]] glm::vec3 toNav(const glm::vec3& world) const { return rotated ? navRotation * world : world; }
    [[nodiscard]] glm::vec3 toWorld(const glm::vec3& nav) const {
        return rotated ? glm::conjugate(navRotation) * nav : nav;
    }
    /// How many connected components survived `minRegionArea`. More than one means an agent's
    /// reachable set depends on where it starts.
    [[nodiscard]] uint32_t regionCount() const { return regions; }

    /**
     * @brief The closest point on the mesh to `p`, or a falsy `NavPoint`.
     *
     * @param maxDistance Search radius. Widen it far and an agent that stepped off the mesh
     *                    snaps across the level instead of returning nothing.
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
     */
    [[nodiscard]] bool findPath(const NavPoint& from, const NavPoint& to, std::vector<glm::vec3>& out) const;

    /// The corridor A* found, unstraightened. Same failure rules as `findPath`.
    [[nodiscard]] bool findCorridor(const NavPoint& from, const NavPoint& to, std::vector<uint32_t>& out) const;

    /// Height of the mesh directly below `p`, within `maxDrop`. Falsy when nothing is under it.
    [[nodiscard]] NavPoint dropToFloor(const glm::vec3& p, float maxDrop = 4.0f) const;

    /**
     * @brief Can an agent walk the straight line from `from` to `to` without leaving the
     *        mesh?
     *
     * No BVH and no distance test -- cost is O(triangles crossed), so a ray the length of the
     * level is priced by the whole strip it walks.
     */
    [[nodiscard]] bool raycast(const NavPoint& from, const glm::vec3& to) const;

    /// The same question for an agent with width: the centre line and both edges of a
    /// `radius`-wide band must all stay on the mesh. `radius` at or below zero is exactly
    /// `raycast`.
    [[nodiscard]] bool corridorClear(const glm::vec3& from, const glm::vec3& to, float radius) const;

    [[nodiscard]] const NavBuildParams& params() const { return build; }

  private:
    struct BvhNode {
        glm::vec3 boundsMin{0.0f};
        uint32_t firstTri = 0;
        glm::vec3 boundsMax{0.0f};
        uint32_t triCount = 0;
    };

    /// Internal callers must reach for these, never the world-space wrappers above:
    /// `corridorClear` calls `nearest` and `raycast`, so a wrapper there rotates twice.
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
    /// False when that rotation is the identity, so a +Y scene has no arithmetic applied at
    /// all rather than arithmetic that rounds to nothing.
    bool rotated = false;
};

/// @brief Where an agent is along a path, and nothing else.
struct PathFollower {
    std::vector<glm::vec3> path;
    /// The waypoint being walked towards. Advanced by `steer`.
    size_t waypoint = 0;
    /// How close counts as arrived at an intermediate waypoint. Tighten it and a follower
    /// stalls against its own arrival test at low frame rates.
    float waypointRadius = 0.4f;
    /// How close counts as arrived at the *last* one, and where slowing begins.
    float arriveRadius = 0.5f;
    /// The axis `steer` drops. Must be set to the mesh's `up()`; drop the wrong one and the
    /// follower measures progress along the axis it is not travelling on and never arrives.
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
 * `follower.up` is dropped from the direction and from the arrival distance, so a ramp's rise
 * does not slow the agent. Zero once the path is done.
 *
 * Advances `follower.waypoint` past every waypoint already reached, not just one: a frame long
 * enough to have crossed two otherwise leaves the agent orbiting a corner it already passed.
 *
 * @param maxSpeed Full speed, in metres per second. Scaled down inside `arriveRadius` of the
 *                 final waypoint so the agent stops rather than oscillating across it.
 */
[[nodiscard]] glm::vec3 steer(PathFollower& follower, const glm::vec3& position, float maxSpeed);

} // namespace nav
