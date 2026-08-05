#include "gfx/ImageTable.h"

#include "core/Logger.h"
#include "core/Resources.h"

namespace gfx {

namespace {

/// Returned by `at()` for a slot nobody owns, so the device half can walk a range
/// without a bounds test per read.
const ImageTable::Entry kDeadEntry{};

} // namespace

void ImageTable::init(uint32_t capacity) {
    entries.clear();
    freeSlots.clear();
    liveSlots = 0;
    // One minimum: slot zero is the font atlas every unwritten descriptor points at, so a
    // capacity of zero would leave those descriptors sampling nothing.
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
            // The number is the device's -- how many combined image samplers one
            // descriptor set can hold -- not an engine constant to raise.
            core::Logger::warn(core::LogCategory::Render,
                               "images().load(%s): all %u image slots are in use", name.c_str(), slotCapacity);
            return {};
        }
        s = static_cast<uint32_t>(entries.size());
        entries.emplace_back();
        // One, not zero: `Handle::valid()` reserves generation zero for "never issued", so
        // a handle carrying it never validates.
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
    // Past the wrap, back to 1 rather than 0: zero is "never issued", so wrapping to it
    // would make every stale handle on this slot validate again.
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
