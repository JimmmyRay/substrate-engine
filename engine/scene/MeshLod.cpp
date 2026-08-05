#include "scene/MeshLod.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "scene/SceneData.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <vector>

namespace scene {

uint32_t buildLodChains(SceneData& data) {
    auto s = core::Profiler::scope("MeshLod::build");

    if (data.vertices.empty() || data.indices.empty()) return 0;

    const size_t indicesBefore = data.indices.size();
    uint32_t chained = 0;

    // Reused across primitives: `meshopt_simplify` wants room for the worst case, the whole
    // input, which is a 60k-index allocation to make once rather than a hundred times.
    std::vector<uint32_t> source;
    std::vector<uint32_t> reduced;

    for (Primitive& p : data.primitives) {
        p.lodCount = 0;

        if (p.indexCount < kLodMinIndices) continue;
        if (p.blended) continue;
        if (p.skinOffset != 0xFFFFFFFFu || p.morphTargets > 0) continue;

        // **Copied, not pointed at.** The loop below appends to `data.indices`, so an
        // iterator or pointer into it here dangles the first time a level is written.
        source.assign(data.indices.begin() + p.firstIndex, data.indices.begin() + p.firstIndex + p.indexCount);

        for (uint32_t level = 0; level < kMaxLodLevels; ++level) {
            // Half the *original* triangle count per level, rounded to a whole triangle.
            // Against the original rather than against `source` so a level that stopped
            // short does not drag every level after it down with it.
            const size_t target = (static_cast<size_t>(p.indexCount) >> (level + 1)) / 3 * 3;
            if (target < 3 || target >= source.size()) break;

            reduced.resize(source.size());
            float error = 0.0f;
            const size_t got = meshopt_simplify(
                reduced.data(), source.data(), source.size(), &data.vertices[0].position.x, data.vertices.size(),
                sizeof(Vertex), target, kLodTargetError,
                // The border is what two primitives meet along, and this scene format has
                // no idea which primitives are neighbours. Letting the simplifier move a
                // boundary vertex opens a crack between a wall and the floor it stands on,
                // which is a hole in the image rather than a coarser silhouette.
                meshopt_SimplifyLockBorder, &error);

            // The simplifier honours topology and `kLodTargetError` ahead of the triangle
            // target, so a mesh it cannot reduce comes back nearly the size it went in. A
            // level saving under a tenth costs an index range, a chain entry and a selection
            // branch to buy nothing.
            if (got < 3 || got > source.size() - source.size() / 10) break;

            p.lods[level].firstIndex = static_cast<uint32_t>(data.indices.size());
            p.lods[level].indexCount = static_cast<uint32_t>(got);
            data.indices.insert(data.indices.end(), reduced.begin(), reduced.begin() + static_cast<ptrdiff_t>(got));
            p.lodCount = level + 1;

            source.assign(reduced.begin(), reduced.begin() + static_cast<ptrdiff_t>(got));
        }

        if (p.lodCount > 0) ++chained;
    }

    data.stats.lodPrimitives = chained;
    data.stats.lodIndices = static_cast<uint32_t>(data.indices.size() - indicesBefore);

    core::Logger::status(core::LogCategory::GLTF, "LOD: %u of %zu primitives carry chains, %u indices added (%.1f%%)",
                         chained, data.primitives.size(), data.stats.lodIndices,
                         indicesBefore == 0 ? 0.0
                                            : 100.0 * static_cast<double>(data.stats.lodIndices) /
                                                  static_cast<double>(indicesBefore));
    return chained;
}

glm::vec4 lodCoverageThresholds(float base) {
    return {base, base * 0.25f, base * 0.0625f, 0.0f};
}

uint32_t lodForCoverage(float coverage, const glm::vec4& thresholds, uint32_t levels) {
    uint32_t level = 0;
    while (level + 1 < levels && level < kMaxLodLevels && coverage < thresholds[static_cast<int>(level)]) ++level;
    return level;
}

} // namespace scene
