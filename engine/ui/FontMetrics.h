#pragma once

#include <cstdint>
#include <string>

/**
 * @file FontMetrics.h
 * @brief The half of the font that is numbers, and the vertex everything draws through.
 *
 * Separate from `ui/Font.h` because `Font` owns a `GpuImage`. Add a Vulkan include here and
 * everything that merely measures a string follows this header out of the hosted set, taking
 * the layout code beyond the unit suite's reach.
 */
namespace ui {

constexpr uint32_t kFirstGlyph = 32; ///< ASCII space
constexpr uint32_t kGlyphCount = 95; ///< ASCII 32..126 inclusive

/**
 * @brief The font atlas's place in the overlay's image array.
 *
 * Zero is load-bearing twice: it is `DrawVertex::texture`'s default, and it is the one slot
 * `overlay.frag` reads as R8 coverage rather than as colour. Move the atlas off it and every
 * vertex that says nothing about textures samples an image, tinted by nothing.
 */
constexpr uint32_t kFontAtlasSlot = 0;

/**
 * @brief One glyph's quad, in stb_truetype's baked convention.
 *
 * The pen sits on the baseline and y increases downward, so `y0` is negative for everything
 * with ink above the baseline. The embedded font is baked to this same convention; break that
 * and one emitter can no longer draw both.
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
 * `gfx::Renderer`'s `OverlayVertex` is an alias of this, so changing the layout here changes
 * the debug HUD's and the binding menu's too. The `static_assert` below pins it to
 * `overlay.vert`.
 */
struct DrawVertex {
    float x = 0.0f, y = 0.0f;
    float u = 0.0f, v = 0.0f;
    uint32_t rgba = 0xFFFFFFFFu; ///< R8G8B8A8_UNORM, little-endian: 0xAABBGGRR

    /// Which image in the overlay's array to sample.
    uint32_t texture = kFontAtlasSlot;
};

static_assert(sizeof(DrawVertex) == 24, "DrawVertex must match overlay.vert's attributes");

/// Pack a colour the way `DrawVertex::rgba` wants it, from 0..1 components. Clamped, never
/// wrapped: a caller that computed 1.2 meant white, and wrapping gives it a dark red.
inline uint32_t packColor(float r, float g, float b, float a = 1.0f) {
    const auto ch = [](float value) {
        const float c = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return ch(r) | (ch(g) << 8) | (ch(b) << 16) | (ch(a) << 24);
}

/// Blend two packed colours, `t` of the way from `a` to `b`.
[[nodiscard]] uint32_t mixColor(uint32_t a, uint32_t b, float t);

/**
 * @brief Glyph table and vertical metrics, plus the one texel that is not a glyph.
 *
 * `whiteU`/`whiteV` point into a solid block the atlas reserves. `overlay.frag` emits
 * `vColor.rgb, vColor.a * coverage`, so a quad on a full-coverage texel *is* a filled rectangle
 * -- which is what puts a widget's background and its label in one buffer, in draw order, on
 * one pipeline.
 */
struct FontMetrics {
    Glyph glyphs[kGlyphCount]{};
    float lineSpacing = 16.0f;
    float ascentPx = 10.0f;
    /// Centre of the reserved solid block. Sampled with NEAREST, so this must land
    /// squarely inside it rather than on its edge.
    float whiteU = 0.0f;
    float whiteV = 0.0f;

    /// Null for anything outside 32..126.
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

    /// Advance width of the first `bytes` of `text`, in pixels. What a text cursor's x is.
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
