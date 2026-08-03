#pragma once

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace gfx {

struct VulkanContext;

/**
 * @file Ktx2.h
 * @brief Read a KTX2 container holding an already-compressed mip chain (4.6a).
 *
 * ## What this deliberately is not
 *
 * It is not libktx, and it is not a transcoder. KTX2 is a container that can hold
 * BasisLZ or UASTC payloads which must be *transcoded* into a GPU format at load time,
 * and supporting that would mean vendoring the Basis universal codec -- tens of
 * thousands of lines to convert one representation into another that the offline tool
 * could have written in the first place.
 *
 * So `scripts/ktx2.py` does the transcode offline and writes plain BC7 (or BC5, or
 * whatever the device wants), and this reads a file whose levels are already the bytes
 * `vkCmdCopyBufferToImage` needs. The whole reader is a header parse and a level index:
 * a few hundred lines against a library, because the file it accepts is the easy half
 * of the format and the offline tool is where the hard half belongs.
 *
 * A file this cannot use -- supercompressed, or in a universal format -- is *rejected
 * with a reason* rather than half-loaded. The caller falls back to the source PNG,
 * which is the behaviour that makes the cache optional rather than load-bearing.
 *
 * ## Level order
 *
 * KTX2 stores levels smallest first in the file, but the level index is ordered base
 * level first. `levels[0]` here is the base level, matching Vulkan's mip numbering; the
 * byte offsets it carries run backwards through the file, and that is the format's
 * doing rather than a mistake.
 */
struct Ktx2Image {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 0;
    uint32_t layerCount = 1;
    uint32_t faceCount = 1;

    struct Level {
        uint64_t offset = 0; ///< into `bytes`
        uint64_t length = 0;
    };
    std::vector<Level> levels;

    /// The whole file. Held rather than streamed because every level is uploaded in one
    /// batch and the file is already smaller than the decoded image it replaces.
    std::vector<std::byte> bytes;

    [[nodiscard]] bool valid() const { return format != VK_FORMAT_UNDEFINED && !levels.empty(); }
};

/**
 * @brief Parse `path`. False means "use the source image instead", with the reason
 *        already logged at debug level -- a missing cache file is normal, not an error.
 */
[[nodiscard]] bool loadKtx2(const std::filesystem::path& path, Ktx2Image& out);

/// Whether the device can sample this format. A cache built for BC7 on one machine and
/// read on another that lacks it must fall back, not bind an unsupported image.
[[nodiscard]] bool formatSupported(const VulkanContext& ctx, VkFormat format);

/// Whether `format` carries sRGB-encoded colour. The engine decides sRGB-ness from the
/// material slot an image is used in; this is what lets a mismatch between that and the
/// cache file be reported instead of silently changing the image's gamma.
[[nodiscard]] bool formatIsSrgb(VkFormat format);

} // namespace gfx
