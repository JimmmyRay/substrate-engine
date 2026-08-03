#include "scene/SpriteTable.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace scene;

namespace fs = std::filesystem;

/**
 * @file tests/SpriteTableTests.cpp
 * @brief P4's arithmetic and its lifetime rules, with no device anywhere near them.
 *
 * The sprite pass is a vertex shader and a fragment shader, and neither is the thing that
 * goes wrong. What goes wrong is the order two layers draw in, a slot handed to two
 * holders when a sprite is destroyed and another created, a pivot applied on the wrong
 * side of a rotation, or a re-sort on every frame that a position changed -- and every one
 * of those is a `std::vector` and some arithmetic. `SpriteTable.cpp` is in
 * `SUBSTRATE_HOSTED_SOURCES` for exactly that reason, so all of it runs under ASan.
 *
 * The image table is real rather than mocked: it is hosted too, and the one thing this
 * table asks of it -- `slot(id)` refusing a stale handle -- is the interesting half.
 */
class SpriteTableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto unique = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root = fs::temp_directory_path() / ("substrate_sprites_" + std::string(unique));
        fs::remove_all(root);
        fs::create_directories(root);
        images.init(64);
        sprites.init(&images);
    }

    void TearDown() override {
        sprites.shutdown();
        images.shutdown();
        fs::remove_all(root);
    }

    /// `ImageTable::load` only asks that a file exists -- decoding is the renderer's half.
    gfx::ImageId makeImage(const char* name) {
        const fs::path path = root / name;
        std::ofstream(path) << "not an image, and this table does not decode one";
        return images.load(path.string());
    }

    /// Run `mutate` and require `revision()` to have moved. Here on the base rather than on
    /// the revision fixture because the sheet half of those tests needs it too, and it is
    /// one rung rather than two: nothing outside this file calls it. It returns nothing on
    /// purpose -- a helper that answered with a bool would let a caller forget to assert.
    template <typename F> void bumps(const char* what, F&& mutate) {
        const uint64_t before = sprites.revision();
        mutate();
        EXPECT_GT(sprites.revision(), before) << what << " did not bump the revision";
    }

    template <typename F> void holds(const char* what, F&& mutate) {
        const uint64_t before = sprites.revision();
        mutate();
        EXPECT_EQ(sprites.revision(), before) << what << " bumped the revision and changed no sprite";
    }

    fs::path root;
    gfx::ImageTable images;
    SpriteTable sprites;
};

// -------------------------------------------------------------------------- lifetime

TEST_F(SpriteTableTest, ASpriteNeedsALiveLayer) {
    const SpriteLayerId layer = sprites.createLayer({});
    EXPECT_TRUE(sprites.valid(layer));

    const SpriteId s = sprites.create(layer, {});
    EXPECT_TRUE(sprites.valid(s));
    EXPECT_EQ(sprites.count(), 1u);

    // A handle nobody issued, and a zeroed one. Both are refused, and the second is what
    // reserving generation zero buys: a default-constructed handle is not slot 0.
    EXPECT_FALSE(sprites.valid(SpriteLayerId{}));
    EXPECT_FALSE(sprites.create(SpriteLayerId{}, {}).valid());
    EXPECT_EQ(sprites.count(), 1u);
}

TEST_F(SpriteTableTest, ADestroyedHandleIsRefusedAndItsSlotIsReused) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId first = sprites.create(layer, {});
    sprites.destroy(first);

    EXPECT_FALSE(sprites.valid(first));
    EXPECT_EQ(sprites.count(), 0u);

    const SpriteId second = sprites.create(layer, {});
    // The slot came back and the generation did not, which is the whole point: the old
    // handle names the same index and is still refused.
    EXPECT_EQ(second.index, first.index);
    EXPECT_NE(second.generation, first.generation);
    EXPECT_FALSE(sprites.valid(first));
    EXPECT_TRUE(sprites.valid(second));

    // And a setter through the stale one changes nothing rather than writing into the
    // sprite that took its place.
    sprites.setPosition(first, {99.0f, 99.0f});
    sprites.prepare();
    EXPECT_FLOAT_EQ(sprites.draws()[0].posSize.x, 0.0f);
}

TEST_F(SpriteTableTest, DestroyingASpriteTwiceIsANoOp) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {});
    sprites.destroy(s);
    sprites.destroy(s);
    EXPECT_EQ(sprites.count(), 0u);
    EXPECT_TRUE(sprites.draws().empty());
}

TEST_F(SpriteTableTest, DestroyingALayerTakesItsSpritesWithIt) {
    const SpriteLayerId keep = sprites.createLayer({.order = 0});
    const SpriteLayerId drop = sprites.createLayer({.order = 1});

    const SpriteId kept = sprites.create(keep, {});
    const SpriteId gone = sprites.create(drop, {});
    EXPECT_EQ(sprites.count(), 2u);

    sprites.destroyLayer(drop);
    EXPECT_EQ(sprites.layerCount(), 1u);
    EXPECT_EQ(sprites.count(), 1u);
    // Through `destroy`, so the handle a game is still holding reports staleness rather
    // than naming whatever takes the slot next.
    EXPECT_FALSE(sprites.valid(gone));
    EXPECT_TRUE(sprites.valid(kept));

    sprites.prepare();
    EXPECT_EQ(sprites.draws().size(), 1u);
}

TEST_F(SpriteTableTest, DestroyingTheMiddleOfTheArrayKeepsEveryOtherHandleGood) {
    const SpriteLayerId layer = sprites.createLayer({});
    std::vector<SpriteId> ids;
    for (int i = 0; i < 8; ++i) {
        ids.push_back(sprites.create(layer, {.position = {static_cast<float>(i), 0.0f}}));
    }

    sprites.destroy(ids[3]);
    sprites.prepare();

    // The swap-remove moved the last entry into slot 3 and the sort put the order back.
    // Every surviving handle still names its own sprite, which is the property an index
    // that moves under a handle exists to preserve.
    EXPECT_EQ(sprites.draws().size(), 7u);
    for (int i = 0; i < 8; ++i) {
        if (i == 3) continue;
        ASSERT_TRUE(sprites.valid(ids[i]));
        sprites.setSize(ids[i], {static_cast<float>(100 + i), 1.0f});
    }
    for (const GpuSprite& s : sprites.draws()) {
        EXPECT_FLOAT_EQ(s.posSize.z, 100.0f + s.posSize.x);
    }
}

// ------------------------------------------------------------------------ draw order

TEST_F(SpriteTableTest, LayerOrderDecidesDrawOrderAndCreationOrderBreaksTies) {
    const SpriteLayerId background = sprites.createLayer({.order = -10});
    const SpriteLayerId actors = sprites.createLayer({.order = 0});

    // Deliberately created back to front against the answer: the actor first, then two
    // background sprites, so a table that simply kept insertion order fails this.
    (void)sprites.create(actors, {.position = {2.0f, 0.0f}});
    (void)sprites.create(background, {.position = {0.0f, 0.0f}});
    (void)sprites.create(background, {.position = {1.0f, 0.0f}});

    sprites.prepare();
    ASSERT_EQ(sprites.draws().size(), 3u);
    EXPECT_FLOAT_EQ(sprites.draws()[0].posSize.x, 0.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[1].posSize.x, 1.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[2].posSize.x, 2.0f);
}

TEST_F(SpriteTableTest, ANegativeLayerOrderSortsBeforeZero) {
    // The one thing a naive unsigned key gets wrong, and it is the sketch's own example:
    // a background at -10 has to draw *first*, not last.
    const SpriteLayerId front = sprites.createLayer({.order = 5});
    const SpriteLayerId back = sprites.createLayer({.order = -10});
    (void)sprites.create(front, {.position = {1.0f, 0.0f}});
    (void)sprites.create(back, {.position = {0.0f, 0.0f}});

    sprites.prepare();
    EXPECT_FLOAT_EQ(sprites.draws()[0].posSize.x, 0.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[1].posSize.x, 1.0f);
}

TEST_F(SpriteTableTest, ReorderingALayerReordersItsSprites) {
    const SpriteLayerId a = sprites.createLayer({.order = 0});
    const SpriteLayerId b = sprites.createLayer({.order = 1});
    (void)sprites.create(a, {.position = {0.0f, 0.0f}});
    (void)sprites.create(b, {.position = {1.0f, 0.0f}});
    sprites.prepare();
    EXPECT_FLOAT_EQ(sprites.draws()[0].posSize.x, 0.0f);

    sprites.setLayerOrder(a, 2);
    sprites.prepare();
    EXPECT_FLOAT_EQ(sprites.draws()[0].posSize.x, 1.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[1].posSize.x, 0.0f);
}

TEST_F(SpriteTableTest, MovingSpritesDoesNotResort) {
    const SpriteLayerId layer = sprites.createLayer({});
    std::vector<SpriteId> ids;
    for (int i = 0; i < 64; ++i) ids.push_back(sprites.create(layer, {}));

    sprites.prepare();
    const uint64_t after = sprites.sortCount();

    // The claim the whole design rests on: a layer is a sort key and a position is not
    // part of it, so ten thousand sprites moving every frame re-sort nothing. If this
    // ever fails, the sprite budget on the card is wrong.
    for (int frame = 0; frame < 100; ++frame) {
        for (SpriteId id : ids) sprites.setPosition(id, {static_cast<float>(frame), 0.0f});
        sprites.prepare();
    }
    EXPECT_EQ(sprites.sortCount(), after);

    // And creating one does.
    (void)sprites.create(layer, {});
    sprites.prepare();
    EXPECT_EQ(sprites.sortCount(), after + 1);
}

// ---------------------------------------------------------------------------- images

TEST_F(SpriteTableTest, ASpriteResolvesItsImageToASlot) {
    const gfx::ImageId image = makeImage("hero.png");
    ASSERT_TRUE(images.valid(image));

    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {.image = image});
    sprites.prepare();
    EXPECT_EQ(sprites.draws()[0].meta.x, images.slot(image));
    EXPECT_NE(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);
}

TEST_F(SpriteTableTest, DestroyingTheImageDropsTheSpriteToTheFallback) {
    const gfx::ImageId image = makeImage("hero.png");
    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {.image = image});
    sprites.prepare();
    ASSERT_NE(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);

    images.destroy(image);
    sprites.prepare();
    // The font atlas, which is visible and harmless -- and specifically *not* whatever
    // image takes the slot next, which is the alias the generation exists to prevent.
    EXPECT_EQ(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);

    const gfx::ImageId replacement = makeImage("other.png");
    sprites.prepare();
    EXPECT_EQ(images.slot(replacement), 1u);
    EXPECT_EQ(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);
}

TEST_F(SpriteTableTest, ASpriteWithNoImageDrawsTheFallback) {
    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {});
    sprites.prepare();
    EXPECT_EQ(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);
}

// -------------------------------------------------------------------- what the GPU sees

TEST_F(SpriteTableTest, TheGpuStructIsWhatTheShaderReads) {
    // The shader reads this by offset. A field reordered here and not there is a sprite
    // drawn with its tint as its position, which no test of behaviour would name.
    EXPECT_EQ(sizeof(GpuSprite), 64u);
    EXPECT_EQ(offsetof(GpuSprite, posSize), 0u);
    EXPECT_EQ(offsetof(GpuSprite, rotPivot), 16u);
    EXPECT_EQ(offsetof(GpuSprite, uvRect), 32u);
    EXPECT_EQ(offsetof(GpuSprite, meta), 48u);
}

TEST_F(SpriteTableTest, RotationIsWrittenAsACosineAndASine) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {.rotation = 1.0472f});
    sprites.prepare();
    EXPECT_NEAR(sprites.draws()[0].rotPivot.x, std::cos(1.0472f), 1e-6f);
    EXPECT_NEAR(sprites.draws()[0].rotPivot.y, std::sin(1.0472f), 1e-6f);

    // Once per change, not once per vertex per frame -- which is why the setter is the
    // only place `sin` and `cos` appear in this subsystem.
    sprites.setRotation(s, 0.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[0].rotPivot.x, 1.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[0].rotPivot.y, 0.0f);
}

TEST_F(SpriteTableTest, WhiteIsExactAndTintRoundTripsThroughEightBits) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {});
    sprites.prepare();
    // The readback check depends on this and on nothing else about the packing: an
    // opaque white tint has to arrive at the shader as exactly 1.0 in all four channels,
    // or the multiply is not the identity and a texel authored is not a texel presented.
    EXPECT_EQ(sprites.draws()[0].meta.y, 0xFFFFFFFFu);

    sprites.setTint(s, {0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_EQ(sprites.draws()[0].meta.y, 0xFF000000u);
    // Out of range is clamped rather than wrapped, which is the difference between a
    // too-bright sprite and a black one.
    sprites.setTint(s, {2.0f, -1.0f, 0.0f, 1.0f});
    EXPECT_EQ(sprites.draws()[0].meta.y, 0xFF0000FFu);
}

TEST_F(SpriteTableTest, FlipsArePackedAsFlags) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {.flipX = true});
    sprites.prepare();
    EXPECT_EQ(sprites.draws()[0].meta.z, kSpriteFlipX);

    sprites.setFlip(s, false, true);
    EXPECT_EQ(sprites.draws()[0].meta.z, kSpriteFlipY);
    sprites.setFlip(s, true, true);
    EXPECT_EQ(sprites.draws()[0].meta.z, kSpriteFlipX | kSpriteFlipY);
    sprites.setFlip(s, false, false);
    EXPECT_EQ(sprites.draws()[0].meta.z, 0u);
}

TEST_F(SpriteTableTest, AUvRectStaysInTexels) {
    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {.uv = {16.0f, 32.0f, 16.0f, 16.0f}});
    sprites.prepare();
    // Untouched all the way to the buffer. The division by `textureSize` is the fragment
    // shader's, because nothing on this side of the boundary knows the file's dimensions
    // -- which is the whole reason the public API is in texels.
    EXPECT_FLOAT_EQ(sprites.draws()[0].uvRect.x, 16.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[0].uvRect.y, 32.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[0].uvRect.z, 16.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[0].uvRect.w, 16.0f);

    // And the default is the whole image, which is a zero rather than a size nobody here
    // could supply.
    (void)sprites.create(layer, {});
    sprites.prepare();
    EXPECT_FLOAT_EQ(sprites.draws()[1].uvRect.z, 0.0f);
    EXPECT_FLOAT_EQ(sprites.draws()[1].uvRect.w, 0.0f);
}

TEST_F(SpriteTableTest, TenThousandSpritesSortOnceAndKeepTheirOrder) {
    // The stated target, at the size the trace arm measures. What is being checked is not
    // speed -- that is `scripts/baseline.py`'s job -- but that the order is total and
    // reproducible at that count, which is what one draw over four layers depends on.
    SpriteLayerId layers[4];
    for (int i = 0; i < 4; ++i) layers[i] = sprites.createLayer({.order = i});

    for (uint32_t i = 0; i < 10000; ++i) {
        (void)sprites.create(layers[i % 4], {.position = {static_cast<float>(i), 0.0f}});
    }
    sprites.prepare();
    ASSERT_EQ(sprites.draws().size(), 10000u);

    // Four contiguous runs, each ascending in creation order.
    for (size_t i = 1; i < sprites.draws().size(); ++i) {
        const float previous = sprites.draws()[i - 1].posSize.x;
        const float current = sprites.draws()[i].posSize.x;
        // Within a layer the positions ascend by 4; the three layer boundaries are the
        // only places the sequence restarts.
        EXPECT_TRUE(current == previous + 4.0f || current < previous) << "at " << i;
    }
}

// ------------------------------------------------------------------ sheets and clips (P5)

/**
 * P5's half, and it is the half the readback cannot cover on its own. The readback proves
 * one cell of one sheet at one instant, bit-exact; everything below is the arithmetic that
 * has to be right at *every* instant -- the boundaries between cells, the wrap, the clamp,
 * the degenerate sheet, and the rule that a playback taken out of the walk stops advancing.
 * None of it needs a device, which is why `SpriteTable.cpp` is hosted.
 */
class SpriteSheetTest : public SpriteTableTest {
  protected:
    /// Two by two, cells of 32x24 -- the readback image's own slicing, so a failure here
    /// and a failure in `scripts/readback.sh` are talking about the same numbers.
    SpriteSheetId quadSheet() { return sprites.createSheet({.frame = {32, 24}, .columns = 2, .count = 4}); }
};

TEST_F(SpriteSheetTest, ACellIsColumnThenRow) {
    const SpriteSheetId sheet = quadSheet();
    ASSERT_TRUE(sprites.valid(sheet));

    // Left to right, then top to bottom. Frames 1 and 2 are the two that a transposed
    // slicing agrees with nowhere: one moves in x only, the other in y only, and the cell
    // is not square, so a swapped column and row is a different rectangle in both.
    EXPECT_EQ(sprites.frameUv(sheet, 0), glm::vec4(0.0f, 0.0f, 32.0f, 24.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 1), glm::vec4(32.0f, 0.0f, 32.0f, 24.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 2), glm::vec4(0.0f, 24.0f, 32.0f, 24.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 3), glm::vec4(32.0f, 24.0f, 32.0f, 24.0f));

    // Past the end repeats the last cell rather than reading a rectangle the file has no
    // texels for.
    EXPECT_EQ(sprites.frameUv(sheet, 4), sprites.frameUv(sheet, 3));
    EXPECT_EQ(sprites.frameUv(sheet, 4000), sprites.frameUv(sheet, 3));

    // And a handle naming no sheet yields a zero rect -- which the shader reads as *the
    // whole image*, the one thing a caller can see rather than undefined data.
    EXPECT_EQ(sprites.frameUv(SpriteSheetId{}, 0), glm::vec4(0.0f));
}

TEST_F(SpriteSheetTest, AMarginAndAGutterMoveEveryCell) {
    // The two fields that exist because exporters put them there, and the reason they are
    // separate: an origin shifts every cell once, spacing shifts cell n by n.
    const SpriteSheetId sheet =
        sprites.createSheet({.frame = {16, 16}, .columns = 3, .count = 6, .origin = {4, 6}, .spacing = {2, 3}});
    ASSERT_TRUE(sprites.valid(sheet));

    EXPECT_EQ(sprites.frameUv(sheet, 0), glm::vec4(4.0f, 6.0f, 16.0f, 16.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 1), glm::vec4(4.0f + 18.0f, 6.0f, 16.0f, 16.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 2), glm::vec4(4.0f + 36.0f, 6.0f, 16.0f, 16.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 3), glm::vec4(4.0f, 6.0f + 19.0f, 16.0f, 16.0f));
    EXPECT_EQ(sprites.frameUv(sheet, 5), glm::vec4(4.0f + 36.0f, 6.0f + 19.0f, 16.0f, 16.0f));
}

TEST_F(SpriteSheetTest, AZeroCellOrAnEmptySheetIsRefused) {
    // Not normalised, refused. A zero UV rect already means *the whole image* to the
    // shader, so a sheet that accepted a zero cell would draw the entire file for every
    // frame -- which looks like a shader bug and is an authoring one.
    EXPECT_FALSE(sprites.valid(sprites.createSheet({.frame = {0, 24}, .columns = 2, .count = 4})));
    EXPECT_FALSE(sprites.valid(sprites.createSheet({.frame = {32, 0}, .columns = 2, .count = 4})));
    EXPECT_FALSE(sprites.valid(sprites.createSheet({.frame = {32, 24}, .columns = 2, .count = 0})));
    EXPECT_EQ(sprites.sheetCount(), 0u);

    // Zero columns is the one that is read rather than refused: a sheet is at least one
    // cell wide by construction, and the alternative is a division by zero.
    const SpriteSheetId strip = sprites.createSheet({.frame = {8, 8}, .columns = 0, .count = 3});
    ASSERT_TRUE(sprites.valid(strip));
    EXPECT_EQ(sprites.frameUv(strip, 2), glm::vec4(0.0f, 16.0f, 8.0f, 8.0f));
}

TEST_F(SpriteSheetTest, ClipsAreFoundByNameAndRefusedByHandle) {
    const SpriteSheetId sheet = quadSheet();
    const uint32_t idle = sprites.addClip(sheet, {.name = "idle", .first = 0, .count = 2, .fps = 4.0f});
    const uint32_t walk = sprites.addClip(sheet, {.name = "walk", .first = 2, .count = 2, .fps = 8.0f});

    EXPECT_EQ(idle, 0u);
    EXPECT_EQ(walk, 1u);
    EXPECT_EQ(sprites.clipCount(sheet), 2u);
    EXPECT_EQ(sprites.findClip(sheet, "walk"), walk);
    // Its own sentinel, from its own domain -- not a frame index and not a character one.
    EXPECT_EQ(sprites.findClip(sheet, "sprint"), SpriteTable::kNoClip);
    EXPECT_EQ(sprites.findClip(SpriteSheetId{}, "idle"), SpriteTable::kNoClip);
    EXPECT_EQ(sprites.addClip(SpriteSheetId{}, {.name = "idle"}), SpriteTable::kNoClip);

    // An unknown sheet or index answers with an empty clip rather than indexing past the
    // end, which is D6's rule applied to a third accessor.
    EXPECT_EQ(sprites.clip(sheet, walk).name, "walk");
    EXPECT_EQ(sprites.clip(sheet, 99).count, 0u);
    EXPECT_EQ(sprites.clip(SpriteSheetId{}, 0).count, 0u);
}

TEST_F(SpriteSheetTest, TheFrameIsFloorOfTimeTimesFps) {
    const SpriteSheetId sheet = quadSheet();
    const uint32_t clip = sprites.addClip(sheet, {.name = "run", .first = 0, .count = 4, .fps = 2.0f});

    // The whole of frame selection: every cell holds for exactly 1/fps, half-open on the
    // right, so a boundary belongs to the cell it starts.
    EXPECT_EQ(sprites.frameAt(sheet, clip, 0.0f), 0u);
    EXPECT_EQ(sprites.frameAt(sheet, clip, 0.499f), 0u);
    EXPECT_EQ(sprites.frameAt(sheet, clip, 0.5f), 1u);
    EXPECT_EQ(sprites.frameAt(sheet, clip, 1.25f), 2u);
    EXPECT_EQ(sprites.frameAt(sheet, clip, 1.75f), 3u);

    // At the duration exactly, `time * fps` is `count` -- one past the end -- and the
    // answer is the last cell. That is where a held `ClampToEnd` playback sits.
    EXPECT_EQ(sprites.frameAt(sheet, clip, 2.0f), 3u);
    EXPECT_EQ(sprites.frameAt(sheet, clip, 900.0f), 3u);

    // Negative clamps to the first rather than converting a negative float to unsigned,
    // which is undefined and would land anywhere at all.
    EXPECT_EQ(sprites.frameAt(sheet, clip, -3.0f), 0u);

    // `first` offsets the answer into the sheet, so a run inside a sheet holding several
    // is a clip rather than a second sheet.
    const uint32_t tail = sprites.addClip(sheet, {.name = "tail", .first = 2, .count = 2, .fps = 2.0f});
    EXPECT_EQ(sprites.frameAt(sheet, tail, 0.0f), 2u);
    EXPECT_EQ(sprites.frameAt(sheet, tail, 0.75f), 3u);
    EXPECT_EQ(sprites.frameAt(sheet, tail, 5.0f), 3u);
}

TEST_F(SpriteSheetTest, ADegenerateClipHoldsItsFirstCell) {
    const SpriteSheetId sheet = quadSheet();
    const uint32_t empty = sprites.addClip(sheet, {.name = "empty", .first = 1, .count = 0, .fps = 12.0f});
    const uint32_t still = sprites.addClip(sheet, {.name = "still", .first = 3, .count = 4, .fps = 0.0f});
    const uint32_t backward = sprites.addClip(sheet, {.name = "backward", .first = 2, .count = 4, .fps = -6.0f});

    // No run to index into and no rate to index at. Each holds rather than dividing by the
    // thing that is zero -- which is how a NaN reaches a UV rect.
    for (const float t : {0.0f, 1.0f, 100.0f}) {
        EXPECT_EQ(sprites.frameAt(sheet, empty, t), 1u);
        EXPECT_EQ(sprites.frameAt(sheet, still, t), 3u);
        EXPECT_EQ(sprites.frameAt(sheet, backward, t), 2u);
    }
}

TEST_F(SpriteSheetTest, PlayWritesTheFirstCellBeforeAnyStepRuns) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    const uint32_t clip = sprites.addClip(sheet, {.name = "run", .first = 1, .count = 3, .fps = 4.0f});
    const SpriteId s = sprites.create(layer, {});

    // Before `play` a sprite has the rect it was created with, which is "the whole image".
    sprites.prepare();
    EXPECT_EQ(sprites.draws()[0].uvRect, glm::vec4(0.0f));
    EXPECT_EQ(sprites.frame(s), SpriteTable::kNoFrame);

    sprites.play(s, sheet, clip);
    // Immediately, not at the next step: a game that creates and plays inside `init` has
    // no step between the call and the first draw.
    EXPECT_EQ(sprites.frame(s), 1u);
    EXPECT_EQ(sprites.draws()[0].uvRect, sprites.frameUv(sheet, 1));
    EXPECT_TRUE(sprites.playing(s));
    EXPECT_EQ(sprites.animatingCount(), 1u);

    // A clip index the sheet does not hold changes nothing at all.
    sprites.play(s, sheet, 7);
    EXPECT_EQ(sprites.frame(s), 1u);
    EXPECT_EQ(sprites.animatingCount(), 1u);
}

TEST_F(SpriteSheetTest, TheCellAdvancesOnTheFixedStepAndWraps) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    const uint32_t clip = sprites.addClip(sheet, {.name = "run", .first = 0, .count = 4, .fps = 2.0f});
    const SpriteId s = sprites.create(layer, {});
    sprites.play(s, sheet, clip);
    sprites.prepare();

    // The readback's arithmetic, run here at the step the engine actually uses: 30 steps
    // of a sixtieth is half a second, which at 2 fps is exactly one cell.
    const float step = 1.0f / 60.0f;
    const auto stepFor = [&](uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) sprites.update(step);
    };

    // Deliberately not landing on a cell boundary. Thirty steps of a sixtieth is half a
    // second *to within a rounding error*, and asserting across an exact boundary would
    // make the test a question about float accumulation rather than about frame selection
    // -- which is the same reason `scripts/readback.sh` picks the rates it does.
    stepFor(29);
    EXPECT_EQ(sprites.frame(s), 0u);
    stepFor(2);
    EXPECT_EQ(sprites.frame(s), 1u);
    EXPECT_EQ(sprites.draws()[0].uvRect, sprites.frameUv(sheet, 1));

    stepFor(60);
    EXPECT_EQ(sprites.frame(s), 3u);

    // Two seconds is the whole clip, so 121 steps in is a lap and a bit -- back at cell 0.
    // The wrap is `advance`'s, unchanged from C7.
    stepFor(30);
    EXPECT_EQ(sprites.frame(s), 0u);
    EXPECT_LT(sprites.clipTime(s), 0.5f);
}

TEST_F(SpriteSheetTest, ClampToEndHoldsTheLastCellAndPausingHoldsWhereverItIs) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    const uint32_t once = sprites.addClip(
        sheet, {.name = "once", .first = 0, .count = 4, .fps = 2.0f, .loop = LoopMode::ClampToEnd});
    const SpriteId s = sprites.create(layer, {});
    sprites.play(s, sheet, once);

    for (uint32_t i = 0; i < 600; ++i) sprites.update(1.0f / 60.0f);
    // Ten seconds on a two-second clip. The last cell, not one past it and not cell 0.
    EXPECT_EQ(sprites.frame(s), 3u);

    // C7's pause, not a second one: the playhead stops and does not forget where it is.
    const SpriteId t = sprites.create(layer, {});
    sprites.play(t, sheet, once);
    for (uint32_t i = 0; i < 40; ++i) sprites.update(1.0f / 60.0f);
    const uint32_t held = sprites.frame(t);
    const float when = sprites.clipTime(t);
    sprites.setPlaying(t, false);
    for (uint32_t i = 0; i < 600; ++i) sprites.update(1.0f / 60.0f);
    EXPECT_EQ(sprites.frame(t), held);
    EXPECT_FLOAT_EQ(sprites.clipTime(t), when);
    EXPECT_FALSE(sprites.playing(t));
}

TEST_F(SpriteSheetTest, EventsAreC7sAndFireOncePerCrossing) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    // Two seconds of clip with a footfall in each half. `AnimationEvent`, in seconds --
    // there is no second event model, which is the row's own constraint.
    const uint32_t clip = sprites.addClip(sheet, {.name = "run",
                                                  .first = 0,
                                                  .count = 4,
                                                  .fps = 2.0f,
                                                  .loop = LoopMode::Loop,
                                                  .events = {{0.5f, "left"}, {1.5f, "right"}}});
    const SpriteId s = sprites.create(layer, {});
    sprites.play(s, sheet, clip);

    std::vector<std::string> heard;
    for (uint32_t i = 0; i < 240; ++i) {
        sprites.update(1.0f / 60.0f);
        for (const FiredSpriteEvent& e : sprites.firedEvents()) {
            EXPECT_EQ(e.sprite, s);
            EXPECT_EQ(e.sheet, sheet);
            heard.push_back(sprites.clip(e.sheet, e.clip).events[e.event].name);
        }
    }

    // Four seconds is two laps of a two-second clip, so each foot lands twice and neither
    // lands three times -- the crossing is half-open, and the wrap does not double-fire.
    EXPECT_EQ(heard, (std::vector<std::string>{"left", "right", "left", "right"}));

    // And the list is cleared by a step that fires nothing, so a game reading it after the
    // step is never handed the previous step's footsteps.
    sprites.stop(s);
    sprites.update(1.0f / 60.0f);
    EXPECT_TRUE(sprites.firedEvents().empty());
}

TEST_F(SpriteSheetTest, StopAndDestroyBothLeaveTheWalk) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    const uint32_t clip = sprites.addClip(sheet, {.name = "run", .first = 0, .count = 4, .fps = 2.0f});

    SpriteId held[4];
    for (SpriteId& s : held) {
        s = sprites.create(layer, {});
        sprites.play(s, sheet, clip);
    }
    EXPECT_EQ(sprites.animatingCount(), 4u);

    // A swap-remove out of the middle, which is where an index that is not repointed goes
    // wrong: the entry that moved has to know its new position or the *next* removal takes
    // the wrong one out.
    sprites.stop(held[1]);
    EXPECT_EQ(sprites.animatingCount(), 3u);
    EXPECT_FALSE(sprites.playing(held[1]));
    EXPECT_EQ(sprites.frame(held[1]), SpriteTable::kNoFrame);

    sprites.destroy(held[2]);
    EXPECT_EQ(sprites.animatingCount(), 2u);

    // The two survivors still advance. Sixty-five steps rather than sixty, for the reason
    // above: sixty of them sum to a hair *under* one second and land on the wrong side of
    // a cell boundary, which is a fact about float addition and not about this table.
    for (uint32_t i = 0; i < 65; ++i) sprites.update(1.0f / 60.0f);
    EXPECT_EQ(sprites.frame(held[0]), 2u);
    EXPECT_EQ(sprites.frame(held[3]), 2u);

    // A reused slot inherits no playback. `held[2]`'s slot comes back on the next create.
    const SpriteId fresh = sprites.create(layer, {});
    EXPECT_EQ(sprites.frame(fresh), SpriteTable::kNoFrame);
    EXPECT_EQ(sprites.animatingCount(), 2u);
}

TEST_F(SpriteSheetTest, DestroyingASheetStopsThePlaybacksReadingIt) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId first = quadSheet();
    const SpriteSheetId second = quadSheet();
    const uint32_t a = sprites.addClip(first, {.name = "run", .first = 0, .count = 4, .fps = 2.0f});
    const uint32_t b = sprites.addClip(second, {.name = "run", .first = 0, .count = 4, .fps = 2.0f});

    const SpriteId onFirst = sprites.create(layer, {});
    const SpriteId onSecond = sprites.create(layer, {});
    sprites.play(onFirst, first, a);
    sprites.play(onSecond, second, b);
    sprites.prepare();
    EXPECT_EQ(sprites.animatingCount(), 2u);

    // Entry 0 is `onFirst`: it was created first, and inside one layer the sort is by
    // creation order.
    const glm::vec4 frozen = sprites.draws()[0].uvRect;
    EXPECT_EQ(frozen, sprites.frameUv(first, sprites.frame(onFirst)));
    sprites.destroySheet(first);

    EXPECT_FALSE(sprites.valid(first));
    EXPECT_EQ(sprites.animatingCount(), 1u);
    EXPECT_TRUE(sprites.playing(onSecond));

    // The sprite keeps the cell it was showing. A frozen frame is a smaller surprise than
    // a rectangle that starts naming whatever the next sheet puts in the slot.
    for (uint32_t i = 0; i < 120; ++i) sprites.update(1.0f / 60.0f);
    EXPECT_EQ(sprites.draws()[0].uvRect, frozen);

    // And the freed slot is reused with a generation that refuses the old handle.
    const SpriteSheetId reused = quadSheet();
    EXPECT_TRUE(sprites.valid(reused));
    EXPECT_FALSE(sprites.valid(first));
    EXPECT_EQ(sprites.clipCount(first), 0u);
}

TEST_F(SpriteSheetTest, AStaticSpriteCanTakeACellWithoutPlayingAnything) {
    // The other half of "atlas slicing": a tile is a cell, and it wants no playback at
    // all. `frameUv` is public for this, and it is what P8 would build a tilemap out of.
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    const SpriteId s = sprites.create(layer, {});

    sprites.setUv(s, sprites.frameUv(sheet, 3));
    sprites.prepare();
    EXPECT_EQ(sprites.draws()[0].uvRect, glm::vec4(32.0f, 24.0f, 32.0f, 24.0f));
    EXPECT_EQ(sprites.animatingCount(), 0u);

    // And an update over a table with nothing playing costs nothing and fires nothing.
    sprites.update(1.0f / 60.0f);
    EXPECT_TRUE(sprites.firedEvents().empty());
    EXPECT_EQ(sprites.draws()[0].uvRect, glm::vec4(32.0f, 24.0f, 32.0f, 24.0f));
}

// ------------------------------------------------------------------------- the revision

/**
 * The upload gate, and the reason it gets a section of its own rather than an assertion
 * bolted onto the tests above.
 *
 * `Renderer::recordSprites` copies `draws()` into mapped memory once per `revision()` per
 * frame in flight. A revision that moves when nothing changed costs a copy nobody needed;
 * a revision that fails to move when something *did* is a sprite that stopped updating on
 * whichever frame slots had already seen the old number -- and that failure is invisible in
 * a still image, because a sprite that stopped moving looks exactly like a sprite that was
 * told not to. So the check is not "moving a sprite bumps it". It is every mutator, one at
 * a time, and every non-mutator asserted the other way.
 *
 * The eight per-sprite setters have a structural guarantee behind them as well: the only
 * way to reach a writable `GpuSprite` is `SpriteTable::at`, which bumps on the way in, so a
 * ninth setter cannot be written that forgets. The five that write `gpu` directly --
 * `create`, `destroy`, `sort`, `applyFrame` and the image reconcile in `prepare` -- have no
 * such funnel, and are exactly the ones enumerated below.
 */
class SpriteRevisionTest : public SpriteTableTest {
  protected:
    /// A live table's revision is never zero, which is what lets a renderer force a
    /// re-upload by zeroing its own copy.
    void SetUp() override {
        SpriteTableTest::SetUp();
        EXPECT_NE(sprites.revision(), 0u);
    }
};

TEST_F(SpriteRevisionTest, EveryPerSpriteSetterBumpsIt) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {});
    const gfx::ImageId image = makeImage("hero.png");
    sprites.prepare();

    // All eight, individually. A test that only moved a sprite would pass for an
    // implementation that bumped in `setPosition` and nowhere else -- which is precisely
    // the shape of the bug this row could introduce, since a sheet frame, a tint and a
    // flip all rewrite the same 64 bytes the pass reads.
    bumps("setPosition", [&] { sprites.setPosition(s, {1.0f, 2.0f}); });
    bumps("setSize", [&] { sprites.setSize(s, {8.0f, 8.0f}); });
    bumps("setPivot", [&] { sprites.setPivot(s, {0.0f, 1.0f}); });
    bumps("setRotation", [&] { sprites.setRotation(s, 0.5f); });
    bumps("setUv", [&] { sprites.setUv(s, {0.0f, 0.0f, 16.0f, 16.0f}); });
    bumps("setTint", [&] { sprites.setTint(s, {1.0f, 0.0f, 0.0f, 1.0f}); });
    bumps("setFlip", [&] { sprites.setFlip(s, true, false); });
    bumps("setImage", [&] { sprites.setImage(s, image); });
}

TEST_F(SpriteRevisionTest, EveryLifetimeChangeBumpsIt) {
    bumps("createLayer + prepare", [&] {
        (void)sprites.createLayer({});
        sprites.prepare();
    });

    const SpriteLayerId layer = sprites.createLayer({});
    sprites.prepare();

    SpriteId a{};
    bumps("create", [&] { a = sprites.create(layer, {}); });
    const SpriteId b = sprites.create(layer, {});
    sprites.prepare();

    // The swap-remove rewrites the entry the dead sprite occupied, so this has to bump
    // even though the sort that follows may leave the array looking the same.
    bumps("destroy", [&] { sprites.destroy(a); });
    sprites.prepare();

    bumps("setLayerOrder + prepare", [&] {
        sprites.setLayerOrder(layer, -5);
        sprites.prepare();
    });

    bumps("destroyLayer", [&] { sprites.destroyLayer(layer); });
    EXPECT_FALSE(sprites.valid(b));
}

TEST_F(SpriteRevisionTest, TheImageReconcileBumpsIt) {
    // `prepare`'s other job. An image loaded or destroyed rewrites `meta.x` for every
    // sprite in the array, and a frame slot that skipped the copy would go on drawing a
    // descriptor slot that now holds something else -- which the golden set cannot see,
    // because no golden scene has a sprite in it.
    const gfx::ImageId image = makeImage("hero.png");
    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {.image = image});
    sprites.prepare();

    bumps("prepare across an image destroy", [&] {
        images.destroy(image);
        sprites.prepare();
    });
    EXPECT_EQ(sprites.draws()[0].meta.x, gfx::ImageTable::kFallbackSlot);
}

TEST_F(SpriteRevisionTest, AStaticTableHoldsItsRevisionAcrossEveryPrepare) {
    // The claim the whole card rests on: a screen that did not change uploads nothing.
    const SpriteLayerId layer = sprites.createLayer({});
    for (int i = 0; i < 64; ++i) (void)sprites.create(layer, {});
    sprites.prepare();

    holds("a hundred steady-state prepares", [&] {
        for (int frame = 0; frame < 100; ++frame) sprites.prepare();
    });

    // And an update over a table with no playback is not a mutation either.
    holds("update with nothing playing", [&] { sprites.update(1.0f / 60.0f); });
}

TEST_F(SpriteRevisionTest, AStaleHandleChangesNothingAndSaysSo) {
    // The other half of the funnel: `at` refuses the handle before it bumps, so a game
    // holding a destroyed id does not force a re-upload of an array it did not touch.
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {});
    sprites.destroy(s);
    sprites.prepare();

    holds("setPosition on a destroyed sprite", [&] { sprites.setPosition(s, {5.0f, 5.0f}); });
    holds("setTint on a destroyed sprite", [&] { sprites.setTint(s, {0.0f, 0.0f, 0.0f, 1.0f}); });
    holds("setImage on a destroyed sprite", [&] { sprites.setImage(s, gfx::ImageId{}); });
}

TEST_F(SpriteRevisionTest, ShutdownBumpsItRatherThanResettingIt) {
    // A table re-`init`ed into a renderer that still holds per-slot revisions from the old
    // one must not be able to match them, so the counter climbs through a shutdown. A reset
    // to one would have the first frame after a re-init skip its copy and draw an empty
    // buffer's worth of sprites.
    const SpriteLayerId layer = sprites.createLayer({});
    (void)sprites.create(layer, {});
    const uint64_t before = sprites.revision();

    sprites.shutdown();
    EXPECT_GT(sprites.revision(), before);
    sprites.init(&images);
    EXPECT_GT(sprites.revision(), before);
}

/// The animated half, where the hazard is sharpest: P5's `applyFrame` deliberately writes
/// only when the *cell* moved, so the bump has to sit inside that guard -- outside it every
/// step of every playback would upload the whole array to write a rectangle already in the
/// buffer, and the four-fifths-of-no-work property P5 measured would be gone.
class SpriteSheetRevisionTest : public SpriteSheetTest {};

TEST_F(SpriteSheetRevisionTest, PlayAndAFrameChangeBumpItAndAHeldCellDoesNot) {
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteSheetId sheet = quadSheet();
    // 4 cells at 4 fps: a quarter of a second a cell, fifteen 60 Hz steps.
    const uint32_t clip = sprites.addClip(sheet, {.name = "spin", .first = 0, .count = 4, .fps = 4.0f});
    const SpriteId s = sprites.create(layer, {});
    sprites.prepare();

    // `play` writes the first cell before any step runs, so it is a mutation.
    bumps("play", [&] { sprites.play(s, sheet, clip); });
    ASSERT_EQ(sprites.frame(s), 0u);

    // Fourteen steps inside cell 0. The rectangle in the buffer is already the right one,
    // so not one of them may bump.
    holds("fourteen steps inside one cell", [&] {
        for (int i = 0; i < 14; ++i) sprites.update(1.0f / 60.0f);
    });
    EXPECT_EQ(sprites.frame(s), 0u);

    // The step that crosses into cell 1 rewrites the rectangle, and must.
    bumps("the step that changes cell", [&] {
        for (int i = 0; i < 2 && sprites.frame(s) == 0u; ++i) sprites.update(1.0f / 60.0f);
    });
    EXPECT_EQ(sprites.frame(s), 1u);
    EXPECT_EQ(sprites.draws()[0].uvRect, sprites.frameUv(sheet, 1));

    // Neither pausing, nor retiming, nor stopping touches a byte the pass reads.
    holds("setPlaying", [&] { sprites.setPlaying(s, false); });
    holds("setSpeed", [&] { sprites.setSpeed(s, 2.0f); });
    holds("update while paused", [&] {
        for (int i = 0; i < 60; ++i) sprites.update(1.0f / 60.0f);
    });
    holds("stop", [&] { sprites.stop(s); });
}

TEST_F(SpriteSheetRevisionTest, SheetBookkeepingIsNotASpriteMutation) {
    // A sheet, a clip and a sheet destroyed all leave `draws()` byte-identical: the sheet
    // is CPU-side slicing, and `destroySheet` leaves every sprite on the cell it had.
    const SpriteLayerId layer = sprites.createLayer({});
    const SpriteId s = sprites.create(layer, {});
    sprites.prepare();

    SpriteSheetId sheet{};
    holds("createSheet", [&] { sheet = quadSheet(); });
    holds("addClip", [&] { (void)sprites.addClip(sheet, {.name = "idle", .first = 0, .count = 4, .fps = 4.0f}); });

    sprites.play(s, sheet, 0);
    const glm::vec4 frozen = sprites.draws()[0].uvRect;
    holds("destroySheet", [&] { sprites.destroySheet(sheet); });
    EXPECT_EQ(sprites.draws()[0].uvRect, frozen);
    EXPECT_EQ(sprites.animatingCount(), 0u);
}
