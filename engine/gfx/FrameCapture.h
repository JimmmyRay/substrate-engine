#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>

namespace gfx {

/**
 * @file
 * Reading a rendered image back to host memory, writing it as a PNG, and the
 * comparisons the golden and readback suites make against one.
 */

/// Bytes one texel of `format` occupies in a tightly packed buffer copy, or 0 when
/// writeCapturePng() cannot interpret the format. A caller that allocates on a 0 gets a
/// buffer nothing can decode.
uint32_t captureBytesPerPixel(VkFormat format);

/**
 * @brief Record a tightly packed copy of `image` into `dst`.
 *
 * `layout` is the image's layout on entry and is restored before returning, so a caller
 * dropping this between two passes tracks no layout change of its own. `aspect`, `mip`
 * and `layer` select one subresource; the defaults are the swapchain case.
 */
void recordCaptureCopy(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkImageLayout layout, VkBuffer dst,
                       VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mip = 0,
                       uint32_t layer = 0);

/**
 * @brief Convert a tightly packed readback to 8-bit RGBA and write it as a PNG.
 *
 * Byte-exact, because golden comparison depends on it: the accepted swapchain formats
 * are sRGB-encoded by the hardware on write and PNG is sRGB by convention, so applying
 * any transfer function here would break every golden. Alpha is forced opaque.
 *
 * Creates parent directories. False means the format was unsupported or the write
 * failed, and the reason has already been logged.
 */
bool writeCapturePng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent, VkFormat format);

/// @brief Bytes per texel for an *intermediate* render target format, or 0 if unhandled.
uint32_t targetBytesPerPixel(VkFormat format);

/// @brief The value range writeTargetPng() found in the data it wrote.
struct TargetStats {
    float min = 0.0f;
    float max = 0.0f;
    /// Divisor applied to reach 0..255. 1.0 means the data already fitted in [0,1].
    float scale = 1.0f;
    bool wrote = false;
};

/**
 * @brief Write an arbitrary render target as a PNG, min..max rescaled to 0..255 across
 *        the whole image so an HDR or reverse-Z buffer is readable.
 *
 * **Never usable as a golden**, unlike writeCapturePng: the scale is derived from the
 * data, so a run that changes the range changes every byte in the file. The range that
 * produced the image is returned so the picture can be read quantitatively.
 */
TargetStats writeTargetPng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent, VkFormat format);

/// @brief Outcome of comparing a capture against a golden image.
struct CompareResult {
    bool matched = false;
    /// False when the golden file is missing or unreadable -- a caller that treats this
    /// as a mismatch reports a regression where the answer is "write one".
    bool goldenLoaded = false;
    /// The two images differ in size, and nothing was resampled: comparing through a
    /// resize turns a resolution mismatch into a plausible-looking diff.
    bool sizeMismatch = false;

    uint32_t width = 0;
    uint32_t height = 0;
    /// Pixels where any channel differs by more than the tolerance.
    uint64_t differingPixels = 0;
    /// Largest single-channel absolute difference anywhere in the image, 0..255.
    uint32_t maxChannelDelta = 0;
    /// Mean absolute per-channel difference over the whole image, 0..255.
    double meanChannelDelta = 0.0;
    /// Where maxChannelDelta was found.
    uint32_t worstX = 0;
    uint32_t worstY = 0;
};

/**
 * @brief Compare a PNG against a golden PNG.
 *
 * `tolerance` is a per-channel absolute difference, 0..255, below which a pixel counts as
 * matching, and `maxDifferingPixels` how many such pixels the verdict survives. Both are
 * non-zero at the call sites on purpose: a driver update or a different GPU moves the last
 * bit of a filtered tap, and a suite that fails on that is one nobody leaves enabled.
 *
 * A non-empty `diffPath` receives a difference image -- matching pixels darkened,
 * differing ones in red scaled by the size of the difference.
 */
CompareResult comparePng(const std::filesystem::path& capturePath, const std::filesystem::path& goldenPath,
                         uint32_t tolerance, uint64_t maxDifferingPixels,
                         const std::filesystem::path& diffPath = {});

/**
 * @brief Expand `source` by an integer `scale` and hold it against the rectangle of the
 *        capture at (`x`, `y`), bit-exact and at zero tolerance.
 *
 * Deliberately not built on `comparePng` and deliberately not sharing the engine's blit:
 * the expectation is recomputed from the source file here, so a check that reused either
 * would agree with the code under test about any bug the two had in common. The nearest-
 * neighbour expansion is written out a second time for that reason -- do not replace it
 * with a call into the presentation path.
 *
 * The source must be **opaque**. Framebuffer alpha blending happens in linear space, and
 * reproducing it here would introduce the rounding this check exists not to have.
 *
 * `expectedPath` and `diffPath` are written for a human; neither affects the verdict.
 * `srcRect` crops the source to one sprite-sheet cell so a single authored file still
 * feeds both the engine and the check; `{0, 0, 0, 0}` is the whole image.
 */
struct ReadbackRect {
    uint32_t x = 0;
    uint32_t y = 0;
    /// Zero in either axis means the whole source, from (0, 0).
    uint32_t width = 0;
    uint32_t height = 0;
};

CompareResult compareReadback(const std::filesystem::path& capturePath, const std::filesystem::path& source,
                              uint32_t scale, int32_t x, int32_t y, const std::filesystem::path& expectedPath = {},
                              const std::filesystem::path& diffPath = {}, const ReadbackRect& srcRect = {});

/// What `compareSilhouette` found. Boxes are half-open in capture coordinates.
struct SilhouetteResult {
    bool loaded = false;
    bool sizeMismatch = false;
    bool matched = false;
    /// Pixels that differ from the background where the sprite does not cover. Must be 0.
    uint64_t outsideDiffering = 0;
    /// Pixels that differ where it does. Not a verdict on its own -- a lit sprite may
    /// legitimately shade to the colour behind it -- but the boxes below are.
    uint64_t insideDiffering = 0;
    uint64_t expectedCovered = 0;
    uint32_t diffX0 = 0, diffY0 = 0, diffX1 = 0, diffY1 = 0;
    uint32_t maskX0 = 0, maskY0 = 0, maskX1 = 0, maskY1 = 0;
    /// The first pixel outside the mask that differed.
    uint32_t worstX = 0, worstY = 0;
};

/**
 * @brief Assert an alpha-cutout sprite's *coverage* rather than its values: which pixels
 *        it covers is computable from the source file, what colour they end up is not.
 *
 * Both halves of the verdict are needed. Outside the expected silhouette `capture` and
 * `background` must be bit-identical, which catches a sprite a texel too wide, offset,
 * mirrored or showing the wrong cell; and the differing set's bounding box must equal the
 * mask's, which is what refuses a sprite that drew nothing or came out too small.
 *
 * `background` must be the same frame with only the sprite's fragments discarded --
 * `--readback-lit-cutoff 2` -- so the two runs differ in the fragments and nothing else,
 * and both must run with the effects that bleed across a silhouette off
 * (`--no-bloom --no-ssao --no-ssr`) or the outside-the-mask half legitimately fails.
 *
 * @param cutoff the alpha at or above which a source texel is covered, 0..1.
 */
SilhouetteResult compareSilhouette(const std::filesystem::path& capturePath,
                                   const std::filesystem::path& backgroundPath,
                                   const std::filesystem::path& source, uint32_t scale, int32_t x, int32_t y,
                                   float cutoff, const std::filesystem::path& diffPath = {});

} // namespace gfx
