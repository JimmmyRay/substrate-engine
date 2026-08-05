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
 * @brief Read a KTX2 container holding an already-compressed mip chain.
 *
 * **Not a transcoder.** Only files whose levels are already the bytes
 * `vkCmdCopyBufferToImage` needs are accepted; `scripts/ktx2.py` does the transcode
 * offline. A supercompressed or universal-format file is rejected with a reason rather
 * than half-loaded, and the caller falls back to the source PNG.
 *
 * KTX2 stores levels smallest first in the file while its level index runs base level
 * first. `levels[0]` here is the base level, so its byte offsets run *backwards* through
 * the file -- that is the format, not a mistake.
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

    /// The whole file; `Level::offset` indexes into it, so it must outlive the upload.
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
/// material slot instead, so a disagreement between the two silently changes an image's
/// gamma unless it is checked here.
[[nodiscard]] bool formatIsSrgb(VkFormat format);

} // namespace gfx
