#include "gfx/FrameCapture.h"

#include "core/Logger.h"
#include "gfx/Resources.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>

namespace gfx {

namespace {

/// Create the parent directory of `path`, if it has one. A capture into
/// `debug_frames/shot.png` on a fresh checkout would otherwise fail at the write with
/// an errno nobody reads, and the directory is gitignored so it is routinely absent.
bool ensureParentDir(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec && !std::filesystem::is_directory(parent)) {
        core::Logger::error(core::LogCategory::Render, "Capture: cannot create %s: %s", parent.string().c_str(),
                      ec.message().c_str());
        return false;
    }
    return true;
}

/// Expand one packed texel to RGBA8. `bgra` and the 10-bit unpack are the only two
/// transformations any swapchain format on this hardware needs; everything else is a
/// straight copy.
struct Rgba8 {
    uint8_t r, g, b, a;
};

Rgba8 decodeTexel(const uint8_t* src, VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return {src[0], src[1], src[2], 255};

    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return {src[2], src[1], src[0], 255};

    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
        uint32_t packed = 0;
        std::memcpy(&packed, src, sizeof(packed));
        // 10 bits to 8 is a shift, not a rescale: 1023 >> 2 is 255, so the endpoints
        // land where they should and no channel gains a bias.
        const uint8_t c0 = static_cast<uint8_t>((packed >> 2) & 0xFFu);
        const uint8_t c1 = static_cast<uint8_t>((packed >> 12) & 0xFFu);
        const uint8_t c2 = static_cast<uint8_t>((packed >> 22) & 0xFFu);
        // A2B10G10R10 packs R in the low bits, A2R10G10B10 packs B there.
        if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) return {c0, c1, c2, 255};
        return {c2, c1, c0, 255};
    }

    default: return {0, 0, 0, 255};
    }
}

uint16_t readU16(const uint8_t* src) {
    uint16_t v = 0;
    std::memcpy(&v, src, sizeof(v));
    return v;
}

/// IEEE half to float. Written out rather than pulled from a library because the only
/// alternative in this build is glm's, and dragging glm into a file that otherwise
/// knows nothing about maths to save fifteen lines is a worse trade than the fifteen
/// lines. Denormals are handled by the exponent==0 branch; NaN and infinity pass
/// through as float NaN and infinity and are filtered by the caller's range scan.
float halfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h >> 15) << 31;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;

    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign; // signed zero
        } else {
            // Denormal: renormalise by shifting the mantissa up until the implicit bit
            // appears, decrementing the exponent for each shift.
            uint32_t e = 0;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                ++e;
            }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13); // infinity or NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }

    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// Decode one texel of an intermediate render target to linear floats. Separate from
/// decodeTexel because the formats are disjoint -- no swapchain is RGBA16F and no
/// render target is BGRA8 -- and because these have to stay in float to be normalised.
/// `count` is how many of the four components the format actually carries; the rest are
/// left at zero so the caller does not average in channels that do not exist.
void decodeTargetTexel(const uint8_t* src, VkFormat format, float out[4], uint32_t& count) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        count = 4;
        for (uint32_t c = 0; c < 4; ++c) out[c] = halfToFloat(readU16(src + c * 2));
        break;
    case VK_FORMAT_R16G16_SFLOAT:
        count = 2;
        for (uint32_t c = 0; c < 2; ++c) out[c] = halfToFloat(readU16(src + c * 2));
        break;
    case VK_FORMAT_D32_SFLOAT:
        count = 1;
        std::memcpy(&out[0], src, sizeof(float));
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        count = 4;
        for (uint32_t c = 0; c < 4; ++c) out[c] = static_cast<float>(src[c]) / 255.0f;
        break;
    default: count = 0; break;
    }
}

/// Load a PNG as RGBA8. Empty on failure, with the reason logged.
std::vector<uint8_t> loadPng(const std::filesystem::path& path, uint32_t& width, uint32_t& height) {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        core::Logger::error(core::LogCategory::Render, "Compare: cannot read %s: %s", path.string().c_str(), stbi_failure_reason());
        return {};
    }

    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    std::memcpy(out.data(), pixels, out.size());
    stbi_image_free(pixels);
    return out;
}

} // namespace

uint32_t captureBytesPerPixel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return 4;
    default: return 0;
    }
}

void recordCaptureCopy(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkImageLayout layout, VkBuffer dst,
                       VkImageAspectFlags aspect, uint32_t mip, uint32_t layer) {
    // The transition covers the whole image, not just the subresource being copied.
    // Copying one mip of a chain whose other mips are in a different layout would need
    // per-mip tracking that nothing here has; every target this is used on has a single
    // layout across all its subresources at the point of capture.
    transitionImage(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, aspect);

    VkBufferImageCopy region{};
    // Zero rowLength and imageHeight mean "tightly packed to imageExtent", which is
    // what the decode loop in writeCapturePng assumes.
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = mip;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);

    transitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, aspect);

    // Make the copy visible to a host read. Without this the mapped pointer may hold
    // whatever was there before, and on a coherent allocation it would appear to work
    // anyway -- which is exactly the kind of bug that shows up on someone else's GPU.
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool writeCapturePng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent, VkFormat format) {
    const uint32_t bpp = captureBytesPerPixel(format);
    if (bpp == 0) {
        core::Logger::error(core::LogCategory::Render, "Capture: unsupported format %d", static_cast<int>(format));
        return false;
    }
    if (!ensureParentDir(path)) return false;

    const size_t count = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
    std::vector<uint8_t> rgba(count * 4);

    const auto* src = static_cast<const uint8_t*>(pixels);
    for (size_t i = 0; i < count; ++i) {
        const Rgba8 texel = decodeTexel(src + i * bpp, format);
        rgba[i * 4 + 0] = texel.r;
        rgba[i * 4 + 1] = texel.g;
        rgba[i * 4 + 2] = texel.b;
        rgba[i * 4 + 3] = texel.a;
    }

    const int stride = static_cast<int>(extent.width) * 4;
    if (stbi_write_png(path.string().c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                       rgba.data(), stride) == 0) {
        core::Logger::error(core::LogCategory::Render, "Capture: failed to write %s", path.string().c_str());
        return false;
    }

    core::Logger::status(core::LogCategory::Render, "Captured %ux%u to %s", extent.width, extent.height, path.string().c_str());
    return true;
}

uint32_t targetBytesPerPixel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R16G16_SFLOAT: return 4;
    case VK_FORMAT_D32_SFLOAT: return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return 4;
    default: return 0;
    }
}

TargetStats writeTargetPng(const std::filesystem::path& path, const void* pixels, VkExtent2D extent,
                           VkFormat format) {
    TargetStats stats;

    const uint32_t bpp = targetBytesPerPixel(format);
    if (bpp == 0) {
        core::Logger::error(core::LogCategory::Render, "Target capture: format %d cannot be decoded",
                      static_cast<int>(format));
        return stats;
    }
    if (!ensureParentDir(path)) return stats;

    const size_t count = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
    const auto* src = static_cast<const uint8_t*>(pixels);

    // Two passes over the image: one to find the range, one to map into it. The
    // alternative -- a fixed exposure -- is what makes an HDR readback a white
    // rectangle, and a white rectangle is indistinguishable from a broken pass.
    std::vector<float> decoded(count * 4);
    uint32_t components = 0;
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();

    for (size_t i = 0; i < count; ++i) {
        float texel[4];
        decodeTargetTexel(src + i * bpp, format, texel, components);
        for (uint32_t c = 0; c < 4; ++c) decoded[i * 4 + c] = texel[c];
        // NaN and infinity are excluded from the range rather than allowed to collapse
        // it -- one stray infinity would otherwise map the entire image to black. A
        // pass that produces them is a bug, and the count below is how it surfaces.
        for (uint32_t c = 0; c < components && c < 3; ++c) {
            const float v = texel[c];
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }

    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        lo = 0.0f;
        hi = 1.0f;
    }
    stats.min = lo;
    stats.max = hi;

    const float span = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;
    stats.scale = span;

    const bool greyscale = components == 1;
    std::vector<uint8_t> rgba(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint8_t out[3];
        for (uint32_t c = 0; c < 3; ++c) {
            const float v = decoded[i * 4 + (greyscale ? 0 : c)];
            const float n = std::isfinite(v) ? (v - lo) / span : 0.0f;
            out[c] = static_cast<uint8_t>(std::clamp(n, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        rgba[i * 4 + 0] = out[0];
        rgba[i * 4 + 1] = out[1];
        rgba[i * 4 + 2] = out[2];
        rgba[i * 4 + 3] = 255;
    }

    const int stride = static_cast<int>(extent.width) * 4;
    if (stbi_write_png(path.string().c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                       rgba.data(), stride) == 0) {
        core::Logger::error(core::LogCategory::Render, "Target capture: failed to write %s", path.string().c_str());
        return stats;
    }

    stats.wrote = true;
    core::Logger::status(core::LogCategory::Render, "Target capture: %ux%u range [%.6g, %.6g] -> %s", extent.width,
                   extent.height, static_cast<double>(lo), static_cast<double>(hi), path.string().c_str());
    return stats;
}

CompareResult compareReadback(const std::filesystem::path& capturePath, const std::filesystem::path& source,
                              uint32_t scale, int32_t x, int32_t y, const std::filesystem::path& expectedPath,
                              const std::filesystem::path& diffPath, const ReadbackRect& srcRect) {
    CompareResult result;
    if (scale == 0) {
        core::Logger::error(core::LogCategory::Render, "Readback: a scale of zero cannot expand anything");
        return result;
    }

    uint32_t cw = 0;
    uint32_t ch = 0;
    const std::vector<uint8_t> capture = loadPng(capturePath, cw, ch);
    if (capture.empty()) return result;

    uint32_t sw = 0;
    uint32_t sh = 0;
    const std::vector<uint8_t> src = loadPng(source, sw, sh);
    if (src.empty()) return result;

    // `goldenLoaded` reads as "both images were readable", which is what the caller acts
    // on: the answer to a missing file is to fix the invocation, not to report a defect.
    result.goldenLoaded = true;

    // The rectangle of the source being asserted about. Zero in either axis is the whole
    // file, which is every P2 and P4 case; P5 passes one cell of a sheet, so that the
    // expectation is still computed from the one authored file rather than from a second
    // PNG somebody would have to keep in step with it.
    const uint32_t rx = (srcRect.width == 0 || srcRect.height == 0) ? 0 : srcRect.x;
    const uint32_t ry = (srcRect.width == 0 || srcRect.height == 0) ? 0 : srcRect.y;
    const uint32_t rw = (srcRect.width == 0 || srcRect.height == 0) ? sw : srcRect.width;
    const uint32_t rh = (srcRect.width == 0 || srcRect.height == 0) ? sh : srcRect.height;

    // A rectangle off the edge of the file is a wrong sheet description, and it is caught
    // here rather than read past: the same argument the capture-fit check below makes.
    if (rx + rw > sw || ry + rh > sh) {
        result.sizeMismatch = true;
        core::Logger::error(core::LogCategory::Render,
                            "Readback: source rect %ux%u at (%u,%u) is outside a %ux%u file", rw, rh, rx, ry, sw, sh);
        return result;
    }

    result.width = rw * scale;
    result.height = rh * scale;

    // A region that does not fit is a configuration error rather than a regression, and it
    // is reported as a size mismatch for exactly the reason comparePng gives about its own:
    // comparing what does fit would turn a wrong window size into a plausible-looking diff.
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) + result.width > cw ||
        static_cast<uint32_t>(y) + result.height > ch) {
        result.sizeMismatch = true;
        core::Logger::error(core::LogCategory::Render,
                            "Readback: %ux%u at (%d,%d) does not fit a %ux%u capture -- wrong window or wrong scale",
                            result.width, result.height, x, y, cw, ch);
        return result;
    }

    std::vector<uint8_t> expected(static_cast<size_t>(result.width) * result.height * 4, 255);
    std::vector<uint8_t> diff;
    if (!diffPath.empty()) diff.assign(expected.size(), 0);

    uint64_t channelSum = 0;
    for (uint32_t ey = 0; ey < result.height; ++ey) {
        for (uint32_t ex = 0; ex < result.width; ++ex) {
            // Integer division back to the source texel. The inverse of the expansion, and
            // exact because the destination is a whole multiple -- which is the property
            // that makes the blit a byte move in the first place.
            const size_t s = (static_cast<size_t>(ry + ey / scale) * sw + rx + ex / scale) * 4;
            const size_t e = (static_cast<size_t>(ey) * result.width + ex) * 4;
            const size_t c = (static_cast<size_t>(y + static_cast<int32_t>(ey)) * cw +
                              static_cast<size_t>(x + static_cast<int32_t>(ex))) *
                             4;

            uint32_t worst = 0;
            for (uint32_t ch3 = 0; ch3 < 3; ++ch3) {
                expected[e + ch3] = src[s + ch3];
                const uint32_t d = static_cast<uint32_t>(std::abs(static_cast<int>(capture[c + ch3]) - src[s + ch3]));
                worst = std::max(worst, d);
                channelSum += d;
            }

            // Zero tolerance, so any difference at all is a differing pixel. There is no
            // filtered tap anywhere in this path by construction, so the driver-noise
            // allowance the golden suite makes has nothing to protect here.
            if (worst > 0) {
                result.differingPixels++;
                if (worst > result.maxChannelDelta) {
                    result.maxChannelDelta = worst;
                    // In capture coordinates, because that is the image somebody opens.
                    result.worstX = static_cast<uint32_t>(x) + ex;
                    result.worstY = static_cast<uint32_t>(y) + ey;
                }
            }

            if (!diff.empty()) {
                diff[e + 3] = 255;
                if (worst > 0) {
                    diff[e + 0] = static_cast<uint8_t>(std::min(255u, 64u + worst * 4u));
                } else {
                    for (uint32_t ch3 = 0; ch3 < 3; ++ch3) diff[e + ch3] = capture[c + ch3] / 4;
                }
            }
        }
    }

    const size_t pixels = static_cast<size_t>(result.width) * result.height;
    result.meanChannelDelta = pixels > 0 ? static_cast<double>(channelSum) / static_cast<double>(pixels * 3) : 0.0;
    result.matched = result.differingPixels == 0;

    const int stride = static_cast<int>(result.width) * 4;
    if (!expectedPath.empty() && ensureParentDir(expectedPath)) {
        stbi_write_png(expectedPath.string().c_str(), static_cast<int>(result.width), static_cast<int>(result.height), 4,
                       expected.data(), stride);
    }
    if (!diff.empty() && ensureParentDir(diffPath)) {
        stbi_write_png(diffPath.string().c_str(), static_cast<int>(result.width), static_cast<int>(result.height), 4,
                       diff.data(), stride);
    }
    return result;
}

SilhouetteResult compareSilhouette(const std::filesystem::path& capturePath,
                                   const std::filesystem::path& backgroundPath,
                                   const std::filesystem::path& source, uint32_t scale, int32_t x, int32_t y,
                                   float cutoff, const std::filesystem::path& diffPath) {
    SilhouetteResult result;
    if (scale == 0) {
        core::Logger::error(core::LogCategory::Render, "Silhouette: a scale of zero covers nothing");
        return result;
    }

    uint32_t cw = 0, ch = 0;
    const std::vector<uint8_t> capture = loadPng(capturePath, cw, ch);
    if (capture.empty()) return result;

    uint32_t bw = 0, bh = 0;
    const std::vector<uint8_t> background = loadPng(backgroundPath, bw, bh);
    if (background.empty()) return result;

    uint32_t sw = 0, sh = 0;
    const std::vector<uint8_t> src = loadPng(source, sw, sh);
    if (src.empty()) return result;

    result.loaded = true;

    if (cw != bw || ch != bh) {
        result.sizeMismatch = true;
        core::Logger::error(core::LogCategory::Render,
                            "Silhouette: a %ux%u capture cannot be held against a %ux%u background", cw, ch, bw, bh);
        return result;
    }
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) + sw * scale > cw || static_cast<uint32_t>(y) + sh * scale > ch) {
        result.sizeMismatch = true;
        core::Logger::error(core::LogCategory::Render,
                            "Silhouette: %ux%u at (%d,%d) does not fit a %ux%u capture -- wrong window or scale",
                            sw * scale, sh * scale, x, y, cw, ch);
        return result;
    }

    // The expected mask, expanded exactly as `compareReadback` expands its expectation:
    // each source texel becomes a scale-by-scale block of its own answer. The threshold is
    // the shader's, `alpha < cutoff` discards, so at or above it is covered.
    const auto threshold = static_cast<uint32_t>(std::clamp(cutoff, 0.0f, 1.0f) * 255.0f);
    std::vector<uint8_t> mask(static_cast<size_t>(cw) * ch, 0);
    result.maskX0 = cw;
    result.maskY0 = ch;
    for (uint32_t sy = 0; sy < sh; ++sy) {
        for (uint32_t sx = 0; sx < sw; ++sx) {
            if (static_cast<uint32_t>(src[(static_cast<size_t>(sy) * sw + sx) * 4 + 3]) < threshold) continue;
            for (uint32_t dy = 0; dy < scale; ++dy) {
                for (uint32_t dx = 0; dx < scale; ++dx) {
                    const uint32_t px = static_cast<uint32_t>(x) + sx * scale + dx;
                    const uint32_t py = static_cast<uint32_t>(y) + sy * scale + dy;
                    mask[static_cast<size_t>(py) * cw + px] = 1;
                    result.maskX0 = std::min(result.maskX0, px);
                    result.maskY0 = std::min(result.maskY0, py);
                    result.maskX1 = std::max(result.maskX1, px + 1);
                    result.maskY1 = std::max(result.maskY1, py + 1);
                    result.expectedCovered++;
                }
            }
        }
    }

    std::vector<uint8_t> diff;
    if (!diffPath.empty()) diff.assign(static_cast<size_t>(cw) * ch * 4, 255);

    result.diffX0 = cw;
    result.diffY0 = ch;
    bool firstOutside = true;
    for (uint32_t py = 0; py < ch; ++py) {
        for (uint32_t px = 0; px < cw; ++px) {
            const size_t i = (static_cast<size_t>(py) * cw + px) * 4;
            const bool differs = capture[i] != background[i] || capture[i + 1] != background[i + 1] ||
                                 capture[i + 2] != background[i + 2];
            const bool covered = mask[static_cast<size_t>(py) * cw + px] != 0;
            if (differs) {
                result.diffX0 = std::min(result.diffX0, px);
                result.diffY0 = std::min(result.diffY0, py);
                result.diffX1 = std::max(result.diffX1, px + 1);
                result.diffY1 = std::max(result.diffY1, py + 1);
                if (covered) {
                    result.insideDiffering++;
                } else {
                    result.outsideDiffering++;
                    if (firstOutside) {
                        result.worstX = px;
                        result.worstY = py;
                        firstOutside = false;
                    }
                }
            }
            if (!diff.empty()) {
                // Green where the sprite covered and the frame moved, red where it moved and
                // it should not have, and the background dimmed everywhere else. The red is
                // the failure, and it is the pixel somebody has to look at.
                const uint8_t shade = static_cast<uint8_t>(background[i] / 4);
                const bool bad = differs && !covered;
                diff[i + 0] = bad ? 255 : shade;
                diff[i + 1] = (differs && covered) ? 255 : shade;
                diff[i + 2] = shade;
            }
        }
    }

    if (result.diffX0 > result.diffX1) result.diffX0 = result.diffY0 = 0;

    result.matched = result.outsideDiffering == 0 && result.expectedCovered > 0 &&
                     result.diffX0 == result.maskX0 && result.diffY0 == result.maskY0 &&
                     result.diffX1 == result.maskX1 && result.diffY1 == result.maskY1;

    if (!diff.empty() && ensureParentDir(diffPath)) {
        stbi_write_png(diffPath.string().c_str(), static_cast<int>(cw), static_cast<int>(ch), 4, diff.data(),
                       static_cast<int>(cw) * 4);
    }
    return result;
}

CompareResult comparePng(const std::filesystem::path& capturePath, const std::filesystem::path& goldenPath,
                         uint32_t tolerance, uint64_t maxDifferingPixels, const std::filesystem::path& diffPath) {
    CompareResult result;

    uint32_t cw = 0;
    uint32_t ch = 0;
    const std::vector<uint8_t> capture = loadPng(capturePath, cw, ch);
    if (capture.empty()) return result;

    uint32_t gw = 0;
    uint32_t gh = 0;
    const std::vector<uint8_t> golden = loadPng(goldenPath, gw, gh);
    if (golden.empty()) return result;

    result.goldenLoaded = true;
    result.width = cw;
    result.height = ch;

    if (cw != gw || ch != gh) {
        result.sizeMismatch = true;
        core::Logger::error(core::LogCategory::Render, "Compare: size mismatch -- capture %ux%u, golden %ux%u", cw, ch, gw, gh);
        return result;
    }

    std::vector<uint8_t> diff;
    if (!diffPath.empty()) diff.assign(static_cast<size_t>(cw) * ch * 4, 0);

    uint64_t channelSum = 0;
    const size_t pixelCount = static_cast<size_t>(cw) * ch;

    for (size_t i = 0; i < pixelCount; ++i) {
        uint32_t worst = 0;
        uint32_t sum = 0;
        for (uint32_t c = 0; c < 3; ++c) {
            const int a = capture[i * 4 + c];
            const int b = golden[i * 4 + c];
            const uint32_t d = static_cast<uint32_t>(std::abs(a - b));
            worst = std::max(worst, d);
            sum += d;
        }
        channelSum += sum;

        if (worst > tolerance) {
            result.differingPixels++;
            if (worst > result.maxChannelDelta) {
                result.maxChannelDelta = worst;
                result.worstX = static_cast<uint32_t>(i % cw);
                result.worstY = static_cast<uint32_t>(i / cw);
            }
        }

        if (!diff.empty()) {
            if (worst > tolerance) {
                // Red, scaled so a one-unit difference is still visible. Without the
                // floor a real regression of 2/255 renders as black and reads as a
                // clean pass.
                const uint8_t intensity = static_cast<uint8_t>(std::min(255u, 64u + worst * 4u));
                diff[i * 4 + 0] = intensity;
            } else {
                // The matching image at a quarter brightness, so the red marks sit on
                // top of something recognisable rather than on a void.
                diff[i * 4 + 0] = static_cast<uint8_t>(capture[i * 4 + 0] / 4);
                diff[i * 4 + 1] = static_cast<uint8_t>(capture[i * 4 + 1] / 4);
                diff[i * 4 + 2] = static_cast<uint8_t>(capture[i * 4 + 2] / 4);
            }
            diff[i * 4 + 3] = 255;
        }
    }

    result.meanChannelDelta = pixelCount > 0 ? static_cast<double>(channelSum) / (static_cast<double>(pixelCount) * 3.0)
                                             : 0.0;
    result.matched = result.differingPixels <= maxDifferingPixels;

    if (!diff.empty() && ensureParentDir(diffPath)) {
        stbi_write_png(diffPath.string().c_str(), static_cast<int>(cw), static_cast<int>(ch), 4, diff.data(),
                       static_cast<int>(cw) * 4);
    }

    return result;
}

} // namespace gfx
