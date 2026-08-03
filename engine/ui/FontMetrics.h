#pragma once

#include <cstdint>
#include <string>

/**
 * @file FontMetrics.h
 * @brief The half of the font that is numbers, and the vertex everything draws through.
 *
 * Split out of `ui/Font.h` by S6, and for one reason: `Font` owns a `GpuImage`, so its
 * header pulls Vulkan onto the include path of anything that measures a string. A UI is
 * mostly arithmetic over glyph advances, and arithmetic belongs in the hosted set where
 * the unit suite can reach it. This is the same sixteen-byte-header argument
 * `gfx/DebugLines.h` makes for the opposite pair -- there, keeping Jolt out of the
 * renderer; here, keeping Vulkan out of the layout.
 *
 * `Font` embeds one of these and hands it out. Nothing here allocates, opens a file or
 * touches a device.
 */
namespace ui {

constexpr uint32_t kFirstGlyph = 32; ///< ASCII space
constexpr uint32_t kGlyphCount = 95; ///< ASCII 32..126 inclusive

/**
 * @brief The font atlas's place in the overlay's image array (C5).
 *
 * Slot zero, and the number is load-bearing rather than arbitrary: it is `DrawVertex`'s
 * default, so a vertex that says nothing about textures samples the atlas, which is what
 * every glyph and every rectangle drawn before C5 existed already meant. It is also the
 * one slot `overlay.frag` reads as *coverage* rather than as colour -- the atlas is R8 and
 * the vertex supplies the tint, where an image supplies its own.
 *
 * Declared here rather than in the renderer because `DrawVertex` needs it and this header
 * is the one that may not know what Vulkan is.
 */
constexpr uint32_t kFontAtlasSlot = 0;

/**
 * @brief One glyph's quad, in stb_truetype's baked convention.
 *
 * The pen sits on the baseline and y increases downward, so `y0` is negative for
 * everything with ink above the baseline. Holding the embedded font to the same
 * convention as the TTF path is what lets one emitter draw either.
 */
struct Glyph {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f; ///< pen-relative pixels
    float s0 = 0.0f, t0 = 0.0f, s1 = 0.0f, t1 = 0.0f; ///< atlas texcoords
    float advance = 0.0f;
};

/**
 * @brief One vertex of everything drawn over the frame. Positions are pixels, origin
 *        top-left, which is the space glyph metrics and layout are already reasoned in.
 *
 * `gfx::Renderer` aliases `OverlayVertex` to this rather than declaring its own, because
 * the debug HUD, the binding menu and the UI all fill the same buffer through the same
 * pipeline -- see `appendRect` for why that is one pipeline and not two.
 */
struct DrawVertex {
    float x = 0.0f, y = 0.0f;
    float u = 0.0f, v = 0.0f;
    uint32_t rgba = 0xFFFFFFFFu; ///< R8G8B8A8_UNORM, little-endian: 0xAABBGGRR

    /// Which image in the overlay's array to sample (C5). Defaulted, so every existing
    /// caller -- text, rects, the debug HUD -- keeps drawing through the font atlas
    /// without knowing this field exists.
    uint32_t texture = kFontAtlasSlot;
};

static_assert(sizeof(DrawVertex) == 24, "DrawVertex must match overlay.vert's attributes");

/// Pack a colour the way `DrawVertex::rgba` wants it, from 0..1 components. Clamped
/// rather than wrapped: a caller that computed 1.2 meant white, not a dark red -- the
/// same rule `gfx::packDebugColor` follows, and deliberately not shared with it, because
/// that one is in a header this must not depend on and the function is four lines.
inline uint32_t packColor(float r, float g, float b, float a = 1.0f) {
    const auto ch = [](float value) {
        const float c = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return ch(r) | (ch(g) << 8) | (ch(b) << 16) | (ch(a) << 24);
}

/// Blend two packed colours, `t` of the way from `a` to `b`. What a hover highlight and
/// a disabled tint are, so neither needs a second constant per state.
[[nodiscard]] uint32_t mixColor(uint32_t a, uint32_t b, float t);

/**
 * @brief Glyph table and vertical metrics, plus the one texel that is not a glyph.
 *
 * `whiteU`/`whiteV` point at the centre of a small solid block the atlas reserves, and
 * that block is what makes S6.1 cost no pipeline at all. `overlay.frag` computes
 * `vColor.rgb, vColor.a * coverage`, so a quad whose texcoords sit on a texel with
 * coverage 1 *is* a solid rectangle in the vertex's colour. A `ui_rect` pipeline beside
 * the font one would draw the same pixels, and would cost either a second pass -- which
 * breaks the moment one panel overlaps another -- or a pipeline bind per widget.
 * Reserving four texels buys correct back-to-front order for free, because a button's
 * background and its label are then consecutive vertices in one buffer.
 */
struct FontMetrics {
    Glyph glyphs[kGlyphCount]{};
    float lineSpacing = 16.0f;
    float ascentPx = 10.0f;
    /// Centre of the reserved solid block. Sampled with NEAREST, so this must land
    /// squarely inside it rather than on its edge.
    float whiteU = 0.0f;
    float whiteV = 0.0f;

    /// Null for anything outside 32..126. Characters outside that range are the caller's
    /// problem to notice: every string this draws is either formatted by the engine or
    /// typed into a field that accepts what the platform gave it.
    [[nodiscard]] const Glyph* glyph(char c) const {
        const auto code = static_cast<uint32_t>(static_cast<unsigned char>(c));
        if (code < kFirstGlyph || code >= kFirstGlyph + kGlyphCount) return nullptr;
        return &glyphs[code - kFirstGlyph];
    }

    /// Advance width of `text` in pixels. Characters outside 32..126 are skipped.
    [[nodiscard]] float measure(const std::string& text) const {
        float width = 0.0f;
        for (char c : text) {
            if (const Glyph* g = glyph(c); g != nullptr) width += g->advance;
        }
        return width;
    }

    /// Advance width of the first `bytes` of `text`. What a text cursor's x offset is,
    /// and the reason `measure` is not enough on its own.
    [[nodiscard]] float measurePrefix(const std::string& text, size_t bytes) const {
        float width = 0.0f;
        for (size_t i = 0; i < bytes && i < text.size(); ++i) {
            if (const Glyph* g = glyph(text[i]); g != nullptr) width += g->advance;
        }
        return width;
    }

    [[nodiscard]] float lineHeight() const { return lineSpacing; }
    [[nodiscard]] float ascent() const { return ascentPx; }
};

} // namespace ui
