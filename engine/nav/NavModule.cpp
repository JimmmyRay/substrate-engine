#include "nav/NavModule.h"

#include "Engine.h"
#include "Modules.h"
#include "core/Logger.h"
#include "core/Profiler.h"

#include <vector>

namespace nav {

namespace {

/// The engine's one navmesh. File-scope rather than an `Engine` member, which would name
/// `nav::NavMesh` in `Engine.h` and link navigation into every game.
NavMesh g_navigation;

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

/// Assign `modules::nav` from a header instead and any transitive include links navigation
/// into a game that never asked for it.
struct Registrar {
    Registrar() { modules::nav = &g_module; }
};

const Registrar g_registrar;

} // namespace

void bakeNavMesh(std::span<const scene::ColliderDesc> colliders, NavMesh& out, const NavBuildParams& params) {
    // Widening this filter to render geometry puts agents on windowsills, curtains and pillar
    // tops; widening it to moving bodies bakes a crate's floor into a mesh that is wrong the
    // moment the crate is pushed. Boxes, capsules and hulls carry no triangles, so a floor
    // authored as a box bakes to nothing here.
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

// Defining these in Engine.cpp instead links navigation into every binary -- Engine.cpp is in
// all of them and this file is not.
const nav::NavMesh& Engine::navMesh() const {
    return nav::g_navigation;
}

void Engine::bakeNavMesh(nav::NavMesh& out, const nav::NavBuildParams& params) const {
    nav::bakeNavMesh(sceneData.colliders(), out, params);
}
