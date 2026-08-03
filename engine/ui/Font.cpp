#include "ui/Font.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/VulkanContext.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace ui {

namespace {

// ------------------------------------------------------- embedded bitmap font
// Each glyph occupies an 8x16 cell in the atlas. The 5x9 art from FontData.cpp is
// inset one column from the left, and its top row lands 7 rows below the top of the
// cell, which puts the baseline at cell row 10:
//
//   cell rows 0..2    leading
//   cell rows 3..9    art rows 0..6, everything above the baseline
//   cell row  10      the baseline itself; art row 7 is the first descender row
//   cell rows 10..11  art rows 7..8, the descender
//   cell rows 12..15  trailing
//
// The whole cell is emitted as the quad, so the blank margins are drawn too. They
// are zero-coverage texels, so blending discards them and the overlap between an
// 8-wide cell and a 6-pixel advance costs nothing but a few fragments.
constexpr uint32_t kCellWidth = 8;
constexpr uint32_t kCellHeight = 16;
constexpr uint32_t kArtCols = 5;
constexpr uint32_t kArtOriginX = 1;
constexpr uint32_t kArtOriginY = 3;
constexpr uint32_t kBaselineRow = 10;
constexpr float kBitmapAdvance = 6.0f;

/// Atlas dimensions for the TTF path. 512x512 holds 95 glyphs well past 48 px.
constexpr uint32_t kTtfAtlasSize = 512;

/// Side of the solid block appended below the glyphs (S6.1). Four rather than one so
/// that the texel `whiteU`/`whiteV` name is surrounded by its own kind, which costs 3 KB
/// and removes any question about rounding at the edge.
constexpr uint32_t kWhiteBlock = 4;

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    const std::streamsize size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) return {};
    return data;
}

} // namespace

void Font::fillFromBitmap(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t magnify) {
    // **Integer magnification, and only integer** (S6.5). The embedded font is a bitmap
    // that exists at exactly one size, so a UI at 200% would otherwise be twice as big
    // around text that stayed 16 px -- which is the "correct at one resolution and wrong
    // at another" the row is about. Replicating each texel N times and sampling NEAREST
    // is *exact*: every output pixel is a source pixel, so a doubled glyph is as crisp as
    // the original. A fractional scale would not be, which is why this rounds to a whole
    // number rather than taking the DPI scale as given -- a blurry bitmap font would be
    // worse than a small one.
    const uint32_t n = std::max(magnify, 1u);
    const uint32_t cellWidth = kCellWidth * n;
    const uint32_t cellHeight = kCellHeight * n;

    width = kGlyphCount * cellWidth;
    height = cellHeight;
    pixels.assign(static_cast<size_t>(width) * height, 0);

    for (uint32_t g = 0; g < kGlyphCount; ++g) {
        const uint32_t cellX = g * cellWidth;

        for (uint32_t r = 0; r < kArtRows; ++r) {
            const char* row = kGlyphArt[g][r];
            // Length-checked rather than assumed: a five-character row is a hand-typed
            // invariant across 855 strings, and one short row would otherwise read off
            // the end of a string literal.
            const size_t len = std::strlen(row);
            for (uint32_t c = 0; c < kArtCols && c < len; ++c) {
                if (row[c] != '#') continue;
                const uint32_t x = (kArtOriginX + c) * n;
                const uint32_t y = (kArtOriginY + r) * n;
                for (uint32_t dy = 0; dy < n; ++dy) {
                    for (uint32_t dx = 0; dx < n; ++dx) {
                        pixels[static_cast<size_t>(y + dy) * width + cellX + x + dx] = 0xFF;
                    }
                }
            }
        }

        const float u0 = static_cast<float>(cellX) / static_cast<float>(width);
        const float u1 = static_cast<float>(cellX + cellWidth) / static_cast<float>(width);

        const auto scale = static_cast<float>(n);
        fontMetrics.glyphs[g].x0 = 0.0f;
        fontMetrics.glyphs[g].y0 = -static_cast<float>(kBaselineRow) * scale;
        fontMetrics.glyphs[g].x1 = static_cast<float>(cellWidth);
        fontMetrics.glyphs[g].y1 = static_cast<float>(kCellHeight - kBaselineRow) * scale;
        fontMetrics.glyphs[g].s0 = u0;
        fontMetrics.glyphs[g].t0 = 0.0f;
        fontMetrics.glyphs[g].s1 = u1;
        fontMetrics.glyphs[g].t1 = 1.0f;
        fontMetrics.glyphs[g].advance = kBitmapAdvance * scale;
    }

    fontMetrics.lineSpacing = static_cast<float>(cellHeight);
    fontMetrics.ascentPx = static_cast<float>(kBaselineRow) * static_cast<float>(n);
}

bool Font::fillFromTtf(const std::string& path, float pixelHeight, std::vector<uint8_t>& pixels, uint32_t& width,
                       uint32_t& height) {
    const std::vector<uint8_t> ttf = readFile(path);
    if (ttf.empty()) {
        core::Logger::warn(core::LogCategory::Asset, "Debug font: cannot read %s; using the embedded bitmap font", path.c_str());
        return false;
    }

    width = kTtfAtlasSize;
    height = kTtfAtlasSize;
    pixels.assign(static_cast<size_t>(width) * height, 0);

    stbtt_bakedchar baked[kGlyphCount]{};
    const int rows = stbtt_BakeFontBitmap(ttf.data(), 0, pixelHeight, pixels.data(), static_cast<int>(width),
                                          static_cast<int>(height), static_cast<int>(kFirstGlyph),
                                          static_cast<int>(kGlyphCount), baked);
    if (rows <= 0) {
        // A negative return is the index of the glyph that did not fit, so the atlas is
        // partially baked and unusable rather than merely truncated.
        core::Logger::warn(core::LogCategory::Asset, "Debug font: %s did not fit a %ux%u atlas at %.1f px; using the embedded font",
                     path.c_str(), width, height, static_cast<double>(pixelHeight));
        return false;
    }

    for (uint32_t g = 0; g < kGlyphCount; ++g) {
        const stbtt_bakedchar& b = baked[g];
        fontMetrics.glyphs[g].x0 = b.xoff;
        fontMetrics.glyphs[g].y0 = b.yoff;
        fontMetrics.glyphs[g].x1 = b.xoff + static_cast<float>(b.x1 - b.x0);
        fontMetrics.glyphs[g].y1 = b.yoff + static_cast<float>(b.y1 - b.y0);
        fontMetrics.glyphs[g].s0 = static_cast<float>(b.x0) / static_cast<float>(width);
        fontMetrics.glyphs[g].t0 = static_cast<float>(b.y0) / static_cast<float>(height);
        fontMetrics.glyphs[g].s1 = static_cast<float>(b.x1) / static_cast<float>(width);
        fontMetrics.glyphs[g].t1 = static_cast<float>(b.y1) / static_cast<float>(height);
        fontMetrics.glyphs[g].advance = b.xadvance;
    }

    // Vertical metrics come from the face, not from the baked rectangles: the tallest
    // glyph that happens to be in ASCII is not the font's ascent.
    stbtt_fontinfo info{};
    if (stbtt_InitFont(&info, ttf.data(), 0) != 0) {
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        const float scale = stbtt_ScaleForPixelHeight(&info, pixelHeight);
        fontMetrics.ascentPx = static_cast<float>(ascent) * scale;
        fontMetrics.lineSpacing = static_cast<float>(ascent - descent + lineGap) * scale;
    } else {
        fontMetrics.ascentPx = pixelHeight * 0.8f;
        fontMetrics.lineSpacing = pixelHeight * 1.2f;
    }

    core::Logger::status(core::LogCategory::Asset, "Debug font: %s at %.1f px (%d atlas rows used)", path.c_str(),
                   static_cast<double>(pixelHeight), rows);
    return true;
}

void Font::reserveWhiteBlock(std::vector<uint8_t>& pixels, uint32_t width, uint32_t& height) {
    // Appended rather than packed into a gap. Both fills leave unused texels -- the
    // bitmap font's cell margins, the TTF's unbaked rows -- but "unused" is a property of
    // the fill rather than of the atlas, and a block placed in a gap one of them happens
    // to have is a block the other one overwrites. Four rows at the bottom cost 3 KB and
    // are correct for both.
    const uint32_t oldHeight = height;
    height += kWhiteBlock;
    pixels.resize(static_cast<size_t>(width) * height, 0);

    for (uint32_t y = oldHeight; y < height; ++y) {
        for (uint32_t x = 0; x < kWhiteBlock && x < width; ++x) {
            pixels[static_cast<size_t>(y) * width + x] = 0xFF;
        }
    }

    // Every glyph's t was normalised against the old height, so growing the atlas moves
    // all of them. Rescaled here rather than by threading the final height through both
    // fills, which would make each of them care about a decision neither one takes.
    const float scale = static_cast<float>(oldHeight) / static_cast<float>(height);
    for (Glyph& g : fontMetrics.glyphs) {
        g.t0 *= scale;
        g.t1 *= scale;
    }

    // The centre of a texel, not its corner: the sampler is NEAREST, so a coordinate on
    // the boundary between the block and the zero texel beside it is a coin flip that
    // renders as an invisible rectangle on some drivers and a solid one on others.
    fontMetrics.whiteU = 1.5f / static_cast<float>(width);
    fontMetrics.whiteV = (static_cast<float>(oldHeight) + 1.5f) / static_cast<float>(height);
}

void Font::init(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::string& ttfPath,
                float pixelHeight) {
    auto s = core::Profiler::scope("Font::init");

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;

    if (ttfPath.empty() || !fillFromTtf(ttfPath, pixelHeight, pixels, width, height)) {
        // The requested height, rounded to a whole multiple of the bitmap's own 16 px.
        // A caller asking for 32 gets a crisp 2x; one asking for 24 gets 1x rather than a
        // blurred 1.5x, which is the trade this font is worth making.
        const auto magnify = static_cast<uint32_t>(std::lround(pixelHeight / static_cast<float>(kCellHeight)));
        fillFromBitmap(pixels, width, height, std::max(magnify, 1u));
    }
    reserveWhiteBlock(pixels, width, height);

    atlas = gfx::createImage(ctx, {width, height}, VK_FORMAT_R8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1);
    uploader.uploadImageWithMips(ctx, atlas, pixels.data(), pixels.size());

    core::Logger::status(core::LogCategory::Render, "Font atlas %ux%u R8 (%u glyphs, %.0f px line height, solid block at %.4f,%.4f)",
                   width, height, kGlyphCount, static_cast<double>(fontMetrics.lineSpacing),
                   static_cast<double>(fontMetrics.whiteU), static_cast<double>(fontMetrics.whiteV));
}

void Font::shutdown(const gfx::VulkanContext& ctx) { gfx::destroyImage(ctx, atlas); }

} // namespace ui
