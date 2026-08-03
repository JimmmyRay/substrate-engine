#include "gfx/Renderer.h"

#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/FrameCapture.h"
#include "gfx/Pipeline.h"
#include "gfx/SpirvReflect.h"
#include "scene/Camera.h"
#include "scene/GltfScene.h"
#include "scene/MeshLod.h"

#include <GLFW/glfw3.h>

#include <stb_image.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
// The MS CRT spells these with a leading underscore and is otherwise identical. Three
// defines rather than an #ifdef at each call site, so the shader-reload code below stays
// readable as the POSIX it was written as -- and because these are the only three uses in
// the engine, which makes a mapping cheaper than a wrapper nobody else calls.
#define popen _popen
#define pclose _pclose
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace gfx {



namespace {

/**
 * @brief Make a device write to host-visible memory visible to *this* thread.
 *
 * Waiting on a fence proves the copy executed. It does not prove the host can see it:
 * that only holds for `HOST_COHERENT` memory, and `VMA_MEMORY_USAGE_AUTO` is entitled to
 * pick a heap that is not. On the three readbacks below -- a screenshot, an intermediate
 * target, and the cull counters -- the difference is between reading the frame that was
 * drawn and reading whatever the CPU's cache line held before it.
 *
 * `vmaInvalidateAllocation` is a no-op on coherent memory, so this costs nothing in the
 * case that already worked and closes the case that only mostly did. It is deliberately
 * *not* paired with a flush on the upload path: those buffers are written by the host and
 * read by the device, which the queue submit already orders.
 *
 * File-local rather than beside `createBuffer` because all three callers are in this
 * translation unit, and that is the narrowest scope that reaches them.
 */
void invalidateForHostRead(const VulkanContext& ctx, const GpuBuffer& buffer) {
    if (buffer.allocation == VK_NULL_HANDLE) return;
    vkCheck(vmaInvalidateAllocation(ctx.allocator, buffer.allocation, 0, VK_WHOLE_SIZE),
            "vmaInvalidateAllocation");
}

constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_SRGB;
/// Two channels, not four: the normal is octahedrally encoded (3.5). SFLOAT rather
/// than UNORM because R16G16_SFLOAT is on Vulkan's mandatory colour-attachment list
/// and R16G16_UNORM is not -- see the reasoning at the top of octahedral.glsl.
constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kOrmFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kEmissiveFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
/// RGBA16F rather than the R11G11B10 the emissive target uses: this one is a storage
/// image, and RGBA16F is on the guaranteed storage-image format list while
/// B10G11R11_UFLOAT is not.
constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R8G8B8A8_UNORM;

/// The G-buffer and forward passes have no push constants at all (0.11). Everything
/// that used to be pushed per draw -- the model matrix, the normal matrix, the
/// material index -- is in the instance buffer, and a vertex shader finds its own
/// record through `gl_InstanceIndex`. What is left is a pass-level bind and a single
/// vkCmdDrawIndexedIndirect.

/// A colour attachment that keeps what is already there. Seven passes want exactly this
/// -- particles, decals, SSR composite, fog composite, forward blend, the debug overlay
/// and the UI -- and each of them spelled the same five lines out until D4.
VkRenderingAttachmentInfo colorAttachment(VkImageView view) {
    VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    return info;
}

/// The same attachment, cleared first. The argument *is* the distinction: an attachment
/// given a clear value clears, one given none loads. That was two functions' worth of
/// difference written out at nine call sites, and the CLEAR half had a helper while the
/// LOAD half did not -- an extraction reaching two of its nine callers.
VkRenderingAttachmentInfo colorAttachment(VkImageView view, const VkClearColorValue& clear) {
    VkRenderingAttachmentInfo info = colorAttachment(view);
    info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    info.clearValue.color = clear;
    return info;
}

/// The one interleaved vertex stream every geometry pass binds at binding 0. Three of
/// them describe different *attributes* out of it -- the G-buffer all four, the shadow
/// pass position and UV, velocity position alone -- but the binding is the same buffer at
/// the same stride, and stating it once is what keeps them in step.
VkVertexInputBindingDescription sceneVertexBinding() {
    return {0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
}

void setViewportScissor(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

/// Must match the Push block in skinning.comp exactly.
struct SkinPush {
    uint32_t sourceBase;
    uint32_t destBase;
    uint32_t influenceBase;
    uint32_t jointBase;
    uint32_t vertexCount;
    uint32_t morphBase;
    uint32_t morphTargets;
    uint32_t weightBase;
};
static_assert(sizeof(SkinPush) <= 128, "push constants must fit the guaranteed minimum");

/// Storage buffers skinning.comp binds: source vertices, deformed output, influences,
/// joint matrices, morph deltas, morph weights. Named because it is written in three
/// places -- the layout, the descriptor writes and the shader -- and the reflection
/// check in verifyShaderBindings is what catches the fourth if one is ever missed.
constexpr uint32_t kSkinBindings = 6;

/// Must match the Push block in cull.comp exactly.
struct CullPush {
    glm::mat4 viewProj;
    /// Immediately after the matrix, because a vec4 wants a 16-byte offset and putting it
    /// after the scalars below would pad the block past the 128 bytes Vulkan guarantees.
    glm::vec4 lodThresholds;
    uint32_t commandCount;
    uint32_t outOffset;
    uint32_t enabled;
    uint32_t viewIndex;
    uint32_t phase;
    uint32_t occlusionEnabled;
    glm::vec2 pyramidSize;
    uint32_t pyramidLevels;
    /// Where the triangle counters start in the stats buffer, in uints.
    uint32_t statsStride;
    /// C17, and set for the camera view alone. A shadow cascade is orthographic and covers
    /// the world rather than the screen, so "how much of the viewport does this cover" is
    /// not a question it can be asked -- and a caster dropping a level while the surface it
    /// shades keeps its own is a shadow that stops fitting what casts it.
    uint32_t lodEnabled;
};
static_assert(sizeof(CullPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in depth_pyramid.comp exactly.
struct DepthPyramidPush {
    glm::uvec2 destSize;
    uint32_t sourceLod;
};
static_assert(sizeof(DepthPyramidPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in light_tile_body.glsl exactly.
struct LightTilePush {
    glm::uvec2 extent;
    glm::uvec2 tiles;
    uint32_t samples;
};
static_assert(sizeof(LightTilePush) <= 128, "push constants must fit the guaranteed minimum");

/// Shared by all three bloom passes. One block rather than three keeps a single
/// pipeline layout across the chain; the fields each pass ignores cost nothing.
/// Must match the Push block in bloom_*.comp exactly.
struct BloomPush {
    glm::vec2 dstTexel; ///< 1 / destination mip size
    float srcLod;
    float threshold;
    float knee;
    float strength;
};
static_assert(sizeof(BloomPush) <= 128, "push constants must fit the guaranteed minimum");

/// Shared by ssao.comp and ssao_blur.comp. Must match their Push blocks exactly.
struct SsaoPush {
    glm::vec2 texel;
    float radius;
    float bias;
    float intensity;
    uint32_t sampleCount;
};
static_assert(sizeof(SsaoPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in ssr_body.glsl exactly.
///
/// The five addresses are read only by the ray-traced variants, which declare them and
/// the pad; the screen-space variants declare a shorter block and read a prefix of the
/// same range. One struct rather than two, at the cost of 40 bytes the march ignores.
struct SsrPush {
    glm::vec2 texel;
    float maxDistance;
    float thickness;
    float intensity;
    float roughnessCutoff;
    uint32_t stepCount;
    uint32_t refineSteps;
    /// Non-zero to trace a shadow ray per light at each reflection hit, so a reflected
    /// surface is shadowed the same way the world one is. Named in the shader too, and
    /// so is `pad`: a device address needs 8-byte alignment and the fields above come to
    /// 36, and two compilers agreeing by accident about implicit padding is one compiler
    /// away from reading garbage.
    uint32_t shadowLights;
    uint32_t pad;
    VkDeviceAddress hitRecords;
    VkDeviceAddress sceneVertices;
    VkDeviceAddress sceneIndices;
    VkDeviceAddress deformedVertices;
    VkDeviceAddress deformedIndices;
};
static_assert(sizeof(SsrPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in decal.frag exactly.
struct DecalPush {
    glm::mat4 worldToDecal;
    glm::vec4 tint;
    uint32_t textureIndex;
    float edgeFade;
    /// `Decal::round` as a uint, because a GLSL bool in a push block is four bytes with a
    /// layout rule nobody remembers and a `uint` compared against zero has neither.
    uint32_t round;
};
static_assert(sizeof(DecalPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in fog.comp exactly.
struct FogPush {
    glm::vec2 texel;
    float density;
    float anisotropy;
    float maxDistance;
    float heightFalloff;
    float baseHeight;
    uint32_t stepCount;
};
static_assert(sizeof(FogPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in taa.comp exactly.
struct TaaPush {
    glm::vec2 texel;
    float blend;
    float valid;
};
static_assert(sizeof(TaaPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in particle_emit.comp exactly.
struct ParticleEmitPush {
    uint32_t spawnCount;
    uint32_t indexBits;
    float sortRange;
    float now;
};
static_assert(sizeof(ParticleEmitPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in particle_simulate.comp exactly.
struct ParticleSimPush {
    float dt;
    float now;
    float sortRange;
    /// Fourth, where `nearPlane` used to be, because `texel` is a `vec2` and std430
    /// aligns those to 8. The shader would insert a hole this struct does not.
    float collisionThickness;
    glm::vec2 texel;
    uint32_t capacity;
    uint32_t indexBits;
};
static_assert(sizeof(ParticleSimPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in particle_sort.comp and particle_sort_local.comp, which
/// share it: one struct because the two shaders are two halves of one network, and a
/// second three-word struct differing in which field is ignored would be noise.
struct ParticleSortPush {
    uint32_t k;
    uint32_t j;
    uint32_t capacity;
    uint32_t pad;
};
static_assert(sizeof(ParticleSortPush) <= 128, "push constants must fit the guaranteed minimum");

/// Must match the Push block in particle.vert exactly.
struct ParticleDrawPush {
    float now;
    uint32_t indexBits;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(ParticleDrawPush) <= 128, "push constants must fit the guaranteed minimum");

/// Elements one workgroup of particle_sort_local.comp holds. Must match BLOCK there.
constexpr uint32_t kParticleSortBlock = 256;

/**
 * @brief One term of the Halton low-discrepancy sequence.
 *
 * `index` is 1-based: Halton(0) is 0 for every base, which would make the first jitter
 * offset the pixel centre and waste one frame of the cycle on the sample MSAA already
 * takes.
 */
float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

/// The overlay's only push constant: 1 / framebuffer extent.
struct OverlayPush {
    float invScreenX;
    float invScreenY;
};

constexpr uint32_t kOverlayWhite = 0xFFFFFFFFu;
constexpr uint32_t kOverlayShadow = 0xC0000000u; ///< 75% black, for the drop shadow

/**
 * @brief An extent scaled by a fraction, never smaller than 1x1.
 *
 * `scale >= 1.0f` returns `e` untouched rather than `e` rounded off a float, so the
 * unscaled path is the same integers it was before a scale existed. The clamp is the one
 * `halfOf` carries for the same reason: a window one pixel wide is legal mid-resize and
 * `vkCreateImage` rejects a zero extent.
 */
VkExtent2D scaledBy(VkExtent2D e, float scale) {
    if (scale >= 1.0f) return e;
    const auto axis = [scale](uint32_t v) {
        return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(v) * scale)));
    };
    return VkExtent2D{axis(e.width), axis(e.height)};
}

// `appendText` used to live here. S6 moved it to `ui/Ui.cpp`, where the UI's draw list
// is the second caller: written twice, the pen convention -- baseline origin, y down --
// would have had two places to drift from overlay.vert. It is a free function over a
// plain vector rather than a DrawList method for exactly that reason, since the HUD does
// no clipping and wants none of the machinery around it.

} // namespace

void Renderer::init(VulkanContext& context, Uploader& up, GLFWwindow* win, bool vsync, uint32_t msaaRequest,
                    const std::string& debugFontPath, float debugFontHeight) {
    ctx = &context;
    window = win;
    uploader = &up;
    vsyncEnabled = vsync;

    // Clamp up front so the G-buffer is built once at the right sample count rather
    // than at a default and immediately rebuilt.
    msaaSamples = ctx->clampSampleCount(msaaRequest);

    {
        auto z = core::Profiler::scope("Swapchain::create");
        swap.create(*ctx, window, vsyncEnabled);
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(ctx->device, &samplerInfo, nullptr, &pointSampler), "vkCreateSampler(fullscreen)");

    // The Hi-Z sampler (C11): nearest everywhere and every level reachable. Filtering a
    // depth pyramid averages a near surface with a far one and yields a plane that is
    // neither, and an unset maxLod clamps `textureLod` to mip 0 -- which is a test against
    // one texel of full-resolution depth, and over-culls badly enough to be visible.
    VkSamplerCreateInfo hizInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    hizInfo.magFilter = VK_FILTER_NEAREST;
    hizInfo.minFilter = VK_FILTER_NEAREST;
    hizInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    hizInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hizInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hizInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hizInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCheck(vkCreateSampler(ctx->device, &hizInfo, nullptr, &hizSampler), "vkCreateSampler(hiz)");

    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    vkCheck(vkCreateSampler(ctx->device, &samplerInfo, nullptr, &fontSampler), "vkCreateSampler(font)");

    // One zone each, and they are named for their functions rather than pooled under a
    // "pipelines" heading: they are individually the largest things `Renderer::init`
    // does, they differ by an order of magnitude between debug and release, and pooling
    // them would leave the reader exactly where the whole of `initRenderer` was before
    // this -- one big number.
    { auto z = core::Profiler::scope("createDescriptorLayouts"); createDescriptorLayouts(); }
    // Before createFrameResources: the per-frame cull sets are allocated from
    // cullSetLayout the first time ensureInstanceCapacity() runs, which that calls.
    { auto z = core::Profiler::scope("createCullPipeline"); createCullPipeline(); }
    { auto z = core::Profiler::scope("createDepthPyramidPipeline"); createDepthPyramidPipeline(); }
    { auto z = core::Profiler::scope("createSkinPipeline"); createSkinPipeline(); }
    { auto z = core::Profiler::scope("createFrameResources"); createFrameResources(); }
    // Before createRenderTargets: it allocates the bloom descriptor sets and writes
    // bloomSampler into them, so the sampler has to exist first.
    { auto z = core::Profiler::scope("createBloomPipelines"); createBloomPipelines(); }
    { auto z = core::Profiler::scope("createSsaoPipelines"); createSsaoPipelines(); }
    { auto z = core::Profiler::scope("createTaaPipeline"); createTaaPipeline(); }
    { auto z = core::Profiler::scope("createRenderTargets"); createViewTargets(); }
    { auto z = core::Profiler::scope("createIblResources"); createIblResources(); }
    { auto z = core::Profiler::scope("createShadowResources"); createShadowResources(); }
    // Consumed, not discarded: without timestamps every zone reports 0.000 ms, and
    // logGpuTimings has to say that rather than print a table of zeros someone could
    // read as "these passes are free".
    //
    // **The left arm is what makes `--no-profiler` mean what it says.** Short-circuiting
    // it leaves `queryPool` null, and every `GpuProfiler` entry point early-outs on that
    // handle -- so no timestamp is written, none is read back, and the flag the profiler's
    // own overhead figure is differenced against costs the GPU query path nothing. What
    // survives is `GpuScope`'s debug-utils label, which is deliberately not tied to the
    // timestamp succeeding; a run that asked for no profiler is still a run whose capture
    // has to be readable.
    gpuTimingAvailable =
        core::Profiler::enabled() && gpuProfiler.init(*ctx, kFramesInFlight) == ProfilerStatus::Enabled;

    // The atlas is built whether or not the overlay starts enabled: F6 can turn it on
    // at any point, and a 12 KB R8 image is not worth making conditional.
    debugFont.init(*ctx, up, debugFontPath, debugFontHeight);
    overlayScratch.reserve(static_cast<size_t>(kMaxOverlayQuads) * 6);

    // Nothing is loaded yet, so this allocates the smallest array there is and points
    // every descriptor at the atlas. `syncImages` grows it from the table.
    ensureImageCapacity(1);
    writeImageDescriptors();

    core::Logger::status(core::LogCategory::Render, "Renderer ready (%ux MSAA)", static_cast<uint32_t>(msaaSamples));
}

void Renderer::setImages(const ImageTable* table) {
    images = table;
    // Zero rather than the table's current value: a different table is a different slot
    // space, and whatever this renderer has resident belongs to the old one.
    imageRevision = 0;
}

void Renderer::createOverlaySetLayout(uint32_t slots) {
    const VkDescriptorSetLayout previous = overlaySetLayout;

    // PARTIALLY_BOUND is deliberately absent -- the same argument `GltfScene` makes about
    // its own array. A partially-bound array lets a shader read an unwritten slot, and
    // "undefined data" is exactly the state a vertex holding a stale index would land in.
    // Every allocated slot is written, so a wrong index draws the font atlas, which is
    // visible and harmless.
    //
    // VARIABLE_DESCRIPTOR_COUNT is absent too, and that is the repair rather than an
    // oversight. It let this layout be created once at the device's ceiling with each
    // allocation taking as much of it as it needed, so growth touched no pipeline -- and
    // the ceiling on this machine is 1,044,480. **The validation layer charges the
    // *declared* count once per draw that samples the array**, at about 8 ns a descriptor,
    // and the overlay's single text draw is such a draw: 8.5 ms of CPU in every debug
    // frame, which is 94% of `Renderer::record` and half the frame rate, for descriptors
    // no image will ever occupy. Measured at three declared counts and linear in all
    // three; see
    // docs/kanban/done/bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md.
    //
    // Declaring the capacity instead makes the charge proportional to what is resident,
    // which is the honest number and is one or two in every scene in the tree. It costs a
    // rebuild of the five pipeline layouts this set appears in each time the array
    // doubles -- log2(N) times over a run, on the same event that already waits for the
    // device -- which is a far better trade than a per-frame tax that scales with a limit
    // nobody can reach.
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slots,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &info, nullptr, &overlaySetLayout),
            "vkCreateDescriptorSetLayout(overlay)");
    // The same registry `createDescriptorLayouts` fills, and for the same reason: Vulkan
    // offers no way to read a layout back, and `verifyShaderBindings` compares against it.
    layoutBindings[overlaySetLayout] = {binding};

    if (previous == VK_NULL_HANDLE) return;

    layoutBindings.erase(previous);
    vkDestroyDescriptorSetLayout(ctx->device, previous, nullptr);
    // Two set layouts are compatible only if they are identically defined, so a layout of
    // a different width is a different layout to everything built against it. The dirty
    // flag `drawFrame` already polls rebuilds the pipeline layouts and the pipelines
    // behind them, after the `vkDeviceWaitIdle` it already takes -- and at init there are
    // none to rebuild, which the early return above is what makes true.
    pipelinesDirty = true;
}

void Renderer::ensureImageCapacity(uint32_t slots) {
    const uint32_t need = std::max(slots, 1u);
    if (need <= imageCapacity) return;

    // Double, so a game loading images one at a time does not reallocate on every load.
    // The first allocation is one slot, because at init the only image is the atlas.
    uint32_t grown = imageCapacity == 0 ? need : imageCapacity;
    while (grown < need) grown *= 2;
    grown = std::min(grown, imageSlotCeiling);

    // A pool's sizes are fixed at creation, so a wider set cannot come out of the old
    // one. Destroying the pool frees the set with it, which is why there is no
    // vkFreeDescriptorSets here.
    if (overlayImagePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx->device, overlayImagePool, nullptr);
        overlaySet = VK_NULL_HANDLE;
        core::Logger::status(core::LogCategory::Render, "Overlay image array grown: %u -> %u slots", imageCapacity,
                             grown);
    }

    // After the pool, so the set allocated from the old layout is already freed, and
    // before the allocation below, which needs the new one. The width is stated once, here.
    createOverlaySetLayout(grown);

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, grown};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    vkCheck(vkCreateDescriptorPool(ctx->device, &poolInfo, nullptr, &overlayImagePool),
            "vkCreateDescriptorPool(overlay images)");

    VkDescriptorSetAllocateInfo overlayAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    overlayAlloc.descriptorPool = overlayImagePool;
    overlayAlloc.descriptorSetCount = 1;
    overlayAlloc.pSetLayouts = &overlaySetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &overlayAlloc, &overlaySet), "vkAllocateDescriptorSets(overlay)");

    imageCapacity = grown;
}

void Renderer::writeImageDescriptors() {
    // Every allocated slot, not just the ones with an image. An index nothing has loaded
    // into then draws the atlas rather than reading a descriptor that was never written
    // -- see the note on the layout's creation for why that is the property worth having.
    std::vector<VkDescriptorImageInfo> infos(imageCapacity,
                                             {fontSampler, debugFont.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    for (uint32_t s = 1; s < imageCapacity && s < overlayImages.size(); ++s) {
        if (overlayImages[s].view == VK_NULL_HANDLE) continue;
        // Linear, not `fontSampler`'s nearest: an image is drawn at whatever size the
        // layout gives it, which is almost never 1:1 with its texels.
        //
        // Unless the game asked for pixel exactness (P2), where the opposite is true and
        // the linear tap is the defect. At 1:1 a linear sampler is *nearly* exact -- the
        // sample lands on a texel centre and the neighbour's weight is zero -- but "nearly"
        // is decided by how many sub-texel bits the hardware keeps, and a coordinate that
        // rounds the wrong side of a centre blends 1/256 of the wrong texel into an image
        // that was supposed to arrive unchanged. Nearest has no such margin.
        infos[s] = {pixelExact ? fontSampler : pointSampler, overlayImages[s].view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = overlaySet;
    write.dstBinding = 0;
    write.descriptorCount = imageCapacity;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = infos.data();
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);
}

void Renderer::syncImages() {
    // Above the revision test, which is the *common* case: this costs nothing on almost
    // every frame and milliseconds on one, so the number worth reading is the max and a
    // zone that vanished on the cheap path would have no max to read.
    // `zone`, not the `s` used elsewhere: the reconcile loop below already indexes with one.
    auto zone = core::Profiler::scope("syncImages");
    if (images == nullptr || images->revision() == imageRevision) return;

    // One wait for the whole reconcile, and the argument for it rather than a retirement
    // list is on the declaration. It covers both hazards at once: an image about to be
    // destroyed that an in-flight frame samples, and a descriptor about to be rewritten
    // in a set an in-flight command buffer bound.
    vkDeviceWaitIdle(ctx->device);

    const uint32_t slots = std::min(images->slotCount(), imageSlotCeiling);
    ensureImageCapacity(slots);
    overlayImages.resize(slots);
    overlayResident.resize(slots, 0);
    overlayBorrowed.resize(slots, 0);

    for (uint32_t s = 1; s < slots; ++s) {
        const ImageTable::Entry& e = images->at(s);
        // Zero for a slot the table has given up. A slot destroyed and reacquired between
        // two syncs differs here too, because `destroy` moved the generation -- which is
        // what stops the old image being left in place under a new name.
        const uint32_t want = e.live ? e.generation : 0;
        if (overlayResident[s] == want) continue;

        // **A borrowed slot is not this function's to load or to free.** A render view's
        // destination is drawn rather than decoded, and `syncViews` owns the image; all
        // that is shared is the descriptor array it lands in. Dropping the handle without
        // destroying it is the whole difference, and getting it wrong is a double free --
        // which is exactly what the validation layer reported the first time this was
        // written with the borrow tracked on the *table's* entry instead of here. The flag
        // has to live beside the handle, because teardown runs when the table is gone.
        if (overlayImages[s].image != VK_NULL_HANDLE && overlayBorrowed[s] == 0) {
            destroyImage(*ctx, overlayImages[s]);
        }
        overlayImages[s] = {};
        overlayBorrowed[s] = 0;
        overlayResident[s] = want;
        if (!e.live || e.external) continue;

        int width = 0;
        int height = 0;
        int channels = 0;
        // STBI_rgb_alpha rather than the file's own channel count: the descriptor array
        // is one format, and a UI that mixed RGB and RGBA images would need two.
        stbi_uc* pixels = stbi_load(e.path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            if (pixels != nullptr) stbi_image_free(pixels);
            // The slot stays empty and its descriptor keeps the atlas, which is what the
            // table promised a caller whose file resolved but was not an image.
            core::Logger::warn(core::LogCategory::Render, "image %u (%s): could not be decoded (%s)", s, e.name.c_str(),
                               stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown");
            continue;
        }

        const VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        // _SRGB, so the hardware decodes to linear on read and `overlay.frag` multiplies
        // two linear values. A UNORM image here would be the exact bug the note in that
        // shader describes for vertex colours, one stage later.
        overlayImages[s] = createImage(*ctx, extent, VK_FORMAT_R8G8B8A8_SRGB,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       mipLevelsFor(extent), VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                       e.name.c_str());
        uploader->uploadImageWithMips(*ctx, overlayImages[s], pixels,
                                      static_cast<VkDeviceSize>(width) * height * 4);
        stbi_image_free(pixels);

        core::Logger::status(core::LogCategory::Render, "image %u: %s (%dx%d)", s, e.name.c_str(), width, height);
    }

    writeImageDescriptors();
    imageRevision = images->revision();
}

void Renderer::setSprites(const scene::SpriteTable* table) {
    sprites = table;
    // Whatever the buffers hold belongs to the old table's ordering. Nothing is freed:
    // the next `ensureSpriteCapacity` either finds them large enough or grows them, and
    // the copy that follows overwrites every byte a draw will read.
    stats.sprites = 0;
    // A new table is a new revision space and the numbers from the old one mean nothing
    // here -- the same reset `setInstances` makes, and for the same reason: a second table
    // whose counter happened to agree would have every slot skip its first copy.
    for (auto& f : frames) f.spriteRevision = 0;
}

void Renderer::ensureSpriteCapacity(uint32_t count) {
    auto s = core::Profiler::scope("ensureSpriteCapacity");
    if (count == 0 || count <= spriteCapacity) return;

    // Double, so a game creating sprites one at a time does not reallocate on every
    // create. The first call sizes exactly, because that one is the level load and there
    // is nothing to guess.
    const uint32_t grown = spriteCapacity == 0 ? count : std::max(count, spriteCapacity * 2);

    // The buffers being replaced may be read by a frame still in flight, and the answer
    // is the one `ensureInstanceCapacity` and `syncImages` already gave: wait. A
    // retirement list is machinery for an event that happens when a level loads, and the
    // trigger for writing one is unchanged -- a caller that reallocates per frame during
    // play, which is streaming.
    if (spriteCapacity != 0) {
        vkDeviceWaitIdle(ctx->device);
        core::Logger::status(core::LogCategory::Render, "Sprite buffers grown: %u -> %u sprites", spriteCapacity, grown);
    }

    for (auto& f : frames) {
        destroyBuffer(*ctx, f.spriteBuffer);
        f.spriteBuffer = createBuffer(*ctx, sizeof(scene::GpuSprite) * grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VMA_MEMORY_USAGE_AUTO,
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                          VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      "spriteBuffer");

        if (f.spriteSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = descriptorPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &spriteSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &f.spriteSet), "vkAllocateDescriptorSets(sprites)");
        }

        const VkDescriptorBufferInfo info{f.spriteBuffer.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = f.spriteSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

        // The buffer this slot's revision described has just been destroyed, so the
        // revision describes nothing. Without this the upload gate would look at a fresh
        // allocation full of whatever the driver handed back, decide it already held the
        // current revision, and draw it.
        f.spriteRevision = 0;
    }

    spriteCapacity = grown;
}

void Renderer::recordSprites(VkCommandBuffer cmd, uint32_t slot, VkImageView target, const scene::Camera& camera) {
    auto cpuZone = core::Profiler::scope("Sprites");
    if (sprites == nullptr || spritePipeline == VK_NULL_HANDLE) return;

    const std::vector<scene::GpuSprite>& draws = sprites->draws();
    // The capacity is grown before recording begins, so a table that outran it between
    // `ensureSpriteCapacity` and here is impossible -- but the clamp costs one comparison
    // and the alternative is a buffer overrun, so it is written down rather than reasoned
    // about.
    const auto count = static_cast<uint32_t>(std::min<size_t>(draws.size(), spriteCapacity));
    // Assigned before the early return, not after the draw: a level that destroyed its
    // last sprite would otherwise leave the HUD reporting the count from the frame before.
    stats.sprites = count;
    if (count == 0 || frames[slot].spriteBuffer.mapped == nullptr) return;

    GpuScope zone(gpuProfiler, cmd, slot, "Sprites");

    // Straight into mapped memory during recording, exactly as the overlay's vertices and
    // the particle emitters are: this slot's fence was waited on at the top of drawFrame,
    // so nothing in flight is reading it. One memcpy of an array that is already in draw
    // order -- there is no gather step, which is the whole reason `SpriteTable` keeps its
    // dense array sorted rather than sorting an index list per frame.
    //
    // **Once per revision per frame slot, not once per frame**, which is the gate
    // `updateInstances` has had since the instance table existed. A screen of sprites that
    // did not change costs nothing: at ten thousand the copy is 640 KB and 0.036 ms, which
    // is a third of everything `Renderer::record` does in this scene, and at fifty
    // thousand it is 0.171 ms and three quarters of it. A moving screen pays exactly what
    // it paid before, because the whole array is still one linear write.
    //
    // Every path that can invalidate what the buffer holds without the table's counter
    // moving zeroes this slot's copy instead: growth destroys the allocation, `setSprites`
    // replaces the revision space, and `destroyFrameResources` destroys both.
    if (frames[slot].spriteRevision != sprites->revision()) {
        frames[slot].spriteRevision = sprites->revision();
        std::memcpy(frames[slot].spriteBuffer.mapped, draws.data(),
                    static_cast<size_t>(count) * sizeof(scene::GpuSprite));
    }

    // LOAD, like the debug lines and the overlay: this composites onto the tonemapped
    // image that is already there, and into the *virtual* target, because a sprite is
    // world-space content.
    VkRenderingAttachmentInfo color = colorAttachment(target);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, spritePipeline);
    const VkDescriptorSet sets[] = {overlaySet, frames[slot].spriteSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, spriteLayout, 0, 2, sets, 0, nullptr);

    // The *unjittered* view-projection, for the reason `recordDebugLines` gives: a jitter
    // applied here would move every sprite by a sub-pixel every frame against a frame that
    // has already been resolved. `pixelExact` turns the jitter off anyway; this pass does
    // not depend on a game having asked for it.
    const float aspect = static_cast<float>(view.renderExtent.width) / static_cast<float>(view.renderExtent.height);
    const glm::mat4 viewProj = camera.viewProjection(aspect);
    vkCmdPushConstants(cmd, spriteLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewProj), &viewProj);

    // Six vertices, `count` instances, one draw for every layer at once. The card that
    // opened this row said one draw per layer; one blend state makes one global sort
    // possible and one global sort makes one draw possible, which is the argument
    // `particle.frag` already carries -- and a layer is then purely a sort key, which is
    // all it was ever asked to be.
    vkCmdDraw(cmd, 6, count, 0, 0);

    vkCmdEndRendering(cmd);
}

void Renderer::setScene(const scene::GltfScene* s) {
    // The outgoing scene's layout is destroyed by `GltfScene::destroy()` immediately after
    // this returns, so its entry goes now: leaving it in the map would leave a key pointing
    // at a freed handle -- and handles get reused, so the next layout to land on that value
    // would be checked against the wrong bindings. Same argument as the IBL bake's erase.
    if (scene != nullptr) layoutBindings.erase(scene->descriptorSetLayout());

    scene = s;

    // The variant cache is keyed by material index into the scene that has just gone, so
    // it says nothing about this one. Zero is a revision no live table reports, which
    // forces the next updateInstances() to rebuild it -- a second scene whose revision
    // happened to match the first's would otherwise be grouped by the first's answers.
    seenMaterialRevision = 0;
    materialVariant.clear();
    reportedVariantOverflow = false;

    // Null is how a caller drops the scene ahead of destroying it -- `Engine::applyPendingScene`
    // does exactly that between the device wait and the upload of the replacement. Nothing
    // below has a scene to read, and the pipelines built against the old layout stay put
    // until the next `setScene` with a real scene calls `createPipelines()`, which begins by
    // destroying them.
    if (s == nullptr) return;

    const float diagonal = glm::length(s->boundsMax - s->boundsMin);
    // Both particle lengths are scene-relative (S3.3, S3.4). The
    // sort key quantises distance over a fixed range, and a range that resolves Sponza
    // to a centimetre would put a 2 m test scene in one bucket. The collision thickness
    // has to exceed what a particle travels in one step and stay well under the depth
    // of a room, or a particle in the next room counts as inside the wall.
    particleSortRange = std::max(diagonal * 2.0f, 1.0f);
    particleCollisionThickness = std::max(diagonal * 0.02f, 0.05f);

    // The scene owns one of the layouts the G-buffer and forward pipelines bind, so it
    // registers here rather than in createDescriptorLayouts(). It is also the only set
    // with a variable-count descriptor array, which makes it the one most worth
    // checking a shader against.
    layoutBindings[s->descriptorSetLayout()] = s->descriptorBindings();

    // Two of the skinning set's six bindings are the scene's vertex buffer and the skin
    // influences, and G4 made the first of those something that moves: growing the shared
    // geometry buffer destroys the old one, and a descriptor still naming it is a dispatch
    // reading freed memory. Found exactly that way -- by growth, under the validation
    // layers, which reported it as binding #0 using a destroyed buffer.
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) writeSkinSet(slot, false);

    // The G-buffer pipeline layout references the scene's descriptor set layout, so
    // pipelines cannot exist until a scene does -- but only the *first* scene has to build
    // them here. `Engine::createMesh` calls this per mesh, and rebuilding every graphics
    // and compute pipeline per prop is 22 ms a call in debug for an answer no frame has
    // asked for yet. `drawFrame`'s `pipelineRebuild` runs before anything records, so what
    // a frame draws with is always current.
    if (pipelinesBuilt) {
        pipelinesDirty = true;
    } else {
        pipelinesBuilt = true;
        createPipelines();
    }
}

void Renderer::setInstances(const scene::InstanceTable* table) {
    instances = table;
    if (instances == nullptr) return;

    if (scene == nullptr) {
        core::Logger::critical(core::LogCategory::Render, "setInstances() before setScene(): the geometry the instances index "
                                              "into does not exist yet");
    }

    ensureInstanceCapacity(instances->slotCount());

    // Force every frame slot to re-upload: a new table is a new revision space, and
    // `instanceRevision` from the previous one means nothing here.
    for (auto& f : frames) f.instanceRevision = 0;

    // ------------------------------------------------------------ ray query (3.9)
    // Built from the instance table rather than from the scene, because a BLAS
    // geometry is one *instance* of a primitive at one transform -- the same thing a
    // draw command is.
    //
    // **Marked rather than built.** This used to build on the spot, and it is called once
    // per `createMesh`, `addModel` and `removeModel` -- so a game making a dozen props at
    // load paid a dozen full BLAS-and-TLAS builds for a structure nothing had traced yet.
    // `rebuildAccelIfStale` consumes the flag once per frame, in `endFrame`, which runs
    // before that frame's `drawFrame`: the structure a frame traces is always the one its
    // instances describe, and a batch of any size costs one build.
    if (ctx->rayQuerySupported) {
        accelDirty = true;

        // The ray-traced variants of the lighting and reflection shaders are selected
        // by whether a TLAS exists, so the pipelines built before the structure did
        // are the wrong ones. Also deferred -- and it has to be sequenced *after* the
        // build, which it is: `pipelineRebuild` is inside `drawFrame`.
        pipelinesDirty = true;
    }
}

void Renderer::instancesGrew() {
    if (instances == nullptr) return;
    ensureInstanceCapacity(instances->slotCount());
    // The slot exists but no frame has uploaded it, and `instanceRevision` is per slot:
    // forcing it re-reads the table on the next `updateInstances`.
    for (auto& f : frames) f.instanceRevision = 0;
    accelDirty = true;
}

void Renderer::rebuildAccelIfStale() {
    if (!ctx->rayQuerySupported || instances == nullptr) return;

    // The dirty flag first, and **before the `accel.valid()` test**, because this is now
    // the path that builds the structure the first time: nothing else does. It is also
    // not a "stale" case -- nothing moved, the set of instances changed -- so it carries
    // no warning. Adding geometry is a thing a game does on purpose.
    if (accelDirty) {
        accelDirty = false;
        vkDeviceWaitIdle(ctx->device);
        buildAccelerationStructures();
        return;
    }

    if (!accel.valid()) return;
    if (!staticTierStale(accel, *instances)) return;

    // Once, not once per offender, and warned rather than silent: a game that moves a
    // static instance every frame would otherwise pay a full structure rebuild every frame
    // with nothing in the log to say why. The flag it is missing is `InstanceDesc::dynamic`.
    if (!staleAccelReported) {
        staleAccelReported = true;
        core::Logger::warn(core::LogCategory::Render,
                     "Acceleration structure rebuilt: a static instance moved after it was built. Once at load is "
                     "a scene node writing a transform the instance was created without; every frame is an "
                     "instance that should have been created dynamic.");
    }

    // The two other callers run before a frame has been submitted; this one runs between
    // them, with `kFramesInFlight` command buffers still executing and still holding the
    // TLAS, the BLASes and the hit-record buffer `buildAccelerationStructures` is about to
    // free. Without the stall this is a use-after-free of GPU objects, which does not look
    // like a rendering bug -- it looks like flaky hardware, on a different case each run.
    vkDeviceWaitIdle(ctx->device);
    buildAccelerationStructures();
}

void Renderer::buildAccelerationStructures() {
    if (!ctx->rayQuerySupported || scene == nullptr || instances == nullptr) return;

    destroySceneAccelStruct(*ctx, accel);
    buildSceneAccelStruct(*ctx, *uploader, scene->vertexBuffer(), sizeof(scene::Vertex), scene->vertexCount(),
                          scene->indexBuffer(), scene->indexData(), skinnedVertices.buffer, skinnedVertexCount,
                          skinDestBase, *instances, scene->emissiveMaterials(), accel);

    // Binding 2 of the TLAS set (the binding kept its number when the shadow maps
    // that shared the set were ripped out). The handle it points at is stable for the
    // life of the structure -- a refit and a TLAS *rebuild into the same handle* both
    // leave it valid, which is what lets the descriptor be written once here rather
    // than per frame.
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &accel.tlas.handle;

    VkWriteDescriptorSet asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    asWrite.pNext = &asInfo;
    asWrite.dstSet = tlasSet;
    asWrite.dstBinding = 2;
    asWrite.descriptorCount = 1;
    asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(ctx->device, 1, &asWrite, 0, nullptr);
}

void Renderer::createDescriptorLayouts() {
    // Every layout is recorded as it is created. Vulkan offers no way to read a
    // VkDescriptorSetLayout back, and verifyShaderBindings() needs something to
    // compare a reflected binding against; the alternative was threading the binding
    // arrays through to every pipeline call site.
    auto record = [&](VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* b, uint32_t count) {
        layoutBindings[layout] = {b, b + count};
    };

    VkDescriptorSetLayoutBinding frameBinding{};
    frameBinding.binding = 0;
    frameBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frameBinding.descriptorCount = 1;
    // Compute too, since ssao.comp reconstructs world position from depth and needs
    // invViewProj and viewProj to do it.
    frameBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1 the lights, binding 3 the instance table (4.1), binding 4 last
    // frame's transforms (3.4); binding 2 is vacant (it held the shadow matrices
    // before the shadow system was ripped out, and the numbering stayed so nothing
    // renumbered). Storage buffers rather than
    // more UBO array slots, so every count is a number the shader reads instead of a
    // constant the descriptor layout was built around. The instance *bounds* are not
    // here: what a cull tests is the union over one draw command, and uploading a value
    // alongside its own reduction would be paying twice to say one thing. They live in
    // the cull set.
    constexpr uint32_t kFrameBindingCount = 5;
    std::array<VkDescriptorSetLayoutBinding, kFrameBindingCount> frameBindings{};
    frameBindings[0] = frameBinding;
    for (uint32_t i = 1; i < kFrameBindingCount; ++i) {
        frameBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                            nullptr};
    }

    VkDescriptorSetLayoutCreateInfo frameInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    frameInfo.bindingCount = kFrameBindingCount;
    frameInfo.pBindings = frameBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &frameInfo, nullptr, &frameSetLayout),
            "vkCreateDescriptorSetLayout(frame)");
    record(frameSetLayout, frameBindings.data(), kFrameBindingCount);

    // Four multisampled G-buffer attachments plus depth, read with texelFetch per
    // sample, then the single-sampled AO buffer at binding 5 -- single-sampled because
    // it is one value per pixel regardless of how many samples the G-buffer holds.
    std::array<VkDescriptorSetLayoutBinding, 6> gbufferBindings{};
    for (uint32_t i = 0; i < gbufferBindings.size(); ++i) {
        gbufferBindings[i].binding = i;
        gbufferBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gbufferBindings[i].descriptorCount = 1;
        // Compute too: ssr.comp (3.1) marches the depth buffer and reads the same
        // normals and roughness the lighting pass does, through this very set.
        gbufferBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo gbufferInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    gbufferInfo.bindingCount = static_cast<uint32_t>(gbufferBindings.size());
    gbufferInfo.pBindings = gbufferBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &gbufferInfo, nullptr, &gbufferSetLayout),
            "vkCreateDescriptorSetLayout(gbuffer)");
    record(gbufferSetLayout, gbufferBindings.data(), static_cast<uint32_t>(gbufferBindings.size()));

    // One sampled image in the fragment stage. The tonemap set takes two of these --
    // the HDR target and the bloom chain -- and the font takes one.
    std::array<VkDescriptorSetLayoutBinding, 2> sampledBindings{};
    for (uint32_t i = 0; i < sampledBindings.size(); ++i) {
        sampledBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }

    VkDescriptorSetLayoutCreateInfo hdrInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    hdrInfo.bindingCount = 2;
    hdrInfo.pBindings = sampledBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &hdrInfo, nullptr, &hdrSetLayout),
            "vkCreateDescriptorSetLayout(hdr)");
    record(hdrSetLayout, sampledBindings.data(), 2);

    // One sampled image, and now shared rather than duplicated.
    //
    // This was two separate layouts of identical shape -- one for the font atlas, one
    // for the SSR composite -- on the argument that binding a reflection buffer through
    // something called `singleImageSetLayout` costs an hour to read. That argument was right
    // and the answer was wrong: the third user (the decal pass, 3.3) makes this a
    // pattern rather than a coincidence, so it gets a name that describes the *shape*
    // instead of one of its users. The Rule of Threes, applied literally.
    // Compute as well as fragment since S3.4: `particle_simulate.comp` collides against
    // the same resolved depth the decal pass projects onto, through this very layout. A
    // stage flag the other four users do not need costs them nothing.
    std::array<VkDescriptorSetLayoutBinding, 1> singleBindings{sampledBindings[0]};
    singleBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo singleInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    singleInfo.bindingCount = 1;
    singleInfo.pBindings = singleBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &singleInfo, nullptr, &singleImageSetLayout),
            "vkCreateDescriptorSetLayout(single image)");
    record(singleImageSetLayout, singleBindings.data(), 1);

    // One storage image, fragment stage: the shadow mask, written by shadowmask.frag and
    // read by the lighting pass through the same descriptor. Not `computeImageSetLayout`,
    // whose one storage binding is compute-only -- widening that would widen it for
    // bloom, SSAO and the depth pyramid to buy one user a stage it does not share.
    const std::array<VkDescriptorSetLayoutBinding, 1> storageBindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};

    VkDescriptorSetLayoutCreateInfo storageInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    storageInfo.bindingCount = 1;
    storageInfo.pBindings = storageBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &storageInfo, nullptr, &storageImageSetLayout),
            "vkCreateDescriptorSetLayout(storage image)");
    record(storageImageSetLayout, storageBindings.data(), 1);

    // The overlay's image array (C5, grown by P1). A layout of its own rather than a
    // second binding on the one above: four other passes bind exactly one image through
    // that, and widening it for the overlay would widen it for all of them.
    //
    // **The layout itself is not built here.** Its one binding's count is the number of
    // slots the array currently holds, so `createOverlaySetLayout` builds it and
    // `ensureImageCapacity` rebuilds it whenever the array grows. The argument for
    // declaring the capacity rather than the ceiling is on that function, and it is a
    // measured one.
    //
    // What is computed here is the *ceiling* -- how far the array is allowed to grow --
    // because that is a property of the device rather than of any allocation, and
    // `maxImageSlots()` reports it to `ImageTable::init` before a single image is loaded.
    // It is the smaller of the two limits that bind: the per-stage one, and the per-set
    // one, because they are all in one set. Nothing here is an engine constant a game
    // could grow into and be refused by.
    //
    // **Less a reserve, and P6 is why -- the validation layer found it on the first run.**
    // Those two limits are sums over *every set in a pipeline layout*, not over one set. So
    // long as this array was bound alone, declaring the whole device limit was free; the
    // moment it joined the G-buffer and shadow layouts beside `GltfScene`'s own texture
    // array, the sum came to the limit plus that scene's 134 textures and
    // `vkCreatePipelineLayout` refused all four of the layouts it appears in. A set that
    // claims the entire budget cannot be bound next to anything, which makes the budget
    // useless rather than generous.
    //
    // The reserve is what the sets bound beside it may declare: `GltfScene`'s array is
    // `textures + 1 + kTextureSlotHeadroom` and is not known until a scene loads, so this is
    // stated ahead rather than derived. Four thousand textures in one scene is the trigger
    // to make it a real calculation; nothing in the tree is within two orders of it.
    constexpr uint32_t kSharedDescriptorReserve = 4096;
    const uint32_t deviceCeiling = std::min(ctx->properties.limits.maxPerStageDescriptorSampledImages,
                                            ctx->properties.limits.maxDescriptorSetSampledImages);
    imageSlotCeiling = std::max(1u, deviceCeiling > kSharedDescriptorReserve
                                        ? deviceCeiling - kSharedDescriptorReserve
                                        : deviceCeiling / 2);

    // One binding: the scene TLAS (3.9) at binding 2 -- it kept its number when the
    // shadow maps that shared this set were ripped out, so no shader renumbered.
    // Present only where the device can trace; a descriptor layout cannot be
    // specialised away, so an empty layout stands in on devices that cannot.
    std::array<VkDescriptorSetLayoutBinding, 3> tlasBindings{};
    // Bindings 0 and 1 are the sun's map and the punctual atlas, always present: they are
    // what a device without ray query shadows with, and the traced path simply never
    // samples them. Binding 2 is the TLAS, only where the device can trace -- so the
    // count is 2 or 3 and the *order* here has to put the optional one last.
    tlasBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    tlasBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    tlasBindings[2] = {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    const uint32_t tlasBindingCount = ctx->rayQuerySupported ? 3u : 2u;
    VkDescriptorSetLayoutCreateInfo tlasInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tlasInfo.bindingCount = tlasBindingCount;
    tlasInfo.pBindings = tlasBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &tlasInfo, nullptr, &tlasSetLayout),
            "vkCreateDescriptorSetLayout(tlas)");
    record(tlasSetLayout, tlasBindings.data(), tlasBindingCount);

    // Compute image processing: binding 0 the source to sample, binding 1 the
    // destination to store into. Bloom and SSAO both use it.
    std::array<VkDescriptorSetLayoutBinding, 2> bloomBindings{};
    bloomBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bloomBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo bloomInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bloomInfo.bindingCount = 2;
    bloomInfo.pBindings = bloomBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &bloomInfo, nullptr, &computeImageSetLayout),
            "vkCreateDescriptorSetLayout(compute image)");
    record(computeImageSetLayout, bloomBindings.data(), 2);

    // IBL: irradiance cube, prefiltered cube, BRDF LUT, and the raw environment for
    // the skybox.
    std::array<VkDescriptorSetLayoutBinding, 4> iblBindings{};
    for (uint32_t i = 0; i < iblBindings.size(); ++i) {
        // Compute too: the ray-traced reflection pass (3.11) samples the environment
        // cube where a ray misses everything.
        iblBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                          VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo iblInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    iblInfo.bindingCount = static_cast<uint32_t>(iblBindings.size());
    iblInfo.pBindings = iblBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &iblInfo, nullptr, &iblSetLayout),
            "vkCreateDescriptorSetLayout(ibl)");
    record(iblSetLayout, iblBindings.data(), static_cast<uint32_t>(iblBindings.size()));

    // Particles (S3): the pool, its sort keys, the emitters and this frame's births.
    // Vertex as well as compute -- particle.vert reads the pool and the keys, which is
    // what makes the draw need no vertex buffer at all.
    std::array<VkDescriptorSetLayoutBinding, 4> particleBindings{};
    for (uint32_t i = 0; i < particleBindings.size(); ++i) {
        particleBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                               VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo particleInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    particleInfo.bindingCount = static_cast<uint32_t>(particleBindings.size());
    particleInfo.pBindings = particleBindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &particleInfo, nullptr, &particleSetLayout),
            "vkCreateDescriptorSetLayout(particles)");
    record(particleSetLayout, particleBindings.data(), static_cast<uint32_t>(particleBindings.size()));

    // The sprite array (P4). One binding, vertex stage only: the fragment half reads the
    // *image* array, which is `overlaySetLayout` and set 0 of the same pipeline.
    const VkDescriptorSetLayoutBinding spriteBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                     VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo spriteInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    spriteInfo.bindingCount = 1;
    spriteInfo.pBindings = &spriteBinding;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &spriteInfo, nullptr, &spriteSetLayout),
            "vkCreateDescriptorSetLayout(sprites)");
    record(spriteSetLayout, &spriteBinding, 1);

    // One storage buffer, written by the build dispatch and read by the lighting and
    // shadow-mask draws (C35). Its own layout rather than a binding on the frame set: the
    // buffer is sized from the render extent, so it is rebuilt by every resize, and the
    // frame set is not.
    VkDescriptorSetLayoutBinding tileBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo tileInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tileInfo.bindingCount = 1;
    tileInfo.pBindings = &tileBinding;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &tileInfo, nullptr, &lightTileSetLayout),
            "vkCreateDescriptorSetLayout(light tiles)");
    record(lightTileSetLayout, &tileBinding, 1);

    // Sized with headroom rather than to today's exact allocation (2 frame sets, the
    // G-buffer set, the HDR set, the font set). A pool that fits precisely fails on
    // the very next pass, and the failure is an allocation error a long way from the
    // line that caused it. Descriptor pool memory is negligible; the tight fit was
    // never buying anything.
    //
    // **Every figure below has a per-view term, and it is the one that moves** (C38):
    // `createRenderTargets` allocates about thirty-four sets -- SSAO, the shadow mask, the
    // tiles, the G-buffer, the bloom chain, the depth pyramid, SSR, fog and TAA -- and
    // there is now one of it per live view rather than one shared serially. Raising
    // `kMaxViews` without these terms fails in `vkAllocateDescriptorSets` on the *fourth*
    // view, which is a long way from the constant that caused it.
    std::array<VkDescriptorPoolSize, 5> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 + 4 * kMaxViews};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 + 48 * kMaxViews};
    sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 + 32 * kMaxViews};
    // Three per frame set (lights, spare, instances), four per cull set, six
    // per skin set and four per particle set, twice over for the frames in flight, plus
    // one per view for the light tile set, with room to spare.
    sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 + 8 * kMaxViews};
    sizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4};

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 48 + 40 * kMaxViews;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    vkCheck(vkCreateDescriptorPool(ctx->device, &poolInfo, nullptr, &descriptorPool),
            "vkCreateDescriptorPool(renderer)");

    // The TLAS set is allocated here, immediately behind the pool it comes from, because
    // there is nowhere else it belongs: the descriptor it holds is written once from
    // buildAccelerationStructures() and never again, so this set has no resource to be
    // created alongside. It lived in the shadow resources until S3.9 -- the cascades and
    // the punctual atlas shared the set, so allocating it there was free -- and went
    // missing when they did, leaving a layout, a write and four binds pointing at a
    // handle nothing had ever filled in.
    //
    // Allocated whether or not the device can trace. Without ray query the layout is
    // empty, but an empty set is still a set, and every pipeline that takes this one at
    // index 2 binds it unconditionally.
    VkDescriptorSetAllocateInfo tlasAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    tlasAlloc.descriptorPool = descriptorPool;
    tlasAlloc.descriptorSetCount = 1;
    tlasAlloc.pSetLayouts = &tlasSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &tlasAlloc, &tlasSet), "vkAllocateDescriptorSets(tlas)");
}

void Renderer::createFrameResources() {
    for (auto& f : frames) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx->graphicsFamily;
        vkCheck(vkCreateCommandPool(ctx->device, &poolInfo, nullptr, &f.pool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbInfo.commandPool = f.pool;
        cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbInfo.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(ctx->device, &cbInfo, &f.cmd), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(ctx->device, &semInfo, nullptr, &f.imageAvailable), "vkCreateSemaphore");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(ctx->device, &fenceInfo, nullptr, &f.inFlight), "vkCreateFence");

        // Sized from the budget, not from a constant. That is what makes `lightBudget`
        // the only cap there is: a config raising it grows the buffer to match, and
        // there is no second limit further down to truncate against silently (0.9).
        lightBufferCapacity = std::max(lightBudget, 1u);

        // One bit per light per tile, so the stride follows the same capacity the light
        // buffer does (C35). Zero disables the pass rather than truncating: a game that
        // states a budget past what the build shader's shared mask can hold gets every one
        // of its lights, untiled, and a line saying so -- not a silent cap that would
        // drop lights above the 1024th from the image entirely.
        lightTileWords = (lightBufferCapacity + 31) / 32;
        if (lightTileWords > kLightTileMaxWords) {
            core::Logger::warn(core::LogCategory::Render,
                               "render.lightBudget is %u, past the %u lights tiled assignment can index; "
                               "running the deferred loop over every light in the view instead.",
                               lightBufferCapacity, kLightTileMaxWords * 32);
            lightTileWords = 0;
        }

        // One block per view. Allocated for all `kMaxViews` up front rather than on
        // demand: they total a few hundred kilobytes across every frame slot, and a view
        // created mid-frame that had to allocate its own would need the device idle to do
        // it. Nothing *writes* a block until a view uses it, which is the cost that would
        // have shown up in a frame time.
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            f.uniforms[v] = createBuffer(*ctx, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VMA_MEMORY_USAGE_AUTO,
                                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                         "frameUniforms");
            f.lightBuffer[v] = createBuffer(*ctx, sizeof(GpuLight) * lightBufferCapacity,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            VMA_MEMORY_USAGE_AUTO,
                                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                            "lightBuffer");
        }
        // Written by the CPU and read once by the GPU in the same frame, so it lives in
        // host-visible memory rather than being staged: a staging copy for 123 KB of
        // text would cost more than the draw.
        f.overlayVertices = createBuffer(*ctx, sizeof(OverlayVertex) * kMaxOverlayQuads * 6,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                         "overlayVertices");

        // Same reasoning, one subsystem along (S4.5): a wireframe of the physics world is
        // rebuilt on the CPU every frame it is drawn, so staging it would be staging
        // something written once and read once.
        f.debugLineVertices = createBuffer(*ctx, sizeof(DebugLineVertex) * kMaxDebugLineVertices,
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                           "debugLineVertices");

        // One projection per atlas layer: six for a point, one for a spot. Read by
        // shadow.vert to render a layer and by shadow.glsl to sample it, which is why it
        // is a buffer both stages can see rather than a push constant.
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            f.shadowMatrixBuffer[v] = createBuffer(*ctx, sizeof(glm::mat4) * kMaxShadowLayers,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                   VMA_MEMORY_USAGE_AUTO,
                                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                                   "shadowMatrixBuffer");

            VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setInfo.descriptorPool = descriptorPool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts = &frameSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &setInfo, &f.frameSet[v]),
                    "vkAllocateDescriptorSets(frame)");

            const std::array<VkDescriptorBufferInfo, 3> bufferInfos{
                VkDescriptorBufferInfo{f.uniforms[v].buffer, 0, sizeof(FrameUniforms)},
                VkDescriptorBufferInfo{f.lightBuffer[v].buffer, 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{f.shadowMatrixBuffer[v].buffer, 0, VK_WHOLE_SIZE}};

            std::array<VkWriteDescriptorSet, 3> writes{};
            for (uint32_t i = 0; i < 3; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = f.frameSet[v];
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType =
                    i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &bufferInfos[i];
            }
            vkUpdateDescriptorSets(ctx->device, 3, writes.data(), 0, nullptr);
        }
    }

    renderFinished.resize(swap.imageCount());
    for (auto& sem : renderFinished) {
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(ctx->device, &semInfo, nullptr, &sem), "vkCreateSemaphore(renderFinished)");
    }

    // A resize destroys and rebuilds these, and the instance buffers live in them --
    // so a scene already loaded needs its buffers back before the next frame records.
    if (instances != nullptr) ensureInstanceCapacity(instances->slotCount());
}

void Renderer::destroyFrameResources() {
    for (auto& sem : renderFinished) {
        vkDestroySemaphore(ctx->device, sem, nullptr);
    }
    renderFinished.clear();

    for (auto& f : frames) {
        // The frame set has to be handed back, not merely forgotten. destroyRenderTargets
        // frees its two sets and this function did not, so every resize leaked
        // kFramesInFlight sets and the pool ran dry after a handful of them -- the
        // allocation that then failed took the process down through Logger::critical,
        // mid-swapchain-recreate.
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &f.frameSet[v]);
            f.frameSet[v] = VK_NULL_HANDLE;
        }
        vkDestroyFence(ctx->device, f.inFlight, nullptr);
        f.inFlight = VK_NULL_HANDLE;
        vkDestroySemaphore(ctx->device, f.imageAvailable, nullptr);
        f.imageAvailable = VK_NULL_HANDLE;
        vkDestroyCommandPool(ctx->device, f.pool, nullptr);
        f.pool = VK_NULL_HANDLE;
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            destroyBuffer(*ctx, f.uniforms[v]);
            destroyBuffer(*ctx, f.lightBuffer[v]);
            destroyBuffer(*ctx, f.shadowMatrixBuffer[v]);
        }
        destroyBuffer(*ctx, f.overlayVertices);
        destroyBuffer(*ctx, f.debugLineVertices);
        destroyBuffer(*ctx, f.spriteBuffer);
        if (f.spriteSet != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &f.spriteSet);
            f.spriteSet = VK_NULL_HANDLE;
        }
        destroyBuffer(*ctx, f.instanceData);
        destroyBuffer(*ctx, f.instanceStaging);
        destroyBuffer(*ctx, f.cullStats);
        destroyBuffer(*ctx, f.commandVisibility);
        vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &f.cullSet);
        f.cullSet = VK_NULL_HANDLE;
        vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &f.skinSet);
        f.skinSet = VK_NULL_HANDLE;
        f.instanceRevision = 0;
        f.spriteRevision = 0;
        f.variantAssignment = 0;
        f.opaqueCommandCount = 0;
        f.staticCommandCount = 0;
        f.opaqueRanges.clear();
        f.instanceUploadPending = false;
        f.cmd = VK_NULL_HANDLE;
    }

    // The instance buffers went with the frames, so the capacity they represented did
    // too. Leaving this set would have the next createFrameResources() believe there
    // were buffers to write into.
    instanceCapacity = 0;
    // Same argument, one subsystem along (P4).
    spriteCapacity = 0;
}

void Renderer::createRenderTargets(View& view, VkExtent2D extent) {
    // P2. The layout is how this view's extent lands in the window: an extent equal to the
    // window makes it the identity and `presentTarget` unnecessary -- the one path, at its
    // degenerate case. Recomputed here rather than held, because the window changes under
    // the user's hands and this function is what a resize calls.
    view.renderExtent = extent;
    view.presentPlan = presentLayout(extent.width, extent.height, swap.extent.width, swap.extent.height);

    const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // Every target listed in captureTargets() carries TRANSFER_SRC so it can be read
    // back by name (5.7). Not gated on a debug build: Release is the configuration whose
    // frame is worth inspecting, and a flag that only works in Debug is a flag that is
    // never there when you want it.
    //
    // The worry is that declaring TRANSFER_SRC lets a driver decline compression on
    // these images. Measured rather than assumed, `scripts/bench.sh 5` either way:
    // Frame 4.221 ms without against 4.355 with, and per-pass the zones move in *both*
    // directions (SSAO 0.997 -> 0.885, Lighting 0.957 -> 0.983). That is the run-to-run
    // spread this project already documents for the bimodal zones, not a cost. If a
    // future device disagrees, this one constant is where to gate it.
    const VkImageUsageFlags readback = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // Half the render extent, never smaller than 1x1. Three targets want this -- the bloom
    // chain, SSAO and SSR -- which is one past the point where writing it out again is
    // coincidence. Local to this function because every caller is in it: the record*
    // methods take the extent from the member each creation below stores it in, rather
    // than recomputing it and risking a dispatch that disagrees with its own image.
    //
    // The clamp is not decoration. vkCreateImage rejects a zero extent, and a swapchain
    // one pixel wide is a legal thing for a window manager to hand over mid-resize -- which
    // reaches here whenever no virtual resolution was named, since then the two are one.
    const auto halfOf = [](VkExtent2D e) {
        return VkExtent2D{std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
    };

    // Every render target carries its member name into the capture. `scripts/rdoc.sh
    // images` writes one PNG per name, so these strings are filenames -- keep them
    // identical to the member rather than turning them into prose.
    view.gAlbedo = createImage(*ctx, extent, kAlbedoFormat, colorUsage, 1, msaaSamples,
                          VK_IMAGE_ASPECT_COLOR_BIT, 1, "gAlbedo");
    view.gNormal = createImage(*ctx, extent, kNormalFormat, colorUsage, 1, msaaSamples,
                          VK_IMAGE_ASPECT_COLOR_BIT, 1, "gNormal");
    view.gOrm = createImage(*ctx, extent, kOrmFormat, colorUsage, 1, msaaSamples,
                       VK_IMAGE_ASPECT_COLOR_BIT, 1, "gOrm");
    view.gEmissive = createImage(*ctx, extent, kEmissiveFormat, colorUsage, 1, msaaSamples,
                            VK_IMAGE_ASPECT_COLOR_BIT, 1, "gEmissive");
    view.gDepth = createImage(*ctx, extent, kDepthFormat,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, msaaSamples,
                         VK_IMAGE_ASPECT_DEPTH_BIT, 1, "gDepth");
    view.hdrTarget = createImage(*ctx, extent, kHdrFormat, colorUsage | readback, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, 1, "hdrTarget");

    // Where the tonemap lands when the presentation step is real (P2). Only then: at
    // native the layout is the identity, `recordPresent` records nothing, and the tonemap
    // draws straight into the swapchain image exactly as it did before this row -- which
    // is what makes the eleven golden cases a check on this change rather than a casualty
    // of it.
    //
    // `swap.format` rather than a format of its own, because the blit that follows has to
    // be a byte move. TRANSFER_SRC is what it is blitted from; SAMPLED is not requested,
    // since nothing samples it and asking would be inviting somebody to.
    //
    // **`primary` first, and it is not a micro-optimisation.** A registered view is almost
    // never an exact integer divisor of the window, so `identityPresent` is false for
    // nearly every one of them -- and each would allocate a presentation target it can
    // never reach, since only the presenting view's tonemap sees the swapchain. It would
    // also log a "Presentation:" line per view per resize.
    if (view.primary && !identityPresent(view.presentPlan, swap.extent.width, swap.extent.height)) {
        view.presentTarget = createImage(*ctx, extent, swap.format,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1,
                                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "presentTarget");
        core::Logger::status(core::LogCategory::Render, "Presentation: %ux%u at %ux, %ux%u window, bars %dx%d",
                             view.presentPlan.srcWidth, view.presentPlan.srcHeight, view.presentPlan.scale, swap.extent.width,
                             swap.extent.height, view.presentPlan.x, view.presentPlan.y);
    }

    // The forward pass draws into hdrTarget, which is single-sample, so it needs a
    // single-sample depth buffer to test against. At MSAA that is a resolve of gDepth
    // taken as the G-buffer pass stores; at 1x gDepth already is one.
    if (msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
        view.gDepthResolved = createImage(*ctx, extent, kDepthFormat,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, "gDepthResolved");
        view.forwardDepth = view.gDepthResolved.image;
        view.forwardDepthView = view.gDepthResolved.view;

        // Which resolve mode, and why this is a query rather than a constant.
        //
        // SAMPLE_ZERO is the only mode the spec guarantees for depth, so it is the
        // fallback. AVERAGE is preferred where available purely because the
        // validation layers in SDK 1.3.280 apply VUID-VkRenderingAttachmentInfo-
        // imageView-06129 -- a rule about non-integer *colour* formats -- to depth
        // attachments too, and reject everything except NONE and AVERAGE. The device
        // here advertises all four modes, so this is a layer disagreement rather than
        // a hardware limit, but shipping a pass that logs an error every frame would
        // destroy the one signal that says the renderer is clean.
        //
        // The choice is close to immaterial: every sample of an interior pixel holds
        // the same depth, so the modes differ only on MSAA silhouette pixels, where
        // any single depth is already an approximation of several surfaces.
        VkPhysicalDeviceDepthStencilResolveProperties resolveProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &resolveProps;
        vkGetPhysicalDeviceProperties2(ctx->physicalDevice, &props2);

        depthResolveMode = (resolveProps.supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0
                               ? VK_RESOLVE_MODE_AVERAGE_BIT
                               : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    } else {
        view.forwardDepth = view.gDepth.image;
        view.forwardDepthView = view.gDepth.view;
    }

    // -------------------------------------------------------------------- ssao
    // RGBA8 for a single channel of occlusion, which is wasteful and deliberate:
    // R8_UNORM is only a guaranteed storage-image format when
    // shaderStorageImageExtendedFormats is present, and RGBA8_UNORM always is.
    //
    // Half resolution. Occlusion is an integral over a hemisphere of world-space radius
    // `ssaoRadius`, so its detail is bounded by the geometry that occludes it and not by
    // the pixel grid -- four neighbouring pixels integrate almost the same hemisphere and
    // spend 16 depth taps each to agree on it. Measured on showcase at 4x MSAA: 0.770 ms
    // full resolution against 0.222 here.
    //
    // What it costs is contact detail, which is what SSAO is *for*: a shadow narrower than
    // two full-resolution pixels now resolves to one AO texel. ssao_blur.comp shrank its
    // kernel to 3x3 in the same change to pay part of that back -- see the note there.
    //
    // The depth this samples stays full resolution and is addressed in normalised uv, so
    // the same coordinates reach it unchanged; only the number of hemispheres falls. The
    // lighting pass reads the blurred result through a filtering sampler, so the upsample
    // is free.
    const VkImageUsageFlags aoUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    view.ssaoExtent = halfOf(extent);
    view.ssaoRaw = createImage(*ctx, view.ssaoExtent, kAoFormat, aoUsage | readback, 1, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, 1, "ssaoRaw");
    view.ssaoBlurred = createImage(*ctx, view.ssaoExtent, kAoFormat, aoUsage | readback, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, 1, "ssaoBlurred");
    view.ssaoRawStorage = createStorageView(*ctx, view.ssaoRaw, 0);
    view.ssaoBlurStorage = createStorageView(*ctx, view.ssaoBlurred, 0);

    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &computeImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.ssaoSet), "vkAllocateDescriptorSets(ssao)");
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.ssaoBlurSet), "vkAllocateDescriptorSets(ssao blur)");

        // The AO pass reads the single-sample depth -- the resolve at MSAA, gDepth
        // itself at 1x -- and writes ssaoRaw; the blur reads that and writes
        // ssaoBlurred. Both sources are declared SHADER_READ_ONLY, which is the layout
        // recordGBuffer and recordSsao leave them in.
        const std::array<VkDescriptorImageInfo, 4> aoImages{
            VkDescriptorImageInfo{pointSampler, view.forwardDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, view.ssaoRawStorage, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{pointSampler, view.ssaoRaw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, view.ssaoBlurStorage, VK_IMAGE_LAYOUT_GENERAL}};

        std::array<VkWriteDescriptorSet, 4> aoWrites{};
        const VkDescriptorSet aoSets[4] = {view.ssaoSet, view.ssaoSet, view.ssaoBlurSet, view.ssaoBlurSet};
        for (uint32_t i = 0; i < 4; ++i) {
            aoWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            aoWrites[i].dstSet = aoSets[i];
            aoWrites[i].dstBinding = i % 2;
            aoWrites[i].descriptorCount = 1;
            aoWrites[i].descriptorType = (i % 2) == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            aoWrites[i].pImageInfo = &aoImages[i];
        }
        vkUpdateDescriptorSets(ctx->device, 4, aoWrites.data(), 0, nullptr);
    }

    // -------------------------------------------------------------- shadow mask
    // One 32-bit word per sample, one bit per light. R32_UINT rather than something
    // narrower because `kDefaultLightBudget` is 32 and the mask is exactly one texel wide
    // in lights -- see kShadowMaskLights in rayshadow.glsl for what a wider budget does.
    //
    // Allocated at every configuration, including the ones that never write it: the
    // lighting shader declares the descriptor whether or not the pass runs, which is the
    // same arrangement the AO buffer has and for the same reason -- a layout cannot be
    // specialised away, only the read can.
    view.shadowMask = createImage(*ctx, extent, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, 1,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                  static_cast<uint32_t>(msaaSamples), "shadowMask");
    view.shadowMaskStorage = createStorageView(*ctx, view.shadowMask, 0);

    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &storageImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.shadowMaskSet),
                "vkAllocateDescriptorSets(shadow mask)");

        // GENERAL, and it never leaves it. Both passes reach the image through this one
        // descriptor -- one writes it, the other reads it -- so there is no second layout
        // for a transition to move it to.
        const VkDescriptorImageInfo maskImage{VK_NULL_HANDLE, view.shadowMaskStorage, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet maskWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        maskWrite.dstSet = view.shadowMaskSet;
        maskWrite.dstBinding = 0;
        maskWrite.descriptorCount = 1;
        maskWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        maskWrite.pImageInfo = &maskImage;
        vkUpdateDescriptorSets(ctx->device, 1, &maskWrite, 0, nullptr);
    }

    // ------------------------------------------------------------ light tiles (C35)
    // One word of light bits per 32 lights per tile. At 1600x900 with the default budget
    // that is 5700 tiles of one word: 22 KB, which is why the bitmask won over a compacted
    // index list -- the list would need a count, a per-tile cap, and an ordering rule to
    // keep the shading sum bit-exact, and it would be larger.
    //
    // Allocated at every configuration, including the ones that never write it, for the
    // reason the shadow mask above is: the lighting pipeline declares the descriptor
    // whether or not the pass runs. `tileParams.z` gates the *read*.
    view.lightTileGrid = {(extent.width + kLightTileSize - 1) / kLightTileSize,
                       (extent.height + kLightTileSize - 1) / kLightTileSize};
    {
        const uint32_t words = lightTileWords > 0 ? lightTileWords : 1;
        const VkDeviceSize tileBytes =
            static_cast<VkDeviceSize>(view.lightTileGrid.width) * view.lightTileGrid.height * words * sizeof(uint32_t);
        view.lightTiles = createBuffer(*ctx, std::max<VkDeviceSize>(tileBytes, sizeof(uint32_t)),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, 0,
                                          "lightTiles");

        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &lightTileSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.lightTileSet),
                "vkAllocateDescriptorSets(light tiles)");

        const VkDescriptorBufferInfo tileBuffer{view.lightTiles.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet tileWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tileWrite.dstSet = view.lightTileSet;
        tileWrite.dstBinding = 0;
        tileWrite.descriptorCount = 1;
        tileWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tileWrite.pBufferInfo = &tileBuffer;
        vkUpdateDescriptorSets(ctx->device, 1, &tileWrite, 0, nullptr);
    }

    // G-buffer descriptor set.
    VkDescriptorSetAllocateInfo gbufferAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    gbufferAlloc.descriptorPool = descriptorPool;
    gbufferAlloc.descriptorSetCount = 1;
    gbufferAlloc.pSetLayouts = &gbufferSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &gbufferAlloc, &view.gbufferSet), "vkAllocateDescriptorSets(gbuffer)");

    // Binding 5 is the blurred AO, single-sampled even when the rest of this set is
    // not: it is a screen-space quantity, one value per pixel, and the per-sample
    // lighting loop reads the same one for every sample.
    //
    // It is also the one entry at *half* resolution, so it needs a filtering sampler or
    // it upsamples as visible 2x2 blocks. `pointSampler` supplies that despite the name --
    // it is VK_FILTER_LINEAR on both axes with CLAMP_TO_EDGE (see where it is created);
    // `fontSampler` is the NEAREST one. The name is a leftover and the only thing here
    // depending on it is this binding, so do not "fix" it by swapping in a nearest
    // sampler. bloomSampler would say it better but does not exist yet at this point in
    // createRenderTargets.
    const std::array<VkImageView, 6> views{view.gAlbedo.view,   view.gNormal.view,     view.gOrm.view,
                                           view.gDepth.view,    view.gEmissive.view,   view.ssaoBlurred.view};
    std::array<VkDescriptorImageInfo, 6> imageInfos{};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t i = 0; i < 6; ++i) {
        imageInfos[i] = {pointSampler, views[i],
                         i == 3 ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = view.gbufferSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 6, writes.data(), 0, nullptr);

    // ----------------------------------------------------------- Hi-Z pyramid (C11)
    //
    // Power-of-two, and rounded *down* rather than up. A pyramid larger than the depth it
    // samples would leave its outer texels holding whatever the reduction wrote for a
    // clamped tap, and an occlusion test reading those would cull against depth that was
    // never rendered. Rounding down loses at most a half texel of the screen edge, which
    // for a conservative test is the safe direction.
    {
        const auto floorPow2 = [](uint32_t v) {
            uint32_t r = 1;
            while (r * 2 <= v) r *= 2;
            return r;
        };
        view.depthPyramidExtent = {std::max(floorPow2(extent.width), 1u), std::max(floorPow2(extent.height), 1u)};
        view.depthPyramidLevels = 1;
        uint32_t w = view.depthPyramidExtent.width;
        uint32_t h = view.depthPyramidExtent.height;
        while (view.depthPyramidLevels < kDepthPyramidMips && (w > 1 || h > 1)) {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            ++view.depthPyramidLevels;
        }

        view.depthPyramid = createImage(*ctx, view.depthPyramidExtent, VK_FORMAT_R32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, view.depthPyramidLevels,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "depthPyramid");

        for (uint32_t m = 0; m < view.depthPyramidLevels; ++m) {
            view.depthPyramidStorage[m] = createStorageView(*ctx, view.depthPyramid, m);
        }

        std::array<VkDescriptorImageInfo, kDepthPyramidMips * 2> pyramidImages{};
        std::array<VkWriteDescriptorSet, kDepthPyramidMips * 2> pyramidWrites{};
        for (uint32_t m = 0; m < view.depthPyramidLevels; ++m) {
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = descriptorPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &computeImageSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.depthPyramidSets[m]),
                    "vkAllocateDescriptorSets(depth pyramid)");

            // Level 0 reduces the single-sample depth; every level after reduces the
            // pyramid itself, which stays in GENERAL for the whole build so the storage
            // write of one level and the sampled read of the next need no layout change.
            const bool fromDepth = m == 0;
            pyramidImages[m * 2] = {pointSampler, fromDepth ? view.forwardDepthView : view.depthPyramid.view,
                                    fromDepth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
            pyramidImages[m * 2 + 1] = {VK_NULL_HANDLE, view.depthPyramidStorage[m], VK_IMAGE_LAYOUT_GENERAL};

            pyramidWrites[m * 2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            pyramidWrites[m * 2].dstSet = view.depthPyramidSets[m];
            pyramidWrites[m * 2].dstBinding = 0;
            pyramidWrites[m * 2].descriptorCount = 1;
            pyramidWrites[m * 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pyramidWrites[m * 2].pImageInfo = &pyramidImages[m * 2];

            pyramidWrites[m * 2 + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            pyramidWrites[m * 2 + 1].dstSet = view.depthPyramidSets[m];
            pyramidWrites[m * 2 + 1].dstBinding = 1;
            pyramidWrites[m * 2 + 1].descriptorCount = 1;
            pyramidWrites[m * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            pyramidWrites[m * 2 + 1].pImageInfo = &pyramidImages[m * 2 + 1];
        }
        vkUpdateDescriptorSets(ctx->device, view.depthPyramidLevels * 2, pyramidWrites.data(), 0, nullptr);

        // The cull dispatch's view of the finished pyramid, rewritten here on every resize
        // because that is exactly when the view it names is replaced. Allocated only when
        // the view does not already hold one, which is what `destroyRenderTargets` freeing
        // it makes true again -- a second View gets its own rather than inheriting this.
        if (view.hizSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = descriptorPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &hizSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.hizSet), "vkAllocateDescriptorSets(hiz)");
        }
        const VkDescriptorImageInfo hizImage{hizSampler, view.depthPyramid.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet hizWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        hizWrite.dstSet = view.hizSet;
        hizWrite.dstBinding = 0;
        hizWrite.descriptorCount = 1;
        hizWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        hizWrite.pImageInfo = &hizImage;
        vkUpdateDescriptorSets(ctx->device, 1, &hizWrite, 0, nullptr);
    }

    // ------------------------------------------------------------------- bloom
    // Half resolution. The clamp inside halfOf matters more here than anywhere else: at
    // 5 mips a window narrower than 32 pixels would otherwise produce a zero-sized
    // level, and vkCreateImage rejects that rather than clamping.
    const VkExtent2D bloomExtent = halfOf(extent);
    view.bloomChain = createImage(*ctx, bloomExtent, kBloomFormat,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | readback, kBloomMips,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "bloomChain");

    for (uint32_t m = 0; m < kBloomMips; ++m) view.bloomStorageViews[m] = createStorageView(*ctx, view.bloomChain, m);

    // 2 * kBloomMips sets, one per dispatch. They are allocated here rather than
    // rebound per frame because none of them ever changes: the chain is the same image
    // every frame and only its contents move.
    for (uint32_t m = 0; m < kBloomMips; ++m) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &computeImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.bloomDownSets[m]), "vkAllocateDescriptorSets(bloom)");
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.bloomUpSets[m]), "vkAllocateDescriptorSets(bloom)");
    }

    // The threshold pass reads the lit HDR image; every other pass reads the chain
    // itself at an explicit LOD, which is why they all share bloomChain.view.
    // Fixed arrays, not vectors: every write holds a pointer into the image-info
    // storage, and a vector that reallocated mid-loop would leave those dangling with
    // no diagnostic beyond a corrupt descriptor.
    constexpr uint32_t kBloomDispatches = 2 * kBloomMips - 1; // kBloomMips down, kBloomMips-1 up
    std::array<VkDescriptorImageInfo, kBloomDispatches * 2> bloomImages{};
    std::array<VkWriteDescriptorSet, kBloomDispatches * 2> bloomWrites{};
    uint32_t bloomWriteCount = 0;

    // The source layout is not the same for every set. The threshold pass reads
    // hdrTarget, which the frame leaves in SHADER_READ_ONLY; every other pass reads
    // the chain, which stays in GENERAL for the duration so the storage writes and
    // sampled reads can interleave without a layout change per step. A descriptor
    // declaring the wrong one is a validation error at dispatch, not at update.
    auto bindPair = [&](VkDescriptorSet set, VkImageView source, VkImageLayout sourceLayout, VkImageView dest) {
        const uint32_t i = bloomWriteCount;
        bloomImages[i] = {bloomSampler, source, sourceLayout};
        bloomImages[i + 1] = {VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL};

        bloomWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        bloomWrites[i].dstSet = set;
        bloomWrites[i].dstBinding = 0;
        bloomWrites[i].descriptorCount = 1;
        bloomWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomWrites[i].pImageInfo = &bloomImages[i];

        bloomWrites[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        bloomWrites[i + 1].dstSet = set;
        bloomWrites[i + 1].dstBinding = 1;
        bloomWrites[i + 1].descriptorCount = 1;
        bloomWrites[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bloomWrites[i + 1].pImageInfo = &bloomImages[i + 1];

        bloomWriteCount += 2;
    };

    bindPair(view.bloomDownSets[0], view.hdrTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, view.bloomStorageViews[0]);
    for (uint32_t m = 1; m < kBloomMips; ++m) {
        bindPair(view.bloomDownSets[m], view.bloomChain.view, VK_IMAGE_LAYOUT_GENERAL, view.bloomStorageViews[m]);
    }
    for (uint32_t m = 0; m + 1 < kBloomMips; ++m) {
        bindPair(view.bloomUpSets[m], view.bloomChain.view, VK_IMAGE_LAYOUT_GENERAL, view.bloomStorageViews[m]);
    }
    vkUpdateDescriptorSets(ctx->device, bloomWriteCount, bloomWrites.data(), 0, nullptr);

    // The last up set is allocated but never written or bound; there is no mip above
    // the smallest to add. Freeing it would make the two arrays disagree in length for
    // no gain.

    // HDR descriptor set for the tonemap pass.
    VkDescriptorSetAllocateInfo hdrAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    hdrAlloc.descriptorPool = descriptorPool;
    hdrAlloc.descriptorSetCount = 1;
    hdrAlloc.pSetLayouts = &hdrSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &hdrAlloc, &view.hdrSet), "vkAllocateDescriptorSets(hdr)");

    // Binding 1 is the bloom chain, sampled at LOD 0 -- the top of the chain already
    // holds every level summed back down into it by the upsample walk.
    const std::array<VkDescriptorImageInfo, 2> hdrImages{
        VkDescriptorImageInfo{pointSampler, view.hdrTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{bloomSampler, view.bloomChain.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};

    std::array<VkWriteDescriptorSet, 2> hdrWrites{};
    for (uint32_t i = 0; i < hdrWrites.size(); ++i) {
        hdrWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        hdrWrites[i].dstSet = view.hdrSet;
        hdrWrites[i].dstBinding = i;
        hdrWrites[i].descriptorCount = 1;
        hdrWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        hdrWrites[i].pImageInfo = &hdrImages[i];
    }
    vkUpdateDescriptorSets(ctx->device, 2, hdrWrites.data(), 0, nullptr);

    // --------------------------------------------------------------------- ssr
    // Full resolution by default; `render.ssrScale` is what moves it, and at 1.0 this is
    // `extent` exactly rather than `extent` through a float, so the default path is the
    // one it always was. Below 1.0 recordSsr picks the upsampling composite instead of the
    // plain one -- the two must move together, and `ssrExtent == renderExtent` is the only
    // thing either of them tests.
    view.ssrExtent = scaledBy(extent, ssrScale);
    builtSsrScale = ssrScale;
    view.ssrTarget = createImage(*ctx, view.ssrExtent, kHdrFormat,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | readback, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "ssrTarget");
    view.ssrStorage = createStorageView(*ctx, view.ssrTarget, 0);
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &computeImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.ssrImageSet), "vkAllocateDescriptorSets(ssr)");
        alloc.pSetLayouts = &singleImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.ssrCompositeSet),
                "vkAllocateDescriptorSets(ssr composite)");

        const std::array<VkDescriptorImageInfo, 3> ssrImages{
            VkDescriptorImageInfo{pointSampler, view.hdrTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, view.ssrStorage, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{bloomSampler, view.ssrTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        std::array<VkWriteDescriptorSet, 3> ssrWrites{};
        const VkDescriptorSet dst[3] = {view.ssrImageSet, view.ssrImageSet, view.ssrCompositeSet};
        const uint32_t bindingOf[3] = {0, 1, 0};
        for (uint32_t i = 0; i < 3; ++i) {
            ssrWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ssrWrites[i].dstSet = dst[i];
            ssrWrites[i].dstBinding = bindingOf[i];
            ssrWrites[i].descriptorCount = 1;
            ssrWrites[i].descriptorType =
                i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ssrWrites[i].pImageInfo = &ssrImages[i];
        }
        vkUpdateDescriptorSets(ctx->device, 3, ssrWrites.data(), 0, nullptr);
    }

    // ------------------------------------------------------- resolved scene depth
    // Written here rather than beside the pass that uses it, because there are two of
    // them now: the decal projection (3.3) and the particle collision test (S3.4).
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &singleImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.sceneDepthSet),
                "vkAllocateDescriptorSets(scene depth)");

        const VkDescriptorImageInfo depthInfo{pointSampler, view.forwardDepthView,
                                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = view.sceneDepthSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &depthInfo;
        vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);
    }

    // --------------------------------------------------------------------- fog
    view.fogTarget = createImage(*ctx, extent, kHdrFormat,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | readback, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "fogTarget");
    view.fogStorage = createStorageView(*ctx, view.fogTarget, 0);
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &computeImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.fogImageSet), "vkAllocateDescriptorSets(fog)");
        alloc.pSetLayouts = &singleImageSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &view.fogCompositeSet),
                "vkAllocateDescriptorSets(fog composite)");

        // The march reads the single-sample depth -- the resolve at MSAA, gDepth at 1x
        // -- exactly as SSAO and TAA do.
        const std::array<VkDescriptorImageInfo, 3> fogImages{
            VkDescriptorImageInfo{pointSampler, view.forwardDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, view.fogStorage, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{bloomSampler, view.fogTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        std::array<VkWriteDescriptorSet, 3> fogWrites{};
        const VkDescriptorSet fogDst[3] = {view.fogImageSet, view.fogImageSet, view.fogCompositeSet};
        const uint32_t fogBinding[3] = {0, 1, 0};
        for (uint32_t i = 0; i < 3; ++i) {
            fogWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            fogWrites[i].dstSet = fogDst[i];
            fogWrites[i].dstBinding = fogBinding[i];
            fogWrites[i].descriptorCount = 1;
            fogWrites[i].descriptorType =
                i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            fogWrites[i].pImageInfo = &fogImages[i];
        }
        vkUpdateDescriptorSets(ctx->device, 3, fogWrites.data(), 0, nullptr);
    }

    // --------------------------------------------------------------------- taa
    // Always allocated, even with TAA off. Two RGBA16F targets at 1600x900 is 11.5 MB
    // against the 649 MB the scene already holds, and allocating them lazily would
    // mean a keypress that toggles TAA had to rebuild render targets rather than a
    // pipeline -- which is the one thing the feature-key path deliberately avoids.
    const VkImageUsageFlags taaUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | readback;
    for (uint32_t i = 0; i < 2; ++i) {
        view.taaHistory[i] = createImage(*ctx, extent, kHdrFormat, taaUsage, 1, VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 1, i == 0 ? "taaHistory0" : "taaHistory1");
        view.taaHistoryStorage[i] = createStorageView(*ctx, view.taaHistory[i], 0);
    }
    view.taaHistoryValid = false;

    // Allocated on the same terms as the history above and for the same reason: a
    // keypress that toggles TAA rebuilds a pipeline, never a render target.
    view.velocityTarget = createImage(*ctx, extent, VK_FORMAT_R16G16_SFLOAT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | readback, 1,
                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "velocityTarget");

    {
        VkDescriptorSetAllocateInfo taaAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        taaAlloc.descriptorPool = descriptorPool;
        taaAlloc.descriptorSetCount = 1;
        for (uint32_t i = 0; i < 2; ++i) {
            taaAlloc.pSetLayouts = &taaSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &taaAlloc, &view.taaSet[i]), "vkAllocateDescriptorSets(taa)");
            taaAlloc.pSetLayouts = &hdrSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &taaAlloc, &view.taaOutputSet[i]),
                    "vkAllocateDescriptorSets(taa output)");
        }

        // Four bindings per resolve set: the frame's lit image, the other history, the
        // single-sample depth, and the history being written. The depth view is the
        // same one SSAO and the forward pass use -- the resolve at MSAA, gDepth at 1x
        // -- so TAA never has to know which of the two it got.
        constexpr uint32_t kTaaBindings = 5;
        std::array<VkDescriptorImageInfo, kTaaBindings * 2> taaImages{};
        std::array<VkWriteDescriptorSet, kTaaBindings * 2> taaWrites{};
        std::array<VkDescriptorImageInfo, 4> outImages{};
        std::array<VkWriteDescriptorSet, 4> outWrites{};

        for (uint32_t p = 0; p < 2; ++p) {
            const uint32_t b = p * kTaaBindings;
            taaImages[b + 0] = {pointSampler, view.hdrTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            // Linear on the history: reprojection lands between texels, and a nearest
            // tap there quantises the motion to whole pixels, which shows up as the
            // image crawling one pixel at a time under a slow pan.
            taaImages[b + 1] = {bloomSampler, view.taaHistory[1 - p].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            taaImages[b + 2] = {pointSampler, view.forwardDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
            taaImages[b + 3] = {VK_NULL_HANDLE, view.taaHistoryStorage[p], VK_IMAGE_LAYOUT_GENERAL};
            // Point, not linear: the correction belongs to the surface the same
            // rasterisation put this depth on, and a bilinear tap along a silhouette
            // straddles two of them. See the texelFetch in taa.comp.
            taaImages[b + 4] = {pointSampler, view.velocityTarget.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

            for (uint32_t i = 0; i < kTaaBindings; ++i) {
                taaWrites[b + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                taaWrites[b + i].dstSet = view.taaSet[p];
                taaWrites[b + i].dstBinding = i;
                taaWrites[b + i].descriptorCount = 1;
                taaWrites[b + i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                taaWrites[b + i].pImageInfo = &taaImages[b + i];
            }

            outImages[p * 2 + 0] = {pointSampler, view.taaHistory[p].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            outImages[p * 2 + 1] = {bloomSampler, view.bloomChain.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            for (uint32_t i = 0; i < 2; ++i) {
                outWrites[p * 2 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                outWrites[p * 2 + i].dstSet = view.taaOutputSet[p];
                outWrites[p * 2 + i].dstBinding = i;
                outWrites[p * 2 + i].descriptorCount = 1;
                outWrites[p * 2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                outWrites[p * 2 + i].pImageInfo = &outImages[p * 2 + i];
            }
        }
        vkUpdateDescriptorSets(ctx->device, kTaaBindings * 2, taaWrites.data(), 0, nullptr);
        vkUpdateDescriptorSets(ctx->device, 4, outWrites.data(), 0, nullptr);
    }
}

void Renderer::createTaaPipeline() {
    // Four sampled images and one storage image. Not computeImageSetLayout, which is
    // the one-in-one-out shape bloom and SSAO share; a resolve reads four sources and
    // parameterising that layout by count would make both harder to read than having
    // two of them. Binding 3 is the output and 4 is 3.4's motion correction, which is
    // why the storage image is in the middle rather than last.
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {static_cast<uint32_t>(i),
                       i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }

    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &info, nullptr, &taaSetLayout),
            "vkCreateDescriptorSetLayout(taa)");
    layoutBindings[taaSetLayout] = {bindings.begin(), bindings.end()};

    taaLayout = createLayout("taa", {"taa.comp"}, {frameSetLayout, taaSetLayout},
                             {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TaaPush)}});
    taaPipeline = createComputePipeline(*ctx, taaLayout, "taa.comp");
}

void Renderer::destroyTaaPipeline() {
    vkDestroyPipeline(ctx->device, taaPipeline, nullptr);
    taaPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, taaLayout, nullptr);
    taaLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, taaSetLayout, nullptr);
    taaSetLayout = VK_NULL_HANDLE;
}

void Renderer::createBloomPipelines() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; // LODs are addressed exactly, never blended
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCheck(vkCreateSampler(ctx->device, &samplerInfo, nullptr, &bloomSampler), "vkCreateSampler(bloom)");

    bloomLayout = createLayout("bloom", {"bloom_threshold.comp", "bloom_down.comp", "bloom_up.comp"},
                               {computeImageSetLayout},
                               {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPush)}});
    bloomThresholdPipeline = createComputePipeline(*ctx, bloomLayout, "bloom_threshold.comp");
    bloomDownPipeline = createComputePipeline(*ctx, bloomLayout, "bloom_down.comp");
    bloomUpPipeline = createComputePipeline(*ctx, bloomLayout, "bloom_up.comp");
}

void Renderer::createDepthPyramidPipeline() {
    depthPyramidLayout = createLayout("depth pyramid", {"depth_pyramid.comp"}, {computeImageSetLayout},
                                      {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DepthPyramidPush)}});
    verifyShaderBindings("depth pyramid", {"depth_pyramid.comp"}, {computeImageSetLayout}, 0);
    depthPyramidPipeline = createComputePipeline(*ctx, depthPyramidLayout, "depth_pyramid.comp");
}

void Renderer::destroyDepthPyramidPipeline() {
    vkDestroyPipeline(ctx->device, depthPyramidPipeline, nullptr);
    depthPyramidPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, depthPyramidLayout, nullptr);
    depthPyramidLayout = VK_NULL_HANDLE;
}

void Renderer::recordDepthPyramidLayout(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("HiZLayout");
    if (view.depthPyramid.image == VK_NULL_HANDLE) return;
    // A zone on a lone barrier, so that every GPU command inside `Frame` sits in one.
    // It measures 0.001 ms, which is the point: an unnamed 0.001 and an unnamed 0.5 look
    // identical from outside.
    GpuScope zone(gpuProfiler, cmd, slot, "HiZLayout");
    // Before the first cull dispatch, because that dispatch binds the pyramid and its
    // descriptor declares GENERAL. On frame one the image is still UNDEFINED, and a
    // descriptor that disagrees with the image is a validation error at submit -- not at
    // the dispatch that would have read it, and not only on the frame that matters.
    //
    // UNDEFINED as the source every frame: the contents are rebuilt from scratch below, so
    // there is nothing to preserve and discarding is cheaper than not.
    // `VK_ACCESS_2_SHADER_READ_BIT` rather than `SHADER_SAMPLED_READ`, and it is not
    // belt-and-braces: a combined image sampler in a compute shader is attributed to
    // *storage* read, so a barrier naming only the sampled form covers none of it and sync
    // validation says so at every dispatch that binds the pyramid.
    transitionImage(cmd, view.depthPyramid.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, view.depthPyramidLevels);
}

void Renderer::recordDepthPyramid(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("HiZ");
    if (depthPyramidPipeline == VK_NULL_HANDLE || view.depthPyramid.image == VK_NULL_HANDLE) return;

    GpuScope zone(gpuProfiler, cmd, slot, "HiZ");

    // The whole chain lives in GENERAL for the build. A per-level layout transition would
    // be correct and would also be five barriers doing what one does: the levels are
    // written and read as storage and sampled data in strict sequence, and it is the
    // *execution* dependency between them that matters, not the layout.
    // Already GENERAL -- `recordDepthPyramidLayout` put it there before the first cull
    // dispatch bound it. What this orders is the write against that dispatch's read: the
    // phase 0 cull has the pyramid bound, validation counts that as a read, and overwriting
    // it here without a barrier is a write-after-read.
    transitionImage(cmd, view.depthPyramid.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, view.depthPyramidLevels);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPyramidPipeline);

    uint32_t w = view.depthPyramidExtent.width;
    uint32_t h = view.depthPyramidExtent.height;
    for (uint32_t m = 0; m < view.depthPyramidLevels; ++m) {
        if (m > 0) {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            // Level m reads level m-1. Without this the reduction races its own input and
            // the upper levels hold whatever happened to have landed -- which looks like
            // an occlusion test that culls at random rather than like a missing barrier.
            transitionImage(cmd, view.depthPyramid.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, view.depthPyramidLevels);
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPyramidLayout, 0, 1, &view.depthPyramidSets[m], 0,
                                nullptr);
        DepthPyramidPush push{};
        push.destSize = {w, h};
        push.sourceLod = m == 0 ? 0u : m - 1;
        vkCmdPushConstants(cmd, depthPyramidLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    }

    // Written by compute, read by the second cull dispatch.
    transitionImage(cmd, view.depthPyramid.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, view.depthPyramidLevels);
}

void Renderer::destroyBloomPipelines() {
    vkDestroyPipeline(ctx->device, bloomThresholdPipeline, nullptr);
    bloomThresholdPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, bloomDownPipeline, nullptr);
    bloomDownPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, bloomUpPipeline, nullptr);
    bloomUpPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, bloomLayout, nullptr);
    bloomLayout = VK_NULL_HANDLE;
    vkDestroySampler(ctx->device, bloomSampler, nullptr);
    bloomSampler = VK_NULL_HANDLE;
}

void Renderer::createSsaoPipelines() {
    // Two sets: the frame UBO, because reconstructing world position from depth needs
    // invViewProj and viewProj, then the image pair. The blur uses only the second and
    // simply does not declare the first.
    ssaoLayout = createLayout("ssao", {"ssao.comp", "ssao_blur.comp"}, {frameSetLayout, computeImageSetLayout},
                              {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SsaoPush)}});
    ssaoPipeline = createComputePipeline(*ctx, ssaoLayout, "ssao.comp");
    ssaoBlurPipeline = createComputePipeline(*ctx, ssaoLayout, "ssao_blur.comp");
}

void Renderer::destroySsaoPipelines() {
    vkDestroyPipeline(ctx->device, ssaoPipeline, nullptr);
    ssaoPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, ssaoBlurPipeline, nullptr);
    ssaoBlurPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, ssaoLayout, nullptr);
    ssaoLayout = VK_NULL_HANDLE;
}


/// Depth-only, so no colour format and no blend state. D32 rather than D16: the box spans
/// tens of metres and 16 bits of it puts visible terracing on a long floor.
static constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

void Renderer::createShadowResources() {
    // One layer, so `createImage` hands back a plain 2D view and there is nothing to
    // build per-layer views for -- the sampler is a sampler2DShadow rather than an array,
    // and the pass renders into `shadowMap.view` directly.
    shadowMap = createImage(*ctx, {kShadowMapSize, kShadowMapSize}, kShadowFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, "shadowMap");

    punctualShadowMap = createImage(*ctx, {kPunctualShadowSize, kPunctualShadowSize}, kShadowFormat,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, kMaxShadowLayers,
                                    "punctualShadowMap");
    for (uint32_t i = 0; i < kMaxShadowLayers; ++i) {
        punctualShadowLayerViews[i] = createLayerView(*ctx, punctualShadowMap, i, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    punctualCacheCold = true;

    // A comparison sampler with LINEAR filtering is what makes each tap a hardware 2x2
    // PCF instead of a raw depth fetch, so the 3x3 loop in shadow.glsl is really 6x6.
    // CLAMP_TO_BORDER with a white border means anything sampled outside a cascade reads
    // as fully lit rather than wrapping into a neighbour's depth.
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    info.compareEnable = VK_TRUE;
    // Forward-Z in this map, unlike the main camera: an orthographic projection spreads
    // depth evenly, so reverse-Z buys nothing and LESS_OR_EQUAL is the natural compare.
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCheck(vkCreateSampler(ctx->device, &info, nullptr, &shadowSampler), "vkCreateSampler(shadow)");

    // Both maps share the sampler: same comparison, same border, same filter. What
    // differs between them is the projection, and that is in the matrices, not here.
    const std::array<VkDescriptorImageInfo, 2> shadowInfos{
        VkDescriptorImageInfo{shadowSampler, shadowMap.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{shadowSampler, punctualShadowMap.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = tlasSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &shadowInfos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 2, writes.data(), 0, nullptr);
}

void Renderer::createIblResources() {
    auto s = core::Profiler::scope("Renderer::createIbl");

    // 512, not the 128 the split-sum chain needs. The convolved cubes below are sampled
    // through a cosine or a roughness lobe and gain nothing from more texels, but a ray
    // that misses reads this one *directly*, and a near-mirror surface resolves it almost
    // 1:1 -- at 128 the sky in a polished reflection was visibly blocky. Baked once at
    // startup, so this is load time and VRAM, not frame time.
    constexpr uint32_t kEnvSize = 512;
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilterSize = 128;
    constexpr uint32_t kPrefilterMips = 5;
    constexpr uint32_t kBrdfSize = 256;
    constexpr VkFormat kIblFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    const uint32_t envMips = mipLevelsFor({kEnvSize, kEnvSize});
    const VkImageUsageFlags cubeUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    envCube = createCubeImage(*ctx, kEnvSize, kIblFormat, cubeUsage, envMips, "envCube");
    irradianceCube = createCubeImage(*ctx, kIrradianceSize, kIblFormat, cubeUsage, 1, "irradianceCube");
    prefilteredCube = createCubeImage(*ctx, kPrefilterSize, kIblFormat, cubeUsage, kPrefilterMips, "prefilteredCube");
    brdfLut = createImage(*ctx, {kBrdfSize, kBrdfSize}, VK_FORMAT_R16G16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          1, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, 1, "brdfLut");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCheck(vkCreateSampler(ctx->device, &samplerInfo, nullptr, &iblSampler), "vkCreateSampler(ibl)");

    // ------------------------------------------------------------ compute layouts
    // One layout shape for all four passes: binding 0 the source cube, binding 1 the
    // destination storage image. sky and brdf have no source, so they simply do not
    // declare binding 0 and it is statically unused -- which is why every one of these
    // shaders must put its *output* at binding 1, including the 2D BRDF LUT.
    std::array<VkDescriptorSetLayoutBinding, 2> computeBindings{};
    computeBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    computeBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo computeLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    computeLayoutInfo.bindingCount = 2;
    computeLayoutInfo.pBindings = computeBindings.data();

    VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &computeLayoutInfo, nullptr, &computeSetLayout),
            "vkCreateDescriptorSetLayout(ibl compute)");

    // Registered only across the creation, which verifies as it goes. This set layout is
    // local to the IBL bake and is destroyed at the end of the function, so leaving it in
    // the map would leave a key pointing at a freed handle -- and handles get reused.
    layoutBindings[computeSetLayout] = {computeBindings.begin(), computeBindings.end()};
    // 32 bytes covers the largest of the four push blocks.
    const VkPipelineLayout computeLayout =
        createLayout("ibl", {"sky.comp", "irradiance.comp", "prefilter.comp", "brdf_lut.comp"}, {computeSetLayout},
                     {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48}});
    layoutBindings.erase(computeSetLayout);

    VkPipeline skyPipe = createComputePipeline(*ctx, computeLayout, "sky.comp");
    VkPipeline irradiancePipe = createComputePipeline(*ctx, computeLayout, "irradiance.comp");
    VkPipeline prefilterPipe = createComputePipeline(*ctx, computeLayout, "prefilter.comp");
    VkPipeline brdfPipe = createComputePipeline(*ctx, computeLayout, "brdf_lut.comp");

    // Storage views: one per destination mip. A cube cannot be written through a CUBE
    // view, so each of these is a 2D_ARRAY view of six faces.
    std::vector<VkImageView> storageViews;
    storageViews.push_back(createStorageView(*ctx, envCube, 0));
    storageViews.push_back(createStorageView(*ctx, irradianceCube, 0));
    for (uint32_t m = 0; m < kPrefilterMips; ++m) storageViews.push_back(createStorageView(*ctx, prefilteredCube, m));
    storageViews.push_back(createStorageView(*ctx, brdfLut, 0));

    const uint32_t setCount = static_cast<uint32_t>(storageViews.size());
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount};

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool computePool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(ctx->device, &poolInfo, nullptr, &computePool), "vkCreateDescriptorPool(ibl)");

    std::vector<VkDescriptorSetLayout> layouts(setCount, computeSetLayout);
    std::vector<VkDescriptorSet> sets(setCount);
    VkDescriptorSetAllocateInfo setAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAlloc.descriptorPool = computePool;
    setAlloc.descriptorSetCount = setCount;
    setAlloc.pSetLayouts = layouts.data();
    vkCheck(vkAllocateDescriptorSets(ctx->device, &setAlloc, sets.data()), "vkAllocateDescriptorSets(ibl)");

    // Every set reads the environment cube and writes one storage view. The sky pass
    // reads nothing, but binding 0 must still point somewhere valid.
    for (uint32_t i = 0; i < setCount; ++i) {
        VkDescriptorImageInfo sampled{iblSampler, envCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo storage{VK_NULL_HANDLE, storageViews[i], VK_IMAGE_LAYOUT_GENERAL};

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &sampled;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &storage;
        vkUpdateDescriptorSets(ctx->device, 2, writes.data(), 0, nullptr);
    }

    // ------------------------------------------------------------------- dispatch
    const auto dispatch2D = [](VkCommandBuffer c, uint32_t size, uint32_t layers) {
        vkCmdDispatch(c, (size + 7) / 8, (size + 7) / 8, layers);
    };

    VkCommandBuffer cmd = uploader->beginImmediate(*ctx);

    // sky -> envCube mip 0
    transitionImage(cmd, envCube.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &sets[0], 0, nullptr);
    struct {
        glm::vec4 sunDirection;
        glm::vec4 sunColor;
        uint32_t size;
    } skyPush{glm::vec4(glm::normalize(sunDirection), sunIntensity), glm::vec4(sunColorValue, 1.0f), kEnvSize};
    vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(skyPush), &skyPush);
    dispatch2D(cmd, kEnvSize, 6);

    uploader->endImmediate(*ctx);

    // Mip chain for envCube, so prefilter can sample a level matched to its lobe.
    cmd = uploader->beginImmediate(*ctx);
    transitionImage(cmd, envCube.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    int32_t mipSize = static_cast<int32_t>(kEnvSize);
    for (uint32_t level = 1; level < envMips; ++level) {
        transitionImage(cmd, envCube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1);

        const int32_t next = mipSize > 1 ? mipSize / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 6};
        blit.srcOffsets[1] = {mipSize, mipSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 6};
        blit.dstOffsets[1] = {next, next, 1};
        vkCmdBlitImage(cmd, envCube.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, envCube.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        transitionImage(cmd, envCube.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1);
        mipSize = next;
    }
    transitionImage(cmd, envCube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, envMips - 1, 1);
    uploader->endImmediate(*ctx);

    // irradiance, prefilter, BRDF LUT -- all read the now-complete envCube.
    cmd = uploader->beginImmediate(*ctx);
    for (GpuImage* img : {&irradianceCube, &prefilteredCube, &brdfLut}) {
        transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &sets[1], 0, nullptr);
    struct {
        uint32_t size;
        float sampleDelta;
    } irrPush{kIrradianceSize, 0.025f};
    vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(irrPush), &irrPush);
    dispatch2D(cmd, kIrradianceSize, 6);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterPipe);
    for (uint32_t m = 0; m < kPrefilterMips; ++m) {
        const uint32_t faceSize = kPrefilterSize >> m;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &sets[2 + m], 0, nullptr);
        struct {
            uint32_t size;
            float roughness;
            uint32_t sampleCount;
            uint32_t envSize;
        } prePush{faceSize, static_cast<float>(m) / static_cast<float>(kPrefilterMips - 1), 256u, kEnvSize};
        vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(prePush), &prePush);
        dispatch2D(cmd, faceSize, 6);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdfPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &sets[setCount - 1], 0, nullptr);
    struct {
        uint32_t size;
        uint32_t sampleCount;
    } brdfPush{kBrdfSize, 512u};
    vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(brdfPush), &brdfPush);
    dispatch2D(cmd, kBrdfSize, 1);

    for (GpuImage* img : {&irradianceCube, &prefilteredCube, &brdfLut}) {
        transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    uploader->endImmediate(*ctx);

    // Everything above was scaffolding for a one-time computation; only the four
    // images and the sampler outlive this function.
    for (VkImageView storage : storageViews) vkDestroyImageView(ctx->device, storage, nullptr);
    vkDestroyPipeline(ctx->device, skyPipe, nullptr);
    vkDestroyPipeline(ctx->device, irradiancePipe, nullptr);
    vkDestroyPipeline(ctx->device, prefilterPipe, nullptr);
    vkDestroyPipeline(ctx->device, brdfPipe, nullptr);
    vkDestroyPipelineLayout(ctx->device, computeLayout, nullptr);
    vkDestroyDescriptorPool(ctx->device, computePool, nullptr);
    vkDestroyDescriptorSetLayout(ctx->device, computeSetLayout, nullptr);

    // The set the lighting pass binds: irradiance, prefiltered, BRDF LUT.
    VkDescriptorSetAllocateInfo iblAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    iblAlloc.descriptorPool = descriptorPool;
    iblAlloc.descriptorSetCount = 1;
    iblAlloc.pSetLayouts = &iblSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx->device, &iblAlloc, &iblSet), "vkAllocateDescriptorSets(ibl)");

    const std::array<VkImageView, 4> iblViews{irradianceCube.view, prefilteredCube.view, brdfLut.view,
                                              envCube.view};
    std::array<VkDescriptorImageInfo, 4> iblInfos{};
    std::array<VkWriteDescriptorSet, 4> iblWrites{};
    for (uint32_t i = 0; i < 4; ++i) {
        iblInfos[i] = {iblSampler, iblViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        iblWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        iblWrites[i].dstSet = iblSet;
        iblWrites[i].dstBinding = i;
        iblWrites[i].descriptorCount = 1;
        iblWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iblWrites[i].pImageInfo = &iblInfos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 4, iblWrites.data(), 0, nullptr);

    // What this bake used, so `rebakeIblIfSunMoved` can tell whether it is still current.
    iblBakedSun = sunDirection;
    iblBakedColor = sunColorValue;
    iblBakedIntensity = sunIntensity;

    core::Logger::status(core::LogCategory::Render, "IBL ready: env %u^2, irradiance %u^2, prefiltered %u^2 x%u mips, LUT %u^2",
                   kEnvSize, kIrradianceSize, kPrefilterSize, kPrefilterMips, kBrdfSize);
}


void Renderer::rebakeIblIfSunMoved() {
    if (iblBakedSun == sunDirection && iblBakedColor == sunColorValue && iblBakedIntensity == sunIntensity) return;

    // The images and the one descriptor set that names them, and nothing else: `iblSetLayout`
    // is created in `init` and is what every pipeline baked into its layout, so a re-bake
    // must not touch it. Rebuilding pipelines here -- which the hot-reload path does, for its
    // own reasons -- would be the expensive half of a reload for no reason.
    //
    // A device wait because the images are the ones frames in flight are sampling.
    vkDeviceWaitIdle(ctx->device);
    destroyIblResources();
    createIblResources();
}

void Renderer::destroyIblResources() {
    vkDestroySampler(ctx->device, iblSampler, nullptr);
    iblSampler = VK_NULL_HANDLE;
    // Freed, not just nulled. This ran once per process until shader hot reload
    // started re-baking the environment, at which point nulling it alone would have
    // leaked one set per reload and drained a 48-set pool -- exactly the failure 0.3b
    // was, one function over.
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &iblSet);
    iblSet = VK_NULL_HANDLE;
    destroyImage(*ctx, envCube);
    destroyImage(*ctx, irradianceCube);
    destroyImage(*ctx, prefilteredCube);
    destroyImage(*ctx, brdfLut);
}

void Renderer::destroyShadowResources() {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) return;
    // VMA asserts on any dedicated allocation still live at teardown, which is how the
    // absence of this function announced itself -- an abort in the allocator's
    // destructor, one frame after a clean run, with nothing in it naming a shadow.
    // `destroyImage` takes the view `createImage` made with it.
    for (VkImageView& layer : punctualShadowLayerViews) {
        vkDestroyImageView(ctx->device, layer, nullptr);
        layer = VK_NULL_HANDLE;
    }
    vkDestroySampler(ctx->device, shadowSampler, nullptr);
    shadowSampler = VK_NULL_HANDLE;
    destroyImage(*ctx, shadowMap);
    destroyImage(*ctx, punctualShadowMap);
}

void Renderer::createViewTargets() {
    createRenderTargets(view, primaryViewExtent());
    for (uint32_t s = 0; s < extraViews.size(); ++s) {
        ViewSlot& v = extraViews[s];
        if (!v.live) continue;
        // The table's extent, not the one it was built at: a view following the presenting
        // view has just changed size, and one that named its own has not. Both answers come
        // out of `viewExtent`, which is why the window resize needs no rule of its own.
        createViewSlot(v, viewExtent(views->at(s)));
    }
    // **And rebind them, in the same function that replaced them.** A resize destroys every
    // destination and builds new ones at the new extent, while the image descriptor array
    // still names the old views -- which the validation layer reported as `imageView ... is
    // invalid or has been destroyed` on the first draw after the first resize. `syncViews`
    // cannot cover this: it reconciles against the *table's* revision, and a resize does
    // not move it.
    bindViewDestinations();
}

void Renderer::destroyViewTargets() {
    for (ViewSlot& v : extraViews) destroyViewSlot(v);
    destroyRenderTargets(view);
}

VkExtent2D Renderer::viewExtent(const ViewTable::Entry& e) const {
    // Either component zero is the table's "follow the presenting view", and it is the
    // table that enforces that rather than this -- see `ViewTable::create`. Clamped to 1
    // for the same reason `createRenderTargets` clamps its halves: a window one pixel wide
    // is a legal thing for a window manager to hand over mid-resize, and it reaches a
    // following view through `primaryViewExtent`.
    const VkExtent2D want =
        (e.extent.x != 0 && e.extent.y != 0) ? VkExtent2D{e.extent.x, e.extent.y} : primaryViewExtent();
    return {std::max(1u, want.width), std::max(1u, want.height)};
}

void Renderer::createViewSlot(ViewSlot& v, VkExtent2D extent) {
    destroyViewSlot(v);
    // Before `createRenderTargets`, which reads it to decide whether a presentation target
    // is worth allocating. A registered view never presents.
    v.targets.primary = false;
    createRenderTargets(v.targets, extent);
    v.targets.destination = createViewDestination(extent);
    v.builtExtent = extent;
}

void Renderer::destroyViewSlot(ViewSlot& v) {
    destroyImage(*ctx, v.targets.destination);
    // A `View` whose targets were never built frees nothing: every handle in it is null
    // and `destroyRenderTargets` is written against exactly that.
    if (v.targets.gAlbedo.image != VK_NULL_HANDLE) destroyRenderTargets(v.targets);
    v.builtExtent = {};
}

GpuImage Renderer::createViewDestination(VkExtent2D extent) const {
    // `swap.format`, and it is not a free choice: `tonemapPipeline` declares its colour
    // attachment format once, and a destination in any other format is a validation error
    // at the draw rather than a wrong image. SAMPLED because the whole point is that a
    // material can read it; TRANSFER_SRC so `--capture-target` can too.
    return createImage(*ctx, extent, swap.format,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, "viewDestination");
}

void Renderer::syncViews() {
    auto zone = core::Profiler::scope("syncViews");
    if (views == nullptr || views->revision() == viewRevision) return;

    // The same one wait `syncImages` takes, for both of its reasons at once: a destination
    // about to be destroyed that an in-flight frame sampled, and the descriptor array about
    // to be rewritten in a set an in-flight command buffer bound.
    vkDeviceWaitIdle(ctx->device);

    extraViews.resize(views->slotCount());
    for (uint32_t s = 0; s < extraViews.size(); ++s) {
        const ViewTable::Entry& e = views->at(s);
        ViewSlot& v = extraViews[s];
        // Zero for a slot the table has given up. A slot destroyed and reacquired between
        // two syncs differs here too, because `destroy` moved the generation -- which is
        // what stops the old destination being left in place under a new view.
        const uint32_t want = e.live ? e.generation : 0;
        const VkExtent2D wantExtent = e.live ? viewExtent(e) : VkExtent2D{};
        // The extent is the second half of the same question, because `ViewTable::resize`
        // moves the revision without moving the generation: a live view that changed size
        // has to rebuild seventeen targets, and comparing what they were built at is what
        // says so without a dirty flag to keep in step.
        if (v.generation == want && v.builtExtent.width == wantExtent.width &&
            v.builtExtent.height == wantExtent.height) {
            continue;
        }

        destroyViewSlot(v);
        // The descriptor array must give the handle up in the same breath, or a slot the
        // table reuses for a loaded image inherits a destroyed view.
        if (v.imageSlot != 0 && v.imageSlot < overlayImages.size()) {
            overlayImages[v.imageSlot] = {};
            overlayBorrowed[v.imageSlot] = 0;
        }
        v = ViewSlot{};
        v.generation = want;
        if (!e.live) continue;

        createViewSlot(v, wantExtent);
        // Uniform block 0 belongs to the presenting view and is never handed out, so the
        // first table slot takes block 1. `ViewTable::init` is given `kMaxViews - 1`, which
        // is what makes this arithmetic safe rather than a second cap to keep in step.
        v.targets.uniformSlot = s + 1;
        v.imageSlot = views->image(ViewId{s, e.generation}).index;
        v.live = true;
        core::Logger::status(core::LogCategory::Render, "View %u: %ux%u, uniform block %u, image slot %u", s,
                             wantExtent.width, wantExtent.height, v.targets.uniformSlot, v.imageSlot);
    }

    bindViewDestinations();
    viewRevision = views->revision();
}

void Renderer::bindViewDestinations() {
    // The destination goes into the ordinary image array, at the slot `ViewTable::create`
    // adopted. That is what makes what a view drew samplable by a sprite, a UI quad or a
    // material without a second kind of texture handle -- and `syncImages` leaves these
    // slots alone precisely so this can own them.
    bool any = false;
    for (const ViewSlot& v : extraViews) {
        if (!v.live || v.imageSlot == 0 || v.imageSlot >= overlayImages.size()) continue;
        overlayImages[v.imageSlot] = v.targets.destination;
        overlayBorrowed[v.imageSlot] = 1;
        any = true;
    }
    if (any) writeImageDescriptors();
}

void Renderer::destroyRenderTargets(View& view) {
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.gbufferSet);
    view.gbufferSet = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.hdrSet);
    view.hdrSet = VK_NULL_HANDLE;

    destroyImage(*ctx, view.gAlbedo);
    destroyImage(*ctx, view.gNormal);
    destroyImage(*ctx, view.gOrm);
    destroyImage(*ctx, view.gEmissive);
    destroyImage(*ctx, view.gDepth);
    destroyImage(*ctx, view.velocityTarget);
    destroyImage(*ctx, view.gDepthResolved); // no-op at 1x, where it was never created
    destroyImage(*ctx, view.presentTarget);  // no-op at native, where the blit was elided

    // Views, then sets, then the image. The sets are freed rather than dropped: an
    // earlier version of this teardown nulled a descriptor set handle without freeing
    // it, and the pool ran dry on the sixth resize with an allocation failure a long
    // way from the leak.
    for (auto& storage : view.bloomStorageViews) {
        vkDestroyImageView(ctx->device, storage, nullptr);
        storage = VK_NULL_HANDLE;
    }
    for (uint32_t m = 0; m < kBloomMips; ++m) {
        if (view.bloomDownSets[m] != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.bloomDownSets[m]);
            view.bloomDownSets[m] = VK_NULL_HANDLE;
        }
        if (view.bloomUpSets[m] != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.bloomUpSets[m]);
            view.bloomUpSets[m] = VK_NULL_HANDLE;
        }
    }
    // The sets go back with the views they name, for the reason given just above. The loop
    // runs the full `kDepthPyramidMips` rather than `depthPyramidLevels` because a resize
    // that shrinks the pyramid lowers that count, and the sets past the new top would
    // otherwise be stranded with nothing left to free them.
    for (uint32_t m = 0; m < kDepthPyramidMips; ++m) {
        vkDestroyImageView(ctx->device, view.depthPyramidStorage[m], nullptr);
        view.depthPyramidStorage[m] = VK_NULL_HANDLE;
        if (view.depthPyramidSets[m] != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.depthPyramidSets[m]);
            view.depthPyramidSets[m] = VK_NULL_HANDLE;
        }
    }
    // **Freed here, and it was not before this row.** The set names `depthPyramid.view`,
    // which the line below destroys, and it was allocated once and never released -- a
    // single view got away with that because the pool outlives it. A second View would not:
    // it would allocate its own on first use and leak it on every teardown. The rule this
    // restores is the one the whole function is: everything a View owns, released here.
    if (view.hizSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.hizSet);
        view.hizSet = VK_NULL_HANDLE;
    }
    destroyImage(*ctx, view.depthPyramid);
    destroyImage(*ctx, view.bloomChain);

    vkDestroyImageView(ctx->device, view.ssaoRawStorage, nullptr);
    view.ssaoRawStorage = VK_NULL_HANDLE;
    vkDestroyImageView(ctx->device, view.ssaoBlurStorage, nullptr);
    view.ssaoBlurStorage = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.ssaoSet);
    view.ssaoSet = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.ssaoBlurSet);
    view.ssaoBlurSet = VK_NULL_HANDLE;
    destroyImage(*ctx, view.ssaoRaw);
    destroyImage(*ctx, view.ssaoBlurred);

    vkDestroyImageView(ctx->device, view.shadowMaskStorage, nullptr);
    view.shadowMaskStorage = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.shadowMaskSet);
    view.shadowMaskSet = VK_NULL_HANDLE;
    destroyImage(*ctx, view.shadowMask);

    if (view.lightTileSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.lightTileSet);
        view.lightTileSet = VK_NULL_HANDLE;
    }
    destroyBuffer(*ctx, view.lightTiles);
    view.lightTileGrid = {};

    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.sceneDepthSet);
    view.sceneDepthSet = VK_NULL_HANDLE;

    vkDestroyImageView(ctx->device, view.fogStorage, nullptr);
    view.fogStorage = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.fogImageSet);
    view.fogImageSet = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.fogCompositeSet);
    view.fogCompositeSet = VK_NULL_HANDLE;
    destroyImage(*ctx, view.fogTarget);

    vkDestroyImageView(ctx->device, view.ssrStorage, nullptr);
    view.ssrStorage = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.ssrImageSet);
    view.ssrImageSet = VK_NULL_HANDLE;
    vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.ssrCompositeSet);
    view.ssrCompositeSet = VK_NULL_HANDLE;
    destroyImage(*ctx, view.ssrTarget);

    for (uint32_t i = 0; i < 2; ++i) {
        vkDestroyImageView(ctx->device, view.taaHistoryStorage[i], nullptr);
        view.taaHistoryStorage[i] = VK_NULL_HANDLE;
        vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.taaSet[i]);
        view.taaSet[i] = VK_NULL_HANDLE;
        if (view.taaOutputSet[i] != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &view.taaOutputSet[i]);
        }
        view.taaOutputSet[i] = VK_NULL_HANDLE;
        destroyImage(*ctx, view.taaHistory[i]);
    }
    view.taaHistoryValid = false;

    view.forwardDepth = VK_NULL_HANDLE;
    view.forwardDepthView = VK_NULL_HANDLE;
    destroyImage(*ctx, view.hdrTarget);
}

// Set on this translation unit alone, because it is the only path here that varies with
// the configured game and a define on the target would recompile the whole library on a
// toggle. Empty when no game is configured or the game brings no shaders.
#ifndef SUBSTRATE_GAME_SHADER_SRC_DIR
#define SUBSTRATE_GAME_SHADER_SRC_DIR ""
#endif

// The two shader trees, and what each may include. The engine's -I list is its own tree
// only; the game's is its own and then the engine's, so a game shader can include
// frame.glsl while an engine shader can never reach into a game -- the same direction the
// static library enforces in C++. Both the flags and the .vert/.frag/.comp filter below
// duplicate the CMake rules, and the two have to agree or a reload compiles something the
// build would not have.
//
// Guarded by the same condition as the two functions that read it, and that is the whole
// reason the guard is here: this is namespace scope, so without it the source directories
// and the glslang path have to exist for the file to compile at all, and a release build
// -- which must not bake a developer's home directory or a build machine's glslang into a
// shipped binary -- could not turn them off. `-USUBSTRATE_GLSLANG` on Linux is the cheap
// way to prove that still holds.
#if defined(SUBSTRATE_SHADER_SRC_DIR) && defined(SUBSTRATE_GLSLANG)

namespace {

struct ShaderTree {
    const char* src;
    const char* includes;
};

// No output directory, and that absence is the point: a reload compiles to a temp file it
// then deletes and publishes the bytes to gfx::overrideShaderBinary, so this table names
// no path the build system owns and nothing here can leave an artifact in build/<cfg>/.
// The game's tree comes second so that, where both trees hold a shader of the same name,
// the game's is the one left in the override table -- which is the precedence
// readShaderBinary already gives the game's directory over the engine's.
constexpr ShaderTree kShaderTrees[] = {
    {SUBSTRATE_SHADER_SRC_DIR, "-I" SUBSTRATE_SHADER_SRC_DIR},
    {SUBSTRATE_GAME_SHADER_SRC_DIR, "-I" SUBSTRATE_GAME_SHADER_SRC_DIR " -I" SUBSTRATE_SHADER_SRC_DIR},
};

}  // namespace

#endif

bool Renderer::recompileShaders() const {
#if defined(SUBSTRATE_SHADER_SRC_DIR) && defined(SUBSTRATE_GLSLANG)
    namespace fs = std::filesystem;

    // glslang has to be given a file to write, so it is given one nothing owns and nothing
    // searches: a single path in the OS temp directory, reused by every shader in the pass
    // and unlinked as soon as its bytes have been read. The build's shader directories are
    // not named here at all, which is what makes "a reloaded shader leaves no artifact" a
    // property of the code rather than a promise about it.
    //
    // The pid is in the name because two engines reloading at the same instant would
    // otherwise read each other's module -- silently, and with a picture neither source
    // explains. Nothing ever reads this path back: it is not on either search path, so
    // even a file left behind by a crash mid-compile is unreachable rather than stale.
    std::error_code tmpEc;
    const fs::path tmpDir = fs::temp_directory_path(tmpEc);
    if (tmpEc) {
        core::Logger::error(core::LogCategory::Render, "Shader reload has nowhere to compile to: %s",
                            tmpEc.message().c_str());
        return false;
    }
    const fs::path tmp = tmpDir / ("substrate-reload-" + std::to_string(getpid()) + ".spv");

    bool ok = true;
    for (const ShaderTree& tree : kShaderTrees) {
        if (tree.src[0] == '\0') continue;

        // With an error_code rather than without: a game that brings no shaders has no
        // directory to walk, and that is an ordinary state rather than an exception.
        std::error_code dirEc;
        for (const auto& entry : fs::directory_iterator(tree.src, dirEc)) {
            const fs::path& src = entry.path();
            const std::string ext = src.extension().string();
            // .glsl fragments are included, never compiled on their own. Matches the
            // CMake rule; the two lists have to agree or a reload compiles a header.
            if (ext != ".vert" && ext != ".frag" && ext != ".comp") continue;

            const std::string cmd = std::string(SUBSTRATE_GLSLANG) + " -V --target-env vulkan1.3 " +
                                    tree.includes + " -o " + tmp.string() + " " + src.string() + " 2>&1";

            std::string output;
            int status = -1;
            if (FILE* pipe = popen(cmd.c_str(), "r"); pipe != nullptr) {
                char buffer[512];
                while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
                status = pclose(pipe);
            }

            std::error_code ignored;
            if (status != 0) {
                // glslang echoes the file name on its first line whether or not it failed,
                // so the message is the interesting part and it is already in `output`.
                //
                // Nothing is published, so the module already bound stays bound -- which is
                // what the write-aside-and-rename dance used to buy by leaving the built
                // SPIR-V on disk, now for free. glslang truncates its output before it
                // knows whether the parse succeeded, and this deletes it either way.
                core::Logger::error(core::LogCategory::Render, "Shader reload failed: %s\n%s", src.filename().string().c_str(),
                              output.c_str());
                fs::remove(tmp, ignored);
                ok = false;
                continue;
            }

            // Read, then removed, whether or not the read worked -- the next shader in the
            // pass compiles to this same path, and a leftover would only be read by the
            // compile that is about to overwrite it.
            const bool published = overrideShaderBinary(src.filename().string(), tmp);
            fs::remove(tmp, ignored);
            if (!published) {
                core::Logger::error(core::LogCategory::Render, "Shader reload compiled %s but could not read %s back",
                              src.filename().string().c_str(), tmp.string().c_str());
                ok = false;
            }
        }
    }
    return ok;
#else
    // Qualified, like every other call in this file. It reads as pedantry and is not: this
    // branch compiles only when SUBSTRATE_SHADER_HOT_RELOAD is off, which is only ever in a
    // `build_release.sh` package -- so D2's namespace pass could not see it, and it has been
    // a compile error in the portable configuration ever since. D9 found it by being the
    // first row since to run a package build.
    core::Logger::warn(core::LogCategory::Render,
                       "Shader hot reload needs the source path baked in at build time");
    return false;
#endif
}

void Renderer::pollShaderReload() {
    // Above the once-a-second gate, so the per-frame miss is in the table beside the hit.
    // The miss is what says the stat sweep is cheap; the max is what says a recompile is
    // not, and a zone that only appeared on the hit would report the second as the median.
    auto s = core::Profiler::scope("pollShaderReload");
#if defined(SUBSTRATE_SHADER_SRC_DIR) && defined(SUBSTRATE_GLSLANG)
    namespace fs = std::filesystem;

    // Once a second. The scan is a stat per file over ~30 files, which is cheap, but
    // it is cheap in the same sense that a per-frame allocation is small.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastShaderPoll < std::chrono::seconds(1)) return;
    lastShaderPoll = now;

    // INT64_MIN means "nothing seen yet", and 0 would not have done. libstdc++ puts
    // file_clock's epoch in 2174, so every real file's tick count is *negative* -- a
    // zero initialiser here left the maximum pinned at zero and the poll never fired,
    // which looks exactly like hot reload being off.
    int64_t newest = INT64_MIN;
    std::error_code ec;
    for (const ShaderTree& tree : kShaderTrees) {
        if (tree.src[0] == '\0') continue;
        for (const auto& entry : fs::directory_iterator(tree.src, ec)) {
            const auto written = fs::last_write_time(entry.path(), ec);
            if (ec) continue;
            newest = std::max(newest, written.time_since_epoch().count());
        }
    }
    if (newest == INT64_MIN) return;

    // The first poll only records the baseline. Treating startup as a change would
    // recompile the whole tree every launch, which is what the build already did.
    if (newestShaderWrite == INT64_MIN) {
        newestShaderWrite = newest;
        return;
    }
    if (newest <= newestShaderWrite) return;
    newestShaderWrite = newest;

    const auto start = std::chrono::steady_clock::now();
    if (!recompileShaders()) return;

    vkDeviceWaitIdle(ctx->device);

    // Everything, in the order init() builds it. Render targets come down as well
    // because the bloom and SSAO descriptor sets live there and were written with the
    // sampler destroyBloomPipelines() is about to free.
    destroyPipelines();
    destroyViewTargets();
    destroyBloomPipelines();
    destroySsaoPipelines();
    destroyTaaPipeline();
    // Not the acceleration structures: they are the scene's geometry, not a shader
    // artefact, and rebuilding them on every hot reload would add a second of blocking
    // work to a path whose whole point is that it takes about one.
    destroyIblResources();

    createBloomPipelines();
    createSsaoPipelines();
    // Before createRenderTargets(): that is where the TAA descriptor sets are
    // allocated, and they are allocated from the layout this creates.
    createTaaPipeline();
    createViewTargets();
    createIblResources();
    createPipelines();

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    core::Logger::status(core::LogCategory::Render, "Shaders reloaded in %.0f ms", ms);
#endif
}

VkPipelineLayout Renderer::createLayout(const char* pass, std::initializer_list<const char*> shaders,
                                        std::initializer_list<VkDescriptorSetLayout> sets,
                                        std::initializer_list<VkPushConstantRange> pushConstants,
                                        size_t constantCount) const {
    // One list, counted by the language rather than by hand.
    const std::vector<VkDescriptorSetLayout> setList(sets);
    const std::vector<VkPushConstantRange> ranges(pushConstants);

    if (shaders.size() > 0) verifyShaderBindings(pass, shaders, sets, constantCount);

    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<uint32_t>(setList.size());
    info.pSetLayouts = setList.empty() ? nullptr : setList.data();
    info.pushConstantRangeCount = static_cast<uint32_t>(ranges.size());
    info.pPushConstantRanges = ranges.empty() ? nullptr : ranges.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(ctx->device, &info, nullptr, &layout), "vkCreatePipelineLayout");
    return layout;
}

void Renderer::verifyShaderBindings([[maybe_unused]] const char* pass,
                                    [[maybe_unused]] std::initializer_list<const char*> shaders,
                                    [[maybe_unused]] std::initializer_list<VkDescriptorSetLayout> sets,
                                    [[maybe_unused]] size_t constantCount) const {
#ifdef SUBSTRATE_DEBUG
    const std::vector<VkDescriptorSetLayout> setList(sets);

    for (const char* shaderName : shaders) {
        const std::string name(shaderName);
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".vert") == 0) {
            stage = VK_SHADER_STAGE_VERTEX_BIT;
        } else if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".comp") == 0) {
            stage = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        const ReflectedModule module = reflectSpirv(readShaderBinary(name));

        for (const ReflectedBinding& b : module.bindings) {
            if (b.set >= setList.size()) {
                core::Logger::critical(core::LogCategory::Render, "%s: %s uses set %u binding %u, but its layout has %zu sets",
                                 pass, name.c_str(), b.set, b.binding, setList.size());
                continue;
            }

            const auto layoutIt = layoutBindings.find(setList[b.set]);
            // A layout nobody registered -- there are none today, since setScene()
            // registers the scene's. Skipped rather than failed, so adding a layout
            // elsewhere loses the check but does not break the build.
            if (layoutIt == layoutBindings.end()) continue;

            const auto& declared = layoutIt->second;
            const auto match = std::find_if(declared.begin(), declared.end(),
                                            [&](const VkDescriptorSetLayoutBinding& d) { return d.binding == b.binding; });
            if (match == declared.end()) {
                core::Logger::critical(core::LogCategory::Render, "%s: %s reads set %u binding %u (%s), which its layout declares no binding for",
                                 pass, name.c_str(), b.set, b.binding, b.name.empty() ? "?" : b.name.c_str());
                continue;
            }

            // MAX_ENUM is "the reflector has no opinion" -- an acceleration structure,
            // say. Not a mismatch, and treating it as one would make every future
            // descriptor type an abort until this file learned about it.
            if (b.type != VK_DESCRIPTOR_TYPE_MAX_ENUM && b.type != match->descriptorType) {
                core::Logger::critical(core::LogCategory::Render, "%s: %s declares set %u binding %u as descriptor type %d, layout says %d",
                                 pass, name.c_str(), b.set, b.binding, static_cast<int>(b.type),
                                 static_cast<int>(match->descriptorType));
            }

            if ((match->stageFlags & stage) == 0) {
                core::Logger::critical(core::LogCategory::Render, "%s: %s reads set %u binding %u, which its layout does not expose to this stage",
                                 pass, name.c_str(), b.set, b.binding);
            }

            // Only an overflow is wrong. A layout with more elements than the shader
            // indexes is headroom, and an unsized array (count 0) declares nothing to
            // compare against.
            if (b.count != 0 && b.count > match->descriptorCount) {
                core::Logger::critical(core::LogCategory::Render, "%s: %s indexes %u elements at set %u binding %u, layout provides %u",
                                 pass, name.c_str(), b.count, b.set, b.binding, match->descriptorCount);
            }
        }

        // The 2.7 half of the check. An unsupplied specialisation constant keeps the
        // default the GLSL declares, and every default in this engine is the "feature
        // on" case -- so forgetting one does not fail, it silently ignores the toggle.
        for (const uint32_t id : module.specConstantIds) {
            if (id >= constantCount) {
                core::Logger::critical(core::LogCategory::Render,
                                 "%s: %s declares constant_id %u, but the pipeline supplies only %zu constants", pass,
                                 name.c_str(), id, constantCount);
            }
        }
    }
#endif
}

uint64_t Renderer::featureKey() const {
    // Order is arbitrary; only "changed or not" is read off it. The sample count needs
    // more than one bit, so it goes in the low byte and the flags stack above it.
    uint64_t key = static_cast<uint32_t>(msaaSamples);
    uint32_t bit = 8;
    for (const bool flag : {ssaoEnabled, bloomEnabled, specularAaStrength > 0.0f, edgeMsaaEnabled, rtEnabled,
                            shadowsEnabled, punctualShadowsEnabled, rtShadowsEnabled, rtShadowMaskEnabled}) {
        key |= static_cast<uint64_t>(flag ? 1u : 0u) << bit++;
    }
    return key | (static_cast<uint64_t>(tonemapOperator) << 32);
}

uint32_t Renderer::addShaderVariant(ShaderVariant variant) {
    const auto index = static_cast<uint32_t>(variants.size());
    core::Logger::status(core::LogCategory::Render, "Shader variant %u \"%s\": %s + %s, shadow %s, forward %s",
                         index, variant.name.empty() ? "?" : variant.name.c_str(), variant.vertexShader.c_str(),
                         variant.fragmentShader.c_str(), variant.shadowFragment.c_str(),
                         variant.forwardFragment.c_str());
    variants.push_back(Variant{std::move(variant)});

    // Forces the next updateInstances() to re-read every material's variant index. A
    // game that creates a material *before* registering the variant it names would
    // otherwise have had that index clamped to 0 once and never looked at again -- an
    // ordering trap worth one assignment to remove rather than one paragraph to document.
    seenMaterialRevision = 0;
    return index;
}

GraphicsPipelineDesc Renderer::variantDesc(const ShaderVariant& v, VariantPass pass) const {
    GraphicsPipelineDesc desc;
    desc.vertexBindings = {sceneVertexBinding()};

    switch (pass) {
    case VariantPass::GBuffer:
        desc.vertexShader = v.vertexShader;
        desc.fragmentShader = v.fragmentShader;
        desc.vertexAttributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(scene::Vertex, tangent)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
        };
        desc.colorFormats = {kAlbedoFormat, kNormalFormat, kOrmFormat, kEmissiveFormat};
        desc.depthFormat = kDepthFormat;
        desc.samples = msaaSamples;
        // Sponza is correctly wound, and culling backfaces makes the G-buffer pass 2.4x
        // cheaper (1.601 -> 0.674 ms at 4x) with pixel-identical output. Per variant
        // since G5, which is what a single-sided leaf card needs -- and the default is
        // still BACK_BIT, so a scene that never registers a variant pays nothing.
        desc.cullMode = v.cullMode;
        // The contract's id space: ENABLE_GSAA at 0, and nothing else the engine owns.
        desc.constants = {specularAaStrength > 0.0f ? 1u : 0u};
        break;

    case VariantPass::Shadow:
        // Depth-only: no colour attachment, no blend state, and the fragment stage exists
        // solely to discard alpha-masked texels -- without it Sponza's foliage casts solid
        // rectangles, which is more obviously wrong than no shadows at all.
        desc.vertexShader = "shadow.vert";
        desc.fragmentShader = v.shadowFragment;
        // The same interleaved Vertex the G-buffer reads, but only the two attributes this
        // pass touches: the position it projects and the UV the alpha-mask discard needs.
        desc.vertexAttributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
                                 {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)}};
        desc.depthFormat = kShadowFormat;
        desc.samples = VK_SAMPLE_COUNT_1_BIT;
        desc.depthTest = true;
        desc.depthWrite = true;
        // Forward-Z in this map, so LESS rather than the GREATER the camera passes use.
        desc.depthCompare = VK_COMPARE_OP_LESS;
        /**
         * Neither face is culled, and that is a *parity* decision rather than a quality
         * one. Do not "optimise" it back to BACK_BIT, and note that it deliberately
         * ignores `ShaderVariant::cullMode` for the same reason.
         *
         * A ray query has no notion of facing: a triangle is hit from either side, so under
         * ray tracing every surface occludes. A raster shadow pass that culls back faces
         * disagrees with the traced path for exactly the geometry whose front faces the light
         * cannot see -- single-sided walls, open shells, anything enclosing a light.
         *
         * Rendering both faces costs no correctness for closed geometry: depth is LESS, so
         * the nearer face wins and the recorded depth is the one BACK_BIT would have left.
         * What it adds is the open and enclosing geometry culling silently dropped.
         *
         * The case that exposed it -- a point light inside the emissive sphere that
         * represents it -- is no longer what this defends against: emissive geometry is
         * masked out of shadows on both paths now (shadow.frag, and non-opaque BLAS geometry
         * in AccelStruct). This remains because *non-emissive* single-sided geometry has the
         * same asymmetry and nothing else fixes it.
         *
         * Acne is handled by the world-unit biases in shadow.glsl, which is where that job
         * belongs -- front-face culling "fixes" acne by moving every depth sample a whole
         * wall away from the surface being tested, and that detaches contact shadows.
         */
        desc.cullMode = VK_CULL_MODE_NONE;
        break;

    case VariantPass::Forward:
        // Same four sets as lighting, but with the scene in slot 1 instead of the
        // G-buffer: this pass has the surface in hand and does not read one.
        desc.vertexShader = "forward.vert";
        desc.fragmentShader = v.forwardFragment;
        desc.vertexAttributes = variantDesc(v, VariantPass::GBuffer).vertexAttributes;
        desc.colorFormats = {kHdrFormat};
        desc.depthFormat = kDepthFormat;
        // Single-sample, matching hdrTarget. This is what forces the depth resolve.
        desc.samples = VK_SAMPLE_COUNT_1_BIT;
        // No culling, and this one really is fixed rather than inherited from the
        // variant. A translucent surface is visible from both sides by definition, and
        // culling one of them turns a pane of glass into a one-way mirror.
        desc.cullMode = VK_CULL_MODE_NONE;
        // Test against the opaque depth so blended surfaces are hidden behind walls, but
        // never write: two blended surfaces must both contribute, and a depth write would
        // let whichever drew first reject the other.
        desc.depthTest = true;
        desc.depthWrite = false;
        desc.blend = v.blend;
        desc.constants = shadingConstants();
        break;
    }

    // A variant's own constants sit above the engine's reserved eight, so an id a game
    // writes cannot collide with one the engine adds later. Appended only when there are
    // any: padding an empty list to eight would put a VkSpecializationInfo on every
    // pipeline in the engine to say nothing, and the point of the reservation is the id
    // space rather than the array length.
    if (!v.constants.empty()) {
        desc.constants.resize(kVariantConstantBase, 0u);
        desc.constants.insert(desc.constants.end(), v.constants.begin(), v.constants.end());
    }
    return desc;
}

VkPipeline Renderer::variantPipeline(uint32_t variant, VariantPass pass) {
    Variant& v = variants[variant < variants.size() ? variant : 0];

    VkPipeline* cached = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    switch (pass) {
    case VariantPass::GBuffer: cached = &v.gbuffer; layout = gbufferLayout; break;
    case VariantPass::Shadow:  cached = &v.shadow;  layout = shadowLayout;  break;
    case VariantPass::Forward: cached = &v.forward; layout = forwardLayout; break;
    }
    if (*cached != VK_NULL_HANDLE) return *cached;

    const GraphicsPipelineDesc desc = variantDesc(v.desc, pass);

    // The reflection check, per variant rather than per family, and this is where it
    // earns most: a family's layout was written by hand in this file and its shaders
    // reviewed beside it, while a variant's GLSL arrives from a game and is compiled
    // against a layout its author never saw. A binding it invents, a descriptor type it
    // disagrees about, or a `constant_id` below the reserved base aborts in Debug naming
    // the shader -- rather than rendering a black surface with no explanation.
    const char* named = v.desc.name.empty() ? "variant" : v.desc.name.c_str();
    if (pass == VariantPass::Forward) {
        verifyShaderBindings(
            named, {desc.vertexShader.c_str(), desc.fragmentShader.c_str()},
            {frameSetLayout, scene->descriptorSetLayout(), tlasSetLayout, iblSetLayout, singleImageSetLayout},
            desc.constants.size());
    } else {
        verifyShaderBindings(named, {desc.vertexShader.c_str(), desc.fragmentShader.c_str()},
                             {frameSetLayout, scene->descriptorSetLayout(), overlaySetLayout},
                             desc.constants.size());
    }

    *cached = createGraphicsPipeline(*ctx, layout, desc);
    return *cached;
}

std::vector<uint32_t> Renderer::shadingConstants() const {
    /**
     * The shading id space, from features.glsl -- index is the constant_id. Shared by
     * the deferred and forward paths because they gate the same features; forward.frag
     * simply never reads SAMPLE_COUNT or ENABLE_SSAO, which costs it nothing.
     *
     * ENABLE_PUNCTUAL_SHADOWS follows updateLights(): it assigns atlas layers only
     * when both toggles are on, so a constant that said otherwise would have the
     * shader looking up layers nothing ever wrote.
     *
     * A method rather than a local in createPipelines() since G5: a variant's forward
     * pipeline is built lazily, long after that function returned, and it shares this
     * id space. `rtActive` is a member for the same reason and is decided there.
     */
    return {
        static_cast<uint32_t>(msaaSamples),
        ssaoEnabled ? 1u : 0u,
        // Id 2 is vacant -- see features.glsl. The element stays because the index *is*
        // the constant_id, so dropping it would renumber every constant below it; the
        // value is ignored, since a map entry for an id no module declares does nothing.
        0u,
        edgeMsaaEnabled ? 1u : 0u,
        // Read only by the non-traced variants -- the ray-query files shadow
        // unconditionally, because `render.rt` selecting the file is the same decision.
        shadowsEnabled ? 1u : 0u,
        // Both, because updateLights assigns atlas layers only when both are on.
        (punctualShadowsEnabled && shadowsEnabled) ? 1u : 0u,
        // The traced counterpart, read only by the ray-query variants. Supplied to every
        // pipeline sharing this id space regardless -- forward.frag never reads it, and a
        // constant nobody reads costs nothing.
        rtShadowsEnabled ? 1u : 0u,
        // `shadowMaskActive`, not the row: the pass runs only where there is a ray to
        // share and more than one sample to share it between, and the shader reading a
        // mask no pass wrote is the SSAO failure again.
        shadowMaskActive ? 1u : 0u,
    };
}

void Renderer::createPipelines() {
    // Shader module reads, SPIR-V reflection, and every graphics and compute pipeline.
    // Also the largest single thing a hot reload pays for, which is why the zone is on
    // the function rather than on the startup call site -- `pollShaderReload` reaches it
    // mid-run and wants the same number.
    auto pipelineZone = core::Profiler::scope("createPipelines");
    destroyPipelines();
    builtFeatureKey = featureKey();

    // Whether any traced path can run at all, decided once here so the ENABLE_RT
    // constant, the SSR variant selection, and the record path that writes the ambient
    // buffer cannot disagree -- a shader reading a buffer no pass filled is the failure
    // this mirrors from SSAO.
    rtActive = rtEnabled && ctx->rayQuerySupported && accel.valid();

    // Decided here, beside `rtActive`, and for the same reason: `shadingConstants()` is
    // read below and again from every lazily-built forward pipeline, and the pass that
    // fills the mask is chosen from this member at record time. At 1x there is one sample
    // per pixel, so the resolve loop never runs and there is no second ray to share --
    // running the pass there would be a full-screen trace to save nothing.
    shadowMaskActive =
        rtActive && rtShadowsEnabled && rtShadowMaskEnabled && msaaSamples != VK_SAMPLE_COUNT_1_BIT;

    // ------------------------------------------------- the three variant families
    // Layouts here, pipelines in variantPipeline(): what a shader variant may change is
    // its shaders, its cull mode and its blend, and what it may not is the layout it is
    // compiled against. Building the layout once per family and the pipeline once per
    // (family, variant) is that division expressed in code.
    //
    // Variant 0's three are built eagerly, because they are the engine's own and every
    // scene draws them on its first frame -- creating them lazily would buy nothing and
    // put a hitch on frame one. Every other variant waits until a draw needs it.
    //
    // No push constant range on the G-buffer or forward layouts. Everything that used to
    // be pushed per draw is in the instance buffer at set 0 binding 3 (0.11). The shadow
    // layout keeps one: the sun needs no index, the atlas does, and both go through the
    // one family.
    shadowLayout = createLayout("shadow", {}, {frameSetLayout, scene->descriptorSetLayout(), overlaySetLayout},
                                {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t) * 2}});
    variantPipeline(0, VariantPass::Shadow);

    gbufferLayout = createLayout("gbuffer", {}, {frameSetLayout, scene->descriptorSetLayout(), overlaySetLayout}, {});
    variantPipeline(0, VariantPass::GBuffer);

    // ------------------------------------------------------------------ decals
    // Same sample count as the G-buffer, because it draws into it, and alpha-blended so
    // a decal composites over the albedo already there rather than replacing it.

    GraphicsPipelineDesc decalDesc;
    decalDesc.vertexShader = "fullscreen.vert";
    decalDesc.fragmentShader = "decal.frag";
    decalDesc.colorFormats = {kAlbedoFormat};
    decalDesc.samples = msaaSamples;
    decalDesc.cullMode = VK_CULL_MODE_NONE;
    decalDesc.depthTest = false;
    decalDesc.depthWrite = false;
    decalDesc.blend = GraphicsPipelineDesc::Blend::AlphaOver;
    decalLayout = createLayout("decal", {decalDesc.vertexShader.c_str(), decalDesc.fragmentShader.c_str()},
                               {frameSetLayout, scene->descriptorSetLayout(), singleImageSetLayout},
                               {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DecalPush)}}, 0);
    decalPipeline = createGraphicsPipeline(*ctx, decalLayout, decalDesc);

    // -------------------------------------------------------------- lighting pass

    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = "fullscreen.vert";
    // A 1x G-buffer is genuinely single-sampled, and single-sample images cannot bind
    // to sampler2DMS descriptors — the 1x baseline needs its own shader variant.
    const bool single = msaaSamples == VK_SAMPLE_COUNT_1_BIT;
    // Four variants, from two independent binary facts. The sample count picks between
    // the sampler2DMS and sampler2D pair; `rtActive` picks whether the acceleration
    // structure at set 2 binding 2 is declared at all. Neither can be a specialisation
    // constant -- one changes a descriptor's *type*, the other its existence.
    //
    // Ray tracing is one switch. Where it is on, this file traces both its shadows and
    // the reflection pass beside it; where it is off, the cascade path below runs and the
    // SSR march samples an image those cascades already shadowed. There is deliberately no
    // state where one of the two is traced and the other is not -- that is the
    // reflected-shadows-do-not-match-the-world bug, expressed as a configuration.
    if (rtActive) {
        lightingDesc.fragmentShader = single ? "lighting1x_rt.frag" : "lighting_rt.frag";
    } else {
        lightingDesc.fragmentShader = single ? "lighting1x.frag" : "lighting.frag";
    }
    lightingDesc.colorFormats = {kHdrFormat};
    lightingDesc.samples = VK_SAMPLE_COUNT_1_BIT; // resolve target is single-sample
    lightingDesc.cullMode = VK_CULL_MODE_NONE;
    lightingDesc.depthTest = false;
    lightingDesc.depthWrite = false;
    lightingDesc.constants = shadingConstants();
    lightingLayout = createLayout("lighting",
                                  {lightingDesc.vertexShader.c_str(), lightingDesc.fragmentShader.c_str()},
                                  {frameSetLayout, gbufferSetLayout, tlasSetLayout, iblSetLayout,
                                   storageImageSetLayout, lightTileSetLayout},
                                  {}, lightingDesc.constants.size());
    lightingPipeline = createGraphicsPipeline(*ctx, lightingLayout, lightingDesc);

    // ------------------------------------------------- light tile build (C35)
    // Beside the lighting pass rather than with the other compute passes, because it takes
    // the same multisampled/1x split for the same reason: it reads gDepth through
    // `gbufferSetLayout`, and a 1x G-buffer cannot bind to a sampler2DMS descriptor. The
    // sample count itself is a push constant, not a specialisation one -- see
    // createComputePipeline.
    const char* tileShader = single ? "light_tile1x.comp" : "light_tile.comp";
    lightTileLayout = createLayout("light tiles", {tileShader},
                                      {frameSetLayout, gbufferSetLayout, lightTileSetLayout},
                                      {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightTilePush)}});
    lightTilePipeline = createComputePipeline(*ctx, lightTileLayout, tileShader);

    // --------------------------------------------------------- shadow mask pass
    // Built only where it will run. The mask is read through a specialisation constant
    // the same value decided, so a pipeline that exists without the constant set, or the
    // reverse, is not a state this can reach.
    //
    // **No colour attachment.** The output is the storage image at set 3, so the pass
    // rasterises a fullscreen triangle purely for the fragment invocations and the
    // interpolated `vUV` -- which is the whole reason it is a draw and not a dispatch;
    // shadowmask.frag says why.
    if (shadowMaskActive) {
        GraphicsPipelineDesc maskDesc;
        maskDesc.vertexShader = "fullscreen.vert";
        maskDesc.fragmentShader = "shadowmask.frag";
        maskDesc.samples = VK_SAMPLE_COUNT_1_BIT;
        maskDesc.cullMode = VK_CULL_MODE_NONE;
        maskDesc.depthTest = false;
        maskDesc.depthWrite = false;
        maskDesc.constants = lightingDesc.constants;
        shadowMaskLayout = createLayout("shadow mask", {"fullscreen.vert", "shadowmask.frag"},
                                        {frameSetLayout, gbufferSetLayout, tlasSetLayout, storageImageSetLayout,
                                         lightTileSetLayout},
                                        {}, maskDesc.constants.size());
        shadowMaskPipeline = createGraphicsPipeline(*ctx, shadowMaskLayout, maskDesc);
    }

    // ------------------------------------------------------------------- ssr pass
    // Two pipelines: a compute march that writes reflected radiance, and a fullscreen
    // additive draw that adds it back. Built here rather than beside the bloom and TAA
    // pipelines because the march reads the G-buffer, so it takes the same
    // multisampled/1x shader split the lighting pass does and has to be rebuilt when
    // the sample count changes.
    // Three extra sets in the ray-traced variant: the TLAS at set 3, the IBL layout at
    // set 4 and the bindless scene set at set 5. The layout differs, so the pipeline
    // does, so the *file* does -- the same reason the lighting pass has four variants
    // rather than one with a constant.
    //
    // Sets 4 and 5 are what shading a hit costs. The environment cube alone would have
    // done for the old reprojecting path; evaluating a BRDF at the hit needs the whole
    // split-sum chain and the material array the G-buffer pass already binds.
    const char* ssrShader = single ? "ssr1x.comp" : "ssr.comp";
    if (rtActive) ssrShader = single ? "ssr1x_rt.comp" : "ssr_rt.comp";

    // The sharpest case D4 had: a six-entry array, a `setLayoutCount = rtActive ? 6 : 3`
    // beside it, and the same two lists spelled out again for verification -- four
    // statements of one fact, and the count was the one nothing checked. One list per
    // branch now, and the branch is the thing that actually differs.
    constexpr VkPushConstantRange ssrRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SsrPush)};
    if (rtActive) {
        ssrLayout = createLayout("ssr", {ssrShader},
                                 {frameSetLayout, gbufferSetLayout, computeImageSetLayout, tlasSetLayout,
                                  iblSetLayout, scene->descriptorSetLayout()},
                                 {ssrRange});
    } else {
        ssrLayout = createLayout("ssr", {ssrShader}, {frameSetLayout, gbufferSetLayout, computeImageSetLayout},
                                 {ssrRange});
    }
    ssrPipeline = createComputePipeline(*ctx, ssrLayout, ssrShader);


    GraphicsPipelineDesc ssrCompositeDesc;
    ssrCompositeDesc.vertexShader = "fullscreen.vert";
    ssrCompositeDesc.fragmentShader = "composite.frag";
    ssrCompositeDesc.colorFormats = {kHdrFormat};
    ssrCompositeDesc.samples = VK_SAMPLE_COUNT_1_BIT;
    ssrCompositeDesc.cullMode = VK_CULL_MODE_NONE;
    ssrCompositeDesc.depthTest = false;
    ssrCompositeDesc.depthWrite = false;
    ssrCompositeDesc.blend = GraphicsPipelineDesc::Blend::Additive;
    ssrCompositeLayout = createLayout("composite", {"fullscreen.vert", "composite.frag"}, {singleImageSetLayout});
    ssrCompositePipeline = createGraphicsPipeline(*ctx, ssrCompositeLayout, ssrCompositeDesc);

    // Built only where a reduced ssrTarget will actually be composited, and the test is
    // `builtSsrScale` rather than `ssrScale` because this runs on its own whenever
    // `pipelinesDirty` fires -- a scene change, a shader reload -- on a frame where the
    // image has not been rebuilt. `builtSsrScale` is what the live image is.
    if (builtSsrScale < 1.0f) {
        const char* upsampleShader = single ? "ssr_upsample1x.frag" : "ssr_upsample.frag";
        GraphicsPipelineDesc upsampleDesc = ssrCompositeDesc;
        upsampleDesc.fragmentShader = upsampleShader;
        ssrUpsampleLayout =
            createLayout("ssr upsample", {"fullscreen.vert", upsampleShader}, {gbufferSetLayout, singleImageSetLayout});
        ssrUpsamplePipeline = createGraphicsPipeline(*ctx, ssrUpsampleLayout, upsampleDesc);
    }

    // ------------------------------------------------------------------- fog pass
    fogLayout = createLayout("fog", {"fog.comp"}, {frameSetLayout, computeImageSetLayout},
                             {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FogPush)}});
    fogPipeline = createComputePipeline(*ctx, fogLayout, "fog.comp");

    // Same layout and same shader as the SSR composite; only the blend differs, and
    // that difference is the whole reason fog is not simply additive.
    GraphicsPipelineDesc fogCompositeDesc = ssrCompositeDesc;
    fogCompositeDesc.blend = GraphicsPipelineDesc::Blend::PremultipliedOver;
    fogCompositePipeline = createGraphicsPipeline(*ctx, ssrCompositeLayout, fogCompositeDesc);

    // -------------------------------------------------------------- velocity pass
    // One set, and it is the frame set: everything this pass reads -- the two camera
    // matrices, the instance transforms, last frame's transforms -- is already there.
    // No scene set, because it never touches a material; no shadow or IBL set, because
    // it computes no radiance at all. It is the smallest pipeline layout in the engine
    // and that is a property of the pass rather than an economy.

    GraphicsPipelineDesc velocityDesc;
    velocityDesc.vertexShader = "velocity.vert";
    velocityDesc.fragmentShader = "velocity.frag";
    // The G-buffer's *binding* -- same buffer, same stride, so one vertex buffer still
    // binds for every geometry pass -- but only the attribute this shader actually reads.
    //
    // Describing attributes the shader ignores is legal, and it is not free of meaning:
    // it hands the driver inputs nothing consumes and trusts it to discard them, which is
    // a behaviour to rely on rather than a decision to make. The validation layer says so
    // outright, three times per pipeline build. Naming position alone states what this
    // pass needs, and the stride in the binding is what keeps the layouts in step.
    velocityDesc.vertexBindings = {sceneVertexBinding()};
    velocityDesc.vertexAttributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)}};
    velocityDesc.colorFormats = {VK_FORMAT_R16G16_SFLOAT};
    velocityDesc.depthFormat = kDepthFormat;
    // Single-sample, which is what forces it to be its own pass and to test against the
    // resolved depth -- the same constraint and the same depth the forward pass took.
    velocityDesc.samples = VK_SAMPLE_COUNT_1_BIT;
    // The G-buffer's cull mode, spelled out rather than read off variant 0's: this pass
    // corrects the motion of surfaces the G-buffer kept, so culling the same faces is
    // what keeps the two agreeing. A variant drawing both faces is not followed here --
    // the extra fragments it writes are behind ones this already covered, and the depth
    // test rejects them.
    velocityDesc.cullMode = VK_CULL_MODE_BACK_BIT;
    // Test, never write. The depth is the G-buffer's, already resolved; a dynamic
    // fragment that is behind the surface the G-buffer kept must not write a correction
    // for a pixel it does not own.
    velocityDesc.depthTest = true;
    velocityDesc.depthWrite = false;
    velocityLayout = createLayout("velocity", {"velocity.vert", "velocity.frag"}, {frameSetLayout});
    velocityPipeline = createGraphicsPipeline(*ctx, velocityLayout, velocityDesc);

    // --------------------------------------------------------------- forward pass
    // Same four sets as lighting, but with the scene in slot 1 instead of the
    // G-buffer: this pass has the surface in hand and does not read one.
    //
    // Set 4 is the opaque depth this pass is already testing against, bound so a fragment
    // can also *read* how far away whatever is behind it is -- an intersection highlight, a
    // soft particle, a water line. Legal against the same image the depth attachment names
    // because that attachment is read-only here: DEPTH_READ_ONLY_OPTIMAL and STORE_OP_NONE
    // are what make the feedback loop allowed rather than undefined. The engine's own
    // `forward.frag` declares nothing at this set, so it costs a bound descriptor and no
    // sample.

    forwardLayout = createLayout(
        "forward", {},
        {frameSetLayout, scene->descriptorSetLayout(), tlasSetLayout, iblSetLayout, singleImageSetLayout}, {});
    variantPipeline(0, VariantPass::Forward);

    // --------------------------------------------------------------- tonemap pass

    GraphicsPipelineDesc tonemapDesc;
    tonemapDesc.vertexShader = "fullscreen.vert";
    tonemapDesc.fragmentShader = "tonemap.frag";
    tonemapDesc.colorFormats = {swap.format};
    tonemapDesc.cullMode = VK_CULL_MODE_NONE;
    tonemapDesc.depthTest = false;
    tonemapDesc.depthWrite = false;
    // tonemap.frag's own id space: the operator selector, then the bloom composite.
    tonemapDesc.constants = {static_cast<uint32_t>(tonemapOperator), bloomEnabled ? 1u : 0u};
    tonemapLayout = createLayout("tonemap", {tonemapDesc.vertexShader.c_str(), tonemapDesc.fragmentShader.c_str()},
                                 {frameSetLayout, hdrSetLayout}, {}, tonemapDesc.constants.size());
    tonemapPipeline = createGraphicsPipeline(*ctx, tonemapLayout, tonemapDesc);

    // --------------------------------------------------------------- overlay pass

    GraphicsPipelineDesc overlayDesc;
    overlayDesc.vertexShader = "overlay.vert";
    overlayDesc.fragmentShader = "overlay.frag";
    overlayDesc.vertexBindings = {{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    overlayDesc.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(OverlayVertex, rgba)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(OverlayVertex, texture)},
    };
    overlayDesc.colorFormats = {swap.format};
    overlayDesc.cullMode = VK_CULL_MODE_NONE;
    overlayDesc.depthTest = false;
    overlayDesc.depthWrite = false;
    overlayDesc.blend = GraphicsPipelineDesc::Blend::AlphaOver;
    overlayLayout = createLayout("overlay", {overlayDesc.vertexShader.c_str(), overlayDesc.fragmentShader.c_str()},
                                 {overlaySetLayout},
                                 {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(OverlayPush)}}, 0);
    overlayPipeline = createGraphicsPipeline(*ctx, overlayLayout, overlayDesc);

    // ----------------------------------------------------- debug lines (S4.5)
    // No descriptor set at all: a line needs a view-projection and its own colour, and
    // taking the frame set to get the first would mean declaring the light buffer and
    // the shadow matrices in a shader that reads neither.

    GraphicsPipelineDesc debugLineDesc;
    debugLineDesc.vertexShader = "debug_line.vert";
    debugLineDesc.fragmentShader = "debug_line.frag";
    debugLineDesc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    debugLineDesc.vertexBindings = {{0, sizeof(DebugLineVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    debugLineDesc.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugLineVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(DebugLineVertex, color)},
    };
    debugLineDesc.colorFormats = {swap.format};
    debugLineDesc.cullMode = VK_CULL_MODE_NONE;
    // No depth attachment and no test -- see the note on `Renderer::debugLines`. A
    // collider is inside the mesh it describes, so a depth-tested wireframe of one is
    // hidden by exactly the geometry it is being compared against.
    debugLineDesc.depthTest = false;
    debugLineDesc.depthWrite = false;
    debugLineDesc.blend = GraphicsPipelineDesc::Blend::AlphaOver;
    debugLineLayout = createLayout("debug_line",
                                   {debugLineDesc.vertexShader.c_str(), debugLineDesc.fragmentShader.c_str()}, {},
                                   {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)}});
    debugLinePipeline = createGraphicsPipeline(*ctx, debugLineLayout, debugLineDesc);

    // ------------------------------------------------------------------ sprites (P4)
    // Unconditional, beside the two above and for the same reason: one pipeline over two
    // small shaders, against a lazily-created pipeline in the hot-reload path.

    GraphicsPipelineDesc spriteDesc;
    spriteDesc.vertexShader = "sprite.vert";
    spriteDesc.fragmentShader = "sprite.frag";
    // No vertex bindings at all: the six corners come from `gl_VertexIndex` and everything
    // else from the storage buffer, which is `particle.vert`'s shape and the argument for
    // this one.
    spriteDesc.colorFormats = {swap.format};
    spriteDesc.cullMode = VK_CULL_MODE_NONE; // a flipped sprite winds the other way
    // No depth attachment and no test: the CPU sort is the order. See `recordSprites`.
    spriteDesc.depthTest = false;
    spriteDesc.depthWrite = false;
    // Premultiplied over, matching what `sprite.frag` writes, and additive for free where a
    // sprite's alpha is zero. One blend state, therefore one draw, therefore one sort.
    spriteDesc.blend = GraphicsPipelineDesc::Blend::PremultipliedOver;
    spriteLayout = createLayout("sprite", {spriteDesc.vertexShader.c_str(), spriteDesc.fragmentShader.c_str()},
                                {overlaySetLayout, spriteSetLayout},
                                {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)}}, 0);
    spritePipeline = createGraphicsPipeline(*ctx, spriteLayout, spriteDesc);
    setObjectName(*ctx, reinterpret_cast<uint64_t>(spritePipeline), VK_OBJECT_TYPE_PIPELINE, "sprite");

    // -------------------------------------------------------------- particles (S3)
    // Conditional on the scene having emitters rather than on a config flag, which is
    // the same rule the skinning pass follows: five pipelines nobody will bind are five
    // pipelines not worth compiling. Sponza builds none of them.
    if (particleCapacity > 0) createParticlePipelines();
}

void Renderer::destroyPipelines() {
    destroyParticlePipelines();

    // Every variant's three, the engine's own included -- variant 0 is not a special
    // case here and nulling the handles is the whole of a variant's teardown. That is
    // what makes a feature toggle, a resize and a shader hot reload rebuild a game's
    // shaders on exactly the same path as the engine's: they are forgotten, and the next
    // draw that wants one builds it against whatever the constants now say.
    for (Variant& v : variants) {
        for (VkPipeline* pipeline : {&v.gbuffer, &v.shadow, &v.forward}) {
            vkDestroyPipeline(ctx->device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }

    // Here rather than beside the shadow *image*, because this is a pipeline layout and
    // createPipelines() begins by calling this. Freed with the image instead, it survived
    // every rebuild that a feature toggle or a resize triggers and only the last one ever
    // got destroyed -- validation caught it as a VkPipeline and a VkPipelineLayout
    // outliving the device.
    vkDestroyPipelineLayout(ctx->device, shadowLayout, nullptr);
    shadowLayout = VK_NULL_HANDLE;

    vkDestroyPipeline(ctx->device, decalPipeline, nullptr);
    decalPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, decalLayout, nullptr);
    decalLayout = VK_NULL_HANDLE;

    vkDestroyPipeline(ctx->device, fogPipeline, nullptr);
    fogPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, fogLayout, nullptr);
    fogLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, fogCompositePipeline, nullptr);
    fogCompositePipeline = VK_NULL_HANDLE;

    vkDestroyPipeline(ctx->device, ssrPipeline, nullptr);
    ssrPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, ssrLayout, nullptr);
    ssrLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, ssrCompositePipeline, nullptr);
    ssrCompositePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, ssrCompositeLayout, nullptr);
    ssrCompositeLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, ssrUpsamplePipeline, nullptr);
    ssrUpsamplePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, ssrUpsampleLayout, nullptr);
    ssrUpsampleLayout = VK_NULL_HANDLE;

    vkDestroyPipeline(ctx->device, lightingPipeline, nullptr);
    lightingPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, shadowMaskPipeline, nullptr);
    shadowMaskPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, shadowMaskLayout, nullptr);
    shadowMaskLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, lightTilePipeline, nullptr);
    lightTilePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, lightTileLayout, nullptr);
    lightTileLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, tonemapPipeline, nullptr);
    tonemapPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, overlayPipeline, nullptr);
    overlayPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, debugLinePipeline, nullptr);
    debugLinePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, debugLineLayout, nullptr);
    debugLineLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, spritePipeline, nullptr);
    spritePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, spriteLayout, nullptr);
    spriteLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, gbufferLayout, nullptr);
    gbufferLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, lightingLayout, nullptr);
    lightingLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, tonemapLayout, nullptr);
    tonemapLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, overlayLayout, nullptr);
    overlayLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, forwardLayout, nullptr);
    forwardLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx->device, velocityPipeline, nullptr);
    velocityPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, velocityLayout, nullptr);
    velocityLayout = VK_NULL_HANDLE;

}

void Renderer::setSampleCount(uint32_t samples) {
    const VkSampleCountFlagBits clamped = ctx->clampSampleCount(samples);
    if (clamped == msaaSamples) return;

    msaaSamples = clamped;
    // The one change that resizes images as well as recompiling shaders.
    renderTargetsDirty = true;
    core::Logger::status(core::LogCategory::Render, "MSAA -> %ux", static_cast<uint32_t>(msaaSamples));
}

void Renderer::shutdown() {
    destroyParticleResources();
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(ctx->device);

    gpuProfiler.shutdown(*ctx);
    destroyPipelines();
    destroyViewTargets();
    destroyIblResources();
    destroyBloomPipelines();
    destroySsaoPipelines();
    destroyTaaPipeline();
    // Before destroyFrameResources: the per-frame cull sets are freed there and their
    // layout is destroyed here, and freeing a set whose layout is gone is undefined.
    destroyFrameResources();
    destroyCullPipeline();
    destroyDepthPyramidPipeline();
    destroySkinResources();
    destroySkinPipeline();
    destroySceneAccelStruct(*ctx, accel);
    debugFont.shutdown(*ctx);

    vkDestroyDescriptorPool(ctx->device, descriptorPool, nullptr);
    descriptorPool = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, frameSetLayout, nullptr);
    frameSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, gbufferSetLayout, nullptr);
    gbufferSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, hdrSetLayout, nullptr);
    hdrSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, singleImageSetLayout, nullptr);
    singleImageSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, storageImageSetLayout, nullptr);
    storageImageSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, lightTileSetLayout, nullptr);
    lightTileSetLayout = VK_NULL_HANDLE;
    // The pool before the layout, and both before the images: destroying the pool frees
    // the set out of it, and a set outliving the layout it was allocated from is
    // undefined -- the same ordering `destroyFrameResources` is called early for.
    vkDestroyDescriptorPool(ctx->device, overlayImagePool, nullptr);
    overlayImagePool = VK_NULL_HANDLE;
    overlaySet = VK_NULL_HANDLE;
    imageCapacity = 0;
    vkDestroyDescriptorSetLayout(ctx->device, overlaySetLayout, nullptr);
    overlaySetLayout = VK_NULL_HANDLE;
    // Borrowed slots hold a copy of a handle `syncViews` owns and `destroyViewTargets`
    // frees. Freeing it here as well is a double free of an image that is still named by a
    // live view.
    for (uint32_t i = 0; i < overlayImages.size(); ++i) {
        if (i < overlayBorrowed.size() && overlayBorrowed[i] != 0) continue;
        destroyImage(*ctx, overlayImages[i]);
    }
    overlayImages.clear();
    overlayResident.clear();
    overlayBorrowed.clear();
    images = nullptr;
    imageRevision = 0;
    vkDestroyDescriptorSetLayout(ctx->device, tlasSetLayout, nullptr);
    tlasSetLayout = VK_NULL_HANDLE;
    destroyShadowResources();
    vkDestroyDescriptorSetLayout(ctx->device, iblSetLayout, nullptr);
    iblSetLayout = VK_NULL_HANDLE;
    // The one handle in this function that was destroyed and not cleared, which is the
    // defect the removed `!= VK_NULL_HANDLE` guards were hiding: 82 of them read as
    // careful code and none of them checked the thing that actually goes wrong.
    vkDestroyDescriptorSetLayout(ctx->device, computeImageSetLayout, nullptr);
    computeImageSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, particleSetLayout, nullptr);
    particleSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, spriteSetLayout, nullptr);
    spriteSetLayout = VK_NULL_HANDLE;
    vkDestroySampler(ctx->device, pointSampler, nullptr);
    pointSampler = VK_NULL_HANDLE;
    vkDestroySampler(ctx->device, fontSampler, nullptr);
    fontSampler = VK_NULL_HANDLE;

    // Freed with the pool above rather than individually, like every other set the
    // renderer holds for its whole life; only the handle needs clearing.
    tlasSet = VK_NULL_HANDLE;

    swap.destroy(*ctx);
}

void Renderer::updateSunShadow(FrameUniforms& u) {
    // There is no camera argument, and that is the entire point of this function. The
    // cascades this replaced fitted their boxes to the view frustum, so the projection a
    // surface was shadowed by depended on where the viewer stood; crossing a split
    // changed the world size of a texel, the world distance the depth bias pushed an
    // occluder, and the width the filter spanned, and the shadow slid across the wall as
    // you walked towards it. Fitting to the scene instead makes that unrepresentable.

    glm::vec3 boundsMin(-20.0f);
    glm::vec3 boundsMax(20.0f);
    if (scene != nullptr) {
        boundsMin = scene->boundsMin;
        boundsMax = scene->boundsMax;
    }

    const glm::vec3 centre = (boundsMin + boundsMax) * 0.5f;

    // The sphere through the bounds' corners, not the box: a sphere is invariant under
    // rotation, so swinging the sun cannot resize the box. A box fit would change the
    // texel size with the sun's heading, which is the same crawl the cascades had, just
    // driven by the light instead of the camera.
    float radius = glm::length(boundsMax - boundsMin) * 0.5f;

    // A cap, not a reach. Past this the world is simply outside the map and reads as
    // lit -- worth it only for a scene far larger than the part anyone looks at.
    if (shadowDistance > 0.0f) radius = std::min(radius, shadowDistance * 0.5f);

    const glm::vec3 lightDir = glm::normalize(sunDirection); // points *toward* the light
    const glm::vec3 eye = centre + lightDir * radius;
    const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

    // Forward-Z, unlike the main camera: orthographic depth is spread evenly, so
    // reverse-Z buys nothing and a plain LESS compare is what the sampler wants.
    const float depthRange = radius * 2.0f;
    const glm::mat4 lightView = glm::lookAt(eye, centre, up);
    const glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, depthRange);

    u.sunViewProj = lightProj * lightView;

    // View 1 is the sun's box. Fitted to the scene it rejects little, but the dispatch
    // runs per view either way and a capped `shadowDistance` gives it something to do.
    cullViewProj[1] = u.sunViewProj;

    // Both biases arrive in metres and leave in the units the shader compares in. The
    // box spans `depthRange` in all three axes, so a metre is the same fraction of NDC
    // depth as it is of a texel's width -- one conversion, one place to be wrong.
    const float texelWorld = depthRange / static_cast<float>(kShadowMapSize);
    u.shadowParams = glm::vec4(1.0f / static_cast<float>(kShadowMapSize), shadowDepthBias / depthRange,
                               shadowNormalBias, texelWorld);
}

void Renderer::updateLights(uint32_t slot, const glm::vec3& viewPosition, const glm::mat4& viewProj) {
    // The one zone on this list that is not a spike. It is a CPU cull and two sorts over
    // the light list, so its cost is a function of light count -- invisible on Sponza, a
    // frame budget on a level with a thousand lights, and read by median rather than max.
    auto s = core::Profiler::scope("updateLights");
    lightScratch.clear();
    lightSourceScratch.clear();

    // **Every view runs the whole of this** (C38); only the shadow *assignment* below is
    // the primary's. The cull, the budget and the ranking are all functions of the matrix
    // and position they were handed, so a view looking somewhere else shades the lights it
    // can see rather than the ones the presenting camera could.
    const bool primary = view.primary;

    // The sun goes in first and always, as its own member rather than as lights[0].
    lightScratch.push_back(makeDirectionalLight(sunDirection, sunColorValue, sunIntensity));
    lightSourceScratch.push_back(kNoLightSource);

    // ------------------------------------------------------ the shading budget (0.9)
    // Clamped to what the buffer was actually allocated for at init. They agree unless
    // someone raised the budget after init, and that case reports rather than writing
    // past the mapping.
    // **The buffer's own size, and there is no second number to disagree with it** (C40).
    // `lightBudget` is a floor read once at init; what binds here is what the buffer was
    // actually allocated for, and `growLightBuffer` raises that at the top of the next frame
    // when the block below finds it short.
    const uint32_t budget = lightBufferCapacity;

    // ------------------------------------------------------- volume culling (C8)
    // Before the budget, and that ordering is the whole row: the budget used to rank
    // every light in the *scene*, so a lamp behind the camera cost a slot a lamp in front
    // of it wanted. Culling first turns it into a cap on lights that can affect this view.
    //
    // `lightVisible` is conservative and exact -- it rejects only lights whose own volume
    // cannot reach any visible surface -- so this changes which lights are *considered*
    // without changing which are *shaded* in any scene that fits the budget. That is why
    // the golden set stays byte-identical across C8.
    const Frustum frustum = extractFrustum(viewProj);
    lightVisibleScratch.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(lights.size()); ++i) {
        if (lightVisible(lights[i], frustum)) lightVisibleScratch.push_back(i);
    }
    // The presenting view's number, and only its: this is what the overlay reports, and a
    // mirror's cull is not the frame's.
    if (primary) culledLights = static_cast<uint32_t>(lights.size() - lightVisibleScratch.size());

    // The sun has already taken a slot, so this is what is left for the scene's lights.
    const uint32_t room = budget > 0 ? budget - 1 : 0;
    const uint32_t sceneLights = static_cast<uint32_t>(lightVisibleScratch.size());
    const uint32_t dropped = sceneLights > room ? sceneLights - room : 0;

    if (dropped == 0) {
        // Everything fits, so there is nothing to rank. Emitting in scene order rather
        // than importance order is not laziness: the shader accumulates radiance in
        // buffer order, floating-point addition is not associative, and reordering a
        // set nobody is dropping from would move pixels to no purpose -- which 5.3's
        // golden images would report as a regression, correctly.
        // Scene order, which for the visible set is ascending index -- see above for why
        // the order the shader sums in has to be stable.
        for (uint32_t i : lightVisibleScratch) {
            lightScratch.push_back(lights[i]);
            lightSourceScratch.push_back(i);
        }
    } else {
        // Rank, take the top `room`, then put the survivors back into scene order for
        // the reason above: importance decides *which* lights, never in what order they
        // are summed.
        lightRankScratch = lightVisibleScratch;

        // stable_sort, so equal-importance lights keep scene order and the frame stays
        // bit-identical run to run. std::sort would pick between them by whatever the
        // introsort pivot happened to do.
        std::stable_sort(lightRankScratch.begin(), lightRankScratch.end(), [&](uint32_t a, uint32_t b) {
            return lightImportance(lights[a], viewPosition) > lightImportance(lights[b], viewPosition);
        });
        lightRankScratch.resize(room);
        std::sort(lightRankScratch.begin(), lightRankScratch.end());

        for (uint32_t i : lightRankScratch) {
            lightScratch.push_back(lights[i]);
            lightSourceScratch.push_back(i);
        }
    }

    // **What this view actually wanted, which is what grows the buffer** (C40). Taken across
    // every view rather than the primary's alone -- a mirror seeing more lights than the
    // camera is still a view that needs the room -- and acted on at the top of the next
    // frame, because the buffer cannot be reallocated from inside a frame that is recording
    // against it. So a light is dropped for one frame and shaded from then on.
    if (dropped > 0) lightsWanted = std::max(lightsWanted, sceneLights + 1);

    // Gated for the reason it is rate-limited at all: four views over budget would say the
    // same thing four times a frame, each disagreeing with the last about the count.
    if (primary && dropped != reportedLightDrops) {
        if (dropped > 0) {
            core::Logger::status(core::LogCategory::Render,
                                 "Light budget: shading %u of %u lights in view plus the sun this frame; %u held "
                                 "back by lowest luminance over squared distance (%u more were outside the view "
                                 "volume and cost nothing). The buffer grows before the next frame.",
                                 room, sceneLights, dropped, culledLights);
        } else {
            core::Logger::status(core::LogCategory::Render, "Light budget: all %u lights in view fit again", sceneLights);
        }
        reportedLightDrops = dropped;
    }

    // ------------------------------------------------------- the shadow atlas
    // A spot needs one projection, a point six. Lights that do not fit keep params.w at
    // -1 and simply do not occlude: the alternative, dropping the light, changes the
    // image far more than losing its shadow does.
    //
    // Assignment is in light order rather than by importance. Ranking with hysteresis is
    // what a scene that *overflows* needs -- otherwise the set of shadowed lights flips
    // as the camera crosses the plane where two of them tie -- and it belongs here, on
    // this loop, when a scene first needs it. Until then the overflow is reported and
    // the order is the scene's.
    //
    // **The primary assigns; every other view looks its lights up** (C38). The atlas holds
    // one assignment and `recordPunctualShadows` has already rendered it by the time a
    // secondary chain reaches here, so a second assignment would put matrices in this
    // view's buffer describing layers the atlas does not hold. A light this view ranked and
    // the primary did not therefore illuminates without occluding -- exactly the
    // degradation an atlas overflow already produces, and the same one this comment's
    // first paragraph accepts for a light that did not fit.
    if (primary) {
        shadowMatrixScratch.clear();
        lightShadowLayer.assign(lights.size(), -1.0f);
    }

    if (!primary) {
        // `shadowMatrixScratch` is deliberately left alone: it still holds the primary's
        // matrices, and those are what this view's block has to carry, because `params.w`
        // indexes atlas layers and the atlas is the frame's rather than the view's.
        if (punctualShadowsEnabled && shadowsEnabled) {
            for (size_t k = 0; k < lightScratch.size(); ++k) {
                GpuLight& light = lightScratch[k];
                const auto type = static_cast<LightType>(static_cast<uint32_t>(light.params.z));
                if (type == LightType::Directional) continue;
                const uint32_t src = lightSourceScratch[k];
                light.params.w = src < lightShadowLayer.size() ? lightShadowLayer[src] : -1.0f;
            }
        } else {
            for (GpuLight& light : lightScratch) light.params.w = -1.0f;
        }
    } else if (punctualShadowsEnabled && shadowsEnabled) {
        uint32_t unshadowed = 0;
        for (size_t k = 0; k < lightScratch.size(); ++k) {
            GpuLight& light = lightScratch[k];
            const auto type = static_cast<LightType>(static_cast<uint32_t>(light.params.z));
            if (type == LightType::Directional) continue;
            // Recorded against the *scene* index rather than this list's position: another
            // view's ranking produces a different list, and the layer has to be findable
            // from the light itself. Written wherever `params.w` is, the -1 included.
            const uint32_t source = lightSourceScratch[k];
            const auto remember = [&](float layer) {
                if (source < lightShadowLayer.size()) lightShadowLayer[source] = layer;
            };

            const uint32_t faces = type == LightType::Point ? 6u : 1u;
            // First fit rather than stop: a point needing six may not fit where a spot
            // needing one still does, and refusing the spot too would waste a layer.
            if (shadowMatrixScratch.size() + faces > kMaxShadowLayers) {
                light.params.w = -1.0f;
                remember(-1.0f);
                ++unshadowed;
                continue;
            }

            const float range = light.position.w > 0.0f ? light.position.w : 50.0f;
            // Near plane relative to range rather than a fixed constant: a fixed near on
            // a small light wastes most of the depth precision, and on a large one clips
            // geometry right next to the bulb.
            const float nearPlane = std::max(range * 0.005f, 0.01f);

            light.params.w = static_cast<float>(shadowMatrixScratch.size());
            remember(light.params.w);
            const glm::vec3 position(light.position);

            if (type == LightType::Spot) {
                // Field of view is twice the outer cone angle, widened 5% so the frustum
                // edge sits past where the falloff already reached zero.
                const float outerCos = glm::clamp(light.params.y, -0.999f, 0.999f);
                const float fov = std::min(2.0f * std::acos(outerCos) * 1.05f, 2.8f);
                const glm::vec3 dir = glm::normalize(glm::vec3(light.direction));
                const glm::vec3 up =
                    std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 proj = glm::perspective(fov, 1.0f, nearPlane, range);
                proj[1][1] *= -1.0f; // Vulkan clip space is Y-down
                shadowMatrixScratch.push_back(proj * glm::lookAt(position, position + dir, up));
            } else {
                // Cube faces in the +X, -X, +Y, -Y, +Z, -Z order shadow.glsl's major
                // axis selection assumes. Change one and the other breaks silently.
                static const glm::vec3 faceDir[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                static const glm::vec3 faceUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1},
                                                    {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
                glm::mat4 proj = glm::perspective(glm::half_pi<float>(), 1.0f, nearPlane, range);
                proj[1][1] *= -1.0f;
                for (uint32_t f = 0; f < 6; ++f) {
                    shadowMatrixScratch.push_back(proj * glm::lookAt(position, position + faceDir[f], faceUp[f]));
                }
            }
        }

        // An error rather than a warning, and the level is the point: a scene that
        // overflows has lights whose shadows switch on and off as the camera moves. It
        // is a scene that needs fixing, not a condition to live with. Still not fatal --
        // the frame renders and the lights that missed out illuminate without occluding.
        if (unshadowed != reportedShadowDrops) {
            if (unshadowed > 0) {
                const auto used = static_cast<uint32_t>(shadowMatrixScratch.size());
                core::Logger::error(core::LogCategory::Render,
                              "Punctual shadow atlas overflowed: %u of %u layers assigned, and %u light%s "
                              "could not fit -- a point needs six layers, a spot one. Those lights "
                              "illuminate without occluding. Give the scene fewer shadow-casting punctual "
                              "lights, or raise kMaxShadowLayers -- each layer is a scene re-render.",
                              used, kMaxShadowLayers, unshadowed, unshadowed == 1 ? "" : "s");
            } else {
                core::Logger::status(core::LogCategory::Render, "Punctual shadow atlas: every caster fits again");
            }
            reportedShadowDrops = unshadowed;
        }
    } else {
        for (GpuLight& light : lightScratch) light.params.w = -1.0f;
    }

    // Into this view's blocks. The selection above ranked and culled against the camera it
    // was given, so what it produced describes one view and not the frame.
    std::memcpy(frames[slot].lightBuffer[view.uniformSlot].mapped, lightScratch.data(),
                lightScratch.size() * sizeof(GpuLight));
    if (!shadowMatrixScratch.empty()) {
        std::memcpy(frames[slot].shadowMatrixBuffer[view.uniformSlot].mapped, shadowMatrixScratch.data(),
                    shadowMatrixScratch.size() * sizeof(glm::mat4));
    }

    // Everything below belongs to the atlas, and the atlas is the primary's: it was
    // rendered before this frame's first secondary chain recorded, so rewriting the
    // staleness cache here would leave the next frame comparing its matrices against a
    // mirror's -- and the cull views 2..2+layers against a projection nothing renders.
    if (!primary) return;

    // ----------------------------------------- which atlas layers need re-rendering
    // Decided here because this is where the matrices exist; recordPunctualShadows only
    // reads the answer. A layer is stale when its matrix changed, when it is newly
    // assigned, or when anything in the scene moved.
    //
    // Geometry is all-or-nothing: a revision bump says *something* moved, not what or
    // where, so every layer has to assume it was in shot. Narrowing that to the layers
    // whose frustum the moved geometry actually touches needs skinned instances to
    // report real world bounds, which today they do not.
    const auto layerCount = static_cast<uint32_t>(shadowMatrixScratch.size());

    // Views 2..2+layers are the atlas layers, culled against their own frustum. Unused
    // layers get the sun's box rather than a stale matrix: they are never drawn, and a
    // matrix left over from a previous assignment would have the dispatch building a
    // command list for a projection nothing renders through.
    for (uint32_t l = 0; l < kMaxShadowLayers; ++l) {
        cullViewProj[2 + l] = l < layerCount ? shadowMatrixScratch[l] : cullViewProj[1];
    }

    // Two ways geometry can move, and the revision only catches one. A transform change
    // bumps it; a *skinned* pose does not -- the instance stays where it is and only its
    // bone matrices change, so a walking character kept a frozen shadow while its body
    // animated out of it. Any skinned draw therefore dirties the atlas every frame.
    //
    // Conservative, and knowingly so: it re-renders every layer for one moving character,
    // because a skinned instance reports an infinite world box today and so intersects
    // every light there is. Narrowing this to the layers a mover actually touches is the
    // optimisation, and it is blocked on those bounds being real.
    const bool skinnedPresent = frames[slot].opaqueCommandCount > frames[slot].staticCommandCount;
    const bool geometryChanged =
        skinnedPresent || (instances != nullptr && instances->revision() != punctualCacheRevision);
    const bool cold = punctualCacheCold || !shadowCacheEnabled;
    for (uint32_t l = 0; l < kMaxShadowLayers; ++l) {
        const bool assigned = l < layerCount;
        const bool matrixChanged = !assigned || l >= cachedPunctualCount ||
                                   cachedPunctualMatrix[l] != shadowMatrixScratch[l];
        punctualLayerDirty[l] = cold || (assigned && (geometryChanged || matrixChanged));
    }
    for (uint32_t l = 0; l < layerCount; ++l) cachedPunctualMatrix[l] = shadowMatrixScratch[l];
    cachedPunctualCount = layerCount;
    if (instances != nullptr) punctualCacheRevision = instances->revision();
}

void Renderer::ensureInstanceCapacity(uint32_t slots) {
    auto s = core::Profiler::scope("ensureInstanceCapacity");
    // One slot minimum. A zero-sized buffer is not creatable, and an empty table is a
    // legitimate state -- a scene may be loaded before anything is placed in it.
    const uint32_t need = std::max(slots, 1u);
    if (need <= instanceCapacity) return;

    // Double, so a table filled one instance at a time does not reallocate on every
    // create(). The first call sizes exactly, because that one is the scene load and
    // there is nothing to guess.
    const uint32_t grown = instanceCapacity == 0 ? need : std::max(need, instanceCapacity * 2);

    // The buffers being replaced may be read by a frame still in flight. There is no
    // per-frame retirement list in this engine and adding one to serve an event that
    // happens at load time would be machinery for nothing.
    if (instanceCapacity != 0) {
        vkDeviceWaitIdle(ctx->device);
        core::Logger::status(core::LogCategory::Render, "Instance buffers grown: %u -> %u slots", instanceCapacity, grown);
    }

    // Five regions, each rounded up so the next one starts on an offset a descriptor
    // and an indirect draw will both accept. The CPU-written four come first so the
    // staging copy is one range.
    auto align = [](VkDeviceSize v) {
        return (v + kInstanceRegionAlign - 1) & ~(kInstanceRegionAlign - 1);
    };
    const VkDeviceSize cmdBytes = sizeof(VkDrawIndexedIndirectCommand) * grown;
    boundsRegion = align(sizeof(scene::GpuInstance) * grown);
    templateRegion = align(boundsRegion + sizeof(GpuCommandBounds) * grown);
    blendedRegion = align(templateRegion + cmdBytes);
    jointRegion = align(blendedRegion + cmdBytes);
    weightRegion = align(jointRegion + sizeof(glm::mat4) * std::max(jointCapacity, 1u));
    // 3.4's previous transforms. Inside the staged range because the CPU writes it, and
    // last within it because it was the last to arrive -- the ordering carries no other
    // meaning, since every consumer addresses its region by offset.
    prevRegion = align(weightRegion + sizeof(float) * std::max(weightCapacity, 1u));
    velocityCmdRegion = align(prevRegion + sizeof(glm::mat4) * grown);
    outRegion = align(velocityCmdRegion + cmdBytes);
    stagedBytes = outRegion;
    instanceDataBytes = align(outRegion + cmdBytes * kCullCommandLists);

    for (auto& f : frames) {
        destroyBuffer(*ctx, f.instanceData);
        destroyBuffer(*ctx, f.instanceStaging);
        destroyBuffer(*ctx, f.cullStats);

        // STORAGE as well as INDIRECT: the culling dispatch writes the command output,
        // and a buffer usage flag cannot be added after creation.
        f.instanceData = createBuffer(*ctx, instanceDataBytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "instanceData");
        f.instanceStaging = createBuffer(*ctx, stagedBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VMA_MEMORY_USAGE_AUTO,
                                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                         "instanceStaging");
        // HOST_ACCESS_RANDOM, not SEQUENTIAL_WRITE: the CPU reads this back. Sequential
        // write maps write-combined memory, where a read is an order of magnitude
        // slower than the atomic that produced the value.
        // Two counters per list: instances, then the triangles those instances actually
        // draw at the level the cull selected for them (C17). The second is the only place
        // an LOD win is *measurable* rather than asserted -- the CPU's `stats.triangles` is
        // what the scene holds, and the whole point of a chain is that a draw stops being
        // that number.
        f.cullStats = createBuffer(*ctx, sizeof(uint32_t) * kCullCommandLists * 2,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_AUTO,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   "cullStats");
        std::memset(f.cullStats.mapped, 0, sizeof(uint32_t) * kCullCommandLists * 2);

        const std::array<VkDescriptorBufferInfo, 2> tableInfos{
            VkDescriptorBufferInfo{f.instanceData.buffer, 0, sizeof(scene::GpuInstance) * grown},
            VkDescriptorBufferInfo{f.instanceData.buffer, prevRegion, sizeof(glm::mat4) * grown}};

        // Into every view's copy of the set. The instance table is *not* per view -- one
        // table feeds all of them -- but bindings 3 and 4 live in the frame set, and a
        // view whose set still named the pre-growth buffer would draw from freed memory.
        std::array<VkWriteDescriptorSet, 2> tableWrites{};
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            for (uint32_t i = 0; i < 2; ++i) {
                tableWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                tableWrites[i].dstSet = f.frameSet[v];
                tableWrites[i].dstBinding = 3 + i;
                tableWrites[i].descriptorCount = 1;
                tableWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                tableWrites[i].pBufferInfo = &tableInfos[i];
            }
            vkUpdateDescriptorSets(ctx->device, 2, tableWrites.data(), 0, nullptr);
        }

        if (f.cullSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setInfo.descriptorPool = descriptorPool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts = &cullSetLayout;
            vkCheck(vkAllocateDescriptorSets(ctx->device, &setInfo, &f.cullSet), "vkAllocateDescriptorSets(cull)");
        }

        // Grown with the command capacity, and zero-filled to "visible" rather than to
        // zero: an empty visibility set would make the first frame after every resize draw
        // nothing in phase 0 and everything in phase 1, which is correct but is a frame
        // that skips the cheap path entirely.
        destroyBuffer(*ctx, f.commandVisibility);
        f.commandVisibility = createBuffer(*ctx, sizeof(uint32_t) * std::max(grown, 1u),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_GPU_ONLY, 0, "commandVisibility");
        f.commandVisibilityInit = true;

        const std::array<VkDescriptorBufferInfo, 5> cullInfos{
            VkDescriptorBufferInfo{f.instanceData.buffer, boundsRegion, sizeof(GpuCommandBounds) * grown},
            VkDescriptorBufferInfo{f.instanceData.buffer, templateRegion, cmdBytes},
            VkDescriptorBufferInfo{f.instanceData.buffer, outRegion, cmdBytes * kCullCommandLists},
            VkDescriptorBufferInfo{f.cullStats.buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{f.commandVisibility.buffer, 0, VK_WHOLE_SIZE}};

        std::array<VkWriteDescriptorSet, 5> cullWrites{};
        for (uint32_t i = 0; i < 5; ++i) {
            cullWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            cullWrites[i].dstSet = f.cullSet;
            cullWrites[i].dstBinding = i;
            cullWrites[i].descriptorCount = 1;
            cullWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            cullWrites[i].pBufferInfo = &cullInfos[i];
        }
        vkUpdateDescriptorSets(ctx->device, 5, cullWrites.data(), 0, nullptr);

        // The skinning set, when there is a rig. Bound to this frame's joint matrices,
        // which is why it is per frame -- but two of its six bindings are the *scene's*
        // buffers, which is why writing it lives in its own function now (G4): those
        // buffers move when the scene's geometry grows, and this is not the only place
        // that has to notice.
        writeSkinSet(static_cast<uint32_t>(&f - &frames[0]), true);

        // Whatever these buffers held belonged to the old allocation.
        f.instanceRevision = 0;
        f.instanceUploadPending = false;
    }

    instanceCapacity = grown;
}

void Renderer::setAnimator(const scene::SceneAnimator* a, const scene::GltfScene* s) {
    animator = a;
    destroySkinResources();
    skinBatches.clear();
    skinDestBase.clear();
    skinnedVertexCount = 0;
    jointCapacity = 0;
    weightCapacity = 0;

    // **Cloth is a reason for this buffer to exist that has nothing to do with an
    // animator** (C19). Before it, no rig meant no deformed vertex buffer at all, and a
    // scene whose only moving geometry is a curtain has no rig -- so the gate is now "does
    // anything deform" rather than "is there a skeleton", and the animator is allowed to be
    // null the whole way down.
    const bool hasCloth = clothSystem != nullptr && !clothSystem->empty();
    const bool posed = animator != nullptr && (!s->skinVertices().empty() || !s->morphDeltas().empty());
    if (instances == nullptr || s == nullptr || (!posed && !hasCloth)) {
        animator = nullptr;
        clothDestBase.clear();
        return;
    }
    if (!posed) animator = nullptr;

    jointCapacity = animator != nullptr ? std::max(animator->totalJoints(), 1u) : 1u;
    weightCapacity = animator != nullptr ? std::max(animator->totalWeights(), 1u) : 1u;

    // One output range per deformed *instance*, in slot order. Two copies of the same
    // skinned mesh are two poses, so they cannot share vertices -- which is also why
    // these instances are excluded from 4.5's run merging.
    skinDestBase.assign(instances->slotCount(), UINT32_MAX);
    for (uint32_t slot = 0; slot < instances->slotCount(); ++slot) {
        if ((instances->slot(slot).meta.z & scene::kInstanceDeformed) == 0u) continue;
        skinDestBase[slot] = skinnedVertexCount;
        skinnedVertexCount += instances->drawRanges()[slot].vertexCount;
    }
    if (skinnedVertexCount == 0) {
        animator = nullptr;
        clothDestBase.clear();
        return;
    }

    // The same conditional the scene's own buffers carry: an acceleration-structure
    // build reads these vertices (S2.5), and the flags that say so are only legal when
    // VK_KHR_acceleration_structure is enabled.
    VkBufferUsageFlags rtUsage = 0;
    if (ctx->rayQuerySupported) {
        rtUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    // TRANSFER_DST because cloth is the third producer of this buffer and the only one
    // that is a copy rather than a dispatch (C19).
    skinnedVertices = createBuffer(*ctx, sizeof(scene::Vertex) * skinnedVertexCount,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtUsage,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "skinnedVertices");

    // One host-visible staging buffer per frame in flight, sized for every cloth vertex in
    // the scene. Created here rather than in `createFrameResources` for the reason the
    // particle buffers are: the size comes from the scene, so it has to be able to change
    // without the frame resources being torn down.
    // Already freed by the `destroySkinResources()` at the top of this function, which is
    // where every other buffer here is released and where a teardown that is not a reload
    // reaches them too.
    if (hasCloth) {
        const VkDeviceSize clothBytes =
            std::max<VkDeviceSize>(clothSystem->vertexCount(), 1) * sizeof(scene::Vertex);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            frames[i].clothStaging =
                createBuffer(*ctx, clothBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                             "clothStaging");
        }
    }

    // Both influence arrays are allocated with a floor of one element. A scene with
    // morph targets and no skin has nothing to put in the first, and a zero-sized
    // storage buffer is not a binding Vulkan accepts -- the descriptor still has to
    // point somewhere even though `influenceBase`'s sentinel means nothing reads it.
    const size_t influenceBytes = std::max<size_t>(s->skinVertices().size(), 1) * sizeof(scene::SkinVertex);
    skinInfluences = createBuffer(*ctx, influenceBytes,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "skinInfluences");
    if (!s->skinVertices().empty()) {
        uploader->uploadBuffer(*ctx, skinInfluences, s->skinVertices().data(),
                               s->skinVertices().size() * sizeof(scene::SkinVertex));
    }

    const size_t morphBytes = std::max<size_t>(s->morphDeltas().size(), 1) * sizeof(scene::MorphDelta);
    morphDeltas = createBuffer(*ctx, morphBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "morphDeltas");
    if (!s->morphDeltas().empty()) {
        uploader->uploadBuffer(*ctx, morphDeltas, s->morphDeltas().data(),
                               s->morphDeltas().size() * sizeof(scene::MorphDelta));
    }

    // The joint and weight regions live inside instanceData, so they are staged and
    // copied by the machinery already there rather than by a second one of their own.
    // Re-sizing the buffers is what puts them there -- and the capacity is reset first
    // because `ensureInstanceCapacity` returns early when the slot count already fits,
    // which it does: the regions moved, not the number of instances.
    const uint32_t slots = std::max(instanceCapacity, instances->slotCount());
    instanceCapacity = 0;
    ensureInstanceCapacity(slots);

    // Where each cloth's vertices land, resolved once here rather than searched for per
    // frame. Indexed by cloth, holding a byte offset into `skinnedVertices` -- so the
    // per-frame copy is a memcpy and a `VkBufferCopy` with nothing to look up.
    clothDestBase.assign(hasCloth ? clothSystem->count() : 0u, UINT32_MAX);
    for (uint32_t c = 0; c < clothDestBase.size(); ++c) {
        const uint32_t slot = clothSystem->at(c).instance;
        if (slot < skinDestBase.size()) clothDestBase[c] = skinDestBase[slot];
    }

    core::Logger::status(core::LogCategory::Render,
                   "Deformation: %u vertices, %u joints and %u morph weights across %u characters, %u cloths",
                   skinnedVertexCount, animator != nullptr ? animator->totalJoints() : 0u,
                   animator != nullptr ? animator->totalWeights() : 0u,
                   animator != nullptr ? animator->characterCount() : 0u,
                   hasCloth ? clothSystem->count() : 0u);

    // The dynamic tier of 3.9 is built over the buffer that was just created, so it
    // could not exist until now (S2.5).
    if (ctx->rayQuerySupported) {
        buildAccelerationStructures();
        pipelinesDirty = true;
    }
}

void Renderer::createSkinPipeline() {
    std::array<VkDescriptorSetLayoutBinding, kSkinBindings> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &info, nullptr, &skinSetLayout),
            "vkCreateDescriptorSetLayout(skin)");
    layoutBindings[skinSetLayout] = {bindings.begin(), bindings.end()};

    skinLayout = createLayout("skinning", {"skinning.comp"}, {skinSetLayout},
                              {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)}});

    verifyShaderBindings("skinning", {"skinning.comp"}, {skinSetLayout}, 0);
    skinPipeline = createComputePipeline(*ctx, skinLayout, "skinning.comp");
}

void Renderer::destroySkinPipeline() {
    vkDestroyPipeline(ctx->device, skinPipeline, nullptr);
    skinPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, skinLayout, nullptr);
    skinLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, skinSetLayout, nullptr);
    skinSetLayout = VK_NULL_HANDLE;
}

void Renderer::destroySkinResources() {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) destroyBuffer(*ctx, frames[i].clothStaging);
    clothCopies.clear();
    destroyBuffer(*ctx, skinnedVertices);
    destroyBuffer(*ctx, skinInfluences);
    destroyBuffer(*ctx, morphDeltas);
}

void Renderer::drawSceneIndirect(VkCommandBuffer cmd, uint32_t slot, uint32_t view, VariantPass pass) {
    const FrameSync& f = frames[slot];
    if (f.opaqueCommandCount == 0) return;

    const VkDeviceSize offset = viewCommandOffset(view);
    const VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize zero = 0;

    // One indirect call per variant group rather than one per half, and both state
    // changes bound lazily. Binding unconditionally is not free: doing it per group
    // measured as most of a 0.1 ms regression on the punctual atlas pass, where a
    // twenty-four-layer loop multiplies whatever this does by twenty-four.
    //
    // A scene using one variant -- which is every scene that has not registered any --
    // therefore records exactly what it recorded before variants existed: one pipeline
    // bind, one indirect draw, and the vertex buffer left as the caller bound it.
    uint32_t boundVariant = UINT32_MAX;
    bool boundSkinned = false;

    for (const VariantRange& r : f.opaqueRanges) {
        // The skinned half, out of the buffer skinning.comp wrote. Absent means the
        // dispatch never ran, and drawing those commands from the scene's vertex buffer
        // would give a character somebody else's silhouette.
        const bool skinned = r.first >= f.staticCommandCount;
        if (skinned && skinnedVertices.buffer == VK_NULL_HANDLE) continue;

        if (r.variant != boundVariant) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, variantPipeline(r.variant, pass));
            boundVariant = r.variant;
        }
        if (skinned != boundSkinned) {
            VkBuffer vb = skinned ? skinnedVertices.buffer : scene->vertexBuffer();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            boundSkinned = skinned;
        }

        vkCmdDrawIndexedIndirect(cmd, f.instanceData.buffer, offset + static_cast<VkDeviceSize>(r.first) * stride,
                                 r.count, stride);
    }

    // Left bound, the skinned buffer would leak into whatever the caller records next.
    // Restoring is one call against a class of bug that only shows up in the *following*
    // pass -- which is exactly how it was found.
    if (boundSkinned) {
        VkBuffer sceneVb = scene->vertexBuffer();
        vkCmdBindVertexBuffers(cmd, 0, 1, &sceneVb, &zero);
    }
}

/**
 * @brief Stage every cloth's solved vertices for this frame. True when there is a copy.
 *
 * The CPU half of the third producer: `ClothSystem::update` has already read the solve back
 * and reshaded it, so all that happens here is a `memcpy` per cloth into the frame's mapped
 * staging buffer and one `VkBufferCopy` region per cloth built beside it.
 *
 * One region per cloth rather than one covering everything, because the destinations are
 * whatever `skinDestBase` handed out and there is no reason two cloths' ranges are
 * adjacent. `clothCopies` is a member so the regions are built into storage that stops
 * growing after the first frame -- the pose-resolve row measured an allocation per body per
 * step at 179 ns and 2.4%, and this is the same shape one subsystem along.
 */
bool Renderer::recordClothUpload(uint32_t slot) {
    clothCopies.clear();
    if (clothSystem == nullptr || clothSystem->empty()) return false;
    FrameSync& f = frames[slot];
    if (f.clothStaging.mapped == nullptr) return false;

    auto* dst = static_cast<std::byte*>(f.clothStaging.mapped);
    VkDeviceSize at = 0;
    for (uint32_t c = 0; c < clothSystem->count() && c < clothDestBase.size(); ++c) {
        if (clothDestBase[c] == UINT32_MAX) continue;
        const auto& cloth = clothSystem->at(c);
        const VkDeviceSize bytes = cloth.vertices.size() * sizeof(scene::Vertex);
        if (bytes == 0 || at + bytes > f.clothStaging.size) continue;
        std::memcpy(dst + at, cloth.vertices.data(), bytes);
        clothCopies.push_back(
            {at, static_cast<VkDeviceSize>(clothDestBase[c]) * sizeof(scene::Vertex), bytes});
        at += bytes;
    }
    return !clothCopies.empty();
}

void Renderer::recordSkinning(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Skinning");
    const bool dispatching = !skinBatches.empty() && skinPipeline != VK_NULL_HANDLE;
    const bool copying = recordClothUpload(slot);
    // **The early-out tests both producers, and it used to test only the dispatch** -- so a
    // scene whose only deformed geometry is cloth returned here and never emitted the
    // barrier its copy needs (C19).
    if (!dispatching && !copying) return;

    GpuScope zone(gpuProfiler, cmd, slot, "Skinning");

    /*
     * The transfer half, recorded before the dispatches so that one barrier covers both.
     * The two write **disjoint ranges** of `skinnedVertices` -- `skinDestBase` hands every
     * deformed instance its own -- so they need no barrier between them, only the one
     * after, and that is the property that lets cloth be a third producer of one buffer
     * rather than a second buffer.
     *
     * A host-visible staging buffer per frame in flight and one `vkCmdCopyBuffer`, which is
     * the pattern debug lines, sprites and particle spawns all use. Deliberately **not**
     * `gfx::Uploader`, whose own header says every submit blocks and it is "fine for load
     * time, never for the frame loop".
     */
    if (copying) {
        FrameSync& f = frames[slot];
        vkCmdCopyBuffer(cmd, f.clothStaging.buffer, skinnedVertices.buffer,
                        static_cast<uint32_t>(clothCopies.size()), clothCopies.data());
    }

    if (!dispatching) {
        // Written by copy, read as vertex input. The same destination scope the dispatch
        // path uses, because the consumers are the same passes.
        bufferBarrier(cmd, {skinnedVertices.buffer}, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinLayout, 0, 1, &frames[slot].skinSet, 0, nullptr);

    // One dispatch per skinned instance. That is per *object*, not per draw per pass:
    // a scene with twenty characters records twenty dispatches once and then draws them
    // through thirteen passes with two indirect calls each. Folding them into a single
    // dispatch would mean every thread binary-searching a batch table to find out which
    // instance it belongs to, which is machinery bought with a real cost to save a
    // handful of push-constant writes.
    for (const SkinBatch& b : skinBatches) {
        SkinPush push{b.sourceBase, b.destBase,  b.influenceBase, b.jointBase,
                      b.vertexCount, b.morphBase, b.morphTargets,  b.weightBase};
        vkCmdPushConstants(cmd, skinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (b.vertexCount + 63) / 64, 1, 1);
    }

    // Written by compute **and by the copy above**, read as vertex input by every geometry
    // pass below. Both source scopes are named in one barrier rather than barriered
    // separately: it is one buffer, two disjoint sets of ranges, and one consumer (C19).
    bufferBarrier(cmd, {skinnedVertices.buffer},
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
}

// ------------------------------------------------------------------ particles (S3)

void Renderer::setParticles(const scene::ParticleSystem* system) {
    particles = system;

    destroyParticleResources();
    particleCapacity = system != nullptr ? system->capacity() : 0;

    if (particleCapacity == 0) {
        // Every scene in this repository but one. Nothing is allocated, no pipeline is
        // built, and recordParticles returns on its first line -- which is the whole of
        // what "the pass costs nothing when the scene has no emitters" means.
        pipelinesDirty = true;
        return;
    }

    particleIndexBits = 0;
    while ((1u << particleIndexBits) < particleCapacity) ++particleIndexBits;

    createParticleResources();
    // The pipelines are built in createPipelines(), which is where a feature toggle or
    // a shader reload would rebuild them too. Marking dirty rather than calling it
    // directly keeps that one path.
    pipelinesDirty = true;

    core::Logger::status(core::LogCategory::Render,
                   "Particles: %zu emitters, pool %u slots (%u index bits, %.1f KB), sort range %.1f",
                   system->emitters().size(), particleCapacity, particleIndexBits,
                   static_cast<double>(particleCapacity * (sizeof(scene::GpuParticle) + sizeof(uint32_t))) / 1024.0,
                   static_cast<double>(particleSortRange));
}

void Renderer::growLightBuffer() {
    if (ctx == nullptr || lightsWanted <= lightBufferCapacity) return;

    // Doubled until it fits, as the physics world and the particle pool are: a scene lighting
    // up gradually would otherwise reallocate every frame.
    uint32_t next = std::max(lightBufferCapacity, 1u);
    while (next < lightsWanted) next *= 2u;

    // The buffers may still be read by frames in flight, and the descriptor sets that name
    // them are about to be rewritten.
    vkDeviceWaitIdle(ctx->device);

    lightBufferCapacity = next;
    lightTileWords = (lightBufferCapacity + 31) / 32;
    if (lightTileWords > kLightTileMaxWords) {
        // The stated consequence rather than a silent cap, exactly as at init: past what the
        // build shader's shared mask can index, every light in the view is shaded by the
        // deferred loop instead of being dropped from the image.
        core::Logger::warn(core::LogCategory::Render,
                           "Light buffer grew to %u, past the %u lights tiled assignment can index; "
                           "running the deferred loop over every light in the view instead.",
                           lightBufferCapacity, kLightTileMaxWords * 32);
        lightTileWords = 0;
    }

    // **Nothing is carried across.** Unlike the particle pool, the light buffer holds no
    // state between frames: `updateLights` refills it from `lights` every frame for every
    // view, so a fresh allocation is a correct one.
    for (FrameSync& f : frames) {
        for (uint32_t v = 0; v < kMaxViews; ++v) {
            destroyBuffer(*ctx, f.lightBuffer[v]);
            f.lightBuffer[v] = createBuffer(*ctx, sizeof(GpuLight) * lightBufferCapacity,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                            "lightBuffer");

            const VkDescriptorBufferInfo info{f.lightBuffer[v].buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = f.frameSet[v];
            write.dstBinding = 1;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &info;
            vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);
        }
    }

    // The tile stride changed, and the pass that builds the mask is specialised on it.
    pipelinesDirty = true;
    core::Logger::status(core::LogCategory::Render, "Lights: buffer grown to %u (%u tile words)", lightBufferCapacity,
                         lightTileWords);
    lightsWanted = 0;
}

void Renderer::resizeParticlePool() {
    if (ctx == nullptr || particles == nullptr) return;
    const uint32_t want = particles->capacity();
    if (want == particleCapacity) return;
    if (particleCapacity == 0) {
        // Nothing to carry, so this is just the ordinary pairing.
        setParticles(particles);
        return;
    }

    // **The pool is the one buffer with state in it.** Every sort key is rewritten from
    // scratch by `particle_simulate.comp` each frame, and the spawn and emitter buffers are
    // filled per frame from the CPU -- so the copy below is the whole of what a resize has to
    // preserve. Without it a growth is a burst of particles vanishing at the moment the pool
    // filled up, which is precisely the moment somebody is looking at it.
    const uint32_t carried = std::min(particleCapacity, want);
    // Moved out of the member so the teardown below cannot free it. `destroyParticleResources`
    // early-outs on `particlePool.buffer`, so the handle has to be put back before the call
    // and taken away again after -- which is why this is a swap rather than a copy.
    GpuBuffer oldPool = particlePool;
    destroyParticleResourcesKeepingPool();

    particleCapacity = want;
    particleIndexBits = 0;
    while ((1u << particleIndexBits) < particleCapacity) ++particleIndexBits;
    createParticleResources();

    {
        VkCommandBuffer cmd = uploader->beginImmediate(*ctx);
        VkBufferCopy region{};
        region.size = static_cast<VkDeviceSize>(carried) * sizeof(scene::GpuParticle);
        vkCmdCopyBuffer(cmd, oldPool.buffer, particlePool.buffer, 1, &region);
        uploader->endImmediate(*ctx);
    }
    destroyBuffer(*ctx, oldPool);

    pipelinesDirty = true;
    core::Logger::status(core::LogCategory::Render, "Particles: pool grown to %u slots (%u index bits, %.1f KB)",
                         particleCapacity, particleIndexBits,
                         static_cast<double>(particleCapacity * (sizeof(scene::GpuParticle) + sizeof(uint32_t))) /
                             1024.0);
}

void Renderer::createParticleResources() {
    // Device-local and cleared to zero. Zero is a lifetime of zero, which is exactly
    // what `particle_simulate.comp` reads as a dead slot -- so the pool starts empty
    // without a shader or a fill pass to make it so. VMA zero-initialises a dedicated
    // allocation on this driver, and the explicit fill below removes the "on this
    // driver".
    // `TRANSFER_SRC` as well as `DST`, because `resizeParticlePool` copies the old pool into
    // the new one -- a pool that could only be written to would lose every particle in
    // flight at the moment it grew.
    particlePool = createBuffer(*ctx, static_cast<VkDeviceSize>(particleCapacity) * sizeof(scene::GpuParticle),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_AUTO, 0, "particlePool");
    particleKeys = createBuffer(*ctx, static_cast<VkDeviceSize>(particleCapacity) * sizeof(uint32_t),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_MEMORY_USAGE_AUTO, 0, "particleKeys");

    {
        // One blocking submit, once, at load. The alternative -- a "first frame" flag
        // threaded through recordParticles -- is a branch every frame to describe an
        // event that happens once.
        VkCommandBuffer cmd = uploader->beginImmediate(*ctx);
        vkCmdFillBuffer(cmd, particlePool.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, particleKeys.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
        uploader->endImmediate(*ctx);
    }

    // Sized for the worst frame there is: every free slot filled at once. That is
    // `capacity` spawns, and at 32 bytes each it is 128 KB for a 4096-slot pool -- far
    // cheaper than the alternative, which is a cap on births per frame that nothing
    // would report exceeding.
    const VkDeviceSize spawnBytes = static_cast<VkDeviceSize>(particleCapacity) * sizeof(scene::GpuSpawn);
    const VkDeviceSize emitterBytes =
        std::max<VkDeviceSize>(particles->emitters().size(), 1) * sizeof(scene::GpuEmitter);

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        particleEmitterBuffers[i] =
            createBuffer(*ctx, emitterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         "particleEmitters");
        particleSpawnBuffers[i] =
            createBuffer(*ctx, spawnBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         "particleSpawns");

        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &particleSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &alloc, &particleSets[i]),
                "vkAllocateDescriptorSets(particles)");

        const std::array<VkDescriptorBufferInfo, 4> buffers{
            VkDescriptorBufferInfo{particlePool.buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{particleKeys.buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{particleEmitterBuffers[i].buffer, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{particleSpawnBuffers[i].buffer, 0, VK_WHOLE_SIZE}};

        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t b = 0; b < 4; ++b) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = particleSets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(ctx->device, 4, writes.data(), 0, nullptr);
    }
}

void Renderer::destroyParticleResources() {
    if (particlePool.buffer == VK_NULL_HANDLE) return;
    destroyBuffer(*ctx, particlePool);
    destroyParticleResourcesKeepingPool();
}

/// Everything but `particlePool`, which `resizeParticlePool` is about to copy out of. Split
/// from `destroyParticleResources` rather than given a flag, because a flag on a teardown is
/// read as "sometimes leaks" until somebody checks.
void Renderer::destroyParticleResourcesKeepingPool() {
    if (ctx == nullptr) return;

    // The buffers may still be in flight: this runs from setParticles(), which a game
    // could call between frames, and from shutdown().
    vkDeviceWaitIdle(ctx->device);

    destroyBuffer(*ctx, particleKeys);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        destroyBuffer(*ctx, particleEmitterBuffers[i]);
        destroyBuffer(*ctx, particleSpawnBuffers[i]);
        if (particleSets[i] != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx->device, descriptorPool, 1, &particleSets[i]);
            particleSets[i] = VK_NULL_HANDLE;
        }
    }
    particleCapacity = 0;
    particleIndexBits = 0;
}

void Renderer::createParticlePipelines() {
    // Five sets and the order is not free: ibl.glsl declares its set index and
    // set 2 and ibl.glsl the environment cubes at set 3, and those two files are shared
    // with the lighting passes. Renumbering them so the particle layout could be
    // "tidier" would be changing four shaders to move one.
    const VkDescriptorSetLayout computeSets[] = {frameSetLayout, particleSetLayout, tlasSetLayout, iblSetLayout,
                                                 singleImageSetLayout};

    // One range covering the largest of the three compute pushes. Vulkan allows a
    // pipeline to push fewer bytes than its layout declares, so three layouts differing
    // only in a size would buy nothing.
    particleComputeLayout = createLayout("particle compute", {}, {computeSets[0], computeSets[1], computeSets[2],
                                                                computeSets[3], computeSets[4]},
                                         {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ParticleSimPush)}});

    verifyShaderBindings("particle emit", {"particle_emit.comp"},
                         {frameSetLayout, particleSetLayout, tlasSetLayout, iblSetLayout, singleImageSetLayout}, 0);
    verifyShaderBindings("particle simulate", {"particle_simulate.comp"},
                         {frameSetLayout, particleSetLayout, tlasSetLayout, iblSetLayout, singleImageSetLayout}, 0);

    particleEmitPipeline = createComputePipeline(*ctx, particleComputeLayout, "particle_emit.comp");
    particleSimulatePipeline = createComputePipeline(*ctx, particleComputeLayout, "particle_simulate.comp");
    particleSortPipeline = createComputePipeline(*ctx, particleComputeLayout, "particle_sort.comp");
    particleSortLocalPipeline = createComputePipeline(*ctx, particleComputeLayout, "particle_sort_local.comp");

    // ------------------------------------------------------------------- the draw
    particleDrawLayout = createLayout("particle draw", {},
                                      {frameSetLayout, particleSetLayout, scene->descriptorSetLayout()},
                                      {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ParticleDrawPush)}});

    GraphicsPipelineDesc desc;
    desc.vertexShader = "particle.vert";
    desc.fragmentShader = "particle.frag";
    // No vertex bindings at all: the six corners come from `gl_VertexIndex` and
    // everything else from the pool. There is no per-particle vertex buffer to keep in
    // step with a simulation that runs entirely on the device.
    desc.colorFormats = {kHdrFormat};
    desc.depthFormat = kDepthFormat;
    desc.samples = VK_SAMPLE_COUNT_1_BIT; // hdrTarget is already resolved
    desc.cullMode = VK_CULL_MODE_NONE;    // a billboard has no back
    desc.depthTest = true;
    desc.depthWrite = false; // a blended surface must not occlude the ones behind it
    // Premultiplied over, which is additive for a particle whose alpha is zero. One
    // blend state, therefore one draw, therefore one global sort -- see particle.frag.
    desc.blend = GraphicsPipelineDesc::Blend::PremultipliedOver;
    particleDrawPipeline = createGraphicsPipeline(*ctx, particleDrawLayout, desc);

    setObjectName(*ctx, reinterpret_cast<uint64_t>(particleDrawPipeline), VK_OBJECT_TYPE_PIPELINE, "particleDraw");
}

void Renderer::destroyParticlePipelines() {
    for (VkPipeline* p : {&particleEmitPipeline, &particleSimulatePipeline, &particleSortPipeline,
                          &particleSortLocalPipeline, &particleDrawPipeline}) {
        vkDestroyPipeline(ctx->device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    vkDestroyPipelineLayout(ctx->device, particleComputeLayout, nullptr);
    particleComputeLayout = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, particleDrawLayout, nullptr);
    particleDrawLayout = VK_NULL_HANDLE;
}

void Renderer::recordParticles(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Particles");
    if (!particlesEnabled || particles == nullptr || particleCapacity == 0) return;
    if (particleSimulatePipeline == VK_NULL_HANDLE) return;

    GpuScope zone(gpuProfiler, cmd, slot, "Particles");

    // --------------------------------------------------------------- host uploads
    // Straight into mapped memory during recording, exactly as the overlay's vertices
    // are: this slot's fence was waited on at the top of drawFrame, so nothing in
    // flight is reading them.
    const std::vector<scene::ParticleEmitter>& emitterList = particles->emitters();
    if (particleEmitterBuffers[slot].mapped != nullptr && !emitterList.empty()) {
        particles->writeGpuEmitters(static_cast<scene::GpuEmitter*>(particleEmitterBuffers[slot].mapped));
    }

    const std::vector<scene::GpuSpawn>& spawns = particles->spawns();
    if (!spawns.empty() && particleSpawnBuffers[slot].mapped != nullptr) {
        std::memcpy(particleSpawnBuffers[slot].mapped, spawns.data(), spawns.size() * sizeof(scene::GpuSpawn));
    }

    // Reported once per change, not once per frame -- 0.9's rule, and for the same
    // reason: a pool permanently over budget at 600 FPS would bury the log.
    if (particles->droppedSpawns() != reportedParticleDrops) {
        reportedParticleDrops = particles->droppedSpawns();
        core::Logger::warn(core::LogCategory::Render,
                     "Particles: %u spawns refused since load -- the %u-slot pool is full (raise "
                     "render.particleBudget or lower the emitter rates)",
                     reportedParticleDrops, particleCapacity);
    }

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], particleSets[slot], tlasSet, iblSet, view.sceneDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleComputeLayout, 0, 5, sets, 0, nullptr);

    /// Storage write -> storage read/write, over both buffers. Every stage below is
    /// separated from the next by exactly this, which is why it is a lambda rather than
    /// four copies of eleven lines.
    const auto computeBarrier = [&] {
        bufferBarrier(cmd, {particlePool.buffer, particleKeys.buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };

    // ------------------------------------------------------------- simulate (S3.2)
    {
        ParticleSimPush push{};
        push.dt = particles->step();
        push.now = particles->time();
        push.sortRange = particleSortRange;
        push.collisionThickness = particleCollisionThickness;
        push.texel = glm::vec2(1.0f / static_cast<float>(view.renderExtent.width),
                               1.0f / static_cast<float>(view.renderExtent.height));
        push.capacity = particleCapacity;
        push.indexBits = particleIndexBits;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleSimulatePipeline);
        vkCmdPushConstants(cmd, particleComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (particleCapacity + 63) / 64, 1, 1);
    }

    // ----------------------------------------------------------------- emit (S3.2)
    // After simulate, never before: a slot freed this frame is reused this frame, and
    // the other order would have simulate integrate and then kill the newborn sitting
    // in it. particle_emit.comp carries the argument.
    if (!spawns.empty()) {
        computeBarrier();

        ParticleEmitPush push{};
        push.spawnCount = static_cast<uint32_t>(spawns.size());
        push.indexBits = particleIndexBits;
        push.sortRange = particleSortRange;
        push.now = particles->time();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleEmitPipeline);
        vkCmdPushConstants(cmd, particleComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.spawnCount + 63) / 64, 1, 1);
    }

    // ----------------------------------------------------------------- sort (S3.3)
    //
    // A bitonic network: log2(N) stages, stage k made of the passes j = k/2 .. 1. The
    // passes with j < BLOCK stay inside one workgroup and are collapsed into a single
    // shared-memory dispatch, which is what turns 78 dispatches into 15 at 4096
    // particles. Everything is a fixed comparison network, so the result is
    // bit-identical run to run and a particle scene can join the golden set.
    {
        // Its own zone inside the outer one, because the sort is the half of this pass
        // whose cost is a *choice* -- the simulation is one dispatch over the pool and
        // the sort is a whole bitonic network over it, and a number that added them
        // together could not say which one to attack.
        GpuScope sortZone(gpuProfiler, cmd, slot, "ParticleSort");

        computeBarrier();

        const bool useLocal = particleCapacity >= kParticleSortBlock;
        const uint32_t localGroups = particleCapacity / kParticleSortBlock;
        const uint32_t globalGroups = (particleCapacity + 255) / 256;

        const auto sortPass = [&](VkPipeline pipeline, uint32_t k, uint32_t j, uint32_t groups) {
            ParticleSortPush push{k, j, particleCapacity, 0};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdPushConstants(cmd, particleComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, groups, 1, 1);
            computeBarrier();
        };

        // Stage 0: every stage up to BLOCK, entirely in shared memory. Nine dispatches
        // become one.
        uint32_t firstGlobalStage = 2;
        if (!particleSortEnabled) firstGlobalStage = particleCapacity * 2;
        if (useLocal && particleSortEnabled) {
            sortPass(particleSortLocalPipeline, 0, 0, localGroups);
            firstGlobalStage = kParticleSortBlock * 2;
        }

        for (uint32_t k = firstGlobalStage; k <= particleCapacity; k <<= 1) {
            const uint32_t lastGlobalJ = useLocal ? kParticleSortBlock : 1;
            for (uint32_t j = k >> 1; j >= lastGlobalJ; j >>= 1) {
                sortPass(particleSortPipeline, k, j, globalGroups);
            }
            // The tail of this stage, j < BLOCK, back inside shared memory.
            if (useLocal) sortPass(particleSortLocalPipeline, k, 0, localGroups);
        }
    }

    // ----------------------------------------------------------------- draw (S3.5)
    const uint32_t aliveCount = particles->aliveCount();
    if (aliveCount == 0) return;

    bufferBarrier(cmd, {particlePool.buffer, particleKeys.buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    // Write-after-write against the forward pass, in the layout it already holds --
    // the same dependency recordForward declares against lighting, for the same reason.
    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo color = colorAttachment(view.hdrTarget.view);

    // Tested against, never written: a particle is occluded by the scene and occludes
    // nothing, which is also why the sort had to happen.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = view.forwardDepthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleDrawPipeline);
    const VkDescriptorSet drawSets[] = {frames[slot].frameSet[view.uniformSlot], particleSets[slot], scene->descriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleDrawLayout, 0, 3, drawSets, 0, nullptr);

    ParticleDrawPush push{};
    push.now = particles->time();
    push.indexBits = particleIndexBits;
    vkCmdPushConstants(cmd, particleDrawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    // Six vertices, `aliveCount` instances, and the instance count comes from the CPU
    // rather than from an indirect buffer -- it is the tally the slot allocator already
    // keeps, so reading it back off the device would be asking the GPU a question the
    // host answered first.
    vkCmdDraw(cmd, 6, aliveCount, 0, 0);

    vkCmdEndRendering(cmd);

    stats.particles = aliveCount;
}

void Renderer::createCullPipeline() {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &info, nullptr, &cullSetLayout),
            "vkCreateDescriptorSetLayout(cull)");
    layoutBindings[cullSetLayout] = {bindings.begin(), bindings.end()};

    // Set 1 is the pyramid, alone. Declared here so the pipeline layout is complete, and
    // written in createRenderTargets where the image it points at is made.
    const VkDescriptorSetLayoutBinding hizBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo hizInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    hizInfo.bindingCount = 1;
    hizInfo.pBindings = &hizBinding;
    vkCheck(vkCreateDescriptorSetLayout(ctx->device, &hizInfo, nullptr, &hizSetLayout),
            "vkCreateDescriptorSetLayout(hiz)");
    layoutBindings[hizSetLayout] = {hizBinding};

    cullLayout = createLayout("cull", {"cull.comp"}, {cullSetLayout, hizSetLayout},
                              {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)}});

    verifyShaderBindings("cull", {"cull.comp"}, {cullSetLayout, hizSetLayout}, 0);
    cullPipeline = createComputePipeline(*ctx, cullLayout, "cull.comp");
}

void Renderer::destroyCullPipeline() {
    vkDestroyPipeline(ctx->device, cullPipeline, nullptr);
    cullPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(ctx->device, cullLayout, nullptr);
    cullLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, cullSetLayout, nullptr);
    cullSetLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(ctx->device, hizSetLayout, nullptr);
    hizSetLayout = VK_NULL_HANDLE;
    vkDestroySampler(ctx->device, hizSampler, nullptr);
    hizSampler = VK_NULL_HANDLE;
}

void Renderer::recordCull(VkCommandBuffer cmd, uint32_t slot, uint32_t phase, CullViews which) {
    // Three names, not two. `CullView` is a second view's list-0 pass and is **never
    // recorded in a one-view frame**, so the trace and the baseline table's `Cull` column
    // keep meaning exactly what they meant -- a zone that only exists once something asks
    // for it cannot move a published number.
    const char* zoneName = phase != 0 ? "CullHiZ" : (which == CullViews::Scene ? "Cull" : "CullView");
    auto cpuZone = core::Profiler::scope(zoneName);
    FrameSync& f = frames[slot];
    const uint32_t count = f.opaqueCommandCount;
    if (count == 0) return;

    GpuScope zone(gpuProfiler, cmd, slot, zoneName);

    // First use of a freshly made visibility buffer. Filled with 1 -- "everything was
    // visible" -- so phase 0 draws the whole frustum set and the occlusion test only ever
    // *removes* from a complete starting point. Filling with 0 would be equally correct
    // and would make the first frame draw everything twice.
    if (phase == 0 && f.commandVisibilityInit) {
        f.commandVisibilityInit = false;
        vkCmdFillBuffer(cmd, f.commandVisibility.buffer, 0, VK_WHOLE_SIZE, 1u);
        bufferBarrier(cmd, {f.commandVisibility.buffer}, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // Phase 1 writes into a buffer the phase 0 draw is still reading commands out of, and
    // overwrites the visibility phase 0 read. Different *ranges* of the command buffer, but
    // a range is not a barrier: without these the second dispatch races the first draw, and
    // the symptom is not a wrong image -- it is an intermittent device loss.
    if (phase == 1) {
        bufferBarrier(cmd, {f.instanceData.buffer}, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, outRegion, instanceDataBytes - outRegion);
        bufferBarrier(cmd, {f.commandVisibility.buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // The counters this frame will accumulate into. Zeroed on the device rather than
    // through the mapping, so the write is ordered against the dispatch by a barrier
    // instead of by hoping the host got there first.
    if (phase == 0) vkCmdFillBuffer(cmd, f.cullStats.buffer, 0, sizeof(uint32_t) * kCullCommandLists * 2, 0);

    // ALL_TRANSFER rather than CLEAR, and the distinction is not pedantry: sync
    // validation attributes `vkCmdFillBuffer`'s write to the *copy* stage, so a barrier
    // sourced at CLEAR alone covers none of it and reports `write_barriers: 0` -- a
    // barrier that is present, looks right, and orders nothing. ALL_TRANSFER spans
    // COPY, BLIT, RESOLVE and CLEAR, so it is correct whichever stage the fill is
    // attributed to. Found with SYNCHRONIZATION_VALIDATION on; standard validation
    // never mentions it.
    bufferBarrier(cmd, {f.cullStats.buffer}, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline);
    const VkDescriptorSet cullSets[] = {f.cullSet, view.hizSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout, 0, 2, cullSets, 0, nullptr);

    // One dispatch per view -- which, since the shadow views were ripped out with the
    // shadow system, is one: the camera.
    const uint32_t groups = (count + 63) / 64;

    // Phase 1 is the camera alone. The shadow views have no depth pyramid of their own and
    // no second draw to feed, so re-running them would be 25 dispatches writing commands
    // nothing reads. A `Camera` pass is the camera alone by definition.
    const uint32_t firstView = 0u;
    const uint32_t viewCount = (phase == 0 && which == CullViews::Scene) ? kCullViews : 1u;
    for (uint32_t v = firstView; v < firstView + viewCount; ++v) {
        CullPush push{};
        push.viewProj = cullViewProj[v];
        push.commandCount = count;
        // Phase 1's camera commands go to their own list, so the phase 0 draw can still be
        // in flight reading view 0 when this is written.
        push.outOffset = (phase == 0 ? v : kOcclusionView) * instanceCapacity;
        push.enabled = cullingEnabled ? 1u : 0u;
        push.viewIndex = phase == 0 ? v : kOcclusionView;
        push.phase = phase;
        push.occlusionEnabled = occlusionCullingEnabled ? 1u : 0u;
        push.pyramidSize = {static_cast<float>(view.depthPyramidExtent.width), static_cast<float>(view.depthPyramidExtent.height)};
        push.pyramidLevels = view.depthPyramidLevels;
        push.lodThresholds = scene::lodCoverageThresholds(meshLodThreshold);
        push.statsStride = kCullCommandLists;
        push.lodEnabled = (meshLodEnabled && v == 0u) ? 1u : 0u;
        vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Written by compute, read by the command processor. This is the one barrier the
    // whole feature needs, and getting its dstStage wrong shows up as last frame's
    // visibility rather than as a validation error.
    bufferBarrier(cmd, {f.instanceData.buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, outRegion, instanceDataBytes - outRegion);
}

void Renderer::writeSkinSet(uint32_t slot, bool allocate) {
    FrameSync& f = frames[slot];
    if (skinPipeline == VK_NULL_HANDLE || skinnedVertices.buffer == VK_NULL_HANDLE || scene == nullptr) return;
    if (f.skinSet == VK_NULL_HANDLE) {
        // Only the capacity path allocates. Called from `setScene`, a slot with no set is
        // a slot whose instance buffers do not exist yet, and it will be written when they
        // are -- allocating here would leave a set bound to a buffer that is about to be
        // replaced.
        if (!allocate) return;
        VkDescriptorSetAllocateInfo skinAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        skinAlloc.descriptorPool = descriptorPool;
        skinAlloc.descriptorSetCount = 1;
        skinAlloc.pSetLayouts = &skinSetLayout;
        vkCheck(vkAllocateDescriptorSets(ctx->device, &skinAlloc, &f.skinSet), "vkAllocateDescriptorSets(skin)");
    }

    const std::array<VkDescriptorBufferInfo, kSkinBindings> skinInfos{
        VkDescriptorBufferInfo{scene->vertexBuffer(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{skinnedVertices.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{skinInfluences.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{f.instanceData.buffer, jointRegion, sizeof(glm::mat4) * std::max(jointCapacity, 1u)},
        VkDescriptorBufferInfo{morphDeltas.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{f.instanceData.buffer, weightRegion, sizeof(float) * std::max(weightCapacity, 1u)}};

    std::array<VkWriteDescriptorSet, kSkinBindings> skinWrites{};
    for (uint32_t i = 0; i < kSkinBindings; ++i) {
        skinWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        skinWrites[i].dstSet = f.skinSet;
        skinWrites[i].dstBinding = i;
        skinWrites[i].descriptorCount = 1;
        skinWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        skinWrites[i].pBufferInfo = &skinInfos[i];
    }
    vkUpdateDescriptorSets(ctx->device, kSkinBindings, skinWrites.data(), 0, nullptr);
}

void Renderer::recordMaterialUpload(VkCommandBuffer cmd, uint32_t slot) {
    FrameSync& f = frames[slot];
    if (scene == nullptr) return;
    const uint32_t count = scene->materialTableCount();
    if (count == 0 || f.materialRevision == scene->materialRevision()) return;
    f.materialRevision = scene->materialRevision();

    // `vkCmdUpdateBuffer` rather than a staging buffer, and the reason is the size: a
    // material is 96 bytes, so a scene with a hundred of them is ten kilobytes -- inside
    // the 65536-byte limit the command carries and cheaper than owning a staging
    // allocation per frame slot for a table that usually never moves. A scene past that
    // limit is written in chunks rather than truncated.
    constexpr VkDeviceSize kMaxUpdate = 65536;
    const auto* bytes = reinterpret_cast<const std::byte*>(scene->materialData());
    const VkDeviceSize total = static_cast<VkDeviceSize>(count) * sizeof(scene::GpuMaterial);
    for (VkDeviceSize written = 0; written < total;) {
        // Rounded down to a whole material so a chunk boundary never splits one, and to
        // four bytes because `vkCmdUpdateBuffer` requires it -- which sizeof(GpuMaterial)
        // already guarantees.
        const VkDeviceSize chunk = std::min(kMaxUpdate - kMaxUpdate % sizeof(scene::GpuMaterial), total - written);
        vkCmdUpdateBuffer(cmd, scene->materialBuffer(), written, chunk, bytes + written);
        written += chunk;
    }

    // Read by every shading path there is: the G-buffer and forward fragment shaders, and
    // the compute passes that shade a ray hit.
    bufferBarrier(cmd, {scene->materialBuffer()}, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT, 0, total);
}

void Renderer::recordInstanceUpload(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("InstanceUpload");
    FrameSync& f = frames[slot];

    // The one zone deliberately *outside* `Frame`: this runs before the frame scope opens,
    // and moving either the call or the scope would change what the published `Frame`
    // number means. Named anyway, because a copy of the whole instance table plus the
    // material updates is the only GPU work in the command buffer nothing else measures.
    // Unconditional, above both early-outs, so a frame that uploads nothing reads as a
    // named zero rather than as an absent zone.
    GpuScope zone(gpuProfiler, cmd, slot, "InstanceUpload");

    recordMaterialUpload(cmd, slot);
    if (!f.instanceUploadPending) return;
    f.instanceUploadPending = false;

    // One region covering all four. They were all written this frame -- the blended
    // commands unconditionally, the rest whenever the table moved -- and splitting the
    // copy to skip a few kilobytes would cost more in barriers than it saves in bytes.
    VkBufferCopy region{0, 0, stagedBytes};
    vkCmdCopyBuffer(cmd, f.instanceStaging.buffer, f.instanceData.buffer, 1, &region);

    // Read by vertex shaders (the instance record), by the command processor (the
    // indirect commands) and by compute (4.2's culling, against the bounds). All three
    // are named here rather than barriered separately: it is one buffer and one copy.
    bufferBarrier(cmd, {f.instanceData.buffer}, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, 0, stagedBytes);
}

void Renderer::updateInstances(uint32_t slot) {
    auto cpuZone = core::Profiler::scope("updateInstances");
    FrameSync& f = frames[slot];

    // Reported every frame, recomputed only when the table moves. These are properties
    // of the table rather than of a frame slot, which is why they are not simply
    // accumulated by whichever slot happened to rebuild.
    stats.drawCalls = opaqueDrawCalls;
    stats.primitives = opaqueInstanceCount;
    stats.triangles = opaqueTriangles;

    // Joint matrices, every frame and before the revision check below: a rig that is
    // animating changes these without changing the table, and a table that never
    // changes is exactly the case a revision test would skip.
    if (animator != nullptr && jointCapacity > 0) {
        auto* mapped = static_cast<std::byte*>(f.instanceStaging.mapped);
        auto* joints = reinterpret_cast<glm::mat4*>(mapped + jointRegion);
        auto* weights = reinterpret_cast<float*>(mapped + weightRegion);
        for (uint32_t c = 0; c < animator->characterCount(); ++c) {
            const auto& m = animator->jointMatrices(c);
            if (!m.empty()) std::memcpy(joints + animator->jointOffset(c), m.data(), m.size() * sizeof(glm::mat4));
            const auto& w = animator->morphWeights(c);
            if (!w.empty()) std::memcpy(weights + animator->weightOffset(c), w.data(), w.size() * sizeof(float));
        }
        f.instanceUploadPending = true;
    }

    // Last frame's transforms, every frame and *not* gated on the revision below --
    // which is the one thing about this upload worth stating. The history changes at the
    // end of every frame whether or not the table did, so a slot that skipped the copy
    // because the revision matched would keep serving the history from two frames before
    // it last uploaded. That reads as a stationary object smearing, intermittently, on
    // whichever slot is stale: 64 bytes a slot is far cheaper than that bug.
    if (taaEnabled && instances->dynamicCount() > 0) {
        std::memcpy(static_cast<std::byte*>(f.instanceStaging.mapped) + prevRegion, instances->previousData(),
                    instances->previousBytes());
        f.instanceUploadPending = true;
    }

    // ---------------------------------------------------- which variant draws what (G5)
    // Refreshed when the material table moves, and `variantAssignment` bumped only when
    // the answer actually changed. Both halves matter. A game animating a material's
    // colour bumps the material revision every frame, and regrouping every command for a
    // value that did not move is exactly what the revision gate below exists to avoid;
    // conversely a material that *does* change variant regroups the whole list without
    // any instance having moved, which the table's own revision would never report.
    if (scene != nullptr && seenMaterialRevision != scene->materialRevision()) {
        seenMaterialRevision = scene->materialRevision();
        const uint32_t count = scene->materialTableCount();
        bool changed = materialVariant.size() != count;
        materialVariant.resize(count, 0u);
        for (uint32_t m = 0; m < count; ++m) {
            uint32_t want = scene->material(m).shader;
            if (want >= variants.size()) {
                // Clamped rather than fatal: a material naming a variant nobody
                // registered renders as the default, which is wrong-looking and
                // recoverable, where an abort takes down a game over one asset.
                if (!reportedVariantOverflow) {
                    reportedVariantOverflow = true;
                    core::Logger::warn(core::LogCategory::Render,
                                       "Material %u names shader variant %u, but only %zu are registered -- "
                                       "drawing it with the engine's",
                                       m, want, variants.size());
                }
                want = 0;
            }
            if (materialVariant[m] != want) {
                materialVariant[m] = want;
                changed = true;
            }
        }
        if (changed) ++variantAssignment;
    }

    // The table is written once per revision per frame slot, not once per frame. A
    // static scene therefore costs two memcpys at load and nothing afterwards, and a
    // scene where one object moves costs the whole array -- which is the trade a flat
    // uploadable array makes, and the reason the arrays are split by consumer.
    if (f.instanceRevision == instances->revision() && f.variantAssignment == variantAssignment) return;

    auto* base = static_cast<std::byte*>(f.instanceStaging.mapped);
    std::memcpy(base, instances->shadingData(), instances->shadingBytes());

    // ------------------------------------------------------- opaque draw commands
    // One command per live opaque instance, `firstInstance` naming the slot the
    // vertex shader will read its transform from. Runs of consecutive slots sharing a
    // primitive collapse into a single instanced command (4.5): the shader reads
    // `gl_InstanceIndex`, which counts up from `firstInstance` across the run, so N
    // copies of one mesh cost one command and N transforms rather than N commands.
    //
    // Each command's bounds go out beside it, as the union over its run -- that is
    // what the cull dispatch tests, and it is why a run is capped rather than merged
    // without limit.
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(base + templateRegion);
    auto* cbounds = reinterpret_cast<GpuCommandBounds*>(base + boundsRegion);
    uint32_t count = 0;
    uint32_t drawnInstances = 0;
    uint64_t triangles = 0;

    const auto& ranges = instances->drawRanges();
    f.opaqueRanges.clear();
    skinBatches.clear();

    // A slot's variant is its material's. An index past the cache -- a material created
    // since the last refresh -- reads as the default, which is what an unassigned
    // material is anyway, and the next refresh puts it in its real group.
    const auto slotVariant = [&](const scene::GpuInstance& inst) {
        return inst.meta.y < materialVariant.size() ? materialVariant[inst.meta.y] : 0u;
    };

    // The LOD chain for the command just written at `c` (C17).
    //
    // **Level 0 is read out of `cmds[c]` and not out of anything else.** The command is the
    // authority on what it draws, so copying its range into the chain's first slot in the
    // same statement that wrote it is the one spelling where the two cannot disagree --
    // including for the deformed half, whose range is not the primitive's at all.
    //
    // The coarser levels come off the primitive the instance names, which is the second half
    // of the same property: there is no per-command LOD array laid out beside `cmds`, so
    // there is nothing for a merged run, a variant sweep or a later append to get out of
    // step with. A chain travels inside the record that describes it.
    const auto writeLodChain = [&](uint32_t c, const scene::GpuInstance& inst, bool allowChain) {
        cbounds[c].lods[0] = glm::uvec2(cmds[c].firstIndex, cmds[c].indexCount);
        cbounds[c].lodLevels = 1;
        if (!allowChain || scene == nullptr) return;

        const std::vector<scene::Primitive>& prims = scene->primitives();
        const uint32_t primitive = inst.meta.x;
        if (primitive >= prims.size()) return;

        const scene::Primitive& p = prims[primitive];
        const uint32_t levels = std::min(p.lodCount, scene::kMaxLodLevels);
        for (uint32_t l = 0; l < levels; ++l) {
            cbounds[c].lods[l + 1] = glm::uvec2(p.lods[l].firstIndex, p.lods[l].indexCount);
        }
        cbounds[c].lodLevels = 1 + levels;
    };

    // The walk that turns slots into commands, run once per (variant, mask) group so
    // that each group comes out as a contiguous range. Two facts make that free rather
    // than a cost: a run only ever merges slots drawing the *same primitive* --
    // `firstIndex` and `indexCount` must both match below -- and one primitive has one
    // material, which has one variant and one alpha mode. So a run was already
    // all-one-variant and already all-masked or all-unmasked; splitting the walk does
    // not split any run that would have formed on its own.
    //
    // What it does cost is runs that would have formed *across* a boundary, since a
    // merge also requires slots to be adjacent. That grows with the number of groups, so
    // an instance-heavy scene using many variants fragments more than the old two-group
    // walk did. On Sponza it is worth nothing either way: its 103 primitives are all
    // distinct and nothing merges at all.
    // Which variants the live instances actually use. The sweep below is a walk over
    // every slot, run once per variant, so a game that registers forty and uses two would
    // otherwise walk the table forty times to find thirty-eight of them empty. One
    // O(slots) pass to find out is the whole of the fix, and it is why registering a
    // variant a level never places costs nothing on the CPU either.
    variantsPresent.assign(variants.size(), 0u);
    for (uint32_t s = 0; s < instances->slotCount(); ++s) {
        const scene::GpuInstance& inst = instances->slot(s);
        if ((inst.meta.z & scene::kInstanceLive) == 0u) continue;
        variantsPresent[slotVariant(inst)] = 1u;
    }

    const auto sweep = [&](uint32_t variant, bool wantDeformed) {
        const uint32_t groupFirst = count;
        uint32_t unmasked = 0;
        for (uint32_t group = 0; group < 2; ++group) {
            const bool wantMasked = group == 1;
            const uint32_t groupStart = count;
            for (uint32_t s = 0; s < instances->slotCount(); ++s) {
                const scene::GpuInstance& inst = instances->slot(s);
                if ((inst.meta.z & scene::kInstanceLive) == 0u) continue;
                if ((inst.meta.z & scene::kInstanceBlended) != 0u) continue;
                if (((inst.meta.z & scene::kInstanceDeformed) != 0u) != wantDeformed) continue;
                if (((inst.meta.z & scene::kInstanceMasked) != 0u) != wantMasked) continue;
                if (ranges[s].indexCount == 0) continue;
                if (slotVariant(inst) != variant) continue;

                const scene::GpuInstanceBounds& b = instances->slotBounds(s);

                if (wantDeformed) {
                    // Its vertices come out of the buffer skinning.comp wrote, so this
                    // half is never merged into a run and every command needs the rebase
                    // and the batch record below.
                    if (s >= skinDestBase.size()) continue;
                    const uint32_t dest = skinDestBase[s];
                    if (dest == UINT32_MAX) continue;

                    // Indices in the shared buffer are absolute, so shifting the whole
                    // primitive to its own skinned range is exactly what a
                    // negative-or-positive vertexOffset does. No index rewriting, no
                    // second index buffer.
                    cmds[count] = {ranges[s].indexCount, 1, ranges[s].firstIndex,
                                   static_cast<int32_t>(dest) - static_cast<int32_t>(ranges[s].baseVertex), s};

                    // Never culled. A skinned mesh's world bounds are a per-frame quantity
                    // that nothing here computes -- the bind-pose box is wrong the moment
                    // the animation leaves it, and a box that is wrong in the *small*
                    // direction makes a character vanish. An infinite box is the honest
                    // answer until something computes the real one; the cost is one
                    // command per character.
                    cbounds[count].boundsMin = glm::vec4(-1e30f, -1e30f, -1e30f, 0.0f);
                    cbounds[count].boundsMax = glm::vec4(1e30f, 1e30f, 1e30f, 0.0f);
                    // Never LOD'd either, and for the same reason it is never culled: this
                    // command draws out of the buffer `skinning.comp` wrote, whose contents
                    // are the *bind-pose* indices shifted by `vertexOffset`. A simplified
                    // level indexes vertices the skinning dispatch was never asked to
                    // deform, so it would draw a character out of somebody else's pose.
                    writeLodChain(count, inst, false);
                    ++count;

                    // **A cloth's vertices arrive by transfer, so there is no dispatch to
                    // record.** This is the one site in the renderer that names the flag:
                    // everything else about a cloth instance is `kInstanceDeformed`, which
                    // it already carries, and gets the command, the rebase, the infinite
                    // box and the skipped LOD chain above for free.
                    if ((inst.meta.z & scene::kInstanceCloth) != 0u) continue;

                    const uint32_t character = inst.meta.w;
                    const bool posed = animator != nullptr && character < animator->characterCount();
                    skinBatches.push_back(
                        {ranges[s].baseVertex, dest, ranges[s].skinOffset,
                         posed ? animator->jointOffset(character) : 0u, ranges[s].vertexCount,
                         ranges[s].morphOffset, ranges[s].morphTargets,
                         // The placement's own weight run, offset by where this character's
                         // copy of the pose starts. Two characters over one node therefore
                         // read two different expressions out of one flat buffer.
                         (posed ? animator->weightOffset(character) : 0u) + ranges[s].morphWeightOffset});
                } else if (count > groupStart && cmds[count - 1].firstIndex == ranges[s].firstIndex &&
                           cmds[count - 1].indexCount == ranges[s].indexCount &&
                           cmds[count - 1].firstInstance + cmds[count - 1].instanceCount == s &&
                           cmds[count - 1].instanceCount < kMaxInstancesPerCommand) {
                    // Extend the previous command: this slot is the next one along and
                    // draws exactly the same geometry. Materials may still differ, since
                    // the fragment stage reads its material out of the instance record
                    // rather than out of the command -- but not their variants, which is
                    // what the group this run lives in already guarantees.
                    cmds[count - 1].instanceCount++;
                    cbounds[count - 1].boundsMin = glm::min(cbounds[count - 1].boundsMin, b.worldMin);
                    cbounds[count - 1].boundsMax = glm::max(cbounds[count - 1].boundsMax, b.worldMax);
                } else {
                    cmds[count] = {ranges[s].indexCount, 1, ranges[s].firstIndex, 0, s};
                    cbounds[count].boundsMin = b.worldMin;
                    cbounds[count].boundsMax = b.worldMax;
                    writeLodChain(count, inst, true);
                    ++count;
                }

                ++drawnInstances;
                triangles += ranges[s].indexCount / 3;
            }
            // Where this group's unmasked run ends. Guarding the merge on
            // `count > groupStart` above is what stops the first masked command extending
            // the last unmasked one: they cannot draw the same primitive, so it could not
            // happen today, but the boundary is load-bearing and saying so costs one
            // comparison.
            if (group == 0) unmasked = count - groupFirst;
        }
        if (count > groupFirst) f.opaqueRanges.push_back({variant, groupFirst, count - groupFirst, unmasked});
    };

    for (uint32_t variant = 0; variant < variants.size(); ++variant) {
        if (variantsPresent[variant] != 0u) sweep(variant, false);
    }

    // The w components carry the per-instance bounding radius, which means nothing for
    // a union. Zeroed so a capture does not show a number that looks meaningful. Only the
    // static commands need it: the deformed ones below write an explicit infinite box.
    for (uint32_t i = 0; i < count; ++i) {
        cbounds[i].boundsMin.w = 0.0f;
        cbounds[i].boundsMax.w = 0.0f;
    }

    // -------------------------------------------- deformed commands (4.4, S2.1)
    // After every static one, so a pass can draw [0, staticCount) with the scene's
    // vertex buffer and the rest with the deformed one.
    f.staticCommandCount = count;
    const auto staticRangeCount = static_cast<ptrdiff_t>(f.opaqueRanges.size());

    // `deforms()` rather than `animator != nullptr`, because cloth is a deformed instance
    // with no rig behind it and this gate used to be the reason it drew out of the scene's
    // vertex buffer instead of its own (C19).
    if (deforms()) {
        for (uint32_t variant = 0; variant < variants.size(); ++variant) {
            if (variantsPresent[variant] != 0u) sweep(variant, true);
        }
    }

    // Two lists, each already ascending by variant, merged into one. This is the whole
    // reason a range carries `first` rather than being read off a running total: the
    // command *buffer* has to stay static-then-skinned, because that split is what lets a
    // pass bind one vertex buffer per half, while the *walk* wants to be variant-major so
    // the pipeline bind -- the expensive state change -- happens once per variant instead
    // of once per half per variant. Stable, so a variant's static range still precedes
    // its skinned one and the vertex buffer changes at most twice within it.
    std::inplace_merge(f.opaqueRanges.begin(), f.opaqueRanges.begin() + staticRangeCount, f.opaqueRanges.end(),
                       [](const VariantRange& a, const VariantRange& b) { return a.variant < b.variant; });

    f.opaqueCommandCount = count;
    f.instanceRevision = instances->revision();
    f.variantAssignment = variantAssignment;

    // Two different numbers, and the overlay reports both: `drawCalls` is what the CPU
    // submits and `primitives` is what the GPU draws. Instancing is exactly the gap
    // between them, so collapsing them into one figure would hide the thing 4.5 buys.
    opaqueDrawCalls = count;
    opaqueInstanceCount = drawnInstances;
    opaqueTriangles = triangles;
    stats.drawCalls = count;
    stats.primitives = drawnInstances;
    stats.triangles = triangles;
}

uint32_t Renderer::buildBlendedCommands(uint32_t slot, const scene::Camera& camera) {
    auto cpuZone = core::Profiler::scope("buildBlendedCommands");
    // Sort back to front. A blended surface reads whatever is already in the target,
    // so the far ones have to land first; with depth writes off there is nothing to
    // sort it out afterwards. The key is the instance's world-bounds centre along the
    // view direction, which is per-object and so gets interpenetrating geometry wrong
    // -- the standard trade, and the reason this is a sort and not a solution.
    //
    // This is the one pass whose commands the CPU must build every frame, and the one
    // place 0.11 does not make recording O(passes): a sort is O(n log n) in the number
    // of blended objects wherever it runs, and moving it to the GPU is a sort kernel
    // rather than a culling one.
    forwardOrder.clear();
    const glm::vec3 eye = camera.position();
    const glm::vec3 forward = camera.forward();

    for (uint32_t s = 0; s < instances->slotCount(); ++s) {
        const scene::GpuInstance& inst = instances->slot(s);
        if ((inst.meta.z & (scene::kInstanceLive | scene::kInstanceBlended)) != (scene::kInstanceLive | scene::kInstanceBlended)) continue;
        if (instances->drawRanges()[s].indexCount == 0) continue;

        const scene::GpuInstanceBounds& b = instances->slotBounds(s);
        const glm::vec3 centre = glm::vec3(b.worldMin + b.worldMax) * 0.5f;
        forwardOrder.push_back({glm::dot(centre - eye, forward), s});
    }
    std::sort(forwardOrder.begin(), forwardOrder.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(
        static_cast<std::byte*>(frames[slot].instanceStaging.mapped) + blendedRegion);
    uint32_t count = 0;
    blendedRanges.clear();
    for (const auto& entry : forwardOrder) {
        const auto& r = instances->drawRanges()[entry.second];

        // Variant *runs*, not variant groups, and this is the one pass where they cannot
        // be groups: depth order is the whole point here, so the list may not be sorted
        // by anything else and a variant that appears twice in that order gets two
        // entries. A scene whose blended surfaces all use one variant still yields one
        // range and one indirect draw, which is what it yielded before variants existed.
        const uint32_t variant =
            instances->slot(entry.second).meta.y < materialVariant.size()
                ? materialVariant[instances->slot(entry.second).meta.y]
                : 0u;
        if (blendedRanges.empty() || blendedRanges.back().variant != variant) {
            blendedRanges.push_back({variant, count, 0, 0});
        }
        ++blendedRanges.back().count;

        cmds[count++] = {r.indexCount, 1, r.firstIndex, 0, entry.second};
        stats.triangles += r.indexCount / 3;
    }

    // No run-merging here, deliberately. Depth order is the whole point of this pass
    // and two adjacent slots being adjacent in *depth* is a coincidence.
    stats.blendedDrawCalls = count;

    // Unconditional: these are rewritten every frame, so the copy is owed even when
    // the table itself has not moved.
    frames[slot].instanceUploadPending = true;
    return count;
}

uint32_t Renderer::buildVelocityCommands(uint32_t slot) {
    auto cpuZone = core::Profiler::scope("buildVelocityCommands");
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(
        static_cast<std::byte*>(frames[slot].instanceStaging.mapped) + velocityCmdRegion);
    const auto& ranges = instances->drawRanges();
    uint32_t count = 0;

    // Static half first, deformed second, so `recordVelocity` binds two vertex buffers
    // and never four. No masked/unmasked split, unlike the opaque list: this pass has no
    // alpha-test variant to select between -- a correction written for a texel the
    // G-buffer discarded is rejected by the depth test rather than by a fragment shader.
    //
    // No run-merging either. A run's members must be adjacent slots drawing one
    // primitive *and* share a command, and a command's instances share one draw -- but
    // `prevInstances[gl_InstanceIndex]` differs per instance and is read per vertex, so
    // merging would be correct here and buys nothing on any scene that exists: dynamic
    // instances are characters and bodies, which are distinct meshes.
    for (uint32_t s = 0; s < instances->slotCount(); ++s) {
        const scene::GpuInstance& inst = instances->slot(s);
        constexpr uint32_t kWant = scene::kInstanceLive | scene::kInstanceDynamic;
        if ((inst.meta.z & kWant) != kWant) continue;
        if ((inst.meta.z & (scene::kInstanceBlended | scene::kInstanceDeformed)) != 0u) continue;
        if (ranges[s].indexCount == 0) continue;
        cmds[count++] = {ranges[s].indexCount, 1, ranges[s].firstIndex, 0, s};
    }
    velocityStaticCount = count;

    if (deforms()) {
        for (uint32_t s = 0; s < instances->slotCount(); ++s) {
            const scene::GpuInstance& inst = instances->slot(s);
            if ((inst.meta.z & scene::kInstanceLive) == 0u) continue;
            if ((inst.meta.z & scene::kInstanceDeformed) == 0u) continue;
            if ((inst.meta.z & scene::kInstanceBlended) != 0u) continue;
            if (ranges[s].indexCount == 0 || s >= skinDestBase.size()) continue;

            const uint32_t dest = skinDestBase[s];
            if (dest == UINT32_MAX) continue;

            // The same rebase the opaque list applies, and it has to be the same or the
            // pass draws a character's silhouette out of somebody else's vertices.
            cmds[count++] = {ranges[s].indexCount, 1, ranges[s].firstIndex,
                             static_cast<int32_t>(dest) - static_cast<int32_t>(ranges[s].baseVertex), s};
        }
    }

    frames[slot].instanceUploadPending = true;
    stats.velocityDrawCalls = count;
    return count;
}

void Renderer::recordVelocity(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Velocity");
    GpuScope zone(gpuProfiler, cmd, slot, "Velocity");

    transitionImage(cmd, view.velocityTarget.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // CLEAR, and the clear value is the whole static-geometry path: zero means "the
    // reprojection taa.comp already computed was right". Nothing has to draw to say so,
    // which is why a scene with nothing dynamic in it pays a clear and no draws.
    VkRenderingAttachmentInfo color = colorAttachment(view.velocityTarget.view, {{0.0f, 0.0f, 0.0f, 0.0f}});

    // The resolved depth, in the same read-only layout the forward pass tests against.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = view.forwardDepthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    if (velocityCommandCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, velocityPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, velocityLayout, 0, 1, &frames[slot].frameSet[view.uniformSlot], 0,
                                nullptr);
        vkCmdBindIndexBuffer(cmd, scene->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

        const VkDeviceSize zero = 0;
        const VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
        VkBuffer sceneVb = scene->vertexBuffer();

        if (velocityStaticCount > 0) {
            vkCmdBindVertexBuffers(cmd, 0, 1, &sceneVb, &zero);
            vkCmdDrawIndexedIndirect(cmd, frames[slot].instanceData.buffer, velocityCmdRegion, velocityStaticCount,
                                     stride);
        }
        if (velocityCommandCount > velocityStaticCount && skinnedVertices.buffer != VK_NULL_HANDLE) {
            vkCmdBindVertexBuffers(cmd, 0, 1, &skinnedVertices.buffer, &zero);
            vkCmdDrawIndexedIndirect(cmd, frames[slot].instanceData.buffer,
                                     velocityCmdRegion + velocityStaticCount * stride,
                                     velocityCommandCount - velocityStaticCount, stride);
        }
    }

    vkCmdEndRendering(cmd);

    transitionImage(cmd, view.velocityTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

/// Per *pass*, not per draw: it names the matrix every draw in the indirect buffer
/// projects through. One vkCmdPushConstants before one vkCmdDrawIndexedIndirect covers a
/// whole layer.
struct ShadowPush {
    /// Ignored when `usePunctual` is 0 -- the sun has one matrix and needs no index.
    uint32_t matrixIndex;
    uint32_t usePunctual;
};

void Renderer::recordPunctualShadows(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("PunctualShadows");
    // A depth map is a function of the matrix it was rendered through and the geometry it
    // saw, so a layer whose matrix and geometry are unchanged still holds the right
    // answer. On a scene with static lights that is every layer, every frame after the
    // first -- nineteen full re-renders of Sponza producing exactly what is already
    // there. updateLights decided which layers are stale; this only acts on it.
    uint32_t dirtyCount = 0;
    for (uint32_t l = 0; l < kMaxShadowLayers; ++l) dirtyCount += punctualLayerDirty[l] ? 1u : 0u;
    punctualLayersRendered = dirtyCount;

    // Nothing to do: no barriers, no render passes. The image is already in
    // DEPTH_READ_ONLY_OPTIMAL from whichever frame last wrote it, which is the layout the
    // descriptor declares, so the lighting pass reads it exactly as it would have.
    //
    // The zone is still opened, deliberately: a pass that vanishes from the trace looks
    // to scripts/baseline.py like a broken capture, where a 0.001 ms zone is the honest
    // report of a frame that had no work to do.
    if (dirtyCount == 0 && !punctualCacheCold) {
        GpuScope zone(gpuProfiler, cmd, slot, "PunctualShadows");
        return;
    }

    // UNDEFINED *discards* the contents, which is right exactly once -- when there is
    // nothing worth keeping. Every other frame must come from DEPTH_READ_ONLY_OPTIMAL so
    // the layers this frame is not redrawing survive the transition. The difference
    // between a cache and a corrupt atlas is this one constant.
    const VkImageLayout entryLayout =
        punctualCacheCold ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    transitionImage(cmd, punctualShadowMap.image, entryLayout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);

    GpuScope zone(gpuProfiler, cmd, slot, "PunctualShadows");

    const VkExtent2D extent{kPunctualShadowSize, kPunctualShadowSize};
    // Set 2 is the game's image array (P6): a variant that shades from an image a game
    // loaded rather than from one a glTF brought. `overlaySet` is reallocated when the
    // array grows, so it is read here per frame rather than cached.
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], scene->descriptorSet(), overlaySet};
    const VkDeviceSize zero = 0;
    VkBuffer vb = scene->vertexBuffer();
    const auto layerCount = static_cast<uint32_t>(shadowMatrixScratch.size());

    // A pass per layer, for the reason recordShadows gives: a layered attachment loses
    // the per-layer depth compression that makes each of these cheap.
    for (uint32_t layer = 0; layer < kMaxShadowLayers; ++layer) {
        // A clean layer is skipped entirely -- no pass, no clear, no draws. On a cold
        // cache every layer is dirty, which clears all twenty-four so none is left
        // UNDEFINED under a descriptor the lighting pipeline has bound, and the unused
        // ones read as "nothing occludes".
        if (!punctualCacheCold && !punctualLayerDirty[layer]) continue;

        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = punctualShadowLayerViews[layer];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0}; // forward-Z, as the sun's map is

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth;

        vkCmdBeginRendering(cmd, &rendering);
        setViewportScissor(cmd, extent);

        if (layer < layerCount && frames[slot].opaqueCommandCount > 0) {
            // No pipeline bind here: drawSceneIndirect binds one per variant group, and
            // for a scene with no game variants that is the same single bind this line
            // used to make.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout, 0, 3, sets, 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, scene->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

            const ShadowPush push{layer, 1u};
            vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            // View 2 + layer: the cull dispatch wrote this layer's own command list
            // against the same projection.
            drawSceneIndirect(cmd, slot, 2 + layer, VariantPass::Shadow);
        }

        vkCmdEndRendering(cmd);
    }

    transitionImage(cmd, punctualShadowMap.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    punctualCacheCold = false;
}

void Renderer::recordShadows(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Shadows");
    GpuScope zone(gpuProfiler, cmd, slot, "Shadows");

    transitionImage(cmd, shadowMap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);

    const VkExtent2D extent{kShadowMapSize, kShadowMapSize};
    // Set 2 is the game's image array (P6): a variant that shades from an image a game
    // loaded rather than from one a glTF brought. `overlaySet` is reallocated when the
    // array grows, so it is read here per frame rather than cached.
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], scene->descriptorSet(), overlaySet};
    const VkDeviceSize zero = 0;
    VkBuffer vb = scene->vertexBuffer();

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = shadowMap.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Forward-Z, unlike the main camera: orthographic depth is spread evenly, so
    // reverse-Z buys nothing and a plain LESS compare is what the sampler wants.
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, extent);

    // With shadows off the pass still runs and still clears. Skipping it would leave
    // the map UNDEFINED for a descriptor the lighting pipeline has bound, and a clear
    // to 1.0 means "nothing occludes anything" -- the right answer anyway.
    if (shadowsEnabled && frames[slot].opaqueCommandCount > 0) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout, 0, 3, sets, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
        vkCmdBindIndexBuffer(cmd, scene->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
        const ShadowPush push{0u, 0u};
        vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        // View 1: the cull dispatch wrote the sun's command list against the same
        // orthographic box this renders with.
        drawSceneIndirect(cmd, slot, 1, VariantPass::Shadow);
    }

    vkCmdEndRendering(cmd);

    transitionImage(cmd, shadowMap.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::updateUniforms(const scene::Camera& camera, uint32_t slot) {
    auto s = core::Profiler::scope("updateUniforms");
    const float aspect = static_cast<float>(view.renderExtent.width) / static_cast<float>(view.renderExtent.height);

    FrameUniforms u{};

    // TAA jitter (3.4). A sub-pixel offset applied in *clip* space, as a translation
    // proportional to w. Nudging the projection matrix directly would be the usual
    // trick and is wrong here, because what is in hand is already a view-projection:
    // its column 2 is not the projection's, so the offset would scale with view-space
    // z rather than with w.
    //
    // **Which is also why it composes with any projection**, and why an orthographic
    // camera needed nothing here (P3): this post-multiplies a finished view-projection
    // rather than reaching into how one was built, and it leaves rows 2 and 3 -- the
    // whole of the depth mapping -- exactly as the matrix produced them.
    const glm::mat4 unjittered = camera.viewProjection(aspect);
    glm::mat4 jitter(1.0f);
    if (view.taaActive) {
        const uint32_t index = static_cast<uint32_t>(framesSubmitted % kTaaJitterCount) + 1;
        jitter[3][0] = (halton(index, 2) - 0.5f) * 2.0f / static_cast<float>(view.renderExtent.width);
        jitter[3][1] = (halton(index, 3) - 0.5f) * 2.0f / static_cast<float>(view.renderExtent.height);
    }

    u.viewProj = jitter * unjittered;
    u.invViewProj = glm::inverse(u.viewProj);
    // Last frame's, before this one overwrites it at the bottom of the function.
    u.prevViewProj = view.prevViewProj;
    u.invViewProjNoJitter = view.taaActive ? glm::inverse(unjittered) : u.invViewProj;
    u.cameraPos = glm::vec4(camera.position(), 1.0f);
    u.depthLinear = camera.depthLinear();

    u.cameraForward = glm::vec4(camera.forward(), 0.0f);

    u.sunDirection = glm::vec4(glm::normalize(sunDirection), sunIntensity);
    u.sunColor = glm::vec4(sunColorValue, 1.0f);

    // w is spare and reads as zero -- frame.glsl says so and nothing samples it. It was
    // documented here as the SSR roughness cutoff and in Renderer.h as an intensity, and
    // was neither; a row wanting a per-frame float gets its own vec4 rather than this one.
    u.ambient = glm::vec4(ambientColor, 0.0f);

    // Fits the sun's box to the scene and fills cullViewProj[1] with the same matrix,
    // along with the biases converted out of world units. Takes no camera and runs
    // whether or not shadows are on: the pass still clears the map, and a stale matrix
    // in the cull views would have view 1 drawing against last frame's box.
    updateSunShadow(u);

    // **Every view ranks its own lights** (C38): this culls against the matrix and ranks
    // against the position it is handed, so a view looking elsewhere shades what it can see
    // rather than what the presenting camera could. What it does *not* do for a secondary
    // is assign atlas layers -- see the shadow-atlas section of `updateLights` for why that
    // one half stays the primary's, and for what a light only this view chose loses.
    updateLights(slot, camera.position(), unjittered);
    const uint32_t lightCount = static_cast<uint32_t>(lightScratch.size());

    // Both of the trailing values are strengths, not switches. ENABLE_GSAA and
    // ENABLE_BLOOM decide whether the code runs; these scale it once it does.
    u.params = glm::vec4(exposure, static_cast<float>(lightCount), specularAaStrength, bloomStrength);

    // `render.lightCutoff` is stated in post-exposure radiance -- what tonemap.frag sees
    // after its `hdr *= frame.params.x` -- and the light loop compares scene-referred
    // radiance, so the division happens here rather than per light per sample. Squared to
    // meet the `dot(radiance, radiance)` the loop already computes.
    //
    // **The zero has to survive exactly**: at the default the shader's compare must be
    // against 0.0 and not against something a division rounded, or the row stops being the
    // no-op the golden set proves. An exposure of 0 renders black anyway, so guarding it
    // costs nothing and keeps a NaN out of the uniform.
    const float cutoff = lightCutoff > 0.0f && exposure > 0.0f ? lightCutoff / exposure : 0.0f;
    u.lightParams = glm::vec4(cutoff * cutoff, 0.0f, 0.0f, 0.0f);

    // flags.z: whether a *traced* reflection pass will composite this frame, which is
    // when the lighting pass yields its prefiltered specular to it. The screen-space
    // march keeps the old additive accounting, so it does not set this.
    u.flags = glm::uvec4(static_cast<uint32_t>(debugView), static_cast<uint32_t>(msaaSamples),
                         (ssrEnabled && rtActive) ? 1u : 0u,
                         camera.projectionMode == scene::Camera::Projection::Orthographic ? 1u : 0u);

    // Tiled light assignment (C35). Decided here rather than in `recordLightTiles`
    // because this is where the stride the shading passes index by is written, and the two
    // are one decision: `tileParams.z` is non-zero exactly when a dispatch will have
    // filled the buffer by the time anything reads it.
    lightTilesActive = lightTilesEnabled && lightTileWords > 0 &&
                          lightTilePipeline != VK_NULL_HANDLE && view.lightTiles.buffer != VK_NULL_HANDLE;
    u.tileParams = lightTilesActive
                          ? glm::uvec4(view.lightTileGrid.width, kLightTileSize, lightTileWords, 0u)
                          : glm::uvec4(0u);

    std::memcpy(frames[slot].uniforms[view.uniformSlot].mapped, &u, sizeof(u));

    // ------------------------------------------------------------- cull views (4.2)
    // Just the camera, since the shadow views went with the shadow system. Unjittered:
    // a sub-pixel offset cannot change what is visible, and using the jittered matrix
    // would make culling decisions flicker on the TAA period.
    cullViewProj[0] = unjittered;

    // Unjittered, because it is what next frame's TAA reprojects into: the history
    // holds the converged image, and that is aligned to pixel centres rather than to
    // any one frame's offset.
    view.prevViewProj = unjittered;
}

void Renderer::recordGBuffer(VkCommandBuffer cmd, uint32_t slot, uint32_t phase) {
    auto cpuZone = core::Profiler::scope(phase == 0 ? "GBuffer" : "GBufferLate");
    GpuScope zone(gpuProfiler, cmd, slot, phase == 0 ? "GBuffer" : "GBufferLate");

    // Phase 0 owns the transitions and the clears; phase 1 walks back into attachments
    // that already hold phase 0's result. The depth is the one that has to move both ways:
    // the pyramid read it as a texture between the two passes.
    if (phase == 0) {
        for (GpuImage* img : {&view.gAlbedo, &view.gNormal, &view.gOrm, &view.gEmissive}) {
            transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        transitionImage(cmd, view.gDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (view.gDepthResolved.image != VK_NULL_HANDLE) {
            transitionImage(cmd, view.gDepthResolved.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
    } else {
        transitionImage(cmd, view.forwardDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 0.0f}};
    std::array<VkRenderingAttachmentInfo, 4> colors{
        phase == 0 ? colorAttachment(view.gAlbedo.view, black) : colorAttachment(view.gAlbedo.view),
        phase == 0 ? colorAttachment(view.gNormal.view, black) : colorAttachment(view.gNormal.view),
        phase == 0 ? colorAttachment(view.gOrm.view, black) : colorAttachment(view.gOrm.view),
        phase == 0 ? colorAttachment(view.gEmissive.view, black) : colorAttachment(view.gEmissive.view)};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = view.gDepth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = phase == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far is 0

    // Resolve depth on store, for the forward pass to test against. Free in the sense
    // that matters: it rides the store the pass was already doing.
    if (view.gDepthResolved.image != VK_NULL_HANDLE) {
        depth.resolveMode = depthResolveMode;
        depth.resolveImageView = view.gDepthResolved.view;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    rendering.pColorAttachments = colors.data();
    rendering.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    // Set 2 is the game's image array (P6): a variant that shades from an image a game
    // loaded rather than from one a glTF brought. `overlaySet` is reallocated when the
    // array grows, so it is read here per frame rather than cached.
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], scene->descriptorSet(), overlaySet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferLayout, 0, 3, sets, 0, nullptr);

    const VkDeviceSize offset = 0;
    VkBuffer vb = scene->vertexBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, scene->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // The whole scene in one command per variant group, and the pipelines bound there
    // too. Whatever the draw count is, this loop is not one: the stats the overlay
    // reports were counted when the commands were written, and they are what the GPU
    // will execute rather than what the CPU submitted.
    drawSceneIndirect(cmd, slot, phase == 0 ? 0u : kOcclusionView, VariantPass::GBuffer);

    vkCmdEndRendering(cmd);

    // The single-sample depth -- the resolve at MSAA, gDepth itself at 1x -- is read
    // by SSAO and tested by the forward pass. Transitioning it once here rather than
    // in each consumer is what keeps those two passes free of a sample-count branch;
    // the alternative was every downstream pass having to know which image it got and
    // therefore which layout that image was already in.
    transitionImage(cmd, view.forwardDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::recordDecals(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Decals");
    GpuScope zone(gpuProfiler, cmd, slot, "Decals");

    // gAlbedo is *still a colour attachment* here: recordGBuffer transitions only the
    // depth target, and the lighting pass moves the colour ones to SHADER_READ_ONLY
    // later. So this is an execution barrier within one layout, not a transition -- the
    // decal blend has to see the G-buffer's writes, and nothing more.
    //
    // Only albedo is written. Projecting a normal map would mean blending octahedral
    // coordinates, which is not a meaningful operation across the encoding's fold, and a
    // decal that changes roughness without changing colour is a feature nobody has asked
    // for yet.
    transitionImage(cmd, view.gAlbedo.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo color = colorAttachment(view.gAlbedo.view);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalPipeline);

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], scene->descriptorSet(), view.sceneDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalLayout, 0, 3, sets, 0, nullptr);

    // One fullscreen draw per decal. See decal.frag for why this is not a box.
    for (const Decal& d : decals) {
        DecalPush push{};
        push.worldToDecal = glm::inverse(d.transform);
        push.tint = d.tint;
        push.textureIndex = d.textureIndex;
        push.edgeFade = d.edgeFade;
        push.round = d.round ? 1u : 0u;
        vkCmdPushConstants(cmd, decalLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);
    // Left as a colour attachment, exactly as it was found. recordLighting owns the
    // transition to SHADER_READ_ONLY and does not need to know this pass ran.
}

void Renderer::recordSsao(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("SSAO");
    if (!ssaoEnabled) {
        // As with bloom: the lighting set binds the AO texture whether or not this ran,
        // so it has to be in the layout that descriptor declares. The contents are
        // undefined here, which is why flags.z gates the *read* -- an earlier version
        // left the shader sampling this and disabling SSAO came out brighter than
        // enabling it, because uninitialised occlusion reads as zero and takes the
        // whole ambient term with it.
        transitionImage(cmd, view.ssaoBlurred.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }

    GpuScope zone(gpuProfiler, cmd, slot, "SSAO");

    for (GpuImage* img : {&view.ssaoRaw, &view.ssaoBlurred}) {
        transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    SsaoPush push{};
    // The pass's own texel, not the swapchain's: it runs at half resolution, and the depth
    // it samples is addressed in normalised uv so the same coordinates reach the
    // full-resolution buffer unchanged.
    push.texel = glm::vec2(1.0f / static_cast<float>(view.ssaoExtent.width),
                           1.0f / static_cast<float>(view.ssaoExtent.height));
    push.radius = ssaoRadius;
    push.bias = ssaoBias;
    push.intensity = ssaoIntensity;
    push.sampleCount = ssaoSamples;

    // Both dispatches below share these and the push constant above, so the blur follows
    // the trace onto the half-resolution grid without a second decision.
    const uint32_t groupsX = (view.ssaoExtent.width + 7) / 8;
    const uint32_t groupsY = (view.ssaoExtent.height + 7) / 8;

    const VkDescriptorSet aoSets[] = {frames[slot].frameSet[view.uniformSlot], view.ssaoSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssaoPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssaoLayout, 0, 2, aoSets, 0, nullptr);
    vkCmdPushConstants(cmd, ssaoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // The blur reads a 4x4 neighbourhood of what that dispatch wrote.
    transitionImage(cmd, view.ssaoRaw.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const VkDescriptorSet blurSets[] = {frames[slot].frameSet[view.uniformSlot], view.ssaoBlurSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssaoBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssaoLayout, 0, 2, blurSets, 0, nullptr);
    vkCmdPushConstants(cmd, ssaoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    transitionImage(cmd, view.ssaoBlurred.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void Renderer::recordGbufferRead(VkCommandBuffer cmd) {
    // **COMPUTE as well as FRAGMENT, and this is the only barrier that says so.** The
    // shadow mask and the lighting pass read these in the fragment stage, but the SSR
    // march binds the same `gbufferSet` to a compute dispatch and samples albedo,
    // normal, ORM and depth out of it -- and nothing re-barriers the G-buffer between
    // here and there, so a FRAGMENT-only destination scope leaves that read unordered
    // against this transition. Naming one stage reads as correct and orders half of it.
    constexpr VkPipelineStageFlags2 kReaders =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    // `SHADER_READ` rather than `SHADER_SAMPLED_READ`, for the reason
    // `recordDepthPyramidLayout` gives about the pyramid: a combined image sampler is
    // attributed to *storage* read, so a barrier naming only the sampled form covers none
    // of it and sync validation reports a hazard at every pass that binds this set --
    // the lighting draw, the SSR march and the tile build alike.
    constexpr VkAccessFlags2 kReadAccess = VK_ACCESS_2_SHADER_READ_BIT;

    for (GpuImage* img : {&view.gAlbedo, &view.gNormal, &view.gOrm, &view.gEmissive}) {
        transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kReaders, kReadAccess);
    }
    // At 1x, gDepth *is* forwardDepth and recordGBuffer already moved it; transitioning
    // it a second time from DEPTH_ATTACHMENT would name a layout it no longer has.
    if (msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
        transitionImage(cmd, view.gDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kReaders, kReadAccess,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

void Renderer::recordLightTiles(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("LightTiles");
    if (!lightTilesActive) return;

    GpuScope zone(gpuProfiler, cmd, slot, "LightTiles");

    // One workgroup per tile, one invocation per pixel. The dispatch and
    // `frame.tileParams` are both derived from `view.lightTileGrid` so the grid the shader
    // writes and the grid the shading passes index are the same grid.
    LightTilePush push{};
    push.extent = {view.renderExtent.width, view.renderExtent.height};
    push.tiles = {view.lightTileGrid.width, view.lightTileGrid.height};
    push.samples = static_cast<uint32_t>(msaaSamples);

    // The buffer is per view, not per frame slot, so with two frames in flight the
    // dispatch below overwrites what the *previous* frame's lighting draw is still
    // reading. A pipeline barrier's first scope covers everything already submitted to
    // this queue, so one execution dependency here closes that write-after-read; there is
    // no second copy of the buffer to make it unnecessary, exactly as there is no second
    // G-buffer.
    {
        VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        war.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        war.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        war.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        war.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &war;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.gbufferSet, view.lightTileSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightTilePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightTileLayout, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, lightTileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, view.lightTileGrid.width, view.lightTileGrid.height, 1);

    // Written by compute, read by the fragment stage of the two passes below it. A buffer
    // barrier rather than a full one so this orders the mask and nothing else -- the
    // G-buffer reads it depends on were ordered by `recordGbufferRead` already.
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = view.lightTiles.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::recordShadowMask(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("ShadowMask");

    // UNDEFINED every frame, whether or not the pass runs. Nothing reads what the last
    // frame left -- the pass rewrites every texel the lighting pass will index -- and the
    // descriptor names GENERAL, so the image has to arrive in it either way.
    //
    // STORAGE_READ as well as STORAGE_WRITE, which is what the *inactive* path needs: the
    // lighting pipeline declares this descriptor whichever way `shadowMaskActive` went, so
    // where the pass returns below the only thing this barrier is ordering is that read.
    // The AO buffer's disabled path grants its declared read for the same reason.
    transitionImage(cmd, view.shadowMask.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (!shadowMaskActive) return;

    GpuScope zone(gpuProfiler, cmd, slot, "ShadowMask");

    // No attachment at all: the fragment shader's only output is the storage image, and a
    // colour target would be a full-screen write of a value nothing reads.
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowMaskPipeline);
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.gbufferSet, tlasSet,
                                    view.shadowMaskSet, view.lightTileSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowMaskLayout, 0, 5, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // Same layout on both sides, so this is the memory dependency and nothing else: the
    // lighting pass has to see the stores this pass made.
    transitionImage(cmd, view.shadowMask.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Renderer::recordLighting(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Lighting");
    GpuScope zone(gpuProfiler, cmd, slot, "Lighting");

    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingAttachmentInfo color = colorAttachment(view.hdrTarget.view, black);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline);
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.gbufferSet, tlasSet, iblSet,
                                    view.shadowMaskSet, view.lightTileSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingLayout, 0, 6, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void Renderer::recordForward(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& camera) {
    auto cpuZone = core::Profiler::scope("Forward");
    // The sort and the commands were written before recording began; this pass only
    // submits them. `camera` stays in the signature because the pass is conceptually
    // view-dependent and hiding that would be worse than one unused parameter.
    (void)camera;
    if (blendedCommandCount == 0) return;

    GpuScope zone(gpuProfiler, cmd, slot, "Forward");

    // hdrTarget already holds the lit opaque scene and stays in COLOR_ATTACHMENT_
    // OPTIMAL; this is a write-after-write against the lighting pass, so it needs the
    // dependency even though the layout does not change.
    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    // recordGBuffer left this in DEPTH_READ_ONLY and SSAO may have sampled it since;
    // this is the execution dependency for the depth test, not a layout change.

    // blend over the lit scene
    VkRenderingAttachmentInfo color = colorAttachment(view.hdrTarget.view);

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = view.forwardDepthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // read-only: nothing to write back

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], scene->descriptorSet(), tlasSet, iblSet,
                                    view.sceneDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardLayout, 0, 5, sets, 0, nullptr);

    const VkDeviceSize offset = 0;
    VkBuffer vb = scene->vertexBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, scene->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // In depth order, because that is the order buildBlendedCommands() wrote them in.
    // An indirect draw executes its commands in buffer order, which is what makes a
    // sorted pass expressible as one submission at all -- and why the variant runs here
    // are walked in that same order rather than gathered by pipeline: reordering them to
    // save a bind would put a far surface over a near one.
    uint32_t boundVariant = UINT32_MAX;
    for (const VariantRange& r : blendedRanges) {
        if (r.variant != boundVariant) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, variantPipeline(r.variant, VariantPass::Forward));
            boundVariant = r.variant;
        }
        vkCmdDrawIndexedIndirect(cmd, frames[slot].instanceData.buffer,
                                 blendedRegion + static_cast<VkDeviceSize>(r.first) *
                                                     sizeof(VkDrawIndexedIndirectCommand),
                                 r.count, sizeof(VkDrawIndexedIndirectCommand));
    }

    vkCmdEndRendering(cmd);
}

void Renderer::recordBloom(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Bloom");
    if (!bloomEnabled) {
        // Not a no-op, even though nothing reads the result. The tonemap set binds the
        // chain unconditionally, and `params.w == 0` skips the sample at runtime
        // rather than statically -- so the descriptor is still "used" as far as
        // validation is concerned and must point at an image in the layout it
        // declares. UNDEFINED as the source discards contents nobody is going to read.
        transitionImage(cmd, view.bloomChain.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, kBloomMips);
        return;
    }

    GpuScope zone(gpuProfiler, cmd, slot, "Bloom");

    // Every mip goes to GENERAL and stays there for the whole chain. Alternating
    // between GENERAL and SHADER_READ_ONLY per step would be one barrier per mip per
    // direction for no benefit: GENERAL is a valid layout for both the storage writes
    // and the sampled reads this pass does.
    transitionImage(cmd, view.bloomChain.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, kBloomMips);

    // hdrTarget was left in COLOR_ATTACHMENT_OPTIMAL by lighting or the forward pass.
    //
    // FRAGMENT too, because this transition is the *only* one hdrTarget gets before the
    // tonemap draw samples it -- recordTonemap deliberately re-barriers it only on the
    // `!bloomEnabled` path, on the argument that bloom already moved it. Bloom moved the
    // layout; a COMPUTE-only destination scope does not order the fragment read that
    // follows.
    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Each step reads what the previous one wrote, so every dispatch needs the write
    // to have landed and be visible. One image barrier over the whole chain is simpler
    // than tracking which mip each step touched, and the chain is five small images.
    auto chainBarrier = [&] {
        transitionImage(cmd, view.bloomChain.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, 0, kBloomMips);
    };

    // Mip m of a half-resolution chain, floor-divided and floored at 1 -- the same
    // rule vkCreateImage used to size the levels, so the dispatch matches the image.
    auto mipExtent = [&](uint32_t m) {
        return VkExtent2D{std::max(1u, view.bloomChain.extent.width >> m), std::max(1u, view.bloomChain.extent.height >> m)};
    };

    auto dispatch = [&](VkPipeline pipeline, VkDescriptorSet set, uint32_t dstMip, float srcLod) {
        const VkExtent2D dst = mipExtent(dstMip);
        BloomPush push{};
        push.dstTexel = glm::vec2(1.0f / static_cast<float>(dst.width), 1.0f / static_cast<float>(dst.height));
        push.srcLod = srcLod;
        push.threshold = bloomThreshold;
        push.knee = bloomSoftKnee;
        push.strength = bloomStrength;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, bloomLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);
    };

    // Down: threshold the HDR target into mip 0, then halve repeatedly.
    dispatch(bloomThresholdPipeline, view.bloomDownSets[0], 0, 0.0f);
    for (uint32_t m = 1; m < kBloomMips; ++m) {
        chainBarrier();
        dispatch(bloomDownPipeline, view.bloomDownSets[m], m, static_cast<float>(m - 1));
    }

    // Up: add each mip back onto the one above it, so mip 0 ends up holding the sum of
    // every level. Progressive, not a single wide blur -- that is what gives bloom a
    // falloff with both a tight core and a long tail.
    for (uint32_t m = kBloomMips - 1; m-- > 0;) {
        chainBarrier();
        dispatch(bloomUpPipeline, view.bloomUpSets[m], m, static_cast<float>(m + 1));
    }

    // Hand mip 0 to the tonemap pass as a sampled image.
    transitionImage(cmd, view.bloomChain.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, kBloomMips);
}

void Renderer::recordSsr(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("SSR");
    GpuScope zone(gpuProfiler, cmd, slot, "SSR");

    // hdrTarget arrives as a colour attachment from lighting or the forward pass, and
    // leaves as one, so nothing downstream has to know this ran. In between it is the
    // image a reflection ray samples.
    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    transitionImage(cmd, view.ssrTarget.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    SsrPush push{};
    // The *trace's* grid, not the frame's: `ssr_body.glsl` turns this back into a 0..1 uv
    // and the G-buffer coordinate it fetches comes from that uv, so a texel taken from
    // renderExtent at a reduced ssrExtent would march the top-left corner of the frame.
    push.texel = glm::vec2(1.0f / static_cast<float>(view.ssrExtent.width),
                           1.0f / static_cast<float>(view.ssrExtent.height));
    // The two techniques bound their reach with different numbers because they pay for
    // it differently. `ssrMaxDistance` divides into a fixed step count, so raising it
    // coarsens the march; a ray query costs the same whatever the range, so it gets one
    // large enough to reach the far wall. Sharing the march budget is what confined a
    // ray-traced reflection to an 8-metre bubble and made everything past it read as sky.
    push.maxDistance = rtActive ? rtMaxDistance : ssrMaxDistance;
    push.thickness = ssrThickness;
    push.intensity = ssrIntensity;
    push.roughnessCutoff = ssrRoughnessCutoff;
    push.stepCount = ssrSteps;
    push.refineSteps = ssrRefineSteps;
    // Always, when this pass is the traced one at all: `rtActive` already gated the
    // shader variant, and the lighting pass traces on exactly the same condition. There is
    // no second toggle to disagree with.
    push.shadowLights = rtActive ? 1u : 0u;
    push.hitRecords = accel.hitRecordAddress;
    push.sceneVertices = accel.sceneVertexAddress;
    push.sceneIndices = accel.sceneIndexAddress;
    // Null when the scene has no rig at all. Legal to leave so: a hit record only names
    // the deformed buffers when its own geometry lives in them, and none does here.
    push.deformedVertices = accel.deformedVertexAddress;
    push.deformedIndices = accel.dynamicIndexAddress;

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.gbufferSet, view.ssrImageSet,
                                    tlasSet,             iblSet,     scene->descriptorSet()};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssrPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssrLayout, 0, rtActive ? 6u : 3u, sets,
                            0, nullptr);
    vkCmdPushConstants(cmd, ssrLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (view.ssrExtent.width + 7) / 8, (view.ssrExtent.height + 7) / 8, 1);

    transitionImage(cmd, view.ssrTarget.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    // LOAD and additive blend: this adds reflected radiance to what is already lit.
    VkRenderingAttachmentInfo color = colorAttachment(view.hdrTarget.view);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);
    // Two pipelines rather than one shader that tests its own texture size, because the
    // upsampling one needs the G-buffer set bound and the full-resolution one must stay
    // the draw it was -- the same reason `ssr.comp` and `ssr1x.comp` are two files.
    if (view.ssrExtent.width == view.renderExtent.width && view.ssrExtent.height == view.renderExtent.height) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrCompositeLayout, 0, 1,
                                &view.ssrCompositeSet, 0, nullptr);
    } else {
        const VkDescriptorSet upsampleSets[] = {view.gbufferSet, view.ssrCompositeSet};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrUpsamplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrUpsampleLayout, 0, 2, upsampleSets, 0,
                                nullptr);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void Renderer::recordFog(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("Fog");
    GpuScope zone(gpuProfiler, cmd, slot, "Fog");

    transitionImage(cmd, view.fogTarget.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    FogPush push{};
    push.texel = glm::vec2(1.0f / static_cast<float>(view.renderExtent.width), 1.0f / static_cast<float>(view.renderExtent.height));
    push.density = fogDensity;
    push.anisotropy = glm::clamp(fogAnisotropy, -0.95f, 0.95f);
    push.maxDistance = fogMaxDistance;
    push.heightFalloff = fogHeightFalloff;
    push.baseHeight = fogBaseHeight;
    push.stepCount = fogSteps;

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.fogImageSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, fogLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (view.renderExtent.width + 7) / 8, (view.renderExtent.height + 7) / 8, 1);

    transitionImage(cmd, view.fogTarget.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // hdrTarget is still a colour attachment here -- recordSsr leaves it as one and so
    // does the lighting pass -- so the composite needs no transition, only the blend.
    VkRenderingAttachmentInfo color = colorAttachment(view.hdrTarget.view);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fogCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrCompositeLayout, 0, 1, &view.fogCompositeSet, 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void Renderer::recordTaa(VkCommandBuffer cmd, uint32_t slot) {
    auto cpuZone = core::Profiler::scope("TAA");
    GpuScope zone(gpuProfiler, cmd, slot, "TAA");

    // hdrTarget arrives in SHADER_READ_ONLY: bloom put it there to threshold it, and
    // recordTonemap does the same when bloom is off. Either way this pass only reads
    // it, so there is nothing to transition.
    const uint32_t p = view.taaHistoryIndex;

    // The destination goes to GENERAL from UNDEFINED -- its previous contents are two
    // frames old and about to be entirely overwritten -- while the source stays in
    // SHADER_READ_ONLY from when it was written.
    transitionImage(cmd, view.taaHistory[p].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // The history read on the very first frame would be of an image nothing has
    // written. `valid` gates that in the shader, but the descriptor still has to point
    // at an image in the layout it declares, so give it one.
    if (!view.taaHistoryValid) {
        transitionImage(cmd, view.taaHistory[1 - p].image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    TaaPush push{};
    push.texel = glm::vec2(1.0f / static_cast<float>(view.renderExtent.width), 1.0f / static_cast<float>(view.renderExtent.height));
    push.blend = glm::clamp(taaBlend, 0.01f, 1.0f);
    push.valid = view.taaHistoryValid ? 1.0f : 0.0f;

    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot], view.taaSet[p]};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, taaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (view.renderExtent.width + 7) / 8, (view.renderExtent.height + 7) / 8, 1);

    // Hand it to the tonemap pass, and to next frame's resolve as history.
    transitionImage(cmd, view.taaHistory[p].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    view.taaHistoryValid = true;
}

void Renderer::recordViewChain(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& camera,
                               uint32_t imageIndex) {
    // The scene-wide cull already ran, before the shadow passes, filling every list from
    // the primary's matrices. A chain re-fills **list 0 only**, and only when that list is
    // not already this view's: a secondary view always, and the primary exactly when a
    // secondary overwrote it. In a one-view frame this records nothing at all, which is
    // what keeps the frame identical to the one before this row.
    if (!view.primary || !extraViews.empty()) {
        recordDepthPyramidLayout(cmd, slot);
        recordCull(cmd, slot, 0, CullViews::Camera);
    }

    // C11's two passes, and the reason they are two. Phase 0 draws what the camera
    // could see last frame -- a good guess, and one that costs nothing to be wrong
    // about. The pyramid is then built from *that* depth, phase 1 tests everything
    // against it, and whatever is newly visible is drawn into the same attachments.
    //
    // Nothing visible is ever dropped: a command that fails the phase 0 guess is
    // re-tested against real current-frame depth before it can be discarded. That is
    // the whole argument for the shape, and it is why the golden images did not move.
    //
    // **The visibility buffer the guess comes from is shared between views**, so a second
    // view's phase 0 starts from what the *previous* chain could see. That costs a phase 1
    // that admits more than it otherwise would, and it cannot drop anything: phase 1 is
    // what decides, and it tests against this view's own depth.
    recordGBuffer(cmd, slot, 0);
    recordDepthPyramid(cmd, slot);
    recordCull(cmd, slot, 1);
    recordGBuffer(cmd, slot, 1);
    // Straight after the G-buffer and before anything reads it: a decal is part of
    // the surface description, so SSAO, lighting and reflections should all see it.
    if (!decals.empty()) recordDecals(cmd, slot);
    // Before lighting: the AO it produces is an input to the ambient term.
    recordSsao(cmd, slot);
    // Anywhere after the G-buffer's depth resolve and before TAA reads it. Here
    // rather than immediately before `recordTaa` because it is a geometry pass and
    // this is where the geometry passes are -- and because the depth it tests
    // against is at its freshest, with only SSAO having sampled it since.
    if (view.taaActive) recordVelocity(cmd, slot);
    // The G-buffer becomes readable once, for both of the passes below: the shadow mask
    // samples the same five attachments the lighting pass does, and whichever ran second
    // would name a layout the first had already moved it out of.
    recordGbufferRead(cmd);
    // After the G-buffer becomes readable, because the tile depth bounds are the depth
    // this frame actually drew, and before both passes below, which are the two that read
    // the bits. Its own dispatch rather than a prologue to the lighting draw: the bounds
    // are a reduction over a tile and the shading is per pixel, so one has to finish
    // before the other starts (C35).
    recordLightTiles(cmd, slot);
    recordShadowMask(cmd, slot);
    recordLighting(cmd, slot);
    // After lighting, before tonemap: blended surfaces need the lit opaque scene
    // underneath them to blend against, and they need to be tonemapped with it
    // rather than composited into an already-display-referred image.
    recordForward(cmd, slot, camera);
    // After the forward pass, for the same reason it runs after lighting: a
    // particle blends over whatever is already in the target, and blended geometry
    // is part of "already". Before SSR, so a reflection can catch a plume of smoke,
    // and before bloom, so an emissive spark glares (S3).
    recordParticles(cmd, slot);
    // After the forward pass, so a reflection ray can sample blended surfaces, and
    // before bloom, so a bright reflection glares like the thing it reflects.
    if (ssrEnabled) recordSsr(cmd, slot);
    // After SSR, so a reflection is fogged by the media in front of it rather than
    // the other way round, and still before bloom.
    if (fogEnabled) recordFog(cmd, slot);
    recordBloom(cmd, slot);
    // After bloom, not before. Bloom is a wide, heavily blurred effect, so feeding
    // it the un-antialiased image costs nothing visible -- and running TAA last
    // means only the tonemap pass has to know which image the resolve wrote,
    // rather than the bloom chain having to ping-pong its source descriptor too.
    if (view.taaActive) recordTaa(cmd, slot);
    // Into `view.destination` where the view has one, and into the compose surface where
    // it does not. `imageIndex` is carried this far and used by neither case that names an
    // image of its own -- it is the swapchain's answer, and only the presenting view asks.
    recordTonemap(cmd, slot, imageIndex);
}

void Renderer::recordViewBarrier(VkCommandBuffer cmd) {
    // Every target is shared between chains, so this is a full stop rather than a list of
    // per-image transitions: the next chain's first pass writes gAlbedo, gDepth and the
    // rest while this one's tonemap may still be sampling the HDR image, and its cull
    // dispatch overwrites the command list the previous chain's indirect draws read.
    //
    // Enumerating the pairs would be seventeen barriers that have to stay in step with
    // `createRenderTargets`, to save one pipeline stall per view on a path that already
    // costs a full frame of GPU work. Not a trade worth making, and stated so nobody
    // "optimises" it without measuring the whole-frame number first.
    VkMemoryBarrier2 all{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    all.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    all.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    all.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    all.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &all;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::recordTonemap(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex) {
    auto cpuZone = core::Profiler::scope("Tonemap");
    GpuScope zone(gpuProfiler, cmd, slot, "Tonemap");

    // The bloom pass already moved hdrTarget to SHADER_READ_ONLY to sample it, so this
    // reads the same member it did rather than a layout tracked in a variable. Both
    // run once per frame off the same flag; they cannot disagree.
    if (!bloomEnabled) {
        transitionImage(cmd, view.hdrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // The compose surface, which is the offscreen presentation target or the swapchain
    // image itself (P2). UNDEFINED as the old layout either way: this pass CLEARs and
    // covers every texel of it, so nothing in it is worth preserving across the frame.
    transitionImage(cmd, composeImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingAttachmentInfo color = colorAttachment(composeView(imageIndex), black);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, view.renderExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, view.renderExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline);
    // With TAA on, the resolved image is whichever history the resolve just wrote, so
    // the tonemap reads that set instead. Same layout, one binding different -- which
    // is why the resolve can ping-pong without copying itself back into hdrTarget.
    const VkDescriptorSet sets[] = {frames[slot].frameSet[view.uniformSlot],
                                    view.taaActive ? view.taaOutputSet[view.taaHistoryIndex] : view.hdrSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // The image stays a colour attachment: the overlay may still draw into it, and the
    // transition that follows is either recordPresent's to TRANSFER_SRC or, where the
    // blit was elided, drawFrame's single one to PRESENT_SRC.
}

void Renderer::recordPresent(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex) {
    auto cpuZone = core::Profiler::scope("Present");
    // Nothing to do at native: the tonemap and the overlay already drew into the
    // swapchain image, and it is already a colour attachment in the layout the tail of
    // drawFrame expects. This is the elision the whole design is arranged around, and it
    // is one test rather than a mode.
    if (view.presentTarget.image == VK_NULL_HANDLE) return;
    if (view.presentPlan.width == 0 || view.presentPlan.height == 0) return;

    GpuScope zone(gpuProfiler, cmd, slot, "Present");

    transitionImage(cmd, view.presentTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
    transitionImage(cmd, swap.images[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

    // The bars. Cleared over the whole image and then blitted over, rather than four
    // rectangles around the destination: a full clear is one command and cannot leave a
    // seam, and the alternative is four VkClearRect computations that are empty in the
    // common case and wrong in exactly the case they exist for. It also means a resize
    // that shrinks the presented rectangle cannot leave last frame's pixels in the gap.
    //
    // Skipped where the destination covers the window, which is every exact multiple.
    const bool letterboxed = view.presentPlan.x != 0 || view.presentPlan.y != 0 ||
                             view.presentPlan.width != swap.extent.width || view.presentPlan.height != swap.extent.height;
    if (letterboxed) {
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, swap.images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

        // Between the clear and the blit, because both write the same image and the blit
        // overlaps the region the clear just touched.
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // NEAREST, and the destination is the source times a whole number -- `presentLayout`
    // guarantees that and a unit test pins it. Every destination texel therefore maps to
    // exactly one source texel and there is nothing for the filter to interpolate, which
    // is what "a texel authored is a texel presented" reduces to at this level.
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {static_cast<int32_t>(view.presentPlan.srcX), static_cast<int32_t>(view.presentPlan.srcY), 0};
    region.srcOffsets[1] = {static_cast<int32_t>(view.presentPlan.srcX + view.presentPlan.srcWidth),
                            static_cast<int32_t>(view.presentPlan.srcY + view.presentPlan.srcHeight), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {view.presentPlan.x, view.presentPlan.y, 0};
    region.dstOffsets[1] = {view.presentPlan.x + static_cast<int32_t>(view.presentPlan.width),
                            view.presentPlan.y + static_cast<int32_t>(view.presentPlan.height), 1};

    VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit.srcImage = view.presentTarget.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = swap.images[imageIndex];
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    blit.filter = VK_FILTER_NEAREST;
    vkCmdBlitImage2(cmd, &blit);

    // Back to a colour attachment, which is the layout the rest of drawFrame is written
    // against: the overlay may still draw over the letterbox, the capture copies from it,
    // and the final transition to PRESENT_SRC names it as the old layout.
    transitionImage(cmd, swap.images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void Renderer::recordDebugLines(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent,
                                const scene::Camera& camera) {
    auto cpuZone = core::Profiler::scope("DebugLines");
    GpuScope zone(gpuProfiler, cmd, slot, "DebugLines");

    size_t count = debugLines.size();
    if (count > kMaxDebugLineVertices) {
        // Once per run, not per frame, for the reason the overlay's cap gives: a warning
        // at 60 Hz drowns the log it is trying to appear in.
        static bool warned = false;
        if (!warned) {
            warned = true;
            core::Logger::warn(core::LogCategory::Render,
                         "Debug lines exceeded %u vertices (%zu wanted); the excess is not drawn",
                         kMaxDebugLineVertices, debugLines.size());
        }
        count = kMaxDebugLineVertices;
    }
    // A line list, so an odd count would draw a vertex with whatever followed it in the
    // buffer. Rounding down loses the half-line the cap cut in two, which is the only
    // way it can happen.
    count &= ~static_cast<size_t>(1);
    if (count == 0) return;

    std::memcpy(frames[slot].debugLineVertices.mapped, debugLines.data(), count * sizeof(DebugLineVertex));

    // LOAD, like the overlay: this draws over the tonemapped image already there.
    VkRenderingAttachmentInfo color = colorAttachment(target);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, extent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debugLinePipeline);

    // The *unjittered* view-projection, which is what the camera is actually looking
    // through. Using the jittered one would make a wireframe wander by a sub-pixel every
    // frame under TAA -- against a history that has already converged, so the lines
    // would be the one thing on screen that shimmered.
    // `renderExtent` rather than the surface this pass draws onto, and they are the same
    // thing because this pass always draws into the virtual target: a wireframe is
    // world-space geometry, so it belongs with the world and scales with it. Only the
    // overlay -- text and widgets, which are screen-space by construction -- gets a
    // choice, and that choice is `uiInsideVirtual`.
    const float aspect = static_cast<float>(view.renderExtent.width) / static_cast<float>(view.renderExtent.height);
    const glm::mat4 viewProj = camera.viewProjection(aspect);
    vkCmdPushConstants(cmd, debugLineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewProj), &viewProj);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frames[slot].debugLineVertices.buffer, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void Renderer::recordOverlay(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent) {
    auto cpuZone = core::Profiler::scope("Overlay");
    GpuScope zone(gpuProfiler, cmd, slot, "Overlay");

    // ------------------------------------------------------------------ the text
    const float margin = 8.0f;
    const float lineHeight = debugFont.lineHeight();
    uint32_t row = 0;

    const auto emit = [&](const char* text) {
        const float baseline = margin + debugFont.ascent() + static_cast<float>(row) * lineHeight;
        ++row;
        // Shadow first so the text blends over it. One pixel down-right is enough to
        // keep white text readable against Sponza's white marble.
        ui::appendText(overlayScratch, debugFont.metrics(), margin + 1.0f, baseline + 1.0f, text, kOverlayShadow);
        ui::appendText(overlayScratch, debugFont.metrics(), margin, baseline, text, kOverlayWhite);
    };

    overlayScratch.clear();

    // P2's readback, and it goes in first so nothing else in this pass can land on top of
    // it. At exact texel coordinates, tinted white, from the top-left corner: the whole
    // point is that the bytes between the source file and the swapchain are untouched, so
    // there is nothing here for the theme, the UI scale or the layout to contribute.
    if (readbackSlot != 0 && readbackWidth != 0) {
        ui::DrawList quad;
        quad.reset({0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
        quad.whiteU = uiDrawList != nullptr ? uiDrawList->whiteU : 0.0f;
        quad.whiteV = uiDrawList != nullptr ? uiDrawList->whiteV : 0.0f;
        quad.image({0.0f, 0.0f, static_cast<float>(readbackWidth), static_cast<float>(readbackHeight)}, readbackSlot);
        quad.finish();
        overlayScratch.insert(overlayScratch.end(), quad.vertices().begin(), quad.vertices().end());
    }

    if (debugOverlay) {
        char line[3][96];
        // `wall` is what FPS is derived from and what the GPU paces; `cpu` is wall less
        // the frame's GPU blocks. They part company exactly when you are GPU-bound, so
        // showing only one of them is what made the CPU look like it tracked the GPU.
        std::snprintf(line[0], sizeof(line[0]), "FPS %6.1f   wall %6.3f   cpu %6.3f   gpu %6.3f ms",
                      avgWallMs > 0.0 ? 1000.0 / avgWallMs : 0.0, avgWallMs, avgCpuBusyMs, avgGpuMs);
        // `vis` is what survived culling, from the last completed frame in this slot,
        // and it is the number that moves when the camera turns -- `prims` is the scene
        // and does not.
        // `tris` is the scene's triangles; `drawn` is what the cull left after LOD
        // selection, which is the pair that says whether a chain is doing anything.
        std::snprintf(line[1], sizeof(line[1]), "draws %u   prims %u   vis %u   tris %llu   drawn %u", stats.drawCalls,
                      stats.primitives, view.visibleInstances, static_cast<unsigned long long>(stats.triangles),
                      view.visibleTriangles);
        // Both resolutions where they differ, because a virtual-resolution run that
        // reported only the window would be reporting the one number that is not what
        // anything was rendered at.
        if (view.presentTarget.image != VK_NULL_HANDLE) {
            std::snprintf(line[2], sizeof(line[2]), "%ux MSAA   %ux%u -> %ux%u @%ux", static_cast<uint32_t>(msaaSamples),
                          view.renderExtent.width, view.renderExtent.height, swap.extent.width, swap.extent.height,
                          view.presentPlan.scale);
        } else {
            std::snprintf(line[2], sizeof(line[2]), "%ux MSAA   %ux%u", static_cast<uint32_t>(msaaSamples),
                          swap.extent.width, swap.extent.height);
        }
        for (const auto& text : line) emit(text);

        // Where the camera is, in the form `--camera` takes back. A rendering artefact
        // reported as "it happens when I walk forward here" is a bug report nobody can
        // act on; the same one with six numbers under it is a command line. Filled by
        // the application, because the renderer has no camera of its own.
        if (!cameraLine.empty()) emit(cameraLine.c_str());

        // Only where there are any. A permanent "particles 0" on every scene in the
        // repository would be a line of HUD that never says anything.
        if (particleCapacity > 0) {
            char particleLine[96];
            std::snprintf(particleLine, sizeof(particleLine), "particles %u / %u", stats.particles, particleCapacity);
            emit(particleLine);
        }

        // Same rule (P4): a permanent "sprites 0" on every 3D scene in the repository
        // would be a line of HUD that never says anything.
        if (stats.sprites > 0) {
            char spriteLine[96];
            std::snprintf(spriteLine, sizeof(spriteLine), "sprites %u in %u layer%s", stats.sprites,
                          sprites != nullptr ? sprites->layerCount() : 0u,
                          sprites != nullptr && sprites->layerCount() == 1 ? "" : "s");
            emit(spriteLine);
        }
    }

    // Application text (S1.4's binding menu is the first caller). A blank line keeps it
    // off the stats when both are on.
    if (!overlayLines.empty()) {
        if (debugOverlay) ++row;
        for (const std::string& text : overlayLines) emit(text.c_str());
    }

    // ------------------------------------------------------------------- the UI (S6)
    // Appended after the stats, so a panel draws over them rather than under: the text is
    // the frame's furniture and the UI is what the user is pointing at. `uiFirstVertex`
    // is where the two split, because the stats are drawn unclipped in one call and the
    // UI needs a scissor per command.
    const auto uiFirstVertex = static_cast<uint32_t>(overlayScratch.size());
    if (uiDrawList != nullptr && !uiDrawList->empty()) {
        const std::vector<OverlayVertex>& src = uiDrawList->vertices();
        overlayScratch.insert(overlayScratch.end(), src.begin(), src.end());
    }

    const size_t maxVertices = static_cast<size_t>(kMaxOverlayQuads) * 6;
    if (overlayScratch.size() > maxVertices) {
        // Once per run, not per frame: a warning at 60 Hz drowns the log it is trying
        // to appear in. Silence was the old behaviour and is the thing being fixed --
        // a menu cut off at the cap looks exactly like a menu that ends there.
        static bool warned = false;
        if (!warned) {
            warned = true;
            core::Logger::warn(core::LogCategory::Render, "Overlay exceeded %u quads (%zu wanted); the excess is not drawn",
                         kMaxOverlayQuads, overlayScratch.size() / 6);
        }
        overlayScratch.resize(maxVertices);
    }
    if (overlayScratch.empty()) return;

    std::memcpy(frames[slot].overlayVertices.mapped, overlayScratch.data(),
                overlayScratch.size() * sizeof(OverlayVertex));

    // ------------------------------------------------------------------ the draw
    // LOAD, not CLEAR: this composites onto the tonemapped image that is already there.
    VkRenderingAttachmentInfo color = colorAttachment(target);

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    setViewportScissor(cmd, extent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayLayout, 0, 1, &overlaySet, 0, nullptr);

    const OverlayPush push{1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
    vkCmdPushConstants(cmd, overlayLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frames[slot].overlayVertices.buffer, &offset);

    const auto uploaded = static_cast<uint32_t>(overlayScratch.size());

    // The stats and the application's text: unclipped, one call, exactly as before S6.
    if (uiFirstVertex > 0) vkCmdDraw(cmd, std::min(uiFirstVertex, uploaded), 1, 0, 0);

    // Then the UI, one call per clip rectangle. A scissor per command rather than a
    // per-vertex clip test is what makes a scrolling list cost nothing to clip: the
    // hardware discards the fragments and the vertices are submitted either way.
    if (uiDrawList != nullptr && uiFirstVertex < uploaded) {
        for (const ui::DrawCommand& command : uiDrawList->commands()) {
            if (command.vertexCount == 0) continue;
            const uint32_t first = uiFirstVertex + command.firstVertex;
            if (first >= uploaded) break;
            const uint32_t count = std::min(command.vertexCount, uploaded - first);

            // Clamped to the framebuffer, and that clamp is required rather than tidy:
            // VkRect2D offsets are signed but a scissor outside the render area is a
            // validation error, and a panel dragged off the left edge produces one.
            const float x0 = std::max(command.clip.x, 0.0f);
            const float y0 = std::max(command.clip.y, 0.0f);
            const float x1 = std::min(command.clip.z, static_cast<float>(extent.width));
            const float y1 = std::min(command.clip.w, static_cast<float>(extent.height));
            if (x1 <= x0 || y1 <= y0) continue;

            VkRect2D scissor{{static_cast<int32_t>(x0), static_cast<int32_t>(y0)},
                             {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdDraw(cmd, count, 1, first, 0);
        }
    }

    vkCmdEndRendering(cmd);
}

glm::uvec2 Renderer::imageSize(ImageId id) {
    if (images == nullptr) return {0u, 0u};

    // Residency first: the table hands out a handle immediately and the renderer catches
    // up at the top of the next `drawFrame`, so a caller that loaded in `Game::init` and
    // asked here would otherwise find no `GpuImage` behind a perfectly valid slot.
    syncImages();
    // After it, because it writes into the descriptor array `syncImages` sizes.
    syncViews();

    const uint32_t slot = images->slot(id);
    if (slot == ImageTable::kFallbackSlot || slot >= overlayImages.size()) return {0u, 0u};
    return {overlayImages[slot].extent.width, overlayImages[slot].extent.height};
}

void Renderer::setReadbackImage(ImageId id) {
    readbackSlot = 0;
    readbackWidth = 0;
    readbackHeight = 0;
    if (images == nullptr) return;

    const glm::uvec2 size = imageSize(id);
    const uint32_t slot = images->slot(id);
    if (size.x == 0 || size.y == 0) {
        core::Logger::error(core::LogCategory::Render, "Readback: no resident image for that handle");
        return;
    }
    readbackSlot = slot;
    readbackWidth = size.x;
    readbackHeight = size.y;
    core::Logger::status(core::LogCategory::Render, "Readback: drawing slot %u at 1:1, %ux%u texels", slot,
                         readbackWidth, readbackHeight);
}

glm::vec2 Renderer::renderTargetFromWindow(glm::vec2 windowPixel) const {
    if (view.presentTarget.image == VK_NULL_HANDLE || view.presentPlan.scale == 0) return windowPixel;

    const auto scale = static_cast<float>(view.presentPlan.scale);
    return {(windowPixel.x - static_cast<float>(view.presentPlan.x)) / scale + static_cast<float>(view.presentPlan.srcX),
            (windowPixel.y - static_cast<float>(view.presentPlan.y)) / scale + static_cast<float>(view.presentPlan.srcY)};
}

glm::vec2 Renderer::uiFromWindow(glm::vec2 windowPixel) const {
    // Identity wherever the UI is drawn at the window's own resolution, which is every
    // native run and every run that put the UI outside the virtual target. Written as an
    // early return rather than as arithmetic that happens to reduce to one: `presentPlan`
    // is the identity in the native case and dividing by a scale of 1 would work, but the
    // UI-outside case has a real scale and must still not be transformed.
    //
    // **The gate is this function's and not the transform's.** A picking ray goes through
    // `renderTargetFromWindow` unconditionally -- the scene is always drawn into the
    // virtual target, whatever the UI was laid out against.
    if (!uiInsideVirtual) return windowPixel;
    return renderTargetFromWindow(windowPixel);
}

void Renderer::requestCapture(std::filesystem::path path) {
    if (!swap.captureSupported) {
        core::Logger::error(core::LogCategory::Render, "Capture: the surface does not allow TRANSFER_SRC on swapchain images");
        return;
    }
    capturePath = std::move(path);
    captureRequested = true;
}

bool Renderer::beginCapture() {
    captureRequested = false;

    const uint32_t bpp = captureBytesPerPixel(swap.format);
    if (bpp == 0) {
        core::Logger::error(core::LogCategory::Render, "Capture: swapchain format %d cannot be written as a PNG",
                      static_cast<int>(swap.format));
        return false;
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(swap.extent.width) * swap.extent.height * bpp;
    captureStaging = createBuffer(*ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (captureStaging.mapped == nullptr) {
        core::Logger::error(core::LogCategory::Render, "Capture: readback buffer is not host-visible");
        destroyBuffer(*ctx, captureStaging);
        return false;
    }
    return true;
}

std::vector<Renderer::TargetEntry> Renderer::captureTargets() const {
    // Read top to bottom: name, image, the layout it is in when the frame's passes have
    // finished, its aspect, and whether the pass that fills it ran this frame.
    //
    // The G-buffer is deliberately absent. At MSAA those images are multisampled and
    // vkCmdCopyImageToBuffer cannot read one; at 1x they are reachable through
    // --debug-view, which is the resolved, tonemapped view of them that is actually
    // worth looking at. What is here is the complement: everything no debug view
    // exposes.
    const bool ssrLive = ssrEnabled && ctx != nullptr;
    return {
        {"ssaoRaw", &view.ssaoRaw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, ssaoEnabled},
        {"ssaoBlurred", &view.ssaoBlurred, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         ssaoEnabled},
        {"hdrTarget", &view.hdrTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, true},
        {"bloomChain", &view.bloomChain, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         bloomEnabled},
        {"ssrTarget", &view.ssrTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, ssrLive},
        {"fogTarget", &view.fogTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, fogEnabled},
        // Signed, and mostly near zero, so the PNG of it is a mid grey with the moving
        // objects picked out. That is the point: a black frame here is the pass having
        // written nothing, which is exactly the failure it is hardest to see in the
        // final image.
        {"velocityTarget", &view.velocityTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         taaEnabled},
        {"taaHistory0", &view.taaHistory[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         taaEnabled},
        {"taaHistory1", &view.taaHistory[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         taaEnabled},
        // The IBL set is baked once at load and never transitioned again, so it is live
        // whenever it was built at all.
        {"envCube", &envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         envCube.image != VK_NULL_HANDLE},
        {"irradianceCube", &irradianceCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         irradianceCube.image != VK_NULL_HANDLE},
        {"prefilteredCube", &prefilteredCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         prefilteredCube.image != VK_NULL_HANDLE},
        {"brdfLut", &brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
         brdfLut.image != VK_NULL_HANDLE},
    };
}

std::vector<std::string> Renderer::captureTargetNames() const {
    std::vector<std::string> names;
    for (const TargetEntry& e : captureTargets()) {
        names.emplace_back(e.live ? e.name : std::string(e.name) + " (pass off)");
    }
    return names;
}

void Renderer::requestTargetCapture(const std::string& name, std::filesystem::path path, uint32_t mip,
                                    uint32_t layer) {
    const std::vector<TargetEntry> targets = captureTargets();

    const auto it = std::find_if(targets.begin(), targets.end(),
                                 [&](const TargetEntry& e) { return name == e.name; });
    if (it == targets.end()) {
        std::string known;
        for (const std::string& n : captureTargetNames()) known += "\n    " + n;
        core::Logger::error(core::LogCategory::Render, "Target capture: no target named '%s'. Known targets:%s", name.c_str(),
                      known.c_str());
        return;
    }
    if (!it->live) {
        core::Logger::error(core::LogCategory::Render,
                      "Target capture: '%s' is written by a pass that is switched off; it holds nothing this "
                      "frame. Enable the pass and try again.",
                      name.c_str());
        return;
    }
    if (it->image->image == VK_NULL_HANDLE) {
        core::Logger::error(core::LogCategory::Render, "Target capture: '%s' has not been created", name.c_str());
        return;
    }
    if (it->image->samples != VK_SAMPLE_COUNT_1_BIT) {
        core::Logger::error(core::LogCategory::Render, "Target capture: '%s' is multisampled and cannot be copied", name.c_str());
        return;
    }
    if (targetBytesPerPixel(it->image->format) == 0) {
        core::Logger::error(core::LogCategory::Render, "Target capture: '%s' has format %d, which cannot be decoded",
                      name.c_str(), static_cast<int>(it->image->format));
        return;
    }

    targetCaptureName = name;
    targetCapturePath = std::move(path);
    targetCaptureMip = mip;
    targetCaptureLayer = layer;
}

void Renderer::recordTargetCapture(VkCommandBuffer cmd) {
    const std::vector<TargetEntry> targets = captureTargets();
    const auto it = std::find_if(targets.begin(), targets.end(),
                                 [&](const TargetEntry& e) { return targetCaptureName == e.name; });
    targetCaptureName.clear();
    if (it == targets.end() || it->image->image == VK_NULL_HANDLE) return;

    const GpuImage& img = *it->image;
    const uint32_t bpp = targetBytesPerPixel(img.format);

    const uint32_t mipFirst = targetCaptureMip == UINT32_MAX ? 0 : targetCaptureMip;
    const uint32_t mipLast = targetCaptureMip == UINT32_MAX ? img.mipLevels - 1 : targetCaptureMip;
    const uint32_t layerFirst = targetCaptureLayer == UINT32_MAX ? 0 : targetCaptureLayer;
    const uint32_t layerLast = targetCaptureLayer == UINT32_MAX ? img.arrayLayers - 1 : targetCaptureLayer;

    const std::filesystem::path stem = targetCapturePath.parent_path() / targetCapturePath.stem();
    const std::string ext = targetCapturePath.extension().empty() ? ".png" : targetCapturePath.extension().string();

    for (uint32_t mip = mipFirst; mip <= mipLast && mip < img.mipLevels; ++mip) {
        // Mip n is the base extent halved n times, never below 1 -- the same rule
        // vkCreateImage applies, and the reason bloomExtent is clamped at creation.
        const VkExtent2D extent{std::max(1u, img.extent.width >> mip), std::max(1u, img.extent.height >> mip)};

        for (uint32_t layer = layerFirst; layer <= layerLast && layer < img.arrayLayers; ++layer) {
            PendingSubresource sub;
            sub.extent = extent;
            sub.format = img.format;

            std::string suffix;
            if (img.mipLevels > 1 && targetCaptureMip == UINT32_MAX) suffix += ".mip" + std::to_string(mip);
            if (img.arrayLayers > 1 && targetCaptureLayer == UINT32_MAX) suffix += ".layer" + std::to_string(layer);
            sub.path = stem.string() + suffix + ext;

            const VkDeviceSize size = static_cast<VkDeviceSize>(extent.width) * extent.height * bpp;
            sub.staging = createBuffer(*ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       "targetCaptureStaging");
            if (sub.staging.mapped == nullptr) {
                core::Logger::error(core::LogCategory::Render, "Target capture: readback buffer is not host-visible");
                destroyBuffer(*ctx, sub.staging);
                continue;
            }

            recordCaptureCopy(cmd, img.image, extent, it->layout, sub.staging.buffer, it->aspect, mip, layer);
            targetPending.push_back(std::move(sub));
        }
    }
}

void Renderer::finishTargetCapture(VkFence fence) {
    vkCheck(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(targetCapture)");

    for (PendingSubresource& sub : targetPending) {
        invalidateForHostRead(*ctx, sub.staging);
        writeTargetPng(sub.path, sub.staging.mapped, sub.extent, sub.format);
        destroyBuffer(*ctx, sub.staging);
    }
    targetPending.clear();
}

void Renderer::finishCapture(VkFence fence) {
    // The frame that recorded the copy has to have finished before the mapped pointer
    // holds anything. This is the one place the renderer deliberately stalls.
    vkCheck(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(capture)");
    invalidateForHostRead(*ctx, captureStaging);

    if (writeCapturePng(capturePath, captureStaging.mapped, swap.extent, swap.format)) captureCount++;
    destroyBuffer(*ctx, captureStaging);
}

namespace {

/// ffmpeg's name for a swapchain layout, or null when it has none. The set is narrower
/// than `captureBytesPerPixel` accepts on purpose: the ten-bit packed formats decode
/// fine into a PNG, one texel at a time, and have no rawvideo equivalent that does not
/// need a conversion pass this deliberately does not have.
const char* recordPixelFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return "rgba";
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return "bgra";
    default: return nullptr;
    }
}

} // namespace

bool Renderer::startRecording(core::Recorder& sink, core::Recorder::Options options, core::AudioTap* audio) {
    if (recorder != nullptr) return true;

    if (!swap.captureSupported) {
        core::Logger::error(core::LogCategory::Render, "Record: the surface does not allow TRANSFER_SRC on swapchain images");
        return false;
    }
    const char* pixelFormat = recordPixelFormat(swap.format);
    if (pixelFormat == nullptr) {
        core::Logger::error(core::LogCategory::Render, "Record: swapchain format %d has no rawvideo equivalent",
                      static_cast<int>(swap.format));
        return false;
    }

    // The size and the layout are the renderer's to state and the caller's to accept:
    // the encoder is fixed to one resolution for the life of the recording, and the only
    // thing that knows what a presented frame looks like is the swapchain.
    options.width = swap.extent.width;
    options.height = swap.extent.height;
    options.pixelFormat = pixelFormat;
    if (!sink.start(std::move(options), audio)) return false;

    const VkDeviceSize size = static_cast<VkDeviceSize>(swap.extent.width) * swap.extent.height * 4;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        recordStaging[i] = createBuffer(*ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                        "recordStaging");
        recordSlotRepeat[i] = 0;
        if (recordStaging[i].mapped == nullptr) {
            core::Logger::error(core::LogCategory::Render, "Record: could not map a %llu-byte readback buffer",
                          static_cast<unsigned long long>(size));
            for (uint32_t j = 0; j <= i; ++j) destroyBuffer(*ctx, recordStaging[j]);
            sink.stop();
            return false;
        }
    }

    recorder = &sink;
    recordStart = std::chrono::steady_clock::now();
    return true;
}

void Renderer::stopRecording() {
    if (recorder == nullptr) return;

    // Every slot still holding pixels has already been submitted, so waiting here is
    // what makes those frames readable rather than a race against the encoder. It is a
    // full device wait, once, at the end of a recording -- not a per-frame cost.
    vkDeviceWaitIdle(ctx->device);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) drainRecordSlot(i);

    recorder = nullptr;
    for (auto& buffer : recordStaging) destroyBuffer(*ctx, buffer);
}

void Renderer::drainRecordSlot(uint32_t slot) {
    if (recorder == nullptr || recordSlotRepeat[slot] == 0) return;

    GpuBuffer& staging = recordStaging[slot];
    if (staging.mapped != nullptr) {
        invalidateForHostRead(*ctx, staging);
        const size_t bytes = static_cast<size_t>(swap.extent.width) * swap.extent.height * 4;
        recorder->submitFrame(staging.mapped, bytes, recordSlotRepeat[slot]);
    }
    recordSlotRepeat[slot] = 0;
}

FrameResult Renderer::handleResize() {
    // The encoder was told one resolution at `start()` and rawvideo carries no size, so
    // feeding it a differently shaped frame would not fail -- it would silently produce a
    // sheared picture from that point on. Ending the recording is the honest answer, and
    // what was recorded up to here is still written.
    if (recorder != nullptr) {
        core::Logger::warn(core::LogCategory::Render, "Record: the window was resized, so the recording ends here");
        // Only the tee stops. The encoder stays up and `Engine` still muxes at shutdown,
        // so the recording is short rather than lost.
        stopRecording();
    }

    vkDeviceWaitIdle(ctx->device);
    destroyViewTargets();
    destroyFrameResources();
    if (swap.recreate(*ctx, window, vsyncEnabled) == FrameResult::WindowClosed) return FrameResult::WindowClosed;
    createFrameResources();
    createViewTargets();
    createPipelines();
    return FrameResult::Continue;
}

FrameResult Renderer::drawFrame(const scene::Camera& camera) {
    if (scene == nullptr) return FrameResult::Continue;

    // Frame-to-frame wall time, which is what FPS means. Measuring the span of
    // drawFrame instead would exclude event polling and camera update and report a
    // number nobody experiences. The first frame has no predecessor, so it is skipped
    // rather than averaged in as a several-hundred-millisecond outlier.
    //
    // Wall time is not a CPU number: it contains this frame's waitFence, acquire and
    // present, so on a GPU-bound scene it equals the GPU frame and reports the CPU as
    // busy when the CPU was asleep. Subtracting those three blocks is what makes the
    // difference visible, and it happens *here* rather than where they are measured
    // because the span [frameStart(n), frameStart(n+1)) is what contains them all.
    // That also means the early returns below need no special casing: they extend the
    // span, and the blocked time they already accumulated is carried into it.
    const auto frameStart = std::chrono::steady_clock::now();
    if (lastFrameStart.time_since_epoch().count() != 0) {
        const double deltaMs = std::chrono::duration<double, std::milli>(frameStart - lastFrameStart).count();
        avgWallMs = wallFrameMs.nextValue(deltaMs);
        // Clamped: the block timestamps sit just inside the span's endpoints rather
        // than exactly on them, so a frame that is nothing but a block can round past.
        avgCpuBusyMs = cpuBusyMs.nextValue(std::max(0.0, deltaMs - frameBlockedMs));
    }
    frameBlockedMs = 0.0;
    lastFrameStart = frameStart;

    // Every GPU block is timed the same way and summed into one total. RAII, so the
    // `return handleResize()` inside the acquire block below still contributes its own
    // wait instead of silently vanishing from it.
    struct BlockGuard {
        double& total;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~BlockGuard() {
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    };

    FrameSync& f = frames[frameSlot];

    {
        auto s = core::Profiler::scope("Renderer::waitFence");
        BlockGuard blocked{frameBlockedMs};
        vkCheck(vkWaitForFences(ctx->device, 1, &f.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }

    // Before anything is recorded, so an image loaded during `Game::init` or on a keypress
    // is resident by the time `recordOverlay` binds the array. Costs one comparison on a
    // frame where nothing was loaded or destroyed.
    syncImages();
    // After it, because it writes into the descriptor array `syncImages` sizes.
    syncViews();

    // Here rather than inside `recordSprites`, and that is the whole reason it is a
    // separate function: growth waits on the device, and a device wait inside an open
    // command buffer is not a thing to find out about later.
    //
    // Called with 0 rather than not called, so the zone inside it is present on a run with
    // no sprite table at all. `ensureSpriteCapacity(0)` returns on its first test.
    ensureSpriteCapacity(sprites != nullptr ? static_cast<uint32_t>(sprites->draws().size()) : 0u);

    gpuProfiler.collect(*ctx, frameSlot);
    if (const double gpuMs = gpuProfiler.lastZoneMs("Frame"); gpuMs > 0.0) avgGpuMs = gpuFrameMs.nextValue(gpuMs);

    // The cull counters this slot wrote last time round (4.2). Safe to read only here,
    // after the fence: this is the point at which the frame that wrote them is known to
    // have finished. Two frames stale, which for a number on a HUD is not a problem
    // worth a second fence to fix.
    if (f.cullStats.mapped != nullptr) {
        invalidateForHostRead(*ctx, f.cullStats);
        const auto* counts = static_cast<const uint32_t*>(f.cullStats.mapped);
        // Both passes. Phase 0 draws what was visible last frame and phase 1 draws
        // whatever the occlusion test newly admitted, so the overlay's `vis` is their sum
        // -- reporting phase 0 alone would show the number falling every time the camera
        // turned, which is exactly when the second pass is doing the most work.
        view.visibleInstances = counts[0] + counts[kOcclusionView];
        // The second half of the same buffer. Zero until something carries a chain, and
        // equal to the LOD-0 triangle count of the visible set whenever nothing does.
        view.visibleTriangles = counts[kCullCommandLists] + counts[kCullCommandLists + kOcclusionView];
    }

    // The trace's counters. Every one of these was already computed and thrown away, or
    // logged once and lost: a zone can say a frame got slower and can never say why, and
    // "the cull admitted three times as much this frame" is the answer nine times out of
    // ten. Recorded here rather than after `record` because the cull figures are the ones
    // this fence just made readable, and the rest are the same frame's.
    //
    // Two frames stale, exactly as `visibleInstances` on the overlay is, and for the same
    // reason: the copy went into this slot two frames ago.
    core::Profiler::counter("drawCalls", stats.drawCalls);
    core::Profiler::counter("visibleInstances", view.visibleInstances);
    core::Profiler::counter("visibleTriangles", view.visibleTriangles);
    if (instances != nullptr) core::Profiler::counter("liveInstances", instances->liveCount());
    {
        // VMA's live figures rather than a regex over the log line that printed them once.
        // The old `scripts/baseline.py` recovered VRAM with a regular expression over
        // stdout and got one steady-state number because one line was all there was to
        // read; this is the same quantity every frame.
        const VulkanContext::MemoryUsage usage = ctx->memoryUsage();
        constexpr double kMiB = 1024.0 * 1024.0;
        core::Profiler::counter("vramMiB", static_cast<double>(usage.allocatedBytes) / kMiB);
        core::Profiler::counter("vramAllocations", usage.allocationCount);
    }

    // Same reasoning as the counters above, and the reason recording costs no stall: the
    // copy went into this slot two frames ago and the fence just waited on is the one
    // that finished it, so the mapped pointer is readable here and nowhere earlier.
    drainRecordSlot(frameSlot);

    // Before the dirty check below, so a reload and a feature toggle in the same frame
    // cost one rebuild rather than two.
    if (shaderHotReload) pollShaderReload();

    // Turning TAA on must not blend against whatever the history held when it was last
    // running: the camera has moved since, so the reprojection would be against a
    // stale matrix and the first frames would ghost badly.
    if (taaEnabled != taaWasEnabled) {
        taaWasEnabled = taaEnabled;
        view.taaHistoryValid = false;
        // Every view's, because every view has one now. A registered view whose history
        // stayed marked valid across the toggle would ghost against a matrix from whenever
        // TAA was last on, which is the one case this test exists for.
        for (ViewSlot& v : extraViews) v.targets.taaHistoryValid = false;
    }

    // `ssrScale` resizes an image, so it goes through the same rebuild the sample count
    // does. Compared on the extent it *produces* rather than on the float: a slider drag
    // writes a new value every frame and most of those values round to the pixels already
    // there, so this rebuilds once per step of the reduced extent instead of once per
    // frame of the drag.
    const VkExtent2D wantedSsrExtent = scaledBy(view.renderExtent, ssrScale);
    if (wantedSsrExtent.width != view.ssrExtent.width || wantedSsrExtent.height != view.ssrExtent.height) {
        renderTargetsDirty = true;
        // The composite is two pipelines and which one exists depends on the scale, so
        // crossing 1.0 in either direction has to rebuild them as well as the images.
        pipelinesDirty = true;
    }

    // A feature toggle is just a public member somebody assigned, so nothing signals
    // it. Comparing the key the live pipelines were built with against the current one
    // means there is no featuresChanged() call to forget.
    {
        // A block rather than a function, so the zone is named for what it is. Above the
        // dirty test for the reason the rest of this list is: not rebuilding is the common
        // case and costs one comparison, and the max is the number that matters.
        auto s = core::Profiler::scope("pipelineRebuild");
        if (renderTargetsDirty || pipelinesDirty || featureKey() != builtFeatureKey) {
            vkDeviceWaitIdle(ctx->device);
            if (renderTargetsDirty) {
                destroyViewTargets();
                createViewTargets();
            }
            createPipelines();
            renderTargetsDirty = false;
            pipelinesDirty = false;
        }
    }

    // Resize *before* acquiring, never after. A successful acquire hands `imageAvailable`
    // to the presentation engine to signal, and vkDeviceWaitIdle does not drain that:
    // it waits on queue work, and the presentation engine is not a queue. Abandoning
    // the frame at that point would have handleResize() destroy a semaphore with a
    // signal operation still outstanding. Bailing out first means the acquire either
    // has not happened or has failed, and a failed acquire signals nothing.
    if (resizeRequested) {
        resizeRequested = false;
        return handleResize();
    }

    uint32_t imageIndex = 0;
    {
        auto s = core::Profiler::scope("Renderer::acquire");
        BlockGuard blocked{frameBlockedMs};
        const VkResult r =
            vkAcquireNextImageKHR(ctx->device, swap.handle, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (r == VK_ERROR_OUT_OF_DATE_KHR) return handleResize();

        // SUBOPTIMAL did acquire an image and did signal the semaphore, so this frame
        // is finished normally and the resize happens at the top of the next one.
        if (r == VK_SUBOPTIMAL_KHR) {
            resizeRequested = true;
        } else if (r != VK_SUCCESS) {
            vkCheck(r, "vkAcquireNextImageKHR");
        }
    }

    // The primary's, and it runs before any secondary view's for one reason that is not
    // about uniforms at all: this is the call that assigns punctual atlas layers, and
    // `recordPunctualShadows` renders that assignment once for the whole frame. A secondary
    // ranks its own lights but looks their layers up in what this call decided -- see the
    // shadow-atlas section of `updateLights`.
    // `primary`, `uniformSlot` and an empty `destination` are what this `View` *is* and are
    // never assigned anywhere; only the user's TAA switch moves between frames.
    view.taaActive = taaEnabled;

    // **Before anything reads or records against the light buffer, and after the last frame
    // said how many it wanted** (C40). Growing here rather than inside `updateLights` is the
    // whole of why a light is dropped for exactly one frame instead of forever: the ranking
    // runs mid-frame with command buffers in flight, and reallocating a buffer there would
    // be a use-after-free rather than a resize.
    growLightBuffer();

    updateUniforms(camera, frameSlot);
    // `cullViewProj` is one array shared by every chain, so a secondary view's
    // `updateUniforms` overwrites entry 0. Kept here rather than recomputed, which would
    // mean a second light ranking and a second atlas assignment.
    const glm::mat4 primaryCullViewProj = cullViewProj[0];

    vkCheck(vkResetFences(ctx->device, 1, &f.inFlight), "vkResetFences");
    vkCheck(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");

    gpuProfiler.beginFrame(f.cmd, frameSlot, core::Profiler::frameNumber());

    {
        // Every pass called from here opens a CPU scope of its own, on its first line,
        // and the naming rule is worth stating once rather than guessing at twenty-seven
        // call sites:
        //
        //   - A pass with a GPU zone takes **that zone's name, spelled identically** --
        //     "SSR", not "Ssr". The two then sit in the trace as `SSR` on the GPU track
        //     and `Renderer::record/SSR` on the CPU one, and can be read against each
        //     other. A CPU zone that invents its own spelling cannot be.
        //   - A step with no GPU zone takes its own function's name. So a reader can tell
        //     from the name alone whether there is a GPU row to compare against.
        //
        // The scope goes *above* the early-outs, not beside the `GpuScope` below them, so
        // a pass that decides to record nothing still costs a named zero rather than
        // vanishing -- and so the children sum to this zone. That sum is the check:
        // `scripts/baseline.py --zones` prints a total-per-frame column, and a gap
        // between the parent and its children is work this list does not name.
        auto s = core::Profiler::scope("Renderer::record");
        stats = FrameStats{};

        // Before any pass records: every scene pass draws out of this buffer, and it
        // is written by the host into staging that has to reach device memory first.
        updateInstances(frameSlot);
        blendedCommandCount = buildBlendedCommands(frameSlot, camera);
        // Only when the pass will run. `dynamicCount()` is the cheap gate: a scene with
        // nothing dynamic in it never walks the slots, and TAA off never asks. Built once
        // rather than per view, because it writes into the staging buffer the upload below
        // has already been handed -- and because TAA runs only for the primary anyway.
        velocityCommandCount =
            (taaEnabled && instances->dynamicCount() > 0) ? buildVelocityCommands(frameSlot) : 0;
        recordInstanceUpload(f.cmd, frameSlot);

        // An outer zone, so whole-frame GPU cost is one number rather than a sum the
        // overlay would have to keep in step with the pass list. It spans **every** view's
        // chain, which is what makes `Frame` still mean "what this frame cost" once there
        // is more than one of them.
        GpuScope frameZone(gpuProfiler, f.cmd, frameSlot, "Frame");

        // ------------------------------------------------------- once, whatever the views
        // Deformed vertices, the structures built over them and the shadow maps are
        // properties of the *scene*, so they are recorded once and every chain below
        // samples the same result. That is the split C31 drew, and this is where it pays:
        // re-running it per view would re-render up to 24 atlas layers for a mirror.
        //
        // The shadow atlas specifically holds the assignment the primary view's
        // `updateLights` made, which is why `updateUniforms` above already ran for the
        // primary and why a secondary view looks layers up rather than assigning them.
        //
        // The cull is here, ahead of the shadow passes, because they draw straight out of
        // the lists it writes. Every list, from the primary's matrices; a chain re-fills
        // list 0 for itself and leaves the other 25 alone.
        recordDepthPyramidLayout(f.cmd, frameSlot);
        recordCull(f.cmd, frameSlot, 0, CullViews::Scene);

        recordSkinning(f.cmd, frameSlot);
        // And straight after it, before anything traces: the dynamic BLASes are built
        // over the vertices that dispatch just wrote (S2.5).
        //
        // Behind the RT toggles, which was not the first answer. Refitting
        // unconditionally is a frame *never* stale and costs 0.12 ms in a scene with one
        // character -- five times what the deformation dispatch itself costs, paid every
        // frame whether or not anything traces, for a feature that is off by default.
        // The staleness this buys back is exactly one frame: the frame the toggle is
        // pressed on shows the previous pose's structure. That is not a trade worth
        // 0.12 ms.
        if (rtEnabled && ctx->rayQuerySupported && accel.hasDynamic()) {
            // The one pass whose CPU scope is at the call site rather than on its first
            // line: `refitSceneAccelStruct` is a free function in AccelStruct.cpp, and a
            // zone named for a pass belongs where the pass is scheduled.
            auto cpuZone = core::Profiler::scope("AsRefit");
            GpuScope zone(gpuProfiler, f.cmd, frameSlot, "AsRefit");
            refitSceneAccelStruct(*ctx, f.cmd, *instances, accel);
        }
        // Before the G-buffer only because both draw the scene and this one wants the
        // skinned vertices that recordSkinning just wrote; nothing downstream of here
        // reads the map until the lighting pass samples it.
        if (!rtActive) {
            recordShadows(f.cmd, frameSlot);
            recordPunctualShadows(f.cmd, frameSlot);
        }

        // ----------------------------------------------------------------- one per view
        // Secondary views first and the primary last, so the chain that presents is the
        // one whose results are left in the shared targets when the frame ends: the
        // capture paths, the readback and `captureTargets()` all describe what is in
        // those images afterwards, and they would otherwise describe a mirror.
        for (uint32_t s = 0; s < extraViews.size(); ++s) {
            ViewSlot& v = extraViews[s];
            if (!v.live) continue;
            // The camera lives in the table, where the game writes it, and is read here --
            // at the same point in the frame the main camera is resolved. Not a callback:
            // a callback would run at a point inside the renderer a game cannot reason
            // about, and every other per-frame quantity a game controls is a value it sets.
            const scene::Camera& viewCamera = views->at(s).active();

            // The chain records through `view` and nothing else, so this is what makes
            // twenty-odd record methods reach *this* view's targets without any of them
            // learning that views exist. Swapped back below, which is what leaves the
            // presenting view's set in `view` for the capture paths at the end of the frame.
            std::swap(view, v.targets);
            view.taaActive = taaEnabled;

            updateUniforms(viewCamera, frameSlot);
            recordViewChain(f.cmd, frameSlot, viewCamera, imageIndex);

            // The tonemap leaves its destination a colour attachment, which is right for
            // the presenting view and wrong for this one: a material samples it, and it
            // does so **in the same frame it was drawn**, so the layout has to move before
            // anything reads it. This is what makes a mirror a mirror rather than a
            // one-frame-late mirror.
            transitionImage(f.cmd, view.destination.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            recordViewBarrier(f.cmd);

            std::swap(view, v.targets);
        }

        // The primary's uniform block was filled before the command buffer opened and is
        // still intact -- blocks are per view, and so are the targets they name, which the
        // swap above put back. What a secondary chain *did* overwrite is `cullViewProj[0]`,
        // which is one array shared by every chain, so the camera's matrix is put back
        // rather than the whole block recomputed. Skipped entirely when nothing else
        // recorded, which is every golden case.
        if (!extraViews.empty()) cullViewProj[0] = primaryCullViewProj;

        recordViewChain(f.cmd, frameSlot, camera, imageIndex);

        // ------------------------------------------------- the primary view's, and only
        // Everything below draws onto the composed image and is a property of the window
        // rather than of a view: a HUD, a wireframe overlay, the presentation blit. A
        // mirror gets the world and nothing else.
        //
        // After the tonemap so a wireframe is not exposed, curved and bloomed along with
        // the scene, and before the overlay so text stays on top of everything (S4.5).
        //
        // Into the compose surface unconditionally: a wireframe is world-space geometry
        // and belongs with the world, so it scales with it whatever the UI does (P2).
        // After the tonemap for the same reason the debug lines are -- an unlit sprite is
        // display-referred art and exposing, curving and bloating it would be applying
        // three corrections to a texel that needs none -- and *before* them, so a
        // wireframe stays a diagnostic drawn over everything (P4).
        recordSprites(f.cmd, frameSlot, composeView(imageIndex), camera);

        if (!debugLines.empty()) {
            recordDebugLines(f.cmd, frameSlot, composeView(imageIndex), view.renderExtent, camera);
        }

        // Three reasons to run the pass, and the third is S6's: the UI draws through it,
        // so a panel open over a run that turned the stats off still gets a pass. This
        // condition having only two arms is why the panel rendered nothing at all the
        // first time it was captured -- `--capture` disables the HUD, and the UI went
        // with it. `readbackSlot` is the fourth, and it is the same class of mistake: a
        // run whose only reason to draw is the readback image must still get a pass.
        const bool anyOverlay = debugOverlay || readbackSlot != 0 || !overlayLines.empty() ||
                                (uiDrawList != nullptr && !uiDrawList->empty());

        // The overlay is the one pass with a choice, and `uiInsideVirtual` is it. Inside,
        // it draws at the virtual resolution and is magnified by the same integer scale as
        // the world -- an 8-pixel font stays an 8-pixel font. Outside, it draws after the
        // blit, at the window's own resolution, over a letterboxed image.
        if (anyOverlay && debugFont.ready() && uiInsideVirtual) {
            recordOverlay(f.cmd, frameSlot, composeView(imageIndex), view.renderExtent);
        }

        // The presentation step (P2). Records nothing at native, where the passes above
        // already drew into the swapchain image.
        recordPresent(f.cmd, frameSlot, imageIndex);

        if (anyOverlay && debugFont.ready() && !uiInsideVirtual) {
            recordOverlay(f.cmd, frameSlot, swap.views[imageIndex], swap.extent);
        }
    }

    // Between the last pass and the present transition, so the readback sees the
    // finished image including the overlay, and the image is still in a layout this
    // frame owns.
    const bool capturing = captureRequested && beginCapture();
    if (capturing) {
        recordCaptureCopy(f.cmd, swap.images[imageIndex], swap.extent, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          captureStaging.buffer);
    }

    // Here for the same reason: every pass has run, so each target is in the layout its
    // row in captureTargets() states, and the copy is inside the frame that owns them.
    if (!targetCaptureName.empty()) recordTargetCapture(f.cmd);

    // And the recording, alongside them rather than instead of them: a session being
    // recorded can still take a screenshot, and two copies out of one image is two
    // barriers, not a conflict. `framesOwed` is what makes this cheap -- above 30 fps
    // most frames are owed nothing and record no copy at all.
    if (recorder != nullptr) {
        const double elapsed = std::chrono::duration<double>(frameStart - recordStart).count();
        if (const uint32_t owed = recorder->framesOwed(elapsed); owed > 0) {
            // Assignment, not accumulation: the slot was drained at the top of this
            // frame, so this is known to be empty. What carries an unpaid repeat forward
            // is `Recorder::submitFrame`, which is the only place that can know whether
            // the encoder took the frame.
            recordSlotRepeat[frameSlot] = owed;
            recordCaptureCopy(f.cmd, swap.images[imageIndex], swap.extent, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              recordStaging[frameSlot].buffer);
        }
    }

    transitionImage(f.cmd, swap.images[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    vkCheck(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = f.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = renderFinished[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = f.cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;

    {
        auto s = core::Profiler::scope("Renderer::submit");
        vkCheck(vkQueueSubmit2(ctx->graphicsQueue, 1, &submit, f.inFlight), "vkQueueSubmit2");
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swap.handle;
    present.pImageIndices = &imageIndex;

    {
        auto s = core::Profiler::scope("Renderer::present");
        BlockGuard blocked{frameBlockedMs};
        const VkResult r = vkQueuePresentKHR(ctx->presentQueue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            resizeRequested = true;
        } else if (r != VK_SUCCESS) {
            vkCheck(r, "vkQueuePresentKHR");
        }
    }

    // After present rather than before: the copy is already recorded, so waiting here
    // costs the screenshot a frame of latency instead of costing the frame a stall
    // before it reaches the display.
    if (capturing) finishCapture(f.inFlight);
    if (!targetPending.empty()) finishTargetCapture(f.inFlight);

    frameSlot = (frameSlot + 1) % kFramesInFlight;
    // Ping-pong after the frame that used it, so recordTaa and recordTonemap above
    // both saw the same index.
    view.taaHistoryIndex ^= 1u;
    framesSubmitted++;
    return FrameResult::Continue;
}

void Renderer::logGpuTimings() {
    if (gpuTimingAvailable) {
        core::Logger::status(core::LogCategory::Render,
                       "GPU @ %ux MSAA: Cull %.3f | GBuffer %.3f | SSAO %.3f | "
                       "Lighting %.3f | "
                       "Decals %.3f | Forward %.3f | Particles %.3f (sort %.3f) | SSR %.3f | Fog %.3f | Bloom %.3f | Velocity %.3f | TAA %.3f | Tonemap %.3f | Overlay %.3f | Frame %.3f ms",
                       static_cast<uint32_t>(msaaSamples), gpuProfiler.lastZoneMs("Cull"),
                       gpuProfiler.lastZoneMs("GBuffer"),
                       gpuProfiler.lastZoneMs("SSAO"),
                       gpuProfiler.lastZoneMs("Lighting"),
                       gpuProfiler.lastZoneMs("Decals"), gpuProfiler.lastZoneMs("Forward"),
                       gpuProfiler.lastZoneMs("Particles"), gpuProfiler.lastZoneMs("ParticleSort"),
                       gpuProfiler.lastZoneMs("SSR"), gpuProfiler.lastZoneMs("Fog"), gpuProfiler.lastZoneMs("Bloom"),
                       gpuProfiler.lastZoneMs("Velocity"), gpuProfiler.lastZoneMs("TAA"),
                       gpuProfiler.lastZoneMs("Tonemap"), gpuProfiler.lastZoneMs("Overlay"),
                       gpuProfiler.lastZoneMs("Frame"));
    } else {
        // Not a table of zeros: a pass reading 0.000 ms because the device cannot
        // time it looks exactly like a pass that costs nothing. The two reasons are
        // separated because one is the machine and the other is the command line, and
        // reporting "no timestamp support" for a flag the caller passed would send them
        // looking at the driver.
        core::Logger::status(core::LogCategory::Render, "GPU @ %ux MSAA: timings unavailable (%s)",
                       static_cast<uint32_t>(msaaSamples),
                       core::Profiler::enabled() ? "no timestamp support" : "--no-profiler");
    }

    // Wall and CPU are both reported because the gap between them is the answer to
    // "which one is the limiter". Wall alone cannot say: it contains the GPU blocks.
    core::Logger::status(core::LogCategory::Render,
                   "Frame %.3f ms wall (%.1f FPS) | CPU busy %.3f ms | %u commands | prims %u (%u visible) | "
                   "blended %u | velocity %u | tris %llu (drawn %u)",
                   avgWallMs, avgWallMs > 0.0 ? 1000.0 / avgWallMs : 0.0, avgCpuBusyMs, stats.drawCalls,
                   stats.primitives, view.visibleInstances, stats.blendedDrawCalls, stats.velocityDrawCalls,
                   static_cast<unsigned long long>(stats.triangles), view.visibleTriangles);
}

} // namespace gfx
