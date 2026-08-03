#pragma once

#include "core/Handle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

/// One image the engine holds resident, addressed by slot and generation (P1).
///
/// A distinct type from every other handle in the engine, so an image passed where a
/// body or an instance belongs is a compile error rather than a silent index into the
/// wrong subsystem's array. See `core/Handle.h` for why generation zero is reserved.
using ImageId = core::Handle<struct ImageTag>;

/**
 * @file engine/gfx/ImageTable.h
 * @brief The engine's resident images: a growable array, a free list, a generation (P1).
 *
 * `Renderer::loadImage` used to be sixteen slots with no unload, and said in its own
 * words that the moment a game streamed UI art was the moment to revisit it. This is
 * that revisit. `kMaxOverlayImages` is deleted rather than raised -- a constant that is
 * raised when someone complains is a silent limit with a changelog -- and what replaces
 * it is the device's own bound on how many sampled images one descriptor set can hold.
 *
 * ```cpp
 * void MyGame::init(Engine& e) {
 *     hero = e.images().load("res:/sprites/hero.png");
 * }
 * void MyGame::shutdown(Engine& e) {
 *     e.images().destroy(hero);
 * }
 * ```
 *
 * ## Why this holds no `VkImage`
 *
 * The same split `SceneData` draws against `GltfScene`, at a much smaller scale: the
 * *lifetime* of an image -- which slot it occupies, which generation that slot is on,
 * and which slots are free -- is bookkeeping a device cannot answer questions about, and
 * `Renderer` already owns the descriptor array, the sampler and the upload path. So this
 * is the CPU half and the renderer is the device half, exactly as `InstanceTable` is the
 * CPU half of the instance buffers the renderer grows for it.
 *
 * That split is not decoration. It is what makes every rule below testable without a
 * device, which matters because the *unit* being got wrong here is not a Vulkan call: it
 * is a slot handed out twice, and `limitations.md` recorded the identical free list in
 * `GltfScene` as untested for exactly as long as it was inseparable from a `VkDevice`.
 *
 * The renderer reconciles against `revision()`. A `load` is therefore resident on the
 * frame after the call rather than inside it -- which is the same "synchronous, at load
 * time" the row asked for from a game's point of view, since a game loads in `init` and
 * nothing has drawn yet.
 *
 * ## What it deliberately is not
 *
 * [CLAUDE.md](../../CLAUDE.md) refuses "a `ResourceManager` / `TextureCache` owning GPU
 * resources behind handles" by name, and this is close enough that the difference is
 * written down rather than assumed. It is a `std::vector` of entries, a free list and a
 * generation counter. It must not grow:
 *
 * - **Reference counting.** `destroy` is explicit. A refcount is what replaces an
 *   explicit destroy with an implicit one.
 * - **A path-keyed cache.** Loading the same file twice loads it twice. Deduplication is
 *   a cache, a cache needs invalidation, and invalidation is the system this is not.
 * - **A virtual `IImageLoader`.** One function, `stbi_load`, in the device half.
 * - **Eviction, residency policy, or an async queue.** Streaming is C10 and stays there.
 *
 * If a later change adds any of the four it has become the refused thing, and the row
 * that adds it owes a new argument.
 */
class ImageTable {
  public:
    /**
     * @brief The slot a handle that names nothing resolves to.
     *
     * Slot zero is not an image and is never handed out: the renderer writes the font
     * atlas there, so a stale, destroyed or never-issued handle draws the atlas rather
     * than reading a descriptor that was never written. That is the property C5 chose
     * over `PARTIALLY_BOUND` and it is preserved here unchanged -- a wrong index is
     * visible and harmless instead of undefined.
     */
    static constexpr uint32_t kFallbackSlot = 0;

    /// What the renderer reconciles a slot against. Public because the device half is a
    /// different class and needs all three; there is nothing here a caller could corrupt
    /// by reading it.
    struct Entry {
        /// Resolved by `core::Resources` at `load`, so the device half opens a path
        /// rather than re-running the search from whatever its working directory is.
        std::string path;
        /// The `res:/` name or path the caller asked for, for logs.
        std::string name;
        /// Bumped by `destroy`, never zero. What makes a reacquired slot a different
        /// image rather than a silent alias onto the one the caller still holds.
        uint32_t generation = 0;
        bool live = false;
        /// **The device half supplies this image instead of decoding `path`.** A render
        /// view's destination is the one thing that takes this route (C34): it is drawn
        /// rather than loaded, but it is an image in every way that matters to a caller,
        /// so it lives in the same array and is named by the same `ImageId`. `syncImages`
        /// neither loads nor frees a slot carrying this — whoever adopted it owns it.
        bool external = false;
    };

    /// @param capacity how many slots the device half can address, slot zero included.
    ///        `gfx::Renderer::maxImageSlots()` is where that number comes from, and it is
    ///        a device limit rather than an engine constant.
    void init(uint32_t capacity);
    void shutdown();

    /**
     * @brief Take a slot for `name` and return its handle.
     *
     * `name` is a `res:/` asset name or a path, resolved exactly as a scene's textures
     * are. The file is *found* here and decoded by the renderer, so a name nothing
     * resolves to fails at the call site -- which is the failure a game actually hits,
     * an asset that was never fetched -- and a file that resolves but is not an image
     * reports at the next frame and leaves the slot drawing the fallback.
     *
     * @return an invalid handle when the name resolves to nothing or the table is full.
     *         Both say so in the log; neither aborts, because a missing logo is not worth
     *         taking a game down for.
     */
    [[nodiscard]] ImageId load(const std::string& name);

    /**
     * @brief Take a slot whose image the device half supplies, rather than one loaded
     *        from a file.
     *
     * What a render view's destination is registered through, so that what one view drew
     * can be sampled by a sprite, a UI quad or a material exactly as a PNG can — one kind
     * of texture handle rather than two. The caller owns the image; this owns the slot.
     *
     * @return an invalid handle when the table is full. Same failure as `load` and the
     *         same reason it does not abort.
     */
    [[nodiscard]] ImageId adopt(const std::string& name);

    /**
     * @brief Give the slot back.
     *
     * The generation moves immediately, so the handle the caller is holding goes stale on
     * the call it made, and a second `destroy` of the same handle is a no-op rather than
     * a double free. The renderer releases the `VkImage` the next time it reconciles.
     */
    void destroy(ImageId id);

    /// Was this handle issued by this table, and is the image behind it still there?
    [[nodiscard]] bool valid(ImageId id) const;

    /// The descriptor index to put in `ui::DrawVertex::texture`, or `kFallbackSlot` for a
    /// handle this table does not recognise. The one place a stale handle is refused.
    [[nodiscard]] uint32_t slot(ImageId id) const;

    /// How many slots exist, slot zero included. The renderer sizes its descriptor array
    /// from this; it only grows, because a live handle names a slot forever.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(entries.size()); }
    /// How many of them hold an image.
    [[nodiscard]] uint32_t liveCount() const { return liveSlots; }
    /// What `init` was given. A `load` past it is refused and says so.
    [[nodiscard]] uint32_t capacity() const { return slotCapacity; }

    /// Bumped by every `load` and every `destroy`, exactly as `InstanceTable::revision`
    /// is: what the renderer compares against to know whether it has any work to do.
    [[nodiscard]] uint64_t revision() const { return rev; }

    /// What the device half reconciles slot `s` against. Out of range yields a dead
    /// entry rather than indexing off the end.
    [[nodiscard]] const Entry& at(uint32_t s) const;

  private:
    std::vector<Entry> entries;
    std::vector<uint32_t> freeSlots;
    uint32_t slotCapacity = 0;
    uint32_t liveSlots = 0;
    uint64_t rev = 0;
};

} // namespace gfx
