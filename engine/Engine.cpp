#include "Engine.h"

#include "Modules.h"

#include "Game.h"
#include "core/InputGlfw.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Resources.h"
#include "gfx/FrameCapture.h"
#include "gfx/RenderDoc.h"
#include "scene/SceneParse.h"
#include "scene/WorldSave.h"

#include <volk.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace {

#ifdef SUBSTRATE_DEBUG
constexpr bool kDebugBuild = true;
#else
constexpr bool kDebugBuild = false;
#endif

void onGlfwError(int code, const char* description) {
    core::Logger::error(core::LogCategory::Core, "GLFW error %d: %s", code, description);
}

/**
 * @brief Place `count - 1` more copies of the scene's first skinned mesh.
 *
 * Called after `setInstances`, the renderer sizes its buffers from a slot count that is
 * already stale.
 *
 * Spread along Z because `Camera::frameBounds` aims down the longest horizontal axis, so a
 * row across it is one the default camera can see end to end.
 */
uint32_t spawnExtraCharacters(const scene::GltfScene& scene, scene::InstanceTable& instances, scene::SceneAnimator& animator,
                              uint32_t count) {
    if (count <= 1 || animator.characterCount() == 0) return 0;

    // The first skinned placement is the template. Every copy shares its primitive, its
    // influences and its skin, and differs in exactly two things: where it stands and
    // which character deforms it.
    const scene::Placement* templatePlacement = nullptr;
    for (const scene::Placement& p : scene.placements()) {
        if (p.skin != 0xFFFFFFFFu && scene.primitives()[p.primitive].skinOffset != 0xFFFFFFFFu) {
            templatePlacement = &p;
            break;
        }
    }
    if (templatePlacement == nullptr) return 0;

    // Every placement driven by the same skin, not just the template's own node. A
    // character is usually several meshes on several nodes -- the Mixamo rig is a body
    // and a separate set of joint caps, on nodes 66 and 67 -- and matching on the node
    // copies one of them. The symptom is a row of characters missing their skin, or
    // their skeleton, depending which node came first.
    std::vector<const scene::Placement*> parts;
    for (const scene::Placement& p : scene.placements()) {
        if (p.skin == templatePlacement->skin && scene.primitives()[p.primitive].skinOffset != 0xFFFFFFFFu) {
            parts.push_back(&p);
        }
    }

    const float spacing = std::max(scene.boundsMax.x - scene.boundsMin.x, 1.0f) * 0.12f;
    uint32_t created = 0;

    for (uint32_t i = 1; i < count; ++i) {
        const scene::AnimatorId character = animator.create(templatePlacement->skin);
        if (!character.valid()) break;
        ++created;

        // Centred on the original, so a row of five straddles where the one stood
        // rather than growing off in one direction from it.
        const float offset = (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5f) * spacing;
        const glm::mat4 place = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, offset));

        for (const scene::Placement* part : parts) {
            const scene::Primitive& prim = scene.primitives()[part->primitive];
            if (prim.indexCount == 0) continue;

            scene::InstanceDesc d;
            d.primitive = part->primitive;
            d.material = prim.materialIndex >= 0 ? static_cast<uint32_t>(prim.materialIndex) : 0u;
            d.firstIndex = prim.firstIndex;
            d.indexCount = prim.indexCount;
            d.baseVertex = prim.baseVertex;
            d.vertexCount = prim.vertexCount;
            d.skinOffset = prim.skinOffset;
            d.morphOffset = prim.morphOffset;
            d.morphTargets = prim.morphTargets;
            d.skin = part->skin;
            // The slot, not the handle. `InstanceDesc::character` becomes
            // `GpuInstance::meta.w`, which the skinning dispatch indexes -- a generation
            // cannot cross to the GPU, so this is the boundary where a handle becomes a
            // bare index again. `SceneAnimator::destroy` is written around that fact.
            d.character = character.index;
            d.localMin = prim.localMin;
            d.localMax = prim.localMax;
            // A skinned mesh's vertices already reach model space, so the copy's
            // transform is the offset alone -- multiplying by the placement's would
            // apply the skeleton root twice, which is the trap the loader's own comment
            // records.
            d.transform = place;
            d.blended = prim.blended;
            (void)instances.create(d);
        }
    }
    return created;
}

} // namespace

// Defined here, not defaulted in the header -- see the declarations in `Engine.h`.
Engine::Engine() = default;

Engine::~Engine() { teardown(); }

bool Engine::init(int argc, char** argv, Game& game) {
    activeGame = &game;

    // Before anything else is set up, so the file is located first; every other flag
    // overrides what it provided.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) configPath = argv[i + 1];
    }

    // Ahead of the other flags because the "this key moved" warnings are logged while the
    // config file is read, below. Taking the log off the terminal any later interleaves them
    // with a document that has a machine consumer. They still reach the log file.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-settings=json") == 0) core::Logger::setOutput(core::LogOutput::File);
    }

    // The working directory wins; only a relative name that is not there falls back beside
    // the executable, which is the packaged case where nobody chose the working directory.
    // Reversing the two would make `--config` name a file relative to the binary instead of
    // to where you are standing.
    //
    // Reads only. Log, trace and capture paths stay relative to the working directory --
    // scripts/golden.sh, scripts/baseline.py and scripts/rdoc.sh all expect debug_frames/ at
    // the repo root.
    if (std::error_code ec; configPath.is_relative() && !std::filesystem::exists(configPath, ec)) {
        if (std::filesystem::path beside = core::executableDir() / configPath; std::filesystem::exists(beside, ec)) {
            configPath = std::move(beside);
        }
    }

    // Before the file is read, and frozen the moment it returns. `loadJson` walks the *file*
    // rather than the table, so a row declared any later has already had its key reported as
    // the user's typo and keeps its built-in. `--write-default-config` and `--help` exit
    // from inside `applyCommandLine` and need the schema complete before either.
    game.declareSettings(configData.settings);
    configData.settings.freezeRows();

    if (!configData.loadFromFile(configPath)) {
        exitCodeValue = 1;
        return false;
    }
    // **The slot this call occupies is what makes the precedence real**: the file has been
    // read and the flags have not, so `Default < Config < Game < Cli` holds by construction
    // rather than by a rule inside the setter. Move it below `applyCommandLine` and a game's
    // `set` beats the command line.
    game.configure(setup, configData.settings);

    if (!configData.applyCommandLine(argc, argv, exitCodeValue)) return false;

    // Settled after the flags, not before: the game fills in only what the command line
    // left empty.
    if (configData.benchmark.capturePath.empty()) configData.benchmark.capturePath = setup.tools.capturePath;
    if (configData.benchmark.rdocCapturePath.empty()) configData.benchmark.rdocCapturePath = setup.tools.rdocCapturePath;
    if (configData.scene.characters == 0) configData.scene.characters = setup.characters;
    configData.settings.setEngineOwned(core::settings::Id::engine_game_name, setup.name);
    configData.settings.setEngineOwned(core::settings::Id::engine_current_scene_path, configData.scene.path.string());

    // The JSON dump keeps the terminal to itself for the whole run, not just until here --
    // `init` would otherwise put the log back on stdout in front of the document.
    core::Logger::init(configData.logging.file, configData.dumpSettings == core::Config::Dump::Json
                                                    ? core::LogOutput::File
                                                    : configData.logging.output);
    core::Logger::setLevel(configData.logging.level);
    core::Logger::setCategories(configData.logCategoryMask());
    core::Logger::status(core::LogCategory::Core, "Substrate starting");
    configData.logSummary();

    // Serviced here rather than inside `applyCommandLine`: a dump taken before
    // `Game::configure` reports the wrong provenance for every row a game writes.
    if (configData.dumpSettings != core::Config::Dump::None) {
        if (configData.dumpSettings == core::Config::Dump::Json) {
            configData.settings.dumpJson(stdout);
        } else {
            configData.settings.dumpTable(stdout);
        }
        exitCodeValue = 0;
        return false;
    }

    core::Profiler::init(configData.profiler);

    // **The startup frame opens here, ahead of every subsystem, and closes in `run` after
    // `Game::init`.** A profiler scope opened outside it records against an empty thread
    // stack, landing at depth 0 as a *sibling* of `Frame` with no path to attribute it by.
    // Every `return false` below leaves it open for `~Engine` to close.
    startupFrameScope.emplace(core::Profiler::beginFrame());

    if (!initWindow()) return false;
    initRenderer();

    // After every feature flag is applied, because which targets are live depends on
    // them, and before the scene loads, because listing names should not cost a
    // 649 MB upload.
    if (configData.benchmark.captureTarget == "list") {
        std::printf("capture targets:\n");
        for (const std::string& name : render.captureTargetNames()) std::printf("  %s\n", name.c_str());
        teardown();
        exitCodeValue = 0;
        return false;
    }

    // **Before anything a game can reach**: `scene().add<scene::Model>` forwards here, so a
    // tree without the importer installed silently imports nothing.
    sceneTree.setImporter(
        [](void* context, scene::NodeId node, const std::filesystem::path& path) -> uint32_t {
            return static_cast<Engine*>(context)->importModel(node, path);
        },
        this);

    loadScene();
    initAudio();
    initPhysics();
    modules::nav->rebuild(sceneData.colliders());
    initLights();
    initInput();
    // After the audio device, because the recorder drains the tap that device feeds, and
    // after the scene, so the seconds it keeps are seconds of the game rather than of a
    // loading screen.
    initRecording();

    simClock = scene::FixedClock(setup.sim.physicsStep, configData.settings.get(core::options::physics::maxStepsPerFrame));
    realtimeClock = configData.physicsRealtimeClock();
    physicsDebugDraw = configData.physics.debugDraw;
    audioDebugDraw = configData.audio.debugDraw;
    uiOpen = configData.ui.panel;
    uiContext.setTextInput(&textInput);

    // Everything an `initOnly` row sized has now been sized. Freezing later lets a raised
    // budget through after the buffer it sizes was allocated, which writes past a mapped
    // range.
    configData.settings.freezeInitOnly();

    ctx.logMemoryUsage("scene loaded");

    if (realtimeClock) {
        core::Logger::status(core::LogCategory::Core,
                       "Simulation: realtime clock -- frame N is no longer a function of N alone, so golden images "
                       "and per-pass measurements taken this way are not comparable with locked ones");
    }

    lastTime = std::chrono::steady_clock::now();
    return true;
}

bool Engine::initWindow() {
    auto zone = core::Profiler::scope("Engine::initWindow");
    // volk must own the loader before GLFW initialises, so both resolve Vulkan
    // entry points through the same dispatch.
    if (volkInitialize() != VK_SUCCESS) {
        core::Logger::critical(core::LogCategory::Vulkan, "volkInitialize failed — no Vulkan loader present");
    }
    glfwInitVulkanLoader(vkGetInstanceProcAddr);

    glfwSetErrorCallback(onGlfwError);
    if (glfwInit() != GLFW_TRUE) core::Logger::critical(core::LogCategory::Core, "glfwInit failed");
    if (glfwVulkanSupported() != GLFW_TRUE) core::Logger::critical(core::LogCategory::Vulkan, "GLFW reports no Vulkan support");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Unmapped, not absent: the window and its surface still exist, so a headless run
    // presents through the path a visible one does and its golden is comparable.
    //
    // **Not GLFW_PLATFORM_NULL.** That routes glfwCreateWindowSurface through
    // vkCreateHeadlessSurfaceEXT, which this driver does not implement while Mesa's lavapipe
    // still advertises it -- the surface is created, the NVIDIA card reports no
    // present-capable queue family, and the suite passes on software-rendered pixels.
    if (configData.window.headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // **A run with a frame budget never steals the keyboard.** Both hints are needed:
    // GLFW_FOCUSED covers the window mapped at creation, GLFW_FOCUS_ON_SHOW the one shown
    // later. Neither binds the window manager, which is why the harnesses pass `--headless`
    // as well.
    if (configData.benchmark.exitAfterFrames != 0) {
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }

    window = glfwCreateWindow(configData.settings.get(core::options::window::width),
                              configData.settings.get(core::options::window::height), setup.name.c_str(), nullptr, nullptr);
    if (window == nullptr) core::Logger::critical(core::LogCategory::Core, "glfwCreateWindow failed");

    // An unmapped window never takes focus, so Escape cannot reach the close path and
    // there is no titlebar to click. Without --frames the run has no way to end itself.
    if (configData.window.headless && configData.benchmark.exitAfterFrames == 0) {
        core::Logger::warn(core::LogCategory::Core,
                     "--headless without --frames: the window cannot be focused or closed, so this run "
                     "will only stop on a signal");
    }

    // Read once, before anything sizes itself: the font is baked at a pixel height and the
    // UI lays out in scaled units, so a value changed later leaves the two disagreeing.
    uiScaleValue = configData.settings.get(core::options::ui::scale);
    if (uiScaleValue <= 0.0f) {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetWindowContentScale(window, &scaleX, &scaleY);
        // The larger of the two rather than the average: a UI slightly too big is
        // readable and one slightly too small is not, and no platform this runs on has
        // genuinely non-square pixels.
        uiScaleValue = std::max(std::max(scaleX, scaleY), 0.5f);
    }
    if (uiScaleValue != 1.0f) {
        core::Logger::status(core::LogCategory::Core, "UI scale: %.2fx (%s)", static_cast<double>(uiScaleValue),
                       configData.settings.get(core::options::ui::scale) > 0.0f ? "from config" : "from the window's content scale");
    }
    return true;
}

void Engine::initRenderer() {
    auto zone = core::Profiler::scope("Engine::initRenderer");
    {
        // Named separately because it is the largest thing startup does and it is the
        // driver's time, not ours -- folded into the caller it reads as a slow renderer.
        auto z = core::Profiler::scope("VulkanContext::init");
        ctx.init(window, configData.validationEnabled(kDebugBuild), configData.rayQueryAllowed(),
                 configData.render.syncValidation);
    }

    // After the instance, so the implicit capture layer has been pulled in and
    // librenderdoc.so is already in the process for RTLD_NOLOAD to find. A no-op
    // without ENABLE_VULKAN_RENDERDOC_CAPTURE=1, which is the ordinary case.
    gfx::renderDocAttach(configData.benchmark.rdocCapturePath);

    // Before the renderer: the font atlas is uploaded during Renderer::init.
    { auto z = core::Profiler::scope("Uploader::init"); uploaderData.init(ctx); }

    // Before init(): every render target is sized by the virtual extent and the presentation
    // layout is computed alongside them, so setting this afterwards builds the frame at the
    // window's resolution and then agrees with itself about it -- the flag parses, the field
    // holds, and no target is ever a different size.
    render.virtualExtent = {setup.present.virtualResolution.x, setup.present.virtualResolution.y};
    if (configData.render.virtualWidth != 0 && configData.render.virtualHeight != 0) {
        render.virtualExtent = {configData.render.virtualWidth, configData.render.virtualHeight};
    }
    render.uiInsideVirtual = setup.present.uiInsideVirtual && !configData.render.uiOutsideVirtualNamed;
    // `--readback` is pixel exactness by definition: a run that asked whether a texel
    // survives presentation and left a temporal filter on has asked nothing.
    render.pixelExact = setup.present.pixelExact || !configData.benchmark.readbackImage.empty();
    // The font is baked at the scaled height, so a TTF at 200% is rasterised at twice the
    // size rather than magnified. The embedded bitmap font ignores the height and stays at
    // 16 px; the layout scales around it either way.
    {
        // Braced, or the zone swallows the image table below it as well.
        auto renderZone = core::Profiler::scope("Renderer::init");
        render.init(ctx, uploaderData, window, configData.settings.get(core::options::window::vsync),
                    configData.settings.get(core::options::render::msaaSamples),
                    core::Resources(configData.settings.get(core::options::render::debugFont)).string(),
                    configData.settings.get(core::options::render::debugFontHeight) * uiScaleValue);
    }

    // After `render.init`: the ceiling is a device limit, so there is nothing to size the
    // table from until a device exists.
    imageTable.init(render.maxImageSlots());
    render.setImages(&imageTable);
    uiContext.setImages(&imageTable);

    // `kMaxViews` counts the presenting view, which a game never creates, so the table holds
    // one fewer. Written as a subtraction rather than a second constant to keep in step.
    viewTable.init(gfx::kMaxViews - 1);
    render.setViews(&viewTable);

    spriteTable.init(&imageTable);
    render.setSprites(&spriteTable);

    // Points each row at the renderer's own field rather than copying a value into it, so
    // there is one storage location per setting. Assigning instead reintroduces the case
    // where the config says one thing and the renderer holds another. See
    // engine/gfx/SettingsBind.cpp.
    core::settings::bindRenderer(configData.settings, render);

    render.setDebugView(configData.render.debugView);
    render.debugOverlay = configData.render.debugOverlay;
    render.shaderHotReload = configData.shaderHotReloadEnabled(kDebugBuild);

    // Authored rather than configured; `--tonemap` is the one-run override.
    render.exposure = setup.look.exposure;
    render.tonemapOperator = configData.render.tonemapNamed ? configData.render.tonemap : setup.look.tonemap;
    render.shadowDepthBias = setup.look.shadowDepthBias;
    render.shadowNormalBias = setup.look.shadowNormalBias;
    // The sun is derived, not written here: `initLights` pulls the first directional light
    // into the fields the cascades and the sky read.
    render.ambientColor = setup.look.ambientColor;

    // The renderer fields `pixelExact` needs were set before `render.init` because the
    // targets are sized by them. These two settings have to follow `bindRenderer` instead --
    // `render.taa` is a live-bound row, and writing it before the binding exists writes it
    // into nothing.
    if (render.pixelExact) {
        // Through the settings table rather than by writing the renderer's fields, so
        // `--dump-settings` names `game` in the source column instead of leaving a developer
        // chasing a soft image with nothing to read. Over the user's file, but **not** over
        // the command line, which is what keeps "was it TAA?" answerable in one run.
        const auto claimedByFlag = [&](core::settings::Id id) {
            return configData.settings.source(id) == core::settings::Source::Cli;
        };
        if (!claimedByFlag(core::settings::Id::render_taa)) {
            (void)configData.settings.setValue(core::settings::Id::render_taa, false, core::settings::Source::Game,
                                               "pixelExact");
        }
        // The curve is not a settings row, so `tonemapNamed` asks of it what `claimedByFlag`
        // asks above: a flag still wins.
        if (!configData.render.tonemapNamed) render.tonemapOperator = core::TonemapOperator::Clamp;
    }
}

void Engine::loadScene() {
    auto zone = core::Profiler::scope("Engine::loadScene");
    {
        // `createEmpty` rather than leaving the scene unloaded: an unloaded scene owns no
        // descriptor set layout, and the renderer puts that layout into every pipeline
        // layout it builds, so bring-up dies on a null handle.
        if (configData.scene.path.empty()) {
            core::Logger::status(core::LogCategory::GLTF, "no scene named; starting with nothing loaded");
            if (!sceneData.createEmpty(ctx, uploaderData)) {
                core::Logger::critical(core::LogCategory::GLTF, "Failed to create the empty scene");
            }
        } else {
            // Resolved once: everything the file names -- buffers, images, the .ktx2 beside
            // each one, a sound in `extras` -- resolves relative to this one lookup.
            const core::Resources scene(configData.scene.path.string());
            if (!sceneData.load(ctx, uploaderData, scene, loadedSceneScale)) {
                core::Logger::critical(core::LogCategory::GLTF, "Failed to load %s", scene.string().c_str());
            }
        }
    }
    sceneLoaded = true;

    scene::addSceneInstances(sceneData, instanceTable);

    // The spawn runs *before* `setInstances` below: spawning characters adds slots, and the
    // renderer sizes its buffers from the count it is handed.
    sceneAnimator.init(std::move(sceneData.rig()));
    const uint32_t extraCharacters =
        spawnExtraCharacters(sceneData, instanceTable, sceneAnimator, configData.scene.characters);

    core::Logger::status(core::LogCategory::Scene, "Instances: %u live (%u blended) in %u slots", instanceTable.liveCount(),
                   instanceTable.blendedCount(), instanceTable.slotCount());
    if (sceneAnimator.characterCount() > 0 && !sceneAnimator.empty()) {
        core::Logger::status(core::LogCategory::Scene, "Animation: %u characters, %zu clips (%u spawned by --characters)",
                       sceneAnimator.characterCount(), sceneAnimator.clipCount(), extraCharacters);
    }

    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    render.setAnimator(&sceneAnimator, &sceneData);

    particleSystem.setEmitters(std::move(sceneData.emitters()), 0);
    render.setParticles(&particleSystem);
    if (!particleSystem.empty()) {
        core::Logger::status(core::LogCategory::Scene, "Particles: %zu emitters, pool %u", particleSystem.emitters().size(),
                       particleSystem.capacity());
    }

    // Fog pools from the floor up, so the height reference is the scene's lower bound
    // rather than the world origin -- a scene authored above y=0 would otherwise get
    // fog only in the basement it does not have.
    render.fogBaseHeight = sceneData.boundsMin.y;
}


scene::GltfScene::ModelId Engine::addModel(const std::filesystem::path& path, const glm::mat4& transform) {
    // `appendModel` grows the scene buffers and `setScene` below destroys and rebuilds every
    // pipeline, both of which the command buffers still in flight refer to. A game calls this
    // from `frameUpdate`, ahead of the fence wait in `drawFrame`, so nothing else establishes
    // that those have retired.
    vkDeviceWaitIdle(ctx.device);

    const scene::GltfScene::ModelId id = sceneData.appendModel(ctx, uploaderData, path, transform);
    if (id == scene::GltfScene::kNoModel) return id;

    const scene::GltfScene::LoadedModel& m = sceneData.model(id);
    if (modelInstances.size() <= id) modelInstances.resize(id + 1);
    // **The rig merges before the instances are made.** `addPlacementInstances` reads
    // `Placement::skin` and writes it into the instance as both the skin and the character,
    // so the placements have to be on the merged numbering before a single instance exists;
    // merging afterwards leaves every appended character deformed by the base scene's
    // skeleton.
    //
    // Taken before the merge, which moves both numberings -- the remap below is the only
    // thing that can still tell them apart.
    const uint32_t characterBase = sceneAnimator.characterCount();
    uint32_t skinBase = scene::GltfScene::kNoRig;
    {
        scene::AnimationRig imported = sceneData.takeAppendedRig(id);
        skinBase = imported.bind.nodes.empty() ? scene::GltfScene::kNoRig : sceneAnimator.merge(imported);
        sceneData.rebaseAppendedSkins(id, skinBase);
    }

    scene::addPlacementInstances(sceneData, instanceTable, m.firstPlacement, m.placementCount, &modelInstances[id]);

    /**
     * **A skin index is not a character index**, and `addPlacementInstances` writes only the
     * first -- `Placement::skin` into `GpuInstance::meta.w`, which the skinning dispatch
     * reads as a character. The two agree only for a scene `SceneAnimator::init` built; a
     * scene with no skin still gets `init`'s lone character, and `GameSetup::characters`
     * shifts every later skin by the copies it made.
     *
     * Skip this correction and an imported rig is deformed by the joint block of whatever
     * came before it, with the first standing in its bind pose.
     */
    if (skinBase != scene::GltfScene::kNoRig && sceneAnimator.characterCount() != characterBase) {
        for (const scene::InstanceId instance : modelInstances[id]) {
            // Skinned only. A morph-only placement is given character 0 deliberately, and an
            // unskinned one is given none at all.
            if (instanceTable.drawRanges()[instance.index].skinOffset == scene::SceneAnimator::kNoSkin) continue;
            const uint32_t skin = instanceTable.characterOf(instance.index);
            if (skin < skinBase) continue;
            instanceTable.setCharacter(instance, skin - skinBase + characterBase);
        }
    }

    // `Renderer::lights` is the live list a game pushes to; leaving these in the scene keeps
    // them out of the frame.
    for (uint32_t i = 0; i < m.lightCount; ++i) render.lights.push_back(sceneData.lights()[m.firstLight + i]);
    for (uint32_t i = 0; i < m.audioCount; ++i) (void)audioEngine.create(sceneData.audioSources()[m.firstAudio + i]);
    // `create` rather than rebuilding the list through `setEmitters`: the renderer allocated
    // its buffers against the capacity the scene loaded with, so an import shares the
    // particles the existing emitters did not claim rather than resizing under it.
    for (uint32_t i = 0; i < m.emitterCount; ++i) {
        (void)particleSystem.create(sceneData.emitters()[m.firstEmitter + i]);
    }

    if (m.colliderCount > 0) {
        // Resized rather than reassigned: the sounds the scene loaded with keep the bodies
        // they were already bound to.
        sourceBody.resize(audioEngine.sourceCount());

        std::vector<DrivenBody> added;
        createColliderBodies(m.firstCollider, m.colliderCount, added);

        // Idempotent, and called again for exactly this: it re-snapshots over the slots the
        // bodies above just added, so their rest transforms read as where they are rather
        // than as the identity.
        physicsWorld.finalize();

        std::vector<DrivenSlot> slots;
        slots.reserve(modelInstances[id].size());
        for (uint32_t p = 0, slot = 0; p < m.placementCount; ++p) {
            const scene::Placement& pl = sceneData.placements()[m.firstPlacement + p];
            if (sceneData.primitives()[pl.primitive].indexCount == 0) continue;
            const uint32_t here = slot++;
            if (here >= modelInstances[id].size()) break;
            slots.push_back({pl.colliderNode, modelInstances[id][here]});
        }

        const uint32_t drivenNodes = bindDrivenNodes(added, slots);

        core::Logger::status(core::LogCategory::Scene,
                             "model %u: %u colliders bound, %u driven -- %u bodies, %u characters now",
                             id, m.colliderCount, drivenNodes, physicsWorld.bodyCount(),
                             physicsWorld.characterCount());
    }

    // `setInstances` is the one that is easy to forget: it resizes the indirect command
    // buffer to the new slot count. Without it the last view's command list runs off the
    // end, which validation reports as a draw overrunning `instanceData` -- a long way from
    // here.
    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    // **The navmesh is baked from static mesh colliders, and an import brings some.** Skip
    // this and geometry imported at runtime is solid to the solver and absent from every
    // path query, silently; a game composing its arena out of imports has no load-time bake
    // to fall back on. Gated on colliders because a bake is a real cost for a file that
    // cannot have changed the answer.
    if (m.colliderCount > 0) modules::nav->rebuild(sceneData.colliders());

    if (m.skinCount > 0 || !sceneData.skinVertices().empty() || !sceneData.morphDeltas().empty()) {
        render.setAnimator(&sceneAnimator, &sceneData);
    }
    sceneIndex.build(instanceTable);
    ++indexRevision;
    core::Logger::status(core::LogCategory::Scene, "model %u: %zu instances", id, modelInstances[id].size());
    return id;
}

scene::GltfScene::ModelId Engine::importModel(scene::NodeId node, const std::filesystem::path& path) {
    if (!sceneTree.valid(node)) {
        core::Logger::error(core::LogCategory::Scene, "add<Model>('%s'): the node does not exist",
                            path.string().c_str());
        return scene::GltfScene::kNoModel;
    }

    // `worldTransform` is valid as of the last sweep, which for a node created in
    // `Game::init` is its local transform.
    //
    // A *scale* does not belong in it -- `setWorldScale` is where that goes. A node scaled
    // by two imports a stretched character and a light whose range stayed where it was,
    // because a placement matrix carries none of what `scaleSceneData` does.
    const scene::GltfScene::ModelId id = addModel(path, sceneTree.worldTransform(node));
    if (id == scene::GltfScene::kNoModel) return id;

    /**
     * **The attachment offset is what the file's own node hierarchy said, and nothing else.**
     * The sweep writes `node.worldTransform * offset` back over the instance every frame, so
     * an identity offset erases the document's hierarchy -- a part whose node carries a
     * translation lands somewhere else entirely, with the instance count, the triangle count
     * and every other check still reading correct.
     *
     * `placementLocals` is that value, kept by `appendModel` before it baked the placement
     * in. Recovering it as `inverse(placement) * instanceTable.transform(instance)` is
     * arithmetically the same only while nothing else has written the instance transform
     * first, which nothing enforces.
     */
    const scene::GltfScene::LoadedModel& m = sceneData.model(id);
    const std::span<const scene::InstanceId> instances = instancesOf(id);
    for (uint32_t p = 0, slot = 0; p < m.placementCount && slot < instances.size(); ++p) {
        // The same walk and the same skip `addPlacementInstances` used, which is what makes
        // `slot` agree with the instance it made.
        const scene::Placement& pl = sceneData.placements()[m.firstPlacement + p];
        if (sceneData.primitives()[pl.primitive].indexCount == 0) continue;
        const scene::InstanceId instance = instances[slot++];

        // A driven mesh already belongs to its body's node (`bindDrivenNodes`), and a second
        // attachment here would be a second writer of the same instance's transform every
        // sweep -- with the solver's answer losing to the import's on whichever ran last.
        if ((instanceTable.flags(instance) & scene::kInstanceDynamic) != 0u) continue;

        const scene::NodeId child = sceneTree.create("mesh", node);
        sceneTree.attachInstance(child, instance, p < m.placementLocals.size() ? m.placementLocals[p] : glm::mat4(1.0f));
    }
    return id;
}

scene::GltfScene::ModelId Engine::createMesh(scene::MeshData data) {
    // Read before the move, because it decides three things below and `data` is gone by
    // the time any of them runs.
    const auto targets = static_cast<uint32_t>(data.morphTargets.size());

    // The same drain `addModel` takes, and for the same two reasons: the geometry buffers
    // may grow, which destroys the ones the frames in flight name, and `setScene` below
    // rebuilds every pipeline.
    vkDeviceWaitIdle(ctx.device);

    const scene::GltfScene::ModelId id = sceneData.createMesh(ctx, uploaderData, std::move(data));
    if (id == scene::GltfScene::kNoModel) return id;

    const scene::GltfScene::LoadedModel& m = sceneData.model(id);
    if (modelInstances.size() <= id) modelInstances.resize(id + 1);
    scene::addPlacementInstances(sceneData, instanceTable, m.firstPlacement, m.placementCount, &modelInstances[id]);

    // `addPlacementInstances` defaults a morphed placement to character 0, which is right for
    // a glTF whose rig owns every weight and wrong for a code-made mesh. Repointed before
    // `setAnimator`, which sizes the weight region and lays out one output range per
    // deformed instance off the state as it stands when it is called.
    if (targets > 0) {
        if (modelCharacters.size() <= id) modelCharacters.resize(id + 1);
        modelCharacters[id] = sceneAnimator.createMorphed(targets);
        for (const scene::InstanceId instance : modelInstances[id]) {
            instanceTable.setCharacter(instance, modelCharacters[id].index);
        }
    }

    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    // Only for a mesh that deforms: this tears down and rebuilds the deformed vertex buffer,
    // the delta buffer and the acceleration structures over them, and calling it for a scene
    // with nothing to deform sets `animator` back to null on every prop a game makes.
    if (targets > 0) render.setAnimator(&sceneAnimator, &sceneData);
    sceneIndex.build(instanceTable);
    ++indexRevision;
    return id;
}

void Engine::pairLocomotion(scene::PhysicsCharacterId character, scene::InstanceId instance) {
    if (!character.valid() || !instance.valid()) return;

    // **The instance is the only thing that knows the pairing.** A `CharacterVirtual` is a
    // capsule with no rig and an animator character is a pose with no collider; the skinned
    // mesh bound to this collider's node is what joins them.
    const uint32_t index = instanceTable.characterOf(instance.index);
    if (index == scene::kNoNode || index >= sceneAnimator.characterCount()) return;
    locomotionDriver.pair(character, sceneAnimator.characterAt(index));
}

scene::InstanceId Engine::addInstance(scene::GltfScene::ModelId model, uint32_t material,
                                      const glm::mat4& transform, scene::InstanceMotion motion) {
    if (model == scene::GltfScene::kNoModel || model >= sceneData.modelCount()) return {};

    const scene::GltfScene::LoadedModel& m = sceneData.model(model);
    if (m.primitiveCount == 0) return {};
    const scene::Primitive& prim = sceneData.primitives()[m.firstPrimitive];

    scene::InstanceDesc desc;
    desc.primitive = m.firstPrimitive;
    desc.material = material;
    desc.firstIndex = prim.firstIndex;
    desc.indexCount = prim.indexCount;
    desc.baseVertex = prim.baseVertex;
    desc.vertexCount = prim.vertexCount;
    desc.localMin = prim.localMin;
    desc.localMax = prim.localMax;
    desc.transform = transform;
    desc.dynamic = motion == scene::InstanceMotion::Dynamic;
    const scene::InstanceId id = instanceTable.create(desc);
    if (!id.valid()) return id;

    // Sizes the buffers from `slotCount()` and marks the acceleration structure. Without the
    // first, a slot past the current capacity is a memcpy past the end of a mapped staging
    // range; without the second the new instance is in every raster pass and in no ray.
    // `staticTierStale` does not cover it -- it walks the slots the structure baked, so a
    // slot that *appeared* is invisible to it.
    //
    // Not `setInstances`, which rebuilds the acceleration structure on the spot: this verb is
    // called in a loop, and sixteen rebuilds instead of five took the demo's `Game::init`
    // from 64 ms to 317. The rebuild happens once, in `endFrame`.
    render.instancesGrew();
    sceneIndex.build(instanceTable);
    ++indexRevision;
    return id;
}

std::span<const scene::InstanceId> Engine::instancesOf(scene::GltfScene::ModelId id) const {
    if (id >= modelInstances.size()) return {};
    return modelInstances[id];
}

scene::AnimatorId Engine::morphCharacterOf(scene::GltfScene::ModelId id) const {
    if (id >= modelCharacters.size()) return {};
    return modelCharacters[id];
}

void Engine::removeModel(scene::GltfScene::ModelId id) {
    if (id >= modelInstances.size()) {
        // Almost always a handle from before a scene swap: `applyPendingScene` drops every
        // model the old scene had, and freeing on that id would give back ranges and texture
        // slots belonging to the new one.
        core::Logger::warn(core::LogCategory::Scene, "removeModel: %u is not a model of the current scene", id);
        return;
    }
    // Destroying the model's images and rebuilding the TLAS reaches resources the frames
    // still in flight name. See the note in `addModel`.
    vkDeviceWaitIdle(ctx.device);

    for (const scene::InstanceId instance : modelInstances[id]) instanceTable.destroy(instance);
    modelInstances[id].clear();
    // Retired, not repacked: `meta.w` names the slot, so moving a weight block repoints every
    // instance that still refers to one after it. The mesh's morph deltas stay in the
    // scene's array for the same reason; see `GltfScene::createMesh`.
    if (id < modelCharacters.size() && modelCharacters[id].valid()) {
        sceneAnimator.destroy(modelCharacters[id]);
        modelCharacters[id] = {};
    }
    sceneData.unloadModel(ctx, id);
    render.setInstances(&instanceTable);
    sceneIndex.build(instanceTable);
    ++indexRevision;
}

uint32_t Engine::litSpriteShader() {
    if (litSpriteVariant != kNoLitSpriteShader) return litSpriteVariant;

    gfx::ShaderVariant v;
    v.name = "sprite_lit";
    v.fragmentShader = "sprite_lit.frag";
    // The shadow pass needs its own, or every lit sprite casts a solid rectangle.
    v.shadowFragment = "sprite_lit_shadow.frag";
    // A single sheet textured on both faces. Also what lets `flipX` be a UV swap rather than
    // a negative scale, which would invert the winding.
    v.cullMode = VK_CULL_MODE_NONE;
    litSpriteVariant = render.addShaderVariant(std::move(v));
    return litSpriteVariant;
}

scene::GltfScene::ModelId Engine::createLitSprite(const scene::LitSpriteDesc& desc) {
    const uint32_t variant = litSpriteShader();

    scene::GpuMaterial m{};
    m.baseColorFactor = desc.tint;
    m.emissiveFactor = glm::vec4(desc.emissive, 0.0f);
    m.metallicFactor = desc.metallic;
    m.roughnessFactor = desc.roughness;
    m.alphaCutoff = desc.cutoff;
    m.normalScale = 1.0f;
    // -1 is "no texture, use the factor alone". A lit sprite's image is in the *game's*
    // array, named by `gameImage` below, so none of these may index the scene's.
    m.baseColorTexture = -1;
    m.metallicRoughnessTexture = -1;
    m.normalTexture = -1;
    m.occlusionTexture = -1;
    m.emissiveTexture = -1;
    m.alphaMask = 1u; ///< a cutout, which is what puts the shadow fragment in the pass
    m.gameImage = imageTable.slot(desc.image);
    m.shader = variant;
    // The texel rect. `params` is untyped, and `setLitSpriteUv` rewrites this field.
    m.params = desc.uv;

    const uint32_t material = sceneData.createMaterial(m);
    if (material == UINT32_MAX) return scene::GltfScene::kNoModel;

    // Rotation about the quad's own +Z, then the world position. The pivot is already inside
    // `quadMesh`, so applying one here too makes the sprite orbit the point it was meant to
    // turn about.
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), desc.position);
    if (desc.rotation != 0.0f) transform = glm::rotate(transform, desc.rotation, glm::vec3(0.0f, 0.0f, 1.0f));

    const scene::GltfScene::ModelId id = createMesh(scene::quadMesh({
        .material = material,
        .size = desc.size,
        .pivot = desc.pivot,
        .flipX = desc.flipX,
        .flipY = desc.flipY,
        .transform = transform,
    }));
    if (id == scene::GltfScene::kNoModel) return id;

    if (desc.dynamic) {
        for (const scene::InstanceId instance : modelInstances[id]) {
            instanceTable.setFlags(instance, scene::kInstanceDynamic, 0u);
        }
    }
    return id;
}

void Engine::setLitSpriteUv(scene::GltfScene::ModelId id, const glm::vec4& uv) {
    const std::span<const scene::InstanceId> instances = instancesOf(id);
    if (instances.empty()) {
        core::Logger::warn(core::LogCategory::Scene, "setLitSpriteUv: %u is not a model of the current scene", id);
        return;
    }
    const uint32_t material = instanceTable.slot(instances.front().index).meta.y;
    scene::GpuMaterial m = sceneData.material(material);
    m.params = uv;
    sceneData.setMaterial(material, m);
}

bool Engine::beginLoadScene(const std::filesystem::path& path) {
    if (sceneLoader.busy()) return false;
    pendingScenePath = path;
    // Everything the worker runs here must touch no device -- it is not the frame thread.
    const bool started = sceneLoader.begin(path.string(), [path](scene::SceneData& data, scene::EmbeddedImages& embedded) {
        return scene::loadSceneCpu(path, data, embedded);
    });
    if (started) core::Logger::status(core::LogCategory::Scene, "streaming %s", path.string().c_str());
    return started;
}

void Engine::applyPendingScene() {
    // Above the ready test, so the frames that do nothing are still in the trace -- a device
    // idle and a scene upload land on the frame thread at whatever moment a load completes.
    auto s = core::Profiler::scope("applyPendingScene");
    if (!sceneLoader.ready()) return;

    scene::SceneData data;
    scene::EmbeddedImages embedded;
    if (!sceneLoader.take(data, embedded)) return; // Already logged; the old scene stands.

    // Everything past here needs the device. Only the *parse* came off the frame thread;
    // dropping this wait records a command buffer against a scene being replaced.
    vkDeviceWaitIdle(ctx.device);
    render.setScene(nullptr);
    sceneData.destroy(ctx);

    if (!sceneData.upload(ctx, uploaderData, pendingScenePath, data, embedded)) {
        core::Logger::warn(core::LogCategory::Scene, "streaming %s: upload failed", pendingScenePath.string().c_str());
        return;
    }

    instanceTable = scene::InstanceTable{};
    // With the table rebuilt, every `InstanceId` a previously appended model handed out is
    // stale -- and a fresh table's slots all carry generation 1, which is exactly what those
    // stale handles carry, so `InstanceTable::valid` waves them through. Left behind, the
    // next `removeModel` would destroy live instances of the scene just loaded.
    modelInstances.clear();
    // **This path does not re-init the animator** -- a streamed scene keeps the rig the first
    // one brought -- so without this a weight block belonging to geometry that no longer
    // exists is uploaded every frame for the rest of the session.
    for (const scene::AnimatorId character : modelCharacters) sceneAnimator.destroy(character);
    modelCharacters.clear();
    scene::addSceneInstances(sceneData, instanceTable);
    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    sceneIndex.build(instanceTable);
    ++indexRevision;
    modules::nav->rebuild(sceneData.colliders());
    core::Logger::status(core::LogCategory::Scene, "streamed %s", pendingScenePath.string().c_str());
}


void Engine::initAudio() {
    auto zone = core::Profiler::scope("Engine::initAudio");
    scene::AudioConfig acfg;
    acfg.enabled = configData.settings.get(core::options::audio::enabled);
    acfg.backend = configData.audio.backend;
    acfg.sampleRate = configData.settings.get(core::options::audio::sampleRate);
    acfg.channels = configData.settings.get(core::options::audio::channels);
    acfg.masterVolume = configData.settings.get(core::options::audio::masterVolume);
    acfg.streamThresholdSeconds = configData.settings.get(core::options::audio::streamThresholdSeconds);
    acfg.decodeBudgetBytes = configData.settings.get(core::options::audio::decodeBudgetBytes);
    acfg.listeners = setup.audio.listeners;
    acfg.occlusion = setup.audio.occlusion.enabled && !configData.audio.occlusionOff;
    acfg.occlusionCutoffHz = setup.audio.occlusion.cutoffHz;
    acfg.occlusionGain = setup.audio.occlusion.gain;
    acfg.occlusionAttack = setup.audio.occlusion.attack;
    acfg.occlusionRelease = setup.audio.occlusion.release;
    acfg.occlusionRayMargin = setup.audio.occlusion.rayMargin;
    // Folded from the same expression as `acfg.occlusion`, so the sweep and the filter cannot
    // disagree about `--no-occlusion`.
    sim.occlusion = acfg.occlusion;
    sim.occlusionMargin = setup.audio.occlusion.rayMargin;
    for (const GameSetup::Audio::Bus& b : setup.audio.buses) {
        acfg.buses.push_back({b.name, b.volume, b.duckedBy, b.duckAmount, b.duckAttack, b.duckRelease});
    }

    if (!audioEngine.init(acfg)) return;

    for (const scene::AudioSourceDesc& desc : sceneData.audioSources()) (void)audioEngine.create(desc);

    for (const GameSetup::Audio::Source& s : setup.audio.sources) {
        scene::AudioSourceDesc desc;
        desc.name = s.name;
        // A config-placed source has no document to resolve against, so this is the only
        // place its path can be anchored.
        desc.file = core::Resources(s.file).string();
        desc.transform = glm::translate(glm::mat4(1.0f), s.position);
        desc.bus = s.bus;
        desc.volume = s.volume;
        desc.spatial = s.spatial;
        desc.loop = s.loop;
        desc.minDistance = s.minDistance;
        desc.maxDistance = s.maxDistance;
        desc.occlusion = s.occlusion;
        // Against `audioLoadName`, which is the one spelling table; a second parser here
        // would let the config and the scene disagree about what "stream" means.
        for (uint32_t i = 0; i < 3; ++i) {
            if (s.load == scene::audioLoadName(static_cast<scene::AudioLoad>(i))) desc.load = static_cast<scene::AudioLoad>(i);
        }
        (void)audioEngine.create(desc);
    }

    if (!audioEngine.empty()) {
        core::Logger::status(core::LogCategory::Audio, "Audio: %u sources (%u streamed, %u decoded costing %.1f MiB), %u buses",
                       audioEngine.sourceCount(), audioEngine.streamedCount(), audioEngine.decodedCount(),
                       static_cast<double>(audioEngine.decodedBytes()) / (1024.0 * 1024.0), audioEngine.busCount());
        for (uint32_t slot = 0; slot < audioEngine.sourceCount(); ++slot) {
            const scene::SoundId id = audioEngine.soundAt(slot);
            if (!id.valid()) continue;
            core::Logger::debug(core::LogCategory::Audio, "  %-20s %-8s %6.1fs  bus=%-10s %s", audioEngine.source(id).name.c_str(),
                          audioEngine.sourceStreamed(id) ? "stream" : "decode",
                          static_cast<double>(audioEngine.sourceSeconds(id)), audioEngine.source(id).bus.c_str(),
                          audioEngine.source(id).spatial ? "spatial" : "bed");
        }
    }
    if (audioEngine.refusedSources() > 0) {
        core::Logger::warn(core::LogCategory::Audio, "Audio: %u sources refused", audioEngine.refusedSources());
    }
}

void Engine::initRecording() {
    auto zone = core::Profiler::scope("Engine::initRecording");
    if (!configData.record.enabled) return;
    (void)startRecording();
}

bool Engine::startRecording(const std::filesystem::path& path) {
    // Already teeing. `Recorder::start` would refuse a second session anyway, but it would
    // refuse it after `startCapture` had reopened the tap.
    if (render.recording()) return true;

    if (configData.window.headless) {
        // No swapchain to read back. Letting it start instead produces an empty file with no
        // error against it; `--frames N --capture` is what answers "what did it draw" here.
        core::Logger::error(core::LogCategory::Render, "Record: there is nothing to record in a headless run");
        return false;
    }

    core::Recorder::Options options;
    options.path = path.empty() ? core::Resources(configData.record.file) : path;
    options.fps = configData.record.fps;
    options.windowSeconds = configData.record.seconds;
    options.sampleRate = audioEngine.sampleRate();
    options.channels = audioEngine.channelCount();

    core::AudioTap* tap = nullptr;
    if (audioEngine.active()) {
        // 1.0s of ring, 384 KiB: it only has to cover the gap between the audio thread
        // producing and the worker draining. Sizing it to the recording window instead is
        // hundreds of megabytes for a problem measured in milliseconds -- the encoder holds
        // the recording, not this.
        audioEngine.startCapture(1.0f);
        tap = audioEngine.captureTap();
    } else {
        core::Logger::warn(core::LogCategory::Render, "Record: audio is off, so the recording will be silent");
    }

    if (!render.startRecording(recorder, std::move(options), tap)) {
        audioEngine.stopCapture();
        return false;
    }
    return true;
}

std::filesystem::path Engine::stopRecording() {
    // Guarded on the tee rather than on `recorder.active()`: a session whose encoder died
    // cleared `active()` and still owns a worker and two pipes, and this is the path that
    // gives them back.
    if (!render.recording()) return {};

    render.stopRecording();
    core::Logger::status(core::LogCategory::Render, "Record: finishing the file");
    const std::filesystem::path written = recorder.stop();
    // After the join inside `stop()`, never before: the worker is reading the tap until it
    // returns, and taking the ring out from under it is a use-after-free.
    audioEngine.stopCapture();
    return written;
}

/**
 * @brief Turn every `FABRIC_` placement into a soft body, and tell the renderer.
 *
 * Called from inside `initPhysics`, after the colliders and before `finalize()` -- a cloth
 * built outside the world the rigid bodies are in collides with none of them.
 *
 * Walks the placements in placement order, skipping exactly what `addPlacementInstances`
 * skips, because the slot numbers have to agree. **The agreement is checked**: a slot the
 * walk arrives at without `kInstanceCloth` is reported and skipped rather than deforming
 * whatever instance the count landed on.
 */
void Engine::initCloth() {
    auto zone = core::Profiler::scope("Engine::initCloth");
    const auto& sources = sceneData.clothSources();
    if (sources.empty()) return;

    const auto& prims = sceneData.primitives();
    uint32_t slot = 0;
    uint32_t placed = 0;
    for (const scene::Placement& p : sceneData.placements()) {
        if (p.primitive >= prims.size()) continue;
        const scene::Primitive& prim = prims[p.primitive];
        if (prim.indexCount == 0) continue;
        const uint32_t thisSlot = slot++;
        if (prim.clothOffset == 0xFFFFFFFFu) continue;

        if (thisSlot >= instanceTable.slotCount() ||
            (instanceTable.slot(thisSlot).meta.z & scene::kInstanceCloth) == 0u) {
            core::Logger::warn(core::LogCategory::Scene,
                               "cloth: placement of primitive %u did not land on a cloth instance; not simulated",
                               p.primitive);
            continue;
        }

        const scene::GltfScene::ClothSource* src = nullptr;
        for (const auto& candidate : sources) {
            if (candidate.primitive == p.primitive) src = &candidate;
        }
        if (src == nullptr) continue;

        scene::ClothDesc desc;
        desc.vertices = src->vertices;
        desc.masses = src->masses;
        desc.indices = src->indices;
        // Applied once, into the vertices. The instance's own transform is identity, so a
        // cloth placed anywhere else in the hierarchy is placed twice or not at all.
        desc.transform = p.transform;
        if (clothSystem.add(physicsWorld, thisSlot, p.primitive, desc)) ++placed;
    }

    if (placed == 0) return;
    core::Logger::status(core::LogCategory::Scene, "Cloth: %u soft bodies, %u vertices", placed,
                         clothSystem.vertexCount());

    // The deformed vertex buffer is sized from what deforms, and nothing knew a curtain did
    // until now. Without the re-run it stays sized for the skinned meshes alone.
    render.setCloth(&clothSystem);
    render.setAnimator(&sceneAnimator, &sceneData);
}

void Engine::createColliderBodies(uint32_t firstCollider, uint32_t colliderCount, std::vector<DrivenBody>& out) {
    for (uint32_t i = 0; i < colliderCount; ++i) {
        const scene::ColliderDesc& desc = sceneData.colliders()[firstCollider + i];
        // `createBody` does not route a Character motion.
        const bool isCharacter = desc.motion == scene::ColliderMotion::Character;
        scene::BodyId body;
        scene::PhysicsCharacterId character;
        if (isCharacter) {
            character = physicsWorld.createCharacter(desc);
            if (!character.valid()) continue;
        } else {
            body = physicsWorld.createBody(desc);
            if (!body.valid()) continue;
        }

        // Not in `bindDrivenNodes`, which skips static bodies: a sound on a static body would
        // then occlude itself forever, and nothing about it ever moves to make that visible.
        // Characters are skipped here instead -- `CharacterVirtual` is not in the broad phase,
        // so it can never be what an occlusion ray hit.
        if (!isCharacter) {
            for (uint32_t sIdx = 0; sIdx < audioEngine.sourceCount(); ++sIdx) {
                if (audioEngine.source(audioEngine.soundAt(sIdx)).node == desc.node) sourceBody[sIdx] = body;
            }
        }
        if (desc.motion == scene::ColliderMotion::Static) continue;
        out.push_back({body, character, desc.node, desc.name});
    }
}

uint32_t Engine::bindDrivenNodes(const std::vector<DrivenBody>& added, const std::vector<DrivenSlot>& slots) {
    // A *child* node per attachment, so one body may drive several meshes and the rest
    // transform's inverse survives as the attachment's offset. Folding it into a local
    // transform instead puts every driven mesh through a translation/rotation/scale
    // decomposition -- exact in mathematics, not in floats, and a moved pixel in a suite
    // that compares bytes.
    uint32_t drivenNodes = 0;
    for (const DrivenBody& a : added) {
        const glm::mat4 rest = a.character.valid() ? physicsWorld.characterTransform(a.character, 0.0f)
                                                   : physicsWorld.bodyTransform(a.body, 0.0f);
        const glm::mat4 restInverse = glm::inverse(rest);

        const scene::NodeId bodyNode = sceneTree.create(a.name);
        if (a.character.valid()) {
            sceneTree.attachCharacter(bodyNode, a.character);
            authored.push_back({a.character, bodyNode});
        } else {
            sceneTree.attachBody(bodyNode, a.body);
        }
        ++drivenNodes;

        for (const auto& [node, id] : slots) {
            if (node != a.node) continue;
            const scene::NodeId meshNode = sceneTree.create("mesh", bodyNode);
            sceneTree.attachInstance(meshNode, id, restInverse * instanceTable.transform(id));
            // What the velocity target and the dynamic BLAS tier both select on.
            instanceTable.setFlags(id, scene::kInstanceDynamic, 0);
            pairLocomotion(a.character, id);
        }

        // A sound authored on the same node rides the body. Its offset goes into the
        // child's *local* transform rather than onto the attachment, and the difference from
        // the mesh above is the whole reason both exist: a sound's position is not compared
        // byte for byte by anything, so it can afford the decomposition a rendered transform
        // cannot.
        for (uint32_t sIdx = 0; sIdx < audioEngine.sourceCount(); ++sIdx) {
            const scene::SoundId sound = audioEngine.soundAt(sIdx);
            if (!sound.valid() || audioEngine.source(sound).node != a.node) continue;
            const scene::NodeId soundNode = sceneTree.create(audioEngine.source(sound).name, bodyNode);
            sceneTree.setLocalTransform(soundNode, restInverse * audioEngine.source(sound).transform);
            sceneTree.attachSound(soundNode, sound);
        }
    }
    return drivenNodes;
}

void Engine::initPhysics() {
    auto zone = core::Profiler::scope("Engine::initPhysics");
    // The world exists whenever physics is enabled, empty or not. Gating it on the document
    // declaring a collider silently refuses every `createBody` and `createCharacter` a game
    // makes in `init`, which runs after this.
    if (!configData.physics.enabled) return;

    scene::PhysicsConfig pcfg;
    pcfg.step = setup.sim.physicsStep;
    pcfg.gravity = setup.sim.gravity;
    pcfg.maxStepsPerFrame = configData.settings.get(core::options::physics::maxStepsPerFrame);
    pcfg.workerThreads = configData.settings.get(core::options::physics::workerThreads);
    // A soft body is a body to the system that has to hold it, so the cloths are in the
    // count the world is sized from.
    physicsWorld.init(pcfg, static_cast<uint32_t>(sceneData.colliders().size() + sceneData.clothSources().size()));
    physicsWorld.debugContacts = configData.physics.debugContacts;

    // Walks the placements exactly the way `addSceneInstances` did -- same order, same skip
    // -- because the slot numbers have to come out identical.
    //
    // Keyed by the *collider* node, not the placing node. The two are the same value for a
    // collider authored on the node carrying the mesh and differ for a rig, where the capsule
    // sits several levels above the skinned meshes: match on the placing node and a rig binds
    // nothing, with the controller walking away from its character. See
    // `Placement::colliderNode`.
    std::vector<DrivenSlot> placementSlots;
    for (size_t i = 0, slot = 0; i < sceneData.placements().size(); ++i) {
        const scene::Placement& p = sceneData.placements()[i];
        if (sceneData.primitives()[p.primitive].indexCount == 0) continue;
        // `idAt`, never a hand-built `InstanceId{slot, 0}`: the generation belongs to the
        // table, and a literal here agrees with how `create()` numbers a fresh slot only
        // until that changes.
        placementSlots.push_back({p.colliderNode, instanceTable.idAt(static_cast<uint32_t>(slot))});
        ++slot;
    }

    // Rebuilt, not appended to: this walks the whole collider table, so anything a previous
    // scene authored is naming a character that no longer exists.
    sourceBody.assign(audioEngine.sourceCount(), scene::BodyId{});
    authored.clear();

    std::vector<DrivenBody> added;
    createColliderBodies(0, static_cast<uint32_t>(sceneData.colliders().size()), added);

    // **Between the bodies and `finalize`, and only here.** `addModel`, the other caller of
    // the pair above, has no cloth to add.
    initCloth();

    physicsWorld.finalize();

    const uint32_t drivenNodes = bindDrivenNodes(added, placementSlots);

    core::Logger::status(core::LogCategory::Scene, "Physics: %u bodies, %u characters, %u driven nodes, capacity %u",
                   physicsWorld.bodyCount(), physicsWorld.characterCount(), drivenNodes,
                   physicsWorld.bodyCapacity());
    if (physicsWorld.refusedBodies() > 0) {
        core::Logger::warn(core::LogCategory::Scene, "Physics: %u colliders refused past the body budget",
                     physicsWorld.refusedBodies());
    }
}

void Engine::initLights() {
    auto zone = core::Profiler::scope("Engine::initLights");
    // Clamped to a texture the scene actually has: an index past the end of the bindless
    // array is undefined behaviour in the shader, reached by an authoring typo.
    for (const GameSetup::Decal& dc : setup.decals) {
        gfx::Decal decal;
        decal.transform = glm::translate(glm::mat4(1.0f), dc.position) *
                          glm::rotate(glm::mat4(1.0f), glm::radians(dc.rotationY), glm::vec3(0.0f, 1.0f, 0.0f)) *
                          glm::scale(glm::mat4(1.0f), dc.size);
        decal.tint = glm::vec4(dc.tint, dc.opacity);
        decal.textureIndex = sceneData.textureCount() > 0 ? std::min(dc.texture, sceneData.textureCount() - 1) : 0;
        decal.edgeFade = dc.edgeFade;
        render.decals.push_back(decal);
    }
    if (!render.decals.empty()) {
        core::Logger::status(core::LogCategory::Scene, "Decals: %zu", render.decals.size());
    }

    // **One walk over the scene's lights and the game's, in that order, and the only place
    // either becomes the sun.** Scene first, so a file that ships its own sun wins over one a
    // game authored.
    //
    // The sun is taken *out* of the list; `updateLights` puts it back at the head of the
    // buffer, so leaving it in here emits it twice.
    if (!sceneData.lights().empty() || !setup.look.lights.empty()) {
        render.lights.clear();
        bool sunTaken = false;
        uint32_t extraSuns = 0;
        const auto take = [&](const gfx::GpuLight& light) {
            const auto type = static_cast<gfx::LightType>(static_cast<uint32_t>(light.params.z));
            if (type == gfx::LightType::Directional) {
                // **A second directional light is dropped, not demoted to an ordinary one.**
                // There is one cascade set and the shader routes every directional light
                // through it, so a kept second is shaded against a shadow map built for the
                // first -- lit correctly and shadowed wrongly, which is worse than either.
                if (sunTaken) {
                    ++extraSuns;
                    return;
                }
                sunTaken = true;
                // `makeDirectionalLight` stored the toward-the-light vector; negating here
                // points the cascades away from the sun.
                render.sunDirection = glm::vec3(light.direction);
                render.sunColorValue = glm::vec3(light.color);
                render.sunIntensity = light.color.w;
                return;
            }
            render.lights.push_back(light);
        };
        for (const gfx::GpuLight& light : sceneData.lights()) take(light);
        for (const gfx::GpuLight& light : setup.look.lights) take(light);

        core::Logger::status(core::LogCategory::Scene, "Lights: %zu from the scene, %zu from the game (%s)",
                             sceneData.lights().size(), setup.look.lights.size(),
                             sunTaken ? "one taken as the sun" : "no directional light, so no sun");

        // **The environment was baked before this ran**, from whatever `sunDirection` held
        // then -- the member's own initialiser. Skip this and every scene whose sun is not
        // that default is image-lit from somewhere its sun is not. A no-op where they agree.
        render.rebakeIblIfSunMoved();
        if (extraSuns > 0) {
            core::Logger::status(core::LogCategory::Scene,
                                 "Lights: %u further directional light%s dropped -- there is one cascade set, and a "
                                 "second sun would be shadowed against the first's map",
                                 extraSuns, extraSuns == 1 ? "" : "s");
        }
    }
}

void Engine::applyCameraConfig() {
    auto zone = core::Profiler::scope("Engine::applyCameraConfig");
    // The `camera.*` rows this cannot apply -- move speed, orbit sensitivity, zoom step --
    // belong to a controller, and a game asks for them with `FlyCamera::applySettings`.
    scene::Camera& cam = camera();
    cam.fovYRadians = glm::radians(configData.settings.get(core::options::camera::fovDegrees));
    // Always framed first: `frameBounds` derives the near plane and the orthographic box from
    // the scene's size, and `--camera` below only says where to stand.
    cam.frameBounds(sceneData.boundsMin, sceneData.boundsMax);
    if (configData.camera.startSet) {
        cam.focus = configData.camera.startFocus;
        cam.yaw = glm::radians(configData.camera.startYawDegrees);
        cam.pitch = glm::radians(configData.camera.startPitchDegrees);
        cam.distance = configData.camera.startDistance;
        const glm::vec3 eye = cam.position();
        core::Logger::status(core::LogCategory::Core,
                       "Camera: focus (%.2f %.2f %.2f) yaw %.1f pitch %.1f dist %.2f, eye (%.2f %.2f %.2f)",
                       cam.focus.x, cam.focus.y, cam.focus.z,
                       configData.camera.startYawDegrees, configData.camera.startPitchDegrees, cam.distance,
                       eye.x, eye.y, eye.z);
    }
}

void Engine::setCamera(scene::Camera* c) {
    scene::Camera* next = c != nullptr ? c : &nullCamera;
    if (next == activeCamera) return;
    activeCamera->deactivate(inputMap);
    activeCamera = next;
    activeCamera->activate(inputMap);
}

void Engine::initInput() {
    auto zone = core::Profiler::scope("Engine::initInput");
    // Declare first, bind second, never the reverse: the config can only override an action
    // that already exists. Only what the engine itself consumes is declared here -- a game's
    // are declared in `Game::init`, which is why `applyBindings()` runs after that and not
    // from here.
    bindingMenu.declareActions(inputMap);
    uiClickAction = inputMap.declare("Ui.Click", "Mouse.Left");
    inputMap.setPointerModeExempt(uiClickAction, true);

    inputMap.gamepadDeadzone = configData.settings.get(core::options::input::gamepadDeadzone);
    textInput.repeatDelay = configData.settings.get(core::options::input::textRepeatDelay);
    textInput.repeatRate = configData.settings.get(core::options::input::textRepeatRate);
    bindingMenu.configPath = configPath.string();

    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, onFramebufferResize);
    glfwSetKeyCallback(window, onKey);
    glfwSetCharCallback(window, onChar);
    glfwSetMouseButtonCallback(window, onMouseButton);
    glfwSetCursorPosCallback(window, onCursorPos);
    glfwSetScrollCallback(window, onScroll);
    glfwSetWindowFocusCallback(window, onWindowFocus);
}

void Engine::applyBindings() {
    const uint32_t rebound = core::input::applyBindings(inputMap, configData.input.bindings);
    core::Logger::status(core::LogCategory::Input, "Input: %u actions, %u rebound, %zu held from %s",
                         inputMap.actionCount(), rebound, inputMap.parkedBindings().size(),
                         configPath.string().c_str());
    for (core::input::ActionId id = 0; id < inputMap.actionCount(); ++id) {
        if (!inputMap.actionLive(id)) continue;
        core::Logger::debug(core::LogCategory::Input, "  %-24s %s", inputMap.actionName(id).c_str(),
                      inputMap.bindingList(id).c_str());
    }

    // After the config, not before: a rebind can create a collision as easily as resolve
    // one, and the table as it will actually be read is the only one worth checking. Warn
    // and continue -- two actions may legitimately share a key in different modes, and the
    // game is the one that knows which.
    for (const auto& c : inputMap.conflicts()) {
        core::Logger::warn(core::LogCategory::Input, "Input: '%s' and '%s' both fire on %s",
                           inputMap.actionName(c.a).c_str(), inputMap.actionName(c.b).c_str(),
                           core::input::bindingName(c.binding).c_str());
    }

    // Checked here rather than in `Config`: the map as it will actually be read does not
    // exist until the game has declared its own actions. Both failures below cost a whole run
    // and neither has a symptom -- a script that names nothing presses nothing.
    if (const core::input::Script& script = configData.input.script; !script.empty()) {
        for (const std::string& name : script.unknownActions(inputMap)) {
            core::Logger::error(core::LogCategory::Input, "Input script names no such action '%s'; it will not fire",
                                name.c_str());
        }
        if (const uint64_t limit = configData.benchmark.exitAfterFrames;
            limit != 0 && script.lastFrame() >= limit) {
            core::Logger::error(core::LogCategory::Input,
                                "Input script runs to frame %llu but --frames is %llu; the run ends first",
                                static_cast<unsigned long long>(script.lastFrame()),
                                static_cast<unsigned long long>(limit));
        }
    }
}

// Nothing in these callbacks may decide what a key *means* -- that is the action table's,
// and a meaning decided here is one no rebind can reach.

void Engine::onFramebufferResize(GLFWwindow* window, int /*w*/, int /*h*/) {
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) e->render.requestResize();
}

void Engine::onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window));
    if (e == nullptr) return;

    // GLFW_REPEAT is dropped: a held key is already `down`, and text repeat is timed
    // by TextInput against the frame delta rather than by the window system.
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return;

    const auto code = static_cast<core::input::Key>(key);
    const bool down = action == GLFW_PRESS;
    e->inputMap.onKey(code, down);
    // Guarded internally by `active()`. Adding a condition here makes a third place that
    // decides which of the two owns the keyboard.
    e->textInput.onKey(code, down);
}

void Engine::onChar(GLFWwindow* window, unsigned int codepoint) {
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) e->textInput.onChar(codepoint);
}

void Engine::onMouseButton(GLFWwindow* window, int button, int action, int /*mods*/) {
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) {
        e->inputMap.onMouseButton(static_cast<core::input::MouseButton>(button), action == GLFW_PRESS);
    }
}

void Engine::onCursorPos(GLFWwindow* window, double x, double y) {
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) e->inputMap.onCursorPos(x, y);
}

void Engine::onScroll(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) e->inputMap.onScroll(yoffset);
}

/// Alt-tabbing away mid-chord otherwise leaves the key down forever, because the
/// release lands in whichever window took the focus.
void Engine::onWindowFocus(GLFWwindow* window, int focused) {
    if (focused == GLFW_TRUE) return;
    if (auto* e = static_cast<Engine*>(glfwGetWindowUserPointer(window)); e != nullptr) e->inputMap.loseFocus();
}

bool Engine::beginFrame() {
    if (closed || glfwWindowShouldClose(window) != GLFW_FALSE) return false;

    frameScope.emplace(core::Profiler::beginFrame());

    const auto now = std::chrono::steady_clock::now();
    frameDelta = std::chrono::duration<float>(now - lastTime).count();
    lastTime = now;

    {
        auto s = core::Profiler::scope("glfwPollEvents");
        glfwPollEvents();
    }

    // Poll the one device that has no callback, resolve every action once, then let the frame
    // read them. Asking a key directly anywhere downstream asks a different question at a
    // different moment.
    {
        auto s = core::Profiler::scope("input");
        core::input::pollGamepads(inputMap);
        // Between the devices and the resolve. After the poll, so a scripted pad wins over
        // whatever is plugged in; before `beginFrame`, so a scripted press goes through
        // `resolve` and the edge accumulator like any other -- text mode suppresses it, the
        // deadzone applies, and a press and release on one frame reads as the tap it is.
        configData.input.script.apply(inputMap, render.frameCount());
        inputMap.beginFrame();
        textInput.update(frameDelta);
        // Two things cannot own one keyboard. Running the menu while the panel is open makes
        // Tab both the panel's focus traversal and the menu's toggle; opening the menu first
        // cannot happen, because it sets text mode, which suppresses the key that opens the
        // panel.
        if (!uiOpen) bindingMenu.update(inputMap, textInput);
        render.overlayLines = bindingMenu.lines();
    }

    // Without this, last frame's list is handed to the renderer again and a frame that draws
    // no panel draws the previous one.
    uiBegun = false;
    render.uiDrawList = nullptr;

    // **Here and not in `endFrame`, because a game fills this from `frameUpdate`.** Cleared
    // at the far end it erases the game's lines before `drawFrame` sees them. Unconditional,
    // so turning a toggle off empties the list on the next frame rather than leaving the last
    // one drawn forever.
    render.debugLines.clear();

    // Before the game's `frameUpdate`: a query a game makes has to see this frame's world.
    {
        auto s = core::Profiler::scope("Engine::spatialIndex");
        if (sceneIndex.stale(instanceTable)) {
            sceneIndex.build(instanceTable);
            indexRevision = instanceTable.revision();
        } else if (instanceTable.revision() != indexRevision) {
            sceneIndex.refit(instanceTable);
            indexRevision = instanceTable.revision();
        }
    }

    // Before the game's `frameUpdate`: a game reading `camera().yaw` to resolve "forward"
    // wants this frame's yaw, not last frame's.
    camera().update(inputMap, frameDelta);

    /**
     * **After `camera().update`, not before it.** The grab is asked for inside that call, so
     * reading it first lands the capture a frame late.
     *
     * Both mode changes make GLFW report a discontinuous cursor position, which would be one
     * enormous `cursorDelta` and a snapped view. Neither reaches the camera: this frame's
     * deltas were resolved at the poll above, the grab lands on the frame the button went
     * down -- which `FlyCamera::update` skips -- and the release lands on a frame where the
     * orbit action is no longer held.
     */
    const bool wantCursorHidden = window != nullptr && core::input::mouseGrabbed() && !uiOpen &&
                                  glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    if (wantCursorHidden && !cursorCaptured) {
        // Read before the mode change: disabling the cursor is what moves it.
        glfwGetCursorPos(window, &cursorBeforeCapture[0], &cursorBeforeCapture[1]);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        cursorCaptured = true;
    } else if (!wantCursorHidden && cursorCaptured) {
        if (window != nullptr) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            glfwSetCursorPos(window, cursorBeforeCapture[0], cursorBeforeCapture[1]);
        }
        cursorCaptured = false;
    }

    // Per frame, not per step: the camera is driven by input, which arrives at the frame
    // rate.
    //
    // World up rather than the camera's own -- rolling the view would otherwise roll the
    // room.
    //
    // Runs before `Game::frameUpdate`, so a game placing listener 0 itself is overwritten
    // unless it clears `listenerFollowsCamera`. Listener 0 only; the rest are the game's.
    if (audioEngine.active() && setup.audio.listenerFollowsCamera) {
        const glm::vec3 eye = camera().position();
        const glm::vec3 toFocus = camera().focus - eye;
        const float reach = glm::length(toFocus);
        audioEngine.setListener(eye, reach > 1e-4f ? toFocus / reach : glm::vec3(0.0f, 0.0f, -1.0f),
                                glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // Per frame, and deliberately **not** scaled by `dt`: frame 60 has to be the same frame
    // 60 on every run, and a wall-clock term makes a cache-hit-rate comparison depend on how
    // fast the machine was.
    if (configData.camera.spinDegreesPerFrame != 0.0f) {
        camera().yaw += glm::radians(configData.camera.spinDegreesPerFrame);
    }

    {
        const glm::vec3 eye = camera().position();
        char text[96];
        std::snprintf(text, sizeof(text), "--camera %.2f,%.2f,%.2f,%.1f,%.1f,%.2f   eye %.2f %.2f %.2f",
                      camera().focus.x, camera().focus.y, camera().focus.z, glm::degrees(camera().yaw),
                      glm::degrees(camera().pitch), camera().distance, eye.x, eye.y, eye.z);
        render.cameraLine = text;
    }

    // One step under a locked clock, the wall-clock delta under a realtime one -- a
    // difference in `dt`, not a second code path. Locked, the accumulator lands on exactly
    // zero, `alpha` is zero, and every frame is a function of the frame index, which is what
    // the golden set depends on.
    simClock.accumulate(realtimeClock ? frameDelta : simClock.step());

    // Before anything moves: the table still holds where things were when the last frame was
    // drawn, which is what the velocity pass reprojects against. Backwards, the pass works
    // and reports no motion. See `InstanceTable::endFrame`.
    instanceTable.endFrame();

    return true;
}

bool Engine::saveGame(const std::filesystem::path& path) {
    core::SaveWriter out;
    scene::writeWorldSave(out, configData.scene.path.string(), instanceTable, simClock.timeScale(),
                          simClock.stepCount());
    if (activeGame != nullptr) activeGame->save(*this, out);

    if (!out.write(path)) return false;
    core::Logger::status(core::LogCategory::Core, "saved %s (%zu sections, %zu bytes)", path.string().c_str(),
                         out.sectionCount(), out.bytes().size());
    return true;
}

bool Engine::loadGame(const std::filesystem::path& path) {
    core::SaveReader in;
    if (!in.open(path)) {
        core::Logger::warn(core::LogCategory::Core, "load refused: %s", in.reason().c_str());
        return false;
    }

    // Read whole, ask, then apply -- collapsing the middle step scatters one scene's
    // transforms over another's before the mismatch is noticed. See scene/WorldSave.h.
    scene::WorldSave world;
    std::string reason;
    if (!scene::readWorldSave(in, world, reason) ||
        !scene::worldSaveApplies(world, configData.scene.path.string(), instanceTable, reason)) {
        core::Logger::warn(core::LogCategory::Core, "load refused: %s", reason.c_str());
        return false;
    }

    scene::applyWorldSave(world, instanceTable);
    simClock.setTimeScale(world.timeScale);

    if (activeGame != nullptr) activeGame->load(*this, in);

    core::Logger::status(core::LogCategory::Core, "loaded %s (%zu instances, %zu sections)", path.string().c_str(),
                         world.flags.size(), in.sections().size());
    return true;
}

scene::Ray Engine::cursorRay() const {
    const VkExtent2D extent = render.renderTargetExtent();
    const glm::vec2 pixel = render.renderTargetFromWindow(
        {static_cast<float>(inputMap.cursorX()), static_cast<float>(inputMap.cursorY())});
    return scene::rayThrough(*activeCamera, pixel,
                             {static_cast<float>(extent.width), static_cast<float>(extent.height)});
}

ui::Context& Engine::ui() {
    if (!uiBegun) {
        uiBegun = true;

        ui::InputState uiInput;
        // Through the presentation transform: the cursor is in window pixels while the UI may
        // have been laid out against a virtual target presented scaled and letterboxed.
        // Identity in every other case, so guarding the call only adds a way to skip it.
        uiInput.mouse = render.uiFromWindow(
            {static_cast<float>(inputMap.cursorX()), static_cast<float>(inputMap.cursorY())});
        uiInput.mouseDown = inputMap.held(uiClickAction);
        uiInput.mousePressed = inputMap.pressed(uiClickAction);
        uiInput.mouseReleased = inputMap.released(uiClickAction);
        uiInput.scroll = static_cast<float>(inputMap.scrollDelta());
        // Raw, not through actions: routing these through the binding table would put
        // Tab/Enter/Escape/arrows in the rebind list, where a player could take them away
        // from the panel that reads them.
        uiInput.tab = inputMap.keyPressed(core::input::Key::Tab);
        uiInput.shift = inputMap.keyDown(core::input::Key::LeftShift) || inputMap.keyDown(core::input::Key::RightShift);
        uiInput.enter = inputMap.keyPressed(core::input::Key::Enter);
        uiInput.escape = inputMap.keyPressed(core::input::Key::Escape);
        uiInput.up = inputMap.keyPressed(core::input::Key::Up);
        uiInput.down = inputMap.keyPressed(core::input::Key::Down);
        uiInput.dt = frameDelta;

        uiContext.begin(uiInput, static_cast<float>(render.framebufferWidth()),
                        static_cast<float>(render.framebufferHeight()), render.fontMetrics(), uiScaleValue);
    }
    return uiContext;
}

bool Engine::consumeStep() { return simClock.consume(); }

const std::vector<glm::mat4>& Engine::poseFor(uint32_t node) const { return sim.poseFor(node); }

void Engine::simulate(float stepSeconds) {
    // The order of a step lives in `scene/Simulation.cpp`, which links with no device.
    // Reproducing any of it here gives the headless loop a second copy to drift from.
    sim.step(stepSeconds);
    growParticles();
}

void Engine::growParticles() {
    /**
     * **The pair, and the only place either half may be called.** `ParticleSystem::grow`
     * resizes the pool's CPU side and `Renderer::resizeParticlePool` the GPU buffers the
     * shaders write into; the first without the second emits past the end of a device
     * allocation.
     *
     * After the step, never inside it: a resize waits for the device to go idle and
     * re-records descriptor sets.
     */
    const uint32_t want = particleSystem.wantedCapacity();
    if (want <= particleSystem.capacity()) return;
    if (!particleSystem.grow(want)) return;
    render.resizeParticlePool();
}

void Engine::endFrame() {
    if (uiBegun) {
        auto s = core::Profiler::scope("ui");
        uiContext.end();
        render.uiDrawList = &uiContext.draw();
    }
    // Takes effect on the *next* frame's resolve: what the UI wants is only known once it has
    // been laid out, and this frame's actions were resolved before that.
    inputMap.setPointerMode(uiBegun && uiContext.wantsPointer());
    // Only while a panel owns the keyboard -- when one does not, the binding menu sets its
    // own, and two writers to this flag is how a field ends up suppressing WASD forever.
    if (uiBegun) inputMap.setTextMode(uiContext.wantsKeyboard());

    {
        auto s = core::Profiler::scope("writeback");
        const float alpha = simClock.alpha();

        // The pose is the one animating *this* node, from `poseFor`, not the first
        // character's. Skinned instances are skipped: their vertices already carry the pose,
        // so applying the node transform as well moves the character twice.
        if (!sceneAnimator.empty()) {
            for (size_t i = 0, slot = 0; i < sceneData.placements().size(); ++i) {
                const scene::Placement& p = sceneData.placements()[i];
                if (sceneData.primitives()[p.primitive].indexCount == 0) continue;
                const scene::InstanceId id{static_cast<uint32_t>(slot), 0};
                ++slot;
                if (p.skin != 0xFFFFFFFFu) continue;
                const std::vector<glm::mat4>& world = poseFor(p.node);
                if (p.node < world.size()) instanceTable.setTransform(id, world[p.node]);
            }

            for (scene::ParticleEmitter& e : particleSystem.emitters()) {
                const std::vector<glm::mat4>& world = poseFor(e.node);
                if (e.node < world.size()) e.transform = world[e.node];
            }
        }

        // *After* the animation loop above: a node with both a clip and a collider is one the
        // physics owns, and the last writer wins.
        scene::SceneTargets targets;
        targets.instances = &instanceTable;
        targets.lights = &render.lights;
        targets.audio = &audioEngine;
        targets.physics = &physicsWorld;
        targets.particles = &particleSystem;
        targets.alpha = alpha;
        sceneTree.update(targets);
    }

    // The sweep above is the first thing that can move an instance a game created and
    // attached to a node, and the acceleration structure was built before it ran. See
    // `Renderer::rebuildAccelIfStale` for what that costs when nobody checks.
    render.rebuildAccelIfStale();

    // `droppedSteps` is a counter as well as the log line below because the log says *once*
    // that steps were dropped and the counter says on which frames.
    core::Profiler::counter("droppedSteps", simClock.droppedSteps());
    core::Profiler::counter("bodies", physicsWorld.bodyCount());
    core::Profiler::counter("particles", particleSystem.aliveCount());
    core::Profiler::counter("audioSources", audioEngine.sourceCount());
    core::Profiler::counter("nodes", sceneTree.liveCount());

    // On change only -- a warning at 60 Hz drowns the log it is trying to appear in.
    if (simClock.droppedSteps() != droppedStepsReported) {
        droppedStepsReported = simClock.droppedSteps();
        core::Logger::warn(core::LogCategory::Core,
                     "Simulation: %u whole steps dropped since start -- the frame is slower than %u steps of "
                     "%.4f s (raise physics.maxStepsPerFrame, or find what is stalling)",
                     droppedStepsReported, configData.settings.get(core::options::physics::maxStepsPerFrame),
                     static_cast<double>(simClock.step()));
    }

    // Appended, never assigned: the list was cleared in `beginFrame` and a game may already
    // have written into it.
    if (physicsDebugDraw) {
        auto s = core::Profiler::scope("physicsDebugDraw");
        physicsWorld.drawDebug(render.debugLines, camera().position());
    }

    // One line from each listener to each source, green when heard clear and red when fully
    // occluded. Every listener, because with two the question is which ears a source is
    // behind a wall from.
    if (audioDebugDraw && audioEngine.active() && !audioEngine.empty()) {
        auto s = core::Profiler::scope("audioDebugDraw");
        for (uint32_t slot = 0; slot < audioEngine.sourceCount(); ++slot) {
            const scene::SoundId i = audioEngine.soundAt(slot);
            if (!i.valid() || !audioEngine.source(i).spatial) continue;
            const float blocked = audioEngine.occlusion(i);
            const uint32_t color = gfx::packDebugColor({blocked, 1.0f - blocked, 0.15f, 1.0f});
            for (uint32_t ears = 0; ears < audioEngine.listenerCount(); ++ears) {
                render.debugLines.push_back({audioEngine.listenerPosition(ears), color});
                render.debugLines.push_back({audioEngine.sourcePosition(i), color});
            }
        }
    }

    // Before `drawFrame`, so the frame carrying index `captureFrame` is the one written.
    // Requested after the draw it silently captures the frame after the one named -- an
    // off-by-one a golden image then enshrines.
    if (configData.benchmark.captureFrame != 0 && render.frameCount() == configData.benchmark.captureFrame) {
        render.requestCapture(configData.benchmark.capturePath);
        if (!configData.benchmark.captureTarget.empty()) {
            render.requestTargetCapture(configData.benchmark.captureTarget, configData.benchmark.captureTargetPath,
                                        configData.benchmark.captureTargetMip, configData.benchmark.captureTargetLayer);
        }
    }

    // Deliberately not adjusted by one. RenderDoc delimits captures by present and counts
    // frames itself, so this arms "the next whole frame from here" and the number in the .rdc
    // filename is its count of presents, not `frameCount()`. An offset added to make the two
    // agree invents a precision the API does not offer.
    if (configData.benchmark.rdocCaptureFrame != 0 && render.frameCount() == configData.benchmark.rdocCaptureFrame) {
        gfx::renderDocTrigger(1);
    }

    // Before `drawFrame`. GLFW dispatches the framebuffer-size callback from
    // `glfwPollEvents`, so asking here lands `requestResize()` one poll later with a full
    // frame of acquire, submit and present recorded in between -- that interleaving is what
    // this drive exists to exercise, and asking after the draw spends it on the poll instead.
    if (configData.benchmark.resizeEveryFrames != 0 && render.frameCount() != 0 &&
        render.frameCount() != lastResizeFrame &&
        render.frameCount() % configData.benchmark.resizeEveryFrames == 0) {
        lastResizeFrame = render.frameCount();
        // Alternating, so every request is a genuine change: asking for the size the window
        // already has is a silent no-op that reads as a clean run having tested nothing.
        resizeSmall = !resizeSmall;
        glfwSetWindowSize(window, configData.settings.get(core::options::window::width) - (resizeSmall ? 160 : 0),
                          configData.settings.get(core::options::window::height) - (resizeSmall ? 90 : 0));
    }

    // The one row `bindRenderer` cannot bind, polled once a frame. `setSampleCount` returns
    // early unless the clamped count differs, so this costs a comparison. Nothing else may be
    // polled here: a value the renderer owns at runtime -- the tonemap curve -- would be
    // overwritten every frame.
    render.setSampleCount(configData.settings.get(core::options::render::msaaSamples));

    // Outside `drawFrame`, which holds the table by const pointer; this is the one thing that
    // mutates it.
    spriteTable.prepare();

    if (render.drawFrame(camera()) == gfx::FrameResult::WindowClosed) closed = true;

    if (const uint64_t limit = configData.benchmark.exitAfterFrames;
        limit != 0 && render.frameCount() >= limit) {
        closed = true;
    }

    frameScope.reset();
}

int Engine::run(Game& game) {
    {
        auto zone = core::Profiler::scope("Game::init");
        game.init(*this);
    }
    // Both after `Game::init`, and for the same reason: a game installs its camera and
    // declares its actions there, so framing or rebinding before it would land on the
    // camera it replaced and on actions that do not exist yet.
    applyCameraConfig();
    applyBindings();

    // Startup ends here: everything above is frame 0, which `scripts/baseline.py --startup`
    // reads.
    startupFrameScope.reset();

    // **The clock starts where startup ends, not where `init` returned.** `Engine::init`
    // stamps `lastTime` too, but `game.init` runs after it and builds the world, so leaving
    // that stamp alone hands the first frame a `frameDelta` covering the game's whole
    // construction -- more steps than `maxStepsPerFrame` allows, the rest discarded, and a
    // stall reported on every launch.
    lastTime = std::chrono::steady_clock::now();

    /**
     * One world unit per texel of the virtual target, with the world origin at its
     * centre. Both run modes below want exactly this, and it is what a pixel-exact 2D
     * game wants: with `orthoHeight` equal to the render height, the half-width is
     * `height * aspect / 2` -- which is half the render *width* -- so world (x, y) lands
     * on texel (x + w/2, h/2 - y), one for one, with no scale left to round.
     */
    const auto pixelPerfectCamera = [&] {
        // **The null camera first, so nothing is driving the pose this computes.** Written
        // over an installed controller instead, its `update()` runs underneath every frame
        // and moves the four pose values back.
        setCamera(nullptr);
        const VkExtent2D extent = render.renderTargetExtent();
        camera().projectionMode = scene::Camera::Projection::Orthographic;
        camera().orthoHeight = static_cast<float>(extent.height);
        camera().nearPlane = 0.1f;
        camera().orthoFar = 100.0f;
        camera().focus = glm::vec3(0.0f);
        // **Down -Z, which is yaw = pi, not 0.** At yaw 0 the camera looks down *+Z*,
        // `glm::lookAt`'s right vector comes out as -X, and the whole world is mirrored -- a
        // sprite on the left edge draws on the right, which reads as a sign error in the
        // projection and is not one.
        camera().yaw = glm::pi<float>();
        camera().pitch = 0.0f;
        // Anywhere between the near and far planes; a parallel projection does not care.
        camera().distance = 10.0f;
        return extent;
    };

    // After `Game::init`, so it loads through the same table a game does and a game that
    // already loaded the file gets the same slot back rather than a second copy.
    gfx::ImageId readbackId;
    scene::SpriteLayerId readbackLayer;
    scene::SpriteSheetId readbackSheet;
    scene::SpriteId readbackSheetSprite;
    uint32_t readbackFrameAtCapture = scene::SpriteTable::kNoFrame;
    if (!configData.benchmark.readbackImage.empty()) {
        readbackId = imageTable.load(configData.benchmark.readbackImage);

        if (configData.benchmark.readbackLitSprite) {
            // The same file, camera and corner as the case above, through the G-buffer
            // instead of after the tonemap -- only the *path* may differ, or the pair says
            // nothing. The value cannot be bit-exact here; see `gfx::compareSilhouette`.
            const VkExtent2D extent = pixelPerfectCamera();
            const glm::uvec2 size = render.imageSize(readbackId);
            if (size.x == 0 || size.y == 0) {
                core::Logger::error(core::LogCategory::Render, "Readback: no resident image for --readback-lit-sprite");
                return 1;
            }
            const scene::GltfScene::ModelId lit = createLitSprite({
                .image = readbackId,
                .size = glm::vec2(size),
                // Top-left, the same convention the unlit case uses -- the two are only
                // comparable while both place the quad's own corner.
                .pivot = {0.0f, 0.0f},
                // 9.85: just inside the near plane at z = 9.9 for this camera. A lit sprite
                // is depth-tested, so anything further back measures whether the test scene
                // happened to stand in front of it.
                .position = {-0.5f * static_cast<float>(extent.width), 0.5f * static_cast<float>(extent.height),
                             9.85f},
                .cutoff = configData.benchmark.readbackLitCutoff,
                // Not emissive: the check reads coverage, and emissive would stop it casting
                // a shadow, which is one of the things the pass has to do.
            });
            if (lit == scene::GltfScene::kNoModel) {
                core::Logger::error(core::LogCategory::Render, "Readback: --readback-lit-sprite could not be created");
                return 1;
            }
            core::Logger::status(core::LogCategory::Render,
                                 "Readback: one lit sprite at 1:1, %ux%u texels on texel (0,0) of a %ux%u target, "
                                 "cutoff %.2f",
                                 size.x, size.y, extent.width, extent.height,
                                 static_cast<double>(configData.benchmark.readbackLitCutoff));
        } else if (configData.benchmark.readbackSprite) {
            // The same file and expectation through the sprite pass instead of the overlay,
            // which is what puts the projection, the quad, the texel-to-normalised divide and
            // the blend under test.
            const VkExtent2D extent = pixelPerfectCamera();
            const glm::uvec2 size = render.imageSize(readbackId);
            if (size.x == 0 || size.y == 0) {
                core::Logger::error(core::LogCategory::Render, "Readback: no resident image for --readback-sprite");
                return 1;
            }
            readbackLayer = spriteTable.createLayer({});

            // The same file cut into quarters and played as a clip, so what lands on texel
            // (0, 0) is **one cell** selected by the clock. Derived from the file rather than
            // stated in a flag, so the only number that has to be right is the image's own.
            const bool sheetCase = configData.benchmark.readbackSheetFps > 0.0f;
            const glm::uvec2 cell = sheetCase ? size / 2u : size;
            if (sheetCase) {
                if (cell.x == 0 || cell.y == 0) {
                    core::Logger::error(core::LogCategory::Render,
                                        "Readback: a %ux%u image does not cut into a two-by-two sheet", size.x, size.y);
                    return 1;
                }
                readbackSheet = spriteTable.createSheet({.frame = cell, .columns = 2, .count = 4});
                const uint32_t clip = spriteTable.addClip(readbackSheet, {.name = "readback",
                                                                         .first = 0,
                                                                         .count = 4,
                                                                         .fps = configData.benchmark.readbackSheetFps,
                                                                         .loop = scene::LoopMode::Loop});
                if (clip == scene::SpriteTable::kNoClip) return 1;
            }

            const scene::SpriteId s = spriteTable.create(readbackLayer, {
                .image = readbackId,
                // The sheet case overwrites this on `play`, with a rectangle the animation
                // chose.
                .size = glm::vec2(cell),
                // Top-left, so the sprite's own corner is the corner being placed.
                .pivot = {0.0f, 0.0f},
                .position = {-0.5f * static_cast<float>(extent.width), 0.5f * static_cast<float>(extent.height)},
            });
            if (!spriteTable.valid(s)) return 1;

            if (sheetCase) {
                spriteTable.play(s, readbackSheet, 0);
                readbackSheetSprite = s;
                core::Logger::status(core::LogCategory::Render,
                                     "Readback: a 2x2 sheet of %ux%u cells at %.3f fps, expecting cell %u at frame %llu",
                                     cell.x, cell.y, static_cast<double>(configData.benchmark.readbackSheetFps),
                                     configData.benchmark.readbackSheetFrame,
                                     static_cast<unsigned long long>(configData.benchmark.captureFrame));
            } else {
                core::Logger::status(core::LogCategory::Render,
                                     "Readback: one sprite at 1:1, %ux%u texels on texel (0,0) of a %ux%u target",
                                     size.x, size.y, extent.width, extent.height);
            }
        } else {
            render.setReadbackImage(readbackId);
        }
    }

    gfx::ImageId stressImage;
    uint32_t stressSpawned = 0;
    uint32_t stressColumns = 1;
    VkExtent2D stressExtent{};
    scene::SpriteLayerId stressLayers[4];
    // Only populated for `--sprites-move`: filling it in the static arm too adds a per-create
    // cost to the arm every recorded number was taken in.
    std::vector<scene::SpriteId> stressMoving;
    std::vector<glm::vec2> stressHome;
    if (configData.benchmark.spriteStress > 0) {
        stressExtent = pixelPerfectCamera();
        stressImage = imageTable.load(configData.benchmark.spriteStressImage);
        const glm::uvec2 size = render.imageSize(stressImage);

        // A square-ish grid across the visible area. Sprites overlap once the count exceeds
        // what the area holds, deliberately: the pass's cost is overdraw, and a grid that
        // thinned out as the count rose would measure a different thing at each arm.
        stressColumns = std::max(
            1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(configData.benchmark.spriteStress)))));

        // Four layers rather than one, or the number is a measurement of a single-key sort.
        for (int i = 0; i < 4; ++i) stressLayers[i] = spriteTable.createLayer({.order = i});

        core::Logger::status(core::LogCategory::Render, "Sprites: %u across %u columns of %ux%u texels",
                             configData.benchmark.spriteStress, stressColumns, size.x, size.y);
    }

    /// A batch per frame, not all of them before the loop: spawning the lot up front sizes
    /// the buffers exactly once, so the doubling path in `ensureSpriteCapacity` never runs
    /// with layers on. Eight batches is three or four real reallocations, with the count at
    /// its stated value from frame 8 onward.
    const auto spawnStressBatch = [&] {
        const uint32_t want = configData.benchmark.spriteStress;
        if (stressSpawned >= want) return;
        const uint32_t batch = std::max(1u, want / 8);
        const uint32_t end = std::min(want, stressSpawned + batch);
        const auto rows = (want + stressColumns - 1) / stressColumns;
        const float stepX = static_cast<float>(stressExtent.width) / static_cast<float>(stressColumns);
        const float stepY = static_cast<float>(stressExtent.height) / static_cast<float>(std::max(rows, 1u));
        for (uint32_t i = stressSpawned; i < end; ++i) {
            const float x =
                -0.5f * static_cast<float>(stressExtent.width) + (static_cast<float>(i % stressColumns) + 0.5f) * stepX;
            const float y =
                0.5f * static_cast<float>(stressExtent.height) - (static_cast<float>(i / stressColumns) + 0.5f) * stepY;
            const scene::SpriteId id = spriteTable.create(stressLayers[i % 4], {
                .image = stressImage,
                .size = {16.0f, 16.0f},
                .position = {x, y},
            });
            if (configData.benchmark.spriteStressMove) {
                stressMoving.push_back(id);
                stressHome.emplace_back(x, y);
            }
        }
        stressSpawned = end;
    };

    /// The moving arm (`--sprites-move`): every sprite, every frame, so the renderer's
    /// per-slot revision never matches and the upload pays its whole copy. Sub-texel, because
    /// the two arms must differ in the upload and not in what is drawn -- a sprite that
    /// wandered off the visible area would measure less overdraw as well as less traffic.
    const auto moveStressBatch = [&] {
        if (stressMoving.empty()) return;
        const float phase = static_cast<float>(render.frameCount()) * 0.05f;
        for (size_t i = 0; i < stressMoving.size(); ++i) {
            const float wobble = std::sin(phase + static_cast<float>(i) * 0.017f) * 0.25f;
            spriteTable.setPosition(stressMoving[i], stressHome[i] + glm::vec2(wobble, wobble));
        }
    };

    while (beginFrame()) {
        // At the top of a frame and nowhere else.
        applyPendingScene();
        spawnStressBatch();
        moveStressBatch();
        {
            auto s = core::Profiler::scope("Game::frameUpdate");
            game.frameUpdate(*this, frameDelta);
        }
        {
            // Above the `uiOpen` test, not inside it: a row that vanishes when the work is
            // skipped reads as "this is missing", not as "this cost nothing".
            auto s = core::Profiler::scope("Game::drawUi");
            if (uiOpen) game.drawUi(*this, ui());
        }

        // Re-asserted every frame, not set once before the loop: a readback case compares
        // against an image computed for this projection, and a game's camera controller
        // writes `focus` every frame. The overlay cases are screen space and never see the
        // world camera, which is why they pass either way.
        if (configData.benchmark.readbackSprite || configData.benchmark.readbackLitSprite) pixelPerfectCamera();
        while (consumeStep()) {
            {
                auto s = core::Profiler::scope("Game::fixedUpdate");
                game.fixedUpdate(*this, simClock.step());
            }
            simulate(simClock.step());
        }

        // **Once a frame, after the steps rather than inside them.** Only the last step's
        // pose is drawn, so reading the solve per step pays the normal recompute several
        // times over for poses nothing looks at. A frame that ran no steps re-reads the same
        // pose, which is what keeps the buffer the renderer copies from valid.
        if (!clothSystem.empty()) {
            auto s = core::Profiler::scope("Cloth");
            clothSystem.update(physicsWorld);
        }
        // After this frame's steps and before `endFrame` takes the capture, so this is the
        // cell the captured image holds. The run does not stop at the capture frame, so
        // reading this at the end asks about a later cell than the one compared.
        if (readbackSheetSprite.valid() && render.frameCount() == configData.benchmark.captureFrame) {
            readbackFrameAtCapture = spriteTable.frame(readbackSheetSprite);
        }
        endFrame();
    }

    game.shutdown(*this);

    render.logGpuTimings();
    ctx.logMemoryUsage("steady state");

    // Catches `--frames` set below `--rdoc-capture-frame`: without this the run exits before
    // the trigger fires, reports success, and leaves the analysis scripts pointed at a file
    // that was never written.
    if (configData.benchmark.rdocCaptureFrame != 0) {
        const uint32_t written = gfx::renderDocCaptureCount();
        if (written == 0) {
            core::Logger::error(core::LogCategory::Render,
                          "RenderDoc: no capture written -- did the run reach frame %llu? (--frames must exceed it)",
                          static_cast<unsigned long long>(configData.benchmark.rdocCaptureFrame));
        } else {
            core::Logger::status(core::LogCategory::Render, "RenderDoc: %u capture%s written to %s_frameNNNN.rdc", written,
                           written == 1 ? "" : "s", configData.benchmark.rdocCapturePath.c_str());
        }
    }

    // Before the golden comparison and not instead of it. The expected image is *computed*
    // from the source rather than snapped from a previous run, so a failure here is never
    // answered by re-snapping.
    if (!configData.benchmark.readbackImage.empty()) {
        if (render.capturesWritten() == 0) {
            core::Logger::error(core::LogCategory::Render,
                                "Readback: no capture was written -- did the run reach frame %llu?",
                                static_cast<unsigned long long>(configData.benchmark.captureFrame));
            return 1;
        }

        // Inside the virtual target the image is magnified by the presentation scale and
        // lands at the letterbox offset; outside it the overlay drew after the blit, at 1x in
        // the corner. A sprite is world-space content and always draws into the virtual
        // target, so `uiInsideVirtual` governs the overlay alone.
        const gfx::PresentLayout& p = render.present();
        const bool inVirtual = render.uiInsideVirtual || configData.benchmark.readbackSprite ||
                               configData.benchmark.readbackLitSprite;
        const uint32_t scale = inVirtual ? p.scale : 1u;
        const int32_t x = inVirtual ? p.x : 0;
        const int32_t y = inVirtual ? p.y : 0;

        // A run with no background named *is* the background run: it wrote the capture the
        // measured run is held against, and has nothing to compare.
        if (configData.benchmark.readbackLitSprite) {
            if (configData.benchmark.readbackBackground.empty()) {
                core::Logger::status(core::LogCategory::Render, "Readback: wrote the lit-sprite background to %s",
                                     configData.benchmark.capturePath.c_str());
                return 0;
            }

            const std::filesystem::path litSource = core::Resources(configData.benchmark.readbackImage);
            const gfx::SilhouetteResult s = gfx::compareSilhouette(
                configData.benchmark.capturePath, configData.benchmark.readbackBackground, litSource, scale, x, y,
                configData.benchmark.readbackLitCutoff, configData.benchmark.diffPath);
            if (!s.loaded || s.sizeMismatch) return 1;
            if (!s.matched) {
                core::Logger::error(core::LogCategory::Render,
                                    "Silhouette: MISMATCH -- %llu pixels differ outside the mask (first at %u,%u); "
                                    "changed box [%u,%u)-[%u,%u), expected [%u,%u)-[%u,%u)",
                                    static_cast<unsigned long long>(s.outsideDiffering), s.worstX, s.worstY, s.diffX0,
                                    s.diffY0, s.diffX1, s.diffY1, s.maskX0, s.maskY0, s.maskX1, s.maskY1);
                return 1;
            }
            core::Logger::status(core::LogCategory::Render,
                                 "Silhouette: exact at %ux -- 0 pixels differ outside the mask, changed box "
                                 "[%u,%u)-[%u,%u) is the %llu covered texels of %s",
                                 scale, s.diffX0, s.diffY0, s.diffX1, s.diffY1,
                                 static_cast<unsigned long long>(s.expectedCovered),
                                 configData.benchmark.readbackImage.c_str());
            return 0;
        }

        // The rectangle is decided by the number the *caller* stated, never the one the
        // animation reached: cropping to whatever cell playback landed on compares frame
        // selection against itself and passes for any selection at all. The frame is asserted
        // first, and the crop follows from the assertion.
        gfx::ReadbackRect srcRect;
        if (readbackSheetSprite.valid()) {
            const uint32_t want = configData.benchmark.readbackSheetFrame;
            if (readbackFrameAtCapture != want) {
                core::Logger::error(core::LogCategory::Render,
                                    "Readback: the sheet was showing cell %u at frame %llu, not the cell %u asked "
                                    "for -- the clip is not being advanced by the fixed step",
                                    readbackFrameAtCapture, static_cast<unsigned long long>(configData.benchmark.captureFrame),
                                    want);
                return 1;
            }
            // The four quarters written out here rather than asked of
            // `SpriteTable::frameUv`, and **the second copy is the point**: cropping with the
            // same call the *draw* used makes a transposed slicing agree with itself -- wrong
            // cell drawn, wrong cell expected, case passes.
            const glm::uvec2 size = render.imageSize(readbackId);
            const uint32_t cw = size.x / 2;
            const uint32_t chh = size.y / 2;
            srcRect = {(want % 2) * cw, (want / 2) * chh, cw, chh};
        }

        const std::filesystem::path source = core::Resources(configData.benchmark.readbackImage);
        const gfx::CompareResult r =
            gfx::compareReadback(configData.benchmark.capturePath, source, scale, x, y,
                                 configData.benchmark.readbackExpectedPath, configData.benchmark.diffPath, srcRect);
        if (!r.goldenLoaded || r.sizeMismatch) return 1;
        if (!r.matched) {
            core::Logger::error(core::LogCategory::Render,
                                "Readback: MISMATCH at %ux -- %llu/%u texels differ, max delta %u at (%u,%u), "
                                "mean delta %.4f",
                                scale, static_cast<unsigned long long>(r.differingPixels), r.width * r.height,
                                r.maxChannelDelta, r.worstX, r.worstY, r.meanChannelDelta);
            return 1;
        }
        char cell[48] = {};
        if (readbackSheetSprite.valid()) {
            std::snprintf(cell, sizeof(cell), " cell %u of", configData.benchmark.readbackSheetFrame);
        }
        core::Logger::status(core::LogCategory::Render,
                             "Readback: bit-identical at %ux --%s %s expanded to %ux%u at (%d,%d), 0 of %u differ",
                             scale, cell, configData.benchmark.readbackImage.c_str(), r.width, r.height, x, y,
                             r.width * r.height);
    }

    // Between the render loop and shutdown, so a failure is reported with the run's timings
    // still on screen and the process still exits cleanly. The verdict travels out as the
    // exit code, which is the only part a shell loop can read.
    if (configData.benchmark.goldenPath.empty()) return 0;

    if (render.capturesWritten() == 0) {
        core::Logger::error(core::LogCategory::Render, "Compare: no capture was written -- did the run reach frame %llu?",
                      static_cast<unsigned long long>(configData.benchmark.captureFrame));
        return 1;
    }

    const gfx::CompareResult r = gfx::comparePng(
        configData.benchmark.capturePath, configData.benchmark.goldenPath, configData.benchmark.compareTolerance,
        configData.benchmark.compareMaxPixels, configData.benchmark.diffPath);
    if (!r.goldenLoaded || r.sizeMismatch) return 1;
    if (r.matched) {
        core::Logger::status(core::LogCategory::Render, "Compare: match -- %llu/%u pixels over tolerance %u, mean delta %.4f",
                       static_cast<unsigned long long>(r.differingPixels), r.width * r.height,
                       configData.benchmark.compareTolerance, r.meanChannelDelta);
        return 0;
    }
    core::Logger::error(core::LogCategory::Render,
                  "Compare: MISMATCH -- %llu/%u pixels over tolerance %u, max delta %u at (%u,%u), "
                  "mean delta %.4f",
                  static_cast<unsigned long long>(r.differingPixels), r.width * r.height,
                  configData.benchmark.compareTolerance, r.maxChannelDelta, r.worstX, r.worstY, r.meanChannelDelta);
    return 1;
}

void Engine::requestQuit() {
    if (window != nullptr) glfwSetWindowShouldClose(window, GLFW_TRUE);
}

void Engine::dumpProfile() {
    core::Profiler::dump();
    render.logGpuTimings();
}

void Engine::shutdown() {
    core::Logger::status(core::LogCategory::Core, "Shutting down");
    teardown();
}

void Engine::teardown() {
    // Idempotent: `shutdown()` and `~Engine` both reach here, and whichever arrives first
    // does the work.
    if (tornDown) return;
    tornDown = true;

    // First of everything, and through `stopRecording()` so exit and a keypress cannot end a
    // recording two different ways.
    if (ctx.device != VK_NULL_HANDLE) (void)stopRecording();
    // A session whose encoder died is past `Renderer::recording()` with its worker and pipes
    // still open, so `stop()` runs unconditionally as well. Both lines are no-ops otherwise.
    if (recorder.active()) core::Logger::status(core::LogCategory::Render, "Record: finishing the file");
    recorder.stop();
    audioEngine.stopCapture();

    // Explicit, and before the GPU teardown: a streamed voice is a file handle and a decode
    // job, neither of which the device knows about. Left to the destructor it would run after
    // the members below.
    audioEngine.shutdown();

    // Sprites first, because they name image slots: the other order leaves sprites resolving
    // handles against a table that has already given up.
    spriteTable.shutdown();
    render.setSprites(nullptr);

    // Before the renderer, so a game that skipped `destroy` still leaves the table saying
    // nothing is live by the time the images behind it are freed. Views first: a live one
    // holds an image slot, and releasing it after the image table was cleared releases a slot
    // that no longer exists.
    viewTable.shutdown();
    imageTable.shutdown();

    if (ctx.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device);
        if (sceneLoaded) sceneData.destroy(ctx);
        uploaderData.shutdown(ctx);
        render.shutdown();
        ctx.shutdown();
    }

    if (window != nullptr) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();

    // Here and not in `applyBindings()`, which is the only moment this answer is true: a row
    // nothing has declared *yet* is held for a declaration that may still come, since a
    // camera declares its rows when it is installed. Anything still parked at the end of the
    // run is a name this build never had.
    for (const auto& [name, list] : inputMap.parkedBindings()) {
        core::Logger::warn(core::LogCategory::Input, "Config binds unknown action \"%s\" (%s); ignored", name.c_str(),
                           list.c_str());
    }

    core::Profiler::dump();
    core::Profiler::shutdown();
    core::Logger::status(core::LogCategory::Core, "Substrate exiting");
    core::Logger::shutdown();
}
