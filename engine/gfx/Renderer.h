#pragma once

#include "core/AveragingBuffer.h"
#include "gfx/GpuProfiler.h"
#include "core/Recorder.h"
#include "core/Settings.h"
#include "gfx/AccelStruct.h"
#include "gfx/DebugLines.h"
#include "gfx/Decal.h"
#include "core/DebugView.h"
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

/// A field `bindRenderer` binds must initialise from here, or its default is spelled twice
/// and the two drift.
namespace rowDefault = core::defaults::render;

/// Lights shaded where a game states no budget. `Renderer::lightBudget` is the live value
/// and what the storage buffer is sized from, so raising this one alone buys no capacity.
constexpr uint32_t kDefaultLightBudget = 32u;

/// @brief Sub-pixel offsets TAA cycles through, as a Halton(2,3) sequence.
///
/// Eight, because at a 0.1 history weight a sample is down to a few percent after eight
/// frames; a longer sequence only lengthens the time a disocclusion takes to settle.
constexpr uint32_t kTaaJitterCount = 8;

/// Levels in the bloom chain, counting the half-resolution top. Five reaches roughly 1/32 of
/// the screen; each further level spreads wider and contributes less than the last.
constexpr uint32_t kBloomMips = 5;

/// Levels in the Hi-Z pyramid. Eight covers a 256x256 footprint from the base, past any
/// single draw's screen-space box.
constexpr uint32_t kDepthPyramidMips = 8;

/// Side of one light-assignment tile, in pixels. **This is `local_size_x` in
/// light_tile_body.glsl and `frame.tileParams.y`**: one workgroup per tile and one
/// invocation per pixel, so moving one moves all three.
constexpr uint32_t kLightTileSize = 16;

/// Mask words per tile, so 1024 lights. The shared array in light_tile_body.glsl is sized by
/// the same number; past it `Renderer` refuses to tile rather than truncating a light list.
constexpr uint32_t kLightTileMaxWords = 32;

/// Side of the sun's shadow map, square, one layer. 64 MB at D32 --
/// see rendering.md, "The sun: one map, and why not cascades".
constexpr uint32_t kShadowMapSize = 4096;

/// Side of one punctual atlas layer, and how many layers there are: a spot takes one, a
/// point takes six -- see rendering.md, "Punctual".
constexpr uint32_t kPunctualShadowSize = 1024;
constexpr uint32_t kMaxShadowLayers = 24;

/// Views the culling dispatch runs for: the camera, the sun, and one per shadow layer.
constexpr uint32_t kCullViews = 2 + kMaxShadowLayers;

/// The extra command list the occlusion phase writes into. One past the views, so phase
/// 0's commands survive until the second draw has read phase 1's.
constexpr uint32_t kOcclusionView = kCullViews;
/// Command lists the output region holds: every view, plus the occlusion phase's.
constexpr uint32_t kCullCommandLists = kCullViews + 1;

/// @brief Camera views a frame can record, and therefore uniform blocks per frame slot.
///
/// Raising it costs kilobytes: one `FrameUniforms`, light buffer and shadow-matrix buffer
/// per view per frame slot. What a live view costs is its *target set*, ~224 MiB at 1600x900
/// and 4x MSAA, which is why `ViewTable::create` takes an extent.
constexpr uint32_t kMaxViews = 4;



/// @brief World bounds of one indirect command's instance run. Must match `CommandBounds`
///        in cull.comp.
///
/// For a merged run it is the union of the boxes, which is why runs are capped at
/// `kMaxInstancesPerCommand`.
struct GpuCommandBounds {
    glm::vec4 boundsMin{0.0f}; ///< w unused
    glm::vec4 boundsMax{0.0f}; ///< w unused

    /// @brief The command's LOD chain, `(firstIndex, indexCount)` per level. `lodLevels == 1`
    ///        where the primitive carries no chain.
    ///
    /// **Level 0 is copied out of the indirect command written in the same statement**;
    /// sourcing it from a second record lets a merge or a variant sweep put the two out of
    /// step.
    glm::uvec2 lods[scene::kMaxLodLevels + 1]{};
    uint32_t lodLevels = 1;
    uint32_t pad[3]{0u, 0u, 0u};
};

static_assert(sizeof(GpuCommandBounds) == 80, "GpuCommandBounds must match cull.comp");

/// @brief Longest run of consecutive instances one indirect command may cover.
///
/// Raising it trades culling granularity for submission cost: an unbounded run makes 4096
/// boxes one draw call and one un-cullable blob.
constexpr uint32_t kMaxInstancesPerCommand = 64;

/// @brief A run of indirect commands that share a shader variant -- see rendering.md,
///        "Shader variants".
///
/// `first` is explicit because the ranges are ordered by variant while the command buffer
/// stays ordered static-then-skinned.
struct VariantRange {
    uint32_t variant = 0;
    uint32_t first = 0;
    uint32_t count = 0;
    /// How many of `count` are alpha-unmasked. They lead, so a depth-only pass can draw
    /// `[first, first + unmasked)` with no fragment shader bound and keep early-Z.
    uint32_t unmasked = 0;
};

/// Must match frame.glsl exactly.
struct FrameUniforms {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 cameraForward; ///< xyz normalised view direction
    /// The projection's depth, inverted: the four coefficients `viewDistance()` in
    /// frame.glsl evaluates, from `Camera::depthLinear`.
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
    /// w the projection is orthographic.
    glm::uvec4 flags;
    /// The tile light grid: x tiles across, y `kLightTileSize`, z mask words per
    /// tile, w spare. **z is zero exactly when tiling is off for this frame**, which
    /// is the one value the light loops branch on.
    glm::uvec4 tileParams;

    /// The sun's orthographic view-projection, fitted to the scene and independent of the
    /// camera.
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

/// One vertex of the debug overlay. Positions are pixels, origin top-left. Must match
/// `overlay.vert`.
using OverlayVertex = ui::DrawVertex;

/// What the overlay reports, gathered while the G-buffer pass records.
struct FrameStats {
    uint32_t drawCalls = 0;
    uint32_t primitives = 0;
    uint64_t triangles = 0;
    uint32_t blendedDrawCalls = 0;  ///< the forward pass
    /// Draws in the velocity pass. The pass writes a signed near-zero quantity, so a
    /// readback of its target is a black PNG whether it drew everything or nothing.
    uint32_t velocityDrawCalls = 0;
    uint32_t particles = 0;        ///< live particles drawn this frame
    uint32_t sprites = 0;          ///< sprite instances in this frame's one draw
};

/// @brief Deferred renderer: G-buffer, per-sample lighting, tonemap.
class Renderer {
  public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static constexpr uint32_t kFramesInFlight = 2;

    /**
     * @param uploader        Must already be initialised: the font atlas is uploaded here.
     * @param debugFontPath   TTF for the overlay; empty selects the embedded bitmap font.
     * @param debugFontHeight Rasterisation height for that TTF, in pixels.
     */
    /// Aborts on failure; there is no partially-built renderer to hand back.
    void init(VulkanContext& ctx, Uploader& uploader, GLFWwindow* window, bool vsync, uint32_t msaaRequest,
              const std::string& debugFontPath, float debugFontHeight);
    void shutdown();

    void setScene(const scene::GltfScene* scene);

    /// @brief The instance table this renderer draws.
    ///
    /// **Must be called before `drawFrame`.** Slots created afterwards must be announced
    /// with `instancesGrew()`; creating one and telling the renderer nothing writes past the
    /// end of a mapped staging range.
    void setInstances(const scene::InstanceTable* table);

    /// @brief Slots were added to the table since the last call. Sizes the buffers and marks
    ///        the acceleration structure, and does no work beyond that.
    ///
    /// The cheap half of `setInstances`, which also rebuilds the whole acceleration
    /// structure at 15 ms a call. The rebuild is deferred to the next `rebuildAccelIfStale`.
    void instancesGrew();

    /// @brief Rebuild the acceleration structure if the scene tree has moved anything the
    ///        static tier baked. Call once per frame, after `Scene::update`.
    ///
    /// A transform reaching an instance through the node sweep lands after `setInstances`
    /// built the structure, so without this an instance is traced at whatever transform
    /// `create` was given -- a shadow and a reflection of geometry that is not there.
    void rebuildAccelIfStale();

    /// @brief The rig driving this scene, if it has one.
    ///
    /// **Must be called after `setInstances`**; it sizes buffers from both.
    void setAnimator(const scene::SceneAnimator* animator, const scene::GltfScene* scene);

    /// @brief Name the scene's cloth, before `setAnimator` sizes the deformed buffer.
    ///
    /// `setAnimator` allocates `skinnedVertices` and resolves `clothDestBase`, so a cloth
    /// named after it has nowhere to write.
    void setCloth(const scene::ClothSystem* cloth) { clothSystem = cloth; }

    /// @brief The scene's particle emitters, and the pool sized for them.
    ///
    /// Sizes every particle buffer from `system->capacity()`; a capacity of zero allocates
    /// nothing. **Must be called after `setScene`** -- the draw pipeline binds the scene's
    /// bindless texture array.
    void setParticles(const scene::ParticleSystem* system);

    /// Re-allocate the pool buffers at the system's current `capacity()`, carrying the
    /// particles in flight across. Paired with `ParticleSystem::grow` by
    /// `Engine::growParticles`: growing one without the other emits into storage the device
    /// does not have.
    void resizeParticlePool();

    /// @brief The sprites a game has created, drawn by `recordSprites`.
    ///
    /// **The textures come from `setImages`'s array, not the scene's.**
    void setSprites(const scene::SpriteTable* table);

    [[nodiscard]] FrameResult drawFrame(const scene::Camera& camera);

    void requestResize() { resizeRequested = true; }

    /// Clamped to what the device supports; rebuilds the G-buffer and pipelines.
    void setSampleCount(uint32_t samples);
    uint32_t sampleCount() const { return static_cast<uint32_t>(msaaSamples); }

    void setDebugView(core::DebugView view) { debugView = view; }
    core::DebugView currentDebugView() const { return debugView; }

    void cycleDebugView(int step) { debugView = core::advanceDebugView(debugView, step); }

    /// @brief Whether the ray-traced paths can do anything on this device and scene.
    ///
    /// **Neither half is knowable from the settings table**, so `rtEnabled` is silently
    /// inert without this.
    [[nodiscard]] bool rayTracingAvailable() const { return ctx->rayQuerySupported && accel.valid(); }

    /// @brief Write the next presented frame to `path` as a PNG.
    ///
    /// The copy is recorded between the last pass and the transition to PRESENT_SRC.
    /// **Reading the image back after presenting is a race**: `vkDeviceWaitIdle` drains
    /// queues, and the presentation engine is not a queue.
    void requestCapture(std::filesystem::path path);

    /// False when the surface refused TRANSFER_SRC on its swapchain images; `requestCapture`
    /// then refuses rather than writing a black PNG.
    bool captureAvailable() const { return swap.captureSupported; }

    /// Frames written since startup.
    uint32_t capturesWritten() const { return captureCount; }

    /// @brief Read an intermediate render target back to a PNG, by name. Names come from
    ///        `captureTargetNames()`; an unknown one is refused with a reason.
    ///
    /// `mip` and `layer` of `UINT32_MAX` mean "all of them", writing `name.mipN.png` /
    /// `name.layerN.png` beside `path`. Multisampled images are absent from the table --
    /// `vkCmdCopyImageToBuffer` cannot read one.
    void requestTargetCapture(const std::string& name, std::filesystem::path path, uint32_t mip = 0,
                              uint32_t layer = 0);

    /// Every name `requestTargetCapture()` accepts in this configuration, with the ones
    /// whose pass is currently off marked.
    std::vector<std::string> captureTargetNames() const;

    /// @brief Tee every presented frame to `sink` while it is recording. False where the
    ///        swapchain cannot be read back or its format described to an encoder.
    ///
    /// **Unlike `requestCapture`, this never blocks**: the pixels arrive two frames late,
    /// read back at the top of the frame that reuses the slot.
    bool startRecording(core::Recorder& sink, core::Recorder::Options options, core::AudioTap* audio);
    /// Stop teeing, draining the slots still holding pixels so the last frames before a quit
    /// reach the file.
    void stopRecording();
    [[nodiscard]] bool recording() const { return recorder != nullptr; }


    /// Geometric specular antialiasing strength, [0, 1]. 0 compiles ENABLE_GSAA out of
    /// gbuffer.frag rather than multiplying by zero.
    float specularAaStrength = 0.5f;

    /// @brief Edge-detect hybrid MSAA: shade once per pixel where every sample holds the
    ///        same G-buffer fragment, per-sample only where they differ. Nothing at 1x.
    ///
    /// The fast path is meant to produce the same pixels as the unconditional per-sample
    /// loop, not an approximation of them -- see rendering.md, "Edge-detect hybrid shading".
    bool edgeMsaaEnabled = rowDefault::edgeMsaa;

    /// @brief Temporal antialiasing.
    ///
    /// **Off by default**: it makes the image a function of the last several frames, which
    /// costs the bit-identical-between-runs property the golden images are built on. See
    /// rendering.md, "TAA, and why it is not simply better".
    bool taaEnabled = rowDefault::taa;
    /// Weight given to the current frame. 0.1 is a ten-frame effective window; higher is
    /// sharper under motion and noisier at rest.
    float taaBlend = rowDefault::taaBlend;

    /// Screen-space reflections: reflects only what is already on screen, and fades out
    /// where a ray leaves the frame.
    bool ssrEnabled = rowDefault::ssr;
    /// Ray-traced reflections in place of the SSR march. Ignored where ray query is
    /// unavailable.
    bool rtEnabled = rowDefault::rt;
    /// The sun's shadow map. **The non-traced path only** -- where `render.rt` is on, none
    /// of this runs.
    bool shadowsEnabled = rowDefault::shadows;
    /// Shadows for point and spot lights, from the atlas above. Separate from
    /// `shadowsEnabled` because the sun is one pass and this is up to 24.
    bool punctualShadowsEnabled = rowDefault::punctualShadows;
    /// @brief Shadow rays in the traced lighting pass. Ignored where ray query is
    ///        unavailable.
    ///
    /// **The primary image only** -- `shadeRayHit` shades reflection hits unshadowed,
    /// because a shadow ray from a reflection hit is a second bounce and inline ray query
    /// has no recursion to spend on one.
    bool rtShadowsEnabled = rowDefault::rtShadows;
    /// @brief Trace each distinct fragment's shadow rays once into a mask, ahead of
    ///        lighting, instead of once per MSAA sample inside it. Nothing at 1x.
    ///
    /// **Keyed per fragment, not per pixel**: one ray is shared between samples the
    /// G-buffer says came from one surface, which differs only where a shadow boundary
    /// crosses a silhouette pixel between two samples of it. See shadowmask.frag.
    bool rtShadowMaskEnabled = rowDefault::rtShadowMask;
    /// Re-render an atlas layer only when its light, its layer assignment or the geometry
    /// changed.
    bool shadowCacheEnabled = rowDefault::shadowCache;
    /// World units the sun's fitted box may span; 0 fits it to the scene bounds. Capping it
    /// concentrates texels and leaves the world past it unshadowed.
    float shadowDistance = rowDefault::shadowDistance;
    /// Metres the depth test offsets an occluder along the light. Kills acne on surfaces
    /// near-parallel to the sun; too large and contact points detach. `Engine::initRenderer`
    /// assigns `GameSetup::look.shadowDepthBias` over this, so a game's value wins.
    float shadowDepthBias = 0.02f;
    /// Metres the lookup moves along the surface normal. Does what a larger depth bias
    /// would without detaching contact shadows, because it moves across the surface
    /// rather than along the light.
    float shadowNormalBias = 0.04f;
    /// World units a reflection *march* may travel before giving up.
    ///
    /// **Not a ray range, and the two must not be merged into one number.** This divides
    /// into `ssrSteps` to give a stride, so raising it coarsens the march rather than
    /// extending its reach.
    float ssrMaxDistance = rowDefault::ssrMaxDistance;
    /// World units a reflection *ray* may travel. A ray that terminates early does not fade,
    /// it returns the environment cube, so a short range reads as a hard bubble of real
    /// reflection surrounded by sky.
    float rtMaxDistance = rowDefault::rtMaxDistance;
    /// How far behind a surface, in world units, the ray may be and still count as hitting
    /// it. Too small and rays tunnel through thin geometry; too large and a ray "hits" the
    /// far side of a column it passed beside.
    float ssrThickness = rowDefault::ssrThickness;
    float ssrIntensity = rowDefault::ssrIntensity;
    /// Surfaces rougher than this reflect nothing: a wide lobe is not something a single
    /// mirror ray can represent.
    float ssrRoughnessCutoff = rowDefault::ssrRoughnessCutoff;
    /// Fraction of `renderExtent` the reflection pass traces at, 0.25 to 1.0 --
    /// see rendering.md, "`render.ssrScale`, and where the SSR zone's time actually goes".
    float ssrScale = rowDefault::ssrScale;
    uint32_t ssrSteps = 32;
    /// Binary-search steps after the linear march finds a crossing. Six halves the
    /// stride sixty-four times over, which is well inside a pixel at these distances.
    uint32_t ssrRefineSteps = 6;

    /// Volumetric fog: single-scattering sunlight through participating media. Its density
    /// and height are authored numbers, not derivable ones, so it is off by default.
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

    bool particlesEnabled = rowDefault::particles;

    bool particleSortEnabled = rowDefault::particleSort;

    core::TonemapOperator tonemapOperator = core::TonemapOperator::Aces;

    bool ssaoEnabled = rowDefault::ssao;
    /// Hemisphere radius in world units, at contact scale: too large and it stops being
    /// contact occlusion and starts darkening whole walls. Scaling it with the scene bounds
    /// gives a warehouse metre-deep creases and a doorknob none.
    float ssaoRadius = rowDefault::ssaoRadius;
    /// Pushes the depth comparison off the surface, in world units. Without it a flat wall
    /// occludes itself wherever precision puts a sample fractionally behind its neighbour.
    float ssaoBias = rowDefault::ssaoBias;
    /// Exponent on the result. Above 1 deepens contact shadows without widening them.
    float ssaoIntensity = 1.6f;
    /// Samples per pixel. 16 is noisy on its own; the 4x4 blur is what resolves it.
    uint32_t ssaoSamples = 16;

    /// @brief A flat ambient added to every surface, as radiance: colour and magnitude in
    ///        one vec3, black by default. Authored by the game -- see `GameSetup`.
    ///
    /// **Diffuse only**, and it is what SSAO and the glTF occlusion texture attenuate: at
    /// black both are computed and multiplied into nothing.
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
    /// How much of the chain lands in the final image; past ~0.1 it stops reading as glare
    /// and starts reading as fog. **If an emitter is not blooming, check that its material
    /// strength reached the shader before raising this** -- raised to compensate for one
    /// unlit emitter, it mis-lights every other scene.
    float bloomStrength = rowDefault::bloomStrength;

    GpuProfiler& gpu() { return gpuProfiler; }

    void logGpuTimings();
    const Swapchain& swapchain() const { return swap; }
    uint64_t frameCount() const { return framesSubmitted; }

    /// Draw the frame stats overlay. **A capture turns it off unless a flag named it** -- a
    /// counter that changes every frame makes every golden comparison differ.
    bool debugOverlay = true;

    /// Extra text the application wants on screen, under the stats. Drawn whether or not
    /// `debugOverlay` is on.
    std::vector<std::string> overlayLines;

    /// The application's UI for this frame, as vertices and clip ranges, in pixel space.
    /// Cleared by the application each frame, like `debugLines`.
    const ui::DrawList* uiDrawList = nullptr;

    /// The glyph table the overlay was built with. Metrics only -- the atlas stays here.
    [[nodiscard]] const ui::FontMetrics& fontMetrics() const { return debugFont.metrics(); }

    /// @brief The images a game has loaded, whose residency this renderer maintains.
    ///
    /// Set once, before any frame; the renderer reconciles against `ImageTable::revision()`
    /// at the top of every `drawFrame`. Null is a renderer that draws no imagery.
    void setImages(const ImageTable* table);

    /// How many images one descriptor set on this device can hold, slot zero included, and
    /// what `ImageTable::init` is given. **A device limit, not an engine constant**, and
    /// valid only after `init`.
    [[nodiscard]] uint32_t maxImageSlots() const { return imageSlotCeiling; }

    /// @brief How many texels the file behind `id` turned out to have, or `{0, 0}`.
    ///
    /// **Reconciles residency first**, so a caller that loaded in `Game::init` does not find
    /// nothing behind a perfectly valid handle.
    [[nodiscard]] glm::uvec2 imageSize(ImageId id);

    /// @brief Register a shader variant and get the index a material stores in
    ///        `GpuMaterial::shader`.
    ///
    /// **Nothing is compiled here**: the pipelines are built the first time a draw carrying
    /// the index reaches a pass, so a shader named here that does not exist is not diagnosed
    /// until then, and the first use costs one hitched frame.
    uint32_t addShaderVariant(ShaderVariant variant);

    /// How many variants are registered, variant 0 included. `GpuMaterial::shader` past
    /// this is clamped to 0 with one warning rather than indexing off the end.
    [[nodiscard]] uint32_t shaderVariantCount() const { return static_cast<uint32_t>(variants.size()); }

    /// @brief The size in pixels of the surface a caller draws onto. **Changes on every
    ///        resize**, so a caller cannot cache the config's window size.
    ///
    /// **The surface, not the window**: with `uiInsideVirtual` set, a panel is laid out
    /// against 320x180 rather than 1920x1080. `windowExtent()` is the other number.
    [[nodiscard]] uint32_t framebufferWidth() const { return uiExtent().width; }
    [[nodiscard]] uint32_t framebufferHeight() const { return uiExtent().height; }

    /// @brief Render at this size and present it at the largest integer scale that fits,
    ///        letterboxed. `{0, 0}` renders at the window extent.
    ///
    /// Set from `GameSetup::present.virtualResolution` before `init`, and honoured from the
    /// next `createRenderTargets` -- see rendering.md, "Presentation: virtual resolution and
    /// integer scale".
    VkExtent2D virtualExtent{};

    /// Whether the overlay, the UI and the debug lines are drawn *into* the virtual target,
    /// scaled up with the world, or onto the window afterwards at its full resolution. From
    /// `GameSetup::present.uiInsideVirtual` -- see rendering.md, "What the UI sees".
    bool uiInsideVirtual = true;

    /// @brief Take the frame's remaining sub-texel machinery out of the 2D path --
    ///        see rendering.md, "`pixelExact` is one switch because it is one decision".
    ///
    /// `Engine::run` forces `render.taa` off and `render.tonemap` to `clamp` through the
    /// settings table; **this member is only the overlay image array's sampler**.
    bool pixelExact = false;

    /// The window's own framebuffer size. What the swapchain is, what a capture reads back,
    /// and what the presentation step fits the virtual target into.
    [[nodiscard]] VkExtent2D windowExtent() const { return swap.extent; }
    /// The views a game asked for: the *device half* of `gfx::ViewTable`. `Engine` calls
    /// this once; nothing else should.
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

    /// @brief A window pixel in the coordinates the UI was laid out in.
    ///
    /// Identity whenever the UI is outside the virtual target or the presentation is native,
    /// which is why `Engine` can call it unconditionally.
    [[nodiscard]] glm::vec2 uiFromWindow(glm::vec2 windowPixel) const;

    /// @brief A window pixel in render-target pixels -- what a picking ray is built from.
    ///
    /// The same arithmetic `uiFromWindow` does and **without its gate**: the scene is drawn
    /// into the virtual target however the UI was laid out, so a ray that skipped this in a
    /// letterboxed run is off by the scale and the bars.
    [[nodiscard]] glm::vec2 renderTargetFromWindow(glm::vec2 windowPixel) const;

    /// Draw `image` at 1:1 in the top-left of the overlay's surface, for `--readback`. Takes
    /// the handle rather than a slot; an invalid one clears the draw rather than drawing the
    /// fallback.
    void setReadbackImage(ImageId id);

    /// Where the camera is, drawn with the stats and phrased as a command line: the six
    /// numbers `--camera` takes. The application fills it each frame or leaves it empty.
    std::string cameraLine;

    /// @brief World-space lines to draw over the finished frame, refilled by the application
    ///        each frame.
    ///
    /// **Drawn without a depth test, deliberately.** A collision shape is almost always
    /// *inside* the mesh it describes, so depth-testing the wireframe hides it or makes it
    /// z-fight.
    std::vector<DebugLineVertex> debugLines;

    /// Decals to project into the G-buffer.
    std::vector<Decal> decals;

    /// GPU frustum culling. It cannot change the image, only what is submitted to produce
    /// it, which is why the golden set is the test for it.
    bool cullingEnabled = rowDefault::culling;
    /// Two-pass Hi-Z occlusion culling.
    bool occlusionCullingEnabled = rowDefault::occlusionCulling;
    /// Screen-coverage LOD selection.
    bool meshLodEnabled = rowDefault::meshLod;
    /// Fraction of the viewport an instance's projected bounds must cover to stay at
    /// LOD 0; see `scene::lodCoverageThresholds` for what the levels below it are.
    float meshLodThreshold = rowDefault::lodThreshold;
    /// Tiled light assignment.
    bool lightTilesEnabled = rowDefault::lightTiles;

    /// Sun and point lights, tweakable from the app.
    glm::vec3 sunDirection{-0.35f, 0.85f, 0.4f};
    glm::vec3 sunColorValue{1.0f, 0.96f, 0.88f};
    float sunIntensity = 3.0f;
    float exposure = 1.0f;
    /// Every light in the scene except the sun, which is prepended into the buffer as
    /// element 0 each frame.
    std::vector<GpuLight> lights;

    /// @brief How many lights may be shaded in one frame, sun included.
    ///
    /// Set before `init()`, which sizes the storage buffer from it. A scene with more keeps
    /// the most important by `lightImportance()` and **reports how many it dropped**.
    uint32_t lightBudget = kDefaultLightBudget;

    /// @brief Radiance below which the deferred light loop drops a light, in **post-exposure**
    ///        units -- what the tonemap sees, not what the light emits. 0 is off.
    ///
    /// `updateUniforms` divides by `exposure` on the way to the shader, so a value chosen
    /// against one game's look survives another's.
    float lightCutoff = rowDefault::lightCutoff;

    /// @brief Recompile shaders from source and rebuild every pipeline when a file under
    ///        either shader tree changes -- see rendering.md, "Shader hot reload".
    ///
    /// A failed compile leaves the previous SPIR-V in place and logs the error, so a syntax
    /// error costs a message rather than a black screen.
    bool shaderHotReload = false;

  private:
    struct FrameSync {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        /// @brief One block per view, indexed by `View::uniformSlot` -- not one per frame.
        ///
        /// Two views recorded into one command buffer read their matrices at *submit* time,
        /// so a shared block has the second view's `updateUniforms` rewrite what the first
        /// view's already-recorded draws will read: one view renders with the other's
        /// camera, and nothing is invalid.
        GpuBuffer uniforms[kMaxViews];
        GpuBuffer lightBuffer[kMaxViews];
        /// One view-projection per atlas layer, filled by updateLights.
        GpuBuffer shadowMatrixBuffer[kMaxViews];
        VkDescriptorSet frameSet[kMaxViews]{};
        GpuBuffer overlayVertices;
        /// Allocated only when something asks for lines.
        GpuBuffer debugLineVertices;
        /// Sized from the scene by `setAnimator`, and absent for a scene with no fabric.
        GpuBuffer clothStaging;
        /// Sized by `ensureSpriteCapacity`, and null until a game creates its first sprite.
        GpuBuffer spriteBuffer;
        VkDescriptorSet spriteSet = VK_NULL_HANDLE;
        /// `SpriteTable::revision()` this slot's `spriteBuffer` was last filled from. Zero
        /// is "holds nothing a revision can describe", which growing it, swapping the table
        /// under it and recreating the frame all reduce to.
        uint64_t spriteRevision = 0;

        /// @brief Everything the scene passes read per object, in one device-local
        ///        allocation: instances, world bounds, opaque commands, blended commands.
        ///
        /// **Do not make it host-visible to skip the copy.** Measured as a 33% regression in
        /// the shadow pass (0.431 -> 0.567 ms median): every vertex of eleven geometry
        /// passes then fetches its 128-byte instance record across PCIe.
        GpuBuffer instanceData;
        /// Host-visible mirror of the CPU-written prefix of `instanceData`. Shorter than it:
        /// the culled command output is written by the GPU and never staged.
        GpuBuffer instanceStaging;
        /// Instances that survived culling, per view. Read back two frames later, when this
        /// slot's fence guarantees the frame that wrote it has finished.
        GpuBuffer cullStats;
        VkDescriptorSet cullSet = VK_NULL_HANDLE;
        /// One uint per command, persisting across frames within this slot. Never read by
        /// the host; the first frame zero-fills it to "everything visible".
        GpuBuffer commandVisibility;
        /// Set when the buffer is (re)made, cleared once the next frame has filled it.
        bool commandVisibilityInit = false;
        VkDescriptorSet skinSet = VK_NULL_HANDLE;
        /// Table revision these buffers were last filled from. `0` is "never", which
        /// no live table reports.
        uint64_t instanceRevision = 0;
        /// `Renderer::variantAssignment` the command list was grouped against. Its own
        /// counter beside the table's: a material changing which variant draws it regroups
        /// every command without any instance having moved.
        uint64_t variantAssignment = 0;
        /// Material-table revision this slot's copy of the scene's material buffer was
        /// filled from.
        uint64_t materialRevision = 0;
        /// Commands written into the opaque region last time it was rebuilt. **Skinned
        /// after static**, so a pass can draw each half with its own vertex buffer bound;
        /// within each half, grouped by shader variant, and within a variant unmasked
        /// before masked, so the shadow pass can draw the unmasked run with no fragment
        /// shader. `opaqueRanges` names every group.
        uint32_t opaqueCommandCount = 0;
        uint32_t staticCommandCount = 0;
        /// Every group in this slot's command list, ordered by variant rather than by
        /// position. Rebuilt only when `updateInstances` rebuilds the commands.
        std::vector<VariantRange> opaqueRanges;
        /// Set when the staging buffer holds bytes `instanceData` does not. Cleared
        /// once the copy has been recorded.
        bool instanceUploadPending = false;
    };

    /// Cap on quads per overlay frame; 492 KB per frame in flight. Overflow drops the excess
    /// and says so once, rather than truncating a menu into something that looks complete.
    static constexpr uint32_t kMaxOverlayQuads = 4096;

    /// Cap on debug line *vertices* per frame, so 16384 lines; 512 KB per frame in flight.
    /// Overflows like the overlay's.
    static constexpr uint32_t kMaxDebugLineVertices = 32768;

    struct View;

    void createFrameResources();
    void destroyFrameResources();

    /// Fill `v` with every image, storage view and descriptor set sized from `extent`.
    void createRenderTargets(View& v, VkExtent2D extent);
    /// The exact inverse of the above; the two lists must stay identical.
    void destroyRenderTargets(View& v);
    [[nodiscard]] VkExtent2D primaryViewExtent() const {
        return (virtualExtent.width != 0 && virtualExtent.height != 0) ? virtualExtent : swap.extent;
    }
    /// The presenting view's targets **and** every registered view's set and destination.
    /// One function: a caller that did only the first half leaves a mirror sampling a
    /// destroyed image. A view that named its own extent is rebuilt at that one.
    void createViewTargets();
    void destroyViewTargets();

    /// Run the whole IBL chain: sky, mips, irradiance, prefilter, BRDF LUT. Once at
    /// startup, and again on a shader hot reload.
    void createIblResources();
    /// The shadow map and its comparison sampler. Once, at init.
    void createShadowResources();
    void destroyShadowResources();
    /// Fit the sun's orthographic box to the scene, filling `sunViewProj`, the biases in
    /// `shadowParams` and `cullViewProj[1]`.
    void updateSunShadow(FrameUniforms& u);
    void recordShadows(VkCommandBuffer cmd, uint32_t slot);
    void recordPunctualShadows(VkCommandBuffer cmd, uint32_t slot);
    void destroyIblResources();

  public:
    /// @brief Re-bake the environment if the sun has moved since it was baked. Call after
    ///        the real sun is resolved: `createIblResources` runs from `init`, before a game
    ///        has said what it is.
    ///
    /// A no-op where they agree; a blocking submit of a 512^2 cube and a five-mip chain
    /// where they do not.
    void rebakeIblIfSunMoved();

  private:
    /// What `createIblResources` last baked with. Not initialised to `sunDirection`'s
    /// default: a bake has to have happened for these to mean anything.
    glm::vec3 iblBakedSun{0.0f};
    glm::vec3 iblBakedColor{0.0f};
    float iblBakedIntensity = 0.0f;


    void createDescriptorLayouts();
    void createPipelines();
    void destroyPipelines();

    /// The shading id space from features.glsl, as `GraphicsPipelineDesc::constants` wants
    /// it. Shared by the lighting pass and by every variant's forward pipeline.
    [[nodiscard]] std::vector<uint32_t> shadingConstants() const;

    /// @brief Every specialisation-constant input packed into one comparable value.
    ///
    /// Compared against `builtFeatureKey` at the top of each frame, so flipping a feature
    /// toggle rebuilds the pipelines that read it without anyone having to remember a
    /// `featuresChanged()` call.
    uint64_t featureKey() const;
    uint64_t builtFeatureKey = 0;

    /// Newest write time seen under the shader source directory, as a raw tick count, with
    /// INT64_MIN for "not polled yet". **Not 0** -- see pollShaderReload() for the trap.
    int64_t newestShaderWrite = INT64_MIN;
    std::chrono::steady_clock::time_point lastShaderPoll{};
    /// Recompiles every shader source into memory, via `gfx::overrideShaderBinary`, and
    /// leaves no file behind, so a cold start can only run SPIR-V the build produced. False
    /// if any shader failed, in which case the module already bound stays bound.
    [[nodiscard]] bool recompileShaders() const;
    /// Polls mtimes and, on a change, recompiles and rebuilds. Called once per frame; the
    /// directory scan itself is rate-limited to once a second.
    void pollShaderReload();

    /// @brief Debug-only: reflect each module and check it against the layouts it was built
    ///        against. Compiled to nothing in Release.
    ///
    /// Catches a binding added to a shader and not to the hand-written
    /// `VkDescriptorSetLayout`, or a specialisation constant declared and never given a
    /// value.
    void verifyShaderBindings(const char* pass, std::initializer_list<const char*> shaders,
                              std::initializer_list<VkDescriptorSetLayout> sets,
                              size_t constantCount) const;

    /// @brief Create a pipeline layout, and check the shaders against the same set list --
    ///        **written once**, so the create info and the verification cannot disagree.
    ///
    /// Passing no shaders skips the verification, for the few layouts whose pipelines are
    /// built elsewhere.
    [[nodiscard]] VkPipelineLayout createLayout(const char* pass, std::initializer_list<const char*> shaders,
                                                std::initializer_list<VkDescriptorSetLayout> sets,
                                                std::initializer_list<VkPushConstantRange> pushConstants = {},
                                                size_t constantCount = 0) const;

    void updateUniforms(const scene::Camera& camera, uint32_t slot);

    /// @brief Grow the per-frame instance buffers to hold `slots` entries.
    ///
    /// **Growth waits for the device**, because the buffers being replaced may be in flight,
    /// so it doubles rather than fitting exactly.
    void ensureInstanceCapacity(uint32_t slots);

    /// @brief Bring the descriptor array in line with the image table. Runs at the top of
    ///        `drawFrame`, and only when `ImageTable::revision()` has moved.
    ///
    /// **It waits for the device**: a descriptor a frame in flight may still read cannot be
    /// rewritten and the image behind it cannot be freed. A caller loading images *per frame
    /// during play* is the trigger to write the retirement list this engine does not have.
    void syncImages();
    /// Build `overlaySetLayout` with `slots` descriptors in its one binding, replacing
    /// whatever was there. Marks the pipelines dirty when it replaced something: a layout of
    /// a different width is a different layout to everything built against it.
    void createOverlaySetLayout(uint32_t slots);
    /// Grow the overlay's descriptor array to `slots`, doubling. Every descriptor is invalid
    /// afterwards and `writeImageDescriptors` has to follow. Callers wait for the device
    /// first; this does not.
    void ensureImageCapacity(uint32_t slots);
    /// Write all `imageCapacity` descriptors: the font atlas at zero, a loaded image where
    /// there is one, the atlas everywhere else. **Every slot, never partially bound.**
    void writeImageDescriptors();

    void updateInstances(uint32_t slot);
    uint32_t buildBlendedCommands(uint32_t slot, const scene::Camera& camera);
    /// One command per live, opaque, dynamic instance, static ones first.
    uint32_t buildVelocityCommands(uint32_t slot);
    void recordVelocity(VkCommandBuffer cmd, uint32_t slot);
    /// Copy staging into the device-local buffer and barrier it for the vertex stage, the
    /// indirect command processor and compute. The first thing the frame's command buffer
    /// records.
    void recordInstanceUpload(VkCommandBuffer cmd, uint32_t slot);
    void recordMaterialUpload(VkCommandBuffer cmd, uint32_t slot);
    /// Point one frame slot's skinning set at the buffers it reads. `allocate` is false for
    /// the refresh path, which must not create a set for a slot whose instance buffers do
    /// not exist yet.
    void writeSkinSet(uint32_t slot, bool allocate);
    /// @brief Which command lists a cull dispatch fills.
    ///
    /// `Scene` runs **once per frame, before the shadow passes**: those passes draw straight
    /// out of lists 1.., so a cull recorded after them has them draw last frame's commands.
    /// `Camera` is list 0 alone, which is all a second view needs.
    enum class CullViews { Scene, Camera };
    /// One dispatch per view, writing this frame's culled commands.
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
    /// **Everything the CPU writes comes first**, so the copy from staging is one contiguous
    /// range and the GPU-written command output sits past its end.
    static constexpr VkDeviceSize kInstanceRegionAlign = 256;
    VkDeviceSize boundsRegion = 0;    ///< GpuCommandBounds, one per command
    VkDeviceSize templateRegion = 0;  ///< un-culled opaque commands, as the CPU built them
    VkDeviceSize blendedRegion = 0;   ///< blended commands, in depth order, never culled
    VkDeviceSize jointRegion = 0;     ///< joint matrices for every character
    VkDeviceSize weightRegion = 0;    ///< morph weights for every character
    VkDeviceSize prevRegion = 0;      ///< last frame's model matrix per slot
    VkDeviceSize velocityCmdRegion = 0; ///< dynamic-only commands for the velocity pass
    VkDeviceSize outRegion = 0;       ///< kCullViews command lists, written by cull.comp
    VkDeviceSize stagedBytes = 0;     ///< prefix of instanceData the CPU writes
    VkDeviceSize instanceDataBytes = 0;

    /// Not owned.
    const scene::SceneAnimator* animator = nullptr;
    /// Joints across every character. Sizes the joint region; zero when there is no rig.
    uint32_t jointCapacity = 0;
    /// Morph weights across every character. Sizes the weight region.
    uint32_t weightCapacity = 0;
    /// Deformed output, one `Vertex` per vertex of every skinned or morphed *instance*.
    /// Bound as vertex buffer 0 for the deformed half of each geometry pass.
    GpuBuffer skinnedVertices;
    /// Per-vertex joint indices and weights. Covers only the primitives that carry them.
    GpuBuffer skinInfluences;
    /// Per-target, per-vertex morph displacements.
    GpuBuffer morphDeltas;
    uint32_t skinnedVertexCount = 0;
    /// Where slot `i`'s deformed vertices start in `skinnedVertices`, or UINT32_MAX.
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
    /// Not owned, and null for every scene that authors no `FABRIC_` mesh.
    const scene::ClothSystem* clothSystem = nullptr;
    /// Where cloth `i`'s vertices start in `skinnedVertices`, in vertices, or UINT32_MAX.
    /// Resolved once in `setAnimator`.
    std::vector<uint32_t> clothDestBase;
    /// This frame's copy regions, one per cloth.
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
    /// copy regions. True when there is something to copy.
    bool recordClothUpload(uint32_t slot);
    /// Does anything in this scene write into `skinnedVertices`? The test every deformed
    /// command sweep uses, and **not `animator != nullptr`**: cloth deforms with no rig.
    [[nodiscard]] bool deforms() const {
        return animator != nullptr || (clothSystem != nullptr && !clothSystem->empty());
    }
    /// Submit view `view`'s commands, one indirect draw per variant group: the static half
    /// from the scene's vertex buffer, the skinned half from `skinnedVertices`.
    /// **Every geometry pass calls this rather than `vkCmdDrawIndexedIndirect`**, which
    /// keeps the skinned/static split and the variant selection in one place.
    void drawSceneIndirect(VkCommandBuffer cmd, uint32_t slot, uint32_t view, VariantPass pass);

    /// A registered variant and the pipelines it has been asked for so far. The handles are
    /// null until a draw needs one and null again after `destroyPipelines()`.
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

    /// The full pipeline description for one of a variant's three pipelines. Stated once, so
    /// a variant and the engine's own pipelines cannot end up with different depth state.
    [[nodiscard]] GraphicsPipelineDesc variantDesc(const ShaderVariant& v, VariantPass pass) const;
    /// `variant`'s pipeline for `pass`, built on first use -- inside a record method, which
    /// is where the hitch lands.
    VkPipeline variantPipeline(uint32_t variant, VariantPass pass);

    /// Which variant each material selects, refreshed when the scene's material revision
    /// moves. **What decides whether a command rebuild is owed at all**, so a game animating
    /// a material's colour does not regroup every command every frame.
    std::vector<uint32_t> materialVariant;
    /// Material revision `materialVariant` was refreshed from.
    uint64_t seenMaterialRevision = 0;
    /// Bumped only when a refresh actually changes an entry. Compared per frame slot
    /// beside the instance revision, so the two together say whether the command list
    /// still describes the scene.
    uint64_t variantAssignment = 1;
    /// One byte per registered variant, non-zero where a live instance uses it. A member so
    /// a rebuild does not allocate.
    std::vector<uint8_t> variantsPresent;
    /// Said once, not once per frame: a material naming a variant nobody registered.
    bool reportedVariantOverflow = false;

    /// Not owned. Null, or a capacity of zero, records no particle pass at all.
    const scene::ParticleSystem* particles = nullptr;
    /// Slots the pool holds. **Always a power of two** -- a bitonic network sorts a power
    /// of two or nothing, and its domain is the whole pool.
    uint32_t particleCapacity = 0;
    /// log2(particleCapacity). The width of the slot field in a sort key, and therefore
    /// what is left over for the quantised depth.
    uint32_t particleIndexBits = 0;
    /// @brief Distance at which a sort key's depth saturates, and how far behind a surface a
    ///        particle may be and still count as having hit it. In world units.
    ///
    /// **Both derive from the scene's diagonal in `setScene()`**, so neither should grow a
    /// settings row: one over a field `setScene` rewrites appears to work and reverts on the
    /// next load. The initialisers hold until the first `setScene`.
    float particleSortRange = 100.0f;
    float particleCollisionThickness = 0.5f;

    /// The pool itself and its keys, both device-local and both *persistent* rather than
    /// per-frame-in-flight: a particle's state this frame is its state last frame integrated
    /// once, so there is one simulation and not two.
    GpuBuffer particlePool;
    GpuBuffer particleKeys;
    GpuBuffer particleEmitterBuffers[kFramesInFlight];
    GpuBuffer particleSpawnBuffers[kFramesInFlight];
    VkDescriptorSet particleSets[kFramesInFlight]{};

    VkDescriptorSetLayout particleSetLayout = VK_NULL_HANDLE;
    /// Five sets: frame, particles, the TLAS set, IBL, depth -- in that order, because
    /// ibl.glsl names set 3.
    VkPipelineLayout particleComputeLayout = VK_NULL_HANDLE;
    /// Three: frame, particles, and the scene's bindless textures.
    VkPipelineLayout particleDrawLayout = VK_NULL_HANDLE;
    VkPipeline particleEmitPipeline = VK_NULL_HANDLE;
    VkPipeline particleSimulatePipeline = VK_NULL_HANDLE;
    VkPipeline particleSortPipeline = VK_NULL_HANDLE;
    VkPipeline particleSortLocalPipeline = VK_NULL_HANDLE;
    VkPipeline particleDrawPipeline = VK_NULL_HANDLE;

    /// Buffers and descriptor sets, sized from the system. Separate from the pipelines: a
    /// shader hot reload rebuilds those and must not reset the simulation.
    void createParticleResources();
    void destroyParticleResources();
    void destroyParticleResourcesKeepingPool();

    /// Reallocate the per-frame per-view light buffers when a view wanted more lights than
    /// they hold, and rewrite the descriptors that name them. Called at the top of a
    /// frame, before anything records: mid-frame it would be a use-after-free.
    void growLightBuffer();
    void createParticlePipelines();
    void destroyParticlePipelines();
    /// Simulate, emit, sort, draw -- in that order, which particle_emit.comp argues for.
    /// After the forward pass, so blended geometry is already in the target to blend
    /// against.
    void recordParticles(VkCommandBuffer cmd, uint32_t slot);
    /// Last reported spawn-drop count: a pool over budget would otherwise emit one warning
    /// per frame.
    uint32_t reportedParticleDrops = 0;

    VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout = VK_NULL_HANDLE;
    VkPipeline cullPipeline = VK_NULL_HANDLE;
    void createCullPipeline();
    void destroyCullPipeline();

    /// View-projection per cull view, filled in updateUniforms. Index 0 is the camera's,
    /// 1 the sun's and 2.. the punctual atlas's. **Only index 0 is contested by a second
    /// camera view**: an atlas layer is a light's frustum, and `updateSunShadow` takes no
    /// camera.
    glm::mat4 cullViewProj[kCullViews];

    /// Derived from the table when it changes, reported every frame. Properties of the scene
    /// rather than of a frame, so they are not accumulated by whichever frame slot happened
    /// to rebuild its commands.
    uint32_t opaqueDrawCalls = 0;
    uint32_t opaqueInstanceCount = 0;
    uint64_t opaqueTriangles = 0;
    /// Blended commands written for the frame being recorded.
    uint32_t blendedCommandCount = 0;
    /// The blended list's variant groups, and **the one place they are runs rather than
    /// groups**: the list is in depth order, so a variant appearing twice in it gets two
    /// entries. `unmasked` means nothing here and is left zero.
    std::vector<VariantRange> blendedRanges;

    /// Fills lightScratch and uploads it into `view.uniformSlot`'s block. Called from
    /// updateUniforms so the record path only reads.
    ///
    /// **Runs for every view; only the atlas assignment inside it is the primary's.** A
    /// secondary looks its lights up in `lightShadowLayer` instead of assigning layers, and
    /// leaves `shadowMatrixScratch` and the staleness cache alone.
    /// @param viewPosition Ranks the light budget when it binds.
    void updateLights(uint32_t slot, const glm::vec3& viewPosition, const glm::mat4& viewProj);
    /// @param phase 0 clears and draws the previously-visible set; 1 loads and draws
    ///              whatever the occlusion test newly admitted.
    void recordGBuffer(VkCommandBuffer cmd, uint32_t slot, uint32_t phase);
    void recordDecals(VkCommandBuffer cmd, uint32_t slot);
    /// The four colour attachments and the depth, moved to their read-only layouts. Two
    /// passes read them -- the shadow mask and lighting -- so the transition cannot sit in
    /// either without the other naming a layout it has already left.
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

    /// Where the chain being recorded is composed: the view's own destination if it has one,
    /// then the offscreen target where the presentation blit is real, then the swapchain
    /// image itself where it was elided.
    [[nodiscard]] VkImage composeImage(uint32_t imageIndex) const {
        if (view.destination.image != VK_NULL_HANDLE) return view.destination.image;
        return view.presentTarget.image != VK_NULL_HANDLE ? view.presentTarget.image : swap.images[imageIndex];
    }
    [[nodiscard]] VkImageView composeView(uint32_t imageIndex) const {
        if (view.destination.image != VK_NULL_HANDLE) return view.destination.view;
        return view.presentTarget.image != VK_NULL_HANDLE ? view.presentTarget.view : swap.views[imageIndex];
    }
    /// Draw `debugLines` between the tonemap and the overlay, so a wireframe is drawn over
    /// the scene and text over both. **`target` and `extent` rather than a swapchain
    /// index**: with the UI outside the virtual target this draws into the swapchain at the
    /// window's size, *after* the blit.
    void recordDebugLines(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent,
                          const scene::Camera& camera);
    void recordOverlay(VkCommandBuffer cmd, uint32_t slot, VkImageView target, VkExtent2D extent);

    /// @brief Draw `sprites` over the tonemapped image, one instanced draw for all of them
    ///        -- see rendering.md, "Sprites".
    ///
    /// **After the tonemap, not before**, so a display-referred texel reaches the swapchain
    /// unaltered -- at the cost of a sprite that is not occluded by 3D geometry, does not
    /// bloom, and is neither reflected nor fogged. Always into the *virtual* target at
    /// `renderExtent`; only the overlay gets `uiInsideVirtual`'s choice.
    void recordSprites(VkCommandBuffer cmd, uint32_t slot, VkImageView target, const scene::Camera& camera);

    /// Grow the per-frame sprite buffers to hold `count`, doubling, with a
    /// `vkDeviceWaitIdle`. **Called from `drawFrame` before recording begins**, never from
    /// `recordSprites` -- a device wait inside an open command buffer.
    void ensureSpriteCapacity(uint32_t count);

    /// @brief Blit the virtual target into the swapchain at an integer scale, letterboxed.
    ///        Records nothing when the layout is the identity -- see `identityPresent`.
    ///
    /// **A blit rather than a fullscreen draw, for correctness** -- see rendering.md, "Why
    /// the presentation step is a blit and not a fullscreen draw".
    void recordPresent(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex);

    /// Allocate the readback buffer for a pending capture. False cancels the request:
    /// capture unavailable, an undecodable swapchain format, or a failed allocation. Each
    /// case logs why.
    [[nodiscard]] bool beginCapture();
    /// Block on `fence`, write the PNG, and free the readback buffer.
    void finishCapture(VkFence fence);

    /// `--readback`'s image, its size in texels, and slot 0 meaning none.
    uint32_t readbackSlot = 0;
    uint32_t readbackWidth = 0;
    uint32_t readbackHeight = 0;

    std::filesystem::path capturePath;
    bool captureRequested = false;
    /// Allocated per capture rather than kept alive: at 1600x900 this is 5.8 MB, and
    /// a screenshot happens on a keypress, not on a frame.
    GpuBuffer captureStaging;
    uint32_t captureCount = 0;

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

    /// Empty means no target capture is pending. A name rather than a resolved pointer: a
    /// resize between the request and the frame that services it invalidates every image.
    std::string targetCaptureName;
    std::filesystem::path targetCapturePath;
    /// Subresource within the named target. UINT32_MAX means "every one of them".
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

    /// Where presented frames go, or null. Owned by `Engine`.
    core::Recorder* recorder = nullptr;
    /// One readback buffer per frame slot, allocated for the life of the recording -- not
    /// the one-per-capture allocation `captureStaging` uses, which is right for a screenshot
    /// and wrong thirty times a second.
    GpuBuffer recordStaging[kFramesInFlight];
    /// Frames the slot's buffer is owed to the recorder, or 0 when it holds nothing.
    /// Carried across the two frames between recording the copy and reading it back.
    uint32_t recordSlotRepeat[kFramesInFlight]{};
    /// Start of the recording, which is what `Recorder::framesOwed` measures from.
    std::chrono::steady_clock::time_point recordStart{};
    /// Hand slot `slot`'s pixels to the recorder and mark it empty. **After the fence wait
    /// at the top of a frame**, which is what makes the data ready without a fence of its
    /// own.
    void drainRecordSlot(uint32_t slot);

    [[nodiscard]] FrameResult handleResize();

    VulkanContext* ctx = nullptr;
    GLFWwindow* window = nullptr;
    const scene::GltfScene* scene = nullptr;
    /// What to draw. Not owned; main() outlives the renderer.
    const scene::InstanceTable* instances = nullptr;
    /// Not owned. Retained so a shader hot reload can re-run the environment bake.
    Uploader* uploader = nullptr;

    bool vsyncEnabled = true;
    bool resizeRequested = false;
    bool pipelinesDirty = false;
    /// Whether `rebuildAccelIfStale` has said so. Once per run, not once per rebuild.
    bool staleAccelReported = false;
    /// The set of instances changed; rebuild at the next `rebuildAccelIfStale`. A flag
    /// rather than a comparison because `staticTierStale` walks the slots the structure
    /// *baked*, so a slot that appeared since is invisible to it.
    bool accelDirty = false;
    /// Whether `createPipelines` has ever run. Only the first `setScene` builds eagerly;
    /// every later one marks `pipelinesDirty` and lets the frame rebuild once.
    bool pipelinesBuilt = false;
    /// Only the sample count needs these rebuilt: a feature toggle changes which pipelines
    /// exist, not which images do.
    bool renderTargetsDirty = false;

    Swapchain swap;
    GpuProfiler gpuProfiler;
    /// False when the device has no timestamp support, which makes every GPU zone
    /// read 0.000 ms. Reported rather than printed as zeros.
    bool gpuTimingAvailable = false;

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_4_BIT;
    core::DebugView debugView = core::DebugView::Lit;

    /// @brief Everything a frame draws into, at one extent, with the descriptor sets that
    ///        name it. One per live view, each at that view's own extent.
    ///
    /// `createRenderTargets` fills one and `destroyRenderTargets` is its exact inverse --
    /// **the two lists must stay identical**, which is the only invariant here a compiler
    /// cannot check.
    struct View {
        /// **Where this view's tonemap lands, when it is not the swapchain's.** Empty for
        /// the view that presents, which falls through to `presentTarget` or the swapchain
        /// image.
        ///
        /// **`destroyRenderTargets` does not free it**: a window resize rebuilds the targets
        /// while the descriptor array still names this image, so it is released where the
        /// image slot it is bound into is given up.
        GpuImage destination;
        /// Whether this is the view that presents: it runs the full cull rather than the
        /// camera alone, its light ranking owns the shadow atlas, and it is the only one
        /// whose tonemap can reach the swapchain.
        bool primary = true;
        /// Whether TAA runs for the chain being recorded; `taaEnabled` is the user's switch
        /// and this is what the passes read.
        bool taaActive = false;
        /// Which of a frame slot's `kMaxViews` uniform blocks this view reads, and the only
        /// thing that decides which `frameSet` a pass binds.
        uint32_t uniformSlot = 0;
        /// Last frame's unjittered view-projection **for this view**, uploaded as
        /// `FrameUniforms::prevViewProj`. Reprojecting view B against view A's matrix is a
        /// smear that only appears once the camera moves.
        glm::mat4 prevViewProj{1.0f};
        /// Instances that survived this view's cull, from the last completed frame in this
        /// slot. Two frames stale by construction.
        uint32_t visibleInstances = 0;
        /// Triangles those instances draw at the LOD level the cull selected. Same
        /// staleness and the same buffer.
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
        /// Whichever of the two above the forward pass tests against, so the record path has
        /// no sample-count branch.
        VkImage forwardDepth = VK_NULL_HANDLE;
        VkImageView forwardDepthView = VK_NULL_HANDLE;
        /// Single-sample: the per-sample resolve has already happened by this point.
        GpuImage hdrTarget;

        /// What every pass draws at: `virtualExtent` where a game set one, the window's
        /// extent otherwise. **Every `renderArea`, viewport, dispatch round-up and
        /// inverse-texel push constant reads this**, so everything sized from it agrees by
        /// construction.
        VkExtent2D renderExtent{};
        /// Recomputed with the render targets, which is every resize -- the scale is a
        /// function of the window.
        PresentLayout presentPlan{};
        /// @brief Where the tonemap draws when the blit is not an identity. Empty otherwise,
        ///        and that emptiness *is* the elision test at record time.
        ///
        /// **Carries `swap.format` rather than a format of its own**, which is what lets the
        /// blit be a byte move.
        GpuImage presentTarget;

        /// Raw AO, then the blurred result the lighting pass actually reads. Two images
        /// rather than one blurred in place: the blur reads a neighbourhood, so writing into
        /// its own source would make the result depend on dispatch order.
        GpuImage ssaoRaw;
        GpuImage ssaoBlurred;
        /// Half `renderExtent`, and the pass runs at it. Held rather than recomputed: the
        /// dispatch, the push constant's texel and the two images must all agree, and
        /// deriving it in two places is how they stop agreeing.
        VkExtent2D ssaoExtent{};
        VkImageView ssaoRawStorage = VK_NULL_HANDLE;
        VkImageView ssaoBlurStorage = VK_NULL_HANDLE;
        VkDescriptorSet ssaoSet = VK_NULL_HANDLE;
        VkDescriptorSet ssaoBlurSet = VK_NULL_HANDLE;

        /// Traced shadow visibility, one bit per light, one array layer per MSAA sample.
        /// `renderExtent` and `msaaSamples` layers, **always allocated**: the lighting
        /// shader declares the descriptor whether or not the pass runs, and
        /// ENABLE_SHADOW_MASK gates the read.
        GpuImage shadowMask;
        /// The 2D_ARRAY view both passes bind. Written as a storage image and read as one,
        /// so it never leaves VK_IMAGE_LAYOUT_GENERAL and the only thing between the write
        /// and the read is a memory barrier.
        VkImageView shadowMaskStorage = VK_NULL_HANDLE;
        VkDescriptorSet shadowMaskSet = VK_NULL_HANDLE;

        /// Half-resolution mip chain.
        GpuImage bloomChain;
        /// One per mip: `imageStore` needs a single-mip view, and the sampled path uses
        /// bloomChain.view with an explicit LOD instead.
        VkImageView bloomStorageViews[kBloomMips]{};
        /// [0] thresholds hdrTarget into mip 0; [i>0] halves mip i-1 into mip i.
        VkDescriptorSet bloomDownSets[kBloomMips]{};
        /// [i] adds mip i+1 back onto mip i. The last entry is unused.
        VkDescriptorSet bloomUpSets[kBloomMips]{};

        /// Min-reduced depth, power-of-two sized, rebuilt from **this** frame's depth
        /// between the two G-buffer passes. A pyramid one frame stale drops things that just
        /// became visible.
        GpuImage depthPyramid;
        VkImageView depthPyramidStorage[kDepthPyramidMips]{};
        VkDescriptorSet depthPyramidSets[kDepthPyramidMips]{};
        VkExtent2D depthPyramidExtent{};
        uint32_t depthPyramidLevels = 0;
        /// The pyramid's sampled-only view, handed to the cull dispatch. Its own set rather
        /// than a binding in `cullSet`: the pyramid is recreated with the render targets
        /// while `cullSet` is rewritten with the instance capacity, and coupling them lets a
        /// resize leave the cull set pointing at a destroyed view.
        VkDescriptorSet hizSet = VK_NULL_HANDLE;

        /// One bit per light per `kLightTileSize` tile, `Renderer::lightTileWords` words per
        /// tile. Device-local; never read by the host.
        ///
        /// **Per view rather than per frame slot**: its extent decides the tile count, so it
        /// is rebuilt by `createRenderTargets` on every resize -- which a `FrameSync` member
        /// could not be, since frame resources survive a swapchain rebuild.
        GpuBuffer lightTiles;
        /// Tiles across and down. Held rather than recomputed for the reason `ssaoExtent`
        /// is: the dispatch, the push constant and the buffer's size must agree.
        VkExtent2D lightTileGrid{};
        VkDescriptorSet lightTileSet = VK_NULL_HANDLE;

        /// Reflection radiance only, not the composited image: a fullscreen additive draw
        /// adds it, which keeps hdrTarget from being both the source a ray samples and the
        /// destination it writes.
        GpuImage ssrTarget;
        /// `renderExtent` scaled by `ssrScale`, and the pass runs at it. Held for the reason
        /// `ssaoExtent` is. Equal to `renderExtent` at scale 1.0, which is what selects the
        /// full-resolution composite.
        VkExtent2D ssrExtent{};
        VkImageView ssrStorage = VK_NULL_HANDLE;
        VkDescriptorSet ssrImageSet = VK_NULL_HANDLE;
        VkDescriptorSet ssrCompositeSet = VK_NULL_HANDLE;

        /// Premultiplied in-scattered radiance in rgb, opacity in alpha. Shares the
        /// composite pipeline layout and shader with SSR; only the blend state differs.
        GpuImage fogTarget;
        VkImageView fogStorage = VK_NULL_HANDLE;
        VkDescriptorSet fogImageSet = VK_NULL_HANDLE;
        VkDescriptorSet fogCompositeSet = VK_NULL_HANDLE;

        /// Ping-ponged: [p] is written this frame and read as history the next. Two images
        /// rather than one, because the resolve reads a *reprojected* texel -- some other
        /// pixel's -- so writing in place would race with a neighbour's read.
        GpuImage taaHistory[2];
        VkImageView taaHistoryStorage[2]{};
        /// [p] reads the current HDR image and history[1-p], and stores into history[p].
        VkDescriptorSet taaSet[2]{};
        /// [p] hands history[p] plus the bloom chain to the tonemap pass, in place of the
        /// `hdrSet` it binds when TAA is off. What lets the resolve ping-pong without a
        /// full-screen copy back into hdrTarget every frame.
        VkDescriptorSet taaOutputSet[2]{};
        uint32_t taaHistoryIndex = 0;
        /// False until a frame has been resolved into the history. Set false by anything
        /// that invalidates it: a resize, a resolution change, or TAA being switched on.
        /// **History is per view** -- a shared one smears each view into the other.
        bool taaHistoryValid = false;

        /// How far reprojecting depth is wrong because the object moved too, in UV. RG16F
        /// and **single-sample**, and one render pass cannot mix sample counts, which is
        /// what makes this a pass of its own rather than a fifth G-buffer attachment.
        ///
        /// **Zero is the identity, so the clear is the static-geometry path.** Nothing
        /// writes it unless the table holds a dynamic instance.
        GpuImage velocityTarget;

        /// The G-buffer attachments as sampled inputs to the lighting pass, and the composed
        /// HDR image as the tonemap's input.
        VkDescriptorSet gbufferSet = VK_NULL_HANDLE;
        VkDescriptorSet hdrSet = VK_NULL_HANDLE;
        /// The single-sample scene depth -- the MSAA resolve at 4x, gDepth itself at 1x --
        /// bound through `singleImageSetLayout`. Shared by the decal pass and the particle
        /// simulate dispatch.
        VkDescriptorSet sceneDepthSet = VK_NULL_HANDLE;
    };

    /// @brief The presenting view's targets, and — for the length of one chain — whichever
    ///        view is being recorded, swapped in and out around it.
    ///
    /// The presenting view's set lives here between frames, which is what lets the capture
    /// paths, `renderTargetExtent()` and `captureTargets()` keep meaning the frame that was
    /// presented.
    View view;

    /// @brief A view a game asked for: its targets, and the bookkeeping that says whether
    ///        they still describe what the table asked for.
    struct ViewSlot {
        /// Everything this view draws into, at its own extent, plus the state that has to
        /// survive between frames: the matrix TAA reprojects against, its history index
        /// and the cull counters the overlay reports.
        View targets;
        /// The extent `targets` was built at. Compared rather than remembered as a flag, so
        /// a `ViewTable::resize` and a window resize under a view that follows it are one
        /// test instead of two rules.
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
    /// presented.
    std::vector<ViewSlot> extraViews;
    const ViewTable* views = nullptr;
    uint64_t viewRevision = 0;
    /// Bring `extraViews` in line with the table. At the top of `drawFrame`, after
    /// `syncImages`, because it writes into the array that one sizes.
    void syncViews();
    /// Pixels a table entry renders at: what it asked for, or the presenting view's where it
    /// asked for nothing. One function, so `syncViews` and `createViewTargets` cannot
    /// disagree about it.
    [[nodiscard]] VkExtent2D viewExtent(const ViewTable::Entry& e) const;
    /// Build one registered view's targets and destination at `extent`, releasing whatever
    /// it held first. Both halves: doing one leaves a view sampling an image the other half
    /// no longer sizes.
    void createViewSlot(ViewSlot& v, VkExtent2D extent);
    void destroyViewSlot(ViewSlot& v);
    /// One destination, at `extent` and in the swapchain's format.
    [[nodiscard]] GpuImage createViewDestination(VkExtent2D extent) const;
    /// Put every live view's destination into the image descriptor array and rewrite it.
    void bindViewDestinations();

    /// @brief Record one complete chain, from the instance upload to the tonemap. Where it
    ///        lands is `view.destination`, or whatever `composeImage` decides.
    ///
    /// **Shadows and the acceleration refit are not here**: they run once per frame from
    /// `drawFrame`, before any chain.
    void recordViewChain(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& camera, uint32_t imageIndex);
    /// @brief Order one chain's last write against the next chain's first read.
    ///
    /// **Still needed with a target set per view**: cull list 0, the per-command visibility
    /// buffer and the indirect command region are all in the frame slot rather than in a
    /// `View`, so chain B's cull dispatch overwrites the commands chain A's indirect draws
    /// are still reading.
    void recordViewBarrier(VkCommandBuffer cmd);

    /// Chosen from what the device reports; see createRenderTargets().
    VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    VkSampler pointSampler = VK_NULL_HANDLE;

    VkPipelineLayout ssaoLayout = VK_NULL_HANDLE;
    VkPipeline ssaoPipeline = VK_NULL_HANDLE;
    VkPipeline ssaoBlurPipeline = VK_NULL_HANDLE;

    /// Whether the traced paths will actually run this frame: the toggle, and a device and
    /// scene that can serve them. Set once in createPipelines, so the SSR variant and the
    /// ENABLE_RT constant that gates the shader branches cannot disagree.
    bool rtActive = false;

    /// Whether the shadow-mask pass runs and the lighting shader reads it -- `rtActive`, the
    /// two shadow toggles, and more than one sample to share a ray between. Set beside
    /// `rtActive`, so the pass that writes the mask and the constant that gates the read
    /// cannot disagree.
    bool shadowMaskActive = false;

    VkPipelineLayout shadowMaskLayout = VK_NULL_HANDLE;
    VkPipeline shadowMaskPipeline = VK_NULL_HANDLE;

    /// One storage buffer, written in compute and read in the fragment stage. Its own layout
    /// rather than a sixth binding on the frame set: the buffer's size is a function of the
    /// render extent, and the frame set is not rebuilt by a resize.
    VkDescriptorSetLayout lightTileSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout lightTileLayout = VK_NULL_HANDLE;
    /// The multisampled or 1x build shader, chosen in `createPipelines` as the lighting
    /// variant is -- it reads gDepth.
    VkPipeline lightTilePipeline = VK_NULL_HANDLE;
    /// Mask words per tile, `ceil(lightBufferCapacity / 32)`. **Zero means the pass cannot
    /// run**, which is what a budget past `kLightTileMaxWords * 32` reduces to; nothing
    /// else sets it to zero, and `render.lightTiles` is a separate question asked at
    /// record time.
    uint32_t lightTileWords = 0;
    /// Whether the build will run for the chain being recorded. Decided in `updateUniforms`,
    /// where `tileParams` is written, so the stride the shaders index by and the dispatch
    /// that fills the buffer cannot disagree.
    bool lightTilesActive = false;
    void recordLightTiles(VkCommandBuffer cmd, uint32_t slot);

    /// Linear + clamp. Clamp specifically: a wrapping sampler pulls the opposite edge of the
    /// screen into the blur, which shows up as a bright rim on the wrong side.
    VkSampler bloomSampler = VK_NULL_HANDLE;
    /// One sampled source, one storage destination, both in the compute stage. Shared by
    /// bloom and SSAO.
    VkDescriptorSetLayout computeImageSetLayout = VK_NULL_HANDLE;

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

    VkPipelineLayout ssrLayout = VK_NULL_HANDLE;
    VkPipeline ssrPipeline = VK_NULL_HANDLE;
    VkPipelineLayout ssrCompositeLayout = VK_NULL_HANDLE;
    VkPipeline ssrCompositePipeline = VK_NULL_HANDLE;
    /// The composite for a reduced `ssrExtent`: same draw, but it reads the G-buffer depth
    /// so it can reject the low-resolution texels that belong to another surface. Built only
    /// when `ssrScale` is below 1.0.
    VkPipelineLayout ssrUpsampleLayout = VK_NULL_HANDLE;
    VkPipeline ssrUpsamplePipeline = VK_NULL_HANDLE;
    /// The scale the live `ssrTarget` was built at. `ssrScale` can be written at any time;
    /// comparing the extents it produces is what turns that into one rebuild rather than one
    /// per frame.
    float builtSsrScale = rowDefault::ssrScale;

    VkPipelineLayout fogLayout = VK_NULL_HANDLE;
    VkPipeline fogPipeline = VK_NULL_HANDLE;
    VkPipeline fogCompositePipeline = VK_NULL_HANDLE;

    VkPipelineLayout velocityLayout = VK_NULL_HANDLE;
    VkPipeline velocityPipeline = VK_NULL_HANDLE;
    /// Commands written into `velocityCmdRegion` this frame, and where the deformed half of
    /// them starts -- the same static-then-skinned split the opaque list uses, so each half
    /// draws with its own vertex buffer bound.
    uint32_t velocityCommandCount = 0;
    uint32_t velocityStaticCount = 0;

    VkDescriptorSetLayout taaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout taaLayout = VK_NULL_HANDLE;
    VkPipeline taaPipeline = VK_NULL_HANDLE;
    /// Last frame's `taaEnabled`, so a toggle can invalidate the history. Nothing signals a
    /// public member being assigned, and TAA has no specialisation constants to put in
    /// `featureKey()`.
    bool taaWasEnabled = false;

    /// Static geometry in one baked BLAS, plus a BLAS per deformed instance refitted every
    /// frame, under a TLAS rebuilt every frame. See AccelStruct.h.
    SceneAccelStruct accel;
    /// Rebuild both tiers from the current scene, instance table and deformed vertex buffer.
    /// **Called from `setScene`, and again from `setAnimator`** -- the dynamic tier is built
    /// over the deformed buffer, which does not exist the first time.
    void buildAccelerationStructures();

    /// Lights uploaded this frame, after the sun is prepended.
    std::vector<GpuLight> lightScratch;
    /// Indices into `lights`, ranked by importance. **Only filled on frames where a budget
    /// actually binds** -- ranking when nothing is dropped would move pixels for nothing.
    std::vector<uint32_t> lightRankScratch;
    /// Indices of the lights that survived the frustum test this frame.
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
    /// **A secondary view ranks its own lights but cannot assign layers**:
    /// `recordPunctualShadows` has already rendered the atlas by the time a secondary chain
    /// gets here, so a light only that view chose illuminates without occluding.
    std::vector<float> lightShadowLayer;
    /// Scene lights the view volume rejected on the last update. Reported, never silent.
    uint32_t culledLights = 0;
    /// **The report fires when the count changes, not every frame**: a scene 9 lights over
    /// budget at 600 FPS would otherwise emit 5400 identical warnings a second.
    uint32_t reportedLightDrops = 0;

    /// Lights `lightBuffer` was actually allocated for, and the only number the ranking
    /// binds against. `lightBudget` is a floor read once at init; this grows past it
    /// whenever a view wanted more.
    uint32_t lightBufferCapacity = 0;
    /// The largest number of lights any view was short by, recorded during a frame and spent
    /// by `growLightBuffer` at the top of the next one. Zero once the growth has happened.
    uint32_t lightsWanted = 0;

    /// All four are computed once at startup and never touched again, so there is no
    /// per-frame IBL cost beyond sampling.
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
    /// One sampled image in the fragment stage. Shared by the font atlas, the SSR and fog
    /// composites, and the decal pass.
    VkDescriptorSetLayout singleImageSetLayout = VK_NULL_HANDLE;
    /// One storage image in the fragment stage. The shadow mask's, and only its: widening
    /// `computeImageSetLayout`'s stage flags for it widens them for bloom, SSAO and the
    /// depth pyramid too.
    VkDescriptorSetLayout storageImageSetLayout = VK_NULL_HANDLE;
    /// The sun's map and its comparison sampler, bound at set 2 binding 0, beside the TLAS
    /// at binding 2.
    GpuImage shadowMap;

    /// Depth array for punctual shadows, at a different resolution from the sun's map and
    /// with a perspective projection per layer.
    GpuImage punctualShadowMap;
    VkImageView punctualShadowLayerViews[kMaxShadowLayers]{};

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

    /// The scene TLAS: set 2 in the raster passes, set 3 in the tracing ones, binding 2 in
    /// both.
    VkDescriptorSetLayout tlasSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    /// What each layout above was created from, kept only so `verifyShaderBindings` has
    /// something to compare a reflected binding against -- **Vulkan offers no way to read a
    /// `VkDescriptorSetLayout` back**. Never read in Release.
    std::unordered_map<VkDescriptorSetLayout, std::vector<VkDescriptorSetLayoutBinding>> layoutBindings;

    /// The overlay's own image array: slot zero the font atlas, the rest whatever `images`
    /// holds. Its own set rather than the scene's, which does not exist until `setScene`
    /// while the overlay draws before one is loaded.
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
    VkPipeline debugLinePipeline = VK_NULL_HANDLE;
    VkPipelineLayout debugLineLayout = VK_NULL_HANDLE;

    /// The layers and the order. Owned by `Engine`; this holds the buffer the sorted array
    /// is copied into and the one pipeline that draws it.
    const scene::SpriteTable* sprites = nullptr;
    /// One storage buffer at binding 0. Its own layout rather than the frame set's: this
    /// pass reads no light, no shadow matrix and no frame uniform, and takes the one matrix
    /// it wants through a push constant.
    VkDescriptorSetLayout spriteSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout spriteLayout = VK_NULL_HANDLE;
    VkPipeline spritePipeline = VK_NULL_HANDLE;
    /// Sprites the per-frame buffers can hold. Doubles; zero until the first draw.
    uint32_t spriteCapacity = 0;

    ui::Font debugFont;
    /// Nearest, not linear: the atlas is drawn at 1:1 texel-to-pixel, and filtering a
    /// bitmap font at that ratio only blurs it.
    VkSampler fontSampler = VK_NULL_HANDLE;
    /// One binding of `imageCapacity` combined image samplers, rebuilt every time the array
    /// doubles, which rebuilds the pipelines with it. **A declared count the array cannot
    /// fill is charged per draw by the validation layer**: declaring the device's ceiling
    /// cost 8.5 ms a frame in Debug.
    VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
    /// The set's own pool. Recreated by `ensureImageCapacity`; a pool's sizes are fixed at
    /// creation, so growth cannot be served out of the old one.
    VkDescriptorPool overlayImagePool = VK_NULL_HANDLE;
    /// Parallel to `images`'s slots, so index n here is descriptor n there. Slot zero is
    /// the font atlas and stays empty in this vector -- `debugFont` owns that image.
    std::vector<GpuImage> overlayImages;
    /// The generation resident in each slot, or 0 for "nothing is". Compared against
    /// `ImageTable::at(s).generation` to find what changed; a slot destroyed and
    /// reacquired differs here even though its index did not move.
    std::vector<uint32_t> overlayResident;
    /// Whether `overlayImages[s]` is a handle this class owns or a copy of one a view owns.
    /// Beside the handle rather than on the table's entry: teardown runs after the table is
    /// gone and still has to know which images are not its to free.
    std::vector<uint8_t> overlayBorrowed;
    /// The images a game loaded. Owned by `Engine`.
    const ImageTable* images = nullptr;
    uint64_t imageRevision = 0;
    /// Descriptors allocated in `overlaySet`. Doubles, capped at `imageSlotCeiling`.
    uint32_t imageCapacity = 0;
    /// The device's bound on the array, from its own limits. See `maxImageSlots()`.
    uint32_t imageSlotCeiling = 0;
    std::vector<OverlayVertex> overlayScratch;

    /// Blended instance slots keyed by view depth, re-sorted every frame: the order is a
    /// property of where the camera is, not of the scene.
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
    /// present. **Consumed and cleared by the next frame**, whose wall span is the one that
    /// contains these three blocks.
    double frameBlockedMs = 0.0;

    FrameSync frames[kFramesInFlight];
    std::vector<VkSemaphore> renderFinished;

    uint32_t frameSlot = 0;
    uint64_t framesSubmitted = 0;
};

} // namespace gfx
