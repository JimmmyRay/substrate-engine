#pragma once

#include <cstdint>

namespace scene {

/**
 * @file engine/scene/Node.h
 * @brief What "no glTF node" is called, in one place.
 *
 * The narrowest scope `Animation.h`, `Collider.h`, `AudioSource.h` and `ParticleEmitter.h`
 * all already reach; anything wider belongs elsewhere.
 */

/// A glTF node index that is absent -- a collider built in code, an emitter with no
/// placing node, an animator with no root motion.
///
/// Numerically equal to every other sentinel in this engine, which principles.md rule 8
/// names as a hazard rather than a convenience: a function returns only a sentinel from
/// its own domain, and this one's domain is node indices.
inline constexpr uint32_t kNoNode = 0xFFFFFFFFu;

} // namespace scene
