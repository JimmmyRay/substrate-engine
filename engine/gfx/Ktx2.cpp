#include "gfx/Ktx2.h"

#include "core/Logger.h"
#include "gfx/VulkanContext.h"

#include <cstring>
#include <fstream>

namespace gfx {

namespace {

constexpr uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

/// Everything before the level index, in file order. The KTX2 header is a fixed 80
/// bytes; reading it as a struct rather than field by field is safe because every
/// member is a fixed-width little-endian integer and the format guarantees the packing.
struct Header {
    uint8_t identifier[12];
    uint32_t vkFormat;
    uint32_t typeSize;
    uint32_t pixelWidth;
    uint32_t pixelHeight;
    uint32_t pixelDepth;
    uint32_t layerCount;
    uint32_t faceCount;
    uint32_t levelCount;
    uint32_t supercompressionScheme;
    uint32_t dfdByteOffset;
    uint32_t dfdByteLength;
    uint32_t kvdByteOffset;
    uint32_t kvdByteLength;
    uint64_t sgdByteOffset;
    uint64_t sgdByteLength;
};

static_assert(sizeof(Header) == 80, "KTX2 header is a fixed 80 bytes");

struct LevelIndexEntry {
    uint64_t byteOffset;
    uint64_t byteLength;
    uint64_t uncompressedByteLength;
};

} // namespace

bool loadKtx2(const std::filesystem::path& path, Ktx2Image& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < sizeof(Header)) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    out.bytes.resize(size);
    file.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(size));
    if (!file) {
        core::Logger::warn(core::LogCategory::GLTF, "%s: truncated read", path.string().c_str());
        out.bytes.clear();
        return false;
    }

    Header header{};
    std::memcpy(&header, out.bytes.data(), sizeof(Header));

    if (std::memcmp(header.identifier, kIdentifier, sizeof(kIdentifier)) != 0) {
        core::Logger::warn(core::LogCategory::GLTF, "%s: not a KTX2 file", path.string().c_str());
        out.bytes.clear();
        return false;
    }

    // The two rejections that keep this a reader rather than a codec. Both are things
    // `scripts/ktx2.py` is responsible for not producing, so hitting either means the
    // cache was built by something else -- worth a warning rather than a silent
    // fallback, because the fallback is a 4x memory regression nobody would notice.
    if (header.supercompressionScheme != 0) {
        core::Logger::warn(core::LogCategory::GLTF, "%s: supercompression scheme %u is not supported; using the source image",
                     path.string().c_str(), header.supercompressionScheme);
        out.bytes.clear();
        return false;
    }
    if (header.vkFormat == 0) {
        core::Logger::warn(core::LogCategory::GLTF,
                     "%s: vkFormat is UNDEFINED, which means a universal payload needing a transcoder; using the "
                     "source image",
                     path.string().c_str());
        out.bytes.clear();
        return false;
    }
    if (header.levelCount == 0 || header.pixelWidth == 0 || header.pixelHeight == 0) {
        core::Logger::warn(core::LogCategory::GLTF, "%s: no levels or zero extent", path.string().c_str());
        out.bytes.clear();
        return false;
    }

    const uint64_t indexBytes = static_cast<uint64_t>(header.levelCount) * sizeof(LevelIndexEntry);
    if (sizeof(Header) + indexBytes > size) {
        core::Logger::warn(core::LogCategory::GLTF, "%s: level index runs past the end of the file", path.string().c_str());
        out.bytes.clear();
        return false;
    }

    out.format = static_cast<VkFormat>(header.vkFormat);
    out.width = header.pixelWidth;
    out.height = header.pixelHeight;
    out.levelCount = header.levelCount;
    out.layerCount = header.layerCount == 0 ? 1u : header.layerCount;
    out.faceCount = header.faceCount == 0 ? 1u : header.faceCount;

    out.levels.resize(header.levelCount);
    for (uint32_t i = 0; i < header.levelCount; ++i) {
        LevelIndexEntry entry{};
        std::memcpy(&entry, out.bytes.data() + sizeof(Header) + i * sizeof(LevelIndexEntry), sizeof(entry));

        // Every offset is checked against the file rather than trusted. A texture cache
        // is a file on disk that a build script wrote, and a truncated one should
        // produce a message rather than a read past the end of a vector.
        if (entry.byteOffset + entry.byteLength > size) {
            core::Logger::warn(core::LogCategory::GLTF, "%s: level %u runs past the end of the file", path.string().c_str(), i);
            out.bytes.clear();
            out.levels.clear();
            return false;
        }
        out.levels[i] = {entry.byteOffset, entry.byteLength};
    }

    return true;
}

bool formatSupported(const VulkanContext& ctx, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

bool formatIsSrgb(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return true;
    default: return false;
    }
}

} // namespace gfx
