#include "gfx/ImageTable.h"
#include "gfx/ViewTable.h"
#include "scene/CameraControllers.h"

#include <gtest/gtest.h>

using namespace gfx;

/**
 * @file tests/ViewTableTests.cpp
 * @brief C34's lifetime rules: create, destroy, reuse a slot, and a stale handle refused.
 *
 * The reason `ViewTable` holds no `VkImage` is so that these can exist with no device.
 * What goes wrong on a table of views is not a Vulkan call -- it is a slot handed to two
 * holders at once, or a handle to a destroyed view resolving to whatever took its place
 * and silently steering somebody else's camera. Both are a `std::vector` and an integer.
 *
 * The drawing half is not testable here and is covered by C31 through C33, whose gate is
 * the golden set and the validation layers.
 */
namespace {

/// Enough image slots that nothing here ever fails for the wrong reason; the view table's
/// own capacity is what these tests are about.
constexpr uint32_t kImageSlots = 64;

class ViewTableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        images.init(kImageSlots);
        views.init(2);
    }
    void TearDown() override {
        views.shutdown();
        images.shutdown();
    }

    ImageTable images;
    ViewTable views;
};

TEST_F(ViewTableTest, ADefaultConstructedHandleNamesNothing) {
    const ViewId none;
    EXPECT_FALSE(none.valid());
    EXPECT_FALSE(views.valid(none));
    EXPECT_EQ(views.camera(none), nullptr);
    EXPECT_FALSE(views.image(none).valid());
}

TEST_F(ViewTableTest, ACreatedViewHasACameraAndAnImage) {
    const ViewId v = views.create(images);
    ASSERT_TRUE(v.valid());
    EXPECT_TRUE(views.valid(v));
    EXPECT_EQ(views.liveCount(), 1u);
    EXPECT_NE(views.camera(v), nullptr);
    // The destination is an ordinary image slot, which is the whole point: what a view
    // drew is nameable anywhere a loaded texture is.
    EXPECT_TRUE(views.image(v).valid());
    EXPECT_TRUE(images.valid(views.image(v)));
    EXPECT_TRUE(images.at(images.slot(views.image(v))).external);
}

TEST_F(ViewTableTest, TheCameraIsWrittenThroughTheHandle) {
    const ViewId v = views.create(images);
    ASSERT_TRUE(v.valid());
    views.camera(v)->focus = {1.0f, 2.0f, 3.0f};
    views.camera(v)->distance = 12.0f;
    EXPECT_FLOAT_EQ(views.camera(v)->focus.y, 2.0f);
    // The const accessor sees the same view rather than a copy.
    const ViewTable& constView = views;
    EXPECT_FLOAT_EQ(constView.camera(v)->distance, 12.0f);
}

TEST_F(ViewTableTest, AnInstalledCameraComesBackOutAsItselfAndIsWhatTheViewRecords) {
    const ViewId v = views.create(images);
    ASSERT_TRUE(v.valid());
    views.camera(v)->focus = {9.0f, 9.0f, 9.0f}; // the slot's own, which must be left alone

    scene::FlyCamera fly;
    fly.focus = {1.0f, 2.0f, 3.0f};
    views.setCamera(v, &fly);

    // The pointer, not a copy of the base half. `entry.camera = fly` would have compiled,
    // sliced off `FlyCamera` and left a view driven by a camera that does nothing.
    EXPECT_EQ(views.camera(v), &fly);
    EXPECT_EQ(dynamic_cast<scene::FlyCamera*>(views.camera(v)), &fly);
    // What the renderer reads is the installed one's pose, not the slot's.
    EXPECT_FLOAT_EQ(views.at(v.index).active().focus.x, 1.0f);

    // And handing it back exposes the slot's own again, untouched by any of that.
    views.setCamera(v, nullptr);
    EXPECT_FLOAT_EQ(views.at(v.index).active().focus.x, 9.0f);
    EXPECT_EQ(views.camera(v), &views.at(v.index).camera);
}

TEST_F(ViewTableTest, EachSlotHasItsOwnDefaultCameraRatherThanASharedOne) {
    const ViewId a = views.create(images);
    const ViewId b = views.create(images);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());

    // One shared null camera would make this move both views, which is the failure the
    // by-value slot exists to make impossible.
    views.camera(a)->focus = {4.0f, 0.0f, 0.0f};
    EXPECT_FLOAT_EQ(views.camera(b)->focus.x, 0.0f);
}

TEST_F(ViewTableTest, AReusedSlotDoesNotKeepThePreviousOwnersCamera) {
    const ViewId first = views.create(images);
    scene::FlyCamera fly;
    views.setCamera(first, &fly);
    views.destroy(first, images);

    // The camera the destroyed view was driven by may well have gone with it, so the new
    // holder of the slot must not inherit the pointer.
    const ViewId second = views.create(images);
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(views.camera(second), &views.at(second.index).camera);
    EXPECT_FALSE(views.at(second.index).installed != nullptr);
}

TEST_F(ViewTableTest, DestroyingAViewReleasesItsImageSlot) {
    const ViewId v = views.create(images);
    const ImageId img = views.image(v);
    ASSERT_TRUE(images.valid(img));

    views.destroy(v, images);
    EXPECT_EQ(views.liveCount(), 0u);
    // The image goes with it. A destination left live would hold a descriptor slot for a
    // view that no longer renders into it.
    EXPECT_FALSE(images.valid(img));
}

TEST_F(ViewTableTest, AStaleHandleIsRefusedRatherThanResolved) {
    const ViewId first = views.create(images);
    views.camera(first)->focus = {5.0f, 0.0f, 0.0f};
    views.destroy(first, images);

    EXPECT_FALSE(views.valid(first));
    EXPECT_EQ(views.camera(first), nullptr);
    EXPECT_FALSE(views.image(first).valid());
    // And a second destroy is a no-op rather than a second release of the same slot.
    views.destroy(first, images);
    EXPECT_EQ(views.liveCount(), 0u);
}

TEST_F(ViewTableTest, AReusedSlotIsADifferentView) {
    const ViewId first = views.create(images);
    views.destroy(first, images);
    const ViewId second = views.create(images);

    ASSERT_TRUE(second.valid());
    // The same slot, which is the point of the free list...
    EXPECT_EQ(first.index, second.index);
    // ...and a different generation, which is what stops the handle the caller still
    // holds from steering the new view's camera.
    EXPECT_NE(first.generation, second.generation);
    EXPECT_FALSE(views.valid(first));
    EXPECT_TRUE(views.valid(second));
    EXPECT_EQ(views.slotCount(), 1u);
}

TEST_F(ViewTableTest, TheTableRefusesPastItsCapacity) {
    const ViewId a = views.create(images);
    const ViewId b = views.create(images);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());

    // A stated refusal, not a silent truncation and not a grown array: the cap is a count
    // of uniform blocks per frame slot, so exceeding it would mean two views writing one
    // block -- which is the bug C32 exists to have made impossible.
    const ViewId over = views.create(images);
    EXPECT_FALSE(over.valid());
    EXPECT_EQ(views.liveCount(), 2u);

    // And the refusal leaves nothing behind: destroying one makes room for exactly one.
    views.destroy(a, images);
    const ViewId c = views.create(images);
    EXPECT_TRUE(c.valid());
    EXPECT_EQ(views.liveCount(), 2u);
}

TEST_F(ViewTableTest, TheRevisionMovesOnEveryLifetimeChange) {
    const uint64_t start = views.revision();
    const ViewId v = views.create(images);
    const uint64_t afterCreate = views.revision();
    EXPECT_GT(afterCreate, start);

    // Writing a camera is not a lifetime change and must not move it: the renderer
    // reconciles residency against this, and a revision that moved every frame would have
    // it destroy and rebuild a destination image every frame.
    views.camera(v)->focus = {0.0f, 1.0f, 0.0f};
    EXPECT_EQ(views.revision(), afterCreate);

    views.destroy(v, images);
    EXPECT_GT(views.revision(), afterCreate);
}

TEST_F(ViewTableTest, AnOutOfRangeSlotYieldsADeadEntry) {
    EXPECT_FALSE(views.at(0).live);
    EXPECT_FALSE(views.at(9999).live);
    EXPECT_EQ(views.mutableAt(9999), nullptr);
}

// --------------------------------------------------------------- per-view extent

TEST_F(ViewTableTest, AViewKeepsTheExtentItAskedForAndZeroMeansFollow) {
    const ViewId sized = views.create(images, {480, 270});
    const ViewId following = views.create(images);
    ASSERT_TRUE(sized.valid());
    ASSERT_TRUE(following.valid());

    EXPECT_EQ(views.extent(sized), glm::uvec2(480u, 270u));
    // Zero is kept as zero rather than resolved here. The table has no window and no
    // swapchain to resolve it against, which is the whole reason it is the renderer that
    // turns this into pixels.
    EXPECT_EQ(views.extent(following), glm::uvec2(0u, 0u));
    EXPECT_EQ(views.at(sized.index).extent, glm::uvec2(480u, 270u));
}

TEST_F(ViewTableTest, AnExtentWithOneSideZeroFollowsRatherThanBeingOnePixelTall) {
    // Half a rule is not a rule: a caller that named a width and left the height at its
    // default gets the follow behaviour, not a 640x1 target set.
    const ViewId v = views.create(images, {640, 0});
    ASSERT_TRUE(v.valid());
    EXPECT_EQ(views.extent(v), glm::uvec2(0u, 0u));
}

TEST_F(ViewTableTest, ResizeMovesTheRevisionOnlyWhenTheSizeMoves) {
    const ViewId v = views.create(images, {480, 270});
    const uint64_t afterCreate = views.revision();

    // The renderer waits on the device and rebuilds seventeen targets off this revision,
    // so a game assigning the same size every frame must not move it.
    views.resize(v, {480, 270});
    EXPECT_EQ(views.revision(), afterCreate);

    views.resize(v, {960, 540});
    EXPECT_GT(views.revision(), afterCreate);
    EXPECT_EQ(views.extent(v), glm::uvec2(960u, 540u));

    // And back to following the presenting view, which is a change like any other.
    const uint64_t afterResize = views.revision();
    views.resize(v, {0, 0});
    EXPECT_GT(views.revision(), afterResize);
    EXPECT_EQ(views.extent(v), glm::uvec2(0u, 0u));
}

TEST_F(ViewTableTest, AStaleHandleCannotResizeTheViewThatTookItsSlot) {
    const ViewId first = views.create(images, {480, 270});
    views.destroy(first, images);
    const ViewId second = views.create(images, {320, 180});
    ASSERT_EQ(first.index, second.index);

    const uint64_t before = views.revision();
    views.resize(first, {1920, 1080});
    EXPECT_EQ(views.revision(), before);
    // A reacquired slot carries the new holder's extent, not the previous one's -- the
    // renderer sizes a target set from this, so inheriting it is 233 MiB of the wrong shape.
    EXPECT_EQ(views.extent(second), glm::uvec2(320u, 180u));
    EXPECT_EQ(views.extent(first), glm::uvec2(0u, 0u));
}

/// The cap `Engine::init` gives the table is `kMaxViews - 1`, which C38 moved from 1 to 3.
/// Its own fixture rather than the shared one, because what is under test is the table at a
/// capacity it did not have before.
TEST(ViewTableCapacity, GenerationsAndSlotsSurviveARaisedCap) {
    ImageTable images;
    ViewTable views;
    images.init(kImageSlots);
    views.init(3);

    ViewId ids[3];
    for (uint32_t i = 0; i < 3; ++i) {
        ids[i] = views.create(images, {320u * (i + 1), 180u * (i + 1)});
        ASSERT_TRUE(ids[i].valid()) << "slot " << i;
        EXPECT_EQ(ids[i].index, i) << "slots are handed out in order, so block i+1 is view i";
    }
    EXPECT_EQ(views.liveCount(), 3u);
    EXPECT_FALSE(views.create(images).valid());

    // The middle one, so the free list is exercised rather than the top of the array.
    views.destroy(ids[1], images);
    const ViewId reused = views.create(images, {64, 64});
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(reused.index, 1u);
    EXPECT_NE(reused.generation, ids[1].generation);
    EXPECT_FALSE(views.valid(ids[1]));

    // The neighbours are untouched by any of it, including their extents -- which is what
    // says the renderer's per-slot target sets cannot be crossed by a lifetime change.
    EXPECT_EQ(views.extent(ids[0]), glm::uvec2(320u, 180u));
    EXPECT_EQ(views.extent(ids[2]), glm::uvec2(960u, 540u));
    EXPECT_EQ(views.extent(reused), glm::uvec2(64u, 64u));

    views.shutdown();
    images.shutdown();
}

} // namespace
