#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace gfx {

/**
 * @brief One end of one debug line, as `debug_line.vert` reads it.
 *
 * A header of its own because `physics/PhysicsWorld.h` and `gfx/Renderer.h` both need it:
 * merging it into either puts Vulkan on the physics world's include path or Jolt on the
 * renderer's.
 */
struct DebugLineVertex {
    glm::vec3 position{0.0f};
    /// 0xAABBGGRR, which is what a `R8G8B8A8_UNORM` vertex attribute reads on a
    /// little-endian host -- so the shader takes it as a `vec4` with no unpacking.
    uint32_t color = 0xFFFFFFFFu;
};

static_assert(sizeof(DebugLineVertex) == 16, "DebugLineVertex must match debug_line.vert");

/// Pack a colour the way `DebugLineVertex::color` wants it. Components are clamped
/// rather than wrapped: a caller that computed 1.2 meant white, not a dark red.
inline uint32_t packDebugColor(const glm::vec4& rgba) {
    const auto ch = [](float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return ch(rgba.r) | (ch(rgba.g) << 8) | (ch(rgba.b) << 16) | (ch(rgba.a) << 24);
}

} // namespace gfx
