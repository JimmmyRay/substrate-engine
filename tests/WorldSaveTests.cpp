#include "scene/WorldSave.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace core;
using namespace scene;

/**
 * @file tests/WorldSaveTests.cpp
 * @brief The engine's save section, and the four ways it declines to apply one (C6).
 *
 * `SaveFileTests` covers the stream; this covers the *decision*. The property that matters
 * is not that a transform round-trips -- it is that a save which does not describe the
 * world in front of it changes **nothing**, rather than changing the first half of it
 * before noticing. Every refusal test below therefore asserts twice: that the call said
 * no, and that the table is exactly as it was.
 */
namespace {

/// Three unit boxes in a row. Built through `create` so the flags and bounds are the ones
/// a real scene would have.
InstanceTable rowOfThree() {
    InstanceTable t;
    for (int i = 0; i < 3; ++i) {
        InstanceDesc d;
        d.transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
        d.localMin = glm::vec3(-0.5f);
        d.localMax = glm::vec3(0.5f);
        (void)t.create(d);
    }
    return t;
}

/// A writer's bytes as a reader sees them. The framing is only assembled in `write`, so a
/// round trip has to go through a file.
std::vector<uint8_t> framed(SaveWriter& w) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("substrate-world-test-" + std::to_string(::rand()) + ".sav");
    EXPECT_TRUE(w.write(p));
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return bytes;
}

/// Write `table` as a save of `scene`, and hand back a reader positioned to read it.
std::vector<uint8_t> saveOf(std::string_view scene, const InstanceTable& table, float timeScale = 1.0f,
                            uint64_t steps = 0) {
    SaveWriter w;
    writeWorldSave(w, scene, table, timeScale, steps);
    return framed(w);
}

/// Every slot's transform, for asserting that a refusal touched none of them.
std::vector<glm::mat4> snapshot(const InstanceTable& t) {
    std::vector<glm::mat4> out;
    for (uint32_t slot = 0; slot < t.slotCount(); ++slot) out.push_back(t.transform(slot));
    return out;
}

} // namespace

// ==================================================================== round trip

TEST(WorldSave, RestoresTransforms) {
    InstanceTable saved = rowOfThree();
    saved.setTransform(saved.idAt(1), glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 8.0f, 9.0f)));
    const std::vector<uint8_t> bytes = saveOf("res:/showcase.gltf", saved);

    InstanceTable live = rowOfThree();
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes)) << r.reason();

    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    ASSERT_TRUE(worldSaveApplies(world, "res:/showcase.gltf", live, reason)) << reason;
    applyWorldSave(world, live);

    EXPECT_EQ(live.transform(1u)[3], glm::vec4(7.0f, 8.0f, 9.0f, 1.0f));
    EXPECT_EQ(live.transform(0u)[3], glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_EQ(live.transform(2u)[3], glm::vec4(2.0f, 0.0f, 0.0f, 1.0f));
}

TEST(WorldSave, RestoresTheWorldBoxWithTheTransform) {
    // setTransform is what rebuilds the bounds, so restoring through it is what keeps the
    // index and the culler agreeing with the save. Writing `model` directly would not.
    InstanceTable saved = rowOfThree();
    saved.setTransform(saved.idAt(0), glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 0.0f, 0.0f)));
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    applyWorldSave(world, live);

    EXPECT_NEAR(live.slotBounds(0).worldMin.x, 49.5f, 1e-4f);
    EXPECT_NEAR(live.slotBounds(0).worldMax.x, 50.5f, 1e-4f);
}

TEST(WorldSave, RestoresDynamicBothWays) {
    // Set and cleared, because a mask used only for setting would pass a test that only
    // checks the set direction and would silently never turn anything back off.
    InstanceTable saved = rowOfThree();
    saved.setFlags(saved.idAt(1), kInstanceDynamic, 0);
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    live.setFlags(live.idAt(0), kInstanceDynamic, 0);
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    applyWorldSave(world, live);

    EXPECT_EQ(live.flags(live.idAt(0)) & kInstanceDynamic, 0u);
    EXPECT_NE(live.flags(live.idAt(1)) & kInstanceDynamic, 0u);
}

TEST(WorldSave, DoesNotPretendToPersistVisibility) {
    // `kInstanceVisible` lives in the GPU's copy of the table; the CPU word is clear for
    // everything, always. This asserts the save does not claim otherwise -- if the mask
    // ever readmits the bit, restoring it would write a value the next cull overwrites.
    EXPECT_EQ(kSavedInstanceFlags & kInstanceVisible, 0u);
    EXPECT_EQ(kSavedInstanceFlags & kInstanceDeformed, 0u);
    EXPECT_EQ(kSavedInstanceFlags & (kInstanceBlended | kInstanceMasked), 0u);
}

TEST(WorldSave, CarriesTimeScaleAndSteps) {
    const InstanceTable saved = rowOfThree();
    const std::vector<uint8_t> bytes = saveOf("s", saved, 0.25f, 12345ull);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    EXPECT_FLOAT_EQ(world.timeScale, 0.25f);
    EXPECT_EQ(world.steps, 12345ull);
}

// ==================================================================== what it will not carry

TEST(WorldSave, WillNotTurnAStaticMeshIntoASkinnedOne) {
    // The geometry bits are properties of the mesh, not of the moment. A save that claims
    // them -- whether by corruption or by being taken before an asset was re-authored --
    // must not be able to route an instance into the skinning dispatch.
    InstanceTable saved = rowOfThree();
    saved.setFlags(saved.idAt(0), kInstanceSkinned | kInstanceMorphed | kInstanceBlended, 0);
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    applyWorldSave(world, live);

    EXPECT_EQ(live.flags(live.idAt(0)) & (kInstanceSkinned | kInstanceMorphed | kInstanceBlended), 0u);
}

TEST(WorldSave, DoesNotResurrectASlotDeadInTheSave) {
    InstanceTable saved = rowOfThree();
    saved.destroy(saved.idAt(1));
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    const glm::mat4 before = live.transform(1u);
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    applyWorldSave(world, live);

    // The live instance is left exactly as it was rather than being killed to match: a
    // save says what its own world looked like, not what this one may destroy.
    EXPECT_NE(live.flags(live.idAt(1)) & kInstanceLive, 0u);
    EXPECT_EQ(live.transform(1u), before);
}

TEST(WorldSave, DoesNotResurrectASlotDeadHere) {
    const InstanceTable saved = rowOfThree();
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    live.destroy(live.idAt(2));
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    ASSERT_TRUE(worldSaveApplies(world, "s", live, reason)) << reason;
    applyWorldSave(world, live);

    // Creating one would issue a new generation and hand it to nobody.
    EXPECT_EQ(live.slot(2).meta.z & kInstanceLive, 0u);
}

// ==================================================================== refusals

TEST(WorldSave, RefusesADifferentScene) {
    const std::vector<uint8_t> bytes = saveOf("res:/sponza.gltf", rowOfThree());

    InstanceTable live = rowOfThree();
    const std::vector<glm::mat4> before = snapshot(live);
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;

    EXPECT_FALSE(worldSaveApplies(world, "res:/showcase.gltf", live, reason));
    EXPECT_NE(reason.find("sponza"), std::string::npos) << reason;
    EXPECT_NE(reason.find("showcase"), std::string::npos) << reason;
    EXPECT_EQ(snapshot(live), before);
}

TEST(WorldSave, RefusesADifferentSlotCount) {
    InstanceTable saved = rowOfThree();
    InstanceDesc d;
    (void)saved.create(d);
    const std::vector<uint8_t> bytes = saveOf("s", saved);

    InstanceTable live = rowOfThree();
    const std::vector<glm::mat4> before = snapshot(live);
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;

    // The name matched; the shape did not. Both halves are needed -- a save of a scene
    // edited since it was taken has the right name and the wrong count.
    EXPECT_FALSE(worldSaveApplies(world, "s", live, reason));
    EXPECT_NE(reason.find('4'), std::string::npos) << reason;
    EXPECT_NE(reason.find('3'), std::string::npos) << reason;
    EXPECT_EQ(snapshot(live), before);
}

TEST(WorldSave, RefusesASectionFromALaterBuild) {
    SaveWriter w;
    w.beginSection("engine", kWorldSaveVersion + 1);
    w.text("s");
    w.u32(0);
    w.f32(1.0f);
    w.u64(0);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    WorldSave world;
    std::string reason;
    EXPECT_FALSE(readWorldSave(r, world, reason));
    EXPECT_NE(reason.find(std::to_string(kWorldSaveVersion + 1)), std::string::npos) << reason;
}

TEST(WorldSave, RefusesASaveWithNoEngineSection) {
    SaveWriter w;
    w.beginSection("demo", 1);
    w.u32(7);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    WorldSave world;
    std::string reason;
    EXPECT_FALSE(readWorldSave(r, world, reason));
    EXPECT_NE(reason.find("engine"), std::string::npos) << reason;
}

TEST(WorldSave, RefusesASectionThatRunsOut) {
    // Two entries promised, two written, and then the trailer missing. The count is
    // plausible, so this is caught by the reader's own bounds rather than by arithmetic.
    SaveWriter w;
    w.beginSection("engine", kWorldSaveVersion);
    w.text("s");
    w.u32(2);
    for (int i = 0; i < 2; ++i) {
        w.u32(kInstanceLive | kInstanceVisible);
        w.mat4(glm::mat4(1.0f));
    }

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    WorldSave world;
    std::string reason;
    EXPECT_FALSE(readWorldSave(r, world, reason));
    EXPECT_FALSE(reason.empty());
}

TEST(WorldSave, RefusesAnImplausibleSlotCountWithoutAllocatingForIt) {
    // A four-byte count is the cheapest thing in the file to corrupt and the most
    // expensive to believe: 2^32 entries at 68 bytes each is 292 GB of resize().
    SaveWriter w;
    w.beginSection("engine", kWorldSaveVersion);
    w.text("s");
    w.u32(0xFFFFFFFFu);
    w.f32(1.0f);
    w.u64(0);

    SaveReader r;
    ASSERT_TRUE(r.openBytes(framed(w)));
    WorldSave world;
    std::string reason;
    EXPECT_FALSE(readWorldSave(r, world, reason));
    EXPECT_NE(reason.find("4294967295"), std::string::npos) << reason;
}

TEST(WorldSave, ReadsBackWhatAnEmptyTableWrote) {
    const InstanceTable empty;
    const std::vector<uint8_t> bytes = saveOf("s", empty);

    InstanceTable live;
    SaveReader r;
    ASSERT_TRUE(r.openBytes(bytes));
    WorldSave world;
    std::string reason;
    ASSERT_TRUE(readWorldSave(r, world, reason)) << reason;
    EXPECT_TRUE(worldSaveApplies(world, "s", live, reason)) << reason;
    EXPECT_TRUE(world.flags.empty());
}
