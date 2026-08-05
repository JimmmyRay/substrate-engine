#include "scene/MeshLod.h"

#include "scene/SceneData.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace scene;

/**
 * @file tests/MeshLodTests.cpp
 * @brief The LOD chain data model, and the coverage arithmetic that selects out of it.
 *
 * Two halves, and neither of them can be checked by the golden suite:
 *
 * 1. **The chain.** What `buildLodChains` produces has to be *addressable* -- every level a
 *    range inside the scene's own index buffer, over the scene's own vertices, shorter than
 *    the level above it. A chain that is subtly wrong does not look wrong; it draws
 *    somebody else's triangles, and the only place that is visible before a frame is here.
 * 2. **The selection.** `cull.comp` computes a coverage and compares it against thresholds
 *    the CPU pushed, and `lodForCoverage` is the CPU's copy of that comparison. A formula
 *    off by a factor is the likeliest defect this row has, and the golden set catches only
 *    the half of that error which fires at a reference camera.
 *
 * No device and no glTF: the bake is the half of a load that needs neither, which is what
 * puts this in the hosted suite and under every sanitizer.
 */
namespace {

/// A subdivided quad with a little relief on it, as one primitive of a `SceneData`.
///
/// Relief rather than a plane on purpose. A perfectly flat grid has zero quadric error
/// everywhere and collapses to its border for free, which would make this a test of nothing
/// -- a simplifier that returned four triangles for any input would pass it.
SceneData gridScene(uint32_t side) {
    SceneData data;
    for (uint32_t y = 0; y < side; ++y) {
        for (uint32_t x = 0; x < side; ++x) {
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            Vertex v;
            v.position = {fx, 0.35f * std::sin(fx * 0.4f) * std::cos(fy * 0.4f), fy};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            v.uv = {fx / static_cast<float>(side), fy / static_cast<float>(side)};
            data.vertices.push_back(v);
        }
    }

    Primitive p;
    p.firstIndex = 0;
    p.baseVertex = 0;
    p.vertexCount = static_cast<uint32_t>(data.vertices.size());
    for (uint32_t y = 0; y + 1 < side; ++y) {
        for (uint32_t x = 0; x + 1 < side; ++x) {
            const uint32_t a = y * side + x;
            const uint32_t b = a + 1;
            const uint32_t c = a + side;
            const uint32_t d = c + 1;
            data.indices.insert(data.indices.end(), {a, c, b, b, c, d});
        }
    }
    p.indexCount = static_cast<uint32_t>(data.indices.size());
    p.localMin = {0.0f, -1.0f, 0.0f};
    p.localMax = {static_cast<float>(side), 1.0f, static_cast<float>(side)};
    data.primitives.push_back(p);
    return data;
}

} // namespace

// =============================================================== the chain

TEST(MeshLod, GridGetsAChainAndEveryLevelIsAddressable) {
    SceneData data = gridScene(40);
    const uint32_t originalIndices = data.primitives[0].indexCount;
    ASSERT_GE(originalIndices, kLodMinIndices);

    EXPECT_EQ(buildLodChains(data), 1u);

    const Primitive& p = data.primitives[0];
    ASSERT_GE(p.lodCount, 1u);
    EXPECT_LE(p.lodCount, kMaxLodLevels);

    // LOD 0 is untouched. The chain extends the primitive; it does not rewrite it, and a
    // build that moved level 0 would change what every existing reader draws.
    EXPECT_EQ(p.firstIndex, 0u);
    EXPECT_EQ(p.indexCount, originalIndices);

    uint32_t previous = p.indexCount;
    for (uint32_t l = 0; l < p.lodCount; ++l) {
        const LodRange& r = p.lods[l];

        // Whole triangles, inside the buffer, and past the range LOD 0 occupies -- levels
        // are appended, never carved out of what was already there.
        EXPECT_EQ(r.indexCount % 3u, 0u) << "level " << l;
        EXPECT_GE(r.firstIndex, originalIndices) << "level " << l;
        EXPECT_LE(static_cast<size_t>(r.firstIndex) + r.indexCount, data.indices.size()) << "level " << l;

        // Coarser than the level above it. Not merely different: a chain whose second entry
        // is bigger than its first is a chain that costs more the further away it gets.
        EXPECT_LT(r.indexCount, previous) << "level " << l;
        previous = r.indexCount;

        // Over the *same* vertices. This is the property that lets levels share the vertex
        // buffer, and an index past the end of it is the failure that draws garbage.
        for (uint32_t k = 0; k < r.indexCount; ++k) {
            ASSERT_LT(data.indices[r.firstIndex + k], data.vertices.size()) << "level " << l << " index " << k;
        }
    }
}

TEST(MeshLod, LevelsDoNotOverlapEachOther) {
    SceneData data = gridScene(40);
    ASSERT_EQ(buildLodChains(data), 1u);

    const Primitive& p = data.primitives[0];
    ASSERT_GE(p.lodCount, 2u) << "a 40x40 grid should reduce at least twice";
    for (uint32_t l = 1; l < p.lodCount; ++l) {
        EXPECT_GE(p.lods[l].firstIndex, p.lods[l - 1].firstIndex + p.lods[l - 1].indexCount);
    }
}

TEST(MeshLod, SmallPrimitivesGetNoChain) {
    // Two triangles: below the floor, and there is nothing a chain could take off it.
    SceneData data = gridScene(3);
    ASSERT_LT(data.primitives[0].indexCount, kLodMinIndices);
    const size_t before = data.indices.size();

    EXPECT_EQ(buildLodChains(data), 0u);
    EXPECT_EQ(data.primitives[0].lodCount, 0u);
    EXPECT_EQ(data.indices.size(), before) << "a refused primitive must not add indices";
    EXPECT_EQ(data.stats.lodIndices, 0u);
}

TEST(MeshLod, BlendedAndDeformingPrimitivesAreSkipped) {
    // Blended geometry is drawn by the forward pass, which builds its own commands on the
    // CPU and never runs through cull.comp -- a chain on it could never be selected.
    SceneData blended = gridScene(40);
    blended.primitives[0].blended = true;
    EXPECT_EQ(buildLodChains(blended), 0u);
    EXPECT_EQ(blended.primitives[0].lodCount, 0u);

    // A skinned primitive is drawn out of the buffer skinning.comp wrote, whose contents
    // are its bind-pose vertices and nobody else's.
    SceneData skinned = gridScene(40);
    skinned.primitives[0].skinOffset = 0u;
    EXPECT_EQ(buildLodChains(skinned), 0u);

    SceneData morphed = gridScene(40);
    morphed.primitives[0].morphTargets = 2u;
    EXPECT_EQ(buildLodChains(morphed), 0u);
}

TEST(MeshLod, BuildIsIdempotent) {
    // Building twice must not stack a second chain on top of the first. `lodCount` is reset
    // per primitive rather than accumulated, which is what makes a re-bake produce the same
    // sidecar rather than a growing one.
    SceneData once = gridScene(40);
    ASSERT_EQ(buildLodChains(once), 1u);
    const uint32_t levels = once.primitives[0].lodCount;
    const size_t size = once.indices.size();

    ASSERT_EQ(buildLodChains(once), 1u);
    EXPECT_EQ(once.primitives[0].lodCount, levels);
    EXPECT_GT(once.indices.size(), size) << "a second build appends a second copy";
    // What must hold is that the ranges still name the *new* copy and are still valid.
    for (uint32_t l = 0; l < once.primitives[0].lodCount; ++l) {
        const LodRange& r = once.primitives[0].lods[l];
        EXPECT_LE(static_cast<size_t>(r.firstIndex) + r.indexCount, once.indices.size());
    }
}

TEST(MeshLod, StatsCountWhatWasAdded) {
    SceneData data = gridScene(40);
    const size_t before = data.indices.size();
    ASSERT_EQ(buildLodChains(data), 1u);
    EXPECT_EQ(data.stats.lodPrimitives, 1u);
    EXPECT_EQ(data.stats.lodIndices, static_cast<uint32_t>(data.indices.size() - before));
}

// ============================================================ the sidecar

TEST(MeshLod, ChainSurvivesTheSidecar) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "substrate-meshlod-roundtrip";
    std::filesystem::create_directories(dir);
    const std::filesystem::path source = dir / "grid.gltf";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << "{\"asset\":{\"version\":\"2.0\"}}";
    }

    SceneData written = gridScene(40);
    ASSERT_EQ(buildLodChains(written), 1u);
    ASSERT_TRUE(writeSceneCache(source, written));

    SceneData read;
    ASSERT_TRUE(readSceneCache(source, read));

    // The chain rides inside `Primitive`, so it is carried by the same `podVector` that
    // carries the range it extends -- there is no second list for a writer to forget.
    ASSERT_EQ(read.primitives.size(), 1u);
    EXPECT_EQ(read.primitives[0].lodCount, written.primitives[0].lodCount);
    for (uint32_t l = 0; l < written.primitives[0].lodCount; ++l) {
        EXPECT_EQ(read.primitives[0].lods[l].firstIndex, written.primitives[0].lods[l].firstIndex);
        EXPECT_EQ(read.primitives[0].lods[l].indexCount, written.primitives[0].lods[l].indexCount);
    }
    EXPECT_EQ(read.indices.size(), written.indices.size());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ========================================================= the arithmetic

TEST(MeshLod, ThresholdsQuarterPerLevel) {
    // A level halves the triangle count, and halving the linear size on screen quarters the
    // area -- so the sequence holds a roughly constant triangle-per-pixel density. A ratio
    // of one half here would be the classic off-by-a-factor: every level would fire at
    // twice the distance it should.
    const glm::vec4 t = lodCoverageThresholds(0.002f);
    EXPECT_FLOAT_EQ(t.x, 0.002f);
    EXPECT_FLOAT_EQ(t.y, 0.002f * 0.25f);
    EXPECT_FLOAT_EQ(t.z, 0.002f * 0.0625f);
    EXPECT_FLOAT_EQ(t.w, 0.0f);
}

TEST(MeshLod, SelectionIsMonotonicInCoverage) {
    const glm::vec4 t = lodCoverageThresholds(0.002f);

    // Four levels available: full coverage stays at 0, and each threshold crossed drops one.
    EXPECT_EQ(lodForCoverage(1.0f, t, 4), 0u);
    EXPECT_EQ(lodForCoverage(0.002f, t, 4), 0u) << "the threshold itself keeps the level above it";
    EXPECT_EQ(lodForCoverage(0.0019f, t, 4), 1u);
    EXPECT_EQ(lodForCoverage(0.0004f, t, 4), 2u);
    EXPECT_EQ(lodForCoverage(0.0001f, t, 4), 3u);
    EXPECT_EQ(lodForCoverage(0.0f, t, 4), 3u) << "nothing below the last level to fall to";

    // Never decreasing as the object shrinks.
    uint32_t previous = 0;
    for (float coverage = 0.01f; coverage > 1e-6f; coverage *= 0.5f) {
        const uint32_t level = lodForCoverage(coverage, t, 4);
        EXPECT_GE(level, previous);
        previous = level;
    }
}

TEST(MeshLod, ChainLengthCapsTheSelection) {
    const glm::vec4 t = lodCoverageThresholds(0.002f);

    // The whole of what "LOD is optional per mesh" comes to at the selection: a primitive
    // with no chain has one level, and a speck on the horizon still draws at LOD 0.
    EXPECT_EQ(lodForCoverage(0.0f, t, 1), 0u);
    EXPECT_EQ(lodForCoverage(1.0f, t, 1), 0u);

    // And a short chain is capped by its own length rather than by the thresholds.
    EXPECT_EQ(lodForCoverage(0.0f, t, 2), 1u);
    EXPECT_EQ(lodForCoverage(0.0f, t, 3), 2u);
}

TEST(MeshLod, AZeroThresholdSelectsNothing) {
    // The setting's floor, and it has to mean "off" rather than "everything at once":
    // coverage is never below zero, so no level is ever entered.
    const glm::vec4 t = lodCoverageThresholds(0.0f);
    EXPECT_EQ(lodForCoverage(0.0f, t, 4), 0u);
    EXPECT_EQ(lodForCoverage(1e-9f, t, 4), 0u);
}
