#include "gfx/ImageTable.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace gfx;

namespace fs = std::filesystem;

/**
 * @file tests/ImageTableTests.cpp
 * @brief P1's lifetime rules: acquire, release, reuse, and a released handle refused.
 *
 * The reason the table holds no `VkImage` is so that these can exist. `limitations.md`
 * recorded the identical free list inside `GltfScene` as untested for exactly as long as
 * it was inseparable from a device, and the thing that goes wrong on a free list without
 * a generation is not a Vulkan call -- it is a slot handed to two holders at once, which
 * is a `std::vector` and an integer and needs no GPU to prove.
 *
 * `load` resolves through `core::Resources`, so every test writes a real file. A name
 * without the `res:/` scheme is passed through as an ordinary path, which is what lets
 * these point at `temp_directory_path()` instead of at an asset tree that is generated
 * and gitignored.
 */
class ImageTableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto unique = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root = fs::temp_directory_path() / ("substrate_images_" + std::string(unique));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override { fs::remove_all(root); }

    /// The bytes are never read here -- decoding is the renderer's half. What `load`
    /// asks of a file is that it exists.
    std::string file(std::string_view name) {
        const fs::path p = root / name;
        std::ofstream out(p, std::ios::binary);
        out << "not really a png";
        return p.string();
    }

    fs::path root;
};

TEST_F(ImageTableTest, AFreshTableHandsOutSlotsPastTheFallback) {
    ImageTable table;
    table.init(8);

    // Slot zero exists and is never handed out: the renderer writes the font atlas there,
    // and it is what every free descriptor and every refused handle resolves to.
    EXPECT_EQ(table.slotCount(), 1u);
    EXPECT_EQ(table.liveCount(), 0u);

    const ImageId a = table.load(file("a.png"));
    const ImageId b = table.load(file("b.png"));

    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_NE(table.slot(a), ImageTable::kFallbackSlot);
    EXPECT_NE(table.slot(b), ImageTable::kFallbackSlot);
    EXPECT_NE(table.slot(a), table.slot(b));
    EXPECT_EQ(table.liveCount(), 2u);
    EXPECT_EQ(table.slotCount(), 3u);
}

TEST_F(ImageTableTest, AReleasedSlotIsReusedAndTheOldHandleIsRefused) {
    ImageTable table;
    table.init(8);

    const ImageId first = table.load(file("first.png"));
    const uint32_t slot = table.slot(first);
    ASSERT_TRUE(table.valid(first));

    table.destroy(first);
    EXPECT_FALSE(table.valid(first));
    EXPECT_EQ(table.liveCount(), 0u);

    const ImageId second = table.load(file("second.png"));

    // The whole point of the generation, in three lines: the slot came back, the new
    // handle names it, and the old handle does not -- so a caller holding `first` draws
    // the atlas rather than whatever `second` loaded into the slot it used to own.
    EXPECT_EQ(table.slot(second), slot);
    EXPECT_TRUE(table.valid(second));
    EXPECT_FALSE(table.valid(first));
    EXPECT_EQ(table.slot(first), ImageTable::kFallbackSlot);
    EXPECT_NE(first.generation, second.generation);

    // And the table did not grow to serve the second load.
    EXPECT_EQ(table.slotCount(), 2u);
}

TEST_F(ImageTableTest, DestroyingTwiceIsANoOpRatherThanADoubleFree) {
    ImageTable table;
    table.init(8);

    const ImageId id = table.load(file("once.png"));
    table.destroy(id);
    table.destroy(id);
    table.destroy(ImageId{});

    EXPECT_EQ(table.liveCount(), 0u);

    // If the second destroy had pushed the slot again, this would take it twice.
    const ImageId a = table.load(file("a.png"));
    const ImageId b = table.load(file("b.png"));
    EXPECT_NE(table.slot(a), table.slot(b));
}

TEST_F(ImageTableTest, AZeroedHandleIsInvalidAndResolvesToTheFallback) {
    ImageTable table;
    table.init(8);
    const ImageId loaded = table.load(file("real.png"));
    ASSERT_TRUE(loaded.valid());

    // The failure `Handle`'s reserved generation exists for: a handle out of memset or
    // out of a default-constructed aggregate names slot zero under an index sentinel,
    // and slot zero is the first one every table hands out.
    const ImageId zeroed{};
    EXPECT_FALSE(zeroed.valid());
    EXPECT_FALSE(table.valid(zeroed));
    EXPECT_EQ(table.slot(zeroed), ImageTable::kFallbackSlot);

    // As is a handle to a slot that exists, carrying a generation nothing ever issued.
    EXPECT_FALSE(table.valid(ImageId{table.slot(loaded), 999u}));
}

TEST_F(ImageTableTest, ANameThatResolvesToNothingFailsAtTheCallSite) {
    ImageTable table;
    table.init(8);

    const ImageId missing = table.load((root / "never_written.png").string());

    EXPECT_FALSE(missing.valid());
    EXPECT_FALSE(table.valid(missing));
    EXPECT_EQ(table.slot(missing), ImageTable::kFallbackSlot);
    // A refused load takes no slot, which is what makes an asset a game never shipped
    // cost nothing rather than leak one per attempt.
    EXPECT_EQ(table.slotCount(), 1u);
    EXPECT_EQ(table.liveCount(), 0u);
}

TEST_F(ImageTableTest, TheCapacityIsStatedAndRefusesPastItself) {
    ImageTable table;
    // Three slots: the fallback and two images.
    table.init(3);

    const ImageId a = table.load(file("a.png"));
    const ImageId b = table.load(file("b.png"));
    const ImageId c = table.load(file("c.png"));

    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_FALSE(c.valid()) << "past the capacity is refused, not silently truncated";
    EXPECT_EQ(table.capacity(), 3u);
    EXPECT_EQ(table.liveCount(), 2u);

    // And the refusal is not permanent: giving one back makes room for the next.
    table.destroy(a);
    const ImageId d = table.load(file("d.png"));
    EXPECT_TRUE(d.valid());
    EXPECT_EQ(table.slot(d), table.slot(b) == 1u ? 2u : 1u);
}

TEST_F(ImageTableTest, EveryMutationMovesTheRevisionAndNothingElseDoes) {
    ImageTable table;
    table.init(8);

    const uint64_t afterInit = table.revision();
    const ImageId id = table.load(file("a.png"));
    const uint64_t afterLoad = table.revision();
    EXPECT_GT(afterLoad, afterInit);

    // What the renderer reconciles against. A frame that read the same number twice and
    // uploaded anyway would wait for the device once per frame, forever.
    EXPECT_TRUE(table.valid(id));
    EXPECT_EQ(table.slot(id), 1u);
    EXPECT_EQ(table.revision(), afterLoad) << "reading the table is not a mutation";

    table.destroy(id);
    EXPECT_GT(table.revision(), afterLoad);

    const uint64_t afterDestroy = table.revision();
    table.destroy(id);
    EXPECT_EQ(table.revision(), afterDestroy) << "a destroy that did nothing is not a mutation";
}

TEST_F(ImageTableTest, TheDeviceHalfSeesWhatItNeedsToReconcile) {
    ImageTable table;
    table.init(8);

    const std::string path = file("named.png");
    const ImageId id = table.load(path);
    const uint32_t slot = table.slot(id);

    const ImageTable::Entry& e = table.at(slot);
    EXPECT_TRUE(e.live);
    EXPECT_EQ(e.generation, id.generation);
    // Resolved once, at load, so the renderer opens a path rather than re-running the
    // search from whatever its working directory happens to be.
    EXPECT_EQ(e.path, path);
    EXPECT_EQ(e.name, path);

    table.destroy(id);
    EXPECT_FALSE(table.at(slot).live);
    EXPECT_NE(table.at(slot).generation, id.generation);

    // Out of range is a dead entry rather than an overrun, so a renderer walking to a
    // capacity the table has not reached yet reads something defined.
    EXPECT_FALSE(table.at(9999u).live);
}

TEST_F(ImageTableTest, ShutdownGivesEverythingBackAndInitStartsOver) {
    ImageTable table;
    table.init(8);
    const ImageId id = table.load(file("a.png"));
    ASSERT_TRUE(table.valid(id));

    table.shutdown();
    EXPECT_FALSE(table.valid(id));
    EXPECT_EQ(table.slotCount(), 0u);
    EXPECT_EQ(table.liveCount(), 0u);

    table.init(4);
    EXPECT_EQ(table.slotCount(), 1u);
    EXPECT_EQ(table.capacity(), 4u);
    // A handle from the previous table does not validate against this one, which matters
    // because the renderer's `setImages` treats a new table as a new slot space.
    EXPECT_FALSE(table.valid(id));
}
