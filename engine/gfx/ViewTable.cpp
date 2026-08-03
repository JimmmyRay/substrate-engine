#include "gfx/ViewTable.h"

#include "core/Logger.h"

namespace gfx {

namespace {

/// Returned by `at()` for a slot nobody owns, so the device half can walk a range
/// without every read needing a bounds test of its own.
const ViewTable::Entry kDeadEntry{};

} // namespace

void ViewTable::init(uint32_t capacity) {
    entries.clear();
    freeSlots.clear();
    liveSlots = 0;
    slotCapacity = capacity;
    ++rev;
}

void ViewTable::shutdown() {
    entries.clear();
    freeSlots.clear();
    slotCapacity = 0;
    liveSlots = 0;
    ++rev;
}

ViewId ViewTable::create(ImageTable& images, glm::uvec2 extent) {
    // Slot zero is a real view here, unlike the image table's: there is no fallback view
    // to reserve it for. What a stale handle resolves to is nothing at all -- `camera`
    // returns null and `image` an invalid id -- because a view has no harmless default the
    // way a texture slot has the font atlas.
    uint32_t s;
    if (!freeSlots.empty()) {
        s = freeSlots.back();
        freeSlots.pop_back();
    } else {
        if (static_cast<uint32_t>(entries.size()) >= slotCapacity) {
            // A stated limit rather than a silent truncation. The number is `kMaxViews`
            // less the presenting view, and it is a uniform-block count rather than an
            // arbitrary cap -- raising it costs one block, light buffer and shadow-matrix
            // buffer per frame slot.
            core::Logger::warn(core::LogCategory::Render, "views().create(): all %u view slots are in use",
                               slotCapacity);
            return {};
        }
        s = static_cast<uint32_t>(entries.size());
        entries.emplace_back();
        // One, not zero. `Handle::valid()` reserves generation zero for "never issued",
        // so the first handle a slot ever produces has to be past it.
        entries[s].generation = 1;
    }

    Entry& e = entries[s];
    e.camera = scene::Camera{};
    // Either component zero is "follow the presenting view", so a caller that named one
    // side and left the other gets the follow rule rather than a one-pixel-tall view.
    e.extent = (extent.x != 0 && extent.y != 0) ? extent : glm::uvec2{0, 0};
    // A reacquired slot must not still be driven by the camera the previous view's owner
    // installed, which may well have been destroyed with it.
    e.installed = nullptr;
    e.image = images.adopt("view");
    if (!e.image.valid()) {
        // The slot goes straight back. A view whose destination cannot be named is a view
        // that renders where nothing can read it, which is worse than not having one --
        // and reporting it here means the caller learns at the call it made.
        core::Logger::warn(core::LogCategory::Render, "views().create(): no image slot for the view's destination");
        freeSlots.push_back(s);
        return {};
    }
    e.live = true;
    ++liveSlots;
    ++rev;
    return ViewId{s, e.generation};
}

void ViewTable::destroy(ViewId id, ImageTable& images) {
    if (!valid(id)) return;

    Entry& e = entries[id.index];
    images.destroy(e.image);
    e.image = {};
    e.live = false;
    // Past the wrap, back to 1 rather than 0: zero is "never issued", and a slot that has
    // been reused four billion times is still a slot that has been issued.
    e.generation = e.generation == 0xFFFFFFFFu ? 1 : e.generation + 1;
    freeSlots.push_back(id.index);
    --liveSlots;
    ++rev;
}

bool ViewTable::valid(ViewId id) const {
    return id.index < entries.size() && entries[id.index].generation == id.generation && entries[id.index].live;
}

scene::Camera* ViewTable::camera(ViewId id) {
    if (!valid(id)) return nullptr;
    Entry& e = entries[id.index];
    return e.installed != nullptr ? e.installed : &e.camera;
}

const scene::Camera* ViewTable::camera(ViewId id) const {
    if (!valid(id)) return nullptr;
    return &entries[id.index].active();
}

void ViewTable::setCamera(ViewId id, scene::Camera* c) {
    if (!valid(id)) return;
    entries[id.index].installed = c;
}

void ViewTable::resize(ViewId id, glm::uvec2 extent) {
    if (!valid(id)) return;
    Entry& e = entries[id.index];
    const glm::uvec2 want = (extent.x != 0 && extent.y != 0) ? extent : glm::uvec2{0, 0};
    // Only when it moved. The revision is what makes the renderer wait on the device and
    // rebuild a target set, so a game assigning the same size every frame would otherwise
    // pay that every frame.
    if (want == e.extent) return;
    e.extent = want;
    ++rev;
}

glm::uvec2 ViewTable::extent(ViewId id) const {
    return valid(id) ? entries[id.index].extent : glm::uvec2{0, 0};
}

ImageId ViewTable::image(ViewId id) const {
    return valid(id) ? entries[id.index].image : ImageId{};
}

const ViewTable::Entry& ViewTable::at(uint32_t s) const {
    return s < entries.size() ? entries[s] : kDeadEntry;
}

ViewTable::Entry* ViewTable::mutableAt(uint32_t s) {
    return s < entries.size() ? &entries[s] : nullptr;
}

} // namespace gfx
