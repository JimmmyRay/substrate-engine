#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>

namespace gfx {

/**
 * @brief Read a rendered image back to host memory and write it as a PNG.
 *
 * Three free functions rather than a class, because there is no state to keep: the
 * staging buffer and the pending path live in `Renderer`, which is the only caller,
 * and everything here is a pure transformation of what it is handed.
 *
 * ## Why the swapchain image and not the G-buffer
 *
 * The roadmap asks for "screenshot any attachment". This captures exactly one image
 * -- whichever the caller hands it -- and `Renderer` hands it the swapchain. That is
 * not a narrowing, because the `DebugView` selector already routes every G-buffer
 * attachment through the lighting pass and onto the screen. Capturing the presented
 * image after selecting a debug view reaches albedo, normals, ORM, depth, cascades,
 * emissive and SSAO through one readback path instead of eight, and it captures the
 * *resolved* pixels rather than a multisampled image no PNG can represent.
 *
 * The one thing this deliberately cannot do is capture a target the debug views do
 * not expose. Adding one is a branch in `lighting_body.glsl`, not a second readback.
 */

/// Bytes one texel of `format` occupies in a tightly packed buffer copy, or 0 when
/// writeCapturePng() cannot interpret the format. Callers should treat 0 as "capture
/// is unavailable" and say so, rather than allocating a buffer nothing can decode.
uint32_t captureBytesPerPixel(VkFormat format);

/**
 * @brief Record a tightly packed copy of `image` into `dst`.
 *
 * `layout` is the image's layout on entry and is restored before the barrier returns,
 * so this can be dropped into a command buffer between two passes without the caller
 * tracking a layout change. The copy is a full barrier on both sides: a screenshot is
 * not a per-frame operation and there is nothing to overlap it with.
 */
/// `aspect`, `mip` and `layer` select one subresource. The defaults are the swapchain
/// case; a shadow cascade or a bloom mip is the same copy with three fields set.
void recordCaptureCopy(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkImageLayout layout, VkBuffer dst,
                       VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mip = 0,
                       uint32_t layer = 0);

/**
 * @brief Convert a tightly packed readback to 8-bit RGBA and write it as a PNG.
 *
 * No colour management: the swapchain formats this accepts are already sRGB-encoded
 * by the hardware on write, and PNG is sRGB by convention, so the bytes travel
 * unchanged. Alpha is forced opaque -- the tonemap pass writes 1.0 and a viewer
 * showing a translucent screenshot helps nobody.
 *
 * Creates parent directories. False means the format was unsupported or the write
 * failed, and the reason has already been logged.
 */
bool writeCapturePng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent, VkFormat format);

/**
 * @brief Bytes per texel for an *intermediate* render target format, or 0 if unhandled.
 *
 * Separate from captureBytesPerPixel because the two sets are disjoint: no swapchain is
 * RGBA16F, no render target is BGRA8, and merging them would produce one switch whose
 * cases nobody can attribute to a caller.
 */
uint32_t targetBytesPerPixel(VkFormat format);

/// What writeTargetPng() found in the data it wrote. The numbers are the point: an
/// image tells you a buffer has structure, and the range tells you whether the values
/// in it are the ones you expected.
struct TargetStats {
    float min = 0.0f;
    float max = 0.0f;
    /// Divisor applied to reach 0..255. 1.0 means the data already fitted in [0,1].
    float scale = 1.0f;
    bool wrote = false;
};

/**
 * @brief Write an arbitrary render target as a PNG, normalised so it is readable.
 *
 * **This does not have writeCapturePng's contract, deliberately.** That function is
 * byte-exact because golden-image comparison depends on it. This one *rescales*: an
 * RGBA16F target holds radiance well above 1.0 and a reverse-Z depth buffer holds
 * values crowded near one end, and clamping either to [0,1] produces a white rectangle
 * or a black one -- an image that proves only that the pass ran.
 *
 * So the mapping is min..max to 0..255 across the whole image, and the range that
 * produced it is returned and logged. That makes the picture qualitative and the log
 * line quantitative, which together answer the question actually being asked: is what
 * this pass wrote plausible? Never use these PNGs as goldens; they are not stable
 * across runs that change the range.
 *
 * A single-component format (depth, and the R of an RG16F LUT) is written as greyscale
 * rather than red, because a depth buffer read as a red channel is much harder to
 * judge by eye.
 */
TargetStats writeTargetPng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent, VkFormat format);

/**
 * @brief Outcome of comparing a capture against a golden image (5.3).
 *
 * `matched` is the verdict the caller should act on. The rest is what makes a failure
 * diagnosable rather than merely reported: which pixel was worst and by how much
 * separates "the shader broke" from "a bilinear tap moved by one unit".
 */
struct CompareResult {
    bool matched = false;
    /// False when the golden file is missing or unreadable -- distinct from a
    /// mismatch, because the answer is "write one", not "something regressed".
    bool goldenLoaded = false;
    /// Set when the two images differ in size. No resampling is attempted: a size
    /// mismatch means the capture was taken at a different resolution, and comparing
    /// through a resize would turn a configuration error into a plausible-looking
    /// diff.
    bool sizeMismatch = false;

    uint32_t width = 0;
    uint32_t height = 0;
    /// Pixels where any channel differs by more than the tolerance.
    uint64_t differingPixels = 0;
    /// Largest single-channel absolute difference anywhere in the image, 0..255.
    uint32_t maxChannelDelta = 0;
    /// Mean absolute per-channel difference over the whole image, 0..255. The number
    /// to quote: a max of 3 on one pixel and a max of 3 everywhere are very different
    /// regressions and this is what separates them.
    double meanChannelDelta = 0.0;
    /// Where maxChannelDelta was found, for a screenshot viewer to be pointed at.
    uint32_t worstX = 0;
    uint32_t worstY = 0;
};

/**
 * @brief Compare a PNG against a golden PNG (5.3).
 *
 * `tolerance` is a per-channel absolute difference, 0..255, below which a pixel counts
 * as matching. Not zero by default at the call site: the renderer is deterministic in
 * the sense that the same build produces the same image, but a driver update or a
 * different GPU will move the last bit of a filtered tap, and a comparison that fails
 * on that is one nobody leaves enabled.
 *
 * `maxDifferingPixels` is the second half of the verdict. A handful of pixels moving
 * by one unit is noise; the same delta across the frame is a regression, and only a
 * count can tell them apart.
 *
 * When `diffPath` is non-empty a difference image is written there: matching pixels
 * darkened, differing pixels in red scaled by the size of the difference. That image
 * is the whole point of the feature -- a boolean tells you something broke, a diff
 * tells you which pass.
 */
CompareResult comparePng(const std::filesystem::path& capturePath, const std::filesystem::path& goldenPath,
                         uint32_t tolerance, uint64_t maxDifferingPixels,
                         const std::filesystem::path& diffPath = {});

/**
 * @brief The readback test (P2): is a texel authored the texel presented?
 *
 * Expands `source` by an integer `scale` and holds it against the rectangle of `capture`
 * at (`x`, `y`), **bit-exact**. Not a variant of `comparePng` and deliberately not built
 * on it, for two reasons that are the whole character of this check:
 *
 * - **The expected image is computed from the input**, not read from a reference. When
 *   this fails there is nothing to re-snap against, which is what makes it a proof rather
 *   than a regression check -- and it is the standard the P arc exists to hold itself to.
 * - **It compares a rectangle, not a frame.** The image occupies a corner of a swapchain
 *   that also holds whatever else the frame drew, and comparing the whole surface would
 *   report the scene as several hundred thousand differing pixels. What is being asserted
 *   is about the texels of the source and nothing else.
 *
 * **Expanded, not resampled.** Each source texel becomes a `scale` by `scale` block of
 * exactly its own bytes -- which is what a `VK_FILTER_NEAREST` blit onto a destination
 * that is a whole multiple of its source does, written a second time on purpose. A check
 * that shared an implementation with the thing it checks would agree with it about any bug
 * the two had in common.
 *
 * The source must be **opaque**. Alpha is blended in the framebuffer, in linear space, and
 * reproducing that here would put a rounding step between the file and the expectation --
 * in the one check that exists not to have any.
 *
 * `expectedPath` receives the expanded block and `diffPath` the region's diff, both for a
 * human rather than for the verdict: a count says something moved and an image says what.
 * `tolerance` is fixed at zero, so `differingPixels` is exactly the number of texels that
 * did not survive, and `matched` is true only when it is none.
 *
 * **`srcRect` selects a rectangle of the source, and P5 is why it exists.** A sprite sheet
 * draws one cell, so the file the expectation is computed from is right and the *region*
 * of it is not the whole file -- and cropping the source here rather than writing a cell
 * out to a second PNG keeps the property that makes this a proof: there is exactly one
 * authored file, and both the engine and the check read it. `{0, 0, 0, 0}`, the default,
 * is the whole image and is what every P2 and P4 case passes.
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
    /// legitimately shade to the colour that was behind it -- but the box below is.
    uint64_t insideDiffering = 0;
    uint64_t expectedCovered = 0;
    uint32_t diffX0 = 0, diffY0 = 0, diffX1 = 0, diffY1 = 0;
    uint32_t maskX0 = 0, maskY0 = 0, maskX1 = 0, maskY1 = 0;
    /// The first pixel outside the mask that differed, for a message worth reading.
    uint32_t worstX = 0, worstY = 0;
};

/**
 * @brief The lit path's check, and the exception the P arc owes itself (P6).
 *
 * The arc's standard is *a texel authored is a texel presented*, read back and compared
 * against the source file. **A lit sprite cannot satisfy it and no design makes it
 * possible**: it goes through the G-buffer, the lighting pass and the tonemapper, and every
 * one of those is a correction applied to the value in the file. That is the reason to use
 * the path, not a defect in it.
 *
 * Re-snapping is not the alternative. `docs/architecture/tooling.md` forbids
 * `scripts/golden.sh snap`, and a golden case for a lit sprite would be a picture somebody
 * accepted -- the standard this arc exists not to use.
 *
 * **So the claim moves from the value to the coverage.** Lighting changes what a pixel is;
 * it cannot change *which* pixels the sprite covers. The silhouette of an alpha-cutout
 * sprite is decided by the source file's alpha, the cutoff, the pivot, the texel rect, the
 * quad, the projection and the viewport transform -- every place a half-texel is lost -- and
 * it is computable from the file rather than snapped from a run.
 *
 * Two properties, and both are needed:
 *
 * 1. **Outside the expected silhouette, `capture` and `background` are bit-identical.** Zero
 *    tolerance, zero pixels allowed. A sprite a texel too wide, a texel offset, mirrored,
 *    rotated or showing the wrong cell puts a differing pixel outside the mask.
 * 2. **The bounding box of the differing set is exactly the mask's bounding box.** Property
 *    1 alone is satisfied by a sprite that drew nothing at all, and by one that came out too
 *    small. This is what refuses both.
 *
 * `background` must be the same frame with the sprite's fragments discarded -- which is what
 * `--readback-lit-cutoff 2` produces, leaving the material, the instance and the draw call
 * identical so the two runs differ in the fragments and nothing else. The pair also has to
 * run with the effects that bleed across a silhouette off (`--no-bloom --no-ssao --no-ssr`),
 * because bleed outside the mask is a real difference and property 1 is right to fail on it.
 *
 * @param cutoff the alpha at or above which a source texel is covered, 0..1.
 */
SilhouetteResult compareSilhouette(const std::filesystem::path& capturePath,
                                   const std::filesystem::path& backgroundPath,
                                   const std::filesystem::path& source, uint32_t scale, int32_t x, int32_t y,
                                   float cutoff, const std::filesystem::path& diffPath = {});

} // namespace gfx
