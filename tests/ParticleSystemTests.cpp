#include "GltfExtras.h"
#include "scene/ParticleSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace core;

using namespace scene;

/**
 * @file tests/ParticleSystemTests.cpp
 * @brief Emitter authoring, pool sizing and the slot allocator.
 *
 * The GPU half of this subsystem is not reachable from here and does not need to be:
 * what a unit test can hold is exactly the part that had to live on the CPU in the
 * first place. Three properties carry the whole design and each has a case below.
 *
 * 1. **The pool is sized from the emitters**, not from a constant. `rate x lifetime`
 *    summed over them is the steady-state population, so a scene that wants more gets
 *    more, and a budget that binds is reported rather than silently applied.
 * 2. **Slot allocation is deterministic.** It is the reason the CPU allocates at all --
 *    a GPU free list built with `atomicAdd` would hand two particles born on the same
 *    frame different slots between runs, and 5.3's golden images would stop meaning
 *    anything. Two systems fed identical steps must produce byte-identical spawns.
 * 3. **A slot is freed exactly when the particle in it expires**, because the shader
 *    kills it on the same comparison. A slot handed out early is a particle drawn from
 *    under another one.
 *
 * The glTF extras parser is here too, and it is here rather than beside the loader for
 * a reason worth stating: it takes bytes, so a test can hand it a five-line document
 * without a device, a file or fastgltf.
 */

namespace {

constexpr float kStep = 1.0f / 60.0f;

ParticleEmitter steadyEmitter(float rate, float lifetime) {
    ParticleEmitter e;
    e.rate = rate;
    e.lifetime = lifetime;
    e.lifetimeJitter = 0.0f;
    return e;
}

/// Run `frames` steps and return every spawn, flattened.
std::vector<GpuSpawn> runFrames(ParticleSystem& system, uint32_t frames) {
    std::vector<GpuSpawn> all;
    for (uint32_t i = 0; i < frames; ++i) {
        system.update(kStep);
        const std::vector<GpuSpawn>& s = system.spawns();
        all.insert(all.end(), s.begin(), s.end());
    }
    return all;
}

} // namespace

// --------------------------------------------------------------------- sizing

TEST(ParticleSystem, EmptyEmitterListAllocatesNothing) {
    // The case every scene in this repository but one takes. A zero capacity is what
    // the renderer tests to skip the pipelines, the buffers and all four passes.
    ParticleSystem system;
    system.setEmitters({}, 0);
    EXPECT_TRUE(system.empty());
    EXPECT_EQ(system.capacity(), 0u);

    system.update(kStep);
    EXPECT_EQ(system.aliveCount(), 0u);
    EXPECT_TRUE(system.spawns().empty());
}

TEST(ParticleSystem, CapacityIsTheSteadyStateRoundedUp) {
    // 100/s x 2 s is 200 particles alive at any moment; +1 for the sub-one-per-frame
    // case, rounded up to the power of two the bitonic sort needs.
    EXPECT_EQ(ParticleSystem::requiredCapacity({steadyEmitter(100.0f, 2.0f)}), 256u);
    EXPECT_EQ(ParticleSystem::requiredCapacity({steadyEmitter(500.0f, 4.0f)}), 2048u);
    // Two emitters are the sum, not the maximum.
    EXPECT_EQ(ParticleSystem::requiredCapacity({steadyEmitter(100.0f, 2.0f), steadyEmitter(100.0f, 2.0f)}), 512u);
}

TEST(ParticleSystem, LifetimeJitterWidensTheCapacity) {
    // The *longest* a particle can live is what the pool has to hold, not the mean. A
    // capacity computed from the mean would be exceeded by exactly the tail the jitter
    // exists to produce.
    ParticleEmitter jittered = steadyEmitter(100.0f, 2.0f);
    jittered.lifetimeJitter = 0.5f;
    EXPECT_FLOAT_EQ(jittered.maxLifetime(), 3.0f);
    EXPECT_EQ(ParticleSystem::requiredCapacity({jittered}), 512u);
}

TEST(ParticleSystem, TheBudgetIsAFloorAndStaysAPowerOfTwo) {
    // **A budget no longer caps**. It used to round *down* to whatever the caller
    // stated, on the grounds that handing back more slots than were asked for is not a
    // budget -- but the pool grows now, so clamping down only meant resizing on the first
    // step and reporting refusals that never happened.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(1000.0f, 4.0f)}, 1000);
    EXPECT_EQ(system.capacity(), 4096u) << "the emitters need 4096 and a lower floor must not cut them down";

    // What a stated budget still buys is allocating up front, so a game that knows it will
    // spawn far past what its scene declares pays for the pool once rather than growing into
    // it. Rounded up, because a floor rounded down is not one.
    ParticleSystem preallocated;
    preallocated.setEmitters({steadyEmitter(10.0f, 1.0f)}, 1000);
    EXPECT_EQ(preallocated.capacity(), 1024u);

    // And zero still means "whatever the emitters need".
    ParticleSystem unbounded;
    unbounded.setEmitters({steadyEmitter(1000.0f, 4.0f)}, 0);
    EXPECT_EQ(unbounded.capacity(), 4096u);
}

TEST(ParticleSystem, CapacityNeverExceedsTheSortKeyCeiling) {
    // kMaxCapacity is not a preference: the sort key packs a slot index and a quantised
    // depth into one 32-bit word, and this is where the slot field stops.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(1000000.0f, 10.0f)}, 0);
    EXPECT_EQ(system.capacity(), ParticleSystem::kMaxCapacity);
}

// -------------------------------------------------------------------- emission

TEST(ParticleSystem, RateIsHonouredOverTime) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 10.0f)}, 0);

    // 60 per second at 1/60 s a step is one per step, and the accumulator is what makes
    // that exact rather than approximately right.
    const std::vector<GpuSpawn> spawns = runFrames(system, 60);
    EXPECT_EQ(spawns.size(), 60u);
    EXPECT_EQ(system.aliveCount(), 60u);
}

TEST(ParticleSystem, SubFrameRatesStillEmit) {
    // Half a particle per second. Without a fractional accumulator this emitter would
    // round to zero every frame and never emit anything at all.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(0.5f, 30.0f)}, 0);
    EXPECT_GT(system.capacity(), 0u);

    const std::vector<GpuSpawn> first = runFrames(system, 60); // one second
    EXPECT_TRUE(first.empty());
    const std::vector<GpuSpawn> next = runFrames(system, 120); // two more
    EXPECT_EQ(next.size(), 1u);
}

TEST(ParticleSystem, SlotsAreFreedExactlyWhenTheParticleExpires) {
    // One particle per step with a lifetime of nine and a half steps: the count ramps
    // to ten and holds, because from then on one dies for every one born. A free that
    // came a frame late would settle at eleven, and the pool would be one short of the
    // population it was sized for.
    //
    // Half a step, not a whole one, and deliberately. A lifetime that is an exact
    // multiple of the step puts `birth + lifetime` exactly on `now`, and `now` is a
    // running sum of steps -- so the comparison lands on whichever side the accumulated
    // rounding falls, and the count flickers between ten and eleven. That is not a
    // defect and it is why a particle carries its birth time: the shader runs the same
    // comparison on the same two numbers, so it flickers *with* this rather than
    // against it, and the two never disagree about which frame a particle dies on.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 9.5f * kStep)}, 0);

    for (uint32_t i = 0; i < 10; ++i) {
        system.update(kStep);
        EXPECT_EQ(system.aliveCount(), i + 1) << "frame " << i;
    }
    for (uint32_t i = 0; i < 20; ++i) {
        system.update(kStep);
        EXPECT_EQ(system.aliveCount(), 10u) << "steady frame " << i;
    }
}

TEST(ParticleSystem, SlotsAreHandedOutInAscendingOrder) {
    // The order is the property, not merely the fact of reuse: it is what makes two
    // runs agree on which slot a particle landed in, and therefore on the image.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 3.0f * kStep)}, 0);

    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < 12; ++i) {
        system.update(kStep);
        for (const GpuSpawn& s : system.spawns()) slots.push_back(s.meta.x);
    }

    ASSERT_EQ(slots.size(), 12u);
    for (uint32_t slot : slots) EXPECT_LT(slot, system.capacity());
    // The first pass hands out the lowest free slots in order, and every later one
    // takes the lowest slot that has come free rather than the next one along.
    EXPECT_EQ(slots[0], 0u);
    EXPECT_EQ(slots[1], 1u);
    EXPECT_EQ(slots[2], 2u);
}

TEST(ParticleSystem, TwoRunsProduceIdenticalSpawns) {
    // The determinism 5.3 rests on, stated as a test rather than as a comment. Every
    // field is compared, not just the slot: the seed decides the direction and the
    // lifetime decides the death, so two runs agreeing on slots and disagreeing on
    // seeds would still be two different images.
    ParticleEmitter e = steadyEmitter(377.0f, 1.7f);
    e.lifetimeJitter = 0.4f;

    ParticleSystem a;
    ParticleSystem b;
    a.setEmitters({e}, 0);
    b.setEmitters({e}, 0);

    const std::vector<GpuSpawn> first = runFrames(a, 200);
    const std::vector<GpuSpawn> second = runFrames(b, 200);

    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].meta.x, second[i].meta.x) << "slot at " << i;
        EXPECT_EQ(first[i].meta.y, second[i].meta.y) << "emitter at " << i;
        EXPECT_EQ(first[i].meta.z, second[i].meta.z) << "seed at " << i;
        EXPECT_FLOAT_EQ(first[i].params.x, second[i].params.x) << "lifetime at " << i;
        EXPECT_FLOAT_EQ(first[i].params.y, second[i].params.y) << "birth at " << i;
    }
}

TEST(ParticleSystem, SeedsAreUniquePerEmitter) {
    // The seed is an emission counter, so it is a function of the emitter's own history
    // and of nothing else. Two emitters must not share a sequence, or two jets aimed
    // the same way jitter identically and read as one.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 5.0f), steadyEmitter(60.0f, 5.0f)}, 0);

    const std::vector<GpuSpawn> spawns = runFrames(system, 30);
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const GpuSpawn& s : spawns) {
        EXPECT_TRUE(seen.insert({s.meta.y, s.meta.z}).second) << "emitter " << s.meta.y << " reused seed " << s.meta.z;
    }
    EXPECT_EQ(spawns.size(), 60u);
}

TEST(ParticleSystem, BirthsAreSpreadAcrossTheStep) {
    // Ten births in one step must not all carry the same birth time. Stacking them on
    // one instant is what turns a fast jet into a string of beads, and the spread is a
    // property of the *record*, since the shader integrates from it.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(600.0f, 2.0f)}, 0);
    system.update(kStep);

    const std::vector<GpuSpawn>& spawns = system.spawns();
    ASSERT_EQ(spawns.size(), 10u);
    for (size_t i = 1; i < spawns.size(); ++i) {
        EXPECT_GT(spawns[i].params.y, spawns[i - 1].params.y) << "birth " << i << " is not later than the one before";
    }
    // All inside the step that produced them, and none in the future.
    EXPECT_LE(spawns.back().params.y, system.time());
    EXPECT_GE(spawns.front().params.y, system.time() - kStep);
}

TEST(ParticleSystem, AFullPoolDropsAndSaysSoUntilItIsGrown) {
    // The 0.9 rule applied to particles, and it still has a window to apply in: growth is
    // the *engine's* pairing and happens after the step, so an emitter created at runtime
    // spends at least one step against the pool as it stands. A count rather than a boolean,
    // because "some were dropped" and "half of them were dropped" are different problems.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(10.0f, 1.0f)}, 0);
    const uint32_t small = system.capacity();
    ASSERT_EQ(small, 16u);

    ASSERT_TRUE(system.create(steadyEmitter(600.0f, 10.0f)).valid());
    ASSERT_EQ(system.capacity(), small) << "create must not resize -- only the engine's paired grow may";

    runFrames(system, 30);
    EXPECT_EQ(system.aliveCount(), small);
    EXPECT_GT(system.droppedSpawns(), 0u);
    // And it never overruns the pool, whatever the emitter asked for.
    EXPECT_LE(system.aliveCount(), system.capacity());

    // The report is what the growth is driven off, so the two have to agree that the pool is
    // now too small.
    EXPECT_GT(system.wantedCapacity(), small);
}

TEST(ParticleSystem, GpuEmitterCarriesTheAuthoredFlags) {
    ParticleEmitter e = steadyEmitter(10.0f, 1.0f);
    e.emissive = true;
    e.collides = true;
    e.texture = 7;
    e.sizeStart = 0.25f;
    e.sizeEnd = 1.5f;
    e.restitution = 0.6f;
    e.emissiveIntensity = 3.0f;
    e.coneAngle = 0.5f;
    e.speedJitter = 0.2f;
    e.drag = 1.25f;

    ParticleSystem system;
    system.setEmitters({e}, 0);

    GpuEmitter g{};
    system.writeGpuEmitters(&g);
    EXPECT_EQ(g.flags.x, 7u);
    EXPECT_EQ(g.flags.y, static_cast<uint32_t>(kEmitterEmissive | kEmitterCollides));
    EXPECT_FLOAT_EQ(g.params.x, 0.25f);
    EXPECT_FLOAT_EQ(g.params.y, 1.5f);
    EXPECT_FLOAT_EQ(g.params.z, 0.6f);
    EXPECT_FLOAT_EQ(g.params.w, 3.0f);
    EXPECT_FLOAT_EQ(g.boxExtent.w, 0.5f);
    EXPECT_FLOAT_EQ(g.velocity.w, 0.2f);
    EXPECT_FLOAT_EQ(g.gravity.w, 1.25f);
}

TEST(ParticleSystem, GpuEmitterPacksTheFlipbookGrid) {
    ParticleEmitter e = steadyEmitter(10.0f, 1.0f);
    e.texture = 3;
    e.flipbookCols = 4;
    e.flipbookRows = 8;
    e.spin = 1.5f;
    e.erosion = 0.4f;
    e.flipbookLoops = 0.25f;

    ParticleSystem system;
    system.setEmitters({e}, 0);

    GpuEmitter g{};
    system.writeGpuEmitters(&g);
    // Columns in the low half, rows in the high half. The shader unpacks it the same way
    // and divides by the product, which is why zero is not representable -- see below.
    EXPECT_EQ(g.flags.z & 0xFFFFu, 4u);
    EXPECT_EQ(g.flags.z >> 16, 8u);
    EXPECT_FLOAT_EQ(g.sprite.x, 1.5f);
    EXPECT_FLOAT_EQ(g.sprite.y, 0.4f);
    EXPECT_FLOAT_EQ(g.sprite.z, 0.25f);
}

TEST(ParticleSystem, AnUnauthoredFlipbookIsOneStillFrame) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(10.0f, 1.0f)}, 0);

    GpuEmitter g{};
    system.writeGpuEmitters(&g);
    // 1x1 rather than 0x0, and the difference is a division: the vertex shader takes the
    // cell size as 1/(cols, rows). Every emitter authored before sheets existed lands here
    // and has to render exactly as it did.
    EXPECT_EQ(g.flags.z, 0x00010001u);
    EXPECT_FLOAT_EQ(g.sprite.x, 0.0f);
    EXPECT_FLOAT_EQ(g.sprite.y, 0.0f);
    EXPECT_FLOAT_EQ(g.sprite.z, 1.0f);
}

TEST(ParticleSystem, AZeroFlipbookGridIsClampedRatherThanUploaded) {
    ParticleEmitter e = steadyEmitter(10.0f, 1.0f);
    e.flipbookCols = 0;
    e.flipbookRows = 0;

    ParticleSystem system;
    system.setEmitters({e}, 0);

    GpuEmitter g{};
    system.writeGpuEmitters(&g);
    EXPECT_EQ(g.flags.z, 0x00010001u);
}

// ---------------------------------------------------------------------- random

TEST(ParticleSystem, RandomIsInRangeAndDecorrelated) {
    // Consecutive seeds are the input this hash actually gets -- they are an emission
    // counter -- so "decorrelates consecutive values" is the property that matters, not
    // the average quality of the stream.
    float sum = 0.0f;
    float previous = particleRandom(0, kRandomLifetime);
    float maxJump = 0.0f;
    for (uint32_t seed = 0; seed < 4096; ++seed) {
        const float v = particleRandom(seed, kRandomLifetime);
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
        sum += v;
        maxJump = std::max(maxJump, std::abs(v - previous));
        previous = v;
    }
    EXPECT_NEAR(sum / 4096.0f, 0.5f, 0.02f);
    // Consecutive seeds must be able to land far apart; a hash that merely incremented
    // would never exceed a tiny step.
    EXPECT_GT(maxJump, 0.9f);
}

TEST(ParticleSystem, StreamsAreIndependent) {
    // Two streams of the same seed must not return the same number, or a particle born
    // at the far corner of its box would always also be the fastest one.
    for (uint32_t seed = 0; seed < 64; ++seed) {
        EXPECT_NE(particleRandom(seed, kRandomPosX), particleRandom(seed, kRandomPosY));
        EXPECT_NE(particleRandom(seed, kRandomPosY), particleRandom(seed, kRandomPosZ));
        EXPECT_NE(particleRandom(seed, kRandomConeU), particleRandom(seed, kRandomConeV));
    }
}

// ----------------------------------------------------------------- glTF extras

TEST(ParticleSystem, ParsesAnEmitterOffANode) {
    const std::string doc = R"({
      "asset": {"version": "2.0"},
      "nodes": [
        {"name": "plain"},
        {"name": "jet", "extras": {"substrate_emitter": {
            "rate": 250.0, "lifetime": 1.5, "lifetimeJitter": 0.25,
            "velocity": [0.0, 3.0, 0.0], "speedJitter": 0.3, "coneAngle": 30.0,
            "boxExtent": [0.1, 0.2, 0.3], "gravity": [0.0, -2.0, 0.0], "drag": 0.5,
            "colorStart": [1.0, 0.5, 0.25, 0.8], "colorEnd": [0.1, 0.2, 0.3, 0.0],
            "sizeStart": 0.2, "sizeEnd": 0.9, "texture": 3,
            "flipbookCols": 4, "flipbookRows": 4, "flipbookLoops": 0.5, "spin": 0.75, "erosion": 0.6,
            "emissive": true, "emissiveIntensity": 4.0, "collides": true, "restitution": 0.7}}}
      ]
    })";

    std::vector<ParticleEmitter> out;
    ASSERT_TRUE(testing_extras::parseNodes(doc.data(), doc.size(), out, parseSceneEmitters));
    ASSERT_EQ(out.size(), 1u);

    const ParticleEmitter& e = out[0];
    EXPECT_EQ(e.node, 1u);
    EXPECT_EQ(e.name, "jet");
    EXPECT_FLOAT_EQ(e.rate, 250.0f);
    EXPECT_FLOAT_EQ(e.lifetime, 1.5f);
    EXPECT_FLOAT_EQ(e.lifetimeJitter, 0.25f);
    EXPECT_FLOAT_EQ(e.velocity.y, 3.0f);
    EXPECT_FLOAT_EQ(e.speedJitter, 0.3f);
    // Degrees in the file, radians in the struct.
    EXPECT_NEAR(e.coneAngle, 0.5235988f, 1e-5f);
    EXPECT_FLOAT_EQ(e.boxExtent.z, 0.3f);
    EXPECT_FLOAT_EQ(e.gravity.y, -2.0f);
    EXPECT_FLOAT_EQ(e.drag, 0.5f);
    EXPECT_FLOAT_EQ(e.colorStart.a, 0.8f);
    EXPECT_FLOAT_EQ(e.colorEnd.b, 0.3f);
    EXPECT_FLOAT_EQ(e.sizeStart, 0.2f);
    EXPECT_FLOAT_EQ(e.sizeEnd, 0.9f);
    EXPECT_EQ(e.texture, 3u);
    EXPECT_EQ(e.flipbookCols, 4u);
    EXPECT_EQ(e.flipbookRows, 4u);
    EXPECT_FLOAT_EQ(e.flipbookLoops, 0.5f);
    EXPECT_FLOAT_EQ(e.spin, 0.75f);
    EXPECT_FLOAT_EQ(e.erosion, 0.6f);
    EXPECT_TRUE(e.emissive);
    EXPECT_FLOAT_EQ(e.emissiveIntensity, 4.0f);
    EXPECT_TRUE(e.collides);
    EXPECT_FLOAT_EQ(e.restitution, 0.7f);
}

TEST(ParticleSystem, AbsentKeysKeepTheirDefaults) {
    // The same rule Config follows: a file naming three properties gets three
    // properties, so an emitter authored against an older build still loads.
    const std::string doc = R"({"nodes": [{"extras": {"substrate_emitter": {"rate": 5.0}}}]})";
    std::vector<ParticleEmitter> out;
    ASSERT_TRUE(testing_extras::parseNodes(doc.data(), doc.size(), out, parseSceneEmitters));
    ASSERT_EQ(out.size(), 1u);

    const ParticleEmitter reference;
    EXPECT_FLOAT_EQ(out[0].rate, 5.0f);
    EXPECT_FLOAT_EQ(out[0].lifetime, reference.lifetime);
    EXPECT_FLOAT_EQ(out[0].sizeStart, reference.sizeStart);
    EXPECT_EQ(out[0].texture, reference.texture);
    EXPECT_EQ(out[0].flipbookCols, reference.flipbookCols);
    EXPECT_EQ(out[0].flipbookRows, reference.flipbookRows);
    EXPECT_FLOAT_EQ(out[0].flipbookLoops, reference.flipbookLoops);
    EXPECT_FLOAT_EQ(out[0].spin, reference.spin);
    EXPECT_FLOAT_EQ(out[0].erosion, reference.erosion);
    EXPECT_FALSE(out[0].emissive);
}

TEST(ParticleSystem, ADocumentWithNoEmittersIsNotAFailure) {
    // Sponza. Returning false here would make every scene in the repository log a
    // warning about a feature it does not use.
    const std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{"name": "a"}, {"extras": {"x": 1}}]})";
    std::vector<ParticleEmitter> out;
    EXPECT_TRUE(testing_extras::parseNodes(doc.data(), doc.size(), out, parseSceneEmitters));
    EXPECT_TRUE(out.empty());

    // A document with no nodes at all is equally fine.
    const std::string empty = R"({"asset": {"version": "2.0"}})";
    EXPECT_TRUE(testing_extras::parseNodes(empty.data(), empty.size(), out, parseSceneEmitters));
    EXPECT_TRUE(out.empty());
}

TEST(ParticleSystem, BytesThatAreNotGltfAreRefused) {
    const std::string junk = "not json at all";
    std::vector<ParticleEmitter> out;
    EXPECT_FALSE(testing_extras::parseNodes(junk.data(), junk.size(), out, parseSceneEmitters));
    EXPECT_FALSE(testing_extras::parseNodes(nullptr, 0, out, parseSceneEmitters));
}

TEST(ParticleSystem, GlbJsonChunkIsUnwrapped) {
    // A GLB is a header and a chunk table, and the emitters are in the first chunk. The
    // alternative to those fifteen lines was a stated limitation reading "particles work
    // except in the container half the world ships in".
    const std::string json = R"({"nodes":[{"extras":{"substrate_emitter":{"rate":42.0}}}]})";

    std::string glb = "glTF";
    const auto append32 = [&glb](uint32_t v) {
        for (int i = 0; i < 4; ++i) glb.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
    };
    append32(2);                                       // version
    append32(static_cast<uint32_t>(28 + json.size())); // total length
    append32(static_cast<uint32_t>(json.size()));      // chunk 0 length
    append32(0x4E4F534Au);                             // 'JSON'
    glb += json;

    std::vector<ParticleEmitter> out;
    ASSERT_TRUE(testing_extras::parseNodes(glb.data(), glb.size(), out, parseSceneEmitters));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].rate, 42.0f);

    // A truncated container is refused rather than read past.
    const std::string truncated = glb.substr(0, 16);
    out.clear();
    EXPECT_FALSE(testing_extras::parseNodes(truncated.data(), truncated.size(), out, parseSceneEmitters));
}

// ========================================== lifetimes: create and destroy

TEST(EmitterLifetime, SetEmittersIssuesHandlesForEverySlot) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f), steadyEmitter(60.0f, 4.0f)}, 0);

    const EmitterId first = system.emitterAt(0);
    const EmitterId second = system.emitterAt(1);
    EXPECT_TRUE(system.valid(first));
    EXPECT_TRUE(system.valid(second));
    EXPECT_NE(first.index, second.index);
    EXPECT_FALSE(system.emitterAt(99).valid());
}

TEST(EmitterLifetime, DestroyingAnEmitterStopsItSpawningAndStalesTheHandle) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const EmitterId id = system.emitterAt(0);
    ASSERT_TRUE(system.valid(id));

    for (int i = 0; i < 10; ++i) system.update(kStep);
    EXPECT_GT(system.aliveCount(), 0u);

    system.destroy(id);
    EXPECT_TRUE(id.valid());
    EXPECT_FALSE(system.valid(id));

    system.update(kStep);
    EXPECT_TRUE(system.spawns().empty()) << "a retired emitter must not spawn";
}

TEST(EmitterLifetime, ARetiredSlotIsReusedAndDoesNotAlias) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const EmitterId first = system.emitterAt(0);

    system.destroy(first);
    const EmitterId reused = system.create(steadyEmitter(60.0f, 4.0f));
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(reused.index, first.index) << "the slot should have been reused";
    EXPECT_NE(reused.generation, first.generation);
    EXPECT_TRUE(system.valid(reused));
    EXPECT_FALSE(system.valid(first));
}

TEST(EmitterLifetime, CreateRefusesWhatThePoolCannotHold) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const uint32_t capacity = system.capacity();
    ASSERT_GT(capacity, 0u);

    EXPECT_FALSE(system.create(steadyEmitter(100000.0f, 30.0f)).valid());
    EXPECT_EQ(system.capacity(), capacity) << "the pool must not have grown";
}

TEST(EmitterLifetime, WantedCapacityCountsTheLiveEmittersAndTheGrowthKeepsWhatIsInFlight) {
    // **The half of C40 that lives in this class.** `wantedCapacity()` is what the engine
    // grows to, so it has to follow the emitters actually running -- a retired slot keeps its
    // record until something overwrites it, and counting those would pin the pool at the
    // high-water mark of every effect that ever played.
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const uint32_t base = system.capacity();
    ASSERT_GT(base, 0u);
    EXPECT_EQ(system.wantedCapacity(), base);

    // Something in flight, so the growth below has something to lose.
    (void)runFrames(system, 30);
    ASSERT_GT(system.aliveCount(), 0u);
    const uint32_t aliveBefore = system.aliveCount();

    const EmitterId extra = system.create(steadyEmitter(500.0f, 4.0f));
    ASSERT_TRUE(system.valid(extra));
    EXPECT_GT(system.wantedCapacity(), base) << "a live emitter the pool cannot hold must ask for a bigger one";

    ASSERT_TRUE(system.grow(system.wantedCapacity()));
    EXPECT_GE(system.capacity(), system.wantedCapacity());
    // Resized rather than reset: the particles already alive are still alive, at the same
    // slots, with the same death times.
    EXPECT_EQ(system.aliveCount(), aliveBefore);

    // Retiring the new emitter takes its claim back out again, so a burst does not hold the
    // pool at its peak forever.
    system.destroy(extra);
    (void)runFrames(system, 1);
    EXPECT_LE(system.wantedCapacity(), base);

    // And a growth that is already satisfied is not a growth.
    EXPECT_FALSE(system.grow(1u));
}

TEST(EmitterLifetime, AWholeListReplacementStalesEveryHandleFromBeforeIt) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const EmitterId old = system.emitterAt(0);
    ASSERT_TRUE(system.valid(old));

    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    EXPECT_FALSE(system.valid(old)) << "a handle into the previous list validated against the new one";
}

// ============================================== runtime effects

TEST(RuntimeEffects, ABurstEmitsOnceAndThenStops) {
    ParticleSystem system;
    ParticleEmitter e = steadyEmitter(60.0f, 1.0f);
    e.burst = 8;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    ASSERT_TRUE(system.spawnEffect(e, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}).valid());

    system.update(kStep);
    // Counted per emitter: the continuous one that sized the pool is spawning too.
    uint32_t fromBurst = 0;
    for (const GpuSpawn& s : system.spawns()) fromBurst += s.meta.y == 1u ? 1u : 0u;
    EXPECT_EQ(fromBurst, 8u) << "the whole burst lands on one update";

    for (int i = 0; i < 5; ++i) {
        system.update(kStep);
        for (const GpuSpawn& s : system.spawns()) EXPECT_NE(s.meta.y, 1u) << "the one-shot emitted twice";
    }
}

TEST(RuntimeEffects, AOneShotReleasesItsOwnSlotWhenTheLastParticleDies) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);

    ParticleEmitter e = steadyEmitter(60.0f, 0.1f);
    e.burst = 4;
    const EmitterId id = system.spawnEffect(e, {0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(system.valid(id));

    system.update(kStep);
    EXPECT_TRUE(system.valid(id)) << "still alive while its particles are";

    for (int i = 0; i < 20; ++i) system.update(kStep);
    EXPECT_FALSE(system.valid(id)) << "a finished one-shot must retire itself";
}

TEST(RuntimeEffects, ManyEffectsRecycleOneSlotRatherThanGrowingTheList) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);
    const size_t base = system.emitters().size();

    ParticleEmitter e = steadyEmitter(60.0f, 0.05f);
    e.burst = 2;
    for (int i = 0; i < 200; ++i) {
        const EmitterId id = system.spawnEffect(e, {static_cast<float>(i), 0.0f, 0.0f});
        ASSERT_TRUE(id.valid()) << "refused on effect " << i;
        for (int f = 0; f < 8; ++f) system.update(kStep);
    }
    EXPECT_LE(system.emitters().size(), base + 1u) << "one slot recycled, not 200 appended";
}

TEST(RuntimeEffects, SpawnEffectAimsTheEmitterAlongTheNormal) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);

    ParticleEmitter e = steadyEmitter(60.0f, 1.0f);
    e.burst = 1;
    const glm::vec3 wall{1.0f, 0.0f, 0.0f};
    const EmitterId id = system.spawnEffect(e, {5.0f, 1.0f, 2.0f}, wall);
    ASSERT_TRUE(id.valid());

    const glm::mat4& t = system.emitters()[id.index].transform;
    EXPECT_EQ(glm::vec3(t[3]), glm::vec3(5.0f, 1.0f, 2.0f));
    EXPECT_NEAR(glm::dot(glm::vec3(t[1]), wall), 1.0f, 1e-5f);
}

TEST(RuntimeEffects, SpawnEffectWithNoBurstStillTerminates) {
    ParticleSystem system;
    system.setEmitters({steadyEmitter(60.0f, 4.0f)}, 0);

    ParticleEmitter e = steadyEmitter(60.0f, 0.1f);
    e.burst = 0;
    const EmitterId id = system.spawnEffect(e, {0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(id.valid());

    for (int i = 0; i < 40; ++i) system.update(kStep);
    EXPECT_FALSE(system.valid(id));
}
