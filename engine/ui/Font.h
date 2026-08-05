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
 * Two fills -- the embedded 8x16 bitmap, or a TTF through stb_truetype -- into one atlas, so
 * everything downstream is written once. The character set is fixed at init; there is no
 * repacking, so a glyph outside 32..126 cannot be added later.
 *
 * The numbers live in `FontMetrics`, in a header with no Vulkan in it. Measure through that,
 * not this, or measuring a string starts requiring a device.
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
    /// Never fails: a TTF that cannot be read or baked warns and falls back to the embedded
    /// font. `ready()` is the query that matters.
    void init(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::string& ttfPath, float pixelHeight);
    void shutdown(const gfx::VulkanContext& ctx);

    VkImageView view() const { return atlas.view; }
    bool ready() const { return atlas.view != VK_NULL_HANDLE; }

    /// Handed out by reference so the UI can hold the metrics without holding the atlas.
    const FontMetrics& metrics() const { return fontMetrics; }

    float lineHeight() const { return fontMetrics.lineSpacing; }
    float ascent() const { return fontMetrics.ascentPx; }
    float measure(const std::string& text) const { return fontMetrics.measure(text); }
    const Glyph* glyph(char c) const { return fontMetrics.glyph(c); }

  private:
    /// @param magnify whole-number texel replication, so the embedded bitmap font can
    ///        follow the DPI scale. Whole numbers only: NEAREST sampling makes an
    ///        integer magnification exact, and anything else blurry.
    void fillFromBitmap(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t magnify);
    bool fillFromTtf(const std::string& path, float pixelHeight, std::vector<uint8_t>& pixels, uint32_t& width,
                     uint32_t& height);
    /// Append the solid block `FontMetrics::whiteU/V` point at, growing `pixels` by a few
    /// rows. Must run for both fills, or rectangles stop drawing under one of the two fonts.
    void reserveWhiteBlock(std::vector<uint8_t>& pixels, uint32_t width, uint32_t& height);

    gfx::GpuImage atlas;
    FontMetrics fontMetrics;
};

} // namespace ui
