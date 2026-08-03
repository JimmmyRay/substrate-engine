#pragma once

#include "core/Handle.h"
#include "gfx/ImageTable.h"
#include "scene/Camera.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gfx {

/// One view the frame renders and does not present, addressed by slot and generation.
///
/// A distinct type from every other handle in the engine, so a view passed where an
/// image or a body belongs is a compile error rather than a silent index into the wrong
/// subsystem's array. See `core/Handle.h` for why generation zero is reserved.
using ViewId = core::Handle<struct ViewTag>;

/**
 * @file engine/gfx/ViewTable.h
 * @brief The views a game asked for: a camera each, a handle each, and no Vulkan (C34).
 *
 * ```cpp
 * void MyGame::init(Engine& e) {
 *     mirror = e.views().create(e.images(), {480, 270});   // a quarter of 1920x1080
 *     e.views().camera(mirror)->distance = 4.0f;
 *     mirrorArt = e.views().image(mirror);        // an ImageId, usable anywhere one is
 * }
 * void MyGame::update(Engine& e, float) {
 *     e.views().camera(mirror)->focus = playerPos;
 * }
 * ```
 *
 * ## Why this holds no `VkImage`
 *
 * The same split `ImageTable` draws against `Renderer`, for the same reason: the
 * *lifetime* of a view -- which slot it occupies, which generation that slot is on, which
 * slots are free -- is bookkeeping a device cannot answer questions about, while the
 * seventeen render targets, the destination image and the descriptor it lands in are the
 * renderer's. The renderer reconciles against `revision()`, exactly as it does for images.
 *
 * That split is what makes the rules below testable with no device, and the unit being got
 * wrong here is not a Vulkan call: it is a slot handed out twice, or a stale handle
 * resolving to whatever took its place.
 *
 * ## The camera is written, not pushed
 *
 * `camera(id)` hands back a pointer the game writes between frames, resolved at the same
 * point in the frame the main camera is. Not a callback: a callback would run at a point
 * in the renderer a game has no way to reason about, and every other per-frame quantity a
 * game controls here is a value it assigns.
 *
 * ## What a view costs, so nothing creates one casually
 *
 * A full pass chain **and a full set of render targets, at the extent this view asked for**
 * -- roughly 224 MiB at 1600x900 and 4x MSAA, a quarter of that at half the side. Time
 * scales the same way, which is why the extent argument exists: four quarter-size views
 * cost about what one full-size one does, in both. See
 * [rendering.md](../../docs/architecture/rendering.md), "More than one view", for those
 * numbers and for what a secondary view still shares with the primary rather than owning:
 * the light ranking and the shadow atlas.
 *
 * ## What it deliberately is not
 *
 * The same four refusals `ImageTable` carries, and for the same reasons: no reference
 * counting, no cache keyed on anything, no virtual view type, no eviction policy. A view
 * is created and destroyed explicitly or it is not managed at all.
 */
class ViewTable {
  public:
    /// How many views can exist at once. One less than the renderer's `kMaxViews`,
    /// because the presenting view holds the first uniform block and is not in this table
    /// -- a game does not create the view it is already looking through.
    void init(uint32_t capacity);
    void shutdown();

    /// What the renderer reconciles a slot against.
    struct Entry {
        /// This slot's own pose, written by the game through `camera(id)` and read by the
        /// renderer once per frame. A view whose camera nothing ever writes renders the
        /// identity, which is a black frame rather than a silent copy of the main one.
        ///
        /// **Per entry rather than one shared default**, because writing `focus` through
        /// one uninstalled view's handle would otherwise move every other one.
        scene::Camera camera;
        /// A camera the game owns, driving this view instead of the slot's own. Non-owning
        /// and null until `setCamera`.
        scene::Camera* installed = nullptr;
        /// The image slot this view's destination is bound into, so what it drew can be
        /// sampled anywhere an image can. Invalid until `create` succeeds.
        ImageId image;
        /// Bumped by `destroy`, never zero. What makes a reacquired slot a different view
        /// rather than a silent alias onto the one the caller still holds.
        uint32_t generation = 0;
        bool live = false;

        /// Pixels this view renders at, or `{0, 0}` to follow the presenting view's --
        /// which is what a resize then moves it with. **A whole target set is sized from
        /// this**, so it is the one number that decides what the view costs; see the class
        /// block. Either component zero means "follow", so `{640, 0}` is not half a rule.
        glm::uvec2 extent{0, 0};

        /// What this view renders through. **The renderer reads this and never `camera`**:
        /// `Camera` has a vtable, so `entry.camera = someFlyCamera;` compiles as base
        /// copy-assignment and drops the derived half, leaving a camera that does nothing
        /// with no diagnostic. Installing a pointer is what has no sliced form.
        [[nodiscard]] const scene::Camera& active() const {
            return installed != nullptr ? *installed : camera;
        }
    };

    /**
     * @brief Take a slot and return its handle.
     *
     * @param images the table the destination is registered in, so what the view draws is
     *        addressable as an ordinary image. Adopting the slot here rather than in the
     *        renderer is what keeps a view's image the *same kind of thing* as a loaded
     *        one, instead of a second way to name a texture.
     * @param extent pixels to render at, or `{0, 0}` to follow the presenting view. **Ask
     *        for the size the result will be sampled at, not the window's**: the whole
     *        target set is this size, so a 480x270 inset costs a sixteenth of what a
     *        1920x1080 one does rather than the same and a downscale.
     * @return an invalid handle when the table is full, which says so in the log and does
     *         not abort -- a missing mirror is not worth taking a game down for.
     */
    [[nodiscard]] ViewId create(ImageTable& images, glm::uvec2 extent = {0, 0});

    /**
     * @brief Render this view at a different size from the next frame on.
     *
     * Moves the revision, because the renderer rebuilds a target set from it -- which is a
     * device wait and seventeen allocations, so this belongs to a resolution setting
     * changing rather than to anything per frame. `{0, 0}` goes back to following the
     * presenting view. A handle this table does not recognise is ignored.
     */
    void resize(ViewId id, glm::uvec2 extent);

    /// What `create` or `resize` was given, **not** the pixels it resolved to: `{0, 0}`
    /// stays `{0, 0}` here and means "the presenting view's". Zero for a stale handle.
    [[nodiscard]] glm::uvec2 extent(ViewId id) const;

    /**
     * @brief Give the slot back.
     *
     * The generation moves immediately, so the handle the caller holds goes stale on the
     * call it made and a second `destroy` is a no-op rather than a double free. The
     * renderer releases the destination image the next time it reconciles.
     */
    void destroy(ViewId id, ImageTable& images);

    /// Was this handle issued by this table, and is the view behind it still there?
    [[nodiscard]] bool valid(ViewId id) const;

    /// The camera to write -- the installed one if there is one, else this slot's own --
    /// or null for a handle this table does not recognise. The one place a stale handle is
    /// refused on the way to a mutation.
    [[nodiscard]] scene::Camera* camera(ViewId id);
    [[nodiscard]] const scene::Camera* camera(ViewId id) const;

    /**
     * @brief Drive this view with a camera the game owns; `nullptr` goes back to the
     *        slot's own pose.
     *
     * **Non-owning, and the table never deletes.** A game destroying an installed camera
     * calls this with `nullptr` first.
     *
     * **The engine calls neither `activate` nor `update` on a view camera.** There is one
     * `InputMap`, and two cameras declaring `Camera.Forward` is exactly the collision
     * `activate`/`deactivate` exists to prevent. A game that wants a view camera to read
     * input updates it in `frameUpdate` with whatever map it likes -- which is also the
     * only route a second player's input could take.
     */
    void setCamera(ViewId id, scene::Camera* c);

    /// What this view drew, as an image any material, sprite or UI quad can name. An
    /// invalid `ImageId` for a handle this table does not recognise.
    [[nodiscard]] ImageId image(ViewId id) const;

    /// How many slots exist. Only grows: a live handle names a slot forever.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(entries.size()); }
    /// How many of them hold a view.
    [[nodiscard]] uint32_t liveCount() const { return liveSlots; }
    /// What `init` was given.
    [[nodiscard]] uint32_t capacity() const { return slotCapacity; }

    /// Bumped by every `create` and every `destroy`. What the renderer compares against
    /// to know whether it has any residency work to do.
    [[nodiscard]] uint64_t revision() const { return rev; }

    /// What the device half reconciles slot `s` against. Out of range yields a dead entry
    /// rather than indexing off the end.
    [[nodiscard]] const Entry& at(uint32_t s) const;
    /// The mutable form, for the renderer to read the camera without copying it.
    [[nodiscard]] Entry* mutableAt(uint32_t s);

  private:
    std::vector<Entry> entries;
    std::vector<uint32_t> freeSlots;
    uint32_t slotCapacity = 0;
    uint32_t liveSlots = 0;
    uint64_t rev = 0;
};

} // namespace gfx
