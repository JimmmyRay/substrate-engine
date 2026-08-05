#pragma once

#include "core/Handle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

/// One image the engine holds resident, addressed by slot and generation.
/// A distinct type from every other handle in the engine, so an image passed where a body
/// or an instance belongs is a compile error. See `core/Handle.h` for why generation zero
/// is reserved.
using ImageId = core::Handle<struct ImageTag>;

/**
 * @file engine/gfx/ImageTable.h
 * @brief The engine's resident images: a growable array, a free list, a generation.
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
 * The renderer reconciles against `revision()`, so a `load` becomes resident on the frame
 * after the call rather than inside it.
 *
 * [CLAUDE.md](../../CLAUDE.md) refuses "a `ResourceManager` / `TextureCache` owning GPU
 * resources behind handles" by name, and this is close enough that adding reference
 * counting, a path-keyed cache, a virtual loader or an eviction policy makes it the
 * refused thing.
 */
class ImageTable {
  public:
    /**
     * @brief The slot a handle that names nothing resolves to.
     *
     * Slot zero is never handed out: the renderer writes the font atlas there, so a stale,
     * destroyed or never-issued handle draws the atlas rather than sampling a descriptor
     * that was never written -- which is undefined, not merely wrong.
     */
    static constexpr uint32_t kFallbackSlot = 0;

    /// What the renderer reconciles a slot against.
    struct Entry {
        /// Resolved by `core::Resources` at `load`, so the device half opens a path rather
        /// than re-running the search from whatever its working directory is.
        std::string path;
        /// The `res:/` name or path the caller asked for, for logs.
        std::string name;
        /// Bumped by `destroy`, never zero. What makes a reacquired slot a different
        /// image rather than a silent alias onto the one the caller still holds.
        uint32_t generation = 0;
        bool live = false;
        /// The device half supplies this image instead of decoding `path`; `syncImages`
        /// neither loads nor frees a slot carrying it, so whoever adopted it owns it.
        bool external = false;
    };

    /// @param capacity how many slots the device half can address, slot zero included.
    ///        `gfx::Renderer::maxImageSlots()` is a device limit, not an engine constant.
    void init(uint32_t capacity);
    void shutdown();

    /**
     * @brief Take a slot for `name` and return its handle.
     *
     * `name` is a `res:/` asset name or a path. The file is *found* here and decoded by
     * the renderer, so a name that resolves to nothing fails at the call site while a file
     * that resolves but is not an image only reports at the next frame, leaving the slot
     * drawing the fallback.
     *
     * @return an invalid handle when the name resolves to nothing or the table is full.
     *         Both log; neither aborts.
     */
    [[nodiscard]] ImageId load(const std::string& name);

    /**
     * @brief Take a slot whose image the device half supplies, rather than one loaded
     *        from a file.
     *
     * The caller owns the image; this owns the slot. Freeing the image without destroying
     * the handle leaves a live slot pointing at nothing.
     *
     * @return an invalid handle when the table is full.
     */
    [[nodiscard]] ImageId adopt(const std::string& name);

    /**
     * @brief Give the slot back.
     *
     * The generation moves immediately, so a second `destroy` of the same handle is a
     * no-op rather than a double free. The renderer releases the `VkImage` the next time
     * it reconciles.
     */
    void destroy(ImageId id);

    /// Was this handle issued by this table, and is the image behind it still there?
    [[nodiscard]] bool valid(ImageId id) const;

    /// The descriptor index to put in `ui::DrawVertex::texture`, or `kFallbackSlot` for a
    /// handle this table does not recognise.
    [[nodiscard]] uint32_t slot(ImageId id) const;

    /// How many slots exist, slot zero included. The renderer sizes its descriptor array
    /// from this; it only grows, because a live handle names a slot forever.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(entries.size()); }
    [[nodiscard]] uint32_t liveCount() const { return liveSlots; }
    [[nodiscard]] uint32_t capacity() const { return slotCapacity; }

    /// Bumped by every `load` and every `destroy`; the renderer does its residency work
    /// only when this moves.
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
