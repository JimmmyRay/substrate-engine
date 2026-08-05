#pragma once

#include "core/Handle.h"
#include "gfx/ImageTable.h"
#include "scene/Camera.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gfx {

/// One view the frame renders and does not present, addressed by slot and generation.
/// A distinct type from every other handle in the engine, so a view passed where an image
/// or a body belongs is a compile error. See `core/Handle.h` for why generation zero is
/// reserved.
using ViewId = core::Handle<struct ViewTag>;

/**
 * @file engine/gfx/ViewTable.h
 * @brief The views a game asked for: a camera each, a handle each, and no Vulkan.
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
 * A view carries a whole render-target set at its own extent -- roughly 224 MiB at
 * 1600x900 and 4x MSAA, with time scaling the same way -- so the extent argument is the
 * one number that decides what one costs. See
 * [rendering.md](../../docs/architecture/rendering.md), "More than one view".
 */
class ViewTable {
  public:
    /// How many views can exist at once. One less than the renderer's `kMaxViews`: the
    /// presenting view holds the first uniform block and is not in this table.
    void init(uint32_t capacity);
    void shutdown();

    /// What the renderer reconciles a slot against.
    struct Entry {
        /// This slot's own pose, per entry rather than one shared default so writing
        /// `focus` through one view's handle cannot move every other one.
        scene::Camera camera;
        /// A camera the game owns, driving this view instead of the slot's own. Non-owning
        /// and null until `setCamera`.
        scene::Camera* installed = nullptr;
        /// The image slot this view's destination is bound into. Invalid until `create`
        /// succeeds.
        ImageId image;
        /// Bumped by `destroy`, never zero. What makes a reacquired slot a different view
        /// rather than a silent alias onto the one the caller still holds.
        uint32_t generation = 0;
        bool live = false;

        /// Pixels this view renders at, or `{0, 0}` to follow the presenting view's.
        /// *Either* component zero means follow, so `{640, 0}` is not half a rule.
        glm::uvec2 extent{0, 0};

        /// What this view renders through. **The renderer reads this and never `camera`**:
        /// `Camera` has a vtable, so `entry.camera = someFlyCamera;` compiles as base
        /// copy-assignment and drops the derived half with no diagnostic. Installing a
        /// pointer is what has no sliced form.
        [[nodiscard]] const scene::Camera& active() const {
            return installed != nullptr ? *installed : camera;
        }
    };

    /**
     * @brief Take a slot and return its handle.
     *
     * @param images the table the destination is registered in.
     * @param extent pixels to render at, or `{0, 0}` to follow the presenting view. **Ask
     *        for the size the result will be sampled at, not the window's**: the whole
     *        target set is this size, so a 480x270 inset costs a sixteenth of a 1920x1080
     *        one rather than the same and a downscale.
     * @return an invalid handle when the table is full; it logs and does not abort.
     */
    [[nodiscard]] ViewId create(ImageTable& images, glm::uvec2 extent = {0, 0});

    /**
     * @brief Render this view at a different size from the next frame on.
     *
     * Moves the revision, so the renderer rebuilds a whole target set: a device wait and
     * seventeen allocations, not something to call per frame. `{0, 0}` goes back to
     * following the presenting view. A handle this table does not recognise is ignored.
     */
    void resize(ViewId id, glm::uvec2 extent);

    /// What `create` or `resize` was given, **not** the pixels it resolved to: `{0, 0}`
    /// stays `{0, 0}` here and means "the presenting view's". Zero for a stale handle.
    [[nodiscard]] glm::uvec2 extent(ViewId id) const;

    /**
     * @brief Give the slot back.
     *
     * The generation moves immediately, so a second `destroy` is a no-op rather than a
     * double free. The renderer releases the destination image when it next reconciles.
     */
    void destroy(ViewId id, ImageTable& images);

    /// Was this handle issued by this table, and is the view behind it still there?
    [[nodiscard]] bool valid(ViewId id) const;

    /// The camera to write -- the installed one if there is one, else this slot's own --
    /// or null for a handle this table does not recognise.
    [[nodiscard]] scene::Camera* camera(ViewId id);
    [[nodiscard]] const scene::Camera* camera(ViewId id) const;

    /**
     * @brief Drive this view with a camera the game owns; `nullptr` goes back to the
     *        slot's own pose.
     *
     * **Non-owning, and the table never deletes.** A game destroying an installed camera
     * calls this with `nullptr` first, or the renderer reads a dangling pointer.
     *
     * **The engine calls neither `activate` nor `update` on a view camera.** There is one
     * `InputMap`, and two cameras declaring `Camera.Forward` is the collision
     * `activate`/`deactivate` exists to prevent; a game driving a view camera from input
     * updates it itself in `frameUpdate`.
     */
    void setCamera(ViewId id, scene::Camera* c);

    /// What this view drew, as an image any material, sprite or UI quad can name. An
    /// invalid `ImageId` for a handle this table does not recognise.
    [[nodiscard]] ImageId image(ViewId id) const;

    /// How many slots exist. Only grows: a live handle names a slot forever.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(entries.size()); }
    [[nodiscard]] uint32_t liveCount() const { return liveSlots; }
    [[nodiscard]] uint32_t capacity() const { return slotCapacity; }

    /// Bumped by every `create` and every `destroy`; the renderer does its residency work
    /// only when this moves.
    [[nodiscard]] uint64_t revision() const { return rev; }

    /// What the device half reconciles slot `s` against. Out of range yields a dead entry
    /// rather than indexing off the end.
    [[nodiscard]] const Entry& at(uint32_t s) const;
    [[nodiscard]] Entry* mutableAt(uint32_t s);

  private:
    std::vector<Entry> entries;
    std::vector<uint32_t> freeSlots;
    uint32_t slotCapacity = 0;
    uint32_t liveSlots = 0;
    uint64_t rev = 0;
};

} // namespace gfx
