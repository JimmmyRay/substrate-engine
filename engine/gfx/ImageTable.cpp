#include "gfx/ImageTable.h"

#include "core/Logger.h"
#include "core/Resources.h"

namespace gfx {

namespace {

/// Returned by `at()` for a slot nobody owns, so the device half can walk a range
/// without every read needing a bounds test of its own.
const ImageTable::Entry kDeadEntry{};

} // namespace

void ImageTable::init(uint32_t capacity) {
    entries.clear();
    freeSlots.clear();
    liveSlots = 0;
    // One minimum, and it is the fallback: slot zero is the font atlas and is never
    // handed out, so a table with room for nothing still has a slot for the thing every
    // free descriptor points at.
    slotCapacity = capacity < 1 ? 1 : capacity;
    entries.emplace_back();
    ++rev;
}

void ImageTable::shutdown() {
    entries.clear();
    freeSlots.clear();
    slotCapacity = 0;
    liveSlots = 0;
    ++rev;
}

ImageId ImageTable::load(const std::string& name) {
    const core::Resources file(name);
    if (!file.found()) {
        core::Logger::warn(core::LogCategory::Render, "images().load(%s): not found (resolved to %s)", name.c_str(),
                           file.string().c_str());
        return {};
    }

    uint32_t s;
    if (!freeSlots.empty()) {
        s = freeSlots.back();
        freeSlots.pop_back();
    } else {
        if (static_cast<uint32_t>(entries.size()) >= slotCapacity) {
            // A stated limit rather than a silent truncation, and the number in it is the
            // device's: how many combined image samplers one descriptor set can hold.
            // Nothing here re-enacts `kMaxOverlayImages` at a larger value.
            core::Logger::warn(core::LogCategory::Render,
                               "images().load(%s): all %u image slots are in use", name.c_str(), slotCapacity);
            return {};
        }
        s = static_cast<uint32_t>(entries.size());
        entries.emplace_back();
        // One, not zero. `Handle::valid()` reserves generation zero for "never issued",
        // so the first handle a slot ever produces has to be past it.
        entries[s].generation = 1;
    }

    Entry& e = entries[s];
    e.path = file.path().string();
    e.name = name;
    e.live = true;
    ++liveSlots;
    ++rev;
    return ImageId{s, e.generation};
}

ImageId ImageTable::adopt(const std::string& name) {
    // Slot acquisition is the same in both directions and the difference is one flag, so
    // this is `load` without the file. Extracting the shared half would be extracting five
    // lines to two callers; the rule of threes says wait, and the third caller would be a
    // real third kind of image rather than a second spelling of this one.
    uint32_t s;
    if (!freeSlots.empty()) {
        s = freeSlots.back();
        freeSlots.pop_back();
    } else {
        if (static_cast<uint32_t>(entries.size()) >= slotCapacity) {
            core::Logger::warn(core::LogCategory::Render, "images().adopt(%s): all %u image slots are in use",
                               name.c_str(), slotCapacity);
            return {};
        }
        s = static_cast<uint32_t>(entries.size());
        entries.emplace_back();
        entries[s].generation = 1;
    }

    Entry& e = entries[s];
    e.path.clear();
    e.name = name;
    e.live = true;
    e.external = true;
    ++liveSlots;
    ++rev;
    return ImageId{s, e.generation};
}

void ImageTable::destroy(ImageId id) {
    if (!valid(id)) return;

    Entry& e = entries[id.index];
    e.live = false;
    e.external = false;
    e.path.clear();
    e.name.clear();
    // Past the wrap, back to 1 rather than 0: zero is "never issued", and a slot that has
    // been reused four billion times is still a slot that has been issued.
    e.generation = e.generation == 0xFFFFFFFFu ? 1 : e.generation + 1;
    freeSlots.push_back(id.index);
    --liveSlots;
    ++rev;
}

bool ImageTable::valid(ImageId id) const {
    return id.index < entries.size() && entries[id.index].generation == id.generation && entries[id.index].live;
}

uint32_t ImageTable::slot(ImageId id) const {
    return valid(id) ? id.index : kFallbackSlot;
}

const ImageTable::Entry& ImageTable::at(uint32_t s) const {
    return s < entries.size() ? entries[s] : kDeadEntry;
}

} // namespace gfx
