#include "scene/GltfScene.h"
#include "scene/SceneData.h"
#include "scene/SceneParse.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/Ktx2.h"
#include "gfx/VulkanContext.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>

namespace scene {

namespace {

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

/**
 * @brief Where the compressed cache entry for one glTF image would live.
 *
 * `<image>.png` -> `<image>.png.ktx2`, source extension kept so two images differing only
 * by extension cannot collide and the entry names the exact file it was built from. An
 * empty URI is an embedded payload, which has no filename to hang a cache off and gets
 * `<scene>.image<N>.ktx2` beside the scene -- the layout `scripts/ktx2.py` writes and the
 * one `writeSceneCache` refuses to bake an embedded image without.
 *
 * Takes a `SceneImageRef` rather than a `fastgltf::Image` because a cached load has no
 * document and has to answer the same way.
 */
std::filesystem::path ktx2CachePath(const std::filesystem::path& scenePath, const SceneImageRef& image,
                                    size_t index) {
    if (!image.uri.empty()) return scenePath.parent_path() / (image.uri + ".ktx2");
    return scenePath.parent_path() / (scenePath.stem().string() + ".image" + std::to_string(index) + ".ktx2");
}

/// stb_image returns malloc'd memory that must go back through stbi_image_free.
struct StbiDeleter {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};

/// Move-only through the `unique_ptr`: the decode fan-out below moves these between
/// threads, and a copyable aggregate holding an owning pointer is one copy from a
/// double free.
struct Decoded {
    std::unique_ptr<stbi_uc, StbiDeleter> pixels;
    int w = 0;
    int h = 0;
};

/// Decode one glTF image to RGBA8. Thread-safe: touches only its own arguments. A URI names
/// a file beside the document; an empty one means the payload was embedded, and `embedded`
/// is where the parse left it.
Decoded decodeImage(const std::filesystem::path& baseDir, const SceneImageRef& image,
                    const std::vector<uint8_t>& embedded) {
    Decoded out;
    int channels = 0;

    if (!image.uri.empty()) {
        const auto filePath = baseDir / image.uri;
        out.pixels.reset(stbi_load(filePath.string().c_str(), &out.w, &out.h, &channels, STBI_rgb_alpha));
    } else if (!embedded.empty()) {
        out.pixels.reset(stbi_load_from_memory(embedded.data(), static_cast<int>(embedded.size()), &out.w, &out.h,
                                               &channels, STBI_rgb_alpha));
    }

    return out;
}

/// The usage flags each of the three shared buffers is made with. Written in two places --
/// `upload` makes the buffers, `growBuffer` remakes them -- and a grown buffer missing a
/// flag the original had surfaces as a device lost inside an unrelated pass.
///
/// Functions rather than constants because the acceleration-structure flags are legal only
/// when VK_KHR_acceleration_structure is enabled.
VkBufferUsageFlags vertexBufferUsage(const gfx::VulkanContext& ctx) {
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (ctx.rayQuerySupported) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return usage;
}

VkBufferUsageFlags indexBufferUsage(const gfx::VulkanContext& ctx) {
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (ctx.rayQuerySupported) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return usage;
}

constexpr VkBufferUsageFlags kMaterialUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

} // namespace

bool GltfScene::load(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::filesystem::path& path,
                     float scale) {
    SceneData data;
    EmbeddedImages embedded;
    if (!loadSceneCpu(path, data, embedded)) return false;
    // Between the parse and the upload -- the one point a document and a sidecar have both
    // passed through. The factor is remembered because a game placing props off
    // `boundsMin`/`boundsMax` needs it, and because an append has to arrive at the same size.
    scaleSceneData(data, scale);
    sceneScale = scale > 0.0f ? scale : 1.0f;
    return upload(ctx, uploader, path, data, embedded);
}

bool GltfScene::createEmpty(const gfx::VulkanContext& ctx, gfx::Uploader& uploader) {
    SceneData empty;
    EmbeddedImages none;
    return upload(ctx, uploader, {}, empty, none);
}

bool GltfScene::upload(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::filesystem::path& path,
                       SceneData& data, EmbeddedImages& embedded) {
    const auto loadStart = std::chrono::steady_clock::now();

    prims = std::move(data.primitives);
    placedPrims = std::move(data.placements);
    sceneLights = std::move(data.lights);
    sceneEmitters = std::move(data.emitters);
    sceneColliders = std::move(data.colliders);
    sceneAudio = std::move(data.audioSources);
    sceneRig = std::move(data.rig);
    skinData = std::move(data.skinVertices);
    morphData = std::move(data.morphDeltas);
    materialEmissive = std::move(data.materialEmissive);

    /*
     * Cloth, lifted out of the flat arrays and into one self-contained record each.
     *
     * **The only reader of `data.clothVertices` and of `Primitive::clothOffset` that ever
     * runs**, and it has to run before `data.vertices` is moved into the upload. Indices are
     * rebased to zero here so no record needs `baseVertex` to be read -- see
     * `clothSources()`.
     */
    clothSourceList.clear();
    for (uint32_t pi = 0; pi < prims.size(); ++pi) {
        const Primitive& prim = prims[pi];
        if (prim.clothOffset == 0xFFFFFFFFu || prim.vertexCount == 0) continue;
        if (static_cast<size_t>(prim.clothOffset) + prim.vertexCount > data.clothVertices.size()) continue;
        if (static_cast<size_t>(prim.baseVertex) + prim.vertexCount > data.vertices.size()) continue;

        ClothSource src;
        src.primitive = pi;
        src.vertices.assign(data.vertices.begin() + prim.baseVertex,
                            data.vertices.begin() + prim.baseVertex + prim.vertexCount);
        src.masses.assign(data.clothVertices.begin() + prim.clothOffset,
                          data.clothVertices.begin() + prim.clothOffset + prim.vertexCount);
        src.indices.reserve(prim.indexCount);
        for (uint32_t k = 0; k < prim.indexCount; ++k) {
            const size_t at = static_cast<size_t>(prim.firstIndex) + k;
            if (at >= data.indices.size()) break;
            src.indices.push_back(data.indices[at] - prim.baseVertex);
        }
        clothSourceList.push_back(std::move(src));
    }

    boundsMin = data.boundsMin;
    boundsMax = data.boundsMax;
    boundsSet = true;
    sceneStats = data.stats;
    // The base scene keeps the node indices its own file wrote, so the first import starts
    // past them. See `LoadedModel::nodeBase`.
    nextNodeBase = std::max(sceneStats.nodes, 1u);


    auto texStart = std::chrono::steady_clock::now();
    {
        auto s = core::Profiler::scope("GltfScene::textures");

        // The slot an image is used in decides its format, and `SceneImageRef::srgb` carries
        // that answer through the sidecar: nothing on this side of the split has a document
        // to recompute it from.
        const size_t imageCount = data.images.size();
        textures.resize(imageCount);

        // A `.ktx2` beside the source image, written by scripts/ktx2.py, is used in its
        // place. Not declared as KHR_texture_basisu: that extension means a Basis payload
        // and these hold plain BC7.
        std::vector<gfx::Ktx2Image> cached(imageCount);
        std::vector<std::filesystem::path> ktxPaths(imageCount);
        {
            auto cs = core::Profiler::scope("GltfScene::textureCache");
            for (size_t i = 0; i < imageCount; ++i) {
                ktxPaths[i] = ktx2CachePath(path, data.images[i], i);
                if (ktxPaths[i].empty()) continue;
                if (!gfx::loadKtx2(ktxPaths[i], cached[i])) continue;
                if (!gfx::formatSupported(ctx, cached[i].format)) {
                    core::Logger::warn(core::LogCategory::GLTF, "%s: this device cannot sample format %d; using the source image",
                                 ktxPaths[i].string().c_str(), static_cast<int>(cached[i].format));
                    cached[i] = {};
                }
            }
        }

        std::vector<Decoded> decoded(imageCount);

        const unsigned workerCount = std::max(
            1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(imageCount)));
        std::atomic<size_t> nextImage{0};

        auto decodeWorker = [&] {
            auto ws = core::Profiler::scope("GltfScene::decode");
            for (;;) {
                const size_t i = nextImage.fetch_add(1, std::memory_order_relaxed);
                if (i >= imageCount) break;
                if (cached[i].valid()) continue;
                decoded[i] = decodeImage(path.parent_path(), data.images[i], embedded[i]);
            }
        };

        {
            auto ds = core::Profiler::scope("GltfScene::decodeAll");
            std::vector<std::thread> workers;
            workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
            // The name goes on the *spawned* workers, never inside `decodeWorker`: the
            // calling thread runs it too, so naming it there relabels that track "texture
            // decode" for the rest of the run -- the main thread, on a synchronous load.
            for (unsigned t = 1; t < workerCount; ++t) {
                workers.emplace_back([&decodeWorker] {
                    core::Profiler::nameThread("texture decode");
                    decodeWorker();
                });
            }
            // Guarded because this allocates -- `decodeImage` and the profiler scope both do
            // -- and a `bad_alloc` escaping here would destroy a vector of still-joinable
            // threads, which is a `std::terminate` rather than the failure it started as.
            // The `reserve` above rules out the other way that vector can throw.
            try {
                decodeWorker();
            } catch (...) {
                for (auto& t : workers) t.join();
                throw;
            }
            for (auto& t : workers) t.join();
        }

        auto us = core::Profiler::scope("GltfScene::uploadTextures");
        uploader.beginBatch(ctx);

        static const uint8_t kWhite[4] = {255, 255, 255, 255};

        for (size_t i = 0; i < imageCount; ++i) {
            if (cached[i].valid()) {
                const gfx::Ktx2Image& k = cached[i];
                if (gfx::formatIsSrgb(k.format) != data.images[i].srgb) {
                    // The cache decides the gamma once it is used, so a stale one changes
                    // how a texture looks rather than failing to load.
                    core::Logger::warn(core::LogCategory::GLTF,
                                 "%s: cache is %s but the material slot wants %s; rebuild it with scripts/ktx2.py",
                                 ktxPaths[i].string().c_str(), gfx::formatIsSrgb(k.format) ? "sRGB" : "linear",
                                 data.images[i].srgb ? "sRGB" : "linear");
                }

                textures[i] = gfx::createImage(ctx, {k.width, k.height}, k.format,
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                               k.levelCount);

                std::vector<std::pair<uint64_t, uint64_t>> levels;
                levels.reserve(k.levels.size());
                uint64_t compressedBytes = 0;
                for (const auto& lv : k.levels) {
                    levels.emplace_back(lv.offset, lv.length);
                    compressedBytes += lv.length;
                }
                uploader.addImageLevels(ctx, textures[i], k.bytes.data(), k.bytes.size(), levels);
                sceneStats.textureBytes += compressedBytes;
                sceneStats.compressedTextures++;
                continue;
            }

            if (decoded[i].pixels == nullptr) {
                core::Logger::warn(core::LogCategory::GLTF, "Image %zu failed to decode; substituting 1x1 white", i);
                textures[i] = gfx::createImage(ctx, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                                               1);
                uploader.addImageWithMips(ctx, textures[i], kWhite, sizeof(kWhite));
                continue;
            }

            const VkExtent2D extent{static_cast<uint32_t>(decoded[i].w), static_cast<uint32_t>(decoded[i].h)};
            const VkFormat format = data.images[i].srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(decoded[i].w) * decoded[i].h * 4;

            textures[i] = gfx::createImage(ctx, extent, format,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT,
                                           gfx::mipLevelsFor(extent));
            uploader.addImageWithMips(ctx, textures[i], decoded[i].pixels.get(), bytes);
            sceneStats.textureBytes += bytes;

            // Released inside the loop, not after it: `addImageWithMips` has already copied
            // into staging, and holding every decode until the loop ends means every decoded
            // texture and every staging copy resident at once -- a few hundred MB of peak on
            // Sponza.
            decoded[i].pixels.reset();
        }

        uploader.endBatch(ctx);
    }
    sceneStats.textureMs = msSince(texStart);


    // Recorded before the material table is uploaded and dropped; `buildSceneAccelStruct`
    // has no other copy to ask.
    materialEmissive.resize(data.materials.size());
    for (size_t i = 0; i < data.materials.size(); ++i) {
        const glm::vec4& e = data.materials[i].emissiveFactor;
        materialEmissive[i] = (e.x > 0.0f || e.y > 0.0f || e.z > 0.0f) ? 1u : 0u;
    }

    {
        auto s = core::Profiler::scope("GltfScene::upload");

        // Capacity, not size: the buffers are sub-allocated from here on, and a quarter over
        // is what makes appending one prop or one character cost no reallocation.
        vertexCapacity = static_cast<uint32_t>(data.vertices.size() + data.vertices.size() / 4 + 1024);
        indexCapacity = static_cast<uint32_t>(data.indices.size() + data.indices.size() / 4 + 1024);
        materialCapacity = static_cast<uint32_t>(data.materials.size() + 64);
        vertexRanges.reset(vertexCapacity);
        indexRanges.reset(indexCapacity);

        const VkDeviceSize vbSize = vertexCapacity * sizeof(Vertex);
        const VkDeviceSize ibSize = indexCapacity * sizeof(uint32_t);
        const VkDeviceSize mbSize = materialCapacity * sizeof(GpuMaterial);

        vertices = gfx::createBuffer(ctx, vbSize, vertexBufferUsage(ctx), VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                     "sceneVertices");
        indices = gfx::createBuffer(ctx, ibSize, indexBufferUsage(ctx), VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                    "sceneIndices");
        materials = gfx::createBuffer(ctx, mbSize, kMaterialUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                      "sceneMaterials");

        // The first allocation out of a fresh allocator, so both come back zero and the
        // offsets already in `prims` still stand -- nothing here rebases them.
        const uint32_t firstVertex = vertexRanges.allocate(static_cast<uint32_t>(data.vertices.size()));
        const uint32_t firstIndex = indexRanges.allocate(static_cast<uint32_t>(data.indices.size()));
        materialCount = static_cast<uint32_t>(data.materials.size());
        materialCpu = data.materials;

        uploader.uploadBufferAt(ctx, vertices, static_cast<VkDeviceSize>(firstVertex) * sizeof(Vertex),
                                data.vertices.data(), data.vertices.size() * sizeof(Vertex));
        uploader.uploadBufferAt(ctx, indices, static_cast<VkDeviceSize>(firstIndex) * sizeof(uint32_t),
                                data.indices.data(), data.indices.size() * sizeof(uint32_t));
        uploader.uploadBufferAt(ctx, materials, 0, data.materials.data(),
                                data.materials.size() * sizeof(GpuMaterial));

        // Kept only for a scene that deforms -- see `indexData()` for what reads it. All
        // three deformers have to be tested: an instance missing from this copy reaches
        // `buildSceneAccelStruct` with nothing to rebase, falls back to the static tier and
        // traces its rest pose forever while drawing correctly.
        if (!skinData.empty() || !morphData.empty() || !clothSourceList.empty()) {
            indexCopy = std::move(data.indices);
        }
    }

    buildDescriptors(ctx, uploader);

    // Take two slots, give the first back, and the next acquire must return exactly it. A
    // free list that hands the same slot to two callers, or loses one, otherwise fails the
    // day a residency system starts using it rather than the day it breaks.
#ifdef SUBSTRATE_DEBUG
    {
        const uint32_t first = acquireTextureSlot();
        const uint32_t second = acquireTextureSlot();
        releaseTextureSlot(ctx, first);
        const uint32_t reused = acquireTextureSlot();
        if (first == UINT32_MAX || second == UINT32_MAX || second == first || reused != first) {
            core::Logger::critical(core::LogCategory::GLTF,
                             "Texture slot free list is broken: acquired %u then %u, released %u, got %u back", first,
                             second, first, reused);
        }
        releaseTextureSlot(ctx, second);
        releaseTextureSlot(ctx, reused);
    }
#endif

    sceneStats.primitives = static_cast<uint32_t>(prims.size());
    sceneStats.draws = static_cast<uint32_t>(placedPrims.size());
    sceneStats.materials = static_cast<uint32_t>(data.materials.size());
    // Images, not slots: buildDescriptors() has already grown `textures` past them.
    sceneStats.textures = static_cast<uint32_t>(data.images.size());
    sceneStats.vertexCount = data.vertices.size();
    sceneStats.indexCount = data.indices.size();
    sceneStats.emitters = static_cast<uint32_t>(sceneEmitters.size());
    sceneStats.colliders = static_cast<uint32_t>(sceneColliders.size());
    sceneStats.audioSources = static_cast<uint32_t>(sceneAudio.size());
    sceneStats.totalMs = msSince(loadStart);

    // No path means `createEmpty`, and the summary below is a report on a document.
    if (path.empty()) {
        core::Logger::status(core::LogCategory::GLTF, "Empty scene ready: %u vertices and %u indices of room, %u texture slots",
                             vertexCapacity, indexCapacity, textureSlots);
        return true;
    }

    core::Logger::status(core::LogCategory::GLTF, "Loaded %s", path.filename().string().c_str());
    core::Logger::status(core::LogCategory::GLTF,
                   "  nodes=%u meshes=%u primitives=%u draws=%u lights=%zu emitters=%u colliders=%u",
                   sceneStats.nodes, sceneStats.meshes, sceneStats.primitives, sceneStats.draws, sceneLights.size(),
                   sceneStats.emitters, sceneStats.colliders);
    core::Logger::status(core::LogCategory::GLTF, "  materials=%u (%u alpha-masked, %u blended) textures=%u (%.1f MB decoded)",
                   sceneStats.materials, sceneStats.alphaMaskedMaterials, sceneStats.blendedMaterials,
                   sceneStats.textures, static_cast<double>(sceneStats.textureBytes) / (1024.0 * 1024.0));
    if (sceneStats.blendedDraws > 0) {
        core::Logger::status(core::LogCategory::GLTF, "  %u draws deferred to the forward pass", sceneStats.blendedDraws);
    }
    core::Logger::status(core::LogCategory::GLTF, "  texture slots: %u of %u used, %u free, fallback at %u%s",
                   sceneStats.textures, textureSlots,
                   static_cast<uint32_t>(freeTextureSlots.size()), fallbackSlot,
                   sceneStats.compressedTextures > 0 ? "" : " (no compressed cache; run scripts/ktx2.py)");
    if (sceneStats.compressedTextures > 0) {
        core::Logger::status(core::LogCategory::GLTF, "  %u of %u images came from the BC7 cache", sceneStats.compressedTextures,
                       sceneStats.textures);
    }
    if (sceneStats.skins > 0 || sceneStats.animations > 0) {
        core::Logger::status(core::LogCategory::GLTF, "  skins=%u animations=%u skinned vertices=%u", sceneStats.skins,
                       sceneStats.animations, sceneStats.skinnedVertices);
    }
    if (sceneStats.morphTargets > 0) {
        core::Logger::status(core::LogCategory::GLTF, "  morph targets=%u over %u vertices (%.2f MB of deltas)",
                       sceneStats.morphTargets, sceneStats.morphedVertices,
                       static_cast<double>(morphData.size() * sizeof(MorphDelta)) / (1024.0 * 1024.0));
    }
    core::Logger::status(core::LogCategory::GLTF, "  vertices=%lu indices=%lu (%lu triangles)",
                   static_cast<unsigned long>(sceneStats.vertexCount),
                   static_cast<unsigned long>(sceneStats.indexCount),
                   static_cast<unsigned long>(sceneStats.indexCount / 3));
    core::Logger::status(core::LogCategory::GLTF, "  LOD chains: %u primitives, %lu of those indices (%s)",
                   sceneStats.lodPrimitives, static_cast<unsigned long>(sceneStats.lodIndices),
                   sceneStats.lodPrimitives > 0 ? "baked" : "none; bake with substrate-bake");
    core::Logger::status(core::LogCategory::GLTF, "  bounds min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f)", boundsMin.x, boundsMin.y,
                   boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);
    core::Logger::status(core::LogCategory::GLTF, "  timing: cache=%.1fms parse=%.1fms geometry=%.1fms textures=%.1fms total=%.1fms",
                   sceneStats.cacheMs, sceneStats.parseMs, sceneStats.geometryMs, sceneStats.textureMs,
                   sceneStats.totalMs);
    core::Logger::status(core::LogCategory::GLTF, "    parse: mmap=%.1fms extras=%.1fms fastgltf=%.1fms",
                   sceneStats.mmapMs, sceneStats.extrasMs, sceneStats.gltfMs);

    return true;
}

void GltfScene::buildDescriptors(const gfx::VulkanContext& ctx, gfx::Uploader& uploader) {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = std::min(16.0f, ctx.properties.limits.maxSamplerAnisotropy);
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    gfx::vkCheck(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");

    // Sized past what the scene loaded: swapping a slot's contents touches no pipeline only
    // if the slot is already in the descriptor array, so the headroom is what makes
    // streaming possible without rebuilding the set.
    const uint32_t loaded = static_cast<uint32_t>(textures.size());
    fallbackSlot = loaded;
    textureSlots = std::max<uint32_t>(1, loaded + 1 + kTextureSlotHeadroom);

    // 1x1 opaque white reads as "no texture" through every material slot: a base colour
    // multiplied by white is the factor alone, and an occlusion or roughness map of white is
    // the neutral value. Every free slot's descriptor points here, so an unresident texture
    // samples something defined rather than whatever the slot last held.
    textures.resize(textureSlots);
    textures[fallbackSlot] = gfx::createImage(ctx, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                              "fallbackTexture");
    // The texel is written, not merely allocated: an image created and never uploaded sits
    // in UNDEFINED, which on this driver reads as transparent black, and a decal naming this
    // slot then discards every fragment on alpha.
    const uint32_t whiteTexel = 0xFFFFFFFFu;
    uploader.uploadImageWithMips(ctx, textures[fallbackSlot], &whiteTexel, sizeof(whiteTexel));

    // Pushed highest first so the first acquire returns the lowest index, which makes a
    // capture easier to read than a reversed one.
    freeTextureSlots.clear();
    for (uint32_t slot = textureSlots; slot > fallbackSlot + 1; --slot) freeTextureSlots.push_back(slot - 1);

    createSceneDescriptors(ctx);
}

void GltfScene::createSceneDescriptors(const gfx::VulkanContext& ctx) {
    const uint32_t texCount = textureSlots;

    // Compute as well as fragment: the tracing passes shade a ray hit from a compute shader
    // and read the same materials and bindless textures the G-buffer pass does.
    const VkShaderStageFlags sceneStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = sceneStages;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = texCount;
    bindings[1].stageFlags = sceneStages;

    const VkDescriptorBindingFlags bindingFlags[2] = {
        0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 2;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    layoutInfo.pNext = &flagsInfo;
    gfx::vkCheck(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &setLayout),
                 "vkCreateDescriptorSetLayout(scene)");
    setBindings.assign(std::begin(bindings), std::end(bindings));

    VkDescriptorPoolSize sizes[2]{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texCount};

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    gfx::vkCheck(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &descriptorPool),
                 "vkCreateDescriptorPool(scene)");

    VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    countInfo.descriptorSetCount = 1;
    countInfo.pDescriptorCounts = &texCount;

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    allocInfo.pNext = &countInfo;
    gfx::vkCheck(vkAllocateDescriptorSets(ctx.device, &allocInfo, &set), "vkAllocateDescriptorSets(scene)");

    VkDescriptorBufferInfo materialInfo{materials.buffer, 0, VK_WHOLE_SIZE};

    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(textures.size());
    for (const auto& tex : textures) {
        // A slot with no image points at the fallback rather than being left unwritten:
        // PARTIALLY_BOUND permits the latter, and a shader reading such a slot reads
        // undefined data.
        const VkImageView view = tex.view != VK_NULL_HANDLE ? tex.view : textures[fallbackSlot].view;
        imageInfos.push_back({sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &materialInfo;

    uint32_t writeCount = 1;
    if (!imageInfos.empty()) {
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = static_cast<uint32_t>(imageInfos.size());
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = imageInfos.data();
        writeCount = 2;
    }

    vkUpdateDescriptorSets(ctx.device, writeCount, writes, 0, nullptr);
}

void GltfScene::destroy(const gfx::VulkanContext& ctx) {
    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(ctx.device, setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(ctx.device, sampler, nullptr);
    descriptorPool = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;

    for (auto& tex : textures) gfx::destroyImage(ctx, tex);
    textures.clear();

    gfx::destroyBuffer(ctx, vertices);
    gfx::destroyBuffer(ctx, indices);
    gfx::destroyBuffer(ctx, materials);

    prims.clear();
    placedPrims.clear();

    // A `ModelId` is an index into `models`, so leaving the vector populated leaves every id
    // a caller holds *valid* against a scene that no longer exists, and `unloadModel` would
    // hand ranges back to allocators that never issued them -- which `RangeAllocator`
    // documents it cannot detect. Cleared, its `id >= models.size()` guard makes a stale id
    // the no-op it should be. The free list goes for the same reason: a `destroy()` with no
    // `upload()` behind it would otherwise be appended to.
    models.clear();
    freeTextureSlots.clear();
}

void GltfScene::writeTextureDescriptor(const gfx::VulkanContext& ctx, uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 1;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}

bool GltfScene::reserveTextureSlots(const gfx::VulkanContext& ctx, uint32_t atLeast) {
    if (atLeast <= freeTextureSlots.size()) return true;

    // Doubling, not exactly-enough: each grow costs the call site a pipeline rebuild, and a
    // composed world imports in bursts.
    const uint32_t need = textureSlots + (atLeast - static_cast<uint32_t>(freeTextureSlots.size()));
    uint32_t grown = std::max(textureSlots * 2u, need);
    if (grown > ctx.properties.limits.maxPerStageDescriptorSampledImages) {
        grown = ctx.properties.limits.maxPerStageDescriptorSampledImages;
        if (grown < need) {
            core::Logger::warn(core::LogCategory::GLTF,
                               "Texture slots: %u needed, and the device binds at most %u per stage", need, grown);
            return false;
        }
    }

    // Slots are appended past the end, so `fallbackSlot` and every slot a material already
    // names keep their index. A grow that renumbered them would repoint every material in
    // the scene at somebody else's image.
    for (uint32_t slot = grown; slot > textureSlots; --slot) freeTextureSlots.push_back(slot - 1);
    textureSlots = grown;
    textures.resize(textureSlots);

    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(ctx.device, setLayout, nullptr);
    descriptorPool = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;
    createSceneDescriptors(ctx);

    core::Logger::status(core::LogCategory::GLTF, "Texture slots grown to %u", textureSlots);
    return true;
}

uint32_t GltfScene::acquireTextureSlot() {
    if (freeTextureSlots.empty()) {
        core::Logger::warn(core::LogCategory::GLTF, "Texture slots exhausted (%u in use of %u)", textureSlots,
                           textureSlots);
        return UINT32_MAX;
    }
    const uint32_t slot = freeTextureSlots.back();
    freeTextureSlots.pop_back();
    return slot;
}

void GltfScene::releaseTextureSlot(const gfx::VulkanContext& ctx, uint32_t slot) {
    // The fallback is not a slot anyone owns, and releasing a free slot twice would put
    // it in the list twice -- after which two callers would be handed the same slot.
    if (slot >= textureSlots || slot == fallbackSlot) return;
    if (std::find(freeTextureSlots.begin(), freeTextureSlots.end(), slot) != freeTextureSlots.end()) return;

    // Descriptor first, image second. The other order leaves a destroyed view bound for
    // as long as it takes to get to the next line.
    writeTextureDescriptor(ctx, slot, textures[fallbackSlot].view);
    gfx::destroyImage(ctx, textures[slot]);
    freeTextureSlots.push_back(slot);
}

void GltfScene::bindTexture(const gfx::VulkanContext& ctx, uint32_t slot, const gfx::GpuImage& image) {
    if (slot >= textureSlots || slot == fallbackSlot) return;
    if (textures[slot].image != VK_NULL_HANDLE) gfx::destroyImage(ctx, textures[slot]);
    textures[slot] = image;
    writeTextureDescriptor(ctx, slot, image.view);
}

GltfScene::ModelId GltfScene::appendModel(const gfx::VulkanContext& ctx, gfx::Uploader& uploader,
                                          const std::filesystem::path& path, const glm::mat4& transform) {
    if (vertices.buffer == VK_NULL_HANDLE) {
        core::Logger::warn(core::LogCategory::GLTF, "appendModel(%s) before a scene was loaded", path.string().c_str());
        return kNoModel;
    }

    SceneData data;
    EmbeddedImages embedded;
    if (!loadSceneCpu(path, data, embedded)) return kNoModel;

    // At the scene's own scale -- `Engine::setWorldScale` where no document set one. A file
    // appended at 1x into a 4x world is a doll's house.
    scaleSceneData(data, sceneScale);
    // **Captured before the placement is baked in**: after it the file's own node hierarchy
    // is indistinguishable from where the caller put the import. See
    // `LoadedModel::placementLocals`.
    std::vector<glm::mat4> placementLocals;
    placementLocals.reserve(data.placements.size());
    for (const Placement& p : data.placements) placementLocals.push_back(p.transform);

    // Scale first, place second: the caller's transform is in world units, so a scale applied
    // after it multiplies the placement too and puts the import at twice the distance it
    // asked for.
    placeSceneData(data, transform);

    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    const auto indexCount = static_cast<uint32_t>(data.indices.size());

    // Both ranges or neither. Taking the first and failing the second leaks it, and this is
    // the one place a leak is invisible -- the buffer simply has less room next time and
    // nothing says why.
    if (!reserveGeometry(ctx, uploader, vertexCount, indexCount) ||
        !reserveMaterials(ctx, uploader, static_cast<uint32_t>(data.materials.size()))) {
        return kNoModel;
    }

    const uint32_t firstVertex = vertexRanges.allocate(vertexCount);
    if (firstVertex == core::RangeAllocator::kNoRange) {
        core::Logger::warn(core::LogCategory::GLTF, "appendModel(%s): needs %u vertices, largest free run is %u",
                           path.string().c_str(), vertexCount, vertexRanges.largestFree());
        return kNoModel;
    }
    const uint32_t firstIndex = indexRanges.allocate(indexCount);
    if (firstIndex == core::RangeAllocator::kNoRange) {
        vertexRanges.free(firstVertex, vertexCount);
        core::Logger::warn(core::LogCategory::GLTF, "appendModel(%s): needs %u indices, largest free run is %u",
                           path.string().c_str(), indexCount, indexRanges.largestFree());
        return kNoModel;
    }

    const auto materialBase = materialCount;
    if (materialBase + data.materials.size() > materialCapacity) {
        vertexRanges.free(firstVertex, vertexCount);
        indexRanges.free(firstIndex, indexCount);
        core::Logger::warn(core::LogCategory::GLTF, "appendModel(%s): needs %zu materials, %u slots left",
                           path.string().c_str(), data.materials.size(), materialCapacity - materialBase);
        return kNoModel;
    }

    LoadedModel entry;
    entry.firstVertex = firstVertex;
    entry.vertexCount = vertexCount;
    entry.firstIndex = firstIndex;
    entry.indexCount = indexCount;
    entry.firstMaterial = materialBase;
    entry.materialCount = static_cast<uint32_t>(data.materials.size());

    // An image that cannot have a slot falls back rather than failing the load: a model with
    // the wrong texture is recoverable and a model that did not appear is not.
    (void)reserveTextureSlots(ctx, static_cast<uint32_t>(data.images.size()));
    const std::filesystem::path baseDir = path.parent_path();
    std::vector<int32_t> slotOf(data.images.size(), -1);
    entry.textureSlotsUsed.assign(data.images.size(), kNoTextureSlot);
    for (size_t i = 0; i < data.images.size(); ++i) {
        if (freeTextureSlots.empty()) {
            core::Logger::warn(core::LogCategory::GLTF, "appendModel(%s): out of texture slots at image %zu",
                               path.string().c_str(), i);
            break;
        }
        Decoded decodedImage = decodeImage(baseDir, data.images[i], i < embedded.size() ? embedded[i] : std::vector<uint8_t>{});
        if (decodedImage.pixels == nullptr) continue;

        const uint32_t slot = freeTextureSlots.back();
        freeTextureSlots.pop_back();

        const VkExtent2D extent{static_cast<uint32_t>(decodedImage.w), static_cast<uint32_t>(decodedImage.h)};
        const VkFormat format = data.images[i].srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        textures[slot] = gfx::createImage(ctx, extent, format,
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                          gfx::mipLevelsFor(extent));
        uploader.uploadImageWithMips(ctx, textures[slot],
                                     decodedImage.pixels.get(),
                                     static_cast<VkDeviceSize>(decodedImage.w) * decodedImage.h * 4);
        writeTextureDescriptor(ctx, slot, textures[slot].view);
        slotOf[i] = static_cast<int32_t>(slot);
        entry.textureSlotsUsed[i] = slot;
    }

    // The appended materials name *image* indices; the descriptor array is addressed by
    // slot. Anything that did not get one falls back to the scene's white 1x1.
    const auto remap = [&](int32_t& index) {
        if (index < 0) return;
        index = (static_cast<size_t>(index) < slotOf.size() && slotOf[index] >= 0) ? slotOf[index]
                                                                                  : static_cast<int32_t>(fallbackSlot);
    };
    for (GpuMaterial& m : data.materials) {
        remap(m.baseColorTexture);
        remap(m.metallicRoughnessTexture);
        remap(m.normalTexture);
        remap(m.emissiveTexture);
        remap(m.occlusionTexture);
    }

    // The index buffer holds absolute vertex indices, so an appended model's are rebased
    // here rather than carried as a `vertexOffset`: the draw already uses that for something
    // else, and the skinning dispatch reads it as a vertex range.
    for (uint32_t& index : data.indices) index += firstVertex;

    uploader.uploadBufferAt(ctx, vertices, static_cast<VkDeviceSize>(firstVertex) * sizeof(Vertex),
                            data.vertices.data(), static_cast<VkDeviceSize>(vertexCount) * sizeof(Vertex));
    uploader.uploadBufferAt(ctx, indices, static_cast<VkDeviceSize>(firstIndex) * sizeof(uint32_t),
                            data.indices.data(), static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t));
    if (!data.materials.empty()) {
        uploader.uploadBufferAt(ctx, materials, static_cast<VkDeviceSize>(materialBase) * sizeof(GpuMaterial),
                                data.materials.data(), data.materials.size() * sizeof(GpuMaterial));
    }

    materialEmissive.resize(materialBase + data.materials.size());
    for (size_t i = 0; i < data.materials.size(); ++i) {
        const glm::vec4& e = data.materials[i].emissiveFactor;
        materialEmissive[materialBase + i] = (e.x > 0.0f || e.y > 0.0f || e.z > 0.0f) ? 1u : 0u;
    }
    materialCount = materialBase + static_cast<uint32_t>(data.materials.size());
    materialCpu.resize(materialCount);
    for (size_t i = 0; i < data.materials.size(); ++i) materialCpu[materialBase + i] = data.materials[i];
    ++materialRev;

    // `skinOffset` and `morphOffset` are absolute offsets into the scene-wide `skinData` and
    // `morphData`, so an import left carrying its own takes a character's influences from
    // whatever the base scene holds at that address -- silently, and only while it is on
    // screen. Appended rather than repacked: an instance already carries these offsets, so
    // reclaiming a run out of the middle renumbers every later primitive under it.
    const bool deforms = !data.skinVertices.empty() || !data.morphDeltas.empty() || !data.clothVertices.empty();
    const auto skinBase = static_cast<uint32_t>(skinData.size());
    const auto morphBase = static_cast<uint32_t>(morphData.size());
    skinData.insert(skinData.end(), data.skinVertices.begin(), data.skinVertices.end());
    morphData.insert(morphData.end(), data.morphDeltas.begin(), data.morphDeltas.end());
    entry.firstSkinVertex = skinBase;
    entry.skinVertexCount = static_cast<uint32_t>(data.skinVertices.size());
    entry.firstMorphDelta = morphBase;
    entry.morphDeltaCount = static_cast<uint32_t>(data.morphDeltas.size());

    // **The CPU index copy has to move with them.** `buildSceneAccelStruct` rebases a
    // deformed primitive's indices by reading `indexData()[firstIndex + k]` on the *host*,
    // and that copy is a snapshot `load` took -- a scene that deformed nothing took none at
    // all, so an imported rig indexes past the end of a vector and the build reads whatever
    // follows it in memory. It does not fault: it hangs the GPU on a structure built over
    // nonsense, five seconds later, as `VK_ERROR_DEVICE_LOST` on an unrelated upload fence.
    if (deforms) {
        const size_t end = static_cast<size_t>(firstIndex) + indexCount;
        if (indexCopy.size() < end) indexCopy.resize(end, 0u);
        std::copy(data.indices.begin(), data.indices.end(), indexCopy.begin() + firstIndex);
    }

    // `Primitive::clothOffset` is read once, here, so the import's records are built from
    // the import's own arrays and there is no offset left to shift afterwards.
    entry.firstCloth = static_cast<uint32_t>(clothSourceList.size());
    for (uint32_t pi = 0; pi < data.primitives.size(); ++pi) {
        const Primitive& prim = data.primitives[pi];
        if (prim.clothOffset == 0xFFFFFFFFu || prim.vertexCount == 0) continue;
        if (static_cast<size_t>(prim.clothOffset) + prim.vertexCount > data.clothVertices.size()) continue;
        if (static_cast<size_t>(prim.baseVertex) + prim.vertexCount > data.vertices.size()) continue;

        ClothSource src;
        // Into the *scene's* primitive array, which the loop below is about to append to at
        // `entry.firstPrimitive`.
        src.primitive = static_cast<uint32_t>(prims.size()) + pi;
        src.vertices.assign(data.vertices.begin() + prim.baseVertex,
                            data.vertices.begin() + prim.baseVertex + prim.vertexCount);
        src.masses.assign(data.clothVertices.begin() + prim.clothOffset,
                          data.clothVertices.begin() + prim.clothOffset + prim.vertexCount);
        src.indices.reserve(prim.indexCount);
        for (uint32_t k = 0; k < prim.indexCount; ++k) {
            const size_t at = static_cast<size_t>(prim.firstIndex) + k;
            if (at >= data.indices.size()) break;
            // `data.indices` was shifted by `firstVertex` above and `prim.baseVertex` has
            // not been yet, so both terms have to be put in the scene's space before the
            // difference is the primitive-local index this record is defined to hold.
            src.indices.push_back(data.indices[at] - (prim.baseVertex + firstVertex));
        }
        clothSourceList.push_back(std::move(src));
    }
    entry.clothCount = static_cast<uint32_t>(clothSourceList.size()) - entry.firstCloth;

    entry.firstPrimitive = static_cast<uint32_t>(prims.size());
    entry.primitiveCount = static_cast<uint32_t>(data.primitives.size());
    for (Primitive& p : data.primitives) {
        if (p.skinOffset != 0xFFFFFFFFu) p.skinOffset += skinBase;
        if (p.morphTargets > 0) p.morphOffset += morphBase;
        // Cleared to zero, not to `0xFFFFFFFF`: it named an offset into a file-local array
        // that no longer exists, but `addPlacementInstances` reads it as the *flag* that
        // makes an instance cloth, so it must stay set where the primitive really is cloth.
        if (p.clothOffset != 0xFFFFFFFFu) p.clothOffset = 0u;
        p.firstIndex += firstIndex;
        for (uint32_t l = 0; l < p.lodCount && l < kMaxLodLevels; ++l) p.lods[l].firstIndex += firstIndex;
        p.baseVertex += firstVertex;
        if (p.materialIndex >= 0) p.materialIndex += static_cast<int32_t>(materialBase);
        prims.push_back(p);
    }

    // Node indices are file-local and several records match on them, so the whole import is
    // shifted past everything already loaded. See `LoadedModel::nodeBase`.
    entry.nodeBase = nextNodeBase;
    const auto shiftNode = [base = entry.nodeBase](uint32_t& node) {
        if (node != kNoNode) node += base;
    };

    entry.firstPlacement = static_cast<uint32_t>(placedPrims.size());
    entry.placementCount = static_cast<uint32_t>(data.placements.size());
    entry.placementLocals = std::move(placementLocals);
    for (Placement& pl : data.placements) {
        pl.primitive += entry.firstPrimitive;
        shiftNode(pl.node);
        shiftNode(pl.colliderNode);
        // `pl.skin` stays **file-local**: where the import's skins land is the animator's
        // answer, and the animator is not reachable from here. `rebaseAppendedSkins` is the
        // shift, after the merge.
        placedPrims.push_back(pl);
    }

    // Kept rather than merged: `sceneRig` was moved out into the animator at load, so
    // merging into what is left produces a rig nothing reads.
    entry.skinCount = static_cast<uint32_t>(data.rig.skins.size());
    entry.importedRig = std::move(data.rig);

    entry.firstCollider = static_cast<uint32_t>(sceneColliders.size());
    entry.colliderCount = static_cast<uint32_t>(data.colliders.size());
    for (ColliderDesc& c : data.colliders) {
        shiftNode(c.node);
        sceneColliders.push_back(std::move(c));
    }

    entry.firstLight = static_cast<uint32_t>(sceneLights.size());
    entry.lightCount = static_cast<uint32_t>(data.lights.size());
    // Already world-space and already placed by `placeSceneData`; a light carries no node to
    // shift.
    sceneLights.insert(sceneLights.end(), data.lights.begin(), data.lights.end());

    entry.firstEmitter = static_cast<uint32_t>(sceneEmitters.size());
    entry.emitterCount = static_cast<uint32_t>(data.emitters.size());
    for (ParticleEmitter& e : data.emitters) {
        shiftNode(e.node);
        // Through the same table the materials went through: an emitter names an *image*
        // index and the descriptor array is addressed by slot, so an import that brought
        // both an emitter and its sheet otherwise draws whatever occupies that slot.
        if (e.texture != 0xFFFFFFFFu) {
            int32_t slot = static_cast<int32_t>(e.texture);
            remap(slot);
            e.texture = static_cast<uint32_t>(slot);
        }
        sceneEmitters.push_back(std::move(e));
    }

    entry.firstAudio = static_cast<uint32_t>(sceneAudio.size());
    entry.audioCount = static_cast<uint32_t>(data.audioSources.size());
    for (AudioSourceDesc& a : data.audioSources) {
        shiftNode(a.node);
        sceneAudio.push_back(std::move(a));
    }

    // Past the highest index this import can have produced. `stats.nodes` is the file's own
    // node count, and a record may name any of them.
    nextNodeBase = entry.nodeBase + std::max(data.stats.nodes, 1u);

    entry.live = true;

    // A world composed only of imports never passes through `load`, so without this
    // `Camera::frameBounds` frames an empty box and the shadow cascades fit nothing.
    //
    // All eight corners, because the import's bounds are in its local space and a rotation
    // turns a box into one whose axis-aligned extent is larger than any two transformed
    // corners would say.
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) != 0 ? data.boundsMax.x : data.boundsMin.x,
                              (corner & 2) != 0 ? data.boundsMax.y : data.boundsMin.y,
                              (corner & 4) != 0 ? data.boundsMax.z : data.boundsMin.z);
        const glm::vec3 world(transform * glm::vec4(local, 1.0f));
        if (!boundsSet) {
            boundsMin = world;
            boundsMax = world;
            boundsSet = true;
        } else {
            boundsMin = glm::min(boundsMin, world);
            boundsMax = glm::max(boundsMax, world);
        }
    }

    // Before the move, not after. Every field read here is a `uint32_t`, so reading after it
    // would work today and break the first time a vector member is named.
    core::Logger::status(core::LogCategory::GLTF,
                         "appended %s: %u verts, %u indices, %u placements, %u colliders, %u lights, "
                         "%u emitters, %u sounds (nodes from %u)",
                         path.filename().string().c_str(), vertexCount, indexCount, entry.placementCount,
                         entry.colliderCount, entry.lightCount, entry.emitterCount, entry.audioCount, entry.nodeBase);
    models.push_back(std::move(entry));
    return static_cast<ModelId>(models.size() - 1);
}

AnimationRig GltfScene::takeAppendedRig(ModelId id) {
    if (id >= models.size()) return {};
    return std::move(models[id].importedRig);
}

void GltfScene::rebaseAppendedSkins(ModelId id, uint32_t skinBase) {
    if (id >= models.size()) return;
    LoadedModel& entry = models[id];
    entry.skinBase = skinBase;
    if (skinBase == kNoRig) return;

    const uint32_t end = entry.firstPlacement + entry.placementCount;
    for (uint32_t p = entry.firstPlacement; p < end && p < placedPrims.size(); ++p) {
        Placement& pl = placedPrims[p];
        // `kNoNode` here is glTF's "this placement is not skinned"; adding a base to it makes
        // an unskinned crate name skin 0 of the base scene's character.
        if (pl.skin != kNoNode) pl.skin += skinBase;
    }
}


void GltfScene::writeMaterialDescriptor(const gfx::VulkanContext& ctx) {
    if (set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo info{materials.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}

bool GltfScene::growBuffer(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, gfx::GpuBuffer& buffer,
                           VkBufferUsageFlags usage, uint32_t oldCapacity, uint32_t newCapacity, VkDeviceSize stride,
                           const char* name) {
    gfx::GpuBuffer grown = gfx::createBuffer(ctx, static_cast<VkDeviceSize>(newCapacity) * stride, usage,
                                             VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, name);
    if (grown.buffer == VK_NULL_HANDLE) {
        core::Logger::error(core::LogCategory::GLTF, "%s: could not grow to %u elements; keeping %u", name,
                            newCapacity, oldCapacity);
        return false;
    }

    // `oldCapacity`, not the new size: the tail past it was never written, and copying it
    // reads uninitialised device memory.
    if (oldCapacity > 0) {
        uploader.copyBuffer(ctx, grown, buffer, static_cast<VkDeviceSize>(oldCapacity) * stride);
    }
    gfx::destroyBuffer(ctx, buffer);
    buffer = grown;
    core::Logger::status(core::LogCategory::GLTF, "%s: grown %u -> %u elements (%.1f MB)", name, oldCapacity,
                         newCapacity, static_cast<double>(newCapacity) * static_cast<double>(stride) / (1024.0 * 1024.0));
    return true;
}

bool GltfScene::reserveGeometry(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t needVertices,
                                uint32_t needIndices) {
    // Floored at what is needed *plus* what is already there: growing to exactly the request
    // grows again on the next append, and each one is a full-buffer copy under a device wait.
    if (needVertices > vertexRanges.largestFree()) {
        const uint32_t target = std::max(vertexCapacity * 2, vertexCapacity + needVertices);
        if (!growBuffer(ctx, uploader, vertices, vertexBufferUsage(ctx), vertexCapacity, target, sizeof(Vertex),
                        "sceneVertices")) {
            return false;
        }
        vertexCapacity = target;
        if (!vertexRanges.grow(vertexCapacity)) return false;
    }
    if (needIndices > indexRanges.largestFree()) {
        const uint32_t target = std::max(indexCapacity * 2, indexCapacity + needIndices);
        if (!growBuffer(ctx, uploader, indices, indexBufferUsage(ctx), indexCapacity, target, sizeof(uint32_t),
                        "sceneIndices")) {
            return false;
        }
        indexCapacity = target;
        if (!indexRanges.grow(indexCapacity)) return false;
    }
    return true;
}

bool GltfScene::reserveMaterials(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, uint32_t need) {
    if (materialCount + need <= materialCapacity) return true;
    const uint32_t target = std::max(materialCapacity * 2, materialCount + need);
    if (!growBuffer(ctx, uploader, materials, kMaterialUsage, materialCapacity, target, sizeof(GpuMaterial),
                    "sceneMaterials")) {
        return false;
    }
    materialCapacity = target;
    // The buffer moved, so the one descriptor naming it is rewritten. Not `buildDescriptors`
    // -- it creates the sampler and the fallback image, which already exist and would leak.
    writeMaterialDescriptor(ctx);
    return true;
}

uint32_t GltfScene::createMaterial(const GpuMaterial& m) {
    if (materialCount >= materialCapacity) {
        core::Logger::warn(core::LogCategory::GLTF,
                           "createMaterial: %u of %u slots used; call it before the buffer is full or grow the scene",
                           materialCount, materialCapacity);
        return UINT32_MAX;
    }
    const uint32_t index = materialCount++;
    materialCpu.resize(materialCount);
    materialCpu[index] = m;
    materialEmissive.resize(materialCount);
    materialEmissive[index] = (m.emissiveFactor.x > 0.0f || m.emissiveFactor.y > 0.0f || m.emissiveFactor.z > 0.0f) ? 1u : 0u;
    ++materialRev;
    return index;
}

const GpuMaterial& GltfScene::material(uint32_t index) const {
    // Material 0 rather than past the end: a bounds bug should be a wrong answer in one
    // place rather than a crash somewhere later.
    static const GpuMaterial kNone{};
    if (materialCpu.empty()) return kNone;
    return materialCpu[index < materialCpu.size() ? index : 0];
}

void GltfScene::setMaterial(uint32_t index, const GpuMaterial& m) {
    if (index >= materialCpu.size()) {
        core::Logger::warn(core::LogCategory::GLTF, "setMaterial: %u is past the %zu materials this scene has", index,
                           materialCpu.size());
        return;
    }
    materialCpu[index] = m;
    materialEmissive.resize(std::max<size_t>(materialEmissive.size(), index + 1));
    materialEmissive[index] = (m.emissiveFactor.x > 0.0f || m.emissiveFactor.y > 0.0f || m.emissiveFactor.z > 0.0f) ? 1u : 0u;
    ++materialRev;
}

GltfScene::ModelId GltfScene::createMesh(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, MeshData data) {
    if (vertices.buffer == VK_NULL_HANDLE) {
        core::Logger::warn(core::LogCategory::GLTF, "createMesh before a scene was loaded");
        return kNoModel;
    }
    if (data.vertices.empty() || data.indices.empty()) {
        core::Logger::warn(core::LogCategory::GLTF, "createMesh: %zu vertices and %zu indices is not a mesh",
                           data.vertices.size(), data.indices.size());
        return kNoModel;
    }

    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    const auto indexCount = static_cast<uint32_t>(data.indices.size());

    // Checked before anything is allocated, so a rejected mesh leaves no range, no primitive
    // and no half-written run of deltas behind. See `MeshData::morphTargets` for why a short
    // target cannot be padded into something harmless.
    for (size_t t = 0; t < data.morphTargets.size(); ++t) {
        if (data.morphTargets[t].size() == data.vertices.size()) continue;
        core::Logger::warn(core::LogCategory::GLTF,
                           "createMesh: morph target %zu has %zu deltas for %zu vertices; every target must be "
                           "exactly as long as the mesh",
                           t, data.morphTargets[t].size(), data.vertices.size());
        return kNoModel;
    }

    if (!reserveGeometry(ctx, uploader, vertexCount, indexCount)) return kNoModel;

    const uint32_t firstVertex = vertexRanges.allocate(vertexCount);
    if (firstVertex == core::RangeAllocator::kNoRange) return kNoModel;
    const uint32_t firstIndex = indexRanges.allocate(indexCount);
    if (firstIndex == core::RangeAllocator::kNoRange) {
        vertexRanges.free(firstVertex, vertexCount);
        return kNoModel;
    }

    // A degenerate box is the "not stated" signal, because a box that is not a box is the
    // one value a caller cannot have meant.
    glm::vec3 localMin = data.localMin;
    glm::vec3 localMax = data.localMax;
    if (!(localMax.x > localMin.x || localMax.y > localMin.y || localMax.z > localMin.z)) {
        localMin = glm::vec3(std::numeric_limits<float>::max());
        localMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (const Vertex& v : data.vertices) {
            localMin = glm::min(localMin, v.position);
            localMax = glm::max(localMax, v.position);
        }
    }

    if (data.material >= materialCount) {
        core::Logger::warn(core::LogCategory::GLTF, "createMesh: material %u of %u; using 0", data.material,
                           materialCount);
        data.material = 0;
    }

    // The index buffer holds absolute vertex indices, so a mesh built at zero is rebased
    // here rather than carrying an offset -- exactly as an appended file's is.
    for (uint32_t& index : data.indices) index += firstVertex;

    uploader.uploadBufferAt(ctx, vertices, static_cast<VkDeviceSize>(firstVertex) * sizeof(Vertex),
                            data.vertices.data(), static_cast<VkDeviceSize>(vertexCount) * sizeof(Vertex));
    uploader.uploadBufferAt(ctx, indices, static_cast<VkDeviceSize>(firstIndex) * sizeof(uint32_t),
                            data.indices.data(), static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t));

    LoadedModel entry;
    entry.firstVertex = firstVertex;
    entry.vertexCount = vertexCount;
    entry.firstIndex = firstIndex;
    entry.indexCount = indexCount;
    entry.firstMaterial = data.material;
    entry.materialCount = 0;

    Primitive prim;
    prim.firstIndex = firstIndex;
    prim.indexCount = indexCount;
    prim.baseVertex = firstVertex;
    prim.vertexCount = vertexCount;
    prim.materialIndex = static_cast<int32_t>(data.material);
    prim.blended = data.blended;
    prim.masked = data.masked;
    prim.localMin = localMin;
    prim.localMax = localMax;

    // Target-major, into the very array the loader fills: `skinning.comp` has one addressing
    // rule and cannot tell which producer a run came from.
    //
    // **Never freed and never repacked.** An instance already carries `morphOffset`, so
    // reclaiming a run out of the middle renumbers every later primitive under instances
    // that still index it -- which is why `morphData` grows monotonically over an
    // append/unload cycle.
    if (!data.morphTargets.empty()) {
        prim.morphOffset = static_cast<uint32_t>(morphData.size());
        prim.morphTargets = static_cast<uint32_t>(data.morphTargets.size());
        morphData.reserve(morphData.size() + data.morphTargets.size() * data.vertices.size());
        for (const std::vector<MorphDelta>& target : data.morphTargets) {
            morphData.insert(morphData.end(), target.begin(), target.end());
        }

        // **The CPU index copy has to move with it.** `buildSceneAccelStruct` rebases a
        // deformed primitive's indices by reading `indexData()[firstIndex + k]` on the host,
        // and that copy is a snapshot the *loader* took -- so a morphed mesh made afterwards
        // indexes past the end of a vector and the build reads whatever follows it in
        // memory. It does not fault: it hangs the GPU on a structure built over nonsense,
        // five seconds later, as `VK_ERROR_DEVICE_LOST` on an unrelated fence.
        //
        // Only inside this branch. The static tier reads the device buffer by address, so a
        // copy for every prop a game makes would be memory nothing reads.
        const size_t end = static_cast<size_t>(firstIndex) + indexCount;
        if (indexCopy.size() < end) indexCopy.resize(end, 0u);
        std::copy(data.indices.begin(), data.indices.end(), indexCopy.begin() + firstIndex);
    }

    entry.firstPrimitive = static_cast<uint32_t>(prims.size());
    entry.primitiveCount = 1;
    prims.push_back(prim);

    Placement placement;
    placement.primitive = entry.firstPrimitive;
    placement.transform = data.transform;
    placement.node = kNoNode;
    entry.firstPlacement = static_cast<uint32_t>(placedPrims.size());
    entry.placementCount = 1;
    placedPrims.push_back(placement);

    entry.live = true;
    models.push_back(std::move(entry));
    core::Logger::status(core::LogCategory::GLTF, "created mesh: %u verts, %u indices, material %u, %u morph targets",
                         vertexCount, indexCount, data.material, prim.morphTargets);
    return static_cast<ModelId>(models.size() - 1);
}

void GltfScene::unloadModel(const gfx::VulkanContext& ctx, ModelId id) {
    if (id >= models.size() || !models[id].live) return;
    LoadedModel& entry = models[id];

    vertexRanges.free(entry.firstVertex, entry.vertexCount);
    indexRanges.free(entry.firstIndex, entry.indexCount);

    // Descriptors are pointed at the fallback rather than left dangling: one naming a
    // destroyed image is undefined behaviour on the next draw that samples it, and nothing
    // here guarantees no material still does.
    for (const uint32_t slot : entry.textureSlotsUsed) {
        // Holes, for the images that never got a slot -- see `LoadedModel`.
        if (slot == kNoTextureSlot) continue;
        writeTextureDescriptor(ctx, slot, textures[fallbackSlot].view);
        gfx::destroyImage(ctx, textures[slot]);
        freeTextureSlots.push_back(slot);
    }
    entry.textureSlotsUsed.clear();

    // **Truncated only when this model's runs are at the tail.** A run freed out of the
    // middle renumbers every later primitive's `skinOffset` and `morphOffset` under
    // instances that still hold them; at the tail there is nothing later to renumber. That
    // is what keeps import-remove-import from growing without bound.
    if (entry.skinVertexCount > 0 &&
        static_cast<size_t>(entry.firstSkinVertex) + entry.skinVertexCount == skinData.size()) {
        skinData.resize(entry.firstSkinVertex);
    }
    if (entry.morphDeltaCount > 0 &&
        static_cast<size_t>(entry.firstMorphDelta) + entry.morphDeltaCount == morphData.size()) {
        morphData.resize(entry.firstMorphDelta);
    }
    // Cloth records the same way, though a `ClothSource` is self-contained and the only
    // index into the list is a caller's own loop over it.
    if (entry.clothCount > 0 &&
        static_cast<size_t>(entry.firstCloth) + entry.clothCount == clothSourceList.size()) {
        clothSourceList.resize(entry.firstCloth);
    }
    entry.skinVertexCount = 0;
    entry.morphDeltaCount = 0;
    entry.clothCount = 0;

    // Left as holes. Compacting them renumbers every survivor, and the instance table holds
    // those numbers.
    for (uint32_t i = 0; i < entry.placementCount; ++i) {
        placedPrims[entry.firstPlacement + i].primitive = 0;
    }
    // Material slots are not reclaimed either -- a dense array the shaders index directly
    // needs the same renumbering placements just refused. So `prims`, `placedPrims`,
    // `models` and the material count grow monotonically over an append/unload cycle, and a
    // long-lived process cycling models is bounded by `materialCapacity`.
    entry.live = false;
    core::Logger::status(core::LogCategory::GLTF, "unloaded model %u: %u verts and %u indices returned", id,
                         entry.vertexCount, entry.indexCount);
}

void addSceneInstances(const GltfScene& scene, InstanceTable& table) {
    addPlacementInstances(scene, table, 0, static_cast<uint32_t>(scene.placements().size()), nullptr);
}

void addPlacementInstances(const GltfScene& scene, InstanceTable& table, uint32_t firstPlacement, uint32_t count,
                           std::vector<InstanceId>* created) {
    const auto& prims = scene.primitives();
    table.reserve(table.slotCount() + count);

    const uint32_t end = std::min(firstPlacement + count, static_cast<uint32_t>(scene.placements().size()));
    for (uint32_t pi = firstPlacement; pi < end; ++pi) {
        const Placement& p = scene.placements()[pi];
        const Primitive& prim = prims[p.primitive];

        // An instance that exists is an instance that draws -- the invariant every record
        // loop downstream relies on instead of testing `indexCount == 0` itself.
        if (prim.indexCount == 0) continue;

        InstanceDesc desc;
        desc.primitive = p.primitive;
        desc.material = prim.materialIndex >= 0 ? static_cast<uint32_t>(prim.materialIndex) : 0u;
        desc.firstIndex = prim.firstIndex;
        desc.indexCount = prim.indexCount;
        desc.baseVertex = prim.baseVertex;
        desc.vertexCount = prim.vertexCount;
        desc.skinOffset = prim.skinOffset;
        // A node with a `skin` whose mesh has no JOINTS_0 is malformed glTF; taking its word
        // for it dispatches skinning over an array that was never filled.
        desc.skin = prim.skinOffset != 0xFFFFFFFFu ? p.skin : 0xFFFFFFFFu;
        desc.morphOffset = prim.morphOffset;
        desc.morphTargets = prim.morphTargets;
        desc.morphWeightOffset =
            p.node < scene.rig().bind.nodes.size() ? scene.rig().bind.nodes[p.node].firstWeight : 0u;

        // Which character deforms it: the animator's character for that skin, or character 0
        // for a morph-only mesh. Both are defaults a game overrides with `setCharacter` the
        // moment it places a second copy.
        if (desc.skin != 0xFFFFFFFFu) {
            desc.character = desc.skin;
        } else if (desc.morphTargets > 0) {
            desc.character = 0u;
        }
        desc.localMin = prim.localMin;
        desc.localMax = prim.localMax;
        desc.transform = p.transform;
        // Cloth is placed once, into its vertices, so its instance transform stays identity:
        // applying the placement here as well moves the fabric twice. The cost is that a
        // `FABRIC_` mesh cannot be moved by animating its parent -- see limitations.md.
        desc.cloth = prim.clothOffset != 0xFFFFFFFFu;
        if (desc.cloth) desc.transform = glm::mat4(1.0f);
        desc.blended = prim.blended;
        desc.masked = prim.masked;
        const InstanceId id = table.create(desc);
        if (created != nullptr) created->push_back(id);
    }
}

} // namespace scene
