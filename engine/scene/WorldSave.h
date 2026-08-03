#pragma once

#include "core/SaveFile.h"
#include "scene/InstanceTable.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file engine/scene/WorldSave.h
 * @brief The engine's own section of a save, and the decision about whether it applies (C6).
 *
 * ## Why this is four functions and not a method on Engine
 *
 * The interesting part of loading a save is the *refusal*: a file written against one
 * scene must not be applied to another, and it must be refused **before** anything is
 * written rather than halfway through. That decision is arithmetic over a scene name, a
 * slot count and a table -- no device, no window -- and leaving it inside
 * `Engine::loadGame` would have made the one part worth testing the one part unreachable
 * by the hosted suite.
 *
 * So the shape is: **read whole, ask whether it applies, then apply.** Three steps because
 * the middle one is the promise, and a two-step version would have to make it while it was
 * already writing.
 *
 * ## What the engine saves, and what it does not
 *
 * What it saves is what it owns *and can put back through its own public surface*: every
 * instance's transform and the flags a game is allowed to own, plus the simulation clock.
 *
 * What it does not save is anything it would have to reach into a subsystem to restore. A
 * rigid body is the case worth stating, and the reason changed once without the answer
 * changing: it used to be that nothing could put a body back, and since P7 something can --
 * `setBodyTransform` and `setLinearVelocity`. What still does not exist is a body's
 * *identity* in the file. This section is keyed by instance slot, a body is not an
 * instance, and the bodies a game spawns are its own to count; a save that wrote them would
 * have to decide which of them the engine owns, which it does not. A game whose bodies
 * matter writes them in its own section and rebuilds them, which is what every engine that
 * ships does.
 */
namespace scene {

/// Bumped when the section's shape changes. A save carrying a higher number is refused
/// with a reason; a lower one is read in the shape that number described.
inline constexpr uint32_t kWorldSaveVersion = 1;

/// The section, read whole. Nothing here has been applied to anything.
struct WorldSave {
    /// The scene it was taken from, as the config named it. The first half of "is this
    /// save about this world".
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
 * @brief Flags a save is allowed to carry back. One bit, and the constant exists to say
 *        which -- an allowlist naming a single flag is a statement; a bare
 *        `kInstanceDynamic` at two call sites is an accident waiting to be widened.
 *
 * The other six are excluded for two different reasons, and the difference matters:
 *
 * - `kInstanceBlended`, `kInstanceMasked`, `kInstanceSkinned`, `kInstanceMorphed` are
 *   properties of the *geometry*, fixed at load from the material and the skin. Restoring
 *   them from a file would let a save turn a static mesh into a skinned one and route it
 *   into a deformation dispatch that has no vertices for it.
 * - `kInstanceVisible` is not state at all. The cull dispatch writes it into the GPU's
 *   copy of the table and it is never read back, so the CPU-side bit is clear for
 *   everything, always. Saving it wrote a constant zero and restoring it overwrote
 *   nothing -- a field that looks like persistence and is not, which is worse than an
 *   absent one. `Inspector` refuses to display it for the same reason.
 * - `kInstanceLive` is carried, but through `applyWorldSave`'s liveness rule rather than
 *   this mask: it decides *whether* a slot is touched, not what is written to it.
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
 * The scene's name and the table's slot count, both. The name alone would accept a save
 * taken before the scene was edited; the count alone would accept a save of a different
 * scene that happened to have as many instances. Neither is sufficient and together they
 * are cheap.
 *
 * @return false with a reason fit for a log line, and the caller has written nothing.
 */
[[nodiscard]] bool worldSaveApplies(const WorldSave& save, std::string_view scene, const InstanceTable& table,
                                    std::string& reason);

/**
 * @brief Put it back. Only call this once `worldSaveApplies` has said yes.
 *
 * A slot dead in the save stays dead; a slot dead *here* is not resurrected, because
 * creating an instance would issue a new generation and hand it to nobody. What is
 * restored is the state of what exists.
 */
void applyWorldSave(const WorldSave& save, InstanceTable& table);

} // namespace scene
