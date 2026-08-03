#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace gfx {

/**
 * @brief One end of one debug line, as `debug_line.vert` reads it (S4.5).
 *
 * Its own header, and a very small one, for a reason worth stating: this is the only
 * type `scene/Physics.h` and `gfx/Renderer.h` both need, and every other candidate home
 * would have dragged something with it. Putting it in `Renderer.h` would put Vulkan on
 * the physics world's include path; putting it in `Physics.h` would put Jolt on the
 * renderer's. Sixteen bytes in a header of their own cost neither.
 *
 * The colour is packed rather than four floats because a line list is the one vertex
 * stream in this engine that is rebuilt from scratch every frame -- ten thousand lines
 * is 320 KB at this size and 640 at the other, uploaded per frame either way.
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
