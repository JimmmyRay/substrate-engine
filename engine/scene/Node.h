#pragma once

#include <cstdint>

namespace scene {

/**
 * @file engine/scene/Node.h
 * @brief What "no glTF node" is called, in one place.
 *
 * A header for one constant, and the precedent is `core/Format.h` -- which says of itself
 * that it is "a header for one macro, rather than the `#ifdef` repeated in each of the four
 * files that needs it". This is the same shape: four structs under `scene/` carry a node
 * index that may be absent, and until D3 each said so its own way. Three declared their own
 * `kNoNode` and `ParticleEmitter` wrote `0xFFFFFFFFu` with a comment explaining it.
 *
 * It is not in `Animation.h` beside `SceneNode`, which would otherwise be the obvious home:
 * `Collider.h`, `AudioSource.h` and `ParticleSystem.h` do not include it and should not
 * start to for a constant. This is the narrowest scope all four already reach.
 */

/// A glTF node index that is absent -- a collider built in code, an emitter with no
/// placing node, an animator with no root motion.
///
/// The same numeric value as every other sentinel in this engine, and that is the hazard
/// `principles.md` rule 8 names rather than a convenience to rely on: a function returns
/// only a sentinel from its own domain, and this one's domain is node indices.
inline constexpr uint32_t kNoNode = 0xFFFFFFFFu;

} // namespace scene
