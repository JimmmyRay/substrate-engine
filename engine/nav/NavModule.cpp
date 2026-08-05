#include "nav/NavModule.h"

#include "Engine.h"
#include "Modules.h"
#include "core/Logger.h"
#include "core/Profiler.h"

#include <vector>

namespace nav {

namespace {

/// The engine's one navmesh.
///
/// A file-scope object rather than a member of `Engine`, because a member is a type named in
/// `Engine.h` and that is the coupling this module exists to remove. It outlives every
/// `Engine` and holds no device resource, so there is no teardown order to get wrong --
/// a `NavMesh` is vertices, indices and a BVH, and its destructor frees three vectors.
NavMesh g_navigation;

/// The real module. Its `rebuild` is what `Engine` calls at load and after an import.
struct Module final : modules::Nav {
    void rebuild(std::span<const scene::ColliderDesc> colliders) override {
        auto zone = core::Profiler::scope("nav::rebuild");
        const NavBuildParams params;
        bakeNavMesh(colliders, g_navigation, params);
        if (g_navigation.empty()) {
            core::Logger::debug(core::LogCategory::Scene, "Nav: no static mesh colliders, so no navmesh");
            return;
        }
        core::Logger::status(core::LogCategory::Scene, "Nav: %u triangles, %u vertices, %u region%s",
                             g_navigation.triangleCount(), g_navigation.vertexCount(), g_navigation.regionCount(),
                             g_navigation.regionCount() == 1 ? "" : "s");
    }
};

Module g_module;

/// Runs when the linker pulls this object file in, which it does only to resolve one of the
/// two `Engine` methods below. Pointing `modules::nav` here rather than in the header is what
/// keeps a transitive include from linking navigation into a game that never asked for it.
struct Registrar {
    Registrar() { modules::nav = &g_module; }
};

const Registrar g_registrar;

} // namespace

void bakeNavMesh(std::span<const scene::ColliderDesc> colliders, NavMesh& out, const NavBuildParams& params) {
    // Static mesh colliders, and nothing else. Three exclusions and each is deliberate:
    //
    // - **Render geometry** would put an agent on a windowsill, a curtain and the tops of
    //   Sponza's pillars. The collision surface is the one a body can actually rest on,
    //   which is the same question navigation is asking.
    // - **Anything that moves** would bake a crate's floor into a mesh that is wrong the
    //   moment the crate is pushed. A dynamic body is what `SpatialIndex` is for.
    // - **Hulls, boxes and capsules** carry no triangles. A box floor is a real authoring
    //   choice and it is not covered; the row that follows this one is a voxel bake, which
    //   would take them all.
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    for (const scene::ColliderDesc& desc : colliders) {
        if (desc.motion != scene::ColliderMotion::Static) continue;
        if (desc.resolvedShape() != scene::ColliderShape::Mesh) continue;
        if (desc.points.empty() || desc.indices.empty()) continue;

        const auto base = static_cast<uint32_t>(positions.size());
        positions.reserve(positions.size() + desc.points.size());
        for (const glm::vec3& p : desc.points) positions.push_back(glm::vec3(desc.transform * glm::vec4(p, 1.0f)));
        indices.reserve(indices.size() + desc.indices.size());
        for (const uint32_t i : desc.indices) indices.push_back(base + i);
    }
    out.bake(positions, indices, params);
}

} // namespace nav

// The two `Engine` methods navigation defines. **Referencing either is the link edge** -- see
// NavModule.h. They are here rather than in Engine.cpp because Engine.cpp is in every binary
// and this file is not.
const nav::NavMesh& Engine::navMesh() const {
    return nav::g_navigation;
}

void Engine::bakeNavMesh(nav::NavMesh& out, const nav::NavBuildParams& params) const {
    nav::bakeNavMesh(sceneData.colliders(), out, params);
}
