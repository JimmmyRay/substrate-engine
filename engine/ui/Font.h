#pragma once

#include "gfx/Resources.h"
#include "ui/FontMetrics.h"

#include <cstdint>
#include <string>

namespace ui {

constexpr uint32_t kArtRows = 9; ///< Rows per glyph in the embedded bitmap font

/// The embedded font, defined in FontData.cpp. Seven rows above the baseline plus a
/// two-row descender.
extern const char* const kGlyphArt[kGlyphCount][kArtRows];

/**
 * @brief An R8 glyph atlas and its glyph table.
 *
 * Two fills, one atlas. The embedded 8x16 bitmap font is always available and needs
 * no asset on disk; a TTF rasterised through stb_truetype replaces it when one is
 * configured. Everything downstream -- upload, measurement, quad emission, the
 * pipeline -- is written once against the result.
 *
 * There is no glyph cache and no dynamic repacking: the character set is 95 glyphs
 * fixed at init, which is all a debug HUD and a settings panel need.
 *
 * The *numbers* live in `FontMetrics`, in a header with no Vulkan in it, so that
 * measuring a string does not require a device -- see the note there. This class is the
 * atlas plus the two fills that produce one.
 */
class Font {
  public:
    /// Non-copyable; see the note on `gfx::Uploader` in gfx/Resources.h. The atlas is a
    /// `GpuImage` this owns.
    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    /**
     * @param ttfPath     TrueType file to rasterise; empty selects the embedded font.
     *                    A path that fails to load or bake warns and falls back.
     * @param pixelHeight Rasterisation height for the TTF path. Ignored otherwise --
     *                    the embedded font is a bitmap and only exists at 16 px.
     */
    /// A TTF that cannot be read or baked falls back to the embedded bitmap font and
    /// warns, so there is no failure to report. `ready()` is the query that matters.
    void init(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::string& ttfPath, float pixelHeight);
    void shutdown(const gfx::VulkanContext& ctx);

    VkImageView view() const { return atlas.view; }
    bool ready() const { return atlas.view != VK_NULL_HANDLE; }

    /// Everything that is arithmetic rather than memory. Handed out by reference so the
    /// UI can hold it without holding the atlas.
    const FontMetrics& metrics() const { return fontMetrics; }

    // Kept as thin forwards because the overlay has called them since 0.8, and renaming
    // forty call sites to prove a point about indirection is not an improvement.
    float lineHeight() const { return fontMetrics.lineSpacing; }
    float ascent() const { return fontMetrics.ascentPx; }
    float measure(const std::string& text) const { return fontMetrics.measure(text); }
    const Glyph* glyph(char c) const { return fontMetrics.glyph(c); }

  private:
    /// @param magnify whole-number texel replication, so the embedded bitmap font can
    ///        follow the DPI scale (S6.5). Whole numbers only: NEAREST sampling makes an
    ///        integer magnification exact, and anything else blurry.
    void fillFromBitmap(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t magnify);
    bool fillFromTtf(const std::string& path, float pixelHeight, std::vector<uint8_t>& pixels, uint32_t& width,
                     uint32_t& height);
    /// Append the solid block `FontMetrics::whiteU/V` point at, growing `pixels` by a
    /// few rows. Called for both fills, because a rect has to work whichever font is
    /// loaded -- see FontMetrics.h for why four texels replace a whole pipeline.
    void reserveWhiteBlock(std::vector<uint8_t>& pixels, uint32_t width, uint32_t& height);

    gfx::GpuImage atlas;
    FontMetrics fontMetrics;
};

} // namespace ui
