#include "scene/WorldSave.h"

namespace scene {

void writeWorldSave(core::SaveWriter& out, std::string_view scene, const InstanceTable& table, float timeScale,
                    uint64_t steps) {
    out.beginSection("engine", kWorldSaveVersion);
    out.text(scene);
    out.u32(table.slotCount());
    for (uint32_t slot = 0; slot < table.slotCount(); ++slot) {
        out.u32(table.slot(slot).meta.z);
        out.mat4(table.transform(slot));
    }
    out.f32(timeScale);
    out.u64(steps);
}

bool readWorldSave(core::SaveReader& in, WorldSave& out, std::string& reason) {
    if (!in.section("engine", kWorldSaveVersion)) {
        reason = in.reason();
        return false;
    }

    WorldSave staged;
    staged.scene = in.text();
    const uint32_t slots = in.u32();
    if (!in.ok()) {
        reason = in.reason();
        return false;
    }

    // Checked against what is actually left, not trusted from the file: a count is 4 bytes
    // and an entry is 68, so a corrupt one is otherwise a request to allocate gigabytes.
    constexpr size_t kEntryBytes = sizeof(uint32_t) + sizeof(glm::mat4);
    if (static_cast<size_t>(slots) * kEntryBytes > in.remaining()) {
        reason = "the save claims " + std::to_string(slots) + " instance slots, which is more than the section holds";
        return false;
    }

    staged.flags.resize(slots);
    staged.transforms.resize(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        staged.flags[i] = in.u32();
        staged.transforms[i] = in.mat4();
    }
    staged.timeScale = in.f32();
    staged.steps = in.u64();

    if (!in.ok()) {
        reason = in.reason();
        return false;
    }

    out = std::move(staged);
    return true;
}

bool worldSaveApplies(const WorldSave& save, std::string_view scene, const InstanceTable& table, std::string& reason) {
    if (save.scene != scene) {
        reason = "the save is of '" + save.scene + "' and this is '" + std::string(scene) + "'";
        return false;
    }
    if (save.flags.size() != table.slotCount()) {
        reason = "the save has " + std::to_string(save.flags.size()) + " instance slots and this scene has " +
                 std::to_string(table.slotCount());
        return false;
    }
    return true;
}

void applyWorldSave(const WorldSave& save, InstanceTable& table) {
    const uint32_t slots = std::min(static_cast<uint32_t>(save.flags.size()), table.slotCount());
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if ((save.flags[slot] & kInstanceLive) == 0) continue;
        if ((table.slot(slot).meta.z & kInstanceLive) == 0) continue;
        const InstanceId id = table.idAt(slot);
        table.setTransform(id, save.transforms[slot]);
        table.setFlags(id, save.flags[slot] & kSavedInstanceFlags, (~save.flags[slot]) & kSavedInstanceFlags);
    }
}

} // namespace scene
