#include "Engine.h"

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
 * @brief Place `count - 1` more copies of the scene's first skinned mesh (S2.3).
 *
 * @return how many extra characters were created; zero for a scene with no skin, and
 *         zero for `count <= 1`, which is the default and leaves every existing scene
 *         rendering exactly what it did.
 *
 * Engine-side rather than game-side, and by the rule that decides every other case in
 * this file: it is selected by `scene.characters` in the config, and the engine acts on
 * its own config. It also has to run before `setInstances`, because spawning characters
 * adds slots and the renderer sizes its buffers from the count it is handed.
 *
 * The demonstration this exists for is the one a glTF file cannot express: a rig is
 * shared, a *character* is not, and a file describes one of everything. Forty copies of
 * one skeleton are forty calls to `addCharacter` and forty instances pointing at them.
 *
 * Copies are spread along Z and given a clip each, cycling if there are fewer clips
 * than copies. Along Z because `Camera::frameBounds` aims down the longest horizontal
 * axis, so a row across it is a row the default camera can see end to end.
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

// =========================================================================== lifetime

// Both of these are one line and both are here rather than in the header on purpose. A
// defaulted constructor and an inline destructor are emitted into every translation unit
// that builds or destroys an `Engine`, and each carries a call to the constructor and the
// destructor of every by-value member -- so `Entry.cpp`, which mentions none of them,
// referenced `scene::PhysicsWorld`, `scene::AudioEngine`, `scene::SceneLoader`,
// `core::Recorder` and `core::settings::Settings` by name in its object file. Anything a
// game writes with an `Engine` on the stack did the same.
//
// Defining them here means `Engine.cpp.o` is the only object file that names what an
// `Engine` holds, which is where G10 wanted the boundary. What G10 also found is that the
// boundary is worth nothing in bytes: the stripped `Release` demo moved 6,054,400 ->
// 6,046,208, which is two pages of alignment rather than content. Every one of those
// object files was already linked to satisfy `Engine.cpp.o`, which is in every game and
// names them all. See docs/kanban/done/G10-a-game-links-only-the-subsystems-it-names.md.
Engine::Engine() = default;

Engine::~Engine() { teardown(); }

// =============================================================================== init

bool Engine::init(int argc, char** argv, Game& game) {
    activeGame = &game;

    // --config is read first so the file can be located before anything else is set
    // up; every other flag overrides whatever the file provided.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) configPath = argv[i + 1];
    }

    // And --dump-settings=json here rather than with the other flags, for a reason the
    // ordering makes unavoidable: its output has a *machine* consumer, warnings and status
    // lines go to stdout too, and the most useful of those warnings -- "this key moved" --
    // are logged while the config file is being read, which is below. Taking the log off
    // the terminal now is the only point at which all of them are still ahead. They still
    // reach the log file. The table form is for a human and keeps them.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-settings=json") == 0) core::Logger::setOutput(core::LogOutput::File);
    }

    // The working directory wins, and only a relative name that is not there falls back to
    // the executable's own directory. That ordering is the whole point: run.sh cds to the
    // repo root, so a development build always takes the first branch and behaves exactly
    // as it did before this existed -- including `--config` naming a file relative to
    // wherever you happened to be standing. A packaged build is the case where nothing is
    // there, because it was launched from a menu with a working directory nobody chose,
    // and its substrate.json ships beside the binary.
    //
    // Reads only. Log, trace and capture paths stay relative to the working directory,
    // because scripts/golden.sh, scripts/baseline.py and scripts/rdoc.sh all agree that
    // debug_frames/ is at the repo root, and a write that quietly moved would be a
    // regression in every one of them.
    if (std::error_code ec; configPath.is_relative() && !std::filesystem::exists(configPath, ec)) {
        if (std::filesystem::path beside = core::executableDir() / configPath; std::filesystem::exists(beside, ec)) {
            configPath = std::move(beside);
        }
    }

    // D17. The game's own rows, before the file is read, and the schema frozen the moment
    // the call returns. Both halves are load-bearing and neither is a convention anybody
    // has to remember. `loadJson` walks the *file* rather than the table, so that a key
    // nothing claims produces a message -- which means a row declared after this point has
    // already had its key reported as the user's typo and keeps its built-in. Freezing here
    // is what makes the split enforced: `declareSettings` may only add rows, `configure`
    // below may only write them, and a `declare` from `configure` is refused by name.
    //
    // It is also why `--write-default-config` and `--help`, which exit from inside
    // `applyCommandLine`, still see a game's rows: the schema is complete before either the
    // file or the flags are looked at.
    game.declareSettings(configData.settings);
    configData.settings.freezeRows();

    if (!configData.loadFromFile(configPath)) {
        exitCodeValue = 1;
        return false;
    }
    // The game names its scene and its authored values now, before anything is built --
    // the window needs the title, the world needs gravity, the device needs the mix graph,
    // and all three happen below (S1). It may also seed a setting, and **the slot this
    // call occupies is what makes the precedence real**: the file has been read and the
    // flags have not, so `Default < Config < Game < Cli` is the order the doors are
    // opened in rather than a rule asserted inside the setter (D15).
    //
    // It used to run after `applyCommandLine`, which made a game's `set` beat the command
    // line -- the exact inversion of what every document about it claimed, and invisible
    // because no game in the tree wrote a setting here. The cost of the move is that a
    // game reading a setting from `configure` sees the user's file and not the flags,
    // which is the same trade in the other direction and the one principles.md asks for:
    // a setting is a property of the person running the program.
    game.configure(setup, configData.settings);

    if (!configData.applyCommandLine(argc, argv, exitCodeValue)) return false;

    // Settled after the flags rather than before them: these are the fields where a bare
    // path or a `--capture` is a per-invocation override, so the game supplies what the
    // command line did not.
    if (configData.benchmark.capturePath.empty()) configData.benchmark.capturePath = setup.tools.capturePath;
    if (configData.benchmark.rdocCapturePath.empty()) configData.benchmark.rdocCapturePath = setup.tools.rdocCapturePath;
    // The scale a game states is a fact about the scene it names, so it travels with that
    // scene and not with whatever `--scene` replaced it: a demo that authored its Sponza
    // at 2x has said nothing about somebody else's physics.gltf.
    // Nothing to settle here any more (C41): a game names no scene, so `configData.scene.path`
    // is exactly what the command line or the config file said, and empty means the game is
    // composing its own world in `Game::init`.
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

    // S3, and it is serviced *here* rather than inside `applyCommandLine`: a dump taken
    // before the game has configured anything would report the wrong provenance for the
    // very thing being debugged, which is the one column that earns the whole feature.
    if (configData.dumpSettings != core::Config::Dump::None) {
        if (configData.dumpSettings == core::Config::Dump::Json) {
            configData.settings.dumpJson(stdout);
        } else {
            configData.settings.dumpTable(stdout);
        }
        exitCodeValue = 0;
        return false;
    }

    // Six lines of transcription, deleted. `Config::profiler` *is* a `ProfilerConfig`
    // since D14, so there is nothing to copy field by field and nothing for a settings
    // default to disagree with the struct's own about -- which is exactly what the
    // `outputFile` comment in Profiler.h used to have to document.
    core::Profiler::init(configData.profiler);

    // **The startup frame opens here, not inside `loadScene`.** It used to, and the effect
    // was that `initWindow` and `initRenderer` ran before the first `beginFrame` while
    // `initAudio`, `initPhysics`, `initNavigation` and `initCloth` ran after it closed --
    // so a scope anywhere but the load recorded against an empty thread stack, landing at
    // depth 0 as a *sibling* of `Frame` with no `Frame/` prefix for anything to key on.
    // A zone like that is in the trace and attributable to nothing.
    //
    // Held on the engine rather than on the stack, because startup does not end when
    // `init` returns: `run` calls `Game::init` next, and that is where a game builds its
    // world. Closed there. Every `return false` below leaves it open and the `Engine`'s
    // own destruction closes it, which is the right answer for a run that never started.
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

    // **Before anything a game can reach.** `scene().add<scene::Model>` is where a game composes
    // its world, and it forwards here: importing needs the device, the uploader and the geometry
    // buffers, none of which belong in a node tree. A function pointer rather than an
    // interface -- see `Scene::setImporter`.
    sceneTree.setImporter(
        [](void* context, scene::NodeId node, const std::filesystem::path& path) -> uint32_t {
            return static_cast<Engine*>(context)->importModel(node, path);
        },
        this);

    loadScene();
    initAudio();
    initPhysics();
    initNavigation();
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

    // Everything an `initOnly` row sized has now been sized -- the light storage buffer,
    // the particle pool, the body budget, the voice pool, the trace's frame capacity. From
    // here a write to one of those rows is refused *with a reason*, which is what replaces
    // the silent clamp the light budget used to get: the buffer is sized from the budget,
    // so raising it afterwards would write past a mapped range, and clamping quietly gave
    // nobody a way to find that out.
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

    // Unmapped, not absent. The window and its surface still exist, so a headless run
    // presents through exactly the path a visible one does -- which is the whole reason
    // a golden captured this way is comparable to one captured on screen.
    //
    // GLFW's null platform (GLFW_PLATFORM_NULL) is the other way to do this and is the
    // wrong one here: it routes glfwCreateWindowSurface through vkCreateHeadlessSurfaceEXT,
    // and this driver does not implement VK_EXT_headless_surface. The extension is still
    // *present* in the instance list because Mesa's lavapipe provides it, so the surface
    // would be created, the NVIDIA card would report no present-capable queue family, and
    // VulkanContext would quietly select llvmpipe. The suite would then pass on
    // software-rendered pixels, which is worse than failing.
    if (configData.window.headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // **A run with a frame budget never steals the keyboard.** `--frames N` is what every
    // harness here passes and what nothing interactive passes, so it is the signal that
    // somebody is working in another window while this one measures something. Both hints
    // are needed: GLFW_FOCUSED covers the window mapped at creation, GLFW_FOCUS_ON_SHOW the
    // one shown later. Neither binds the window manager -- an X11 WM configured to focus
    // whatever it maps will still do so -- which is why the harnesses pass `--headless` as
    // well, and why this is the second line of defence rather than the first.
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

    // -------------------------------------------------------- DPI scale (S6.5)
    // Read once, here, before anything sizes itself: the font is baked at a pixel height
    // and the UI lays out in scaled units, so both have to agree from the first frame. A
    // configured value overrides the window's, which is what makes the scaled path
    // testable on an ordinary monitor -- `--set ui.scale=2` renders exactly what a HiDPI
    // display would.
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
        // Instance, physical device selection, logical device and VMA. The single largest
        // thing startup does in either configuration, and it is the driver's time rather
        // than ours -- which is exactly why it needs a name: without one it reads as a
        // renderer that is slow to build.
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

    // Before init() for the same reason, and it is the sharper case (P2): every render
    // target is sized by the virtual extent and the presentation layout is computed
    // alongside them, so setting this afterwards would build a frame at the window's
    // resolution and then quietly agree with itself about it. Found exactly that way --
    // the flag parsed, the field was assigned, and no target was ever a different size.
    //
    // Authored like the sun, overridable per invocation like the scene path:
    // `--virtual-resolution` is how one binary drives four presentation cases for
    // `scripts/readback.sh`, and it is a developer control rather than a preference.
    render.virtualExtent = {setup.present.virtualResolution.x, setup.present.virtualResolution.y};
    if (configData.render.virtualWidth != 0 && configData.render.virtualHeight != 0) {
        render.virtualExtent = {configData.render.virtualWidth, configData.render.virtualHeight};
    }
    render.uiInsideVirtual = setup.present.uiInsideVirtual && !configData.render.uiOutsideVirtualNamed;
    // `--readback` is pixel exactness by definition: a run that asked whether a texel
    // survives presentation and left a temporal filter on has asked nothing.
    render.pixelExact = setup.present.pixelExact || !configData.benchmark.readbackImage.empty();
    // The font is baked at the scaled height, so a TTF at 200% is rasterised at twice the
    // size rather than magnified. The embedded bitmap font ignores the height and stays
    // 16 px, which is the honest limitation of a bitmap: S6.5 scales the *layout* around
    // it either way, so a panel at 200% is twice as big with the same crisp glyphs in it.
    // Named separately from `Engine::initRenderer` so the device's time and the renderer's
    // are two numbers rather than one; they answer to different people.
    {
        // Named separately from `Engine::initRenderer` so the device's time and the
        // renderer's are two numbers rather than one; they answer to different people.
        // Braced, or it would swallow the image table below it as well -- the mistake the
        // simulation card found `audioSources` making.
        auto renderZone = core::Profiler::scope("Renderer::init");
        render.init(ctx, uploaderData, window, configData.settings.get(core::options::window::vsync),
                    configData.settings.get(core::options::render::msaaSamples),
                    core::Resources(configData.settings.get(core::options::render::debugFont)).string(),
                    configData.settings.get(core::options::render::debugFontHeight) * uiScaleValue);
    }

    // P1, and it has to be after `render.init`: the ceiling is a device limit, so there is
    // nothing to size the table from until a device exists. The renderer then holds the
    // images behind the slots, and the UI resolves handles through the same table.
    imageTable.init(render.maxImageSlots());
    render.setImages(&imageTable);
    uiContext.setImages(&imageTable);

    // C34. `kMaxViews` counts the presenting view as one of them, and a game does not
    // create the view it is already looking through -- so the table holds one fewer, and
    // that subtraction lives here rather than being a second constant to keep in step.
    viewTable.init(gfx::kMaxViews - 1);
    render.setViews(&viewTable);

    // P4, and it depends on nothing but the table above: sprites take their textures from
    // it, and a game with no scene at all can still draw them. Nothing is allocated here
    // -- the renderer sizes its buffers the first time a game creates a sprite.
    spriteTable.init(&imageTable);
    render.setSprites(&spriteTable);

    // The 34 assignments, deleted. `bindRenderer` points each row at the renderer's own
    // field instead of copying a value into it, so from here there is one storage location
    // per setting -- which is what removes the class of bug where the config said one thing
    // and the renderer held another. See engine/gfx/SettingsBind.cpp.
    core::settings::bindRenderer(configData.settings, render);

    // The three developer controls with no row to bind, applied by hand. Three is not a
    // pattern worth a mechanism; a fourth would be.
    render.setDebugView(configData.render.debugView);
    render.debugOverlay = configData.render.debugOverlay;
    render.shaderHotReload = configData.shaderHotReloadEnabled(kDebugBuild);

    // Authored, not configured: the sun, the ambient, the exposure and -- since D14 -- the
    // curve that balances them and the two shadow biases tuned against the geometry (S1).
    // `--tonemap` is the one-run override.
    render.exposure = setup.look.exposure;
    render.tonemapOperator = configData.render.tonemapNamed ? configData.render.tonemap : setup.look.tonemap;
    render.shadowDepthBias = setup.look.shadowDepthBias;
    render.shadowNormalBias = setup.look.shadowNormalBias;
    // The sun is not written here any more (D20): it is a light in `setup.look.lights`, and
    // `initLights` pulls whichever directional light comes first -- the game's or the
    // scene's -- into the three renderer fields the cascades and the sky read. Those fields
    // are derived now rather than authored, which is the whole of the change.
    render.ambientColor = setup.look.ambientColor;

    // The rest of P2's presentation switch. The three renderer fields it needs were set
    // before `render.init` because the targets are sized by them; these two settings are
    // here because they have to follow `bindRenderer` -- `render.taa` is a live-bound row,
    // and writing it before the binding exists would write it into nothing.
    if (render.pixelExact) {
        // Through the settings table rather than by writing the renderer's fields, so
        // `--dump-settings` names `game` in the source column and a developer chasing a
        // soft image is told why rather than left to find it. Over the user's file, which
        // `Game::configure`'s own documentation names as a thing a fixed-resolution game
        // legitimately does -- but **not** over the command line, which is what makes "was
        // it TAA?" a question one run answers.
        //
        // The guard is here rather than in the setter because this is the one write in the
        // engine that a game owns and that cannot be made earlier than the flags: both
        // rows have to follow `bindRenderer`, so the ordering that carries precedence
        // everywhere else (D15) cannot reach them and the comparison is written out.
        const auto claimedByFlag = [&](core::settings::Id id) {
            return configData.settings.source(id) == core::settings::Source::Cli;
        };
        if (!claimedByFlag(core::settings::Id::render_taa)) {
            (void)configData.settings.setValue(core::settings::Id::render_taa, false, core::settings::Source::Game,
                                               "pixelExact");
        }
        // The tonemap half is no longer a row, so it is no longer a settings write: D14
        // made the curve `GameSetup::look.tonemap` with `--tonemap` as the one-run override,
        // and `tonemapNamed` is the same question `claimedByFlag` asks above. What has not
        // changed is the answer -- a flag still wins, which is what makes *"is the tonemap
        // what is flattening this?"* a question one run settles.
        if (!configData.render.tonemapNamed) render.tonemapOperator = gfx::TonemapOperator::Clamp;
    }
}

void Engine::loadScene() {
    auto zone = core::Profiler::scope("Engine::loadScene");
    {
        // The usual case since C41: nothing is named unless `--scene` named it, and the game
        // composes its world in `init`. An *unloaded* scene is not the same shape as one
        // holding a file with no meshes in it -- it owns no descriptor set layout, and the
        // renderer puts that layout into every pipeline layout it builds, so bring-up died on
        // a null handle. `createEmpty` is no document, but every GPU resource a scene has.
        if (configData.scene.path.empty()) {
            core::Logger::status(core::LogCategory::GLTF, "no scene named; starting with nothing loaded");
            if (!sceneData.createEmpty(ctx, uploaderData)) {
                core::Logger::critical(core::LogCategory::GLTF, "Failed to create the empty scene");
            }
        } else {
            // Resolved once, here. Everything the file goes on to name -- its buffers, its
            // images, the .ktx2 beside each one, a sound named in `extras` -- is relative
            // to the document and resolves against this, so one lookup anchors the scene.
            const core::Resources scene(configData.scene.path.string());
            if (!sceneData.load(ctx, uploaderData, scene, loadedSceneScale)) {
                core::Logger::critical(core::LogCategory::GLTF, "Failed to load %s", scene.string().c_str());
            }
        }
    }
    sceneLoaded = true;

    // Instances are created here rather than inside load(), which is what makes the
    // renderer's draw list something the application owns (4.1). A game that wants
    // forty copies of one mesh, or none, calls create() forty times or not at all --
    // it does not load a second glTF file to say so.
    scene::addSceneInstances(sceneData, instanceTable);

    // Animation (4.4, S2). The animator takes ownership of the rig the loader parsed; a
    // scene with no skin and no clip leaves it empty and the renderer skips the pass.
    // The spawn runs *before* setInstances, because spawning characters adds slots and
    // the renderer sizes its buffers from the count it is handed.
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

    // Particles (S3). The emitters come out of the scene's own node extras, so a file
    // that declares none produces a system with a zero capacity and the renderer skips
    // every particle pass. Created here rather than inside load() for the reason
    // instances are: what a scene *has* and what the application *runs* are two
    // decisions, and a game that wants an explosion the file never mentioned should not
    // have to edit an asset to get one.
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

void Engine::bakeNavMesh(scene::NavMesh& out, const scene::NavBuildParams& params) const {
    // Static mesh colliders, and nothing else. Three exclusions and each is deliberate:
    //
    // - **Render geometry** would put an agent on a windowsill, a curtain and the tops of
    //   Sponza's pillars. The collision surface is the one a body can actually rest on,
    //   which is the same question navigation is asking.
    // - **Anything that moves** would bake a crate's floor into a mesh that is wrong the
    //   moment the crate is pushed. A dynamic body is what `SpatialIndex` is for.
    // - **Hulls, boxes and capsules** carry no triangles. A box floor is a real authoring
    //   choice and it is not covered; the row that follows this one is a voxel bake, which
    //   would take them all.
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    for (const scene::ColliderDesc& desc : sceneData.colliders()) {
        if (desc.motion != scene::ColliderMotion::Static) continue;
        if (desc.resolvedShape() != scene::ColliderShape::Mesh) continue;
        if (desc.points.empty() || desc.indices.empty()) continue;

        const auto base = static_cast<uint32_t>(positions.size());
        positions.reserve(positions.size() + desc.points.size());
        for (const glm::vec3& p : desc.points) positions.push_back(glm::vec3(desc.transform * glm::vec4(p, 1.0f)));
        indices.reserve(indices.size() + desc.indices.size());
        for (const uint32_t i : desc.indices) indices.push_back(base + i);
    }
    out.bake(positions, indices, params);
}

scene::GltfScene::ModelId Engine::addModel(const std::filesystem::path& path, const glm::mat4& transform) {
    // `appendModel` grows the scene buffers and `setScene` below destroys and rebuilds every
    // pipeline, both of which the command buffers still in flight refer to. This runs inside
    // a frame -- a game calls it from `frameUpdate`, ahead of the fence wait in `drawFrame`
    // -- so there is no point at which those are known to have retired without asking.
    //
    // A full drain rather than a retirement list, for the same reason `ensureInstanceCapacity`
    // gives: this is an explicit load event, not a per-frame one, and at the rate anything
    // drives it today the stall is invisible. The caller that would change that is a residency
    // system streaming world cells -- `Uploader::endBatchAsync` exists for it and nothing calls
    // that yet. When something retires models several times a second, this is the line that
    // says a retirement list has finally earned itself.
    vkDeviceWaitIdle(ctx.device);

    const scene::GltfScene::ModelId id = sceneData.appendModel(ctx, uploaderData, path, transform);
    if (id == scene::GltfScene::kNoModel) return id;

    const scene::GltfScene::LoadedModel& m = sceneData.model(id);
    if (modelInstances.size() <= id) modelInstances.resize(id + 1);
    // ------------------------------------------------------------------- the rig (C22)
    //
    // **Before the instances, and this ordering is the whole of the wiring.**
    // `addPlacementInstances` reads `Placement::skin` and writes it into the instance as
    // both the skin and the character, so the placements have to be on the merged rig's
    // numbering before a single instance exists. Merging afterwards would leave every
    // appended character deformed by the base scene's skeleton -- which is the silent
    // corruption `appendModel` refused deforming imports to avoid.
    // Where the animator's numbering stands *before* the merge. The remap below is the only
    // thing that can tell it from the skin numbering, and after the merge both have moved.
    const uint32_t characterBase = sceneAnimator.characterCount();
    uint32_t skinBase = scene::GltfScene::kNoRig;
    {
        scene::AnimationRig imported = sceneData.takeAppendedRig(id);
        skinBase = imported.bind.nodes.empty() ? scene::GltfScene::kNoRig : sceneAnimator.merge(imported);
        sceneData.rebaseAppendedSkins(id, skinBase);
    }

    scene::addPlacementInstances(sceneData, instanceTable, m.firstPlacement, m.placementCount, &modelInstances[id]);

    /**
     * **A skin index is not a character index, and `addPlacementInstances` can only write the
     * first one.** It writes `Placement::skin` into `GpuInstance::meta.w`, which the skinning
     * dispatch reads as an animator *character* -- the slot whose joint block deforms this
     * mesh. The two agree for a scene `SceneAnimator::init` built, because that numbers one
     * character per skin from zero. They agree for nothing else:
     *
     * - a scene with no skin at all still gets `init`'s lone character, so the first imported
     *   rig's skin 0 lands at character 1;
     * - `GameSetup::characters` makes extra copies of one skin, so every later skin is short
     *   by that many.
     *
     * Off by one, an imported rig is deformed by the joint block of whatever came before it
     * and the first one is deformed by a character with no skeleton -- two rigs playing one
     * pose, and one standing in its bind pose. `merge` creates exactly one character per
     * appended skin, in order, so the correction is the two bases it sat between.
     *
     * The free function cannot do this itself: it takes no animator, and the whole reason it
     * takes none is that `scene/` builds instances without one.
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

    // C21. What the file has besides geometry, now that `appendModel` carries it. Each of
    // these subsystems already owns a list it appends to, so the import is a range copied
    // onto the end of one -- the ordering `initPhysics` depends on is untouched because
    // none of these is physics.
    //
    // Lights go to the renderer rather than staying in the scene, because
    // `Renderer::lights` is the live list and a game pushes to it too.
    for (uint32_t i = 0; i < m.lightCount; ++i) render.lights.push_back(sceneData.lights()[m.firstLight + i]);
    for (uint32_t i = 0; i < m.audioCount; ++i) (void)audioEngine.create(sceneData.audioSources()[m.firstAudio + i]);
    // `create` rather than rebuilding the list through `setEmitters`: the pool was sized
    // from the emitters the scene loaded with and the renderer allocated its buffers
    // against that capacity, so an import shares the particles the existing ones did not
    // claim. Over-budget is refused and counted, which is the policy an over-budget spawn
    // already gets.
    for (uint32_t i = 0; i < m.emitterCount; ++i) {
        (void)particleSystem.create(sceneData.emitters()[m.firstEmitter + i]);
    }

    // Colliders, through the same pair `initPhysics` uses. What differs is only what this
    // caller owns: the sounds and characters a previous scene bound are *kept*, and the
    // slot list comes from `modelInstances[id]` -- already in placement order -- rather than
    // being rebuilt by re-walking the whole table.
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

    // Both, and `setInstances` is the one that is easy to forget: it is what resizes the
    // indirect command buffer to the new slot count. Without it the buffer stays sized for
    // the table as it was, and the last view's command list runs off the end -- which
    // validation reports as a draw overrunning `instanceData` rather than as a missing
    // call, a long way from here.
    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    // C22. Only where the import actually deformed: `setAnimator` tears down and rebuilds
    // the deformed vertex buffer, the delta buffer and the structures over them, and
    // `createMesh` already refuses to pay that for every prop a game makes.
    // **The navmesh is baked from static mesh colliders, and an import brings some** (C41).
    // `initNavigation` ran at load and from `applyPendingScene` and never from here, so
    // geometry imported at runtime was solid to the solver and absent from every path query
    // -- silently, which is the worst way for a navmesh to be wrong. A game that composes its
    // arena out of imports has no load-time bake at all, so without this it has no navmesh.
    //
    // Gated on the import actually bringing colliders: a rig or a prop has none, and a bake
    // is a real cost to pay for a file that cannot have changed the answer.
    if (m.colliderCount > 0) initNavigation();

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

    // The node's world transform is the placement. `worldTransform` is valid as of the last
    // sweep, which for a node created in `Game::init` is its local transform -- so a game
    // that positions the node before importing gets what it positioned.
    //
    // A *scale* does not belong in that transform and `setWorldScale` is where it goes:
    // `scaleSceneData` holds rigs and dynamic colliders at their authored size and carries
    // light ranges, intensities and audio falloff with the factor. A placement matrix does
    // none of that, so a node scaled by two imports a stretched character and a light whose
    // range stayed where it was.
    const scene::GltfScene::ModelId id = addModel(path, sceneTree.worldTransform(node));
    if (id == scene::GltfScene::kNoModel) return id;

    /**
     * **The attachment offset is what the file's own node hierarchy said, and nothing else.**
     * The sweep writes `node.worldTransform * offset` back over the instance every frame, so
     * an identity offset erases the document's hierarchy: `arena.glb`'s floor sits at its
     * origin and survived that, while its `columns` node carries a -36.9 z translation and
     * landed half outside the arena. Same instance count, same triangle count, wrong place --
     * which is why every count this row checks reads correct.
     *
     * `placementLocals` is that value, kept by `appendModel` before it baked the placement in.
     * It used to be recovered here as `inverse(placement) * instanceTable.transform(instance)`,
     * which is arithmetically the same and is a round trip through a matrix inverse for
     * something the loader had in its hand -- and it only worked while nothing else wrote an
     * instance transform first, a constraint nothing enforced.
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

    // ------------------------------------------------------------------- morph (G11)
    //
    // A code-made morphed mesh gets a character of its own: no skeleton, and a weight block
    // sized from the mesh rather than from the rig -- see `SceneAnimator::createMorphed` for
    // why the rig cannot supply it. `addPlacementInstances` defaults a morphed placement to
    // character 0, which is the right default for a glTF whose rig owns every weight and
    // the wrong one here, so the instances are repointed before the renderer is told
    // anything: `setAnimator` sizes the weight region from `totalWeights()` and lays out one
    // output range per deformed *instance*, and both are read off the state as it stands
    // when it is called.
    if (targets > 0) {
        if (modelCharacters.size() <= id) modelCharacters.resize(id + 1);
        modelCharacters[id] = sceneAnimator.createMorphed(targets);
        for (const scene::InstanceId instance : modelInstances[id]) {
            instanceTable.setCharacter(instance, modelCharacters[id].index);
        }
    }

    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    // Only for a mesh that deforms. This tears down and rebuilds the deformed vertex
    // buffer, the delta buffer and the acceleration structures over them, which is a price
    // every `createMesh` should not pay -- and calling it for a scene with nothing to
    // deform would set `animator` back to null on every prop a game makes.
    if (targets > 0) render.setAnimator(&sceneAnimator, &sceneData);
    sceneIndex.build(instanceTable);
    ++indexRevision;
    return id;
}

void Engine::pairLocomotion(scene::PhysicsCharacterId character, scene::InstanceId instance) {
    if (!character.valid() || !instance.valid()) return;

    // **The instance is what knows the pairing, and it is the only thing that does.** A
    // `CharacterVirtual` is a capsule and has no rig; an animator character is a pose and
    // has no collider. What joins them is a skinned mesh: the scene bound this instance to
    // this collider's node, and `characterOf` says which pose deforms it. Deriving it here
    // is what lets a game add a second rig and write nothing.
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

    // **This is the half a game could not have written**, and the reason the verb is here.
    // The buffers are sized from `slotCount()` -- without that, a slot past the current
    // capacity is a memcpy past the end of a mapped staging range -- and the acceleration
    // structure is marked, without which the new instance is in every raster pass and in
    // no ray. `staticTierStale` does not cover it: it walks the slots the structure baked,
    // so a slot that *appeared* is invisible to it.
    //
    // `instancesGrew` rather than `setInstances`, and the difference is the whole reason
    // that method exists: `setInstances` rebuilds the acceleration structure on the spot,
    // and this verb is called in a loop. Sixteen rebuilds instead of five took the demo's
    // `Game::init` from 64 ms to 317. The rebuild happens once, in `endFrame`.
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
        // model the old scene had, and a game holding the id would otherwise free ranges and
        // texture slots belonging to the new one. Said rather than swallowed, because the
        // caller is the only one who can stop asking.
        core::Logger::warn(core::LogCategory::Scene, "removeModel: %u is not a model of the current scene", id);
        return;
    }
    // Destroying the model's images and rebuilding the TLAS reaches resources the frames
    // still in flight name. See the note in `addModel`.
    vkDeviceWaitIdle(ctx.device);

    for (const scene::InstanceId instance : modelInstances[id]) instanceTable.destroy(instance);
    modelInstances[id].clear();
    // Retired, not repacked. The block stays where it is and goes inert -- C1's argument,
    // and it applies to a weight block exactly as it does to a joint block, because
    // `meta.w` names the slot either way. The mesh's morph deltas stay in the scene's
    // array for the same reason; see `GltfScene::createMesh`.
    if (id < modelCharacters.size() && modelCharacters[id].valid()) {
        sceneAnimator.destroy(modelCharacters[id]);
        modelCharacters[id] = {};
    }
    sceneData.unloadModel(ctx, id);
    render.setInstances(&instanceTable);
    sceneIndex.build(instanceTable);
    ++indexRevision;
}

// ------------------------------------------------------------------ lit sprites (P6)

uint32_t Engine::litSpriteShader() {
    if (litSpriteVariant != kNoLitSpriteShader) return litSpriteVariant;

    gfx::ShaderVariant v;
    v.name = "sprite_lit";
    v.fragmentShader = "sprite_lit.frag";
    // The shadow pass gets its own too, or every lit sprite casts a solid rectangle -- the
    // failure `shadow.frag`'s header already records for Sponza's foliage. The vertex stage
    // stays the engine's in both, because a sprite displaces nothing.
    v.shadowFragment = "sprite_lit_shadow.frag";
    // A sprite is a single sheet with a texture on both faces, which is exactly the case
    // `ShaderVariant::cullMode`'s own comment names. It is also what lets `flipX` be a UV
    // swap rather than a negative scale that would invert the winding.
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
    // Every slot in the *scene's* array is empty: a lit sprite's image is in the game's,
    // named by `gameImage` below, and -1 is what says "no texture, use the factor alone" to
    // anything that reads these.
    m.baseColorTexture = -1;
    m.metallicRoughnessTexture = -1;
    m.normalTexture = -1;
    m.occlusionTexture = -1;
    m.emissiveTexture = -1;
    m.alphaMask = 1u; ///< a cutout, which is what puts the shadow fragment in the pass
    m.gameImage = imageTable.slot(desc.image);
    m.shader = variant;
    // The texel rect, which is the one thing about a lit sprite that changes at runtime.
    m.params = desc.uv;

    const uint32_t material = sceneData.createMaterial(m);
    if (material == UINT32_MAX) return scene::GltfScene::kNoModel;

    // Rotation about the quad's own +Z, then the world position. Composed here rather than
    // by the caller because a pivot applied on the wrong side of a rotation is the single
    // most common way a sprite ends up orbiting a point it was meant to turn about -- and
    // the pivot is already inside `quadMesh`, so this side must only place and turn.
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
    // The material is on the instance rather than tracked beside it: `GpuInstance::meta.y`
    // is already the per-instance material index, so a second map keyed by model id would
    // be a copy of something the table maintains.
    const uint32_t material = instanceTable.slot(instances.front().index).meta.y;
    scene::GpuMaterial m = sceneData.material(material);
    m.params = uv;
    sceneData.setMaterial(material, m);
}

bool Engine::beginLoadScene(const std::filesystem::path& path) {
    if (sceneLoader.busy()) return false;
    pendingScenePath = path;
    // The lambda is the only thing that knows about `GltfScene`, and it is why `SceneLoader`
    // takes work rather than naming it: everything the worker runs here touches no device.
    const bool started = sceneLoader.begin(path.string(), [path](scene::SceneData& data, scene::EmbeddedImages& embedded) {
        return scene::loadSceneCpu(path, data, embedded);
    });
    if (started) core::Logger::status(core::LogCategory::Scene, "streaming %s", path.string().c_str());
    return started;
}

void Engine::applyPendingScene() {
    // In `Engine` rather than `Renderer` and on the same list as the rest of them: a full
    // device idle and a scene upload on the frame thread, at whatever moment an async load
    // happens to complete. Above the ready test, which is every frame but one.
    auto s = core::Profiler::scope("applyPendingScene");
    if (!sceneLoader.ready()) return;

    scene::SceneData data;
    scene::EmbeddedImages embedded;
    if (!sceneLoader.take(data, embedded)) return; // Already logged; the old scene stands.

    // Everything past here needs the device, and all of it happens on this thread between
    // frames. The wait is the honest part of streaming: the *parse* came off the frame
    // thread, the upload cannot, and pretending otherwise would mean recording a command
    // buffer against a scene that is being replaced.
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
    // And the characters those models were driven by. **This path does not re-init the
    // animator** -- a streamed scene keeps the rig the first one brought, which is a
    // limitation of C10 rather than of this row -- so retiring them here is what stops a
    // weight block belonging to geometry that no longer exists from being uploaded every
    // frame for the rest of the session.
    for (const scene::AnimatorId character : modelCharacters) sceneAnimator.destroy(character);
    modelCharacters.clear();
    scene::addSceneInstances(sceneData, instanceTable);
    render.setScene(&sceneData);
    render.setInstances(&instanceTable);
    sceneIndex.build(instanceTable);
    ++indexRevision;
    initNavigation();
    core::Logger::status(core::LogCategory::Scene, "streamed %s", pendingScenePath.string().c_str());
}

void Engine::initNavigation() {
    auto zone = core::Profiler::scope("Engine::initNavigation");
    scene::NavBuildParams params;
    bakeNavMesh(navigation, params);
    if (navigation.empty()) {
        core::Logger::debug(core::LogCategory::Scene, "Nav: no static mesh colliders, so no navmesh");
        return;
    }
    core::Logger::status(core::LogCategory::Scene, "Nav: %u triangles, %u vertices, %u region%s", navigation.triangleCount(),
                         navigation.vertexCount(), navigation.regionCount(),
                         navigation.regionCount() == 1 ? "" : "s");
}

void Engine::initAudio() {
    auto zone = core::Profiler::scope("Engine::initAudio");
    // Built from what the file authored plus what the config placed, for the reason
    // instances, emitters and colliders are all built here rather than inside load():
    // what a scene *has* and what the application *plays* are two decisions. Sponza
    // declares no sound, so the config list is what gives the subsystem something to do
    // in the sample scene -- the same role `decals` plays for 3.3.
    // The device, the budgets and the volume are settings -- properties of the machine and
    // of whoever is listening. The mix graph and the six occlusion constants are the
    // game's, because a bus a game does not use is not a preference and six numbers tuned
    // against one scene's geometry are game feel (S1).
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
    // The step reads these two rather than reaching back into `GameSetup` and `Config` every
    // frame, which is what lets it live in a translation unit that knows about neither.
    // Folded once, here, exactly as `acfg.occlusion` is -- so `--no-occlusion` is honoured by
    // the sweep and by the filter from the same expression.
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
        // A config-placed source has no scene to sit beside, so unlike the ones a glTF
        // declares (GltfScene resolves those against the document) this is the only
        // chance to say where it comes from.
        desc.file = core::Resources(s.file).string();
        desc.transform = glm::translate(glm::mat4(1.0f), s.position);
        desc.bus = s.bus;
        desc.volume = s.volume;
        desc.spatial = s.spatial;
        desc.loop = s.loop;
        desc.minDistance = s.minDistance;
        desc.maxDistance = s.maxDistance;
        desc.occlusion = s.occlusion;
        // Parsed here rather than in Config, so the schema has exactly one
        // spelling table and `audioLoadName` is it.
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
    // The config gate and nothing else. Everything below it is the same work a game keying
    // "start recording" does, which is why it is a public method and this is one line.
    if (!configData.record.enabled) return;
    (void)startRecording();
}

bool Engine::startRecording(const std::filesystem::path& path) {
    // Already teeing. `Recorder::start` would refuse a second session anyway, but it would
    // refuse it after `startCapture` had reopened the tap.
    if (render.recording()) return true;

    if (configData.window.headless) {
        // A headless run has no swapchain to read back, and `--frames N --capture` is the
        // thing that already answers "what did it draw" for one. Saying so beats a
        // recording that starts and produces an empty file.
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
        // A second of ring is far more than the gap between the audio thread producing
        // and the worker draining, and it is 384 KiB. Sizing it to the recording window
        // instead would be hundreds of megabytes to solve a problem measured in
        // milliseconds -- what holds the *recording* is the encoder, not this.
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
 * @brief Turn every `FABRIC_` placement into a soft body, and tell the renderer (C19).
 *
 * Called from `initPhysics`, after the colliders and before `finalize()`, because a cloth
 * has to be in the world the rigid bodies are in -- that is the whole reason C19 took
 * Jolt's solver rather than porting a compute shader across, and it is what makes a curtain
 * collide with a crate for nothing.
 *
 * The walk is the placements', in placement order, skipping what `addPlacementInstances`
 * skips -- the same repeated walk the collider binding above uses and for the same reason:
 * the slot numbers have to agree and repeating the walk is how that agreement is kept
 * rather than guessed at. **Here the agreement is also checked**: `kInstanceCloth` on the
 * slot the walk arrived at must be set, and a mismatch is reported and skipped rather than
 * silently deforming whatever instance the count landed on.
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

        // Linear over a list that is one entry per `FABRIC_` primitive -- single digits in
        // any scene anyone will author, so a map would be machinery for a scan that never
        // runs long.
        const scene::GltfScene::ClothSource* src = nullptr;
        for (const auto& candidate : sources) {
            if (candidate.primitive == p.primitive) src = &candidate;
        }
        if (src == nullptr) continue;

        scene::ClothDesc desc;
        desc.vertices = src->vertices;
        desc.masses = src->masses;
        desc.indices = src->indices;
        // The placement's transform, applied once, into the vertices. The instance's own
        // transform was set to identity when it was created, so this is the only place the
        // node hierarchy reaches a cloth -- and the last.
        desc.transform = p.transform;
        if (clothSystem.add(physicsWorld, thisSlot, p.primitive, desc)) ++placed;
    }

    if (placed == 0) return;
    core::Logger::status(core::LogCategory::Scene, "Cloth: %u soft bodies, %u vertices", placed,
                         clothSystem.vertexCount());

    // The deformed vertex buffer is sized from what deforms, and until this moment nothing
    // knew a curtain did. Re-running `setAnimator` is what re-sizes it -- the same
    // re-entry `createMorphed` already uses one subsystem along, and for the same reason.
    render.setCloth(&clothSystem);
    render.setAnimator(&sceneAnimator, &sceneData);
}

void Engine::createColliderBodies(uint32_t firstCollider, uint32_t colliderCount, std::vector<DrivenBody>& out) {
    for (uint32_t i = 0; i < colliderCount; ++i) {
        const scene::ColliderDesc& desc = sceneData.colliders()[firstCollider + i];
        // Two verbs, because the world makes two kinds of thing and each has its own handle
        // type. `createBody` does not route a Character motion.
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

        // Bound here rather than in `bindDrivenNodes`, which skips static bodies -- and a
        // sound on a static body is precisely the case that occludes itself forever without
        // being noticed, since nothing about it ever moves. A character is skipped instead:
        // `CharacterVirtual` is not in the broad phase at all, so it cannot be what a ray hit.
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
    // G3. A node per body, with the meshes and sounds the file authored on that glTF node
    // hanging off it: a *child* node per attachment, so one body may drive several meshes,
    // and the rest transform's inverse survives as the attachment's offset rather than being
    // folded into a local transform. A local transform is translation, rotation and scale,
    // and going through those would put every driven mesh through a decomposition -- exact in
    // mathematics and not in floats, which is a moved pixel in a suite that compares bytes.
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
            // What 3.4's velocity target and 3.9's dynamic BLAS tier both select on. Set here
            // rather than at load, which is the point of 4.1b's property (ii): the file says
            // what a thing *is*, the application says what moves.
            instanceTable.setFlags(id, scene::kInstanceDynamic, 0);
            pairLocomotion(a.character, id);
        }

        // A sound authored on the same node rides the body (S5.3). Its offset goes into the
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
    // The world exists whenever physics is enabled, empty or not. It used to be created
    // only where the loaded document declared a collider or a cloth, which reads as
    // "nothing here needs physics" and means "no file said so": a game that composes its
    // world in `init` -- after this runs -- had every `createBody` and `createCharacter`
    // refused by an uninitialised world, silently. An empty world costs one Jolt system
    // sized at `kBodyHeadroom`, and C40 made that capacity elastic, so there is nothing
    // left to save by guessing.
    if (!configData.physics.enabled) return;

    // The step and gravity are the game's: they are what the simulation *is*, and the step
    // in particular is load-bearing for determinism. The thread count is a property of the
    // machine, so it stays a setting (S1), and the capacity is nobody's to state (C40).
    scene::PhysicsConfig pcfg;
    pcfg.step = setup.sim.physicsStep;
    pcfg.gravity = setup.sim.gravity;
    pcfg.maxStepsPerFrame = configData.settings.get(core::options::physics::maxStepsPerFrame);
    pcfg.workerThreads = configData.settings.get(core::options::physics::workerThreads);
    // A soft body is a body to the system that has to hold it, so the cloths are in the
    // count the world is sized from.
    physicsWorld.init(pcfg, static_cast<uint32_t>(sceneData.colliders().size() + sceneData.clothSources().size()));
    physicsWorld.debugContacts = configData.physics.debugContacts;

    // The instance slot each placement got, keyed by node, so a body can find what it
    // drives. Built by walking the placements exactly the way addSceneInstances did:
    // same order, same skip, so the same slot numbers come out. That agreement is
    // load-bearing and is why the walk is repeated rather than guessed at.
    //
    // Keyed by the *collider* node rather than the placing node. For a collider
    // authored on the node that carries the mesh those are the same value, which is
    // every collider in `physics.gltf`. They differ for a rig: a capsule goes on the
    // node an author can see, and the skinned meshes hang several levels below it --
    // so matching on the placing node bound nothing at all and the controller walked
    // away from the character it was supposed to be. See `Placement::colliderNode`.
    std::vector<DrivenSlot> placementSlots;
    for (size_t i = 0, slot = 0; i < sceneData.placements().size(); ++i) {
        const scene::Placement& p = sceneData.placements()[i];
        if (sceneData.primitives()[p.primitive].indexCount == 0) continue;
        // `idAt` rather than a hand-built `InstanceId{slot, 0}`. The generation belongs to
        // the table, and writing a literal here meant this line silently agreed with a
        // detail of how `create()` numbers a fresh slot -- which C1 changed, because a
        // handle whose generation is zero has to be the invalid one.
        placementSlots.push_back({p.colliderNode, instanceTable.idAt(static_cast<uint32_t>(slot))});
        ++slot;
    }

    // Rebuilt, not appended to: this walks the whole collider table, so anything a previous
    // scene authored is naming a character that no longer exists.
    sourceBody.assign(audioEngine.sourceCount(), scene::BodyId{});
    authored.clear();

    std::vector<DrivenBody> added;
    createColliderBodies(0, static_cast<uint32_t>(sceneData.colliders().size()), added);

    // **Between the bodies and `finalize`, and only here.** A cloth has to be in the world
    // it collides with, and `addModel` -- the other caller of the pair above -- has no cloth
    // to add. This is the ordering that kept the two walks apart until they were split at it.
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
    // Decals (3.3). Clamped to a texture the scene actually has: an index past the end
    // of the bindless array is undefined behaviour in the shader, and a config typo
    // should not be the way that is discovered.
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

    // A file that ships its own lights wins, and the engine is where that is decided
    // because it is a property of the *file*. What a scene with no lights in it should get
    // instead is a game decision, taken by whatever a game does after `gltfScene().lights()`
    // comes back empty -- named as the condition rather than as the class, because engine
    // source citing one game by name is the thing the boundary exists to stop.
    //
    // D11 audited the one asymmetry here and left it: when a file ships lights but no
    // *directional*, the sun below keeps whatever `GameSetup` supplied, so an indoor scene
    // lit by point lights still gets the game's sun and the log line says so out loud.
    // Zeroing it there would make the stated rule true of all of the file's lighting and it
    // is the right change; it also moves three golden cases, which is a separate row's
    // claim to make rather than a byte-identical one's.
    // **One walk over the scene's lights and the game's, in that order** (D20). The sun used
    // to be three `GameSetup` fields copied straight into the renderer, and only a *scene*
    // light could be promoted into them; a game's sun and a file's sun were different kinds
    // of thing reaching the same three floats by different routes. Both are `GpuLight`s in a
    // list now, so this is the only place either becomes the sun.
    //
    // The first directional light wins and is taken *out* of the list rather than left in
    // it: the cascades are fitted to one direction and the shader routes every directional
    // light through them, so a second would be shaded against a shadow map built for the
    // first -- lit correctly and shadowed wrongly, which is worse than either. `updateLights`
    // puts it back at the head of the buffer, which is why the emission order is unchanged
    // and the golden set is byte-identical across this row.
    //
    // Scene before game, so a file that ships its own sun wins over one a game authored: a
    // light in the document is a property of the *content*, and D11's audit of the
    // asymmetry -- a file with point lights but no directional still gets the game's sun --
    // is preserved exactly, because the walk simply never finds a directional in the file.
    if (!sceneData.lights().empty() || !setup.look.lights.empty()) {
        render.lights.clear();
        bool sunTaken = false;
        uint32_t extraSuns = 0;
        const auto take = [&](const gfx::GpuLight& light) {
            const auto type = static_cast<gfx::LightType>(static_cast<uint32_t>(light.params.z));
            if (type == gfx::LightType::Directional) {
                // **A second directional light is dropped, not demoted to an ordinary one.**
                // There is one cascade set and the shader routes every directional light
                // through it, so keeping the second would shade it against a shadow map
                // built for the first: lit correctly and shadowed wrongly, which is worse
                // than either. Counted and reported rather than silent.
                if (sunTaken) {
                    ++extraSuns;
                    return;
                }
                sunTaken = true;
                // makeDirectionalLight stored the toward-the-light vector, so this
                // reads straight back out.
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

        // **The environment was baked before this ran.** `initRenderer` bakes the cube, the
        // irradiance map and the prefiltered chain from whatever `sunDirection` held then,
        // which is the member's own initialiser -- so every scene whose sun is not that
        // default was image-lit from somewhere its sun is not. A no-op where they agree.
        render.rebakeIblIfSunMoved();
        if (extraSuns > 0) {
            // The case this fires on is a game authoring a sun for its own scene and then
            // being run against a scene that ships one -- `scripts/golden.sh` does exactly
            // that to both games in this tree, eleven times each.
            core::Logger::status(core::LogCategory::Scene,
                                 "Lights: %u further directional light%s dropped -- there is one cascade set, and a "
                                 "second sun would be shadowed against the first's map",
                                 extraSuns, extraSuns == 1 ? "" : "s");
        }
    }
}

void Engine::applyCameraConfig() {
    auto zone = core::Profiler::scope("Engine::applyCameraConfig");
    // The three `camera.*` rows this cannot apply -- move speed, orbit sensitivity, zoom
    // step -- belong to a *controller*, and what this holds is a `Camera&`. A game asks
    // for them with `FlyCamera::applySettings`.
    scene::Camera& cam = camera();
    cam.fovYRadians = glm::radians(configData.settings.get(core::options::camera::fovDegrees));
    // Always framed first: frameBounds derives the near plane and the orthographic box
    // from the scene's size, and --camera says where to stand rather than replacing
    // everything that framing works out.
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
    // Declare first, bind second, in that order and never the reverse: the config can
    // only override an action that exists, and applyBindings says so about one it
    // cannot find. The engine declares only what the engine consumes -- the binding
    // menu's and the UI's click -- and a game declares its own in `Game::init`, which is
    // why `applyBindings()` is called after that rather than from here.
    //
    // **No `Camera.*` rows.** The camera's actions belong to whichever controller a game
    // installs, and a game that installs none has no camera rows in its binding menu at
    // all; see `Engine::setCamera` and `scene::FlyCamera`.
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

    // C16, here rather than in Config for the reason the conflict scan is here: the map as
    // it will actually be read is the only one worth checking against, and it does not
    // exist until the game has declared its own actions. Both failures below cost a whole
    // run and neither has a symptom -- a script that names nothing presses nothing, which
    // looks exactly like a feature that does not work.
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

// ========================================================================== callbacks
// Every callback does the same thing: hand the event to the input map and get out.
// Nothing here decides what a key *means* -- that is the action table, and `main.cpp`
// used to be where the two were the same thing (S1.1).

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
    // Guarded internally by `active()`, so both can be fed unconditionally and there
    // is no third place that decides which of them owns the keyboard.
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

// =============================================================================== frame

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

    // Order matters, and it is the same order every frame: poll the one device
    // that has no callback, resolve every action once, then let the frame read
    // them. Anything that asked a key directly would be asking a different
    // question at a different moment.
    {
        auto s = core::Profiler::scope("input");
        core::input::pollGamepads(inputMap);
        // C16, between the devices and the resolve, and in that position for two reasons.
        // After the poll, so a scripted pad wins over whatever is plugged in -- a run that
        // states its input must not depend on what is on the desk. Before `beginFrame`, so
        // a scripted press goes through `resolve` and the edge accumulator like any other:
        // text mode suppresses it, the deadzone applies to it, and a press and release on
        // one frame reads as the tap it is.
        configData.input.script.apply(inputMap, render.frameCount());
        inputMap.beginFrame();
        textInput.update(frameDelta);
        // Two things cannot own one keyboard. While the game's panel is open the binding
        // menu is not run at all, so Tab is the panel's focus traversal rather than the
        // menu's toggle -- and the menu cannot be opened *first* and then shadowed,
        // because it sets text mode while open, which suppresses the key that would have
        // opened the panel. The exclusion resolves itself.
        if (!uiOpen) bindingMenu.update(inputMap, textInput);
        render.overlayLines = bindingMenu.lines();
    }

    // The UI is begun lazily by ui(), so this is where last frame's list stops being
    // handed to the renderer and a frame that draws no panel draws none.
    uiBegun = false;
    render.uiDrawList = nullptr;

    // **Here and not in `endFrame`, because a game fills this from `frameUpdate`.** Cleared
    // at the far end it erased the game's lines before `drawFrame` ever saw them, which is
    // the one thing `Renderer::debugLines` documents itself as being for. The engine's own
    // two writers -- the physics wireframe and the audio occlusion pairs -- run inside
    // `endFrame` and so land after the game's, which is the order they should draw in
    // anyway. Still unconditional and still once a frame, so turning a toggle off still
    // empties it on the next frame rather than leaving the last one drawn forever.
    render.debugLines.clear();

    // C9, and before the game's frameUpdate for the reason `spatialIndex()` states: a
    // query a game makes has to see this frame's world. A structural change is a rebuild
    // and a moved instance is a refit -- the second is the common case by a wide margin,
    // and telling them apart is the whole reason the index tracks a revision.
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

    // Before the game's frameUpdate rather than after it, which is the one ordering
    // change G1 makes to the old loop and it is made deliberately: a game reading
    // `camera().yaw` to resolve "forward" wants this frame's yaw, not last frame's.
    // Nothing else observes the difference -- action state was resolved above and is
    // cached, and the UI does not read the camera.
    camera().update(inputMap, frameDelta);

    /**
     * **The pointer is held while something has asked for it, and given back the frame
     * nothing has.** `core::input::mouseGrab()` is that ask, and the engine no longer makes
     * it: a controller does, in the `update` above, which is what lets a camera that takes
     * no input take the pointer with it. A first-person camera or a cutscene wants the
     * pointer with no button down at all, and this reads the *desire* rather than any one
     * camera's action. `GLFW_CURSOR_DISABLED` hides it and stops reporting an absolute
     * position, so a turn can run further than the screen is wide instead of stopping at
     * the edge, and the cursor does not wander over whatever is behind the window while the
     * view spins.
     *
     * **After `update`, not before it**, now that the ask is made inside it -- a grab read
     * ahead of the camera that asks for it lands a frame late. Both mode changes make GLFW
     * report a discontinuous cursor position, which would be one enormous `cursorDelta` and
     * a snapped view; neither reaches the camera, because this frame's deltas were resolved
     * at the poll above, the grab lands on the frame the button went down, which
     * `FlyCamera::update` already skips for the same reason, and the release lands on a
     * frame where the orbit action is no longer held.
     *
     * Not while the game's panel is open, and not while the window is unfocused -- the
     * camera still orbits under a panel, and taking the pointer away from a UI the user is
     * clicking is worse than a drag that stops at the edge of the screen. The desire outlives
     * both, so nothing has to re-assert it when the panel closes.
     */
    const bool wantCursorHidden = window != nullptr && core::input::mouseGrabbed() && !uiOpen &&
                                  glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    if (wantCursorHidden && !cursorCaptured) {
        // Read before the mode change: disabling the cursor is what moves it.
        glfwGetCursorPos(window, &cursorBeforeCapture[0], &cursorBeforeCapture[1]);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        cursorCaptured = true;
    } else if (!wantCursorHidden && cursorCaptured) {
        // The drag ended, the panel opened, or the focus went elsewhere. Give the pointer
        // back rather than leaving it captured by something nothing is now reading.
        if (window != nullptr) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            glfwSetCursorPos(window, cursorBeforeCapture[0], cursorBeforeCapture[1]);
        }
        cursorCaptured = false;
    }

    // The listener is the camera (S5.3), and it is here rather than inside the fixed-step
    // block because the camera is a per-frame quantity: it is driven by input, which
    // arrives at the frame rate. Where it looks is where the panning resolves against, so
    // an orbit sweeps a source across the stereo field.
    //
    // World up rather than the camera's own: rolling the view should not roll the room,
    // and nothing in this engine rolls the camera anyway.
    //
    // **Off for a game that places its own ears** (C28). This runs before
    // `Game::frameUpdate`, so a game writing its own listener here would have it overwritten
    // before it was heard, and moving this after the game's write would overwrite the game's
    // instead. Neither order is right for both, so `GameSetup::audio.listenerFollowsCamera` says
    // which of the two owns the ears and the other one does not write. It only ever owned
    // listener 0 in any case -- the rest are the game's from the day it asked for them.
    if (audioEngine.active() && setup.audio.listenerFollowsCamera) {
        const glm::vec3 eye = camera().position();
        const glm::vec3 toFocus = camera().focus - eye;
        const float reach = glm::length(toFocus);
        audioEngine.setListener(eye, reach > 1e-4f ? toFocus / reach : glm::vec3(0.0f, 0.0f, -1.0f),
                                glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // A fixed yaw step per frame, after the input-driven update so a spin is what the
    // flag says rather than what the frame rate made of it. Deliberately not scaled by
    // `dt`: the whole point is that frame 60 is the same frame 60 on every run, and a
    // wall-clock term would make a cache-hit-rate comparison depend on how fast the
    // machine happened to be. See Config::Camera::spinDegreesPerFrame.
    if (configData.camera.spinDegreesPerFrame != 0.0f) {
        camera().yaw += glm::radians(configData.camera.spinDegreesPerFrame);
    }

    // Rebuilt every frame rather than only when the camera moves: the comparison
    // that would save the snprintf is six floats wide, and the string it saves is
    // eighty bytes into a buffer that is reused.
    {
        const glm::vec3 eye = camera().position();
        char text[96];
        std::snprintf(text, sizeof(text), "--camera %.2f,%.2f,%.2f,%.1f,%.1f,%.2f   eye %.2f %.2f %.2f",
                      camera().focus.x, camera().focus.y, camera().focus.z, glm::degrees(camera().yaw),
                      glm::degrees(camera().pitch), camera().distance, eye.x, eye.y, eye.z);
        render.cameraLine = text;
    }

    // ------------------------------------------------------- simulation (S4.3)
    // The clock is fed either exactly one step (`locked`, the default) or the frame's
    // wall-clock delta (`realtime`), and that is the whole of the difference. Under a
    // locked clock the accumulator lands on exactly zero, so the step loop runs once,
    // `alpha` is exactly zero, and every frame is a function of the frame index -- which
    // is bit-for-bit what the engine did before S4 and is what keeps ten golden cases
    // valid. There is no second code path; there is a different `dt`.
    simClock.accumulate(realtimeClock ? frameDelta : simClock.step());

    // Before anything moves. The table still holds where things were when the last
    // frame was drawn, and that is exactly what 3.4's velocity pass has to reproject
    // against -- see InstanceTable::endFrame, which argues the ordering at length
    // because getting it backwards produces a pass that works and reports no motion.
    instanceTable.endFrame();

    return true;
}

// ==================================================================== persistence (C6)

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

    // Read whole, ask, then apply. The middle step is the promise: a save from another
    // scene is refused here, with nothing written, rather than halfway through scattering
    // one scene's transforms over another's. See scene/WorldSave.h for why the decision
    // lives there and not in this function.
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
        // Through the presentation transform (P2). The cursor is in window pixels and the
        // UI may have been laid out against a 320x180 target presented at 6x in the middle
        // of a letterbox, so without this every hit test in a virtual-resolution game is
        // wrong by the scale and the bars. Identity in every other case, which is why it
        // is called unconditionally rather than guarded here.
        uiInput.mouse = render.uiFromWindow(
            {static_cast<float>(inputMap.cursorX()), static_cast<float>(inputMap.cursorY())});
        uiInput.mouseDown = inputMap.held(uiClickAction);
        uiInput.mousePressed = inputMap.pressed(uiClickAction);
        uiInput.mouseReleased = inputMap.released(uiClickAction);
        uiInput.scroll = static_cast<float>(inputMap.scrollDelta());
        // Read raw rather than through actions, and for the reason the binding menu
        // gives about its own keys: while a panel has the keyboard nothing else is
        // listening, and Tab/Enter/Escape in the binding list would be four things a
        // player must not rebind.
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
    // One line, and that is the row's result (C27). The order every one of these runs in
    // lives in `scene/Simulation.cpp`, which links with no device -- so a headless loop
    // steps the same code this does rather than a second copy of it.
    sim.step(stepSeconds);
    growParticles();
}

void Engine::growParticles() {
    /**
     * **The pair, and the only place either half may be called** (C40).
     * `ParticleSystem::grow` resizes the pool's CPU side and `Renderer::resizeParticlePool`
     * resizes the GPU buffers the shaders write into; doing the first without the second
     * emits past the end of a device allocation, which is why `ParticleSystem.h` used to say
     * flatly that the pool is not resized and a game had to state `particleBudget` instead.
     *
     * After the step rather than inside it. A resize waits for the device to go idle and
     * re-records descriptor sets, neither of which belongs in the middle of a fixed step, and
     * an emitter created this step is one whose particles are wanted next step.
     */
    const uint32_t want = particleSystem.wantedCapacity();
    if (want <= particleSystem.capacity()) return;
    if (!particleSystem.grow(want)) return;
    render.resizeParticlePool();
}

void Engine::endFrame() {
    // ------------------------------------------------------------------ UI (S6)
    if (uiBegun) {
        auto s = core::Profiler::scope("ui");
        uiContext.end();
        render.uiDrawList = &uiContext.draw();
    }
    // Set for the *next* frame's resolution, which is the same one-frame arrangement the
    // binding menu makes and for the same reason: what the UI wants is only known once it
    // has been laid out, and the actions were resolved before that.
    inputMap.setPointerMode(uiBegun && uiContext.wantsPointer());
    // Text mode only while a panel owns the keyboard. When one does not, the binding menu
    // does, and it sets its own -- two writers to one flag is how a field ends up
    // suppressing WASD forever.
    if (uiBegun) inputMap.setTextMode(uiContext.wantsKeyboard());

    {
        auto s = core::Profiler::scope("writeback");
        const float alpha = simClock.alpha();

        // Rigid nodes follow the clip: a placement records the node that put it there, so
        // an animated transform is a setTransform() away. The pose they follow is the one
        // that animates the node -- `poseFor` -- rather than the first character's. The
        // skinned instances are excluded because their vertices already carry the pose --
        // applying the node transform as well would move the character twice.
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

            // An emitter placed by an animated node follows it, which is the whole reason
            // ParticleEmitter retains a node index (S3.1). A torch on a walking character
            // is an emitter whose transform is a joint's -- and on *that* character's, which
            // is what this row was.
            for (scene::ParticleEmitter& e : particleSystem.emitters()) {
                const std::vector<glm::mat4>& world = poseFor(e.node);
                if (e.node < world.size()) e.transform = world[e.node];
            }
        }

        // And the scene tree writes what it owns, *after* the animation loop above rather
        // than before it: a node with both a clip and a collider is one the physics owns,
        // and the last writer is the one that wins.
        //
        // G3 replaced two hand-written loops here with this one call, and it does more
        // than they did: a light, an emitter or a sound on a node now follows it too, and
        // so does anything a game parented to one.
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

    // The simulation side of the trace's counters. `droppedSteps` in particular is why a
    // counter is not the same thing as the log line below it: the log says *once* that
    // steps have been dropped, and the counter says on which frames -- which is the
    // question anyone reading a stutter actually has.
    core::Profiler::counter("droppedSteps", simClock.droppedSteps());
    core::Profiler::counter("bodies", physicsWorld.bodyCount());
    core::Profiler::counter("particles", particleSystem.aliveCount());
    core::Profiler::counter("audioSources", audioEngine.sourceCount());
    core::Profiler::counter("nodes", sceneTree.liveCount());

    // Reported when it changes rather than every frame, for the reason every other stated
    // policy in this engine gives -- a warning at 60 Hz drowns the log it is trying to
    // appear in. Time the simulation did not run is something a game needs told, and told
    // once.
    if (simClock.droppedSteps() != droppedStepsReported) {
        droppedStepsReported = simClock.droppedSteps();
        core::Logger::warn(core::LogCategory::Core,
                     "Simulation: %u whole steps dropped since start -- the frame is slower than %u steps of "
                     "%.4f s (raise physics.maxStepsPerFrame, or find what is stalling)",
                     droppedStepsReported, configData.settings.get(core::options::physics::maxStepsPerFrame),
                     static_cast<double>(simClock.step()));
    }

    // The wireframe of the physics world, rebuilt each frame it is asked for and handed
    // to the renderer as plain vertices (S4.5). Appended rather than assigned: the list was
    // cleared in `beginFrame` and a game may already have written into it.
    if (physicsDebugDraw) {
        // `debugContacts` is assigned once, in `initPhysics`. It used to be re-read here
        // every frame so that a panel toggle over the row took effect; D14 took the row --
        // drawing contact points is a developer control behind `--physics-contacts` -- so
        // there is nothing left for a frame to notice a change in.
        auto s = core::Profiler::scope("physicsDebugDraw");
        physicsWorld.drawDebug(render.debugLines, camera().position());
    }

    // One line from each listener to each source, green when it is heard clear and red
    // when it is fully occluded (S5.5). Into the same vector the physics wireframe goes
    // into, which is the payoff of S4.5 having made `debugLines` a plain vertex list the
    // application fills: making occlusion visible needed no drawing code, no pipeline and
    // no pass -- twelve lines here and a colour ramp.
    //
    // Every listener, because with two of them the interesting picture is which ears a
    // source is behind a wall from -- one line would draw the answer the sweep did not take.
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

    // Before drawFrame, so the frame that carries index `captureFrame` is the one
    // written. Requesting after the draw would silently capture the frame after the one
    // named, which is exactly the sort of off-by-one a golden image enshrines for months.
    if (configData.benchmark.captureFrame != 0 && render.frameCount() == configData.benchmark.captureFrame) {
        render.requestCapture(configData.benchmark.capturePath);
        if (!configData.benchmark.captureTarget.empty()) {
            render.requestTargetCapture(configData.benchmark.captureTarget, configData.benchmark.captureTargetPath,
                                        configData.benchmark.captureTargetMip, configData.benchmark.captureTargetLayer);
        }
    }

    // Same placement, and deliberately not adjusted by one. RenderDoc delimits captures
    // by present and counts frames itself, so what this arms is "the next whole frame
    // from here", which is the frame `drawFrame` is about to record or the one after it
    // -- and the number in the .rdc filename is RenderDoc's count of presents, not
    // `frameCount()`. Guessing an offset to make those two agree would be inventing a
    // precision the API does not offer. What the flag actually guarantees is the thing
    // that matters: the capture is taken at a stated point well past the load hitch, in
    // steady state, rather than wherever the run ended.
    if (configData.benchmark.rdocCaptureFrame != 0 && render.frameCount() == configData.benchmark.rdocCaptureFrame) {
        gfx::renderDocTrigger(1);
    }

    // 0.4's drive. Before drawFrame so the resize is discovered inside the frame that
    // follows it rather than by the next poll: GLFW dispatches the framebuffer-size
    // callback from glfwPollEvents, so requesting the size here means the callback --
    // and therefore `requestResize()` -- lands one poll later, with a full frame of
    // acquire, submit and present recorded in between. That interleaving is the whole
    // hazard, and asking for it after the draw would spend it on the poll instead.
    if (configData.benchmark.resizeEveryFrames != 0 && render.frameCount() != 0 &&
        render.frameCount() != lastResizeFrame &&
        render.frameCount() % configData.benchmark.resizeEveryFrames == 0) {
        lastResizeFrame = render.frameCount();
        // Two sizes rather than a walk, because what is under test is the recreate and
        // not the dimensions. Alternating guarantees every request is a genuine change --
        // asking for the size the window already has is a no-op the window system answers
        // with silence, which would read as a clean run having tested nothing at all.
        resizeSmall = !resizeSmall;
        glfwSetWindowSize(window, configData.settings.get(core::options::window::width) - (resizeSmall ? 160 : 0),
                          configData.settings.get(core::options::window::height) - (resizeSmall ? 90 : 0));
    }

    // G2. The one row `bindRenderer` cannot bind, applied the way every other row is
    // refreshed: by comparison, once a frame, with no dispatch and nothing to subscribe
    // to. `setSampleCount` returns early unless the clamped count actually differs, so
    // this costs a comparison, and it is what makes the panel, the console and
    // `substrate.json` reach it by the same path as the other thirty-odd.
    //
    // `render.tonemap` was the second line here and D14 removed it rather than kept it:
    // the curve is `GameSetup::look.tonemap` now, written once in `initRenderer`, so polling
    // it would overwrite the renderer's own field every frame -- which is precisely what
    // would have broken the demo's F11 cycle.
    render.setSampleCount(configData.settings.get(core::options::render::msaaSamples));

    // P4. Here rather than inside `drawFrame` because the renderer holds the table by
    // const pointer and this is the one thing that mutates it -- and it costs two
    // comparisons on a frame where no sprite was created, destroyed or reassigned a layer.
    // Sprites moving is not one of those things, which is the whole design.
    spriteTable.prepare();

    if (render.drawFrame(camera()) == gfx::FrameResult::WindowClosed) closed = true;

    if (const uint64_t limit = configData.benchmark.exitAfterFrames;
        limit != 0 && render.frameCount() >= limit) {
        closed = true;
    }

    frameScope.reset();
}

// ================================================================================= run

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

    // Startup ends here. Everything above -- window, device, renderer, scene, subsystems
    // and the game's own world -- is frame 0, and `scripts/baseline.py --startup` is what
    // reads it.
    startupFrameScope.reset();

    // **The clock starts where startup ends, not where `init` returned.** `Engine::init`
    // stamps `lastTime` too, but `game.init` runs after it and builds the world -- meshes,
    // acceleration structures, an appended model -- so leaving that stamp alone hands the
    // first frame a `frameDelta` covering the game's whole construction. The accumulator
    // turns that into more steps than `maxStepsPerFrame` allows and discards the rest,
    // which reported as a stall on every launch and buried the warning that means one.
    lastTime = std::chrono::steady_clock::now();

    /**
     * One world unit per texel of the virtual target, with the world origin at its
     * centre. Both run modes below want exactly this, and it is what a pixel-exact 2D
     * game wants: with `orthoHeight` equal to the render height, the half-width is
     * `height * aspect / 2` -- which is half the render *width* -- so world (x, y) lands
     * on texel (x + w/2, h/2 - y), one for one, with no scale left to round.
     */
    const auto pixelPerfectCamera = [&] {
        // **The null camera first, so nothing is driving the pose this computes.** These
        // eight numbers used to be written over whatever controller the game had installed,
        // while its `update()` ran underneath every frame free to move all four of the pose
        // ones back. It survived only because the run modes below are non-interactive; a
        // real 2D game asking for this projection would not have been so lucky.
        setCamera(nullptr);
        const VkExtent2D extent = render.renderTargetExtent();
        camera().projectionMode = scene::Camera::Projection::Orthographic;
        camera().orthoHeight = static_cast<float>(extent.height);
        camera().nearPlane = 0.1f;
        camera().orthoFar = 100.0f;
        camera().focus = glm::vec3(0.0f);
        // **Down -Z, which is yaw = pi, and it is not the obvious value.** At yaw 0 the
        // camera looks down *+Z*, `glm::lookAt`'s right vector comes out as -X, and the
        // whole world is mirrored -- a sprite placed on the left edge draws on the right,
        // which looks exactly like a sign error in the projection and is not one. Down -Z
        // is what "+X right, +Y up" means for a flat world, and it is the first thing a 2D
        // game has to know about this camera.
        camera().yaw = glm::pi<float>();
        camera().pitch = 0.0f;
        // Anywhere between the near and far planes; a parallel projection does not care,
        // which is the one thing that makes this arithmetic short.
        camera().distance = 10.0f;
        return extent;
    };

    // P2's readback, after `Game::init` so it loads through the same table a game does,
    // and so a game that already loaded the same file gets the same slot back rather than
    // a second copy. A run mode, not a feature: `--readback` is the only thing that sets it.
    gfx::ImageId readbackId;
    scene::SpriteLayerId readbackLayer;
    // P5's arm. Held here rather than in a member because the only thing outside this
    // function that needs them is the loop below, which is in this function too.
    scene::SpriteSheetId readbackSheet;
    scene::SpriteId readbackSheetSprite;
    uint32_t readbackFrameAtCapture = scene::SpriteTable::kNoFrame;
    if (!configData.benchmark.readbackImage.empty()) {
        readbackId = imageTable.load(configData.benchmark.readbackImage);

        if (configData.benchmark.readbackLitSprite) {
            // P6. The same file, the same camera and the same corner as the case above,
            // through the G-buffer instead of after the tonemap -- so what differs between
            // the two run modes is the *path*, which is the only way the pair says
            // anything. The value cannot be bit-exact here and the check does not ask it
            // to; see `gfx::compareSilhouette` for what it asks instead.
            const VkExtent2D extent = pixelPerfectCamera();
            const glm::uvec2 size = render.imageSize(readbackId);
            if (size.x == 0 || size.y == 0) {
                core::Logger::error(core::LogCategory::Render, "Readback: no resident image for --readback-lit-sprite");
                return 1;
            }
            const scene::GltfScene::ModelId lit = createLitSprite({
                .image = readbackId,
                .size = glm::vec2(size),
                // Top-left, so the quad's own corner is the corner being placed -- the
                // same convention the unlit case uses, and the reason the two are
                // comparable at all.
                .pivot = {0.0f, 0.0f},
                // Just inside the near plane, which is at z = 9.9 for this camera. A lit
                // sprite is depth-tested by design, so the one thing this case must not
                // measure is whether the test scene happened to stand in front of it.
                .position = {-0.5f * static_cast<float>(extent.width), 0.5f * static_cast<float>(extent.height),
                             9.85f},
                .cutoff = configData.benchmark.readbackLitCutoff,
                // Matte and unlit-looking is not the point; what the check reads is
                // coverage, and emissive would stop it casting a shadow, which is one of
                // the things the pass being exercised has to do.
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
            // P4. The same file, the same expectation, through the sprite pass instead of
            // the overlay -- so the thing being proved is the projection, the quad, the
            // texel-to-normalised divide and the blend, none of which the five overlay
            // cases touch.
            const VkExtent2D extent = pixelPerfectCamera();
            const glm::uvec2 size = render.imageSize(readbackId);
            if (size.x == 0 || size.y == 0) {
                core::Logger::error(core::LogCategory::Render, "Readback: no resident image for --readback-sprite");
                return 1;
            }
            readbackLayer = spriteTable.createLayer({});

            // P5. The same file again, cut into its own four quarters and played as a
            // clip, so what lands on texel (0, 0) is **one cell** selected by the clock
            // rather than the whole image. The sheet is derived from the file rather than
            // stated in a flag for the reason the UV rect is in texels at all: the only
            // number that has to be right is the one already in the image.
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
                // The whole image, which is the case the shader resolves rather than the
                // call site: nothing here knows the file's dimensions and it should not
                // have to learn them to draw all of it. The sheet case overwrites this on
                // `play`, with a rectangle the animation chose.
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

    // P4's trace arm. Not a feature and not reachable from a game: `scripts/baseline.py`
    // needs a stated sprite count it can set from a shell, because a number quoted on a
    // card that nobody else can reproduce is an anecdote.
    gfx::ImageId stressImage;
    uint32_t stressSpawned = 0;
    uint32_t stressColumns = 1;
    VkExtent2D stressExtent{};
    scene::SpriteLayerId stressLayers[4];
    // Only populated for `--sprites-move`: the static arm is the one every number on P4's
    // card was taken in, and a vector filled per create in it would be a difference between
    // the arms that is not the thing being measured.
    std::vector<scene::SpriteId> stressMoving;
    std::vector<glm::vec2> stressHome;
    if (configData.benchmark.spriteStress > 0) {
        stressExtent = pixelPerfectCamera();
        stressImage = imageTable.load(configData.benchmark.spriteStressImage);
        const glm::uvec2 size = render.imageSize(stressImage);

        // A square-ish grid across the visible area, at 16 world units -- which is 16
        // texels, which is the size the arc's own sketch draws a sprite at. Sprites
        // overlap where the count exceeds what the area holds, which is deliberate: the
        // pass's cost is overdraw, and a grid that thinned out as the count rose would be
        // measuring a different thing at each arm.
        stressColumns = std::max(
            1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(configData.benchmark.spriteStress)))));

        // Four layers rather than one, so the sort has something to order and the number
        // is not a measurement of a single-key sort.
        for (int i = 0; i < 4; ++i) stressLayers[i] = spriteTable.createLayer({.order = i});

        core::Logger::status(core::LogCategory::Render, "Sprites: %u across %u columns of %ux%u texels",
                             configData.benchmark.spriteStress, stressColumns, size.x, size.y);
    }

    /// A batch per frame rather than all of them before the loop, and the reason is the
    /// verification rather than realism -- though it is also what a game does. Spawning
    /// the lot up front sizes the buffers exactly once, so the doubling path in
    /// `ensureSpriteCapacity` would never run with layers on, and a growth path first
    /// exercised in somebody's game is a growth path that has never been tested. Eight
    /// batches means three or four real reallocations, and the count is at its stated
    /// value from frame 8 onward -- which is before any trace this is used for begins to
    /// mean anything.
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

    /// The moving arm (`--sprites-move`). Every sprite, every frame, by a fraction of a
    /// texel about where it was spawned: the point is that the *table* changes every frame,
    /// so the renderer's per-slot revision never matches and the upload pays its whole
    /// copy. Sub-texel because the two arms must differ in the upload and not in what is
    /// drawn -- a sprite that wandered off the visible area would be measuring less
    /// overdraw as well as less traffic.
    const auto moveStressBatch = [&] {
        if (stressMoving.empty()) return;
        const float phase = static_cast<float>(render.frameCount()) * 0.05f;
        for (size_t i = 0; i < stressMoving.size(); ++i) {
            const float wobble = std::sin(phase + static_cast<float>(i) * 0.017f) * 0.25f;
            spriteTable.setPosition(stressMoving[i], stressHome[i] + glm::vec2(wobble, wobble));
        }
    };

    while (beginFrame()) {
        // At the top of a frame and nowhere else. C10.
        applyPendingScene();
        spawnStressBatch();
        moveStressBatch();
        {
            auto s = core::Profiler::scope("Game::frameUpdate");
            game.frameUpdate(*this, frameDelta);
        }
        {
            // Above the `uiOpen` test, not inside it, for the reason every other zone in
            // the tree sits above its early-out: a row that vanishes when the work is
            // skipped cannot be read as "this cost nothing", only as "this is missing".
            auto s = core::Profiler::scope("Game::drawUi");
            if (uiOpen) game.drawUi(*this, ui());
        }

        // A readback case pins the camera, and a game moves it. Both are right, which is
        // why this re-asserts rather than forbids: `--readback-sprite` compares against an
        // image computed from the source file, so the projection it was computed for has to
        // be the projection that renders -- and the demo's follow rig (G13) writes `focus`
        // every frame, as a third-person camera must. Setting it once before the loop was
        // enough only while nothing else wrote it.
        //
        // The overlay cases need none of this: they are screen space and never see the world
        // camera, which is exactly why they kept passing while all four sprite cases and the
        // lit silhouette failed.
        if (configData.benchmark.readbackSprite || configData.benchmark.readbackLitSprite) pixelPerfectCamera();
        while (consumeStep()) {
            {
                auto s = core::Profiler::scope("Game::fixedUpdate");
                game.fixedUpdate(*this, simClock.step());
            }
            simulate(simClock.step());
        }

        // **Once a frame, after the steps rather than inside them** (C19). A frame runs
        // between zero and four steps and only the last one's pose is ever drawn, so
        // reading the solve per step would pay for the normal recompute -- the expensive
        // half -- three times over for poses nothing looks at. A frame that ran no steps
        // reads the same pose again, which costs one memcpy per cloth and keeps the buffer
        // the renderer copies from valid.
        if (!clothSystem.empty()) {
            auto s = core::Profiler::scope("Cloth");
            clothSystem.update(physicsWorld);
        }
        // P5. After this frame's steps and before the draw, which is where `endFrame`
        // takes the capture -- so this is the cell the captured image actually holds. The
        // run does not stop at the capture frame, and reading the frame at the *end* of
        // the run would be asking about a later cell than the one being compared.
        if (readbackSheetSprite.valid() && render.frameCount() == configData.benchmark.captureFrame) {
            readbackFrameAtCapture = spriteTable.frame(readbackSheetSprite);
        }
        endFrame();
    }

    game.shutdown(*this);

    render.logGpuTimings();
    ctx.logMemoryUsage("steady state");

    // A capture that was asked for and did not happen has to say so. The failure mode
    // this catches is `--frames` set below `--rdoc-capture-frame`: the run exits before
    // the trigger fires, reports success, and leaves the analysis scripts pointed at a
    // file that was never written.
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

    // -------------------------------------------------------------- readback (P2)
    // Before the golden comparison and not instead of it: the two answer different
    // questions and a run may reasonably ask both. This one is the P arc's own standard --
    // a texel authored is a texel presented -- and its expected image is *computed* from
    // the source rather than snapped from a previous run, so there is nothing to re-snap
    // when it fails.
    if (!configData.benchmark.readbackImage.empty()) {
        if (render.capturesWritten() == 0) {
            core::Logger::error(core::LogCategory::Render,
                                "Readback: no capture was written -- did the run reach frame %llu?",
                                static_cast<unsigned long long>(configData.benchmark.captureFrame));
            return 1;
        }

        // Where the image was actually drawn, which is the whole question. Inside the
        // virtual target it is magnified by the presentation scale and lands at the
        // letterbox offset; outside it, the overlay drew after the blit at the window's
        // own resolution, so it is at 1x in the corner and the bars are behind it.
        // A sprite is world-space content and always draws into the virtual target, so
        // `uiInsideVirtual` has nothing to say about it (P4). It is the overlay that has
        // the choice, and only the overlay.
        const gfx::PresentLayout& p = render.present();
        const bool inVirtual = render.uiInsideVirtual || configData.benchmark.readbackSprite ||
                               configData.benchmark.readbackLitSprite;
        const uint32_t scale = inVirtual ? p.scale : 1u;
        const int32_t x = inVirtual ? p.x : 0;
        const int32_t y = inVirtual ? p.y : 0;

        // P6. The lit path takes the silhouette check instead, and a run with no background
        // named *is* the background: it wrote the capture the measured run will be held
        // against, and there is nothing for it to compare.
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

        // P5. Which rectangle of the source file the capture is held against, and the
        // number that decides it is the one the *caller* stated -- never the one the
        // animation reached. Cropping to whatever cell the playback happened to land on
        // would compare frame selection against itself and pass for any selection at all.
        // So the frame is asserted first, in its own terms, and the crop follows from the
        // assertion rather than from the engine's own answer.
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
            // The four quarters of the file, written out here rather than asked of
            // `SpriteTable::frameUv` -- and the second copy is the point, for the reason
            // `compareReadback` gives about expanding rather than resampling. Cropping
            // with the same call the *draw* used would make a transposed slicing agree
            // with itself: the wrong cell would be drawn and the wrong cell expected, and
            // the case would pass. Two independent statements of one layout is what makes
            // a disagreement between them visible.
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
        // The cell is named in the verdict rather than only in the set-up line, because
        // the verdict is what a script echoes and "which cell" is the claim P5 makes.
        char cell[48] = {};
        if (readbackSheetSprite.valid()) {
            std::snprintf(cell, sizeof(cell), " cell %u of", configData.benchmark.readbackSheetFrame);
        }
        core::Logger::status(core::LogCategory::Render,
                             "Readback: bit-identical at %ux --%s %s expanded to %ux%u at (%d,%d), 0 of %u differ",
                             scale, cell, configData.benchmark.readbackImage.c_str(), r.width, r.height, x, y,
                             r.width * r.height);
    }

    // ------------------------------------------------------- golden image (5.3)
    // Between the render loop and shutdown, so a failure is reported with the run's
    // timings still on screen and the process still exits cleanly. The verdict travels
    // out as the exit code, which is the only part a shell loop can read.
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
    // Idempotent, because there are two ways in: `shutdown()`, which `main` calls, and
    // `~Engine`, which runs if `run()` unwinds instead of returning. Whichever gets here
    // first does the work and the other is a no-op.
    if (tornDown) return;
    tornDown = true;

    // First of everything. `stopRecording()` is the ordered path -- drain the readback
    // slots while the device is still up, join the worker, then take the audio tap back --
    // and it is the same call a game makes from a key, so exit and a keypress cannot end a
    // recording two different ways.
    if (ctx.device != VK_NULL_HANDLE) (void)stopRecording();
    // What that call cannot cover is a session whose encoder died: `Renderer::recording()`
    // is already false there, and the worker thread and the two pipes are still open. So
    // `stop()` runs unconditionally as well. Both lines are no-ops after a `stopRecording`
    // that did the work, and after a run that never recorded anything.
    if (recorder.active()) core::Logger::status(core::LogCategory::Render, "Record: finishing the file");
    recorder.stop();
    audioEngine.stopCapture();

    // Before the GPU teardown and not after it, because a streamed voice is a file handle
    // and a decode job, and neither has anything to do with the device. Explicit rather
    // than left to the destructor so the order is stated where the rest of the shutdown
    // order is.
    audioEngine.shutdown();

    // Sprites first, because they name image slots: a table torn down in the other order
    // would leave sprites resolving handles against a table that had already given up.
    spriteTable.shutdown();
    render.setSprites(nullptr);

    // Before the renderer, so a game that skipped `destroy` still leaves the table saying
    // nothing is live by the time the images behind it are freed. Views first: a live one
    // holds an image slot, and releasing it after the image table had been cleared would
    // be releasing a slot that no longer exists.
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

    // Here and not in `applyBindings()`, which is the only moment this answer is true. A
    // config row nothing has declared *yet* is held for the declaration that may still
    // come -- a camera declares its rows when it is installed, which can be a keypress an
    // hour in. Anything still held when the run ends is a name this build never had, and
    // that is what the message has always claimed to mean.
    for (const auto& [name, list] : inputMap.parkedBindings()) {
        core::Logger::warn(core::LogCategory::Input, "Config binds unknown action \"%s\" (%s); ignored", name.c_str(),
                           list.c_str());
    }

    core::Profiler::dump();
    core::Profiler::shutdown();
    core::Logger::status(core::LogCategory::Core, "Substrate exiting");
    core::Logger::shutdown();
}
