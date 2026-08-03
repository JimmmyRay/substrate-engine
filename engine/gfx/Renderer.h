#pragma once

#include "core/AveragingBuffer.h"
#include "gfx/GpuProfiler.h"
#include "core/Recorder.h"
#include "core/Settings.h"
#include "gfx/AccelStruct.h"
#include "gfx/DebugLines.h"
#include "gfx/Decal.h"
#include "gfx/DebugView.h"
#include "gfx/ImageTable.h"
#include "gfx/Light.h"
#include "gfx/Presentation.h"
#include "gfx/Resources.h"
#include "gfx/ShaderVariant.h"
#include "gfx/Swapchain.h"
#include "gfx/ViewTable.h"
#include "gfx/VulkanContext.h"
#include "scene/Animation.h"
#include "scene/Camera.h"
#include "scene/Cloth.h"
#include "scene/InstanceTable.h"
#include "scene/ParticleSystem.h"
#include "scene/SceneTypes.h"
#include "scene/SpriteTable.h"
#include "ui/Font.h"
#include "ui/Ui.h"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct GLFWwindow;
namespace core {
class AudioTap;
} // namespace core

namespace scene {
class GltfScene;
class Camera;
} // namespace scene

namespace gfx {

/// The settings table's built-in, for every field `bindRenderer` binds. **A bound field
/// must initialise from here**, or its value is spelled twice and drifts. Only rows in the
/// `render` module are aliased, because only those have a field here.
namespace rowDefault = core::defaults::render;

/// Shading budget for lights where the game states none. The live value is
/// `Renderer::lightBudget`, set before init, and the storage buffer is sized from *that*,
/// so this is a default and not a capacity. What bounds the light count is the
/// O(pixels x lights) deferred loop.
constexpr uint32_t kDefaultLightBudget = 32u;

/**
 * @brief Sub-pixel offsets TAA cycles through, as a Halton(2,3) sequence.
 *
 * Eight rather than sixteen: the history blend gives the current frame 0.1 weight, so a
 * sample is down to a few percent after eight frames and a longer sequence mostly
 * lengthens the time a disocclusion takes to settle.
 *
 * **Halton rather than random**, because a low-discrepancy sequence covers the pixel evenly
 * at *every* prefix length -- and the effective window is however many frames the clamp has
 * not rejected, which is not known in advance.
 */
constexpr uint32_t kTaaJitterCount = 8;

/// Levels in the bloom chain, counting the half-resolution top. Five reaches roughly
/// 1/32 of the screen, which is a wide enough glare radius at 1080p; more levels keep
/// spreading but each one contributes less than the last.
constexpr uint32_t kBloomMips = 5;

/// Levels in the Hi-Z pyramid. Eight covers a 256x256 footprint from the base, which is
/// more than any single draw's screen-space box needs -- the test picks the level where the
/// box spans about two texels and stops there.
constexpr uint32_t kDepthPyramidMips = 8;

/// Side of one light-assignment tile, in pixels (C35). **This is `local_size_x` in
/// light_tile_body.glsl and `frame.tileParams.y`**: one workgroup per tile and one
/// invocation per pixel, so the three are one fact and moving one moves all three.
///
/// 16 rather than 8 or 32 because the two costs pull opposite ways -- a smaller tile
/// bounds each light more tightly and multiplies the per-tile work, a larger one shares
/// the build across more pixels and admits lights that reach none of them.
constexpr uint32_t kLightTileSize = 16;

/// Mask words per tile the build pass will write, so 1024 lights. The shared array in
/// light_tile_body.glsl is sized by the same number, which is why this is a ceiling and
/// not a hint: above it `Renderer` refuses to tile and says so, rather than truncating
/// a light list a game asked for.
constexpr uint32_t kLightTileMaxWords = 32;

/**
 * @brief Side of the sun's shadow map, square, one layer.
 *
 * **One map fitted to the scene, not four fitted to the camera.** Every quantity that
 * differs between cascades -- the world size of a texel, the distance a depth bias moves an
 * occluder, the width the kernel spans -- changes when a surface crosses a split, and which
 * cascade a surface is in is a function of where the camera stands. Walking towards a wall
 * slid the shadow across it: 24% of pixels changed, peak delta 630, cascade assignment the
 * only difference. No bias tuning removes it, because it is the camera term in the
 * projection, and a scene-fitted box has none.
 *
 * 4096 costs no more memory than the cascades: four 2048-square layers and one
 * 4096-square map are both 64 MB at D32.
 *
 * **The limit this accepts**: a scene much larger than its detail is dense gets uniform
 * texel density where cascades would have concentrated it. `shadowDistance` caps the
 * fitted box for that case; an unbounded world would want cascades back, anchored to the
 * light rather than the camera.
 */
constexpr uint32_t kShadowMapSize = 4096;

/**
 * @brief Side of one punctual atlas layer, and how many layers there are.
 *
 * Shadow-casting punctual lights share one depth array: a spot takes one layer, a point
 * takes six. 24 layers is four points, or three points and six spots. **A light that does
 * not fit still lights**; it just does not occlude.
 *
 * 1024 rather than the sun's 4096 because there are up to 24 and each is a scene re-render.
 * What makes that affordable is the cache in `recordPunctualShadows`: a layer whose matrix
 * and geometry are unchanged already holds the right depth.
 */
constexpr uint32_t kPunctualShadowSize = 1024;
constexpr uint32_t kMaxShadowLayers = 24;

/// Views the culling dispatch runs for: the camera, the sun, and one per shadow layer.
/// The sun's is its orthographic box, which the same plane extraction handles.
constexpr uint32_t kCullViews = 2 + kMaxShadowLayers;

/// The extra command list the occlusion phase writes into. One past the views, so phase
/// 0's commands survive until the second draw has read phase 1's.
constexpr uint32_t kOcclusionView = kCullViews;
/// Command lists the output region holds: every view, plus the occlusion phase's.
constexpr uint32_t kCullCommandLists = kCullViews + 1;

/**
 * @brief Camera views a frame can record, and therefore uniform blocks per frame slot.
 *
 * Four: the presenting view and three a game creates, which is a minimap beside a
 * rear-view mirror, or a four-way inset. It costs one `FrameUniforms`, light buffer and
 * shadow-matrix buffer per view per frame slot, allocated once — **a block is written only
 * by a view that exists**, so a one-view frame uploads exactly what it uploaded before.
 *
 * **This is not what a view costs.** The uniform blocks are kilobytes; a live view's
 * *target set* is ~224 MiB at 1600x900 and 4x MSAA, and that is why `ViewTable::create`
 * takes an extent. Raising this constant is still cheap. Creating the views is not.
 */
constexpr uint32_t kMaxViews = 4;



/**
 * @brief World bounds of one indirect command's instance run. Must match
 *        `CommandBounds` in cull.comp.
 *
 * Per *command* rather than per instance, because a command is what the cull can switch
 * off. For a merged run it is the union of the boxes, which is the cost of collapsing N
 * draws into one -- and why runs are capped at `kMaxInstancesPerCommand`.
 */
struct GpuCommandBounds {
    glm::vec4 boundsMin{0.0f}; ///< w unused
    glm::vec4 boundsMax{0.0f}; ///< w unused

    /**
     * @brief The command's LOD chain, `(firstIndex, indexCount)` per level.
     *
     * **Level 0 is copied out of the indirect command written in the same statement**, not
     * out of a second record describing it, which is why the chain rides here rather than
     * in an array of its own: nothing can fall out of step when a run merges or a variant
     * sweep reorders the list.
     *
     * A command whose primitive carries no chain has `lodLevels == 1`.
     */
    glm::uvec2 lods[scene::kMaxLodLevels + 1]{};
    uint32_t lodLevels = 1;
    uint32_t pad[3]{0u, 0u, 0u};
};

static_assert(sizeof(GpuCommandBounds) == 80, "GpuCommandBounds must match cull.comp");

/**
 * @brief Longest run of consecutive instances one indirect command may cover.
 *
 * Submission cost against culling granularity: an unbounded run makes 4096 boxes one draw
 * call and one un-cullable blob, a cap of one gives perfect culling and no instancing. 64
 * keeps a 16x16x16 grid at 64 commands while each still covers a block small enough that
 * leaving the view removes it.
 */
constexpr uint32_t kMaxInstancesPerCommand = 64;

/**
 * @brief A run of indirect commands that share a shader variant.
 *
 * Each group is a contiguous range a pass submits in one call with one pipeline bound.
 * `first` is explicit because the ranges are *ordered by variant* while the command buffer
 * stays ordered static-then-skinned; walking them by variant keeps the pipeline bind at
 * one per variant.
 *
 * Instancing survives untouched -- a merged run shares a primitive, one primitive has one
 * material, one material has one variant. **What grouping costs is runs that would have
 * formed across a group boundary**, since a merge requires adjacent slots, so
 * fragmentation grows with variant count on an instance-heavy scene.
 */
struct VariantRange {
    uint32_t variant = 0;
    uint32_t first = 0;
    uint32_t count = 0;
    /// How many of `count` are alpha-unmasked, and they lead, so a depth-only pass can draw
    /// `[first, first + unmasked)` with no fragment shader bound and keep early-Z. Nothing
    /// reads it today, but the split cannot be recovered later without rebuilding the list.
    uint32_t unmasked = 0;
};

/// Must match frame.glsl exactly.
struct FrameUniforms {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 cameraForward; ///< xyz normalised view direction, for cascade selection
    /// The projection's depth, inverted: the four coefficients `viewDistance()` in
    /// frame.glsl evaluates, from `Camera::depthLinear`. Here rather than in the three push
    /// blocks that each carried a `nearPlane` for it: this is a property of the frame's
    /// projection, not of any one pass.
    glm::vec4 depthLinear;

    glm::vec4 sunDirection; ///< xyz toward the light, w intensity
    glm::vec4 sunColor;

    glm::vec4 params;       ///< x exposure, y light count, z specular AA, w bloom strength
    /// x the light cutoff, **squared and divided out of exposure** so the light loop's test
    /// is one compare against the `dot(radiance, radiance)` it already has; yzw spare.
    glm::vec4 lightParams;
    /// Constant ambient: rgb radiance, w spare and always zero.
    glm::vec4 ambient;
    /// x debug view, y sample count, z traced reflections composite this frame,
    /// w the projection is orthographic -- read only by the skybox ray, which is the one
    /// thing in the renderer that cannot be written for both at once.
    glm::uvec4 flags;
    /// The tile light grid (C35): x tiles across, y `kLightTileSize`, z mask words per
    /// tile, w spare. **z is zero exactly when tiling is off for this frame**, which
    /// is the one value the light loops branch on.
    glm::uvec4 tileParams;

    /// The sun's orthographic view-projection, fitted to the scene and independent of the
    /// camera. Read by the lighting pass to sample *and* by shadow.vert to render into,
    /// which is why it is here rather than in a push constant.
    glm::mat4 sunViewProj;
    /// x texel size in UV, y depth bias in NDC depth, z normal offset in world units,
    /// w spare. Both biases are converted from world units on the CPU, once.
    glm::vec4 shadowParams;

    /// Previous frame's view-projection, *unjittered*, for TAA reprojection.
    glm::mat4 prevViewProj;
    /// This frame's inverse view-projection with the TAA jitter removed. **Both sides of
    /// the reprojection have to be unjittered**, or the history lookup is offset by the
    /// jitter every frame and re-filters itself -- see taa.comp.
    glm::mat4 invViewProjNoJitter;
};

// `DebugView` and `TonemapOperator` live in gfx/DebugView.h so `engine/core/Config.cpp`
// can name them: it is a hosted source and cannot include anything that reaches Vulkan,
// and a parser returning bare integers was one enum reordering away from silently
// mis-mapping every `--debug-view` and `--tonemap` flag.

// `Decal` and `decalAt` are in gfx/Decal.h, which has no Vulkan in it and is testable.

/// One vertex of the debug overlay. Positions are pixels, origin top-left.
///
/// An alias rather than a struct of its own: the HUD, the binding menu and the UI all fill
/// one buffer through one pipeline, so a second declaration of the same five fields is a
/// second place to drift from `overlay.vert`. It lives in `ui/FontMetrics.h`, the header
/// with no Vulkan in it, which is what lets layout code build vertices without a device.
using OverlayVertex = ui::DrawVertex;

/// What the overlay reports, gathered while the G-buffer pass records.
struct FrameStats {
    uint32_t drawCalls = 0;
    uint32_t primitives = 0;
    uint64_t triangles = 0;
    uint32_t blendedDrawCalls = 0;  ///< the forward pass
    /// Draws in the velocity pass. **Reported because the pass's output is a signed
    /// near-zero quantity**: a readback of it is a black PNG whether it wrote the whole
    /// screen or nothing at all, so the count is the only cheap thing that tells the two
    /// apart.
    uint32_t velocityDrawCalls = 0;
    uint32_t particles = 0;        ///< live particles drawn this frame
    uint32_t sprites = 0;          ///< sprite instances in this frame's one draw
};

/**
 * @brief Deferred renderer: G-buffer, per-sample lighting, tonemap.
 *
 * Dynamic rendering throughout -- no VkRenderPass or VkFramebuffer anywhere.
 */
class Renderer {
  public:
    /// Non-copyable; see the note on `Uploader` in Resources.h. This one owns roughly sixty
    /// handles.
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static constexpr uint32_t kFramesInFlight = 2;

    /**
     * @param uploader        Must already be initialised: the font atlas is uploaded here.
     * @param debugFontPath   TTF for the overlay; empty selects the embedded bitmap font.
     * @param debugFontHeight Rasterisation height for that TTF, in pixels.
     */
    /// Aborts on failure, as VulkanContext::init does. There is no partially-built
    /// renderer to hand back and nothing the caller could do with one.
    void init(VulkanContext& ctx, Uploader& uploader, GLFWwindow* window, bool vsync, uint32_t msaaRequest,
              const std::string& debugFontPath, float debugFontHeight);
    void shutdown();

    void setScene(const scene::GltfScene* scene);

    /**
     * @brief The instance table this renderer draws.
     *
     * Separate from `setScene`, which supplies *resources* -- one vertex buffer, one index
     * buffer, one descriptor set of materials and textures. What to draw, where and how
     * many times is the table's, so forty copies of one mesh need no loader involvement.
     *
     * **Must be called before `drawFrame`.** Instances may be created and destroyed
     * afterwards — but only through `instancesGrew()`, which is what sizes the buffers and
     * marks the structure. Creating a slot and telling the renderer nothing writes past
     * the end of a mapped staging range.
     */
    void setInstances(const scene::InstanceTable* table);

    /**
     * @brief Slots were added to the table since the last call. Sizes the buffers and
     *        marks the acceleration structure, and does no work beyond that (G14).
     *
     * The cheap half of `setInstances`. That method resizes *and* rebuilds the whole
     * acceleration structure, which is the right answer when the geometry underneath it
     * changed and is 15 ms a call when only a row was added — `Engine::addInstance` is
     * called in a loop, so paying it per instance is a demo whose startup is five times
     * longer. The rebuild it defers happens once, in the next `rebuildAccelIfStale`.
     */
    void instancesGrew();

    /**
     * @brief Rebuild the acceleration structure if the scene tree has moved anything the
     *        static tier baked. Call once per frame, after `Scene::update`.
     *
     * A game creates an instance, attaches it to a node and says it has finished with
     * `setInstances` -- and the node's transform reaches the instance only on the next
     * sweep, which is after the structure was built from the transform `create` was given.
     * The demo did exactly this for twelve brazier pieces and left twelve unit cylinders
     * traced at the world origin: a hard elliptical shadow on the floor with nothing above
     * it, and a dark shape in the mirror the world does not contain.
     *
     * Costs one comparison per static geometry when nothing moved, which is every frame of
     * a scene that behaves, and nothing at all without ray tracing.
     */
    void rebuildAccelIfStale();

    /**
     * @brief The rig driving this scene, if it has one.
     *
     * Optional and separate, so a scene with no skin carries no skinning pass and a caller
     * driving a rig from something other than a glTF clip can hand over joint matrices
     * without satisfying an animation system it is not using.
     *
     * **Must be called after `setInstances`**; it sizes buffers from both.
     */
    void setAnimator(const scene::SceneAnimator* animator, const scene::GltfScene* scene);

    /**
     * @brief Name the scene's cloth, before `setAnimator` sizes the deformed buffer.
     *
     * **Order matters and is the caller's to keep.** `setAnimator` allocates
     * `skinnedVertices` and resolves `clothDestBase`, so a cloth named after it has nowhere
     * to write. `Engine` calls this first; nothing else calls either.
     */
    void setCloth(const scene::ClothSystem* cloth) { clothSystem = cloth; }

    /**
     * @brief The scene's particle emitters, and the pool sized for them.
     *
     * Optional and separate from `setScene`, so a scene with no emitter carries no particle
     * pass and a game spawning an explosion needs no asset edited to say so.
     *
     * Sizes every particle buffer from `system->capacity()`. **A capacity of zero allocates
     * nothing and records nothing**, so a scene with no emitters pays nothing.
     *
     * **Must be called after `setScene`**; the draw pipeline binds the scene's bindless
     * texture array.
     */
    void setParticles(const scene::ParticleSystem* system);

    /// Re-allocate the pool buffers at the system's current `capacity()`, carrying the
    /// particles in flight across. Paired with `ParticleSystem::grow` by
    /// `Engine::growParticles`, which is the only thing that should call either: growing one
    /// without the other emits into storage the device does not have.
    void resizeParticlePool();

    /**
     * @brief The sprites a game has created, drawn by `recordSprites`.
     *
     * The device half of `scene::SpriteTable`: the table owns the layers, the handles and
     * the order; this owns one storage buffer per frame in flight and one pipeline. Nothing
     * is sized from the scene, so a game with no scene at all draws sprites and a scene
     * with no sprites allocates nothing.
     *
     * **The textures come from `setImages`'s array, not the scene's.**
     */
    void setSprites(const scene::SpriteTable* table);

    /// Record and submit one frame.
    [[nodiscard]] FrameResult drawFrame(const scene::Camera& camera);

    void requestResize() { resizeRequested = true; }

    /// Clamped to what the device supports; rebuilds the G-buffer and pipelines.
    void setSampleCount(uint32_t samples);
    uint32_t sampleCount() const { return static_cast<uint32_t>(msaaSamples); }

    void setDebugView(DebugView view) { debugView = view; }
    DebugView currentDebugView() const { return debugView; }

    /// Step `step` places along the debug view list, wrapping, so a game binding "next
    /// view" to a key writes one call rather than a modulo over `DebugView::Count`.
    void cycleDebugView(int step) { debugView = advanceDebugView(debugView, step); }

    /**
     * @brief Whether the ray-traced paths can do anything on this device and scene.
     *
     * **Neither half is knowable from the settings table.** The device may lack the
     * extension trio, and a scene may have produced no acceleration structure; `rtEnabled`
     * is an ordinary bool somebody set, and it is silently inert without this.
     */
    [[nodiscard]] bool rayTracingAvailable() const { return ctx->rayQuerySupported && accel.valid(); }

    /**
     * @brief Write the next presented frame to `path` as a PNG.
     *
     * The copy is recorded into the frame about to be submitted, between the last pass and
     * the transition to PRESENT_SRC, so what lands on disk is what lands on screen.
     * **Reading the image back after presenting is a race**: `vkDeviceWaitIdle` drains
     * queues, and the presentation engine is not a queue.
     *
     * One request at a time; a second before the first is serviced replaces it. The frame
     * it lands on blocks on its own fence.
     */
    void requestCapture(std::filesystem::path path);

    /// False when the surface refused TRANSFER_SRC on its swapchain images, in which
    /// case requestCapture() will refuse rather than write a black PNG.
    bool captureAvailable() const { return swap.captureSupported; }

    /// Frames written since startup. Lets a benchmark run confirm the capture it
    /// asked for actually happened before it compares against a golden image.
    uint32_t capturesWritten() const { return captureCount; }

    /**
     * @brief Read an intermediate render target back to a PNG, by name.
     *
     * For everything the debug views cannot reach -- `ssaoRaw`, the bloom mip chain, the
     * TAA history, the IBL cubes, the shadow cascades. A G-buffer attachment routed through
     * the lighting pass is still the right way to see a resolved, tonemapped version of it.
     *
     * `mip` and `layer` of `UINT32_MAX` mean "all of them", writing `name.mipN.png` /
     * `name.layerN.png` beside `path`. Names come from `captureTargetNames()`.
     *
     * **Multisampled images are absent from the table**: `vkCmdCopyImageToBuffer` cannot
     * read one, and the G-buffer at MSAA is what the debug views are for.
     *
     * Refuses, with a reason, for an unknown name or a target whose pass is disabled.
     */
    void requestTargetCapture(const std::string& name, std::filesystem::path path, uint32_t mip = 0,
                              uint32_t layer = 0);

    /// Every name `requestTargetCapture()` accepts in this configuration, with the ones
    /// whose pass is currently off marked. For `--capture-target list` and error text.
    std::vector<std::string> captureTargetNames() const;

    /**
     * @brief Tee every presented frame to `sink` while it is recording.
     *
     * **Unlike `requestCapture`, this never blocks**: the copy goes into a per-slot staging
     * buffer and is read back at the top of the frame that reuses the slot, by which point
     * the fence the loop already waits on has made it ready. The pixels arrive two frames
     * late.
     *
     * The renderer does not own the recorder and does not start or stop it; it asks the
     * recorder what it is owed, and a frame that is not owed costs nothing.
     *
     * False means the swapchain cannot be read back or its format cannot be described
     * to an encoder, and the reason is logged.
     */
    bool startRecording(core::Recorder& sink, core::Recorder::Options options, core::AudioTap* audio);
    /// Stop teeing. Drains the slots still holding pixels, so the last frames before a
    /// quit are in the file rather than lost to the shutdown.
    void stopRecording();
    [[nodiscard]] bool recording() const { return recorder != nullptr; }


    /// Geometric specular antialiasing strength; 0 disables it -- and disables it
    /// properly, by compiling ENABLE_GSAA out of gbuffer.frag rather than multiplying
    /// by zero. Widens the specular lobe by the normal variance inside a pixel, which
    /// MSAA cannot fix because the aliasing is in the shading rather than at a
    /// geometric edge.
    float specularAaStrength = 0.5f;

    /**
     * @brief Edge-detect hybrid MSAA: shade once per pixel where every sample holds the
     *        same G-buffer fragment, per-sample only where they differ.
     *
     * Off restores the unconditional per-sample loop, which is the reference image as well
     * as the baseline -- the fast path is meant to produce the same pixels, not an
     * approximation of them. Nothing at 1x.
     */
    bool edgeMsaaEnabled = rowDefault::edgeMsaa;

    /**
     * @brief Temporal antialiasing.
     *
     * **Off by default**, because it makes the image a function of the last several frames
     * rather than of this one -- which costs the bit-identical-between-runs property the
     * golden images are built on. On quality per millisecond it wins, and a renderer whose
     * scene is static already runs 4x MSAA comfortably.
     *
     * TAA + 2x MSAA beats TAA alone: MSAA holds silhouettes stable in exactly the
     * disocclusion cases where the neighbourhood clamp throws the history away.
     */
    bool taaEnabled = rowDefault::taa;
    /// Weight given to the current frame. 0.1 is a ten-frame effective window, which
    /// is enough to converge the jitter sequence twice over; higher is sharper under
    /// motion and noisier at rest.
    float taaBlend = rowDefault::taaBlend;

    /**
     * @brief Screen-space reflections.
     *
     * Traces the depth buffer, so it reflects only what is already on screen and fades out
     * where a ray leaves the frame -- the trade for running on any GPU at a fraction of
     * what the traced path costs. Kept as a separate toggle rather than superseded by it.
     */
    bool ssrEnabled = rowDefault::ssr;
    /**
     * @brief Ray-traced reflections in place of the SSR march. Ignored where ray query is
     *        unavailable.
     *
     * The ambient term is the flat `ambientColor` in both the lighting pass and
     * `shadeRayHit`, which is what keeps a surface and its own reflection agreeing.
     */
    bool rtEnabled = rowDefault::rt;
    /**
     * @brief The sun's shadow map, and the tuning it needs. **The non-traced path only** --
     *        where `render.rt` is on, none of this runs.
     *
     * Both biases are in **world units** -- metres of offset, not NDC depth and not texels.
     * That is what fitting one box to the scene buys: a bias in world units converts to
     * this projection's depth range once, on the CPU, and means the same thing everywhere
     * in the map. Under cascades the same numbers meant four different world distances.
     *
     * `shadowDistance` 0 fits the box to the scene bounds. A positive value caps that side
     * length, concentrating texels for a scene far larger than its interesting part, at the
     * cost of leaving the world past it unshadowed.
     */
    bool shadowsEnabled = rowDefault::shadows;
    /**
     * @brief Shadows for point and spot lights, from the atlas above.
     *
     * Separate from `shadowsEnabled` because it is a separate cost: the sun is one pass,
     * this is up to 24. Off, punctual lights illuminate without occluding -- which in an
     * interior means no cast shadows at all.
     */
    bool punctualShadowsEnabled = rowDefault::punctualShadows;
    /**
     * @brief Shadow rays in the traced lighting pass. Ignored where ray query is
     *        unavailable.
     *
     * Separate from `rtEnabled` because they are separate passes with separate costs:
     * shadows are one ray per light per shaded sample, reflections one ray per pixel and
     * then a full BRDF at the hit. Off, every light in that pass reads as unoccluded, so
     * the delta is the ray cost and nothing else -- not something to ship off.
     *
     * **There is no map-based path beside this one.** The cascades and the punctual atlas
     * were removed rather than kept as a fallback, so a device without ray query renders
     * unshadowed; see engine/shaders/rayshadow.glsl.
     *
     * **The primary image only.** `shadeRayHit` shades reflection hits unshadowed, because
     * a shadow ray from a reflection hit is a second bounce and inline ray query has no
     * recursion to spend on one.
     */
    bool rtShadowsEnabled = rowDefault::rtShadows;
    /**
     * @brief Trace each distinct fragment's shadow rays once into a mask, ahead of
     *        lighting, instead of once per MSAA sample inside it.
     *
     * Does nothing at 1x, or wherever `rtEnabled` or `rtShadowsEnabled` is off -- there is
     * one sample to answer, or no ray to share. Off is the per-sample trace this replaced,
     * and is what the delta across it measures.
     *
     * **The mask is keyed per fragment, not per pixel**, so what it shares is one ray
     * between samples the G-buffer says came from one surface. That is an approximation
     * only where a shadow boundary crosses a silhouette pixel between two samples of the
     * same fragment; see engine/shaders/shadowmask.frag.
     */
    bool rtShadowMaskEnabled = rowDefault::rtShadowMask;
    /// Re-render an atlas layer only when its light, its layer assignment or the geometry
    /// changed. Off, every layer is redrawn every frame -- which is only useful for
    /// telling whether a stale layer is behind an artefact.
    bool shadowCacheEnabled = rowDefault::shadowCache;
    float shadowDistance = rowDefault::shadowDistance;
    /// Metres the depth test offsets an occluder along the light. Kills acne on surfaces
    /// near-parallel to the sun; too large and contact points detach. Authored rather than
    /// configured -- this is what a game that states nothing gets, and
    /// `Engine::initRenderer` assigns `GameSetup::look.shadowDepthBias` over it.
    float shadowDepthBias = 0.02f;
    /// Metres the lookup moves along the surface normal. Does what a larger depth bias
    /// would without detaching contact shadows, because it moves across the surface
    /// rather than along the light.
    float shadowNormalBias = 0.04f;
    /// World units a reflection *march* may travel before giving up.
    ///
    /// **Not a ray range, and the two must not be merged into one number.** This divides
    /// into `ssrSteps` to give a stride, so raising it coarsens the march rather than
    /// extending its reach. `rtMaxDistance` is the traced counterpart, and is large
    /// because a ray query costs the same at any range.
    float ssrMaxDistance = rowDefault::ssrMaxDistance;
    /// World units a reflection *ray* may travel. Large enough to cross any scene this
    /// engine has been pointed at: a ray that terminates early does not fade, it returns
    /// the environment cube, so a short range reads as a hard bubble of real reflection
    /// surrounded by sky.
    float rtMaxDistance = rowDefault::rtMaxDistance;
    /// How far behind a surface, in world units, the ray may be and still count as hitting
    /// it. Too small and rays tunnel through thin geometry; too large and a ray "hits" the
    /// far side of a column it passed beside.
    float ssrThickness = rowDefault::ssrThickness;
    float ssrIntensity = rowDefault::ssrIntensity;
    /// Surfaces rougher than this reflect nothing. A rough surface's reflection is a
    /// wide lobe that a single mirror ray cannot represent, and faking it by blurring
    /// the result is a much larger feature than this one.
    float ssrRoughnessCutoff = rowDefault::ssrRoughnessCutoff;
    /// Fraction of `renderExtent` the reflection pass traces at, 0.25 to 1.0.
    ///
    /// Read by the frame rather than applied by a setter: writing it marks the render
    /// targets dirty only when the *extent it produces* changes, so dragging the slider
    /// does not tear down and rebuild `ssrTarget` on every frame of the drag.
    float ssrScale = rowDefault::ssrScale;
    uint32_t ssrSteps = 32;
    /// Binary-search steps after the linear march finds a crossing. Six halves the
    /// stride sixty-four times over, which is well inside a pixel at these distances.
    uint32_t ssrRefineSteps = 6;

    /**
     * @brief Volumetric fog: single-scattering sunlight through participating media.
     *
     * **Off by default.** Unlike SSAO or bloom this corrects nothing -- it is media that
     * either exists in the scene or does not, and its density and height are authored
     * numbers rather than derivable ones.
     */
    bool fogEnabled = rowDefault::fog;
    /// Extinction per world unit at `fogBaseHeight`. Across a 30-unit interior, 0.02 is a
    /// haze and 0.2 is weather. **Per world unit**, so it scales inversely rather than with
    /// the lengths beside it.
    float fogDensity = rowDefault::fogDensity;
    /// Henyey-Greenstein g. Positive scatters forward, so the media brightens sharply
    /// when looking toward the sun -- which is what makes a light shaft a shaft.
    float fogAnisotropy = rowDefault::fogAnisotropy;
    float fogMaxDistance = rowDefault::fogMaxDistance;
    /// e-folding distance in world Y. 0 makes the media uniform, which reads as a dirty
    /// lens rather than as air.
    float fogHeightFalloff = rowDefault::fogHeightFalloff;
    /// World Y at which density is `fogDensity`. **Derived, not configured** -- `Engine`
    /// sets it from the scene's lower bound, so it has no settings row and must not grow
    /// one: a row over a field the next scene load rewrites appears to work and reverts.
    float fogBaseHeight = 0.0f;
    /// Steps per pixel. The march is the whole cost of this pass and scales linearly.
    uint32_t fogSteps = 32;

    /// GPU particles. On by default, which costs nothing to say: the pass is skipped
    /// entirely for a scene whose emitters sum to no particles. Off is what measures the
    /// whole subsystem in a scene that does have them.
    bool particlesEnabled = rowDefault::particles;

    /// Sort blended particles back to front. Off skips every pass of the bitonic network
    /// and draws the pool in whatever order the keys sit in, which measures *the sort*
    /// rather than the presence of a pass. Not a specialisation constant -- there is no
    /// shader branch to fold away, only dispatches not to record.
    bool particleSortEnabled = rowDefault::particleSort;

    TonemapOperator tonemapOperator = TonemapOperator::Aces;

    /// SSAO. Off makes the lighting pass fall back to the glTF occlusion texture
    /// alone, which is baked and cannot see anything the artist did not paint.
    bool ssaoEnabled = rowDefault::ssao;
    /**
     * @brief Hemisphere radius in world units. Contact scale: too large and it stops being
     *        contact occlusion and starts darkening whole walls.
     *
     * **A row, not a derivation from the scene bounds.** Contact occlusion is contact-scale
     * at every scene size; scaling this with the bounds gives a warehouse metre-deep
     * creases and a doorknob none.
     */
    float ssaoRadius = rowDefault::ssaoRadius;
    /// Pushes the depth comparison off the surface, in world units. Without it a flat wall
    /// occludes itself wherever precision puts a sample fractionally behind its neighbour.
    float ssaoBias = rowDefault::ssaoBias;
    /// Exponent on the result. Above 1 deepens contact shadows without widening them.
    float ssaoIntensity = 1.6f;
    /// Samples per pixel. 16 is noisy on its own; the 4x4 blur is what resolves it.
    uint32_t ssaoSamples = 16;

    /**
     * @brief A flat ambient added to every surface, as radiance: colour and magnitude in
     *        one vec3, black by default. Authored by the game -- see `GameSetup`.
     *
     * Multiplied by the same `occlusion` SSAO and the glTF occlusion texture produce, which
     * is what gives both of those something to attenuate; at black they are computed and
     * multiplied into nothing.
     *
     * **Diffuse only** -- a constant has no direction, so there is nothing honest to hand a
     * specular lobe, and a metal stays black unless a reflection reaches it.
     */
    glm::vec3 ambientColor{0.0f, 0.0f, 0.0f};

    /// Bloom. `bloomStrength` 0 skips the composite in tonemap.frag but still runs the
    /// chain; `bloomEnabled` false skips the passes as well.
    bool bloomEnabled = rowDefault::bloom;
    /// Scene-referred radiance above which a pixel starts to bleed. The lit scene sits
    /// near 1.0, so this is a little above "as bright as a white surface in sun".
    float bloomThreshold = rowDefault::bloomThreshold;
    /// Half-width of the soft ramp around the threshold. Without one, a highlight
    /// crossing the threshold pops, and that reads as flicker under camera motion.
    float bloomSoftKnee = rowDefault::bloomSoftKnee;
    /// How much of the chain lands in the final image. Bloom is an artefact of the lens,
    /// not of the scene, so a little goes a long way; past ~0.1 it stops reading as glare
    /// and starts reading as fog. **If an emitter is not blooming, check that its material
    /// strength reached the shader before raising this** -- a global default raised to
    /// compensate for one unlit emitter mis-lights every other scene.
    float bloomStrength = rowDefault::bloomStrength;

    GpuProfiler& gpu() { return gpuProfiler; }

    /// Log the most recent per-pass GPU timings.
    void logGpuTimings();
    const Swapchain& swapchain() const { return swap; }
    uint64_t frameCount() const { return framesSubmitted; }

    /// Draw the frame stats overlay. Reached by `--overlay` / `--no-overlay` through
    /// `Config::render.debugOverlay`, which `Engine::initRenderer` assigns over this; F6
    /// writes this field directly. **A capture turns it off unless a flag named it** -- a
    /// counter that changes every frame makes every golden comparison differ.
    bool debugOverlay = true;

    /// Extra text the application wants on screen, under the stats. **Drawn whether or not
    /// `debugOverlay` is on**, because a menu that only appears when the HUD does is a menu
    /// with a hidden dependency. Not a UI system -- that is `uiDrawList`.
    std::vector<std::string> overlayLines;

    /**
     * @brief The application's UI for this frame, as vertices and clip ranges.
     *
     * `ui::Context` fills a `DrawList` in pixel space and this is where it is handed over.
     * The renderer neither knows nor asks what a button is.
     *
     * Drawn through the overlay's own pipeline, after the stats text, with one
     * `vkCmdSetScissor` per command. There is no `ui_rect` pipeline and no second pass --
     * see `ui/FontMetrics.h` for why four texels replaced both.
     *
     * Cleared by the application each frame, like `debugLines`.
     */
    const ui::DrawList* uiDrawList = nullptr;

    /// The glyph table the overlay was built with, so the application can lay a UI out in
    /// the same font the renderer will draw it in. Metrics only -- the atlas stays here.
    [[nodiscard]] const ui::FontMetrics& fontMetrics() const { return debugFont.metrics(); }

    /**
     * @brief The images a game has loaded, whose residency this renderer maintains.
     *
     * The device half of `gfx::ImageTable`: the table owns the slots, the generations and
     * the free list; this owns the `GpuImage` behind each one, the descriptor array they
     * live in and the sampler. Set once, before any frame; the renderer reconciles against
     * `ImageTable::revision()` at the top of every `drawFrame`.
     *
     * A null table is a renderer that draws text and rectangles and no imagery.
     */
    void setImages(const ImageTable* table);

    /// How many images one descriptor set on this device can hold, slot zero included, and
    /// what `ImageTable::init` is given. `maxPerStageDescriptorSampledImages` against
    /// `maxDescriptorSetSampledImages`, whichever binds first -- **a device limit, not an
    /// engine constant**. Valid after `init`.
    [[nodiscard]] uint32_t maxImageSlots() const { return imageSlotCeiling; }

    /**
     * @brief How many texels the file behind `id` turned out to have, or `{0, 0}`.
     *
     * Here rather than on `ImageTable` because the renderer is the half that decodes.
     * **Reconciles residency first**: the table hands out a handle immediately and the
     * renderer catches up at the top of the next `drawFrame`, so a caller that loaded in
     * `Game::init` would otherwise find nothing behind a perfectly valid handle.
     */
    [[nodiscard]] glm::uvec2 imageSize(ImageId id);

    /**
     * @brief Register a shader variant and get the index a material stores.
     *
     * Call it once, in `Game::init`, and put the result in `GpuMaterial::shader`. **Nothing
     * is compiled here**: the variant's pipelines are built the first time a draw command
     * carrying its index reaches a pass, so declaring one a level never uses costs a struct
     * in a vector, and the first use costs one hitched frame.
     *
     * Variant 0 is the engine's own -- `gbuffer.frag`, `shadow.frag`, `forward.frag` --
     * registered before any game can, so the first index a game gets is 1 and a material
     * nobody assigned is already the default.
     *
     * **A shader named here that does not exist is not diagnosed until first use**, and
     * then it aborts naming both directories it searched.
     */
    uint32_t addShaderVariant(ShaderVariant variant);

    /// How many variants are registered, variant 0 included. `GpuMaterial::shader` past
    /// this is clamped to 0 with one warning rather than indexing off the end.
    [[nodiscard]] uint32_t shaderVariantCount() const { return static_cast<uint32_t>(variants.size()); }

    /**
     * @brief The size in pixels of the surface a caller draws onto. **Changes on every
     *        resize**, so a caller cannot cache the config's window size.
     *
     * **The surface, not the window.** With a virtual resolution set and `uiInsideVirtual`
     * true, "the screen" a panel is laid out against is 320x180 rather than 1920x1080, and
     * handing back the window would put every widget off the right-hand edge at 6x.
     * `windowExtent()` is the other number.
     */
    [[nodiscard]] uint32_t framebufferWidth() const { return uiExtent().width; }
    [[nodiscard]] uint32_t framebufferHeight() const { return uiExtent().height; }

    // --------------------------------------------------------------------- presentation

    /**
     * @brief Render at this size and present it at the largest integer scale that fits,
     *        letterboxed. `{0, 0}` renders at the window extent.
     *
     * Set from `GameSetup::present.virtualResolution` before `init`, honoured from the next
     * `createRenderTargets` -- which is every resize, because the scale is a function of
     * the window. Native is this path with `V` equal to the window, which makes the scale 1
     * and the blit an identity `recordPresent` skips.
     */
    VkExtent2D virtualExtent{};

    /// Whether the overlay, the UI and the debug lines are drawn *into* the virtual target
    /// -- scaled up with the world, and pixel-art crisp -- or onto the window afterwards at
    /// its full resolution. From `GameSetup::present.uiInsideVirtual`. A game whose HUD is drawn in
    /// the same 8-pixel font as its sprites wants the first; one that wants readable text
    /// over a chunky world wants the second.
    bool uiInsideVirtual = true;

    /**
     * @brief Take the frame's remaining sub-texel machinery out of the 2D path.
     *
     * TAA and its jitter, the tonemap curve and the overlay's filtering each destroy
     * pixel-exactness on their own, so this is one switch rather than three. `Engine::run`
     * forces `render.taa` off and `render.tonemap` to `clamp` through the settings table;
     * **this member is only what the renderer still owns**, the overlay image array's
     * sampler -- `VK_FILTER_NEAREST` instead of linear.
     *
     * Linear stays right for a UI icon drawn at whatever height the layout gives it, which
     * is why this is a switch rather than a correction.
     */
    bool pixelExact = false;

    /// The window's own framebuffer size. What the swapchain is, what a capture reads back,
    /// and what the presentation step fits the virtual target into.
    [[nodiscard]] VkExtent2D windowExtent() const { return swap.extent; }
    /**
     * @brief The views a game asked for. The *device half* of `gfx::ViewTable` (C34).
     *
     * The table owns the lifetime and the camera; this owns the destination image, the
     * uniform block it fills and the descriptor its result lands in, and reconciles
     * against `revision()` exactly as `setImages`/`syncImages` do. `Engine` calls this
     * once; nothing else should.
     */
    void setViews(const ViewTable* table) {
        views = table;
        viewRevision = 0;
    }

    /// What every pass before the presentation step renders at.
    [[nodiscard]] VkExtent2D renderTargetExtent() const { return view.renderExtent; }
    /// The surface the overlay and the UI draw onto: the virtual target or the window.
    [[nodiscard]] VkExtent2D uiExtent() const { return uiInsideVirtual ? view.renderExtent : swap.extent; }
    /// Where the virtual target lands in the window, in whole texels.
    [[nodiscard]] const PresentLayout& present() const { return view.presentPlan; }

    /**
     * @brief A window pixel in the coordinates the UI was laid out in.
     *
     * The cursor arrives in window pixels and the UI may have been built against a
     * 320x180 target presented at 6x in the middle of a letterbox, so without this every
     * hit test in a virtual-resolution game is wrong by the scale and the bars. Identity
     * whenever the UI is outside the virtual target or the presentation is native, which
     * is why `Engine` can call it unconditionally.
     */
    [[nodiscard]] glm::vec2 uiFromWindow(glm::vec2 windowPixel) const;

    /**
     * @brief A window pixel in render-target pixels -- what a picking ray is built from.
     *
     * The same inverse-presentation arithmetic `uiFromWindow` does and **without its
     * gate**: the UI may have been laid out against the window, but the scene is drawn into
     * the virtual target either way, so a ray that skipped this in a letterboxed run would
     * be off by the scale and the bars. Identity for a native presentation.
     */
    [[nodiscard]] glm::vec2 renderTargetFromWindow(glm::vec2 windowPixel) const;

    /**
     * @brief Draw `image` at 1:1 in the top-left of the overlay's surface, for `--readback`.
     *
     * **Placed by the renderer at exact texel coordinates rather than through
     * `ui::Context`**: routing it through the widget layout would put the theme's padding
     * and the UI scale between the source file and the answer, and a failure would not say
     * which of the three moved.
     *
     * Takes the handle rather than a slot, unlike `ui::DrawList::image`, because the
     * renderer holds the `GpuImage` and is the only half that knows the texel count. An
     * invalid handle clears the draw rather than drawing the fallback.
     */
    void setReadbackImage(ImageId id);

    /**
     * @brief Where the camera is, drawn with the stats and phrased as a command line.
     *
     * Separate from `overlayLines` because it belongs to the stats: it appears and
     * disappears with F6, and a binding menu on screen should not push it around. The
     * renderer holds no camera, so the application fills this each frame or leaves it
     * empty.
     *
     * The point is reproduction -- the six numbers `--camera` takes turn "it pops when I
     * walk forward here" into a run anyone can repeat.
     */
    std::string cameraLine;

    /**
     * @brief World-space lines to draw over the finished frame.
     *
     * A plain vertex vector the application refills each frame, which keeps the renderer
     * free of any dependency on physics: `PhysicsWorld::drawDebug` writes into this, and a
     * game drawing its own lines writes into the same one. Empty costs a branch.
     *
     * **Drawn without a depth test, deliberately.** A collision shape is almost always
     * *inside* the mesh it describes, so a depth-tested wireframe of a box collider on a
     * box is either hidden or z-fighting. Drawing over everything is what makes a collider
     * that has drifted away from its mesh visible at a glance.
     */
    std::vector<DebugLineVertex> debugLines;

    /**
     * @brief Decals to project into the G-buffer (3.3).
     *
     * Empty by default and populated from the config, because there is no decal content
     * in this repository: Sponza ships none, and inventing a stain to put on its floor
     * would be the sort of help nobody asked for. The pass costs nothing when this is
     * empty -- it is a `for` loop over no elements.
     */
    std::vector<Decal> decals;

    /**
     * @brief GPU frustum culling (4.2).
     *
     * On by default: it cannot change the image, only what is submitted to produce it,
     * which is why the golden set is the test for it. Off runs the same dispatch with
     * every command marked visible, so switching it measures the frustum test rather
     * than the presence of the pass.
     */
    bool cullingEnabled = rowDefault::culling;
    /// Two-pass Hi-Z occlusion culling (C11). Off leaves the two passes drawing exactly
    /// what one pass drew, which is what the golden suite proves before this is turned on.
    bool occlusionCullingEnabled = rowDefault::occlusionCulling;
    /// Screen-coverage LOD selection (C17). On by default and on for the same reason
    /// occlusion is: with it off the code rots, and with it on every golden run re-proves
    /// that no reference camera stands far enough out to select a coarser level.
    bool meshLodEnabled = rowDefault::meshLod;
    /// Fraction of the viewport an instance's projected bounds must cover to stay at
    /// LOD 0; see `scene::lodCoverageThresholds` for what the levels below it are.
    float meshLodThreshold = rowDefault::lodThreshold;
    /// Tiled light assignment (C35). Off skips `recordLightTiles` and has the light
    /// loops walk every light in the view, which is what the three rows above it are for
    /// and for the same reason: the frame either way must be the same frame.
    bool lightTilesEnabled = rowDefault::lightTiles;

    /// Sun and point lights, tweakable from the app.
    glm::vec3 sunDirection{-0.35f, 0.85f, 0.4f};
    glm::vec3 sunColorValue{1.0f, 0.96f, 0.88f};
    float sunIntensity = 3.0f;
    float exposure = 1.0f;
    /// Every light in the scene except the sun, which stays a separate member because
    /// the cascades need to know which light they were fitted to. The sun is written
    /// into the buffer as element 0 each frame.
    std::vector<GpuLight> lights;

    /**
     * @brief How many lights may be shaded in one frame, sun included.
     *
     * **A stated budget, not a hidden capacity.** Set before `init()`, which sizes the
     * storage buffer from it, so this is the only cap.
     *
     * A scene with more lights does not lose whichever came last in vector order: it keeps
     * the most important by `lightImportance()` and **reports how many it dropped**.
     */
    uint32_t lightBudget = kDefaultLightBudget;

    /// @brief Radiance below which the deferred light loop drops a light, in **post-exposure**
    ///        units -- what the tonemap sees, not what the light emits.
    ///
    /// 0 is off and is the default. `updateUniforms` divides by `exposure` on the way to the
    /// shader, so a value chosen against one game's look survives another's.
    float lightCutoff = rowDefault::lightCutoff;

    /**
     * @brief Recompile shaders from source and rebuild every pipeline when a file under
     *        either shader tree changes.
     *
     * Deliberately dumb: it polls the newest write time across the shader directory once a
     * second and, on any change, re-runs glslangValidator over *every* source and rebuilds
     * *every* pipeline. No dependency tracking -- a `.glsl` fragment is included by half the
     * tree, so the graph that would save the other half is more machinery than the ~200 ms.
     *
     * A failed compile leaves the previous SPIR-V in place and logs the error, so a syntax
     * error costs a message rather than a black screen.
     *
     * **The environment bake is re-run too**, even though it is baked once at startup: a
     * hot reload that silently ignores four of the twenty-eight shaders looks like it works.
     */
    bool shaderHotReload = false;

  private:
    struct FrameSync {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        /**
         * @brief One block per view, not one per frame.
         *
         * Two views recorded into one command buffer read their matrices at *submit*
         * time, so a single block would have the second view's `updateUniforms` rewrite
         * what the first view's already-recorded draws are going to read — and the first
         * view would silently render with the second view's camera. **That is the
         * failure that looks like it works**: one view is right, the other is subtly
         * wrong, and nothing is invalid.
         *
         * Indexed by `View::uniformSlot`. The light and shadow-matrix buffers are here
         * for the same reason and not a weaker one: `updateLights` ranks and culls
         * against the camera, so its outputs are per view by construction.
         */
        GpuBuffer uniforms[kMaxViews];
        /// Host-visible and rewritten every frame: lights move, and a staged upload
        /// for a few kilobytes would cost more than it saved.
        GpuBuffer lightBuffer[kMaxViews];
        /// One view-projection per atlas layer, filled by updateLights.
        GpuBuffer shadowMatrixBuffer[kMaxViews];
        /// The three buffers above, bound as one set. Per view, because they are.
        VkDescriptorSet frameSet[kMaxViews]{};
        /// Host-visible and rewritten every frame; the overlay is a few hundred quads.
        GpuBuffer overlayVertices;
        /// Host-visible and rewritten every frame, for the same reason: a wireframe of the
        /// physics world is rebuilt from scratch each time it is drawn. Allocated only when
        /// something asks for lines.
        GpuBuffer debugLineVertices;
        /// Host-visible, rewritten every frame, one `vkCmdCopyBuffer` into the deformed
        /// vertex range this frame's cloth solve produced. Sized from the scene by
        /// `setAnimator`, and absent for a scene with no fabric.
        GpuBuffer clothStaging;
        /// This frame's sprites, host-visible: what a sprite pass writes is written once and
        /// read once, so staging it would be staging a copy to save a copy. Sized by
        /// `ensureSpriteCapacity`, and null until a game creates its first sprite.
        GpuBuffer spriteBuffer;
        /// One binding, the buffer above. Allocated with it, and rewritten when it grows.
        VkDescriptorSet spriteSet = VK_NULL_HANDLE;
        /// `SpriteTable::revision()` this slot's `spriteBuffer` was last filled from, the
        /// same gate `instanceRevision` is for the instance array. Zero is "this buffer
        /// holds nothing a revision can describe", which is what growing it, swapping the
        /// table under it or recreating the frame all reduce to.
        uint64_t spriteRevision = 0;

        // ------------------------------------------------------------- instances
        /**
         * @brief Everything the scene passes read per object, in one device-local
         *        allocation: instances, world bounds, opaque commands, blended commands.
         *
         * **Device-local, and do not make it host-visible to skip the copy.** That looks
         * free -- written once per revision, read once per frame -- and measured as a 33%
         * regression in the shadow pass (0.431 -> 0.567 ms median), turning a steady zone
         * into one varying 0.51-0.82: every vertex of eleven geometry passes fetches its
         * 128-byte instance record across PCIe. The copy costs microseconds once; the read
         * costs a fraction of a microsecond several million times.
         *
         * One buffer with four regions rather than four buffers: they are written together,
         * copied together, and need one barrier between the copy and the passes that read
         * them. Region offsets are on the Renderer -- they are the same for every slot.
         */
        GpuBuffer instanceData;
        /// Host-visible mirror of the CPU-written prefix of `instanceData`, copied on
        /// the graphics queue at the top of the frame. Shorter than `instanceData`:
        /// the culled command output is written by the GPU and never staged.
        GpuBuffer instanceStaging;
        /// Instances that survived culling, per view. Written by the cull dispatch with
        /// atomicAdd and read back two frames later, when this slot's fence guarantees
        /// the frame that wrote it has finished. Host-visible for that reason, and it
        /// is 13 integers.
        GpuBuffer cullStats;
        /// Reads `commandBounds` and `commandTemplate`, writes `commandsOut` and
        /// `cullStats` -- all four inside this frame's own buffers.
        VkDescriptorSet cullSet = VK_NULL_HANDLE;
        /// One uint per command, persisting across frames within this slot. Never read by
        /// the host; the first frame zero-fills it to "everything visible".
        GpuBuffer commandVisibility;
        /// Set when the buffer is (re)made, cleared once the next frame has filled it.
        bool commandVisibilityInit = false;
        /// Reads the scene's bind-pose vertices and this frame's joint matrices, writes the
        /// skinned vertices. Per frame because the joint matrices are.
        VkDescriptorSet skinSet = VK_NULL_HANDLE;
        /// Table revision these buffers were last filled from. `0` is "never", which
        /// no live table reports.
        uint64_t instanceRevision = 0;
        /// `Renderer::variantAssignment` the command list was grouped against. Its own
        /// counter beside the table's because the two move for different reasons: a
        /// material changing which variant draws it regroups every command without any
        /// instance having moved.
        uint64_t variantAssignment = 0;
        /// Material-table revision this slot's copy of the scene's material buffer was
        /// filled from. Per slot only because each slot records its own command buffer --
        /// one write per revision per slot is what gets a mutated material to every frame
        /// in flight without re-uploading a table that never moves.
        uint64_t materialRevision = 0;
        /// Commands written into the opaque region last time it was rebuilt. **Skinned
        /// after static**, so a pass can draw each half with its own vertex buffer bound;
        /// within each half, grouped by shader variant, and within a variant unmasked
        /// before masked, so the shadow pass can draw the unmasked run with no fragment
        /// shader. `opaqueRanges` names every group.
        uint32_t opaqueCommandCount = 0;
        uint32_t staticCommandCount = 0;
        /// Every group in this slot's command list, ordered by variant rather than by
        /// position, so a draw loop binds one pipeline per variant instead of one per
        /// variant per half. Rebuilt only when `updateInstances` rebuilds the commands.
        std::vector<VariantRange> opaqueRanges;
        /// Set when the staging buffer holds bytes `instanceData` does not. Cleared
        /// once the copy has been recorded.
        bool instanceUploadPending = false;
    };

    /// Cap on quads per overlay frame; 492 KB per frame in flight. **Stated, not silent**
    /// -- overflow drops the excess and says so once, rather than truncating a menu into
    /// something that looks complete.
    static constexpr uint32_t kMaxOverlayQuads = 4096;

    /// Cap on debug line *vertices* per frame, so 16384 lines; 512 KB per frame in flight.
    /// **Stated, not silent**, like the overlay's.
    static constexpr uint32_t kMaxDebugLineVertices = 32768;

    /// Declared here and defined with the members it owns, because the two functions
    /// below take one and a parameter type has to be declared before it is named.
    struct View;

    void createFrameResources();
    void destroyFrameResources();

    /// Fill `v` with every image, storage view and descriptor set sized from `extent`.
    /// **Takes the view and the extent rather than reading either off the object**: the
    /// body already worked entirely from a local `extent`, so this is what it was doing.
    void createRenderTargets(View& v, VkExtent2D extent);
    /// The exact inverse of the above, and the authoritative list of what a View owns.
    void destroyRenderTargets(View& v);
    /// The extent a view renders at: `virtualExtent` where a game set one, the window
    /// otherwise. Read by every caller of createRenderTargets, so the rule lives once.
    [[nodiscard]] VkExtent2D primaryViewExtent() const {
        return (virtualExtent.width != 0 && virtualExtent.height != 0) ? virtualExtent : swap.extent;
    }
    /// The presenting view's targets **and** every registered view's set and destination.
    /// One function because there are four callers, and a fifth that did only the first
    /// half would leave a mirror sampling a destroyed image. A view that named its own
    /// extent is rebuilt at that one, so a window resize does not resize an inset.
    void createViewTargets();
    void destroyViewTargets();

    /// Run the whole IBL chain: sky, mips, irradiance, prefilter, BRDF LUT. Once at
    /// startup, and again on a shader hot reload.
    void createIblResources();
    /// The shadow map and its comparison sampler. Once, at init: nothing here depends on
    /// the swapchain, so it does not belong in createRenderTargets().
    void createShadowResources();
    void destroyShadowResources();
    /// Fit the sun's orthographic box to the scene, filling `sunViewProj`, the biases in
    /// `shadowParams` and `cullViewProj[1]`. Deliberately takes no camera.
    void updateSunShadow(FrameUniforms& u);
    void recordShadows(VkCommandBuffer cmd, uint32_t slot);
    void recordPunctualShadows(VkCommandBuffer cmd, uint32_t slot);
    void destroyIblResources();

  public:
    /**
     * @brief Re-bake the environment if the sun has moved since it was baked.
     *
     * `createIblResources` runs from `init`, before a scene or a game has said what the sun
     * is, so the cube it bakes is lit by whatever `sunDirection` held at that moment.
     * `Engine::initLights` calls this once it has resolved the real one. A no-op when they
     * agree, which is the common case; a blocking submit of a 512^2 cube and a five-mip
     * chain when they do not.
     */
    void rebakeIblIfSunMoved();

  private:
    /// What `createIblResources` last baked with -- see `rebakeIblIfSunMoved`. Deliberately
    /// not initialised to `sunDirection`'s default: a bake has to have happened for these to
    /// mean anything, and `createIblResources` is what writes them.
    glm::vec3 iblBakedSun{0.0f};
    glm::vec3 iblBakedColor{0.0f};
    float iblBakedIntensity = 0.0f;


    void createDescriptorLayouts();
    void createPipelines();
    void destroyPipelines();

    /// The shading id space from features.glsl, as `GraphicsPipelineDesc::constants`
    /// wants it. Shared by the lighting pass and by every variant's forward pipeline,
    /// and a method rather than a local because the second of those is built lazily.
    [[nodiscard]] std::vector<uint32_t> shadingConstants() const;

    /**
     * @brief Every specialisation-constant input packed into one comparable value.
     *
     * Compared against `builtFeatureKey` at the top of each frame, so flipping a feature
     * toggle rebuilds the pipelines that read it without anyone having to remember a
     * `featuresChanged()` call. One hitched frame is the entire cost.
     *
     * **There is no permutation cache and must not be one.** Nine flags is 512 pipelines
     * enumerated; instead there is one live pipeline per (variant, pass), created the
     * moment a draw asks for it and thrown away wholesale when a constant changes.
     */
    uint64_t featureKey() const;
    uint64_t builtFeatureKey = 0;

    /// Newest write time seen under the shader source directory, as a raw tick count,
    /// with INT64_MIN for "not polled yet". One number rather than a per-file map: any
    /// edit moves the maximum, and a deleted shader means the next build is broken
    /// anyway. Not defaulted to 0 -- see pollShaderReload() for why that is a trap.
    int64_t newestShaderWrite = INT64_MIN;
    std::chrono::steady_clock::time_point lastShaderPoll{};
    /// Recompiles every shader source into memory, via `gfx::overrideShaderBinary`, and
    /// leaves no file behind: the build's shader directories are never written, so a cold
    /// start can only run SPIR-V the build produced. False if any shader failed, in which
    /// case that one publishes nothing -- the module already bound stays bound -- and the
    /// error has already been logged.
    [[nodiscard]] bool recompileShaders() const;
    /// Polls mtimes and, on a change, recompiles and rebuilds. Called once per frame;
    /// the directory scan itself is rate-limited to once a second.
    void pollShaderReload();

    /**
     * @brief Debug-only: reflect each module and check it against the layouts it was built
     *        against. Compiled to nothing in Release.
     *
     * Catches a binding added to a shader and not to the hand-written
     * `VkDescriptorSetLayout`, or a specialisation constant declared and never given a
     * value. **An assertion rather than a generator** -- generating layouts from reflection
     * would hide what is bound.
     */
    void verifyShaderBindings(const char* pass, std::initializer_list<const char*> shaders,
                              std::initializer_list<VkDescriptorSetLayout> sets,
                              size_t constantCount) const;

    /**
     * @brief Create a pipeline layout, and check the shaders against the same set list.
     *
     * **The set list is written once**, rather than as a `VkDescriptorSetLayout[]` for the
     * create info and again as a brace-list to `verifyShaderBindings` with a hand-written
     * `setLayoutCount` beside them. Nothing checked that those three agreed, and the
     * failure mode is a validation error at draw time.
     *
     * Passing no shaders skips the verification, for the few layouts whose pipelines are
     * built elsewhere.
     *
     * **Private, and stays private.** It knows nothing about what a pass *is* -- the name
     * is a string for an error message.
     */
    [[nodiscard]] VkPipelineLayout createLayout(const char* pass, std::initializer_list<const char*> shaders,
                                                std::initializer_list<VkDescriptorSetLayout> sets,
                                                std::initializer_list<VkPushConstantRange> pushConstants = {},
                                                size_t constantCount = 0) const;

    void updateUniforms(const scene::Camera& camera, uint32_t slot);

    // ------------------------------------------------------------------ instances
    /**
     * @brief Grow the per-frame instance buffers to hold `slots` entries.
     *
     * Sized from the table rather than fixed, so there is no instance count this silently
     * drops. **Growth waits for the device**, because the buffers being replaced may be in
     * flight -- so it doubles each time rather than fitting exactly, to keep it rare.
     */
    void ensureInstanceCapacity(uint32_t slots);

    /**
     * @brief Bring the descriptor array in line with the image table.
     *
     * Runs at the top of `drawFrame`, and only when `ImageTable::revision()` has moved.
     * Decodes and uploads what was loaded, releases what was destroyed, grows the array
     * when it has run out, and rewrites every descriptor.
     *
     * **It waits for the device**, because a descriptor a frame in flight may still read
     * cannot be rewritten and the image behind it cannot be freed. The alternative is a
     * per-frame retirement list, which this engine does not have -- `ensureInstanceCapacity`
     * and `GltfScene::unloadModel` take the same trade. The trigger to write one is a
     * caller that loads images *per frame during play*.
     */
    void syncImages();
    /// Build `overlaySetLayout` with `slots` descriptors in its one binding, replacing
    /// whatever was there. Marks the pipelines dirty when it replaced something, because
    /// a layout of a different width is a different layout to everything built against
    /// it. The argument for sizing it to the capacity is on the definition.
    void createOverlaySetLayout(uint32_t slots);
    /// Grow the overlay's descriptor array to `slots`, doubling. Rebuilds the layout, the
    /// pool and the set, so every descriptor is invalid afterwards and
    /// `writeImageDescriptors` has to follow. Callers wait for the device first; this
    /// does not.
    void ensureImageCapacity(uint32_t slots);
    /// Write all `imageCapacity` descriptors: the font atlas at zero, a loaded image where
    /// there is one, the atlas everywhere else. **Every slot, never partially bound.**
    void writeImageDescriptors();

    /// Write the table into this slot's staging buffer if it has changed since it was
    /// last written, and rebuild the opaque indirect commands with it.
    void updateInstances(uint32_t slot);
    /// Sort blended instances back to front and write their indirect commands into
    /// staging. Returns how many were written.
    uint32_t buildBlendedCommands(uint32_t slot, const scene::Camera& camera);
    /// One command per live, opaque, dynamic instance, static ones first. Its own list
    /// rather than a fifth partition of the opaque one, which is already cut four ways by
    /// (static|skinned) x (unmasked|masked) -- a dynamic bit crossing all four would take
    /// it to eight ranges to serve one pass.
    uint32_t buildVelocityCommands(uint32_t slot);
    void recordVelocity(VkCommandBuffer cmd, uint32_t slot);
    /// Copy staging into the device-local buffer and barrier it for the vertex stage,
    /// the indirect command processor and compute. The first thing the frame's command
    /// buffer records.
    void recordInstanceUpload(VkCommandBuffer cmd, uint32_t slot);
    /// Re-upload the scene's material table when a game has changed one (G4).
    void recordMaterialUpload(VkCommandBuffer cmd, uint32_t slot);
    /// Point one frame slot's skinning set at the buffers it reads. Its own function
    /// because two of the six are the scene's, and those move when its geometry grows.
    /// `allocate` is false for the refresh path, which must not create a set for a slot
    /// whose instance buffers do not exist yet.
    void writeSkinSet(uint32_t slot, bool allocate);
    /**
     * @brief Which command lists a cull dispatch fills.
     *
     * `Scene` is every view -- the camera, the sun and each atlas layer -- and runs **once
     * per frame, before the shadow passes**, because those passes draw straight out of
     * lists 1.. and a cull recorded after them would have them draw last frame's commands.
     * That is not hypothetical: moving the cull into the per-view chain did exactly it, and
     * the `no-rt` golden case is what said so -- it is the only case that rasterises
     * shadows, so it is the only one that could.
     *
     * `Camera` is list 0 alone, which is what a second view needs and all it needs: the
     * sun's box and a light's frustum are not properties of a camera.
     */
    enum class CullViews { Scene, Camera };
    /// One dispatch per view, writing this frame's culled commands (4.2).
    /// @param phase 0 before the depth exists, 1 against the pyramid built from it.
    void recordCull(VkCommandBuffer cmd, uint32_t slot, uint32_t phase, CullViews which = CullViews::Scene);
    /// Byte offset of view `v`'s commands inside `instanceData`.
    [[nodiscard]] VkDeviceSize viewCommandOffset(uint32_t view) const {
        return outRegion + static_cast<VkDeviceSize>(view) * instanceCapacity * sizeof(VkDrawIndexedIndirectCommand);
    }

    /// How many slots each frame's instance buffers can hold. Zero until the first
    /// setInstances().
    uint32_t instanceCapacity = 0;
    /// Byte offsets of the regions inside `FrameSync::instanceData`. Each is aligned to
    /// `kInstanceRegionAlign`, which covers every `minStorageBufferOffsetAlignment` a
    /// desktop driver reports and the 4-byte alignment an indirect buffer offset needs.
    ///
    /// The order is not arbitrary: everything the CPU writes comes first, so the copy
    /// from staging is one contiguous range and the GPU-written command output sits
    /// past its end.
    static constexpr VkDeviceSize kInstanceRegionAlign = 256;
    VkDeviceSize boundsRegion = 0;    ///< GpuCommandBounds, one per command
    VkDeviceSize templateRegion = 0;  ///< un-culled opaque commands, as the CPU built them
    VkDeviceSize blendedRegion = 0;   ///< blended commands, in depth order, never culled
    VkDeviceSize jointRegion = 0;     ///< joint matrices for every character (4.4, S2.3)
    VkDeviceSize weightRegion = 0;    ///< morph weights for every character (S2.1)
    VkDeviceSize prevRegion = 0;      ///< last frame's model matrix per slot (3.4)
    VkDeviceSize velocityCmdRegion = 0; ///< dynamic-only commands for the velocity pass (3.4)
    VkDeviceSize outRegion = 0;       ///< kCullViews command lists, written by cull.comp
    VkDeviceSize stagedBytes = 0;     ///< prefix of instanceData the CPU writes
    VkDeviceSize instanceDataBytes = 0;

    // ------------------------------------------------ deformation (4.4, S2.1, S2.3)
    /// Not owned. Null for a scene with no rig, which is what makes the whole pass and
    /// its buffers conditional on the asset rather than on a config flag.
    const scene::SceneAnimator* animator = nullptr;
    /// Joints across every character. Sizes the joint region; zero when there is no rig.
    uint32_t jointCapacity = 0;
    /// Morph weights across every character. Sizes the weight region (S2.1).
    uint32_t weightCapacity = 0;
    /// Deformed output, one `Vertex` per vertex of every skinned or morphed *instance*.
    /// Bound as vertex buffer 0 for the deformed half of each geometry pass.
    GpuBuffer skinnedVertices;
    /// Per-vertex joint indices and weights, uploaded once at load. Covers only the
    /// primitives that carry them.
    GpuBuffer skinInfluences;
    /// Per-target, per-vertex morph displacements, uploaded once at load (S2.1).
    GpuBuffer morphDeltas;
    uint32_t skinnedVertexCount = 0;
    /// Where slot `i`'s deformed vertices start in `skinnedVertices`, or UINT32_MAX.
    /// Assigned when the table changes, and read when the commands are built.
    std::vector<uint32_t> skinDestBase;
    /// One dispatch's worth of arguments. Rebuilt with the commands.
    struct SkinBatch {
        uint32_t sourceBase = 0;
        uint32_t destBase = 0;
        uint32_t influenceBase = 0;
        uint32_t jointBase = 0;
        uint32_t vertexCount = 0;
        uint32_t morphBase = 0;
        uint32_t morphTargets = 0;
        uint32_t weightBase = 0;
    };
    std::vector<SkinBatch> skinBatches;
    /// Not owned, and null for every scene that authors no `FABRIC_` mesh (C19).
    const scene::ClothSystem* clothSystem = nullptr;
    /// Where cloth `i`'s vertices start in `skinnedVertices`, in vertices, or UINT32_MAX.
    /// Resolved once in `setAnimator` out of `skinDestBase` and the cloth's instance slot,
    /// so the per-frame path is a `memcpy` and a `VkBufferCopy` with nothing to look up.
    std::vector<uint32_t> clothDestBase;
    /// This frame's copy regions, one per cloth. A member so the storage stops growing
    /// after the first frame rather than being rebuilt from nothing every one of them.
    std::vector<VkBufferCopy> clothCopies;
    VkDescriptorSetLayout skinSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout skinLayout = VK_NULL_HANDLE;
    VkPipeline skinPipeline = VK_NULL_HANDLE;
    void createSkinPipeline();
    void destroySkinPipeline();
    void destroySkinResources();
    /// Skin every skinned instance into `skinnedVertices`, before any pass that
    /// draws the result.
    void recordSkinning(VkCommandBuffer cmd, uint32_t slot);
    /// Copy every cloth's solved vertices into the frame's staging buffer and build the
    /// copy regions. True when there is something to copy; see the definition.
    bool recordClothUpload(uint32_t slot);
    /// Does anything in this scene write into `skinnedVertices`? The test the deformed
    /// command sweeps use, and it is not `animator != nullptr` any more: cloth deforms with
    /// no rig at all (C19).
    [[nodiscard]] bool deforms() const {
        return animator != nullptr || (clothSystem != nullptr && !clothSystem->empty());
    }
    /// Submit view `view`'s commands, one indirect draw per variant group: the static half
    /// from the scene's vertex buffer, the skinned half from `skinnedVertices`, and
    /// `pass`'s pipeline for each variant bound as the walk reaches it. **Every geometry
    /// pass calls this rather than `vkCmdDrawIndexedIndirect`**, which keeps the
    /// skinned/static split and the variant selection in one place.
    void drawSceneIndirect(VkCommandBuffer cmd, uint32_t slot, uint32_t view, VariantPass pass);

    // ----------------------------------------------------------------- shader variants
    /// A registered variant and the pipelines it has been asked for so far. The handles are
    /// null until a draw needs one and null again after `destroyPipelines()`, so a feature
    /// toggle and a hot reload rebuild a game's shaders by forgetting them rather than by
    /// knowing about them.
    struct Variant {
        ShaderVariant desc;
        VkPipeline gbuffer = VK_NULL_HANDLE;
        VkPipeline shadow = VK_NULL_HANDLE;
        VkPipeline forward = VK_NULL_HANDLE;
    };
    /// **Variant 0 is the engine's own and exists before anything can call
    /// `addShaderVariant`**, which is what makes a zero-initialised `GpuMaterial` name the
    /// default rather than an unregistered index.
    std::vector<Variant> variants{Variant{ShaderVariant{"engine"}}};

    /// The full pipeline description for one of a variant's three pipelines. Stated here
    /// rather than at three creation sites, so a variant and the engine's own pipelines
    /// cannot end up with different depth state.
    [[nodiscard]] GraphicsPipelineDesc variantDesc(const ShaderVariant& v, VariantPass pass) const;
    /// `variant`'s pipeline for `pass`, built on first use. Called from inside a record
    /// method, which is legal and is where the hitch lands.
    VkPipeline variantPipeline(uint32_t variant, VariantPass pass);

    /// Which variant each material selects, refreshed when the scene's material revision
    /// moves. **What decides whether a command rebuild is owed at all**: a game animating a
    /// material's colour bumps the material revision every frame, and re-grouping every
    /// command for a value that did not change is what this avoids.
    std::vector<uint32_t> materialVariant;
    /// Material revision `materialVariant` was refreshed from.
    uint64_t seenMaterialRevision = 0;
    /// Bumped only when a refresh actually changes an entry. Compared per frame slot
    /// beside the instance revision, so the two together say whether the command list
    /// still describes the scene.
    uint64_t variantAssignment = 1;
    /// One byte per registered variant, non-zero where a live instance uses it. Scratch
    /// for the command builder, kept as a member only so a rebuild does not allocate.
    std::vector<uint8_t> variantsPresent;
    /// Said once, not once per frame: a material naming a variant nobody registered.
    bool reportedVariantOverflow = false;

    // ------------------------------------------------------------------------ particles
    /// Not owned. Null, or a system whose capacity is zero, is what makes every pass below
    /// conditional on the *scene* rather than on a config flag.
    const scene::ParticleSystem* particles = nullptr;
    /// Slots the pool holds. **Always a power of two** -- a bitonic network sorts a power
    /// of two or nothing, and its domain is the whole pool.
    uint32_t particleCapacity = 0;
    /// log2(particleCapacity). The width of the slot field in a sort key, and therefore
    /// what is left over for the quantised depth.
    uint32_t particleIndexBits = 0;
    /**
     * @brief Distance at which a sort key's depth saturates, and how far behind a surface a
     *        particle may be and still count as having hit it.
     *
     * **Both derive from the scene's diagonal in `setScene()`**, so neither has a settings
     * row and neither should grow one -- a row over a field `setScene` rewrites appears to
     * work and reverts on the next load. Unlike `ssaoRadius`, the right answer here is a
     * function of scene size and nothing else.
     *
     * The initialisers hold until the first `setScene`.
     */
    float particleSortRange = 100.0f;
    float particleCollisionThickness = 0.5f;

    /// The pool itself and its keys, both device-local and both *persistent* rather
    /// than per-frame-in-flight: a particle's state this frame is its state last frame
    /// integrated once, so there is one simulation and not two.
    GpuBuffer particlePool;
    GpuBuffer particleKeys;
    /// Per frame in flight, host-visible, rewritten every frame: a handful of emitters
    /// and the frame's births. Kilobytes, written once and read once, which is exactly
    /// the case a staging copy costs more than it saves.
    GpuBuffer particleEmitterBuffers[kFramesInFlight];
    GpuBuffer particleSpawnBuffers[kFramesInFlight];
    VkDescriptorSet particleSets[kFramesInFlight]{};

    VkDescriptorSetLayout particleSetLayout = VK_NULL_HANDLE;
    /// Five sets: frame, particles, the TLAS set, IBL, depth -- in that order because
    /// ibl.glsl already names set 3 and set 2 kept its slot when the shadow maps that
    /// used to live there were ripped out.
    VkPipelineLayout particleComputeLayout = VK_NULL_HANDLE;
    /// Three: frame, particles, and the scene's bindless textures.
    VkPipelineLayout particleDrawLayout = VK_NULL_HANDLE;
    VkPipeline particleEmitPipeline = VK_NULL_HANDLE;
    VkPipeline particleSimulatePipeline = VK_NULL_HANDLE;
    VkPipeline particleSortPipeline = VK_NULL_HANDLE;
    VkPipeline particleSortLocalPipeline = VK_NULL_HANDLE;
    VkPipeline particleDrawPipeline = VK_NULL_HANDLE;

    /// Buffers and descriptor sets, sized from the system. Separate from the pipelines
    /// because a shader hot reload rebuilds those and must not reset the simulation.
    void createParticleResources();
    void destroyParticleResources();
    void destroyParticleResourcesKeepingPool();

    /// Reallocate the per-frame per-view light buffers when a view wanted more lights than
    /// they hold, and rewrite the descriptors that name them (C40). Called at the top of a
    /// frame, before anything records: mid-frame it would be a use-after-free.
    void growLightBuffer();
    void createParticlePipelines();
    void destroyParticlePipelines();
    /// Simulate, emit, sort, draw -- in that order, and the order is argued for in
    /// particle_emit.comp. After the forward pass, so blended geometry is already in
    /// the target to blend against.
    void recordParticles(VkCommandBuffer cmd, uint32_t slot);
    /// Last reported spawn-drop count, for the same reason `reportedLightDrops` exists:
    /// a pool over budget would otherwise emit one warning per frame.
    uint32_t reportedParticleDrops = 0;

    VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout = VK_NULL_HANDLE;
    VkPipeline cullPipeline = VK_NULL_HANDLE;
    void createCullPipeline();
    void destroyCullPipeline();

    /// View-projection per cull view, filled in updateUniforms. Index 0 is the camera's,
    /// 1 the sun's and 2.. the punctual atlas's.
    ///
    /// **Only index 0 is contested by a second camera view**, which is what makes one a
    /// 27th list rather than a second set of 26 — `updateSunShadow` deliberately takes no
    /// camera, and an atlas layer is a light's frustum. Growing this array is C33's, not
    /// this row's: a 27th entry is a 27th dispatch in every frame, including the ones with
    /// only one view in them.
    glm::mat4 cullViewProj[kCullViews];

    /// Derived from the table when it changes, reported every frame. Properties of the
    /// scene rather than of a frame, so they are not accumulated by whichever frame
    /// slot happened to rebuild its commands.
    uint32_t opaqueDrawCalls = 0;
    uint32_t opaqueInstanceCount = 0;
    uint64_t opaqueTriangles = 0;
    /// Blended commands written for the frame being recorded. Rebuilt every frame, so
    /// unlike the opaque count it lives here rather than in FrameSync.
    uint32_t blendedCommandCount = 0;
    /// The blended list's variant groups, and **the one place they are runs rather than
    /// groups**: depth order is the point of this pass, so the list cannot be sorted by
    /// variant and a variant appearing twice in the depth order gets two entries.
    /// `unmasked` means nothing here and is left zero.
    std::vector<VariantRange> blendedRanges;

    /// Fills lightScratch and uploads it into `view.uniformSlot`'s block. Called from
    /// updateUniforms so the record path only reads.
    ///
    /// **Runs for every view; only the atlas assignment inside it is the primary's.** A
    /// secondary looks its lights up in `lightShadowLayer` instead of assigning layers, and
    /// leaves `shadowMatrixScratch` and the staleness cache alone.
    /// @param viewPosition Ranks the light budget when it binds. Passed rather than stored
    ///        because the only caller already holds the camera.
    void updateLights(uint32_t slot, const glm::vec3& viewPosition, const glm::mat4& viewProj);
    /// @param phase 0 clears and draws the previously-visible set; 1 loads and draws
    ///              whatever the occlusion test newly admitted.
    void recordGBuffer(VkCommandBuffer cmd, uint32_t slot, uint32_t phase);
    void recordDecals(VkCommandBuffer cmd, uint32_t slot);
    /// The four colour attachments and the depth, moved to their read-only layouts. Its
    /// own step because two passes read them -- the shadow mask and lighting -- and the
    /// second of the two would otherwise name a layout the first had already left.
    void recordGbufferRead(VkCommandBuffer cmd);
    void recordShadowMask(VkCommandBuffer cmd, uint32_t slot);
    void recordLighting(VkCommandBuffer cmd, uint32_t slot);
    void recordForward(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& camera);
    void recordSsao(VkCommandBuffer cmd, uint32_t slot);
    void recordSsr(VkCommandBuffer cmd, uint32_t slot);
    void recordFog(VkCommandBuffer cmd, uint32_t slot);
    void recordBloom(VkCommandBuffer cmd, uint32_t slot);

    /// Pipelines and sampler are resolution-independent; the chain itself is not, so
    /// it is built in createRenderTargets() and these are built once in init().
    void createBloomPipelines();
    void destroyBloomPipelines();
    void createSsaoPipelines();
    void destroySsaoPipelines();
    void createTaaPipeline();
    void destroyTaaPipeline();
    void recordTaa(VkCommandBuffer cmd, uint32_t slot);
    /// Into `composeImage`, at `renderExtent`.
    void recordTonemap(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex);

    /// Where the chain being recorded is composed: the view's own destination if it has
    /// one, then the offscreen target where the presentation blit is real, then the
    /// swapchain image itself where it was elided (P2). **Three cases, still one test in
    /// one place** -- the emptiness of an image -- rather than a flag each pass has to
    /// agree with. A view that names its own destination is the third answer from the
    /// pair that already answered the other two.
    [[nodiscard]] VkImage composeImage(uint32_t imageIndex) const {
        if (view.destination.image != VK_NULL_HANDLE) return view.destination.image;
        return view.presentTarget.image != VK_NULL_HANDLE ? view.presentTarget.image : swap.images[imageIndex];
    }
    [[nodiscard]] VkImageView composeView(uint32_t imageIndex) const {
        if (view.destination.image != VK_NULL_HANDLE) return view.destination.view;
        return view.presentTarget.image != VK_NULL_HANDLE ? view.presentTarget.view : swap.views[imageIndex];
    }
    /// Draw `debugLines` over the tonemapped image, between the tonemap and the overlay,
    /// so a wireframe is drawn over the scene and text over both.
    ///
    /// **`target` and `extent` rather than a swapchain index**: with the UI inside the
    /// virtual target this draws into an offscreen image at 320x180, and with it outside it
    /// draws into the swapchain at the window's size *after* the blit.
    void recordDebugLines(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent,
                          const scene::Camera& camera);
    void recordOverlay(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent);

    /**
     * @brief Draw `sprites` over the tonemapped image, one instanced draw for all of them.
     *
     * **After the tonemap, not before.** An unlit sprite is display-referred art -- the
     * texel in the file is the texel the artist chose -- and exposure, a curve and a resolve
     * would apply three corrections to a value that needs none. Downstream of the tonemap
     * is the only place in the frame a texel reaches the swapchain unaltered.
     *
     * **What that costs**: a sprite is not occluded by 3D geometry, does not bloom, is not
     * reflected by SSR and is not fogged. A lit sprite is the opposite trade.
     *
     * Always into the *virtual* target at `renderExtent`, as `recordDebugLines` is -- a
     * sprite is world-space content. Only the overlay gets `uiInsideVirtual`'s choice.
     *
     * No depth attachment and no depth test: the CPU sort is the order, and a blended
     * surface cannot depth-write without occluding what is behind it.
     *
     * **The copy into the mapped buffer is gated on `SpriteTable::revision()`**, the same
     * rule the instance array has. A screen of sprites that did not change costs no copy;
     * one where anything changed costs the whole array.
     */
    void recordSprites(VkCommandBuffer cmd, uint32_t slot, VkImageView target, const scene::Camera& camera);

    /// Grow the per-frame sprite buffers to hold `count`, doubling. `vkDeviceWaitIdle`
    /// rather than a retirement list, as `ensureInstanceCapacity` and `syncImages` also do.
    /// **Called from `drawFrame` before recording begins**, never from `recordSprites` -- a
    /// device wait inside an open command buffer is not a thing to discover later.
    void ensureSpriteCapacity(uint32_t count);

    /**
     * @brief Blit the virtual target into the swapchain at an integer scale, letterboxed.
     *
     * **A blit rather than a fullscreen draw, for correctness.** `presentTarget` carries the
     * swapchain's own `_SRGB` format, so `vkCmdBlitImage` between two images of identical
     * format at `VK_FILTER_NEAREST` moves bytes -- no sampling, no filter weights, no
     * colour-space arithmetic. A fullscreen quad sampling an `_SRGB` view decodes every
     * texel to linear and lets the attachment encode it back, and an 8-bit sRGB round trip
     * is the off-by-one this exists to prevent.
     *
     * Records nothing when the layout is the identity -- see `identityPresent`.
     */
    void recordPresent(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex);

    // ------------------------------------------------------------------- capture
    /// Allocate the readback buffer for a pending capture. False cancels the request:
    /// capture unavailable, an undecodable swapchain format, or an allocation that
    /// failed. Each case logs why, because a screenshot that silently does not happen
    /// is worse than one that fails.
    [[nodiscard]] bool beginCapture();
    /// Block on `fence`, write the PNG, and free the readback buffer.
    void finishCapture(VkFence fence);

    /// `--readback`'s image, its size in texels, and slot 0 meaning none. A slot rather
    /// than an `ImageId` because `Engine` has already resolved it -- see `setReadbackImage`.
    uint32_t readbackSlot = 0;
    uint32_t readbackWidth = 0;
    uint32_t readbackHeight = 0;

    std::filesystem::path capturePath;
    bool captureRequested = false;
    /// Allocated per capture rather than kept alive: at 1600x900 this is 5.8 MB, and
    /// a screenshot happens on a keypress, not on a frame.
    GpuBuffer captureStaging;
    uint32_t captureCount = 0;

    // ------------------------------------------------------------- intermediate targets
    /// One row per render target reachable by name. **Built fresh on request rather than
    /// cached**: the images are recreated on every resize, so a stale pointer here is a
    /// use-after-free that only fires when someone drags a window.
    struct TargetEntry {
        const char* name;
        const GpuImage* image;
        /// The layout the target is in once the frame's passes have finished. **Stated, not
        /// inferred**: `recordCaptureCopy` transitions *from* it, and a wrong value is
        /// undefined behaviour rather than a wrong picture.
        VkImageLayout layout;
        VkImageAspectFlags aspect;
        /// False when the pass that writes this target is switched off, in which case the
        /// image holds the previous frame's contents or nothing. Refusing is the point.
        bool live;
    };
    std::vector<TargetEntry> captureTargets() const;

    /// Empty means no target capture is pending. Held as a name rather than a resolved
    /// pointer for the same reason the table is rebuilt: a resize between the request
    /// and the frame that services it invalidates every image.
    std::string targetCaptureName;
    std::filesystem::path targetCapturePath;
    /// Subresource within the named target. UINT32_MAX means "every one of them", which
    /// is what makes a bloom chain or a cascade array one request instead of five.
    uint32_t targetCaptureMip = 0;
    uint32_t targetCaptureLayer = 0;
    /// One staging buffer per subresource in flight for the pending request.
    struct PendingSubresource {
        GpuBuffer staging;
        std::filesystem::path path;
        VkExtent2D extent;
        VkFormat format;
    };
    std::vector<PendingSubresource> targetPending;
    void recordTargetCapture(VkCommandBuffer cmd);
    void finishTargetCapture(VkFence fence);

    // ------------------------------------------------------------------------ recording
    /// Where presented frames go, or null. Owned by `Engine`; the renderer only asks it
    /// what it is owed and hands it bytes.
    core::Recorder* recorder = nullptr;
    /// One readback buffer per frame slot, allocated for the life of the recording -- not
    /// the one-per-capture allocation `captureStaging` uses, which is right for a
    /// screenshot and wrong thirty times a second.
    GpuBuffer recordStaging[kFramesInFlight];
    /// Frames the slot's buffer is owed to the recorder, or 0 when it holds nothing.
    /// Carried across the two frames between recording the copy and reading it back.
    uint32_t recordSlotRepeat[kFramesInFlight]{};
    /// Start of the recording, which is what `Recorder::framesOwed` measures from.
    std::chrono::steady_clock::time_point recordStart{};
    /// Hand slot `slot`'s pixels to the recorder and mark it empty. Called after the
    /// fence wait at the top of a frame, which is what makes the data ready without a
    /// fence of its own -- the same trick `cullStats` uses one line above.
    void drainRecordSlot(uint32_t slot);

    [[nodiscard]] FrameResult handleResize();

    VulkanContext* ctx = nullptr;
    GLFWwindow* window = nullptr;
    const scene::GltfScene* scene = nullptr;
    /// What to draw. Not owned; main() outlives the renderer.
    const scene::InstanceTable* instances = nullptr;
    /// Retained only so a shader hot reload can re-run the environment bake. main()
    /// keeps the uploader alive past the render loop, so this outlives every use.
    Uploader* uploader = nullptr;

    bool vsyncEnabled = true;
    bool resizeRequested = false;
    bool pipelinesDirty = false;
    /// Whether `rebuildAccelIfStale` has said so. Once per run, not once per rebuild.
    bool staleAccelReported = false;
    /// The set of instances changed; rebuild at the next `rebuildAccelIfStale`. Set by
    /// `instancesGrew` and by `setInstances`, cleared by the rebuild. A flag rather than a
    /// comparison because `staticTierStale` walks the slots the structure *baked*, so a
    /// slot that appeared since is invisible to it — and a flag rather than an immediate
    /// build because both setters are called in loops.
    bool accelDirty = false;
    /// Whether `createPipelines` has ever run. Only the first `setScene` has to build them
    /// eagerly; every later one marks `pipelinesDirty` and lets the frame rebuild once.
    bool pipelinesBuilt = false;
    /// Only the sample count needs these rebuilt. A feature toggle changes which
    /// pipelines exist, not which images do, and tearing down 300 MB of render targets
    /// to answer a keypress would be a needless hitch.
    bool renderTargetsDirty = false;

    Swapchain swap;
    GpuProfiler gpuProfiler;
    /// False when the device has no timestamp support, which makes every GPU zone
    /// read 0.000 ms. Reported rather than printed as zeros.
    bool gpuTimingAvailable = false;

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_4_BIT;
    DebugView debugView = DebugView::Lit;

    /**
     * @brief Everything a frame draws into, at one extent, with the descriptor sets that
     *        name it.
     *
     * The seventeen images sized from the view's own extent, and the views, sets and
     * derived extents that would dangle if they moved without them.
     * `createRenderTargets` fills one and `destroyRenderTargets` is its exact inverse --
     * **the two lists must stay identical**, which is the only invariant here a compiler
     * cannot check.
     *
     * **One per live view, each at that view's own extent** (C38). `Renderer::view` is
     * whichever set is being recorded right now, and a chain swaps its slot's in and out
     * around itself — so every field here, the scalar state included, follows the view it
     * describes with no copying list to keep in step.
     *
     * What is deliberately *not* here: the IBL cubes, the BRDF LUT and both shadow maps.
     * Those are properties of the scene rather than of a view, and a second view shares
     * every one of them -- a View that swallowed the shadow atlas would re-render 24
     * layers per view for no reason. Pipelines, layouts and samplers are
     * resolution-independent and stay on the Renderer for the same kind of reason.
     */
    struct View {
        /// **Where this view's tonemap lands, when it is not the swapchain's.** Empty for
        /// the view that presents, which then falls through to `presentTarget` or the
        /// swapchain image exactly as before — `composeImage`/`composeView` answer three
        /// cases now instead of two, and it is still one test in one place.
        ///
        /// Owned by `syncViews` for a registered view, and empty for the one that
        /// presents. **`destroyRenderTargets` does not free it**: a window resize rebuilds
        /// the targets while the descriptor array still names this image, so it is
        /// released where the image slot it is bound into is given up.
        GpuImage destination;
        /// Whether this is the view that presents. Decides three things and they are all
        /// the same decision: it runs the full cull rather than the camera alone, it is
        /// the one whose light ranking owns the shadow atlas, and it is the only one whose
        /// tonemap can reach the swapchain -- which is why `createRenderTargets` reads it
        /// to decide whether a `presentTarget` is worth allocating.
        bool primary = true;
        /// Whether TAA runs for the chain being recorded. `taaEnabled` is the user's
        /// switch and this is what the passes read; they agree for every view now that the
        /// history pair is per view, and the split stays because the jitter, the resolve
        /// and the tonemap's source set are three reads of one decision.
        bool taaActive = false;
        /// Which of a frame slot's `kMaxViews` uniform blocks this view reads, and the
        /// only thing that decides which `frameSet` a pass binds. Every record method
        /// reaches it through the view it is already recording, so no pass needed a
        /// signature for it.
        uint32_t uniformSlot = 0;
        /// Last frame's unjittered view-projection **for this view**, uploaded as
        /// `FrameUniforms::prevViewProj`. Reprojecting view B against view A's matrix is
        /// a smear that only appears once the camera moves, which is why it is here
        /// rather than beside the TAA pipeline it feeds.
        glm::mat4 prevViewProj{1.0f};
        /// Instances that survived this view's cull, from the last completed frame in
        /// this slot. Reported by the overlay; two frames stale by construction, which
        /// for a HUD number is not worth a second fence to fix.
        uint32_t visibleInstances = 0;
        /// Triangles those instances draw at the LOD level the cull selected (C17). Same
        /// staleness and the same buffer; it is the measured half of what a chain buys.
        uint32_t visibleTriangles = 0;

        // G-buffer, all at msaaSamples.
        GpuImage gAlbedo;
        GpuImage gNormal;
        GpuImage gOrm;
        /// Emissive radiance. R11G11B10 rather than RGBA8 so it can exceed 1.0 and
        /// feed bloom; same 4 bytes either way.
        GpuImage gEmissive;
        GpuImage gDepth;
        /// Single-sample copy of gDepth, produced by a depth resolve as the G-buffer pass
        /// stores. The forward pass draws into the already-resolved HDR target, and one
        /// render pass cannot mix sample counts, so it cannot test against gDepth itself.
        /// Empty at 1x, where there is nothing to resolve.
        GpuImage gDepthResolved;
        /// Whichever of the two above the forward pass tests against. Set in
        /// createRenderTargets() so the record path has no sample-count branch.
        VkImage forwardDepth = VK_NULL_HANDLE;
        VkImageView forwardDepthView = VK_NULL_HANDLE;
        /// Single-sample: the per-sample resolve has already happened by this point.
        GpuImage hdrTarget;

        // ----------------------------------------------------------------- presentation
        /// What every pass draws at: `virtualExtent` where a game set one, the window's
        /// extent otherwise. **Every `renderArea`, viewport, dispatch round-up and
        /// inverse-texel push constant reads this**, so a view's resolution is decided
        /// once and everything sized from it agrees by construction.
        VkExtent2D renderExtent{};
        /// Recomputed with the render targets, which is every resize -- the scale is a
        /// function of the window and therefore changes under the user's hands.
        PresentLayout presentPlan{};
        /**
         * @brief Where the tonemap draws when the blit is not an identity. Empty otherwise.
         *
         * **Carries `swap.format` rather than a format of its own**, which is what lets the
         * blit be a byte move. Its emptiness *is* the elision test at record time; a second
         * bool would be a second thing to keep in step with `identityPresent`.
         */
        GpuImage presentTarget;

        // ------------------------------------------------------------------------- ssao
        /// Raw AO, then the blurred result the lighting pass actually reads. Two images
        /// rather than one blurred in place: the blur reads a neighbourhood, so writing
        /// into its own source would make the result depend on dispatch order.
        GpuImage ssaoRaw;
        GpuImage ssaoBlurred;
        /// Half `renderExtent`, and the pass runs at it. Held rather than recomputed in
        /// recordSsao: the dispatch, the push constant's texel and the two images must all
        /// agree, and deriving it in two places is how they stop agreeing.
        VkExtent2D ssaoExtent{};
        VkImageView ssaoRawStorage = VK_NULL_HANDLE;
        VkImageView ssaoBlurStorage = VK_NULL_HANDLE;
        VkDescriptorSet ssaoSet = VK_NULL_HANDLE;
        VkDescriptorSet ssaoBlurSet = VK_NULL_HANDLE;

        // ------------------------------------------------------------------ shadow mask
        /// Traced shadow visibility, one bit per light, one array layer per MSAA sample.
        /// `renderExtent` and `msaaSamples` layers, always allocated: the lighting shader
        /// declares the descriptor whether or not the pass runs, exactly as it does for
        /// the AO buffer, and ENABLE_SHADOW_MASK gates the read.
        GpuImage shadowMask;
        /// The 2D_ARRAY view both passes bind. One image and one view, written as a
        /// storage image and read as one, so it never leaves VK_IMAGE_LAYOUT_GENERAL and
        /// the only thing between the write and the read is a memory barrier.
        VkImageView shadowMaskStorage = VK_NULL_HANDLE;
        VkDescriptorSet shadowMaskSet = VK_NULL_HANDLE;

        // ------------------------------------------------------------------------ bloom
        /// Half-resolution mip chain. Bloom is a wide, low-frequency effect, so running it
        /// at full resolution costs four times the bandwidth to produce an image that is
        /// blurred past the point where the difference survives.
        GpuImage bloomChain;
        /// One per mip: `imageStore` needs a single-mip view, and the sampled path uses
        /// bloomChain.view with an explicit LOD instead.
        VkImageView bloomStorageViews[kBloomMips]{};
        /// [0] thresholds hdrTarget into mip 0; [i>0] halves mip i-1 into mip i.
        VkDescriptorSet bloomDownSets[kBloomMips]{};
        /// [i] adds mip i+1 back onto mip i. The last entry is unused.
        VkDescriptorSet bloomUpSets[kBloomMips]{};

        // ------------------------------------------------------------------ Hi-Z pyramid
        /// Min-reduced depth, power-of-two sized, rebuilt from **this** frame's depth
        /// between the two G-buffer passes. A pyramid one frame stale is what makes
        /// single-pass occlusion culling drop things that just became visible.
        GpuImage depthPyramid;
        VkImageView depthPyramidStorage[kDepthPyramidMips]{};
        VkDescriptorSet depthPyramidSets[kDepthPyramidMips]{};
        VkExtent2D depthPyramidExtent{};
        uint32_t depthPyramidLevels = 0;
        /// The pyramid's sampled-only view, handed to the cull dispatch. Its own set
        /// rather than a binding in `cullSet`, because the pyramid is recreated with the
        /// render targets while `cullSet` is rewritten with the instance capacity;
        /// coupling them lets a resize leave the cull set pointing at a destroyed view.
        VkDescriptorSet hizSet = VK_NULL_HANDLE;

        // --------------------------------------------------------- light tiles (C35)
        /// One bit per light per `kLightTileSize` tile, `Renderer::lightTileWords` words
        /// per tile. Device-local: written by a dispatch and read by the two passes after
        /// it, never by the host.
        ///
        /// **Per view rather than per frame slot, and that is the same rule the G-buffer
        /// follows.** Its extent decides the tile count, so it is rebuilt by
        /// `createRenderTargets` on every resize -- which a `FrameSync` member could not
        /// be, since frame resources survive a swapchain rebuild.
        GpuBuffer lightTiles;
        /// Tiles across and down. Held rather than recomputed at record time for the
        /// reason `ssaoExtent` is: the dispatch, the push constant and the buffer's size
        /// must agree, and deriving that in two places is how they stop agreeing.
        VkExtent2D lightTileGrid{};
        /// The buffer above, as one storage-buffer binding. Bound by the build dispatch
        /// that writes it and by the two shading passes that read it.
        VkDescriptorSet lightTileSet = VK_NULL_HANDLE;

        // -------------------------------------------------------------------------- ssr
        /// Reflection radiance only, not the composited image: the pass writes what to add
        /// and a fullscreen additive draw adds it, which keeps hdrTarget out of being both
        /// the source a ray samples and the destination it writes.
        GpuImage ssrTarget;
        /// `renderExtent` scaled by `ssrScale`, and the pass runs at it. Held for the same
        /// reason `ssaoExtent` is: the dispatch, the push constant's texel and the image
        /// must agree, and deriving it twice is how they stop agreeing. Equal to
        /// `renderExtent` at scale 1.0, which is what selects the full-resolution
        /// composite.
        VkExtent2D ssrExtent{};
        VkImageView ssrStorage = VK_NULL_HANDLE;
        VkDescriptorSet ssrImageSet = VK_NULL_HANDLE;
        VkDescriptorSet ssrCompositeSet = VK_NULL_HANDLE;

        // -------------------------------------------------------------------------- fog
        /// Premultiplied in-scattered radiance in rgb, opacity in alpha. Shares the
        /// composite pipeline layout and shader with SSR; only the blend state differs.
        GpuImage fogTarget;
        VkImageView fogStorage = VK_NULL_HANDLE;
        VkDescriptorSet fogImageSet = VK_NULL_HANDLE;
        VkDescriptorSet fogCompositeSet = VK_NULL_HANDLE;

        // -------------------------------------------------------------------------- taa
        /// Ping-ponged: [p] is written this frame and read as history the next. Two images
        /// rather than one, because the resolve reads a *reprojected* texel -- some other
        /// pixel's -- so writing in place would race with a neighbour's read.
        GpuImage taaHistory[2];
        VkImageView taaHistoryStorage[2]{};
        /// [p] reads the current HDR image and history[1-p], and stores into history[p].
        VkDescriptorSet taaSet[2]{};
        /// [p] hands history[p] plus the bloom chain to the tonemap pass, in place of the
        /// `hdrSet` it binds when TAA is off. One duplicated set is what lets the resolve
        /// ping-pong without a full-screen copy back into hdrTarget every frame.
        VkDescriptorSet taaOutputSet[2]{};
        uint32_t taaHistoryIndex = 0;
        /// False until a frame has been resolved into the history. Set false by anything
        /// that invalidates it: a resize, a resolution change, or TAA being switched on.
        /// **History is per view** -- a shared one smears each view into the other.
        bool taaHistoryValid = false;

        // ----------------------------------------------------------------- motion vectors
        /// How far reprojecting depth is wrong because the object moved too, in UV. RG16F
        /// and **single-sample** -- a per-fragment quantity gains nothing from
        /// multisampling, and one render pass cannot mix sample counts, which is what makes
        /// this a pass of its own rather than a fifth G-buffer attachment. 5.5 MiB at
        /// 1600x900 against ~23 MB at 4x.
        ///
        /// **Zero is the identity, so the clear is the static-geometry path.** Nothing
        /// writes it unless the table holds a dynamic instance.
        GpuImage velocityTarget;

        // ------------------------------------------------------------- sets over the above
        /// The G-buffer attachments as sampled inputs to the lighting pass, and the
        /// composed HDR image as the tonemap's input. Both name images in this view, which
        /// is what puts them here rather than beside the layouts they were allocated from.
        VkDescriptorSet gbufferSet = VK_NULL_HANDLE;
        VkDescriptorSet hdrSet = VK_NULL_HANDLE;
        /// The single-sample scene depth -- the MSAA resolve at 4x, gDepth itself at 1x --
        /// bound through `singleImageSetLayout`. Shared by the decal pass and the particle
        /// simulate dispatch, which is why it is named after the resource rather than after
        /// the first pass that wanted it.
        VkDescriptorSet sceneDepthSet = VK_NULL_HANDLE;
    };

    /**
     * @brief The presenting view's targets, and — for the length of one chain — whichever
     *        view is being recorded.
     *
     * **A set per view, each at its own extent** (C38). A chain swaps its slot's `View`
     * into here, records, and swaps it back, so every `view.` in the twenty-odd record
     * methods reaches the right images without any of them learning that views exist. The
     * presenting view's set lives here between frames, which is what lets the capture
     * paths, `renderTargetExtent()` and `captureTargets()` keep meaning the frame that was
     * presented.
     *
     * What that costs is the thing to know before creating one: ~224 MiB at 1600x900 and
     * 4x MSAA, scaling with the pixel count. C33 shared one set serially to avoid it and
     * bought a single extent for every view with the saving; the extent is worth more,
     * because four views at half the side cost what one full-size view does.
     */
    View view;

    /**
     * @brief A view a game asked for: its targets, and the bookkeeping that says whether
     *        they still describe what the table asked for.
     */
    struct ViewSlot {
        /// Everything this view draws into, at its own extent, plus the state that has to
        /// survive between frames: the matrix TAA reprojects against, its history index
        /// and the cull counters the overlay reports.
        View targets;
        /// The extent `targets` was built at. Compared rather than remembered as a flag,
        /// so a `ViewTable::resize` and a window resize under a view that follows it are
        /// one test instead of two rules.
        VkExtent2D builtExtent{};
        /// Where the destination is bound in the image descriptor array, which is the slot
        /// `ViewTable::create` adopted. Zero means "not bound", the fallback slot's index.
        uint32_t imageSlot = 0;
        /// The table generation this residency was built for. What makes a slot destroyed
        /// and reacquired between two syncs a rebuild rather than a stale image left live.
        uint32_t generation = 0;
        bool live = false;
    };
    /// Secondary views, indexed by `ViewTable` slot, recorded before the primary and never
    /// presented. Empty in every golden case, which is what makes a moved pixel there a
    /// statement about the one-view path rather than about this list.
    std::vector<ViewSlot> extraViews;
    const ViewTable* views = nullptr;
    uint64_t viewRevision = 0;
    /// Bring `extraViews` in line with the table. At the top of `drawFrame`, after
    /// `syncImages`, because it writes into the array that one sizes.
    void syncViews();
    /// Pixels a table entry renders at: what it asked for, or the presenting view's where
    /// it asked for nothing. One function because `syncViews` and `createViewTargets` must
    /// not be able to disagree about it.
    [[nodiscard]] VkExtent2D viewExtent(const ViewTable::Entry& e) const;
    /// Build one registered view's targets and destination at `extent`, releasing
    /// whatever it held first. Both halves, because a caller that did one is a view
    /// sampling an image the other half no longer sizes.
    void createViewSlot(ViewSlot& v, VkExtent2D extent);
    void destroyViewSlot(ViewSlot& v);
    /// One destination, at `extent` and in the swapchain's format.
    [[nodiscard]] GpuImage createViewDestination(VkExtent2D extent) const;
    /// Put every live view's destination into the image descriptor array and rewrite it.
    void bindViewDestinations();

    /**
     * @brief Record one complete chain into the shared targets, ending in a tonemap.
     *
     * Everything from the instance upload to the tonemap. Takes no `imageIndex`: where it
     * lands is `view.destination` or, empty, whatever `composeImage` decides — which is
     * the seam that already existed and that this row turned into a third answer rather
     * than a flag every pass has to agree with.
     *
     * The scene-wide passes are **not** here. Shadows and the acceleration refit run once
     * per frame from `drawFrame`, before any chain, because they are properties of the
     * scene and re-running them per view is the cost C31 split the View out to avoid.
     */
    void recordViewChain(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& camera, uint32_t imageIndex);
    /**
     * @brief Order one chain's last write against the next chain's first read.
     *
     * **Still needed with a target set per view**, and the reason moved rather than went
     * away: cull list 0, the per-command visibility buffer and the indirect command region
     * are all in the frame slot rather than in a `View`, so chain B's cull dispatch
     * overwrites the commands chain A's indirect draws are still reading. Making those per
     * view is the remaining half of C33's deferred "private visibility buffer".
     */
    void recordViewBarrier(VkCommandBuffer cmd);

    /// Chosen from what the device reports; see createRenderTargets().
    VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    VkSampler pointSampler = VK_NULL_HANDLE;

    VkPipelineLayout ssaoLayout = VK_NULL_HANDLE;
    VkPipeline ssaoPipeline = VK_NULL_HANDLE;
    VkPipeline ssaoBlurPipeline = VK_NULL_HANDLE;

    /// Whether the traced paths will actually run this frame: the toggle, and a device
    /// and scene that can serve them. Set once in createPipelines and read at record
    /// time, so the SSR variant and the ENABLE_RT constant that gates the shader
    /// branches cannot disagree.
    bool rtActive = false;

    /// Whether the shadow-mask pass runs and the lighting shader reads it -- `rtActive`,
    /// the two shadow toggles, and more than one sample to share a ray between. Set beside
    /// `rtActive` and read at record time for the same reason: the pass that writes the
    /// mask and the constant that gates the read cannot be allowed to disagree.
    bool shadowMaskActive = false;

    VkPipelineLayout shadowMaskLayout = VK_NULL_HANDLE;
    VkPipeline shadowMaskPipeline = VK_NULL_HANDLE;

    // ------------------------------------------------------------ light tiles (C35)
    /// One storage buffer, written in compute and read in the fragment stage. Its own
    /// layout rather than a sixth binding on the frame set, because the buffer's size is a
    /// function of the render extent and the frame set is not rebuilt by a resize.
    VkDescriptorSetLayout lightTileSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout lightTileLayout = VK_NULL_HANDLE;
    /// The multisampled or 1x build shader, chosen in `createPipelines` exactly as the
    /// lighting variant is and for the same reason -- it reads gDepth.
    VkPipeline lightTilePipeline = VK_NULL_HANDLE;
    /// Mask words per tile, `ceil(lightBufferCapacity / 32)`. **Zero means the pass cannot
    /// run**, which is what a budget past `kLightTileMaxWords * 32` reduces to; nothing
    /// else sets it to zero, and `render.lightTiles` is a separate question asked at
    /// record time.
    uint32_t lightTileWords = 0;
    /// Whether the build will run for the chain being recorded. Decided in
    /// `updateUniforms`, which is where `tileParams` is written, so the stride the
    /// shaders index by and the dispatch that fills the buffer cannot disagree -- a shading
    /// pass reading a buffer no pass wrote is the SSAO failure, one buffer along.
    bool lightTilesActive = false;
    void recordLightTiles(VkCommandBuffer cmd, uint32_t slot);

    // -------------------------------------------------------------------- bloom
    /// Linear + clamp. Clamp specifically: a wrapping sampler pulls the opposite edge
    /// of the screen into the blur, which shows up as a bright rim on the wrong side.
    VkSampler bloomSampler = VK_NULL_HANDLE;
    /// One sampled source, one storage destination, both in the compute stage. Shared by
    /// bloom and SSAO -- the third user of this shape, which is what made it worth naming.
    VkDescriptorSetLayout computeImageSetLayout = VK_NULL_HANDLE;

    // ----------------------------------------------------------------------- Hi-Z pyramid
    VkPipeline depthPyramidPipeline = VK_NULL_HANDLE;
    VkPipelineLayout depthPyramidLayout = VK_NULL_HANDLE;
    /// The pyramid's own sampler, and it is not `pointSampler`: that one is LINEAR despite
    /// the name and leaves `maxLod` at zero, so every `textureLod` through it silently
    /// clamps to mip 0. Both are wrong here -- a blended depth is a depth of nothing, and a
    /// level-0 tap makes the pyramid pointless.
    VkSampler hizSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout hizSetLayout = VK_NULL_HANDLE;
    void createDepthPyramidPipeline();
    void destroyDepthPyramidPipeline();
    /// Puts the pyramid in GENERAL before anything binds it. See the definition.
    void recordDepthPyramidLayout(VkCommandBuffer cmd, uint32_t slot);
    void recordDepthPyramid(VkCommandBuffer cmd, uint32_t slot);
    VkPipelineLayout bloomLayout = VK_NULL_HANDLE;
    VkPipeline bloomThresholdPipeline = VK_NULL_HANDLE;
    VkPipeline bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline bloomUpPipeline = VK_NULL_HANDLE;

    // --------------------------------------------------------------------- ssr
    VkPipelineLayout ssrLayout = VK_NULL_HANDLE;
    VkPipeline ssrPipeline = VK_NULL_HANDLE;
    VkPipelineLayout ssrCompositeLayout = VK_NULL_HANDLE;
    VkPipeline ssrCompositePipeline = VK_NULL_HANDLE;
    /// The composite for a reduced `ssrExtent`: same draw, but it reads the G-buffer depth
    /// so it can reject the low-resolution texels that belong to another surface. Built
    /// only when `ssrScale` is below 1.0, so the default frame carries no second pipeline.
    VkPipelineLayout ssrUpsampleLayout = VK_NULL_HANDLE;
    VkPipeline ssrUpsamplePipeline = VK_NULL_HANDLE;
    /// The scale the live `ssrTarget` was built at. `ssrScale` is bound to the settings
    /// table and can be written at any time; comparing the extents it produces is what
    /// turns that into one rebuild rather than one per frame.
    float builtSsrScale = rowDefault::ssrScale;

    // --------------------------------------------------------------------- fog
    VkPipelineLayout fogLayout = VK_NULL_HANDLE;
    VkPipeline fogPipeline = VK_NULL_HANDLE;
    VkPipeline fogCompositePipeline = VK_NULL_HANDLE;

    // ----------------------------------------------------------------- motion correction
    VkPipelineLayout velocityLayout = VK_NULL_HANDLE;
    VkPipeline velocityPipeline = VK_NULL_HANDLE;
    /// Commands written into `velocityCmdRegion` this frame, and where the deformed half
    /// of them starts -- the same static-then-skinned split the opaque list uses, for the
    /// same reason: each half draws with its own vertex buffer bound.
    uint32_t velocityCommandCount = 0;
    uint32_t velocityStaticCount = 0;

    VkDescriptorSetLayout taaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout taaLayout = VK_NULL_HANDLE;
    VkPipeline taaPipeline = VK_NULL_HANDLE;
    /// Last frame's `taaEnabled`, so a toggle can invalidate the history. Nothing
    /// signals a public member being assigned, which is the same reason featureKey()
    /// exists -- but TAA has no specialisation constants, so it needs its own.
    bool taaWasEnabled = false;

    // ---------------------------------------------------------------------- rt
    /// Static geometry in one baked BLAS, plus a BLAS per deformed instance refitted every
    /// frame, under a TLAS rebuilt every frame. See AccelStruct.h.
    SceneAccelStruct accel;
    /// Rebuild both tiers from the current scene, instance table and deformed vertex
    /// buffer. **Called from `setScene`, and again from `setAnimator`** -- the dynamic tier
    /// is built over the deformed buffer, which does not exist the first time.
    void buildAccelerationStructures();

    /// Lights uploaded this frame, after the sun is prepended. Reused rather than
    /// reallocated.
    std::vector<GpuLight> lightScratch;
    /// Indices into `lights`, ranked by importance. **Only filled on frames where a budget
    /// actually binds** -- ranking when nothing is dropped would move pixels for nothing.
    std::vector<uint32_t> lightRankScratch;
    /// Indices of the lights that survived the frustum test this frame. A member so the
    /// frame stays allocation-free.
    std::vector<uint32_t> lightVisibleScratch;
    /// Marks the sun's slot in `lightSourceScratch`: it is prepended rather than drawn from
    /// `lights`, so it has no index there.
    static constexpr uint32_t kNoLightSource = 0xffffffffu;
    /// Where each entry of `lightScratch` came from in `lights`, parallel to it and the
    /// same length. `kNoLightSource` for the sun, which has no scene index. **This is what
    /// lets a secondary view find the atlas layer the primary gave a light it also chose**;
    /// without it a per-view ranking has a light and no way to name it.
    std::vector<uint32_t> lightSourceScratch;
    /// Atlas layer the *primary* assigned each scene light this frame, or -1 for a light it
    /// did not shadow. Indexed by scene light index, rebuilt by the primary's pass and read
    /// by every other view's.
    ///
    /// **A secondary view ranks its own lights but cannot assign layers**: the atlas holds
    /// one assignment and `recordPunctualShadows` has already rendered it by the time a
    /// secondary chain gets here. A light the primary shadowed occludes in every view; one
    /// only this view chose illuminates without occluding, which is the same degradation an
    /// atlas overflow already produces.
    std::vector<float> lightShadowLayer;
    /// Scene lights the view volume rejected on the last update. Reported, never silent.
    uint32_t culledLights = 0;
    /// **The report fires when the count changes, not every frame**: a scene 9 lights over
    /// budget at 600 FPS would otherwise emit 5400 identical warnings a second, and a log
    /// nobody can read is the same as silence.
    uint32_t reportedLightDrops = 0;

    /// Lights `lightBuffer` was actually allocated for, and the only number the ranking
    /// binds against. `lightBudget` is a floor read once at init; this grows past it
    /// whenever a view wanted more (C40).
    uint32_t lightBufferCapacity = 0;
    /// The largest number of lights any view was short by, recorded during a frame and spent
    /// by `growLightBuffer` at the top of the next one. Zero once the growth has happened.
    uint32_t lightsWanted = 0;

    // ----------------------------------------------------------------------- IBL
    /// All four are computed once at startup and never touched again -- the
    /// environment is static -- so there is no per-frame IBL cost beyond sampling.
    GpuImage envCube;         ///< radiance, mipped so the prefilter can pick a level
    GpuImage irradianceCube;  ///< cosine-convolved, replaces the ambient hack
    GpuImage prefilteredCube; ///< one mip per roughness step
    GpuImage brdfLut;         ///< split-sum scale and bias, RG16F
    VkSampler iblSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout iblSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet iblSet = VK_NULL_HANDLE;

    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gbufferSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout hdrSetLayout = VK_NULL_HANDLE;
    /// One sampled image in the fragment stage. Shared by the font atlas, the SSR and
    /// fog composites, and the decal pass -- four users of one shape, which is what
    /// promoted it from two identically-shaped layouts with different names.
    VkDescriptorSetLayout singleImageSetLayout = VK_NULL_HANDLE;
    /// One storage image in the fragment stage. The shadow mask's, and only its: the pair
    /// at `computeImageSetLayout` is compute-only and a stage flag widened for one user is
    /// widened for bloom, SSAO and the depth pyramid too.
    VkDescriptorSetLayout storageImageSetLayout = VK_NULL_HANDLE;
    /// The sun's map and its comparison sampler, bound at set 2 binding 0, beside the TLAS
    /// at binding 2 -- one "how is this point occluded" set, whichever path answers it.
    GpuImage shadowMap;

    /// Depth array for punctual shadows, separate from the sun's map: a different
    /// resolution, and a perspective projection per layer rather than one orthographic
    /// box shared by everything.
    GpuImage punctualShadowMap;
    VkImageView punctualShadowLayerViews[kMaxShadowLayers]{};

    // ------------------------------------------------------ punctual atlas cache
    /// The matrix each layer was last rendered through. A layer whose matrix and geometry
    /// are both unchanged still holds the right depth, so it is not redrawn.
    glm::mat4 cachedPunctualMatrix[kMaxShadowLayers];
    /// How many leading entries of `cachedPunctualMatrix` mean anything.
    uint32_t cachedPunctualCount = 0;
    /// `instances->revision()` the cache was built against. A bump means something moved,
    /// and every layer has to assume it was in shot.
    uint64_t punctualCacheRevision = 0;
    /// Nothing in the atlas is worth keeping yet, so every layer is cleared and the
    /// entry transition may discard. True exactly once, and after any resize.
    bool punctualCacheCold = true;
    /// Which layers this frame must re-render. Decided in updateLights, where the
    /// matrices exist; recordPunctualShadows only acts on it.
    bool punctualLayerDirty[kMaxShadowLayers]{};
    /// How many layers the last frame actually redrew, for the overlay.
    uint32_t punctualLayersRendered = 0;
    /// One view-projection per assigned layer, rebuilt each frame by updateLights.
    std::vector<glm::mat4> shadowMatrixScratch;
    /// Lights that could not be given layers last time it changed, so the overflow is
    /// reported when it starts and when it stops rather than every frame.
    uint32_t reportedShadowDrops = 0;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkPipelineLayout shadowLayout = VK_NULL_HANDLE;

    /// The scene TLAS, alone in the set that used to hold the shadow maps beside it. It
    /// keeps the old set index (2 in the raster passes, 3 in the tracing ones) and the old
    /// binding (2), so no shader renumbered when the maps were ripped out.
    VkDescriptorSetLayout tlasSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    /// What each layout above was created from, kept only so `verifyShaderBindings` has
    /// something to compare a reflected binding against -- **Vulkan offers no way to read a
    /// `VkDescriptorSetLayout` back**. Populated as each layout is created and replaced.
    /// Never read in Release.
    std::unordered_map<VkDescriptorSetLayout, std::vector<VkDescriptorSetLayoutBinding>> layoutBindings;

    /// The overlay's own image array: slot zero the font atlas, the rest whatever `images`
    /// holds. Its own set rather than the scene's, because the scene's does not exist until
    /// `setScene` and the overlay draws before one is loaded.
    VkDescriptorSet overlaySet = VK_NULL_HANDLE;
    VkDescriptorSet tlasSet = VK_NULL_HANDLE;

    VkPipelineLayout gbufferLayout = VK_NULL_HANDLE;
    VkPipelineLayout lightingLayout = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
    VkPipelineLayout overlayLayout = VK_NULL_HANDLE;
    VkPipelineLayout forwardLayout = VK_NULL_HANDLE;
    VkPipelineLayout decalLayout = VK_NULL_HANDLE;
    VkPipeline decalPipeline = VK_NULL_HANDLE;
    VkPipeline lightingPipeline = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline = VK_NULL_HANDLE;
    VkPipeline overlayPipeline = VK_NULL_HANDLE;
    /// Created unconditionally, as the overlay's is: one pipeline over two twenty-line
    /// shaders, and creating it lazily would put it into the hot-reload path for nothing.
    VkPipeline debugLinePipeline = VK_NULL_HANDLE;
    VkPipelineLayout debugLineLayout = VK_NULL_HANDLE;

    // -------------------------------------------------------------------------- sprites
    /// The layers and the order. Owned by `Engine`; this holds the buffer the sorted array
    /// is copied into and the one pipeline that draws it.
    const scene::SpriteTable* sprites = nullptr;
    /// One storage buffer at binding 0. Its own layout rather than the frame set's, because
    /// this pass reads no light, no shadow matrix and no frame uniform -- it takes the one
    /// matrix it wants through a push constant, as the debug-line pass does.
    VkDescriptorSetLayout spriteSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout spriteLayout = VK_NULL_HANDLE;
    /// Created unconditionally, as the overlay's and the debug lines' are.
    VkPipeline spritePipeline = VK_NULL_HANDLE;
    /// Sprites the per-frame buffers can hold. Doubles; zero until the first draw.
    uint32_t spriteCapacity = 0;

    // ------------------------------------------------------------------- overlay
    ui::Font debugFont;
    /// Nearest, not linear: the atlas is drawn at 1:1 texel-to-pixel, and filtering a
    /// bitmap font at that ratio only blurs it.
    VkSampler fontSampler = VK_NULL_HANDLE;
    /// One binding of `imageCapacity` combined image samplers. Not `singleImageSetLayout`,
    /// which four other passes share for exactly one image. Rebuilt every time the array
    /// doubles, which rebuilds the pipelines with it -- **a declared count the array cannot
    /// fill is charged per draw by the validation layer**, and declaring the device's
    /// ceiling cost 8.5 ms a frame in Debug.
    VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
    /// The set's own pool, so growing the array is a pool and a set rather than a hole in
    /// the renderer's. Recreated by `ensureImageCapacity`; a pool's sizes are fixed at
    /// creation, so growth cannot be served out of the old one.
    VkDescriptorPool overlayImagePool = VK_NULL_HANDLE;
    /// Parallel to `images`'s slots, so index n here is descriptor n there. Slot zero is
    /// the font atlas and stays empty in this vector -- `debugFont` owns that image.
    std::vector<GpuImage> overlayImages;
    /// The generation resident in each slot, or 0 for "nothing is". Compared against
    /// `ImageTable::at(s).generation` to find what changed; a slot destroyed and
    /// reacquired differs here even though its index did not move.
    std::vector<uint32_t> overlayResident;
    /// Whether `overlayImages[s]` is a handle this class owns or a copy of one a view
    /// owns. Beside the handle rather than on the table's entry, because teardown runs
    /// after the table is gone and still has to know which images are not its to free.
    std::vector<uint8_t> overlayBorrowed;
    /// The images a game loaded. Owned by `Engine`; this renderer holds the `GpuImage`
    /// behind each slot and nothing else.
    const ImageTable* images = nullptr;
    /// The revision this renderer has reconciled to, so a frame that changed nothing
    /// costs one comparison.
    uint64_t imageRevision = 0;
    /// Descriptors allocated in `overlaySet`. Doubles, capped at `imageSlotCeiling`.
    uint32_t imageCapacity = 0;
    /// The device's bound on the array, from its own limits. See `maxImageSlots()`.
    uint32_t imageSlotCeiling = 0;
    /// Reserved once at init so building the HUD allocates nothing per frame.
    std::vector<OverlayVertex> overlayScratch;

    /// Blended instance slots keyed by view depth, rebuilt and re-sorted every frame
    /// because the order is a property of where the camera is, not of the scene.
    std::vector<std::pair<float, uint32_t>> forwardOrder;

    FrameStats stats;
    core::AveragingBuffer<double> wallFrameMs{60};
    core::AveragingBuffer<double> cpuBusyMs{60};
    core::AveragingBuffer<double> gpuFrameMs{60};
    std::chrono::steady_clock::time_point lastFrameStart{};
    /// Frame-to-frame wall time, which is what FPS means. **It contains every GPU block**,
    /// so calling it a CPU number makes it track the GPU frame whenever the GPU is the
    /// limiter, which reads as a broken counter.
    double avgWallMs = 0.0;
    /// Wall time less the frame's three GPU blocks, so it moves with CPU work alone.
    double avgCpuBusyMs = 0.0;
    double avgGpuMs = 0.0;

    /// Time blocked on the GPU so far this frame, summed across waitFence, acquire and
    /// present. Consumed and cleared by the next frame, which is the frame whose wall
    /// span contains these three blocks -- see the comment at the top of `drawFrame`.
    double frameBlockedMs = 0.0;

    FrameSync frames[kFramesInFlight];
    std::vector<VkSemaphore> renderFinished;

    uint32_t frameSlot = 0;
    uint64_t framesSubmitted = 0;
};

} // namespace gfx
