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
 * @brief Where the compressed cache entry for one glTF image would live (4.6a).
 *
 * `<image>.png` -> `<image>.png.ktx2`, keeping the source extension so two images that
 * differ only by extension cannot collide, and so the cache file names the exact file
 * it was built from. An embedded image -- one that lives in a buffer view rather than
 * on disk -- gets `<scene>.image<N>.ktx2` beside the scene, because it has no filename
 * of its own to hang a cache off.
 *
 * Empty means "no cache is possible for this image", which today never happens: both
 * cases are covered. It is a return value rather than an assertion because a future
 * glTF source kind would otherwise be an abort.
 */
/// Takes a `SceneImageRef` rather than a `fastgltf::Image` since C15: the cached path has
/// no document, and this has to answer the same way whichever way the scene was loaded.
/// An empty URI is an embedded payload, which is the case the `<stem>.image<N>.ktx2`
/// fallback exists for -- and the case `writeSceneCache` refuses to bake without one.
std::filesystem::path ktx2CachePath(const std::filesystem::path& scenePath, const SceneImageRef& image,
                                    size_t index) {
    if (!image.uri.empty()) return scenePath.parent_path() / (image.uri + ".ktx2");
    return scenePath.parent_path() / (scenePath.stem().string() + ".image" + std::to_string(index) + ".ktx2");
}

/// stb_image returns malloc'd memory that must go back through stbi_image_free.
struct StbiDeleter {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};

/// Owning the pixels through a unique_ptr is what makes Decoded move-only. A
/// copyable aggregate holding an owning pointer is one accidental copy away from a
/// double free, and the decode fan-out below moves these between threads.
struct Decoded {
    std::unique_ptr<stbi_uc, StbiDeleter> pixels;
    int w = 0;
    int h = 0;
};

/// Decode one glTF image to RGBA8. Thread-safe: touches only its own arguments.
/// The encoded bytes of every embedded image, indexed like `SceneData::images` and empty
/// at every entry whose `uri` is set. Filled only where the scene came from a document,
/// and deliberately **not** part of `SceneData`: it is the one thing the parse produces
/// that the sidecar must not hold, because holding it would make the sidecar a texture
/// format. A cached load has none, and needs none -- `writeSceneCache` refuses a scene
/// whose embedded images have no `.ktx2` for exactly this reason.

/// Two sources, one decode. A URI names a file beside the document; an empty one means
/// the payload was embedded, and `embedded` is where the parse left it.
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

/// The usage flags each of the three shared buffers is made with. Named because they are
/// now written in two places -- `upload` makes the buffers and `growBuffer` remakes them --
/// and a grown buffer missing a flag the original had is a bug that surfaces as a device
/// lost inside an unrelated pass.
///
/// Geometry an acceleration-structure build reads has to say so, and the flags that say so
/// are only legal when VK_KHR_acceleration_structure is enabled -- which is why these take
/// the context rather than being constants.
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
    // Between the parse and the upload, which is the one point the document and the
    // sidecar have both passed through -- see `scaleSceneData`. Remembered so a game
    // deriving its own placements from `boundsMin`/`boundsMax` can tell how big a metre
    // is in the scene it was handed, and so an appended model arrives at the same size.
    scaleSceneData(data, scale);
    sceneScale = scale > 0.0f ? scale : 1.0f;
    return upload(ctx, uploader, path, data, embedded);
}

bool GltfScene::createEmpty(const gfx::VulkanContext& ctx, gfx::Uploader& uploader) {
    // Default-constructed and handed straight to `upload`, rather than a second setup path
    // that makes "the parts an empty scene needs". Every zero here is a case the loader
    // already had to survive -- a document with no images, no materials and no primitives is
    // legal glTF -- so the empty scene is the same scene, and there is nothing that works on
    // one and not the other.
    SceneData empty;
    EmbeddedImages none;
    return upload(ctx, uploader, {}, empty, none);
}

bool GltfScene::upload(const gfx::VulkanContext& ctx, gfx::Uploader& uploader, const std::filesystem::path& path,
                       SceneData& data, EmbeddedImages& embedded) {
    const auto loadStart = std::chrono::steady_clock::now();

    // Adopted rather than copied. `data` is dead after this line and the scene owns what
    // it held -- several megabytes of vertices on a large file, which is worth one move
    // rather than one copy however cheap the rest of the function is.
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
     * Cloth, lifted out of the flat arrays and into one self-contained record each (C19).
     *
     * **This loop is the only reader of `data.clothVertices` and of `Primitive::clothOffset`
     * that ever runs**, and it runs before `data.vertices` is moved into the upload -- which
     * is what makes the pair a file format rather than a live parallel array. See
     * `clothSources()` for why that matters: the shape being avoided is `indexData()`'s,
     * where a snapshot of one array was indexed by an offset in another and one of them
     * grew.
     *
     * Indices are rebased to zero here rather than at use, because a record that needed
     * `baseVertex` to be read would not be self-contained and this whole design is that it
     * is.
     */
    clothSourceList.clear(); // replaced rather than appended to, like every move above
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


    // ---------------------------------------------------------------- textures
    auto texStart = std::chrono::steady_clock::now();
    {
        auto s = core::Profiler::scope("GltfScene::textures");

        // Which slot an image is used in decides its format, and `parseSceneData` already
        // worked it out -- it is `SceneImageRef::srgb`, and it travels in the sidecar for
        // exactly the reason it is not recomputed here: nothing on this side of the split
        // has a document to recompute it from.
        const size_t imageCount = data.images.size();
        textures.resize(imageCount);

        // ------------------------------------------------------ texture cache (4.6a)
        // A `.ktx2` sitting beside the source image, written by scripts/ktx2.py, is used
        // in its place. A *sibling* rather than a rewritten glTF, and rather than the
        // KHR_texture_basisu extension: that extension declares a Basis payload and
        // these files hold plain BC7, so claiming it would be a lie about the contents.
        // What this is is a cache -- the scene file is unmodified, the cache is
        // optional, and a missing or unreadable entry falls back to the PNG silently.
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

        // Decode is pure CPU work and dominated load time when serial, so it is fanned
        // out across every core. Upload is then recorded as a single batch, turning
        // one blocking submit per image into one for the whole scene.
        std::vector<Decoded> decoded(imageCount);

        const unsigned workerCount = std::max(
            1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(imageCount)));
        std::atomic<size_t> nextImage{0};

        auto decodeWorker = [&] {
            auto ws = core::Profiler::scope("GltfScene::decode");
            for (;;) {
                const size_t i = nextImage.fetch_add(1, std::memory_order_relaxed);
                if (i >= imageCount) break;
                // Skipped entirely where the cache answered. This is where the load-time
                // win lives: a PNG that is never decoded costs nothing to decode.
                if (cached[i].valid()) continue;
                decoded[i] = decodeImage(path.parent_path(), data.images[i], embedded[i]);
            }
        };

        {
            auto ds = core::Profiler::scope("GltfScene::decodeAll");
            std::vector<std::thread> workers;
            workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
            // The name goes on the *spawned* workers and not inside `decodeWorker`, because
            // the calling thread runs it too -- naming it there would relabel whichever
            // track called `loadGltf` as "texture decode" for the rest of the run, and on
            // a synchronous load that is the main thread.
            for (unsigned t = 1; t < workerCount; ++t) {
                workers.emplace_back([&decodeWorker] {
                    core::Profiler::nameThread("texture decode");
                    decodeWorker();
                });
            }
            // This thread takes a share of the work too, rather than waiting on the others.
            // Guarded because it allocates -- `decodeImage` and the profiler scope both do --
            // and a `bad_alloc` escaping here would destroy a vector of still-joinable
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
            // A compressed cache entry wins over the decode (4.6a). It was checked for
            // before the decode fan-out, so an image with one was never decoded at all
            // -- which is where most of the load-time saving comes from; the memory
            // saving is the resident BC7 chain being a quarter of RGBA8.
            if (cached[i].valid()) {
                const gfx::Ktx2Image& k = cached[i];
                if (gfx::formatIsSrgb(k.format) != data.images[i].srgb) {
                    // Not fatal, and worth saying out loud: the cache decides the gamma
                    // once it is used, so a stale one silently changes how a texture
                    // looks rather than failing to load.
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

            // addImageWithMips has already copied into its staging buffer, so this
            // decode is dead the moment it returns. Releasing here rather than after
            // the loop keeps the decoded set shrinking as staging grows, instead of
            // holding every decoded texture and every staging copy at full size
            // simultaneously -- which on Sponza is a few hundred MB of peak.
            decoded[i].pixels.reset();
        }

        uploader.endBatch(ctx);
    }
    sceneStats.textureMs = msSince(texStart);


    // ------------------------------------------------------------------ upload
    // Recorded before the table is uploaded and dropped. A light sitting inside the
    // emissive mesh that represents it is the case this exists for, and it is decided by
    // the material rather than by the node so that no scene has to author anything.
    materialEmissive.resize(data.materials.size());
    for (size_t i = 0; i < data.materials.size(); ++i) {
        const glm::vec4& e = data.materials[i].emissiveFactor;
        materialEmissive[i] = (e.x > 0.0f || e.y > 0.0f || e.z > 0.0f) ? 1u : 0u;
    }

    {
        auto s = core::Profiler::scope("GltfScene::upload");

        // Capacity, not size. The buffers are sub-allocated from here on (C10), so they
        // are made with room for a second model to land in without a reallocation -- and
        // `appendModel` grows them when that room runs out. A quarter over is the cheapest
        // headroom that makes the common case (append one prop, append one character) free.
        vertexCapacity = static_cast<uint32_t>(data.vertices.size() + data.vertices.size() / 4 + 1024);
        indexCapacity = static_cast<uint32_t>(data.indices.size() + data.indices.size() / 4 + 1024);
        materialCapacity = static_cast<uint32_t>(data.materials.size() + 64);
        vertexRanges.reset(vertexCapacity);
        indexRanges.reset(indexCapacity);

        const VkDeviceSize vbSize = vertexCapacity * sizeof(Vertex);
        const VkDeviceSize ibSize = indexCapacity * sizeof(uint32_t);
        const VkDeviceSize mbSize = materialCapacity * sizeof(GpuMaterial);

        // Geometry an acceleration structure build reads has to say so, and the flags
        // that say so are only legal when VK_KHR_acceleration_structure is enabled --
        // so this is conditional on the device rather than always on. A device without
        // ray query gets exactly the buffers it got before 3.9.
        // G4 added TRANSFER_SRC to all three: growing a buffer copies the old one into the
        // new one, and the old one is the source.
        vertices = gfx::createBuffer(ctx, vbSize, vertexBufferUsage(ctx), VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                     "sceneVertices");
        indices = gfx::createBuffer(ctx, ibSize, indexBufferUsage(ctx), VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                    "sceneIndices");
        materials = gfx::createBuffer(ctx, mbSize, kMaterialUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0,
                                      "sceneMaterials");

        // The scene's own geometry is the first allocation out of each, which for a fresh
        // allocator is offset zero -- so every existing offset in `prims` is still correct
        // and this stays byte-for-byte what it was.
        const uint32_t firstVertex = vertexRanges.allocate(static_cast<uint32_t>(data.vertices.size()));
        const uint32_t firstIndex = indexRanges.allocate(static_cast<uint32_t>(data.indices.size()));
        materialCount = static_cast<uint32_t>(data.materials.size());
        // Kept rather than dropped (G4): the table is mutable now, and the renderer
        // re-uploads from this copy when `materialRevision` moves.
        materialCpu = data.materials;

        uploader.uploadBufferAt(ctx, vertices, static_cast<VkDeviceSize>(firstVertex) * sizeof(Vertex),
                                data.vertices.data(), data.vertices.size() * sizeof(Vertex));
        uploader.uploadBufferAt(ctx, indices, static_cast<VkDeviceSize>(firstIndex) * sizeof(uint32_t),
                                data.indices.data(), data.indices.size() * sizeof(uint32_t));
        uploader.uploadBufferAt(ctx, materials, 0, data.materials.data(),
                                data.materials.size() * sizeof(GpuMaterial));

        // Kept only for a scene that deforms -- see indices() for what reads it. **Cloth
        // is the third thing that deforms** and was the third condition this test needed:
        // without it a `FABRIC_` instance reached `buildSceneAccelStruct` with no index
        // array to rebase, fell back to the static tier, and traced its rest pose forever
        // -- a curtain that drew correctly and reflected as a flat sheet in mid-air (C19).
        if (!skinData.empty() || !morphData.empty() || !clothSourceList.empty()) {
            indexCopy = std::move(data.indices);
        }
    }

    buildDescriptors(ctx, uploader);

    // ------------------------------------------------- residency self-check (4.6b)
    // The delegation's obligation is to *verify* each named property holds, not to
    // assert it. Three of the four are structural and visible in the code; the free
    // list is the one with behaviour, so it is exercised here: take two slots, give the
    // first back, and check the next acquire returns exactly it. A free list that hands
    // out a slot twice, or forgets one, fails this immediately -- and would otherwise
    // fail the day a residency system started using it, months from now.
    //
    // Debug only. It costs three vector operations and a descriptor write, and its
    // value is entirely in catching a regression on the build that introduces one.
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

    // No path means `createEmpty`, and the summary below is a report on a document. One line
    // instead, naming the two numbers that are not zero -- what a mesh built in code has room
    // to land in, which is the only question anyone asks of an empty scene.
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
    // Reported whichever way the load went, and reported as zero when there are none. A run
    // with no chains is a run whose LOD selection has nothing to select between, and that is
    // the difference between a golden case that checks the coverage test and one that only
    // looks as though it does -- so it belongs in the log every case captures.
    core::Logger::status(core::LogCategory::GLTF, "  LOD chains: %u primitives, %lu of those indices (%s)",
                   sceneStats.lodPrimitives, static_cast<unsigned long>(sceneStats.lodIndices),
                   sceneStats.lodPrimitives > 0 ? "baked" : "none; bake with substrate-bake");
    core::Logger::status(core::LogCategory::GLTF, "  bounds min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f)", boundsMin.x, boundsMin.y,
                   boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);
    core::Logger::status(core::LogCategory::GLTF, "  timing: cache=%.1fms parse=%.1fms geometry=%.1fms textures=%.1fms total=%.1fms",
                   sceneStats.cacheMs, sceneStats.parseMs, sceneStats.geometryMs, sceneStats.textureMs,
                   sceneStats.totalMs);
    // The split inside `parse` (C13). Reported always rather than behind a flag: the
    // three numbers are what rank C14 against C15, and a number nobody prints is one
    // nobody has.
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

    // ------------------------------------------------------- texture slots (4.6b)
    // The array is sized past what the scene loaded. The extra slots are what a
    // residency system streams into: property (i) of the delegation says swapping a
    // slot's contents must touch no pipeline, and that holds only if the slot exists in
    // the descriptor array to begin with.
    //
    // A *stated* capacity, not a hidden one. `acquireTextureSlot()` reports when it is
    // exhausted and returns an invalid slot rather than handing back one that is in
    // use, which is the difference between a budget and the silent truncations 0.9 and
    // 0.10 exist to fix.
    const uint32_t loaded = static_cast<uint32_t>(textures.size());
    fallbackSlot = loaded;
    textureSlots = std::max<uint32_t>(1, loaded + 1 + kTextureSlotHeadroom);

    // The reserved fallback (property iv): 1x1 opaque white, which reads as "no texture"
    // through every material slot -- a base colour multiplied by white is the factor
    // alone, and an occlusion or roughness map of white is the neutral value. Every free
    // slot's descriptor points here, so sampling an unresident texture returns something
    // defined rather than whatever the slot last held.
    textures.resize(textureSlots);
    textures[fallbackSlot] = gfx::createImage(ctx, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                              "fallbackTexture");
    // **And its one texel is written, which for a long time it was not.** The image was
    // created and never uploaded, so it sat in UNDEFINED with whatever the allocator
    // handed over -- zero on this driver, which is transparent black rather than opaque
    // white. Nothing noticed because the only readers are a material whose image failed to
    // decode (no scene here has one) and `sampleOr`, which short-circuits on a negative
    // index and never touches the image at all. A decal naming this slot discarded every
    // fragment, since alpha zero is what the pass tests.
    const uint32_t whiteTexel = 0xFFFFFFFFu;
    uploader.uploadImageWithMips(ctx, textures[fallbackSlot], &whiteTexel, sizeof(whiteTexel));

    // Slots past the fallback are free, highest first so the first acquire returns the
    // lowest index -- which makes a capture easier to read than a reversed one.
    freeTextureSlots.clear();
    for (uint32_t slot = textureSlots; slot > fallbackSlot + 1; --slot) freeTextureSlots.push_back(slot - 1);

    createSceneDescriptors(ctx);
}

void GltfScene::createSceneDescriptors(const gfx::VulkanContext& ctx) {
    const uint32_t texCount = textureSlots;

    // binding 0: material storage buffer, binding 1: every texture in the scene.
    // One set for the whole scene means primitives differ only by push constants.
    //
    // Compute as well as fragment: the tracing passes shade the surface a ray hit, which
    // means reading the same materials and the same bindless textures the G-buffer pass
    // reads, from a compute shader. Exposing the stage costs nothing where nothing uses
    // it -- the alternative is a second identical layout differing only in a stage flag.
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
        // A slot with no image points at the fallback rather than being left unwritten.
        // PARTIALLY_BOUND would permit the latter, and it would also mean that a shader
        // reading a free slot reads undefined data -- which is exactly the state
        // property (iv) exists to remove.
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

    // The model registry goes with them. A `ModelId` is an index into `models`, so leaving
    // the vector populated leaves every id a caller is holding *valid* against a scene that
    // no longer exists -- and `unloadModel` would then hand ranges back to allocators that
    // never issued them, which `RangeAllocator` documents it cannot detect. Cleared, the
    // `id >= models.size()` guard at the top of `unloadModel` turns a stale id into the
    // no-op it should be. `upload()` resets the range allocators and rebuilds the texture
    // free list itself, so those are its business, not this function's -- except for the
    // free list, which must not survive to be appended to by a `destroy()` with no `upload()`
    // behind it.
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

    // Doubling, not exactly-enough: a composed world imports in bursts, and rebuilding the
    // descriptor set costs a pipeline rebuild at the call site.
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

    // The scale the scene was loaded at, so a model appended into it arrives in the same
    // units. A file appended at 1x into a 4x world would be a doll's house. Since C41 that
    // scale can be the *game's* -- `Engine::setWorldScale` -- rather than only what `load`
    // resized a document by, because a composed world has no single document to take it from.
    scaleSceneData(data, sceneScale);
    // **Captured before the placement is baked in**, because after it the file's own node
    // hierarchy is indistinguishable from where the caller put the import -- see
    // `LoadedModel::placementLocals`.
    std::vector<glm::mat4> placementLocals;
    placementLocals.reserve(data.placements.size());
    for (const Placement& p : data.placements) placementLocals.push_back(p.transform);

    // Then where the caller wants it. Scale first and place second, deliberately: the
    // caller's transform is in world units, so a scale applied after it would multiply the
    // placement as well and put the import at twice the distance it asked for.
    placeSceneData(data, transform);

    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    const auto indexCount = static_cast<uint32_t>(data.indices.size());

    // Both ranges or neither. Taking the first and failing the second would leak it, and
    // this is the one place a leak is invisible -- the buffer simply has less room next
    // time and nothing says why.
    // G4. The buffers grow rather than the append being refused -- C10 shipped with the
    // refusal and said so, and this is the increment it named. Growth is a copy under the
    // device wait `Engine::addModel` already takes, so it is an explicit load event's cost
    // rather than a frame's.
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

    // ------------------------------------------------------------------ textures
    // An image that cannot have a slot falls back rather than failing the load: a model
    // with the wrong texture is recoverable and a model that did not appear is not.
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

    // ------------------------------------------------------------------ geometry
    // The index buffer holds absolute vertex indices, so an appended model's indices are
    // rebased here rather than carried as a `vertexOffset` -- which the draw already uses
    // for something else and which the skinning dispatch reads as a vertex range.
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

    // -------------------------------------------------------- what deforms (C22)
    //
    // **The three arrays a deforming primitive indexes are scene-wide, and this is where
    // they stop being fixed at load.** `appendModel` refused anything that deformed until
    // now, and the refusal was correct: `skinOffset` and `morphOffset` are absolute offsets
    // into `skinData` and `morphData`, so an import carrying its own offsets would take a
    // character's influences from whatever the base scene had at that address -- silently,
    // and only for the frames it is on screen.
    //
    // Appended rather than repacked, for the reason `createMesh` gives one array along: an
    // instance already carries these offsets, so reclaiming a run out of the middle would
    // renumber every later primitive under instances that still index them.
    const bool deforms = !data.skinVertices.empty() || !data.morphDeltas.empty() || !data.clothVertices.empty();
    const auto skinBase = static_cast<uint32_t>(skinData.size());
    const auto morphBase = static_cast<uint32_t>(morphData.size());
    skinData.insert(skinData.end(), data.skinVertices.begin(), data.skinVertices.end());
    morphData.insert(morphData.end(), data.morphDeltas.begin(), data.morphDeltas.end());
    entry.firstSkinVertex = skinBase;
    entry.skinVertexCount = static_cast<uint32_t>(data.skinVertices.size());
    entry.firstMorphDelta = morphBase;
    entry.morphDeltaCount = static_cast<uint32_t>(data.morphDeltas.size());

    // **And the CPU index copy, which is the array that has to move with them.**
    // `buildSceneAccelStruct` rebases a deformed primitive's indices onto the deformed
    // vertex buffer by reading `indexData()[firstIndex + k]` -- on the *host*, so the device
    // buffer is no use to it. That copy is a snapshot `load` took, and a scene that
    // deformed nothing did not take one at all, so an imported rig indexes past the end of
    // a vector and the build is handed whatever follows it in memory.
    //
    // It does not fault. It hangs the GPU on a structure built over nonsense, five seconds
    // later, as `VK_ERROR_DEVICE_LOST` on an unrelated upload fence -- which is precisely
    // what it did on the first end-to-end run of this card, and precisely what `createMesh`
    // records having done one array along. Two vectors laid out to match, one of them
    // grown, for the third time.
    if (deforms) {
        const size_t end = static_cast<size_t>(firstIndex) + indexCount;
        if (indexCopy.size() < end) indexCopy.resize(end, 0u);
        std::copy(data.indices.begin(), data.indices.end(), indexCopy.begin() + firstIndex);
    }

    // Cloth is the one that needs no offset shift, and it is worth saying why rather than
    // leaving it looking like an omission: `Primitive::clothOffset` is a *file format*, read
    // once when the vertices are lifted into self-contained `ClothSource` records and never
    // again. So the import's records are built here from the import's own arrays, exactly as
    // `load` builds the base scene's, and pushed onto the same list.
    entry.firstCloth = static_cast<uint32_t>(clothSourceList.size());
    for (uint32_t pi = 0; pi < data.primitives.size(); ++pi) {
        const Primitive& prim = data.primitives[pi];
        if (prim.clothOffset == 0xFFFFFFFFu || prim.vertexCount == 0) continue;
        if (static_cast<size_t>(prim.clothOffset) + prim.vertexCount > data.clothVertices.size()) continue;
        if (static_cast<size_t>(prim.baseVertex) + prim.vertexCount > data.vertices.size()) continue;

        ClothSource src;
        // The index this record reports is into the *scene's* primitive array, which the
        // loop below is about to append to at `entry.firstPrimitive`.
        src.primitive = static_cast<uint32_t>(prims.size()) + pi;
        src.vertices.assign(data.vertices.begin() + prim.baseVertex,
                            data.vertices.begin() + prim.baseVertex + prim.vertexCount);
        src.masses.assign(data.clothVertices.begin() + prim.clothOffset,
                          data.clothVertices.begin() + prim.clothOffset + prim.vertexCount);
        src.indices.reserve(prim.indexCount);
        for (uint32_t k = 0; k < prim.indexCount; ++k) {
            const size_t at = static_cast<size_t>(prim.firstIndex) + k;
            if (at >= data.indices.size()) break;
            // Zero-based, off the import's own pre-rebase indices. `data.indices` has
            // already been shifted by `firstVertex` above, and `prim.baseVertex` has not
            // yet -- so both terms are in the scene's space and the difference is the
            // primitive-local index this record is defined to hold.
            src.indices.push_back(data.indices[at] - (prim.baseVertex + firstVertex));
        }
        clothSourceList.push_back(std::move(src));
    }
    entry.clothCount = static_cast<uint32_t>(clothSourceList.size()) - entry.firstCloth;

    // ------------------------------------------------------------------ records
    entry.firstPrimitive = static_cast<uint32_t>(prims.size());
    entry.primitiveCount = static_cast<uint32_t>(data.primitives.size());
    for (Primitive& p : data.primitives) {
        if (p.skinOffset != 0xFFFFFFFFu) p.skinOffset += skinBase;
        if (p.morphTargets > 0) p.morphOffset += morphBase;
        // Cleared rather than shifted. It named an offset into a file-local array that no
        // longer exists anywhere, and the `ClothSource` above is what carries the data now
        // -- but `addPlacementInstances` reads it as the *flag* that makes an instance
        // cloth, so it has to stay set where the primitive really is cloth.
        if (p.clothOffset != 0xFFFFFFFFu) p.clothOffset = 0u;
        p.firstIndex += firstIndex;
        // The chain is rebased in the same loop as the range it extends, because it *is*
        // ranges of the same buffer -- and because it lives on the primitive rather than in
        // an array beside it, this is the only place that has to know. A per-level array
        // indexed by primitive would need a second loop here, a second one in
        // `writeSceneCache`, and would be wrong the first time either was forgotten.
        for (uint32_t l = 0; l < p.lodCount && l < kMaxLodLevels; ++l) p.lods[l].firstIndex += firstIndex;
        p.baseVertex += firstVertex;
        if (p.materialIndex >= 0) p.materialIndex += static_cast<int32_t>(materialBase);
        prims.push_back(p);
    }

    // Node indices are file-local and three records match on them, so the whole import is
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
        // `pl.skin` is left **file-local** on purpose. Where the import's skins land is the
        // animator's answer and the animator is not reachable from here, so the shift is
        // `rebaseAppendedSkins`, called after the merge.
        placedPrims.push_back(pl);
    }

    // C22. Kept rather than merged, because `sceneRig` was moved out into the animator at
    // load and merging into what is left would produce a rig nothing reads.
    entry.skinCount = static_cast<uint32_t>(data.rig.skins.size());
    entry.importedRig = std::move(data.rig);

    // ------------------------------------------------- what a scene has besides geometry
    // C21. Before this, an import brought its meshes and left its colliders, its lights,
    // its emitters and its sounds in the file -- so a mirror could be imported and the
    // thing it hung from could not, and `make_composite_scene.py` existed to graft what
    // could not be composed at runtime.
    entry.firstCollider = static_cast<uint32_t>(sceneColliders.size());
    entry.colliderCount = static_cast<uint32_t>(data.colliders.size());
    for (ColliderDesc& c : data.colliders) {
        shiftNode(c.node);
        sceneColliders.push_back(std::move(c));
    }

    entry.firstLight = static_cast<uint32_t>(sceneLights.size());
    entry.lightCount = static_cast<uint32_t>(data.lights.size());
    // Already world-space, and `placeSceneData` has put them where the caller asked. A
    // light carries no node, which is why this one is a plain append.
    sceneLights.insert(sceneLights.end(), data.lights.begin(), data.lights.end());

    entry.firstEmitter = static_cast<uint32_t>(sceneEmitters.size());
    entry.emitterCount = static_cast<uint32_t>(data.emitters.size());
    for (ParticleEmitter& e : data.emitters) {
        shiftNode(e.node);
        // Through the same table the materials went through, and for the same reason: an
        // emitter names an *image* index and the descriptor array is addressed by slot.
        // Without this an imported model that brought both an emitter and its sheet drew
        // whatever texture happened to occupy that slot, silently.
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

    // **The scene's bounds grow with what was put in it** (C41). `load` sets them and
    // `appendModel` did not, which was invisible while every scene arrived through `load`
    // and every import was a prop inside one. A game that composes its world out of imports
    // has no `load` at all, so without this `Camera::frameBounds` frames an empty box and
    // the shadow cascades are fitted to nothing.
    //
    // The import's own bounds are in its local space, so the caller's transform has to be
    // applied -- all eight corners, because a rotation turns a box into one whose axis-aligned
    // extent is larger than any two transformed corners would say.
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

    // Before the move, not after. Only the vector member is left empty by it and every
    // field read here is a `uint32_t`, so reading through would work and would be exactly
    // the line someone deletes a field into later.
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
        // `kNoNode` here is glTF's "this placement is not skinned", and adding a base to it
        // would make an unskinned crate name skin 0 of the base scene's character.
        if (pl.skin != kNoNode) pl.skin += skinBase;
    }
}


// ============================================================ growth (G4)

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

    // Everything the old buffer held, not everything it could hold: the tail past
    // `oldCapacity` was never written, and copying it would be reading uninitialised
    // device memory to no purpose.
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
    // Doubling, floored at what is needed *plus* what is already there. Growing to exactly
    // the request is growth that happens again on the next append, and each one is a
    // full-buffer copy under a device wait.
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
    // The buffer moved, so the one descriptor naming it has to be rewritten. Not
    // `buildDescriptors`, which creates the sampler and the fallback image -- both of
    // which already exist and would leak.
    writeMaterialDescriptor(ctx);
    return true;
}

// ============================================================ materials (G4)

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
    // The first row rather than past the end, for the reason `settings::row` gives about
    // its own out-of-range case: a bounds bug should be a wrong answer in one place rather
    // than a crash somewhere later.
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

// ============================================================== createMesh (G4)

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

    // Refused before anything is allocated, so a rejected mesh leaves no range, no
    // primitive and no half-written run of deltas behind. See `MeshData::morphTargets`
    // for why a short target cannot be padded into something harmless.
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

    // Bounds from the vertices unless the caller stated them. A degenerate box is the
    // signal, because a box that is not a box is the one value that cannot be meant.
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

    // The index buffer holds absolute vertex indices, exactly as it does for an appended
    // file -- so a mesh built at zero is rebased here rather than carrying an offset.
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
    entry.materialCount = 0; ///< it owns no material: the caller's outlives it

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

    // ------------------------------------------------------------------ morph targets (G11)
    //
    // Appended to the very array the loader fills, in the same target-major order, so a
    // code-made target and a file-authored one are one buffer and one addressing rule --
    // `Renderer::setAnimator` re-uploads the whole of it and `skinning.comp` cannot tell
    // which producer a run came from.
    //
    // **Never freed and never repacked**, exactly like the material slots below: an
    // instance already carries `morphOffset`, so reclaiming a run out of the middle would
    // renumber every later primitive under instances that still index them. That is the
    // same refusal `unloadModel` makes about placements, one array along, and it is why
    // `morphData` grows monotonically over an append/unload cycle.
    if (!data.morphTargets.empty()) {
        prim.morphOffset = static_cast<uint32_t>(morphData.size());
        prim.morphTargets = static_cast<uint32_t>(data.morphTargets.size());
        morphData.reserve(morphData.size() + data.morphTargets.size() * data.vertices.size());
        for (const std::vector<MorphDelta>& target : data.morphTargets) {
            morphData.insert(morphData.end(), target.begin(), target.end());
        }

        // **And the CPU index copy, which is the array that has to move with it.**
        // `buildSceneAccelStruct` rebases a deformed primitive's indices onto the deformed
        // vertex buffer by reading `indexData()[firstIndex + k]` -- the device buffer is no
        // use to it, because the rebase happens on the host. That copy is a snapshot the
        // *loader* took, so a morphed mesh made afterwards indexes past the end of a vector
        // and the build is handed whatever follows it in memory. It does not fault: it
        // hangs the GPU on a structure built over nonsense, five seconds later, as
        // `VK_ERROR_DEVICE_LOST` on an unrelated fence.
        //
        // This is precisely G12's defect one array along -- two vectors laid out to match,
        // one of them grown -- and it is why the copy is written here rather than left to
        // agree by construction. Only a mesh that deforms needs it: the static tier reads
        // the device buffer by address and never touches this, so a copy of every prop a
        // game makes would be memory nothing reads.
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

    // The slots go back on the free list and their descriptors are pointed at the fallback
    // rather than left dangling. A descriptor naming a destroyed image is undefined
    // behaviour on the next draw that samples it, and nothing here guarantees no material
    // still does.
    for (const uint32_t slot : entry.textureSlotsUsed) {
        // Holes, for the images that never got one. The list is image-indexed so
        // `modelTextureSlot` can answer -- see LoadedModel.
        if (slot == kNoTextureSlot) continue;
        writeTextureDescriptor(ctx, slot, textures[fallbackSlot].view);
        gfx::destroyImage(ctx, textures[slot]);
        freeTextureSlots.push_back(slot);
    }
    entry.textureSlotsUsed.clear();

    // **The deform arrays are truncated when this model's runs are at the tail, and left
    // alone otherwise** (C22). A run freed out of the middle would renumber every later
    // primitive's `skinOffset` and `morphOffset` under instances that still hold them --
    // the same refusal placements and materials make below. At the tail there is nothing
    // later to renumber, so the space goes back.
    //
    // That covers the case that would otherwise grow without bound: import a rig, remove
    // it, import it again. `character.gltf` is 0.9 MB of influences, so a thousand cycles
    // is the difference between a flat high-water mark and 900 MB.
    if (entry.skinVertexCount > 0 &&
        static_cast<size_t>(entry.firstSkinVertex) + entry.skinVertexCount == skinData.size()) {
        skinData.resize(entry.firstSkinVertex);
    }
    if (entry.morphDeltaCount > 0 &&
        static_cast<size_t>(entry.firstMorphDelta) + entry.morphDeltaCount == morphData.size()) {
        morphData.resize(entry.firstMorphDelta);
    }
    // Cloth records the same way, and they are easier: a `ClothSource` is self-contained, so
    // the only index into the list is the caller's own loop over it.
    if (entry.clothCount > 0 &&
        static_cast<size_t>(entry.firstCloth) + entry.clothCount == clothSourceList.size()) {
        clothSourceList.resize(entry.firstCloth);
    }
    entry.skinVertexCount = 0;
    entry.morphDeltaCount = 0;
    entry.clothCount = 0;

    // Placements are left as holes. Compacting them would renumber every survivor, and the
    // instance table holds those numbers -- which is the same argument the allocator makes
    // for a free list over compaction, one level up.
    for (uint32_t i = 0; i < entry.placementCount; ++i) {
        placedPrims[entry.firstPlacement + i].primitive = 0;
    }
    // Material slots are not reclaimed: they are a dense array the shaders index directly,
    // and a hole in it would need the same renumbering placements just refused.
    //
    // So `prims`, `placedPrims`, `models` and the material count all grow monotonically over
    // an append/unload cycle, and only a `destroy()`/`upload()` gives any of it back. That is
    // the price of stable indices and it is the right one at the scale this runs -- but it
    // does mean a long-lived process cycling models is bounded by `materialCapacity`, which
    // `upload()` sizes at the scene's own count plus 64.
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

        // A primitive with no indices draws nothing. Dropping it here rather than
        // testing `indexCount == 0` in four record loops is the point of the table:
        // an instance that exists is an instance that draws.
        if (prim.indexCount == 0) continue;

        InstanceDesc desc;
        desc.primitive = p.primitive;
        desc.material = prim.materialIndex >= 0 ? static_cast<uint32_t>(prim.materialIndex) : 0u;
        desc.firstIndex = prim.firstIndex;
        desc.indexCount = prim.indexCount;
        desc.baseVertex = prim.baseVertex;
        desc.vertexCount = prim.vertexCount;
        desc.skinOffset = prim.skinOffset;
        // A skin is only a skin if the primitive actually carries influences. A node
        // with a `skin` whose mesh has no JOINTS_0 is malformed, and taking its word
        // for it would dispatch skinning over an array that was never filled.
        desc.skin = prim.skinOffset != 0xFFFFFFFFu ? p.skin : 0xFFFFFFFFu;
        desc.morphOffset = prim.morphOffset;
        desc.morphTargets = prim.morphTargets;
        desc.morphWeightOffset =
            p.node < scene.rig().bind.nodes.size() ? scene.rig().bind.nodes[p.node].firstWeight : 0u;

        // Which character deforms it. A skinned mesh takes the one the animator creates
        // for its skin; a morph-only mesh takes character 0, which is the single
        // character a rig with no skin gets and the first of however many it has. Both
        // are defaults a game overrides with `setCharacter` the moment it places a
        // second copy -- see property (ii) in 4.1b, which is the same argument.
        if (desc.skin != 0xFFFFFFFFu) {
            desc.character = desc.skin;
        } else if (desc.morphTargets > 0) {
            desc.character = 0u;
        }
        desc.localMin = prim.localMin;
        desc.localMax = prim.localMax;
        desc.transform = p.transform;
        // Cloth is placed once, into its vertices, and its instance transform stays
        // identity from then on (C19). A soft body has no rigid transform to push down a
        // node hierarchy, so applying the placement here as well would move the fabric
        // twice -- and the *stated* consequence is that a `FABRIC_` mesh cannot be moved by
        // animating its parent, which limitations.md records.
        desc.cloth = prim.clothOffset != 0xFFFFFFFFu;
        if (desc.cloth) desc.transform = glm::mat4(1.0f);
        desc.blended = prim.blended;
        desc.masked = prim.masked;
        const InstanceId id = table.create(desc);
        if (created != nullptr) created->push_back(id);
    }
}

} // namespace scene
