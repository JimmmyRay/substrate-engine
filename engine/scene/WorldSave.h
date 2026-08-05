#pragma once

#include "core/SaveFile.h"
#include "scene/InstanceTable.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file engine/scene/WorldSave.h
 * @brief The engine's own section of a save, and the decision about whether it applies.
 *
 * **Read whole, ask whether it applies, then apply.** Three calls rather than one because
 * the middle is a promise: a file written against another scene has to be refused *before*
 * anything is written, not halfway through. The decision is arithmetic over a scene name, a
 * slot count and a table -- no device, no window -- so the hosted suite can reach it.
 *
 * The section is keyed by instance slot, so what it carries is what an instance is:
 * transforms, the flags in `kSavedInstanceFlags`, and the clock. A body has no identity
 * here; a game whose bodies matter writes them in its own section.
 */
namespace scene {

/// Bumped when the section's shape changes. A save carrying a higher number is refused
/// with a reason; a lower one is read in the shape that number described.
inline constexpr uint32_t kWorldSaveVersion = 1;

/// The section, read whole. Nothing here has been applied to anything.
struct WorldSave {
    /// The scene it was taken from, as the config named it.
    std::string scene;
    /// One entry per *slot*, live or not, so an index in the file is the table's own and a
    /// reader needs no mapping.
    std::vector<uint32_t> flags;
    std::vector<glm::mat4> transforms;
    float timeScale = 1.0f;
    /// The simulation's step counter. Written because it is cheap and a game may want it;
    /// **not** restored, because a step count is history rather than state.
    uint64_t steps = 0;
};

/**
 * @brief Flags a save is allowed to carry back. An allowlist, so widening it is a decision
 *        rather than a diff.
 *
 * Two reasons a flag is not in here, and the difference matters:
 *
 * - `kInstanceBlended`, `kInstanceMasked`, `kInstanceSkinned` and `kInstanceMorphed` are
 *   properties of the *geometry*, fixed at load from the material and the skin. Restoring
 *   them from a file lets a save turn a static mesh into a skinned one and route it into a
 *   deformation dispatch that has no vertices for it.
 * - `kInstanceVisible` is not state. The cull dispatch writes it into the GPU's copy and
 *   nothing reads it back, so the CPU-side bit is clear for everything, always -- saving it
 *   persists a constant zero.
 * - `kInstanceLive` is carried by `applyWorldSave`'s liveness rule instead: it decides
 *   *whether* a slot is touched, not what is written to it.
 */
inline constexpr uint32_t kSavedInstanceFlags = kInstanceDynamic;

/// Open the `engine` section and stream the table into it.
void writeWorldSave(core::SaveWriter& out, std::string_view scene, const InstanceTable& table, float timeScale,
                    uint64_t steps);

/**
 * @brief Read it back, whole.
 *
 * @return false when the section is absent, newer than `kWorldSaveVersion`, or runs out
 *         part way. `reason` is set in every case, and `out` is not to be trusted.
 */
[[nodiscard]] bool readWorldSave(core::SaveReader& in, WorldSave& out, std::string& reason);

/**
 * @brief Does this save describe the world in front of us?
 *
 * The scene's name and the table's slot count, both: the name alone accepts a save taken
 * before the scene was edited, and the count alone accepts a save of a different scene with
 * as many instances.
 *
 * @return false with a reason fit for a log line, and the caller has written nothing.
 */
[[nodiscard]] bool worldSaveApplies(const WorldSave& save, std::string_view scene, const InstanceTable& table,
                                    std::string& reason);

/**
 * @brief Put it back. Only call this once `worldSaveApplies` has said yes.
 *
 * A slot dead on either side stays dead: creating an instance to fill one would issue a new
 * generation and hand it to nobody. What is restored is the state of what exists.
 */
void applyWorldSave(const WorldSave& save, InstanceTable& table);

} // namespace scene
