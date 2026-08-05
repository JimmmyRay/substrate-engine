#pragma once

#include "core/Handle.h"
#include "gfx/ImageTable.h"
#include "scene/Animation.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace scene {

/// One sprite, addressed by slot and generation.
using SpriteId = core::Handle<struct SpriteTag>;
/// One layer. A distinct type from `SpriteId` so that `destroy(layer)` and
/// `destroy(sprite)` -- different verbs on different arrays -- cannot be confused.
using SpriteLayerId = core::Handle<struct SpriteLayerTag>;
/// One sprite sheet: an image, a cell size, and the clips cut out of it.
using SpriteSheetId = core::Handle<struct SpriteSheetTag>;

/**
 * @file engine/scene/SpriteTable.h
 * @brief Layers, sprites and the order they draw in -- the CPU half of the sprite pass.
 *
 * **No Vulkan here.** The file is in `SUBSTRATE_HOSTED_SOURCES`, which is what lets the
 * unit suite prove the arithmetic -- a slot handed to two holders, a sort that is not a
 * total order, a pivot applied on the wrong side of a rotation -- with no device.
 * `Renderer` binds `draws()` and issues one `vkCmdDraw`.
 *
 * `LoopMode`, `ClipPlayback`, `AnimationEvent` and `advance`/`crossedEvents` come from
 * `Animation.h` and mean what they mean for a skeleton. Timing runs off the fixed step via
 * `Engine::simulate`, so a paused game has paused sprites and a time scale slows them.
 *
 * See systems.md, "Sprites", for what a sheet deliberately cannot express.
 */

/// Which sprites draw first.
struct SpriteLayerDesc {
    /// Lower draws first, so a background is negative. Ties inside one layer break by
    /// creation order, which is what makes the sort a **total** order and so reproducible
    /// run to run -- the property the golden suite needs.
    int32_t order = 0;
};

/**
 * @brief Everything about one sprite that a game states rather than computes.
 *
 * **The UV rect is in texels, not normalised coordinates.** The division happens in the
 * fragment shader against `textureSize` of the resident image, so an atlas re-exported at
 * a different size needs no number in the game changed and there is no CPU-side copy of an
 * image dimension to fall out of step with the file.
 */
struct SpriteDesc {
    /// From `Engine::images()`. A handle this table cannot resolve draws the font atlas,
    /// which is `ImageTable`'s stated fallback rather than undefined data.
    gfx::ImageId image;
    /// Texels: x, y, width, height. A zero width or height means the whole image.
    glm::vec4 uv{0.0f, 0.0f, 0.0f, 0.0f};
    /// World units. At `orthoHeight` equal to the virtual resolution's height that is one
    /// texel per unit, which is what makes a sprite pixel-exact.
    glm::vec2 size{1.0f, 1.0f};
    /// The point `position` names, as a fraction of `size`, measured from the image's
    /// top-left. `{0.5, 1.0}` is a character's feet.
    glm::vec2 pivot{0.5f, 0.5f};
    /// Where the pivot is, on the z = 0 plane in world space.
    glm::vec2 position{0.0f, 0.0f};
    /// Straight (non-premultiplied) sRGB, multiplied into the texel. White leaves the
    /// image alone, and white is exact -- which is what the readback check depends on.
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    /// Radians, anticlockwise about the pivot.
    float rotation = 0.0f;
    /// Mirror the image about the middle of its UV rect -- the *rect*, not the pivot, so a
    /// character with its pivot at its feet turns without the anchor moving.
    bool flipX = false;
    bool flipY = false;
};

/// `meta.z` of `GpuSprite`.
inline constexpr uint32_t kSpriteFlipX = 1u;
inline constexpr uint32_t kSpriteFlipY = 2u;

/**
 * @brief One sprite as `sprite.vert` reads it. std430, and 64 bytes on purpose: ten
 *        thousand is 640 KB.
 *
 * That is what the packing buys -- an RGBA8 tint, and a rotation stored as a cosine and a
 * sine written once per change rather than a `sin`/`cos` per vertex per frame.
 */
struct GpuSprite {
    /// xy: world position of the pivot. zw: size in world units.
    glm::vec4 posSize{0.0f, 0.0f, 1.0f, 1.0f};
    /// xy: cosine and sine of the rotation. zw: pivot, as a fraction of size.
    glm::vec4 rotPivot{1.0f, 0.0f, 0.5f, 0.5f};
    /// Texels: x, y, width, height. A zero width or height means the whole image.
    glm::vec4 uvRect{0.0f, 0.0f, 0.0f, 0.0f};
    /// x: descriptor slot in the image array. y: tint, packed RGBA8. z: flags. w: unused.
    glm::uvec4 meta{gfx::ImageTable::kFallbackSlot, 0xFFFFFFFFu, 0u, 0u};
};

static_assert(sizeof(GpuSprite) == 64, "sprite.vert reads this layout; keep it 64 bytes");

/**
 * @brief How an image is cut into equal cells. Texels, and a regular grid.
 *
 * Frames are numbered left to right then top to bottom, so frame *n* is column
 * `n % columns`, row `n / columns`.
 */
struct SpriteSheetDesc {
    /// Cell size in texels. **Zero in either axis is refused** -- `createSheet` returns an
    /// invalid handle -- because a zero UV rect already means *the whole image* to the
    /// shader, so a silently-accepted zero draws every cell as the whole file and looks
    /// like a shader bug.
    glm::uvec2 frame{0, 0};
    /// Cells per row. Zero is read as one.
    uint32_t columns = 1;
    /// Cells in the sheet, over all rows. Zero is refused for the reason above.
    uint32_t count = 0;
    /// Texels from the image's top-left to the first cell's, for a sheet with a margin.
    glm::uvec2 origin{0, 0};
    /// Texels between adjacent cells, for a sheet exported with gutters.
    glm::uvec2 spacing{0, 0};
};

/// A named run of cells, played at a rate. The duration is `count / fps`; storing it would
/// be a second copy to keep in step with a retimed clip.
struct SpriteClip {
    /// What `findClip` matches. The engine never interprets it.
    std::string name;
    /// The sheet frame this clip's frame 0 is. A run inside a sheet holding several.
    uint32_t first = 0;
    /// How many cells the run is. Zero holds `first` forever rather than dividing by it.
    uint32_t count = 1;
    /// Cells per second. Non-positive holds `first`, for the same reason.
    float fps = 12.0f;
    /// `Loop` wraps, `ClampToEnd` holds the last cell.
    LoopMode loop = LoopMode::Loop;
    /// Instants a game may react to, **in ascending time order** -- the contract
    /// `crossedEvents` is written against. `time` is seconds from the start of the clip
    /// rather than a frame index (cell *f* begins at `f / fps`), so an event keeps its
    /// place in the motion when the clip is retimed.
    std::vector<AnimationEvent> events;
};

/// One event a sprite's playback crossed this step, reported by `firedEvents()`. A list
/// read after the update rather than a callback, which would be the engine calling into a
/// game mid-update.
struct FiredSpriteEvent {
    SpriteId sprite;
    SpriteSheetId sheet;
    /// Index into the sheet's clips; `SpriteTable::clip(sheet, clip)` resolves it.
    uint32_t clip = 0;
    /// Index into `clip(sheet, clip).events`.
    uint32_t event = 0;
};

class SpriteTable {
  public:
    /// "No such clip", returned by `findClip`.
    static constexpr uint32_t kNoClip = 0xFFFFFFFFu;
    /// "This sprite is not playing anything", returned by `frame`.
    static constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

    /// @param images the table sprites take their textures from. Held by pointer and not
    ///        owned; `Engine` owns both and outlives both.
    void init(const gfx::ImageTable* images);
    void shutdown();

    [[nodiscard]] SpriteLayerId createLayer(const SpriteLayerDesc& desc);

    /// Destroy the layer **and every sprite in it**. A sprite whose layer is gone has no
    /// place in the order, and leaving it drawing at the freed layer's old key is the
    /// aliasing generations exist to prevent one level up.
    void destroyLayer(SpriteLayerId id);

    [[nodiscard]] bool valid(SpriteLayerId id) const;

    /// Change where the layer sits in the order. Re-sorts, so it is rare by construction --
    /// which is what makes sorting on the CPU affordable.
    void setLayerOrder(SpriteLayerId id, int32_t order);

    [[nodiscard]] uint32_t layerCount() const { return liveLayers; }

    /// @return an invalid handle if `layer` names no live layer. Nothing aborts: a sprite
    ///         that did not appear is a smaller problem than a game that did not run.
    [[nodiscard]] SpriteId create(SpriteLayerId layer, const SpriteDesc& desc);

    /// The generation moves immediately, so the handle the caller holds goes stale on the
    /// call it made and a second `destroy` is a no-op rather than a double free.
    void destroy(SpriteId id);

    [[nodiscard]] bool valid(SpriteId id) const;

    /// Where the pivot is, on the z = 0 plane in world space.
    void setPosition(SpriteId id, const glm::vec2& position);
    void setSize(SpriteId id, const glm::vec2& size);
    void setPivot(SpriteId id, const glm::vec2& pivot);
    /// Radians, anticlockwise about the pivot.
    void setRotation(SpriteId id, float radians);
    /// Texels: x, y, width, height. Zero width or height means the whole image.
    void setUv(SpriteId id, const glm::vec4& uv);
    void setTint(SpriteId id, const glm::vec4& tint);
    void setFlip(SpriteId id, bool flipX, bool flipY);
    void setImage(SpriteId id, gfx::ImageId image);

    /// Live sprites, across every layer.
    [[nodiscard]] uint32_t count() const { return liveSprites; }

    /// @return an invalid handle if the cell size or the frame count is zero.
    [[nodiscard]] SpriteSheetId createSheet(const SpriteSheetDesc& desc);

    /// Destroy the sheet **and stop every playback reading it**. A sprite keeps the cell it
    /// was showing; a frozen frame is a smaller surprise than a rectangle that starts
    /// naming whatever the next sheet puts in the slot.
    void destroySheet(SpriteSheetId id);

    [[nodiscard]] bool valid(SpriteSheetId id) const;

    [[nodiscard]] uint32_t sheetCount() const { return liveSheets; }

    /// @return the clip's index in the sheet, or `kNoClip` if the sheet is not live.
    ///         Clips are append-only within a sheet: an index, once handed out, names the
    ///         same clip for as long as the sheet does, which is what lets a playback hold
    ///         one rather than a handle.
    uint32_t addClip(SpriteSheetId sheet, SpriteClip clip);

    /// Index of the clip named `name` in `sheet`, or `kNoClip`. Called once at load.
    [[nodiscard]] uint32_t findClip(SpriteSheetId sheet, const std::string& name) const;

    /// Bounds-checked: an unknown sheet or index yields an empty clip rather than indexing
    /// past the end.
    [[nodiscard]] const SpriteClip& clip(SpriteSheetId sheet, uint32_t index) const;

    [[nodiscard]] uint32_t clipCount(SpriteSheetId sheet) const;

    /// The texel rectangle of sheet frame `frame`, in `SpriteDesc::uv`'s terms. A frame
    /// past the end clamps to the last one; zero for a sheet that is not live.
    [[nodiscard]] glm::vec4 frameUv(SpriteSheetId sheet, uint32_t frame) const;

    /**
     * @brief Which **sheet** frame `clip` shows at `time` seconds into it.
     *
     * `first + min(floor(time * fps), count - 1)`. The `min` is what keeps a `ClampToEnd`
     * clip on its last cell rather than one past it, since `advance` clamps the time to the
     * duration and the duration is exactly where `time * fps` reaches `count`. A time
     * outside the clip clamps rather than wrapping: wrapping is `advance`'s job, and doing
     * it here too would hide a playback that was never advanced.
     */
    [[nodiscard]] uint32_t frameAt(SpriteSheetId sheet, uint32_t clip, float time) const;

    /// Start `clip` of `sheet` on `sprite` from its beginning, **writing the first cell's
    /// rectangle immediately** -- so a sprite plays from the frame it was told to on the
    /// frame it was told, without a step having to run first. A second `play` replaces
    /// whatever was running; there is no blend.
    void play(SpriteId sprite, SpriteSheetId sheet, uint32_t clip, float speed = 1.0f);

    /// Stop advancing and leave the cell where it is. The rectangle stays one a game may
    /// overwrite with `setUv`.
    void stop(SpriteId sprite);

    /// Hold or resume without forgetting where the playhead is.
    void setPlaying(SpriteId sprite, bool playing);
    /// Negative plays the clip backwards, which `advance` and `crossedEvents` both handle.
    void setSpeed(SpriteId sprite, float speed);

    [[nodiscard]] bool playing(SpriteId sprite) const;
    /// The sheet frame on screen, or `kNoFrame` for a sprite that is not playing.
    [[nodiscard]] uint32_t frame(SpriteId sprite) const;
    /// Seconds into the clip; zero for a sprite with no playback.
    [[nodiscard]] float clipTime(SpriteId sprite) const;
    /// Sprites with a live playback -- what `update` walks.
    [[nodiscard]] uint32_t animatingCount() const { return static_cast<uint32_t>(animated.size()); }

    /**
     * @brief Advance every playback by `dt` and write the cells that changed.
     *
     * `dt` is the fixed step, from `Engine::simulate`, so a paused game has paused sprites
     * and a time scale slows them. Only a sprite whose *frame index* moved has its `uvRect`
     * written, which makes a thousand sprites at 12 fps on a 60 Hz step four fifths of no
     * work.
     */
    void update(float dt);

    /// This step's crossings, in the order they were found. **Cleared by every `update`,
    /// including one that fires nothing** -- a game reading this after the step must not be
    /// handed the previous step's list.
    [[nodiscard]] const std::vector<FiredSpriteEvent>& firedEvents() const { return fired; }

    /**
     * @brief Put the array in draw order and resolve every image handle to a slot.
     *
     * Called once per frame by `Engine`, before `Renderer::drawFrame`, and it does nothing
     * on a frame where nothing changed shape. **A position is not part of the sort key**,
     * so ten thousand sprites moving every frame re-sort nothing: the sort runs when a
     * sprite is created, destroyed or moved between layers, and the slot resolve runs when
     * `ImageTable::revision()` moves.
     */
    void prepare();

    /// The sprites to draw, in order, back to front. Contiguous and ready to memcpy --
    /// there is no gather step between this and the mapped buffer.
    [[nodiscard]] const std::vector<GpuSprite>& draws() const { return gpu; }

    /// How many times `prepare()` has actually sorted. What a test asserts against to prove
    /// that moving a sprite does not re-sort.
    [[nodiscard]] uint64_t sortCount() const { return sorts; }

    /**
     * @brief Bumped by every mutation of `draws()`, and by nothing else.
     *
     * The renderer remembers, per frame in flight, which revision that slot's buffer last
     * received, so a screen of sprites that did not change costs no copy at all.
     *
     * **The whole array or none of it.** A dirty range would have to be a range per frame
     * in flight -- each slot last uploaded at a different revision, so each needs the union
     * of every range since -- buying a scatter of small writes into write-combined memory
     * where there was one linear one.
     *
     * Zero is never reported by a live table, so a renderer forcing a re-upload sets its
     * own copy to zero.
     */
    [[nodiscard]] uint64_t revision() const { return rev; }

  private:
    /// `Record::animIndex` for a sprite with no playback.
    static constexpr uint32_t kNotAnimating = 0xFFFFFFFFu;

    /// What a `SpriteId` resolves to. Indexed by handle slot and never reordered, which is
    /// what makes a handle stable while `gpu` is permuted underneath it.
    struct Record {
        /// Into `gpu` and `slotOf`, and moved by every sort.
        uint32_t index = 0;
        /// Bumped by `destroy`, never zero.
        uint32_t generation = 0;
        /// The layer's own handle slot.
        uint32_t layer = 0;
        /// Creation order, and the sort's tiebreak. Monotonic across the table's life, so
        /// two sprites in one layer keep the order they were created in.
        uint32_t sequence = 0;
        gfx::ImageId image;
        bool live = false;

        /// Where this slot sits in `animated`, or `kNotAnimating`. An index rather than a
        /// flag so `stop` and `destroy` are a swap-remove rather than a scan.
        uint32_t animIndex = kNotAnimating;
        SpriteSheetId sheet;
        /// `clip` indexes the sheet's clips.
        ClipPlayback playback;
        /// The sheet frame the `uvRect` currently holds, so `update` writes only when it
        /// moves. `kNoFrame` forces the next write.
        uint32_t frame = kNoFrame;
    };

    struct Layer {
        int32_t order = 0;
        uint32_t generation = 0;
        bool live = false;
    };

    struct Sheet {
        SpriteSheetDesc desc;
        std::vector<SpriteClip> clips;
        uint32_t generation = 0;
        bool live = false;
    };

    /**
     * @brief The one place a `SpriteId` becomes a writable entry, so a stale handle is
     *        refused once **and the revision cannot be forgotten**.
     *
     * Reaching a mutable `GpuSprite` any other way makes the bump something a setter has to
     * remember; a missed one is a sprite that quietly stops updating on one frame slot in
     * three, which looks exactly like a sprite that was told not to move.
     *
     * **It bumps on the way in**, before the caller has written anything, so it cannot
     * double as an accessor for a reader.
     */
    [[nodiscard]] GpuSprite* at(SpriteId id);

    void sort();

    /// Take a slot out of `animated`, if it is in it. Swap-remove, so the entry that moved
    /// has its record repointed -- the invariant `animIndex` exists to keep.
    void detach(uint32_t slot);

    /// Resolve the playback's frame and write the rectangle if it moved.
    void applyFrame(uint32_t slot);

    const gfx::ImageTable* images = nullptr;

    std::vector<Layer> layers;
    std::vector<uint32_t> freeLayers;
    uint32_t liveLayers = 0;

    std::vector<Sheet> sheets;
    std::vector<uint32_t> freeSheets;
    uint32_t liveSheets = 0;

    /// Slots with a live playback. Dense, in no particular order, and walked by `update`.
    std::vector<uint32_t> animated;
    /// Members rather than locals, so a step that fires nothing allocates nothing.
    std::vector<FiredSpriteEvent> fired;
    std::vector<uint32_t> crossings;

    std::vector<Record> records;
    std::vector<uint32_t> freeSprites;
    uint32_t liveSprites = 0;
    uint32_t nextSequence = 1;

    /// Dense, in draw order after `prepare()`, and the buffer the renderer copies.
    std::vector<GpuSprite> gpu;
    /// Parallel to `gpu`: which record owns entry n. What a sort has to keep in step.
    std::vector<uint32_t> slotOf;
    /// Scratch for the permutation, kept so a sort allocates nothing after the first.
    std::vector<uint32_t> order;
    std::vector<GpuSprite> gpuScratch;
    std::vector<uint32_t> slotScratch;

    bool sortDirty = false;
    uint64_t sorts = 0;
    /// What `revision()` reports. Starts at one and only ever climbs -- `shutdown()` bumps
    /// it rather than resetting it, so a table re-`init`ed into the same renderer cannot
    /// hand a frame slot a revision it has already seen.
    uint64_t rev = 1;
    /// `ImageTable::revision()` the slots in `gpu` were resolved against. Zero is "never",
    /// which no live table reports.
    uint64_t imageRevision = 0;
};

} // namespace scene
