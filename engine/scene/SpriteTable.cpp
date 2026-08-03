#include "scene/SpriteTable.h"

#include "core/Profiler.h"

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

/// RGBA8, in the order `unpackUnorm4x8` reads it. The same packing `OverlayVertex` uses
/// for the same reason: a tint is a colour a person picked, eight bits per channel is
/// what a colour picker offers, and white round-trips exactly -- which is the property the
/// readback check is built on.
uint32_t packTint(const glm::vec4& c) {
    const auto q = [](float v) {
        const float clamped = std::min(std::max(v, 0.0f), 1.0f);
        return static_cast<uint32_t>(std::lround(clamped * 255.0f));
    };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}

/// Layer order first, creation order second, as one unsigned key so the comparison is a
/// single 64-bit test. The XOR flips the sign bit, which is what maps a signed order onto
/// an unsigned that sorts the same way -- without it a background at -10 would draw last.
uint64_t sortKey(int32_t layerOrder, uint32_t sequence) {
    const auto biased = static_cast<uint32_t>(layerOrder) ^ 0x80000000u;
    return (static_cast<uint64_t>(biased) << 32) | sequence;
}

/// How long `c` runs, in seconds. Derived rather than stored: a clip retimed by changing
/// `fps` would otherwise carry a duration that no longer agrees with it, which is the
/// two-copies bug this table already refuses for image dimensions.
float clipDuration(const SpriteClip& c) {
    if (c.count == 0 || !(c.fps > 0.0f)) return 0.0f;
    return static_cast<float>(c.count) / c.fps;
}

/**
 * @brief The frame-selection arithmetic, and the one line in P5 that can be wrong quietly.
 *
 * `first + min(floor(time * fps), count - 1)`, with every degenerate input answered rather
 * than assumed away, because this is reachable from a public method:
 *
 * - A zero `count` or a non-positive `fps` holds `first`. There is no run to index into
 *   and no rate to index at, and dividing by either is how a NaN reaches a UV rect.
 * - A negative or NaN `time` is `first`. `advance` never produces one -- it wraps or
 *   clamps into `[0, duration]` -- but `frameAt` is public and a caller may.
 * - A `time` at or past the duration is the **last** cell. That is not a defensive clamp:
 *   `time * fps` reaches exactly `count` at the duration, which is where a `ClampToEnd`
 *   playback sits for as long as it is held, and `count` is one past the end.
 *
 * The `float` to `uint32_t` conversion is guarded rather than trusted: converting a value
 * outside the destination's range is undefined, and `count` is the bound that makes it
 * defined here.
 */
uint32_t clipFrame(const SpriteClip& c, float time) {
    if (c.count == 0 || !(c.fps > 0.0f)) return c.first;
    const float f = std::floor(time * c.fps);
    if (!(f > 0.0f)) return c.first;
    if (f >= static_cast<float>(c.count)) return c.first + c.count - 1;
    return c.first + static_cast<uint32_t>(f);
}

/// The clip a bad handle or a bad index resolves to. Empty, so every accessor above it
/// answers with "nothing plays" rather than indexing past the end (D6).
const SpriteClip& noClip() {
    static const SpriteClip empty{.name = {}, .first = 0, .count = 0, .fps = 0.0f, .loop = LoopMode::Loop, .events = {}};
    return empty;
}

} // namespace

void SpriteTable::init(const gfx::ImageTable* table) {
    images = table;
    imageRevision = 0;
}

void SpriteTable::shutdown() {
    layers.clear();
    freeLayers.clear();
    liveLayers = 0;
    sheets.clear();
    freeSheets.clear();
    liveSheets = 0;
    animated.clear();
    fired.clear();
    crossings.clear();
    records.clear();
    freeSprites.clear();
    liveSprites = 0;
    nextSequence = 1;
    gpu.clear();
    slotOf.clear();
    order.clear();
    gpuScratch.clear();
    slotScratch.clear();
    sortDirty = false;
    imageRevision = 0;
    images = nullptr;
    // Emptying `gpu` is a mutation of what the renderer copies, and the counter climbs
    // through a shutdown rather than resetting: a table re-`init`ed into a renderer that
    // still holds per-slot revisions from the old one must not be able to match them.
    ++rev;
}

// ------------------------------------------------------------------------- layers

SpriteLayerId SpriteTable::createLayer(const SpriteLayerDesc& desc) {
    uint32_t slot = 0;
    if (!freeLayers.empty()) {
        slot = freeLayers.back();
        freeLayers.pop_back();
    } else {
        slot = static_cast<uint32_t>(layers.size());
        layers.emplace_back();
    }

    Layer& layer = layers[slot];
    layer.order = desc.order;
    layer.live = true;
    // Generation zero is "never issued", so a fresh slot starts at one -- see Handle.h for
    // why that is the reservation rather than an index sentinel.
    if (layer.generation == 0) layer.generation = 1;
    ++liveLayers;

    // A new layer changes nothing until something is in it, but a *reused* slot may be
    // reached by sprites created before this call is over, and the key it contributes has
    // changed. One flag either way.
    sortDirty = true;
    return SpriteLayerId{slot, layer.generation};
}

void SpriteTable::destroyLayer(SpriteLayerId id) {
    if (!valid(id)) return;

    // Its sprites go with it, and they go through `destroy` rather than being torn out
    // here: that is what moves each one's generation, so a handle a game is still holding
    // reports staleness instead of naming a slot the next sprite will take.
    for (uint32_t s = 0; s < records.size(); ++s) {
        if (records[s].live && records[s].layer == id.index) destroy(SpriteId{s, records[s].generation});
    }

    Layer& layer = layers[id.index];
    layer.live = false;
    ++layer.generation;
    if (layer.generation == 0) layer.generation = 1;
    freeLayers.push_back(id.index);
    --liveLayers;
    sortDirty = true;
}

bool SpriteTable::valid(SpriteLayerId id) const {
    return id.valid() && id.index < layers.size() && layers[id.index].live &&
           layers[id.index].generation == id.generation;
}

void SpriteTable::setLayerOrder(SpriteLayerId id, int32_t order_) {
    if (!valid(id)) return;
    if (layers[id.index].order == order_) return;
    layers[id.index].order = order_;
    sortDirty = true;
}

// ------------------------------------------------------------------------ sprites

SpriteId SpriteTable::create(SpriteLayerId layer, const SpriteDesc& desc) {
    if (!valid(layer)) return {};

    uint32_t slot = 0;
    if (!freeSprites.empty()) {
        slot = freeSprites.back();
        freeSprites.pop_back();
    } else {
        slot = static_cast<uint32_t>(records.size());
        records.emplace_back();
    }

    Record& r = records[slot];
    r.live = true;
    r.layer = layer.index;
    r.sequence = nextSequence++;
    r.image = desc.image;
    r.index = static_cast<uint32_t>(gpu.size());
    if (r.generation == 0) r.generation = 1;

    // A reused slot inherits nothing from whoever held it. `destroy` already detached it
    // from `animated`, so these are the fields that would otherwise still name the dead
    // sprite's sheet and frame -- and `frame` in particular has to be `kNoFrame` or the
    // first `play` would decide the cell had not changed and write no rectangle.
    r.animIndex = kNotAnimating;
    r.sheet = {};
    r.playback = {};
    r.frame = kNoFrame;

    GpuSprite entry;
    entry.posSize = glm::vec4(desc.position, desc.size);
    entry.rotPivot = glm::vec4(std::cos(desc.rotation), std::sin(desc.rotation), desc.pivot);
    entry.uvRect = desc.uv;
    entry.meta = glm::uvec4(images != nullptr ? images->slot(desc.image) : gfx::ImageTable::kFallbackSlot,
                            packTint(desc.tint),
                            (desc.flipX ? kSpriteFlipX : 0u) | (desc.flipY ? kSpriteFlipY : 0u), 0u);

    gpu.push_back(entry);
    slotOf.push_back(slot);
    ++liveSprites;
    sortDirty = true;
    ++rev;

    return SpriteId{slot, r.generation};
}

void SpriteTable::destroy(SpriteId id) {
    if (!valid(id)) return;

    // Before the swap-remove below, and before the generation moves: `detach` reaches
    // `records` through `animated`, and a sprite left in that walk after its slot was
    // freed is a playback advancing a rectangle nobody owns.
    detach(id.index);

    Record& r = records[id.index];
    const uint32_t index = r.index;

    // Swap-remove, and the order it breaks is repaired by the sort this marks dirty.
    // Erasing in place to preserve the order would be O(n) on an event a game does ten
    // thousand times a level, to keep an ordering that is about to be recomputed anyway.
    const auto last = static_cast<uint32_t>(gpu.size() - 1);
    if (index != last) {
        gpu[index] = gpu[last];
        slotOf[index] = slotOf[last];
        records[slotOf[index]].index = index;
    }
    gpu.pop_back();
    slotOf.pop_back();

    r.live = false;
    ++r.generation;
    if (r.generation == 0) r.generation = 1;
    freeSprites.push_back(id.index);
    --liveSprites;
    sortDirty = true;
    // The swap-remove above rewrote `gpu[index]` and shortened the array, both of which the
    // renderer's copy has to see -- and it has to see them even on a frame where the sort
    // that follows changes nothing, because the entry that moved is already different.
    ++rev;
}

bool SpriteTable::valid(SpriteId id) const {
    return id.valid() && id.index < records.size() && records[id.index].live &&
           records[id.index].generation == id.generation;
}

GpuSprite* SpriteTable::at(SpriteId id) {
    if (!valid(id)) return nullptr;
    // Before the caller writes, and unconditionally once the handle is good: every setter
    // below reaches its entry through here and through nothing else, so no setter can
    // forget the bump and no setter added later can either. A stale handle bumps nothing,
    // which is the case that would otherwise re-upload the array for a call that changed
    // no byte of it.
    ++rev;
    return &gpu[records[id.index].index];
}

void SpriteTable::setPosition(SpriteId id, const glm::vec2& position) {
    if (GpuSprite* s = at(id); s != nullptr) {
        s->posSize.x = position.x;
        s->posSize.y = position.y;
    }
}

void SpriteTable::setSize(SpriteId id, const glm::vec2& size) {
    if (GpuSprite* s = at(id); s != nullptr) {
        s->posSize.z = size.x;
        s->posSize.w = size.y;
    }
}

void SpriteTable::setPivot(SpriteId id, const glm::vec2& pivot) {
    if (GpuSprite* s = at(id); s != nullptr) {
        s->rotPivot.z = pivot.x;
        s->rotPivot.w = pivot.y;
    }
}

void SpriteTable::setRotation(SpriteId id, float radians) {
    if (GpuSprite* s = at(id); s != nullptr) {
        s->rotPivot.x = std::cos(radians);
        s->rotPivot.y = std::sin(radians);
    }
}

void SpriteTable::setUv(SpriteId id, const glm::vec4& uv) {
    if (GpuSprite* s = at(id); s != nullptr) s->uvRect = uv;
}

void SpriteTable::setTint(SpriteId id, const glm::vec4& tint) {
    if (GpuSprite* s = at(id); s != nullptr) s->meta.y = packTint(tint);
}

void SpriteTable::setFlip(SpriteId id, bool flipX, bool flipY) {
    if (GpuSprite* s = at(id); s != nullptr) {
        s->meta.z = (flipX ? kSpriteFlipX : 0u) | (flipY ? kSpriteFlipY : 0u);
    }
}

void SpriteTable::setImage(SpriteId id, gfx::ImageId image) {
    if (GpuSprite* s = at(id); s != nullptr) {
        records[id.index].image = image;
        s->meta.x = images != nullptr ? images->slot(image) : gfx::ImageTable::kFallbackSlot;
    }
}

// -------------------------------------------------------------- sheets and clips (P5)

SpriteSheetId SpriteTable::createSheet(const SpriteSheetDesc& desc) {
    // A zero cell or an empty sheet is refused rather than normalised. `SpriteDesc::uv`
    // already gives a zero width the meaning *the whole image*, so a sheet that accepted
    // one would draw every cell as the entire file -- which looks like a shader bug and
    // is an authoring one.
    if (desc.frame.x == 0 || desc.frame.y == 0 || desc.count == 0) return {};

    uint32_t slot = 0;
    if (!freeSheets.empty()) {
        slot = freeSheets.back();
        freeSheets.pop_back();
    } else {
        slot = static_cast<uint32_t>(sheets.size());
        sheets.emplace_back();
    }

    Sheet& sheet = sheets[slot];
    sheet.desc = desc;
    if (sheet.desc.columns == 0) sheet.desc.columns = 1;
    sheet.clips.clear();
    sheet.live = true;
    if (sheet.generation == 0) sheet.generation = 1;
    ++liveSheets;

    return SpriteSheetId{slot, sheet.generation};
}

void SpriteTable::destroySheet(SpriteSheetId id) {
    if (!valid(id)) return;

    // Backwards, because `detach` swap-removes and the entry that moves is the last one.
    for (uint32_t i = static_cast<uint32_t>(animated.size()); i-- > 0;) {
        const uint32_t slot = animated[i];
        if (records[slot].sheet.index == id.index) detach(slot);
    }

    Sheet& sheet = sheets[id.index];
    sheet.live = false;
    sheet.clips.clear();
    sheet.clips.shrink_to_fit();
    ++sheet.generation;
    if (sheet.generation == 0) sheet.generation = 1;
    freeSheets.push_back(id.index);
    --liveSheets;
}

bool SpriteTable::valid(SpriteSheetId id) const {
    return id.valid() && id.index < sheets.size() && sheets[id.index].live &&
           sheets[id.index].generation == id.generation;
}

uint32_t SpriteTable::addClip(SpriteSheetId sheet, SpriteClip clip) {
    if (!valid(sheet)) return kNoClip;
    std::vector<SpriteClip>& clips = sheets[sheet.index].clips;
    clips.push_back(std::move(clip));
    return static_cast<uint32_t>(clips.size() - 1);
}

uint32_t SpriteTable::findClip(SpriteSheetId sheet, const std::string& name) const {
    if (!valid(sheet)) return kNoClip;
    const std::vector<SpriteClip>& clips = sheets[sheet.index].clips;
    for (uint32_t i = 0; i < static_cast<uint32_t>(clips.size()); ++i) {
        if (clips[i].name == name) return i;
    }
    return kNoClip;
}

const SpriteClip& SpriteTable::clip(SpriteSheetId sheet, uint32_t index) const {
    if (!valid(sheet) || index >= sheets[sheet.index].clips.size()) return noClip();
    return sheets[sheet.index].clips[index];
}

uint32_t SpriteTable::clipCount(SpriteSheetId sheet) const {
    if (!valid(sheet)) return 0;
    return static_cast<uint32_t>(sheets[sheet.index].clips.size());
}

glm::vec4 SpriteTable::frameUv(SpriteSheetId sheet, uint32_t frame) const {
    if (!valid(sheet)) return {};
    const SpriteSheetDesc& d = sheets[sheet.index].desc;

    // Past the end clamps to the last cell. A clip whose `first + count` runs off its
    // sheet is an authoring mistake, and repeating the last cell says so on screen
    // without reading a rectangle that is not in the file.
    const uint32_t f = std::min(frame, d.count - 1);
    const uint32_t col = f % d.columns;
    const uint32_t row = f / d.columns;

    return {static_cast<float>(d.origin.x + col * (d.frame.x + d.spacing.x)),
            static_cast<float>(d.origin.y + row * (d.frame.y + d.spacing.y)), static_cast<float>(d.frame.x),
            static_cast<float>(d.frame.y)};
}

uint32_t SpriteTable::frameAt(SpriteSheetId sheet, uint32_t clipIndex, float time) const {
    return clipFrame(clip(sheet, clipIndex), time);
}

// --------------------------------------------------------------------- playback (P5)

void SpriteTable::detach(uint32_t slot) {
    Record& r = records[slot];
    if (r.animIndex == kNotAnimating) return;

    const auto last = static_cast<uint32_t>(animated.size() - 1);
    if (r.animIndex != last) {
        animated[r.animIndex] = animated[last];
        records[animated[r.animIndex]].animIndex = r.animIndex;
    }
    animated.pop_back();
    r.animIndex = kNotAnimating;
}

void SpriteTable::applyFrame(uint32_t slot) {
    Record& r = records[slot];
    const uint32_t f = clipFrame(clip(r.sheet, r.playback.clip), r.playback.time);
    if (f == r.frame) return;
    r.frame = f;
    gpu[r.index].uvRect = frameUv(r.sheet, f);
    // Inside the guard, not above it. A thousand sprites at 12 fps on a 60 Hz step change
    // cell on one step in five, and bumping on every step would upload the whole array on
    // the four where the rectangle is the one already in the buffer -- which is the same
    // four fifths of no work P5 built this early return for.
    ++rev;
}

void SpriteTable::play(SpriteId id, SpriteSheetId sheet, uint32_t clipIndex, float speed) {
    if (!valid(id) || !valid(sheet)) return;
    if (clipIndex >= sheets[sheet.index].clips.size()) return;

    Record& r = records[id.index];
    if (r.animIndex == kNotAnimating) {
        r.animIndex = static_cast<uint32_t>(animated.size());
        animated.push_back(id.index);
    }

    r.sheet = sheet;
    // C7's `ClipPlayback`, and the loop mode comes off the clip rather than off this call:
    // for a flipbook, whether it repeats is a property of the animation an artist drew,
    // not of the moment a game started it. `SceneAnimator::play` takes one because a
    // transition clip and a locomotion clip share a skeleton and not a mode.
    r.playback = ClipPlayback{.clip = clipIndex,
                              .time = 0.0f,
                              .speed = speed,
                              .loop = sheets[sheet.index].clips[clipIndex].loop,
                              .playing = true};

    // Immediately, not at the next step: a sprite told to play frame 0 shows frame 0 on
    // the frame it was told, and a game that creates and plays inside `init` has no step
    // between the call and the first draw.
    r.frame = kNoFrame;
    applyFrame(id.index);
}

void SpriteTable::stop(SpriteId id) {
    if (!valid(id)) return;
    detach(id.index);
}

void SpriteTable::setPlaying(SpriteId id, bool playing_) {
    if (!valid(id)) return;
    records[id.index].playback.playing = playing_;
}

void SpriteTable::setSpeed(SpriteId id, float speed) {
    if (!valid(id)) return;
    records[id.index].playback.speed = speed;
}

bool SpriteTable::playing(SpriteId id) const {
    if (!valid(id)) return false;
    const Record& r = records[id.index];
    return r.animIndex != kNotAnimating && r.playback.playing;
}

uint32_t SpriteTable::frame(SpriteId id) const {
    if (!valid(id) || records[id.index].animIndex == kNotAnimating) return kNoFrame;
    return records[id.index].frame;
}

float SpriteTable::clipTime(SpriteId id) const {
    if (!valid(id) || records[id.index].animIndex == kNotAnimating) return 0.0f;
    return records[id.index].playback.time;
}

void SpriteTable::update(float dt) {
    auto s = core::Profiler::scope("SpriteTable::update");
    // Cleared even on a step that advances nothing. A game reads this after the step, and
    // handing it the previous step's list would fire every footstep twice.
    fired.clear();

    for (const uint32_t slot : animated) {
        Record& r = records[slot];
        const SpriteClip& c = clip(r.sheet, r.playback.clip);
        const float duration = clipDuration(c);
        const float from = r.playback.time;

        // C7's two calls, in C7's order and for C7's reasons -- `advance` moves the
        // playhead and wraps or clamps it, `crossedEvents` then asks what the interval
        // it just travelled contains. Nothing here is a second implementation of either.
        (void)advance(r.playback, duration, dt);

        if (!c.events.empty()) {
            crossings.clear();
            crossedEvents(r.playback, c.events, duration, from, crossings);
            for (const uint32_t e : crossings) {
                fired.push_back({SpriteId{slot, r.generation}, r.sheet, r.playback.clip, e});
            }
        }

        applyFrame(slot);
    }
}

// ----------------------------------------------------------------------- the draw

void SpriteTable::prepare() {
    // A load or a destroy in the image table can change what slot a handle resolves to --
    // and a handle whose image was destroyed has to stop naming the slot the next load
    // will take. Rare, so it is a revision comparison rather than a lookup per sprite per
    // frame, exactly as `Renderer::syncImages` reconciles residency.
    if (images != nullptr && images->revision() != imageRevision) {
        imageRevision = images->revision();
        for (size_t i = 0; i < gpu.size(); ++i) {
            gpu[i].meta.x = images->slot(records[slotOf[i]].image);
        }
        // One revision reconciled into another, which is the shape the third occurrence of
        // this pattern already had: an image loaded or destroyed rewrites `meta.x` for
        // every sprite, and a frame slot that skipped the copy would draw the old slot.
        ++rev;
    }

    if (sortDirty) sort();
}

void SpriteTable::sort() {
    sortDirty = false;
    ++sorts;
    // Unconditionally, and before the size test: a sort permutes `gpu`, and `setLayerOrder`
    // is the one mutation that reaches the array through nothing but this. A sort of fewer
    // than two entries permutes nothing, but the flag that asked for it was set by
    // something -- `createLayer` reusing a slot, or a `destroyLayer` -- so the cheap bump is
    // the one that cannot be reasoned wrong.
    ++rev;
    if (gpu.size() < 2) return;

    const auto n = static_cast<uint32_t>(gpu.size());
    order.resize(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;

    // A permutation, then one pass applying it, rather than sorting three arrays in
    // lockstep: `std::sort` over an index is one comparison per swap instead of moving 64
    // bytes of sprite per swap, and the key is a plain integer read out of the record.
    //
    // `std::sort` rather than `stable_sort` because the key is already a total order --
    // the sequence number is unique per sprite -- so stability would be buying a guarantee
    // that is a property of the key. That is also what makes the frame reproducible run to
    // run, which the golden suite needs.
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const Record& ra = records[slotOf[a]];
        const Record& rb = records[slotOf[b]];
        return sortKey(layers[ra.layer].order, ra.sequence) < sortKey(layers[rb.layer].order, rb.sequence);
    });

    gpuScratch.resize(n);
    slotScratch.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        gpuScratch[i] = gpu[order[i]];
        slotScratch[i] = slotOf[order[i]];
        records[slotScratch[i]].index = i;
    }
    gpu.swap(gpuScratch);
    slotOf.swap(slotScratch);
}

} // namespace scene
