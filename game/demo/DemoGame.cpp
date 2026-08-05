#include "DemoGame.h"

#include "Entry.h"
#include "core/Resources.h"
#include "core/Logger.h"
#include "core/Profiler.h"
#include "gfx/RenderDoc.h"
#include "gfx/ShaderVariant.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <initializer_list>
#include <span>
#include <string>

namespace {

const char* debugViewName(core::DebugView view) {
    switch (view) {
    case core::DebugView::Lit: return "lit";
    case core::DebugView::Albedo: return "albedo";
    case core::DebugView::Normal: return "normal";
    case core::DebugView::Orm: return "occlusion/roughness/metallic";
    case core::DebugView::Depth: return "depth";
    case core::DebugView::Emissive: return "emissive";
    case core::DebugView::Ssao: return "ssao";
    case core::DebugView::Edges: return "edges";
    default: return "?";
    }
}

/// Where an interactive F12 screenshot goes.
std::string nextCapturePath() {
    static uint32_t counter = 0;
    char name[64];
    std::snprintf(name, sizeof(name), "debug_frames/capture_%03u.png", counter++);
    return name;
}


/**
 * @brief The locomotion state machine every character runs.
 *
 * The second parameter is `airborne` and not `grounded`, which is load-bearing: every
 * parameter starts at zero, so a machine nobody is driving must read as *standing on
 * something*. Spelled the other way round, a scene whose characters run off the fixed step
 * rather than off a controller falls through the floor of the machine on the first frame.
 */
scene::AnimationStateMachine locomotionMachine(const anim::SceneAnimator& animator) {
    // Several spellings per state: Mixamo, Khronos and Blender each name these
    // differently, and the alternative is one machine per exporter.
    struct Candidate {
        const char* state;
        std::initializer_list<const char*> clips;
        core::LoopMode loop;
    };
    const Candidate candidates[] = {
        {"idle", {"idle", "Idle", "Survey", "idle (2)"}, core::LoopMode::Loop},
        {"walk", {"walking", "walk", "Walk", "left strafe walking"}, core::LoopMode::Loop},
        {"run", {"running", "run", "Run"}, core::LoopMode::Loop},
        // `jumping up` ahead of `jump`, and the order is not cosmetic. On this rig `jump`
        // is the *whole* 2.17 s leap -- crouch, launch, hang and landing -- while
        // `jumping up` is the 0.25 s launch alone. With `fall` and `land` as states of
        // their own, preferring `jump` plays its own landing over theirs, a second and a
        // half early.
        {"jump", {"jumping up", "jump", "Jump"}, core::LoopMode::ClampToEnd},
        {"fall", {"falling idle", "fall", "Fall", "falling"}, core::LoopMode::Loop},
        {"land", {"hard landing", "land", "Land", "landing"}, core::LoopMode::ClampToEnd},
    };

    scene::AnimationStateMachine m;
    std::vector<std::string> found;
    for (const Candidate& c : candidates) {
        for (const char* name : c.clips) {
            const uint32_t clip = animator.findClip(name);
            if (clip == anim::SceneAnimator::kNoClip) continue;
            m.states.push_back({c.state, clip, c.loop, 1.0f});
            found.emplace_back(c.state);
            break;
        }
    }
    // One state is not a machine: it is the clip the animator would have played anyway.
    if (m.states.size() < 2) return {};

    m.parameters = {{"speed", false}, {"jump", true}, {"airborne", false}};
    const uint32_t kSpeed = 0;
    const uint32_t kJump = 1;
    const uint32_t kAir = 2;

    const auto state = [&m](const char* name) { return m.findState(name); };
    const uint32_t idle = state("idle");
    const uint32_t walk = state("walk");
    const uint32_t run = state("run");
    const uint32_t jump = state("jump");
    const uint32_t fall = state("fall");
    const uint32_t land = state("land");

    // `findState` answers `kAnyState` for a name this rig does not have, which is the same
    // value `AnimationTransition::from` uses to mean *out of everything* -- so writing one
    // straight into a transition turns a missing state into a wildcard. Every transition
    // between two named states goes through here; the one real wildcard is spelled out below.
    const auto link = [&m](uint32_t from, uint32_t to, std::vector<scene::AnimationCondition> conditions, float fade,
                           bool waitForExit = false) {
        if (from == scene::kAnyState || to == scene::kAnyState || from == to) return;
        m.transitions.push_back({from, to, std::move(conditions), fade, waitForExit});
    };

    // Order is priority, and the jump has to stay first: a trigger fired on the frame the
    // character also crossed a speed threshold should jump, not change gait.
    if (jump != scene::kAnyState) {
        m.transitions.push_back({scene::kAnyState, jump, {{kJump, scene::ConditionTest::Greater, 0.5f}}, 0.1f, false});
    }

    link(jump, fall, {{kAir, scene::ConditionTest::Greater, 0.5f}}, 0.2f, true);
    link(jump, land, {{kAir, scene::ConditionTest::Less, 0.5f}}, 0.15f, true);
    link(fall, land, {{kAir, scene::ConditionTest::Less, 0.5f}}, 0.1f);

    // Enumerated over the three grounded gaits rather than written as a wildcard: a
    // wildcard also holds one step after the launch, and cuts the clip the two transitions
    // above exist to let finish.
    for (const uint32_t from : {idle, walk, run}) {
        link(from, fall, {{kAir, scene::ConditionTest::Greater, 0.5f}}, 0.15f);
    }

    // Every one of these carries `airborne < 0.5` as well as its speed band; drop it and a
    // character changes gait in mid-air.
    const auto gait = [&](uint32_t from, float fade, bool waitForExit) {
        link(from, run, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Greater, 0.66f}}, fade,
             waitForExit);
        link(from, walk,
             {{kAir, scene::ConditionTest::Less, 0.5f},
              {kSpeed, scene::ConditionTest::Greater, 0.2f},
              {kSpeed, scene::ConditionTest::Less, 0.66f}},
             fade, waitForExit);
        link(from, idle, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Less, 0.2f}}, fade,
             waitForExit);
    };
    gait(idle, 0.25f, false);
    gait(walk, 0.25f, false);
    gait(run, 0.25f, false);

    // Deliberately not `gait(land, ...)`: `hard landing` is two seconds, and only the
    // standing case waits it out. Making the moving exits wait roots a character for two
    // seconds because they touched the floor.
    link(land, run, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Greater, 0.66f}}, 0.15f);
    link(land, walk,
         {{kAir, scene::ConditionTest::Less, 0.5f},
          {kSpeed, scene::ConditionTest::Greater, 0.2f},
          {kSpeed, scene::ConditionTest::Less, 0.66f}},
         0.15f);
    link(land, idle, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Less, 0.2f}}, 0.25f, true);

    // Must stay last, and only fires on a rig missing `fall` or `land`: without these a
    // character that got into `fall` or `jump` on such a rig has no way out.
    gait(fall, 0.2f, false);
    gait(jump, 0.25f, true);

    std::string names;
    for (const std::string& n : found) names += (names.empty() ? "" : ", ") + n;
    core::Logger::status(core::LogCategory::Scene, "Animation: state machine over %zu states (%s)", m.states.size(),
                   names.c_str());
    return m;
}

/**
 * @brief Drive every character's state machine parameters for the frame.
 *
 * A function of the step index, not of input: the golden images need frame N to be the
 * same image on every run. Characters are offset from each other by their own index --
 * five identical characters mid-transition look like one, and no cross-fade is visible.
 */
void driveCharacters(anim::SceneAnimator& animator, uint64_t stepIndex, float step) {
    const uint32_t speed = animator.stateMachine().findParameter("speed");
    if (speed == scene::kAnyState) return;
    const uint32_t jump = animator.stateMachine().findParameter("jump");

    constexpr float kCycle = 9.0f;   ///< seconds for one idle-walk-run-walk-idle round
    constexpr float kStagger = 1.6f; ///< how far apart in that cycle two neighbours sit
    const float now = static_cast<float>(stepIndex) * step;

    for (uint32_t c = 0; c < animator.characterCount(); ++c) {
        const float phase = std::fmod(now + static_cast<float>(c) * kStagger, kCycle) / kCycle;
        // Triangular rather than sinusoidal: a sine spends most of its time near the
        // extremes, which is exactly where the machine is *not* transitioning.
        animator.setParameter(animator.characterAt(animator.characterCount() > 0 ? c : 0), speed,
                              phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f);

        // Fired on the edge rather than held: a trigger the machine consumes is the only
        // thing separating "jump once" from "jump forever".
        if (jump != scene::kAnyState) {
            const float previous = std::fmod(now - step + static_cast<float>(c) * kStagger, kCycle) / kCycle;
            if (phase >= 0.9f && previous < 0.9f) animator.fire(animator.characterAt(c), jump);
        }
    }
}

/// One frame of "what did the user ask for". Every branch is `pressed`, which is an
/// edge: held keys do not re-fire, and a key tapped between two frames still counts.
void applyActions(Engine& e, const AppActions& a, bool& inspectorOpen) {
    auto s = core::Profiler::scope("applyActions");
    const core::input::InputMap& in = e.input();
    gfx::Renderer& r = e.renderer();

    if (in.pressed(a.quit)) e.requestQuit();

    // Through the settings table, never `setSampleCount`. Assigning the renderer directly
    // still works and is the bug: the table would say 4 while the image was 8.
    static const uint32_t kSampleCounts[] = {1, 2, 4, 8};
    for (uint32_t i = 0; i < 4; ++i) {
        if (in.pressed(a.msaa[i])) (void)e.settingsTable().set(core::options::render::msaaSamples, kSampleCounts[i]);
    }

    for (uint32_t i = 0; i < 5; ++i) {
        if (!in.pressed(a.view[i])) continue;
        const auto view = static_cast<core::DebugView>(i);
        r.setDebugView(view);
        core::Logger::status(core::LogCategory::Render, "Debug view: %s", debugViewName(view));
    }

    if (in.pressed(a.viewCycle)) {
        r.cycleDebugView(+1);
        core::Logger::status(core::LogCategory::Render, "Debug view: %s", debugViewName(r.currentDebugView()));
    }

    if (in.pressed(a.overlay)) {
        r.debugOverlay = !r.debugOverlay;
        core::Logger::status(core::LogCategory::Render, "Frame stats overlay: %s", r.debugOverlay ? "on" : "off");
    }

    // Each of these flips a specialisation constant, so it recompiles the pipelines that
    // read it: one hitched frame per press, and the disabled feature then costs nothing
    // rather than costing a branch.
    const auto flip = [&](core::input::ActionId id, bool& flag, const char* name) {
        if (!in.pressed(id)) return;
        flag = !flag;
        core::Logger::status(core::LogCategory::Render, "%s: %s", name, flag ? "on" : "off");
    };
    flip(a.ssao, r.ssaoEnabled, "SSAO");
    flip(a.bloom, r.bloomEnabled, "Bloom");
    flip(a.culling, r.cullingEnabled, "GPU frustum culling");
    flip(a.edgeMsaa, r.edgeMsaaEnabled, "Edge-detect hybrid MSAA");
    // The ray-traced three are gated on `rayQuerySupported && accel.valid()` inside the
    // renderer, so the plain `flip` above announces "on" while the specialisation constant
    // stays false on a device without the extensions. Use this one for them.
    const auto flipRt = [&](core::input::ActionId id, bool& flag, const char* name) {
        if (!in.pressed(id)) return;
        flag = !flag;
        if (!r.rayTracingAvailable()) {
            core::Logger::status(core::LogCategory::Render, "%s: %s (inert -- no ray query on this device or scene)", name,
                           flag ? "on" : "off");
            return;
        }
        core::Logger::status(core::LogCategory::Render, "%s: %s", name, flag ? "on" : "off");
    };
    flipRt(a.rt, r.rtEnabled, "Ray tracing");
    flipRt(a.rtShadowMask, r.rtShadowMaskEnabled, "Per-fragment shadow mask");
    flip(a.ssr, r.ssrEnabled, "SSR");
    flip(a.fog, r.fogEnabled, "Volumetric fog");
    flip(a.taa, r.taaEnabled, "TAA");
    flip(a.particles, r.particlesEnabled, "Particles");
    // These three are not specialisation constants, so they cost no pipeline rebuild.
    flip(a.physicsDebug, e.physicsDebugDraw, "Physics debug draw");
    flip(a.audioDebug, e.audioDebugDraw, "Audio debug draw");
    flip(a.inspector, inspectorOpen, "Inspector");

    // Not a plain flag, because the engine reads it: while a panel is open the binding
    // menu is not run at all, so two things cannot own one keyboard.
    if (in.pressed(a.panel)) {
        e.setUiVisible(!e.uiVisible());
        core::Logger::status(core::LogCategory::Render, "Settings panel: %s", e.uiVisible() ? "on" : "off");
    }

    if (in.pressed(a.audioMute)) {
        // Muted rather than stopped, so a mute held across a minute of a streamed track
        // resumes where the track actually is rather than where it was silenced.
        e.audio().setMuted(!e.audio().muted());
        core::Logger::status(core::LogCategory::Audio, "Audio: %s", e.audio().muted() ? "muted" : "unmuted");
    }

    if (in.pressed(a.tonemap)) {
        // Cycles rather than toggles: TONEMAP_OPERATOR is a selector, not a flag. Onto the
        // renderer's field and not `GameSetup::tonemap`, so nothing here persists.
        const auto next = static_cast<core::TonemapOperator>((static_cast<uint32_t>(r.tonemapOperator) + 1) %
                                                            static_cast<uint32_t>(core::TonemapOperator::Count));
        r.tonemapOperator = next;
        core::Logger::status(core::LogCategory::Render, "Tonemap: %s", core::tonemapKey(next));
    }

    if (in.pressed(a.screenshot)) r.requestCapture(nextCapturePath());

    if (in.pressed(a.record)) {
        if (r.recording()) {
            const std::filesystem::path written = e.stopRecording();
            core::Logger::status(core::LogCategory::Render, "Record: stopped (%s)",
                                 written.empty() ? "no file" : written.string().c_str());
        } else if (e.startRecording()) {
            core::Logger::status(core::LogCategory::Render, "Record: started");
        }
    }

    if (in.pressed(a.renderDoc)) {
        // The next whole frame, not this one: RenderDoc delimits captures by present,
        // and the frame this keypress lands in is already part-recorded.
        gfx::renderDocTrigger(1);
    }

    if (in.pressed(a.dumpProfile)) e.dumpProfile();

    if (in.pressed(a.save)) (void)e.saveGame("debug_frames/demo.sav");
    if (in.pressed(a.load)) (void)e.loadGame("debug_frames/demo.sav");
}

/// One frame of the settings panel. Reads top to bottom, which is also how it draws.
void drawSettingsPanel(ui::Context& ui, Engine& e, PanelState& state, const glm::vec2& pos, const glm::vec2& size) {
    if (!ui.beginPanel("Substrate", pos, size)) return;

    gfx::Renderer& r = e.renderer();
    audio::AudioEngine& audio = e.audio();

    if (state.uiImage.valid()) {
        ui.labelDim("Overlay image (C5)");
        // Three rows tall rather than one: at a row's height the 64x64 source is minified
        // past the point where a wrong sampler or blend is visible rather than plausible.
        ui.image(state.uiImage, 48.0f, 1.0f);
        ui.separator();
    }

    // Every render setting there is, generated from the table: the names and ranges are
    // already declared in `SUBSTRATE_SETTINGS`, and writing them out here is a fifth
    // spelling free to drift from the other four.
    ui.labelDim("Rendering");
    if (ui.button("Save settings")) {
        // Empty means no config file was found, and an unwritten config already means
        // defaults.
        const std::string path = e.config().sourcePath.empty() ? std::string("substrate.json")
                                                               : e.config().sourcePath.string();
        state.saveStatus = e.settingsTable().saveJson(path) ? "saved to " + path : "save failed: see the log";
    }
    if (!state.saveStatus.empty()) ui.labelDim(state.saveStatus);
    ui.separator();
    ui::drawSettings(ui, e.settingsTable(), "render");

    ui.separator();
    // `ui::Context` has no disabled state, so an unavailable feature has to be a dim line
    // rather than a control that moves and changes nothing.
    if (!r.rayTracingAvailable()) {
        ui.labelDim("Ray tracing unavailable on this device");
        ui.separator();
    }

    ui.labelDim("Exposure and light");
    ui.slider("Exposure", r.exposure, 0.1f, 4.0f);
    ui.slider("Sun", r.sunIntensity, 0.0f, 10.0f);

    ui.separator();
    ui.labelDim("Debug view");
    if (ui.list("Views", state.debugViewNames, state.debugView, ui.scaled().rowHeight * 5.0f)) {
        r.setDebugView(static_cast<core::DebugView>(state.debugView));
    }

    ui.separator();
    ui.labelDim("Debug draw");
    ui.checkbox("Physics wireframe", e.physicsDebugDraw);
    if (audio.active() && !audio.empty()) {
        ui.checkbox("Audio lines", e.audioDebugDraw);
        bool muted = audio.muted();
        if (ui.checkbox("Mute", muted)) audio.setMuted(muted);
        for (uint32_t bus = 0; bus < audio.busCount(); ++bus) {
            char line[64];
            std::snprintf(line, sizeof(line), "%-10s %.2f", audio.busName(bus).c_str(),
                          static_cast<double>(audio.busGain(bus)));
            // A ducking bus moves its own gain, so this row has to be a readout: made a
            // control, it would fight the ducking every frame.
            ui.labelDim(line);
        }
    }

    ui.separator();
    ui.labelDim("Demo");
    ui::drawSettings(ui, e.settingsTable(), "demo");

    ui.separator();
    ui.labelDim("Capture");
    ui.textField("File", state.capturePath);
    ui.beginRow(2);
    if (ui.button("Screenshot")) r.requestCapture(state.capturePath);
    if (ui.button("Dump profile")) e.dumpProfile();
    ui.endRow();

    ui.endPanel();
}

} // namespace

void AppActions::declare(core::input::InputMap& map) {
    quit = map.declare("App.Quit", "Escape");

    msaa[0] = map.declare("Msaa.1x", "Num1");
    msaa[1] = map.declare("Msaa.2x", "Num2");
    msaa[2] = map.declare("Msaa.4x", "Num4");
    msaa[3] = map.declare("Msaa.8x", "Num8");

    view[0] = map.declare("View.Lit", "F1");
    view[1] = map.declare("View.Albedo", "F2");
    view[2] = map.declare("View.Normal", "F3");
    view[3] = map.declare("View.Orm", "F4");
    view[4] = map.declare("View.Depth", "F5");
    viewCycle = map.declare("View.Cycle", "GraveAccent");

    overlay = map.declare("Toggle.Overlay", "F6");
    ssao = map.declare("Toggle.Ssao", "F8");
    // Bloom keeps F10 rather than sliding up into the vacated F9, so bindings files
    // written before this still mean what they say.
    rtShadowMask = map.declare("Toggle.RtShadowMask", "F9");
    bloom = map.declare("Toggle.Bloom", "F10");
    tonemap = map.declare("Toggle.Tonemap", "F11");
    culling = map.declare("Toggle.Culling", "C");
    edgeMsaa = map.declare("Toggle.EdgeMsaa", "M");
    rt = map.declare("Toggle.Rt", "Y");
    ssr = map.declare("Toggle.Ssr", "R");
    fog = map.declare("Toggle.Fog", "G");
    taa = map.declare("Toggle.Taa", "T");
    particles = map.declare("Toggle.Particles", "N");
    physicsDebug = map.declare("Toggle.PhysicsDebug", "B");
    audioMute = map.declare("Toggle.AudioMute", "J");
    audioDebug = map.declare("Toggle.AudioDebug", "K");
    panel = map.declare("Toggle.Panel", "I");
    inspector = map.declare("Toggle.Inspector", "O");

    screenshot = map.declare("Capture.Screenshot", "F12");
    renderDoc = map.declare("Capture.RenderDoc", "PrintScreen");
    // Punctuation: all twenty-six letters are already spoken for by the table above, the
    // camera or the player. `InputMap::conflicts` reports a reused key at startup.
    record = map.declare("Capture.Record", "Period");
    dumpProfile = map.declare("Debug.DumpProfile", "P");
    navGo = map.declare("Nav.GoTo", "V");
    streamScene = map.declare("Scene.Stream", "X");
    addModel = map.declare("Scene.AddModel", "Z");
    // Punctuation for the same reason. Land one of these on a letter an older toggle
    // already holds and neither shadows the other -- one press runs both, which here means
    // a rig import on every physics-wireframe toggle.
    addRig = map.declare("Scene.AddRig", "Semicolon");
    dropModel = map.declare("Scene.DropModel", "H");
    carry = map.declare("Scene.Carry", "U");
    fetch = map.declare("Scene.Fetch", "Comma");
    spawnCube = map.declare("Scene.SpawnCube", "F");
    ridePlatform = map.declare("Scene.RidePlatform", "Slash");
    save = map.declare("Game.Save", "F7");
    load = map.declare("Game.Load", "L");
    // `Tab` reads better and is the binding menu's.
    cameraToggle = map.declare("App.Camera", "Apostrophe");
}

void PlayerActions::declare(core::input::InputMap& map) {
    forward = map.declare("Player.Forward", "W Pad.LeftY-");
    back = map.declare("Player.Back", "S Pad.LeftY+");
    left = map.declare("Player.Left", "A Pad.LeftX-");
    right = map.declare("Player.Right", "D Pad.LeftX+");
    run = map.declare("Player.Run", "LeftShift Pad.RightBumper");
    jump = map.declare("Player.Jump", "Space Pad.A");
}

/// What a key asks for when the run modifier is not held, as a fraction of the
/// character's top speed. The two numbers it has to sit between are the state machine's:
/// above 0.2 or the machine calls it standing still, below 0.66 or it calls it a run. At
/// the showcase character's 3.2 m/s this is 1.44 m/s, which normalises to 0.45 -- the
/// middle of that band rather than the edge of it, because an assertion on a threshold is
/// a question about float accumulation.
constexpr float kWalkFraction = 0.45f;

/// How fast the character turns to face where it is going, in radians per second. Slewed
/// rather than snapped because a snap is a mesh that flips through 180 degrees in one step
/// on a direction reversal, and the character reverses whenever the camera swings past it.
/// A quarter-turn takes about a tenth of a second at this rate, which is quick enough that
/// the walk still reads as going where it points.
constexpr float kTurnRate = 16.0f;

/// The speed below which the character's motion is not a heading, in metres per second. A
/// twentieth of the showcase character's 3.2 m/s of travel, which is well under anything a
/// key or a stick can ask for and well over the millimetre a settling capsule moves.
constexpr float kFacingFloor = 0.16f;

/// Close enough to the torch to take hold of it, in metres. An arm's length plus the
/// capsule's radius, so `near_torch` becomes true at about the point a player would press
/// `U` — and comfortably wider than a step at walking pace, or the character would arrive,
/// overshoot and re-plan forever.
constexpr float kReachDistance = 1.1f;

namespace {
/// The shortest signed way round from `from` to `to`, in radians. Two headings a degree
/// apart across the +/-pi seam differ by a degree, not by a full turn, and a slew that took
/// the long way there is a character that spins on the spot to face where it already is.
[[nodiscard]] float shortestTurn(float from, float to) {
    float delta = std::fmod(to - from + glm::pi<float>(), glm::two_pi<float>());
    if (delta < 0.0f) delta += glm::two_pi<float>();
    return delta - glm::pi<float>();
}
} // namespace

glm::vec3 PlayerActions::moveDirection(const core::input::InputMap& in, const scene::Camera& camera) const {
    // Derived from the camera's own vectors, never restated as a yaw expression. A
    // hand-written `(sin yaw, 0, -cos yaw)` agrees with `Camera::forward()` only where
    // `cos yaw` is zero, and `frameBounds` picks exactly that yaw whenever X is the longer
    // horizontal axis -- so the default scene is the one place a 180-degree heading error
    // cancels and no check in the tree can see it.
    const glm::vec3 view = camera.forward();
    const glm::vec3 flat(view.x, 0.0f, view.z);
    const float reach = glm::length(flat);
    // A camera looking straight down has no horizontal forward to resolve against. It
    // cannot happen through `Camera::update`, whose pitch is clamped short of the pole, but
    // `--camera` writes the four numbers directly and a game may too.
    if (reach <= 1e-4f) return glm::vec3(0.0f);
    const glm::vec3 ahead = flat / reach;
    // Screen-right, by the same cross product `Camera::update` takes and in the same order.
    const glm::vec3 side = glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f));
    // `value` rather than `held`, which is what makes a stick a stick: a pad axis carries
    // how far it was pushed, so half a deflection is half a request. A key contributes its
    // full value, which is why the keyboard needs `run` as a modifier and a pad does not.
    glm::vec3 d = ahead * (in.value(forward) - in.value(back)) + side * (in.value(right) - in.value(left));
    const float length = glm::length(d);
    if (length <= 1e-4f) return glm::vec3(0.0f);

    // Two diagonal keys sum to a length of 1.41 and would ask for 40% more speed than one
    // key does, so the length is clamped rather than normalised: normalising would push a
    // stick at rest-plus-a-nudge back out to full travel.
    const float fraction = std::min(length, 1.0f) * (in.held(run) ? 1.0f : kWalkFraction);
    return d / length * fraction;
}

void DemoGame::declareSettings(core::settings::Settings& settings) {
    // Runs before `substrate.json` is read, which is why it is separate from `configure`.
    // The file is walked once, so a row declared any later has its key reported as a typo
    // and keeps its built-in whatever the user wrote.
    impactVolume = settings.declare("demo.impactVolume", 1.0f, "Impact volume", 0.0f, 1.0f);
    impactDust = settings.declare("demo.impactDust", true, "Impact dust");

    panelX = settings.declare("demo.panelX", 16.0f, "Panel x", 0.0f, 4096.0f);
    panelY = settings.declare("demo.panelY", 16.0f, "Panel y", 0.0f, 4096.0f);
    panelWidth = settings.declare("demo.panelWidth", 300.0f, "Panel width", 80.0f, 4096.0f);
    panelHeight = settings.declare("demo.panelHeight", 560.0f, "Panel height", 80.0f, 4096.0f);
}

void DemoGame::configure(GameSetup& setup, core::settings::Settings& /*settings*/) {
    setup.name = "Substrate";

    // The only directional light here, so it is the one `initLights` promotes to the sun.
    setup.look.lights = {gfx::makeDirectionalLight({-0.35f, 0.85f, 0.4f}, {1.0f, 0.96f, 0.88f}, 3.0f)};
    // Sandstone, not sky: Sponza is a stone courtyard, so the light bouncing around its
    // arcades is the colour of the stone that bounced it. A colour rather than a
    // brightness for that reason.
    setup.look.ambientColor = {0.0025f, 0.0021f, 0.0016f};
    setup.look.exposure = 1.0f;

    setup.audio.buses = {
        {"music", 1.0f, "sfx", 0.45f, 0.05f, 0.4f},
        {"sfx", 1.0f, "", 1.0f, 0.05f, 0.4f},
        {"ambience", 1.0f, "sfx", 0.7f, 0.05f, 0.4f},
    };
}

void DemoGame::init(Engine& e) {
    // The building imports first: everything below reads the bounds it establishes.
    //
    // The scale is stated before the import rather than written into the node's transform,
    // because it has to reach the props this game imports next -- and because a scale in a
    // placement matrix stretches a rig.
    e.setWorldScale(kDemoWorldScale);
    const scene::NodeId building = e.scene().create("sponza");
    e.scene().add<scene::Model>(building, {core::Resources("res:/Sponza/glTF/Sponza.gltf")});

    // `Engine::run` applies the config's rebinds immediately after this returns, so every
    // action declared here is one `substrate.json` can name.
    actions.declare(e.input());

    playerActions.declare(e.input());

    // `setBindings` and `setDefaultBindings` both move the live list and only the second
    // moves the declared default with it, so `isDefault` is the only thing that tells a
    // shipped binding from a rebound one. The `*` marks the latter.
    {
        std::string line;
        for (const char* name : {"Player.Forward", "Player.Back", "Player.Left", "Player.Right", "Player.Run",
                                 "Player.Jump"}) {
            const core::input::ActionId id = e.input().find(name);
            line += (line.empty() ? "" : "  ") + std::string(name) + "=" + e.input().bindingList(id) +
                    (e.input().isDefault(id) ? "" : "*");
        }
        core::Logger::status(core::LogCategory::Core, "Shipped bindings: %s", line.c_str());
    }

    // No fallback light placement here, because `buildDemoWorld` authors the interior's
    // whole light set. A fallback set is *added to* rather than overwritten by it: the
    // interior ends up lit by two key sets at once, the aisles meant to stay dark read as
    // fully lit, and the atlas needs 23 layers for a scene that has room for 22.

    // The costs below are in metres, roughly, so a third action added later can be compared
    // against these without a unit conversion nobody would remember to make.
    propNearTorch = planner.declare("near_torch");
    propCarrying = planner.declare("carrying_torch");
    {
        ai::Action walk;
        walk.name = "walk to the torch";
        walk.effects.set(propNearTorch, true);
        walk.cost = 4.0f;
        actWalkToTorch = planner.add(walk);

        ai::Action pick;
        pick.name = "pick up the torch";
        pick.prerequisites.set(propNearTorch, true);
        pick.effects.set(propCarrying, true);
        pick.cost = 1.0f;
        actPickUpTorch = planner.add(pick);
    }

    // Registered unconditionally: nothing is compiled until a draw carrying this index
    // reaches the G-buffer pass, so a run that never presses F pays a struct in a vector.
    {
        gfx::ShaderVariant hologram;
        hologram.name = "hologram";
        hologram.fragmentShader = "hologram.frag";
        // ENABLE_SCANLINES, at constant_id 8 -- the first id above the engine's reserved
        // block.
        hologram.constants = {1u};
        hologramVariant = e.renderer().addShaderVariant(std::move(hologram));
    }

    // Sits after the lights the scene decided, and builds only into the scene `configure`
    // named, so eleven golden cases running this same binary see nothing new.
    buildDemoWorld(e, world);

    // Only where the world built nobody: a scene that authors a `Character` collider --
    // `physics.gltf` among the golden eleven -- would otherwise have no one to drive.
    if (world.players.empty()) {
        for (const Engine::AuthoredCharacter& c : e.authoredCharacters()) {
            world.players.push_back({c.character, c.node, {}});
        }
    }

    // The torch carries a light that already exists rather than adding one; an extra light
    // is an extra light in every golden image.
    //
    // After the world, because the world is what lights this scene and the index is
    // therefore the world's to name. The `1` fallback is only a warm fill on a scene where
    // the engine's automatic placement ran first.
    {
        const uint32_t light = world.torchLight != DemoWorld::kNoLight ? world.torchLight : 1u;
        if (e.renderer().lights.size() > light) {
            torchLight = light;
            torchNode = e.scene().create("torch");
            e.scene().attachLight(torchNode, torchLight);
            // Seeded from where the light already is, so the first sweep writes it back
            // unchanged. Left at the origin, the node moves the light on frame one.
            e.scene().setLocalPosition(torchNode, glm::vec3(e.renderer().lights[torchLight].position));
        }
    }

    // After the world, because the machine is built from the clips the rig carries and the
    // rig arrives with the import above. Run before the character exists, it finds no clips,
    // builds nothing, and every arm of `scripts/locomotion.sh` fails on a missing machine.
    e.animator().setStateMachine(locomotionMachine(e.animator()));

    inspectorOpen = e.config().ui.inspector;
    panelState.capturePath = e.config().benchmark.capturePath;
    for (uint32_t i = 0; i < static_cast<uint32_t>(core::DebugView::Count); ++i) {
        panelState.debugViewNames.emplace_back(debugViewName(static_cast<core::DebugView>(i)));
    }
    panelState.debugView = static_cast<uint32_t>(e.renderer().currentDebugView());
    // A package built from a narrower `package.txt` may not carry this, which is why the
    // panel checks before drawing rather than this aborting.
    panelState.uiImage = e.images().load("res:/ui_test.png");

    // Checked for the same reason: a package built without it should be a demo without a
    // thud rather than a warning on every collision.
    {
        const core::Resources file("res:/audio/impact.wav");
        if (file.found()) {
            impactSound.name = "impact";
            impactSound.file = file.string();
            impactSound.bus = "sfx";
            impactSound.spatial = true;
            impactSound.minDistance = 2.0f;
            impactSound.maxDistance = 60.0f;
            // The occlusion filter is inserted at load, so leaving it on costs every shot
            // a biquad for a quarter of a second that ends before anyone could walk behind
            // a wall.
            impactSound.occlusion = false;
        }
    }

    // The last thing `init` does that touches input: installing a controller is what makes
    // its rows exist, and `Engine::run` applies the config's rebinds the moment this
    // returns. It also has to follow `buildDemoWorld` and the authored-character adoption,
    // which is what it asks about.
    //
    // The follow target is the demo's own player node and only that. `golden.sh` runs this
    // binary against eleven scenes, one of which authors a character, and a camera that
    // re-aimed itself there would move a baseline for a reason unrelated to the renderer.
    if (world.built && world.playerNode().valid()) followCamera.follow(&e.scene(), world.playerNode());
    installCamera(e, !world.playerCharacter().valid());

    // Move a binding above and this hint moves with it: a controls line naming the keys a
    // previous version used is worse than none, because a reader trusts it.
    core::Logger::status(core::LogCategory::Core, "Controls: drag=orbit scroll=zoom %s, %s swaps them",
                         flying ? "-- free-fly camera, WASD/QE=fly shift=fast"
                                : "-- the camera follows the character",
                         e.input().bindingList(actions.cameraToggle).c_str());
    core::Logger::status(core::LogCategory::Core,
                   "          %s opens the binding menu -- every action, its keys, and a rebind",
                   e.input().bindingList(e.input().find("Menu.Bindings")).c_str());
    core::Logger::status(core::LogCategory::Core, "          1/2/4/8=MSAA  F1..F5=debug view  F6=overlay  P=dump  Esc=quit");
    core::Logger::status(core::LogCategory::Core,
                   "          F8=SSAO F9=shadow mask F10=bloom F11=tonemap (each rebuilds its pipeline)");
    core::Logger::status(core::LogCategory::Core, "          F12=screenshot  .=record  `=cycle debug view  M=hybrid MSAA  C=cull  T=TAA  R=SSR  G=fog  N=particles  Y=RT reflections");
    if (!e.physics().empty()) {
        core::Logger::status(core::LogCategory::Core, "          B=physics wireframe%s",
                       world.playerCharacter().valid() ? "  WASD=walk  shift=run  space=jump (while the camera follows)"
                                                       : "");
    }
    core::Logger::status(core::LogCategory::Core, "          F=build a cube in code (G4)  Z=append a model  ;=append a rig  H=drop it  ,=fetch");
    if (torchNode.valid() && world.playerNode().valid()) {
        core::Logger::status(core::LogCategory::Core, "          U=pick up the torch (G3: reparenting)");
    }
    if (e.audio().active() && !e.audio().empty()) {
        core::Logger::status(core::LogCategory::Core, "          J=mute  K=audio lines (green heard, red occluded)");
    }
}

void DemoGame::installCamera(Engine& e, bool fly) {
    flying = fly;
    scene::Camera& from = e.camera();
    scene::Camera& to = fly ? static_cast<scene::Camera&>(flyCamera) : followCamera;
    // The pose does not come with the controller: `Engine::setCamera` installs a pointer
    // and copies nothing, so a swap without this drops the view to focus zero, distance
    // five, looking down +Z. The projection scalars have to come too -- `frameBounds` sized
    // the near plane to the scene and neither it nor the fov is re-applied after startup.
    if (&from != &to) {
        to.focus = from.focus;
        to.distance = from.distance;
        to.yaw = from.yaw;
        to.pitch = from.pitch;
        to.nearPlane = from.nearPlane;
        to.fovYRadians = from.fovYRadians;
    }
    // The three `camera.*` feel rows belong to the controller and the engine holds only a
    // `Camera&`, so re-applying them is the game's job on every install rather than once.
    if (fly) {
        flyCamera.applySettings(e.settingsTable());
    } else {
        followCamera.applySettings(e.settingsTable());
    }
    e.setCamera(&to);

    // The same walk `ui::BindingMenu` does over `actionCount()`/`actionLive()`, so this
    // reports the rebind menu's actual contents after the swap.
    {
        std::string rows;
        for (core::input::ActionId id = 0; id < e.input().actionCount(); ++id) {
            if (!e.input().actionLive(id) || e.input().actionName(id).rfind("Camera.", 0) != 0) continue;
            rows += (rows.empty() ? "" : "  ") + e.input().actionName(id) + "=" + e.input().bindingList(id) +
                    (e.input().isDefault(id) ? "" : "*");
        }
        core::Logger::status(core::LogCategory::Core, "Shipped camera (%s): %s", fly ? "fly" : "follow",
                             rows.c_str());
    }
}

void DemoGame::frameUpdate(Engine& e, float /*dt*/) {
    applyActions(e, actions, inspectorOpen);

    if (e.input().pressed(actions.cameraToggle)) {
        core::Logger::status(core::LogCategory::Core, "Camera: %s",
                             flying ? "following the character" : "free-fly -- the character is not being driven");
        installCamera(e, !flying);
    }

    if (e.input().pressed(actions.addModel)) {
        // Placed where the camera is looking, so successive presses land in different
        // places instead of stacking one file on top of itself at the origin.
        const glm::vec3 at = e.camera().position() + e.camera().forward() * 6.0f;
        const auto id = e.addModel(core::Resources("res:/physics.gltf"), glm::translate(glm::mat4(1.0f), at));
        if (id != scene::GltfScene::kNoModel) loadedModels.push_back(id);
    }
    // A skinned file's skeleton, clips and influences are merged onto the rig the scene is
    // already animating rather than replacing it.
    if (e.input().pressed(actions.addRig)) {
        const glm::vec3 at = e.camera().position() + e.camera().forward() * 4.0f;
        const auto id = e.addModel(core::Resources("res:/character.gltf"), glm::translate(glm::mat4(1.0f), at));
        if (id != scene::GltfScene::kNoModel) {
            loadedModels.push_back(id);
            const scene::GltfScene::LoadedModel& m = e.gltfScene().model(id);
            core::Logger::status(core::LogCategory::Scene,
                                 "Imported rig: %u skins from skin %u, %zu characters, %zu clips now",
                                 m.skinCount, m.skinBase, static_cast<size_t>(e.animator().characterCount()),
                                 e.animator().clipCount());
        }
    }

    if (e.input().pressed(actions.dropModel) && !loadedModels.empty()) {
        e.removeModel(loadedModels.back());
        loadedModels.pop_back();
    }

    // `scripts/locomotion.sh`'s platform arm scripts this press; the platform is the only
    // surface in the scene a character cannot reach on foot.
    //
    // The body's transform rather than the node's, because the body is what the character
    // stands on; 0.11 is the platform's own `halfExtent.y`, so the feet arrive on the
    // surface rather than a fifth of a metre inside it.
    if (e.input().pressed(actions.ridePlatform) && world.platform.valid()) {
        const scene::PhysicsCharacterId rider = world.playerCharacter();
        if (rider.valid()) {
            glm::vec3 top(e.physics().bodyTransform(world.platform, 0.0f)[3]);
            top.y += 0.11f;
            e.physics().setCharacterTransform(rider, glm::translate(glm::mat4(1.0f), top));
            locomotion.placed = true;
            core::Logger::status(core::LogCategory::Scene, "Placed the character on the platform at %.2f %.2f %.2f",
                                 static_cast<double>(top.x), static_cast<double>(top.y),
                                 static_cast<double>(top.z));
        }
    }

    // Placed where the camera is looking, one metre up, so it lands somewhere visible in
    // any scene.
    if (e.input().pressed(actions.spawnCube)) {
        if (cubeMaterial == 0xFFFFFFFFu) {
            scene::GpuMaterial m{};
            m.baseColorFactor = {0.9f, 0.3f, 0.2f, 1.0f};
            m.metallicFactor = 0.0f;
            m.roughnessFactor = 0.4f;
            m.baseColorTexture = -1;
            m.metallicRoughnessTexture = -1;
            m.normalTexture = -1;
            m.occlusionTexture = -1;
            m.emissiveTexture = -1;
            m.normalScale = 1.0f;
            m.shader = hologramVariant;
            // x band frequency in world units, y phase, z emissive gain, w roughness --
            // matched to `hologram.frag`, which is the only thing that reads them.
            m.params = {6.0f, 0.0f, 2.5f, 0.25f};
            cubeMaterial = e.gltfScene().createMaterial(m);
        }
        if (cubeMaterial != 0xFFFFFFFFu) {
            // Two and a half metres up: dropped from much less it lands below the 1 m/s
            // floor `playImpacts` screens at, and the contact is silent.
            const glm::vec3 where = e.camera().focus + glm::vec3(0.0f, 2.5f, 0.0f);
            const auto id = e.createMesh(unitCube(cubeMaterial, glm::translate(glm::mat4(1.0f), where)));
            if (id != scene::GltfScene::kNoModel) {
                spawnedCubes.push_back(id);

                // A dynamic body attached to a node pushes its transform *up* into the
                // node, and the instance attached below it follows. No offset, because the
                // mesh was built around the origin and placed where the body was.
                scene::ColliderDesc desc;
                desc.name = "spawned cube";
                desc.shape = scene::ColliderShape::Box;
                desc.motion = scene::ColliderMotion::Dynamic;
                desc.halfExtent = glm::vec3(0.5f);
                desc.mass = 4.0f;
                desc.friction = 0.5f;
                // Enough to bounce once, so a single press produces a loud contact and a
                // quiet one and the impact volume scaling is audible.
                desc.restitution = 0.35f;
                desc.transform = glm::translate(glm::mat4(1.0f), where);

                const scene::BodyId body = e.physics().createBody(desc);
                const std::span<const scene::InstanceId> instances = e.instancesOf(id);
                if (body.valid() && !instances.empty()) {
                    const scene::NodeId node = e.scene().create("spawned cube");
                    e.scene().attachBody(node, body);
                    e.scene().attachInstance(node, instances.front());
                    // Saying the instance is dynamic is the caller's job: `createMesh`
                    // builds every instance static, so without this the cube is baked into
                    // the acceleration structure's static tier, then falls out of it, and
                    // `rebuildAccelIfStale` rebuilds the whole structure *every frame* for
                    // the rest of the run.
                    e.instances().setFlags(instances.front(), scene::kInstanceDynamic, 0u);
                }
            }
        }
    }

    // Guarded on a cube existing, so a run that spawns none renders what it always did.
    if (!spawnedCubes.empty() && cubeMaterial != 0xFFFFFFFFu) {
        scene::GpuMaterial m = e.gltfScene().material(cubeMaterial);
        const float t = static_cast<float>(e.renderer().frameCount()) * 0.03f;
        m.baseColorFactor = {0.55f + 0.45f * std::sin(t), 0.3f, 0.55f + 0.45f * std::cos(t), 1.0f};
        // A parameter, not the variant assignment: this costs a memcpy of the material
        // table, where changing `shader` would make the command builder regroup.
        m.params.y = t * 0.5f;
        e.gltfScene().setMaterial(cubeMaterial, m);
    }

    // Reparenting keeps the world transform, so the torch does not jump on the frame it is
    // picked up; the local position below is what then puts it in the character's hand
    // rather than at their feet.
    if (e.input().pressed(actions.carry) && torchNode.valid() && world.playerNode().valid()) {
        carrying = !carrying;
        e.scene().setParent(torchNode, carrying ? world.playerNode() : scene::NodeId{});
        if (carrying) e.scene().setLocalPosition(torchNode, {0.3f, 1.3f, 0.2f});
        core::Logger::status(core::LogCategory::Scene, "Torch: %s", carrying ? "carried" : "put down");
        // Cancelled here rather than left for `advance` to notice, or the planner and the
        // player argue over the same node on the same frame.
        fetching = false;
    }

    if (e.input().pressed(actions.fetch) && torchNode.valid() && world.playerNode().valid()) {
        ai::WorldState goal;
        goal.set(propCarrying, true);
        fetcher.setGoal(goal);
        fetching = true;
        fetchStep = ai::Agent::kNoAction;
        core::Logger::status(core::LogCategory::Scene, "Goal: carry the torch");
    }

    if (e.input().pressed(actions.streamScene)) {
        if (!e.beginLoadScene(e.config().scene.path)) {
            core::Logger::status(core::LogCategory::Scene, "a scene is already streaming");
        } else {
            // The ids belong to the scene being replaced. Holding them across the swap and
            // then pressing `H` would ask the engine to drop a model of the *new* scene.
            loadedModels.clear();
        }
    }

    // Read once per frame rather than once per step: input arrives at the frame rate,
    // and a key held across two steps in one frame is one press.
    if (world.playerCharacter().valid()) {
        const glm::vec3 here(e.physics().characterTransform(world.playerCharacter(), 0.0f)[3]);

        // Paths to wherever the camera is standing, which in this scene is a few metres
        // behind the character rather than across the room.
        if (e.input().pressed(actions.navGo)) {
            navFollower.reset({});
            const nav::NavMesh& nav = e.navMesh();
            const nav::NavPoint from = nav.nearest(here, 2.0f);
            const nav::NavPoint to = nav.nearest(e.camera().position(), 4.0f);
            std::vector<glm::vec3> path;
            if (!from || !to) {
                core::Logger::status(core::LogCategory::Scene, "Nav: %s is not on the navmesh",
                                     from ? "the camera" : "the character");
            } else if (!nav.findPath(from, to, path)) {
                core::Logger::status(core::LogCategory::Scene, "Nav: no path -- %s",
                                     nav.reachable(from, to) ? "the search failed" : "different regions");
            } else {
                core::Logger::status(core::LogCategory::Scene, "Nav: %zu waypoints over %.1f m", path.size(),
                                     glm::distance(path.front(), path.back()));
                navFollower.reset(std::move(path));
            }
        }

        // After `Camera::update`, never before: the engine runs the look, the zoom and the
        // follow in `beginFrame`, and asking for "forward" ahead of that drives the
        // character off the previous frame's yaw.
        //
        // Not while the flycam is in, which is what lets both schemes ship on WASD. The
        // flycam declares `Camera.Forward` on W the moment it is installed, so asking for a
        // movement direction underneath it puts one press on two things.
        const glm::vec3 manual =
            flying ? glm::vec3(0.0f) : playerActions.moveDirection(e.input(), e.camera());
        if (glm::length(manual) > 1e-4f) navFollower.reset({});

        glm::vec3 move = manual;
        if (!navFollower.done()) {
            const glm::vec3 desired = nav::steer(navFollower, here, 1.0f);
            // The controller wants a direction, not a velocity -- it owns the speed.
            if (glm::length(desired) > 1e-4f) move = glm::normalize(desired);
        }

        // The world state is read off the world rather than tracked alongside it, so a
        // torch a player took by hand is seen the same way one the planner took is.
        if (fetching && torchNode.valid() && world.playerNode().valid()) {
            auto planZone = core::Profiler::scope("DemoGame::fetch");
            const glm::vec3 torch(e.scene().worldTransform(torchNode)[3]);
            const glm::vec3 toTorch = torch - here;
            const glm::vec3 flat(toTorch.x, 0.0f, toTorch.z);

            // Named `state` and not `world`: the block below reads the `world` member, and
            // a local of that name shadows it silently.
            ai::WorldState state;
            state.set(propNearTorch, glm::length(flat) < kReachDistance);
            state.set(propCarrying, carrying);

            const uint32_t step = fetcher.advance(planner, state);
            if (fetcher.replanned() && !fetcher.plan().empty()) {
                std::string route;
                for (const uint32_t a : fetcher.plan()) {
                    route += (route.empty() ? "" : " > ") + planner.action(a).name;
                }
                core::Logger::status(core::LogCategory::Scene, "Plan: carry the torch -- %s", route.c_str());
            }
            if (step != fetchStep) {
                fetchStep = step;
                if (step == ai::Agent::kNoAction) {
                    core::Logger::status(core::LogCategory::Scene, "Plan: done, the torch is carried");
                    fetching = false;
                } else {
                    core::Logger::status(core::LogCategory::Scene, "Plan: %s", planner.action(step).name.c_str());
                }
            }

            if (step == actWalkToTorch && glm::length(manual) <= 1e-4f && glm::length(flat) > 1e-3f) {
                move = glm::normalize(flat);
            } else if (step == actPickUpTorch && !carrying) {
                // The same two calls `Scene.Carry` makes; let them drift apart and a
                // planned pickup puts the torch somewhere a manual one does not.
                carrying = true;
                e.scene().setParent(torchNode, world.playerNode());
                e.scene().setLocalPosition(torchNode, {0.3f, 1.3f, 0.2f});
            }
        }

        // The press goes straight to the controller. Re-deriving the jump outside it, as
        // `pressed && characterOnGround(...)`, disagrees with the controller's own answer:
        // a press inside the coyote window launches with no ground under it, and one inside
        // the buffer launches a step or two after the frame it arrived on.
        e.physics().setCharacterInput(world.playerCharacter(), move, e.input().pressed(playerActions.jump));
    }
}

void DemoGame::playImpacts(Engine& e) {
    auto s = core::Profiler::scope("DemoGame::playImpacts");
    if (impactSound.file.empty() || !e.audio().active()) return;

    // Read per call rather than cached: the generated panel writes them live.
    const float volumeScale = e.settingsTable().get(impactVolume);
    const bool dust = e.settingsTable().get(impactDust);

    for (const physics::PhysicsWorld::Contact& contact : e.physics().contacts()) {
        // A floor is touched constantly by anything resting on it, and a shot per graze is
        // a buzz. One metre a second is roughly a five-centimetre drop, which is the point
        // below which nothing sounds like it landed.
        if (contact.speed < 1.0f) continue;

        scene::AudioSourceDesc one = impactSound;
        // Loudness from the closing speed, saturating at 6 m/s -- a metre and a half of
        // fall. `Contact::speed` is read before the solver resolves the contact; taken
        // afterwards, every impact is equally quiet.
        one.volume = std::min(1.0f, contact.speed / 6.0f) * volumeScale;
        // The pitch has to vary, or a stack of crates settling sounds like a machine gun.
        // Hashed from the pair and the step rather than drawn from a generator, so a locked
        // run stays a function of the frame index and `--capture` still reproduces.
        const uint32_t noise = (contact.a.index * 2654435761u) ^ (contact.b.index * 40503u) ^
                               static_cast<uint32_t>(e.stepIndex());
        one.pitch = 0.9f + 0.2f * static_cast<float>(noise & 0xFFu) / 255.0f;

        e.audio().playAt(one, contact.point);

        if (dust && world.dustReady) e.particles().spawnEffect(world.dust, contact.point, contact.normal);
        // Debug level: a busy scene has several of these a second, and a status line each
        // drowns the log.
        core::Logger::debug(core::LogCategory::Scene, "Impact: bodies %u/%u at %.1f m/s, %.2f %.2f %.2f",
                            contact.a.index, contact.b.index, static_cast<double>(contact.speed),
                            static_cast<double>(contact.point.x), static_cast<double>(contact.point.y),
                            static_cast<double>(contact.point.z));
    }
}

void DemoGame::fixedUpdate(Engine& e, float step) {
    // Both of these sit above the animator guard below: a scene with no rig still has
    // collisions and still has a platform, and moving either past the early return makes
    // it a property of whether the file happened to ship a skinned mesh.
    //
    // `fixedUpdate` runs *before* the engine's movers, so these are the previous step's
    // contacts -- one step of latency, and nothing dropped, because every step's contacts
    // are drained by the next step's call to this.
    playImpacts(e);

    stepDemoWorld(e, world, step);

    anim::SceneAnimator& animator = e.animator();
    if (animator.empty()) return;

    // A controller drives the gait from where the character actually is; a scene with an
    // animated mesh and no controller falls back to the step-index triangle wave.
    if (!world.playerCharacter().valid()) {
        driveCharacters(animator, e.stepIndex(), step);
        return;
    }

    driveLocomotion(e, step);
}

void DemoGame::driveLocomotion(Engine& e, float step) {
    auto s = core::Profiler::scope("DemoGame::driveLocomotion");
    anim::SceneAnimator& animator = e.animator();
    const scene::AnimationStateMachine& machine = animator.stateMachine();

    // The showcase clips are not authored in place: `walking` moves `Hips` 1.80 m down its
    // own +Z over the clip, `running` 3.20 m and the strafes 2.88 m sideways. Only
    // `mixamorig:Root` sits still, so naming the rig's topmost joint names the one node
    // with nothing on it.
    //
    // Leave it unnamed and the pose keeps that translation, so the character is moved twice
    // -- once by the controller below and once by the clip -- then snaps back the instant
    // the machine blends to `idle`, whose hips stand still.
    //
    // Set here rather than in `init` because rebuilding the animator under `X` returns this
    // to `kNoNode`. Guarded, because `setRootNode` restarts every character's measurement
    // and calling it every step reports a delta of zero forever.
    if (rootJoint == anim::SceneAnimator::kNoNode) rootJoint = animator.findNode("Hips");
    if (rootJoint != anim::SceneAnimator::kNoNode && animator.rootNode() != rootJoint) {
        animator.setRootNode(rootJoint);
        core::Logger::status(core::LogCategory::Scene, "Root motion: held at node %u (Hips)", rootJoint);
    }

    // Nothing before the first sweep: the numbers below read `characterTransform`, which
    // until then is the transform the file authored rather than the one the solver resolved.
    if (e.stepIndex() == 0) return;

    const scene::PhysicsCharacterId player = world.playerCharacter();
    const bool grounded = e.physics().characterOnGround(player);
    const glm::vec3 at(e.physics().characterTransform(player, 0.0f)[3]);

    const uint32_t state = animator.currentState(world.playerRig());
    if (!locomotion.active) {
        locomotion.active = true;
        locomotion.start = at;
        locomotion.previous = at;
    }
    // A teleport is not travel: `setCharacterTransform` moves the character without the
    // solver moving it, so the step crossing the gap is one displacement the size of the
    // room, and three of the sums below divide by exactly that. `path`, `changes` and
    // `poseDrift` are deliberately not reset -- none of them is a distance.
    if (locomotion.placed) {
        locomotion.placed = false;
        locomotion.start = at;
        locomotion.previous = at;
        locomotion.travelled = 0.0f;
        locomotion.peakRise = 0.0f;
        locomotion.alongCamera = 0.0f;
        locomotion.acrossCamera = 0.0f;
        locomotion.alongFacing = 0.0f;
        locomotion.turned = 0.0f;
        locomotion.aimed = false;
    }

    const glm::vec3 moved = at - locomotion.previous;
    // Horizontal: a check on the full displacement is satisfied by a character that only fell.
    const glm::vec3 flat(moved.x, 0.0f, moved.z);
    const float carried = glm::length(flat);
    locomotion.travelled += carried;
    locomotion.peakRise = std::max(locomotion.peakRise, at.y - locomotion.start.y);
    locomotion.previous = at;

    // The rotation goes on the mesh children, never `playerNode()` itself: the sweep writes
    // `characterTransform` straight into that node's world matrix and drops whatever TRS
    // was there, so a rotation set on it is silently discarded every step.
    //
    // Every child named `mesh`, not the first. `Engine::bindPhysicsToScene` creates one per
    // *placement* bound to the body and a rig is routinely several -- the Mixamo character
    // here is two skinned meshes over one skeleton -- so turning only the head of the list
    // leaves half the character pointing wherever the file authored it.
    //
    // Tested on the front rather than on emptiness alone: re-streaming the scene retires
    // every id in here at once, and a list of stale handles is not empty.
    if (!facingNodes.empty() && !e.scene().valid(facingNodes.front())) facingNodes.clear();
    if (facingNodes.empty() && world.playerNode().valid()) {
        for (scene::NodeId c = e.scene().firstChild(world.playerNode()); c.valid(); c = e.scene().nextSibling(c)) {
            // By name rather than by position in the list, because the torch above is a
            // sibling of these and must not be turned with them.
            if (e.scene().name(c) == "mesh") facingNodes.push_back(c);
        }
    }
    if (world.built && !facingNodes.empty()) {
        // Where it went, not where it was asked to go: turning to face the request is the
        // same mistake as animating a jump off the keypress.
        //
        // `characterVelocity` and not the `flat` above. That displacement includes whatever
        // is carrying the character, so parked on the kinematic platform with nothing
        // pressed it turns to face the way the platform is travelling and swings round
        // again at each end of the run. The solver's velocity has the ground's own motion
        // taken out of it.
        //
        // The zero facing offset is right only because this rig is authored looking down
        // +Z, which is where `turnToward` measures from; a rig authored otherwise passes
        // its own rather than having a sign flipped in here.
        const glm::vec3 gait = e.physics().characterVelocity(player);
        for (const scene::NodeId part : facingNodes) {
            e.scene().turnToward(part, gait, kTurnRate, step, kFacingFloor);
        }
    }

    // Projected onto the same basis the movement was resolved against, so a character that
    // walked somewhere else grows a different sum. The facing below comes back out of the
    // *tree* rather than off the yaw the turn returned, which is what makes it a check on
    // the rotation having reached the scene at all.
    const glm::vec3 view = e.camera().forward();
    const glm::vec3 ahead(view.x, 0.0f, view.z);
    if (const float reach = glm::length(ahead); reach > 1e-4f) {
        const glm::vec3 forward = ahead / reach;
        locomotion.alongCamera += glm::dot(flat, forward);
        locomotion.acrossCamera += std::abs(glm::dot(flat, glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f))));
    }
    // How far the character has ever turned from the heading it began with -- the number a
    // run that should not turn at all is asserted on, because every ratio in this function
    // divides by a world displacement a rider gets for free. See `LocomotionTrace::turned`.
    if (!facingNodes.empty() && e.scene().valid(facingNodes.front())) {
        const glm::vec3 front(e.scene().worldTransform(facingNodes.front())[2]);
        const glm::vec3 aim(front.x, 0.0f, front.z);
        if (glm::length(aim) > 1e-4f) {
            const float yaw = std::atan2(aim.x, aim.z);
            if (!locomotion.aimed) {
                locomotion.startYaw = yaw;
                locomotion.aimed = true;
            }
            // Through the shortest arc, so a character that turned a degree past -pi reads
            // as a degree rather than as most of a revolution.
            locomotion.turned =
                std::max(locomotion.turned, std::abs(shortestTurn(locomotion.startYaw, yaw)));
        }
    }

    // The minimum over every part, never the single node the rotation was written to: read
    // back off the node this function just turned, the number restates the angle just
    // written and cannot fall while that node turns, whatever the rest of the character does.
    float worst = 1.0f;
    bool measured = false;
    for (const scene::NodeId part : facingNodes) {
        if (!e.scene().valid(part)) continue;
        // Column 2 is the node's +Z in world space, which is the axis this rig is authored
        // looking down. As of the last sweep, so one step behind the displacement it is
        // projected onto -- the latency every number in this function carries.
        const glm::vec3 facing(e.scene().worldTransform(part)[2]);
        const glm::vec3 heading(facing.x, 0.0f, facing.z);
        if (const float reach = glm::length(heading); reach > 1e-4f) {
            worst = std::min(worst, glm::dot(flat, heading / reach));
            measured = true;
        }
    }
    // `flat` is a displacement, so the projection is metres and the sum is divided by the
    // path length at the end. A step that went nowhere contributes nothing either way.
    if (measured) locomotion.alongFacing += worst;

    // Read out of the animator's resolved transforms rather than off the clip, so it says
    // the hold *took* rather than that the engine was asked for one. `worldTransforms` is
    // model space, so a held root is a constant here.
    if (rootJoint != anim::SceneAnimator::kNoNode) {
        const std::vector<glm::mat4>& pose = animator.worldTransforms(world.playerRig());
        if (rootJoint < pose.size()) {
            const glm::vec3 here(pose[rootJoint][3]);
            if (!locomotion.posed) {
                locomotion.poseRoot = here;
                locomotion.posed = true;
            }
            // Horizontal. `setRootNode` holds X and Z at the bind translation and
            // deliberately keeps Y -- a rig binds in a T-pose and animates with bent knees,
            // so holding Y stands the character eight centimetres off the floor and
            // flattens the bob out of every walk cycle. Widen this to all three axes and it
            // counts the hips' authored rise and fall as drift: 0.05 m for a walk cycle and
            // 0.50 m through `jumping up`, against a bound of 0.02.
            const glm::vec3 moved = here - locomotion.poseRoot;
            locomotion.poseDrift = std::max(locomotion.poseDrift, glm::length(glm::vec3(moved.x, 0.0f, moved.z)));
        }
    }

    if (state == locomotion.state || state >= machine.states.size()) return;
    const std::string& name = machine.states[state].name;
    const bool entry = locomotion.state >= machine.states.size();
    // The entry state is the head of the path but is not counted as a change; a sequence
    // that did not say where it started is one nobody can read.
    if (!entry) {
        core::Logger::status(core::LogCategory::Scene, "Locomotion: %s -> %s at step %llu (%.2f m/s, %s)",
                             machine.states[locomotion.state].name.c_str(), name.c_str(),
                             static_cast<unsigned long long>(e.stepIndex()),
                             static_cast<double>(e.physics().characterSpeed(player)),
                             grounded ? "grounded" : "airborne");
        ++locomotion.changes;
    }
    locomotion.state = state;
    // Capped: a run nobody is scripting changes state all day, and the summary line this
    // feeds is a claim about a bounded scripted sequence.
    if (locomotion.changes <= 32) locomotion.path += (locomotion.path.empty() ? "" : " > ") + name;
}

void DemoGame::shutdown(Engine& e) {
    // The engine's pointer is non-owning and both cameras are members of this object, so
    // handing it back is what stops a later frame updating freed memory.
    e.setCamera(nullptr);

    // `droppedSpawns()` counts what the pool had no room for since the last `setEmitters`,
    // so it is only meaningful once there is nothing left to change it.
    if (world.built) {
        core::Logger::status(core::LogCategory::Scene, "Demo world: pool %u, %u spawns dropped over %llu steps",
                             e.particles().capacity(), e.particles().droppedSpawns(),
                             static_cast<unsigned long long>(e.stepIndex() + 1));
    }

    // Emitted for a run that pressed nothing too, where it reads `idle` and two zeroes:
    // that is the negative control.
    if (locomotion.active) {
        core::Logger::status(core::LogCategory::Scene, "Locomotion path: %s", locomotion.path.c_str());
        // Appended to, never rewritten: `scripts/locomotion.sh` parses this line with
        // patterns ending in `.*`, so new fields go on the end and older arms keep matching.
        const glm::vec3 net = locomotion.previous - locomotion.start;
        const float scale = locomotion.travelled > 1e-4f ? 1.0f / locomotion.travelled : 0.0f;
        core::Logger::status(core::LogCategory::Scene,
                             "Locomotion: %u changes over %llu steps, %.2f m travelled, peak rise %.2f m, "
                             "net %.2f m, along %.2f across %.2f facing %.2f drift %.2f turned %.2f",
                             locomotion.changes, static_cast<unsigned long long>(e.stepIndex() + 1),
                             static_cast<double>(locomotion.travelled), static_cast<double>(locomotion.peakRise),
                             static_cast<double>(std::sqrt(net.x * net.x + net.z * net.z)),
                             static_cast<double>(locomotion.alongCamera * scale),
                             static_cast<double>(locomotion.acrossCamera * scale),
                             static_cast<double>(locomotion.alongFacing * scale),
                             // Metres, not a ratio: a clip drags the rig the same way
                             // whether the character walked a metre or ten.
                             static_cast<double>(locomotion.poseDrift),
                             // Radians.
                             static_cast<double>(locomotion.turned));
    }

    e.images().destroy(panelState.uiImage);
}

void DemoGame::save(Engine& e, core::SaveWriter& out) {
    (void)e;
    out.beginSection("demo", 1);
    out.u32(++saveCount);
    out.boolean(inspectorOpen);
    out.u32(panelState.debugView);
}

void DemoGame::load(Engine& e, core::SaveReader& in) {
    // The whole section is read before anything is assigned, which is the atomicity
    // `Game::load` leaves to the game: a partial apply on a short or mismatched save is
    // state no later frame can tell from a good one.
    if (!in.section("demo", 1)) {
        core::Logger::warn(core::LogCategory::Core, "demo: %s", in.reason().c_str());
        return;
    }

    const uint32_t count = in.u32();
    const bool inspector = in.boolean();
    const uint32_t view = in.u32();
    if (!in.ok()) {
        core::Logger::warn(core::LogCategory::Core, "demo: %s; nothing applied", in.reason().c_str());
        return;
    }

    saveCount = count;
    inspectorOpen = inspector;
    panelState.debugView = view;
    e.renderer().setDebugView(static_cast<core::DebugView>(view));
    core::Logger::status(core::LogCategory::Core, "demo: restored save #%u", saveCount);
}

void DemoGame::drawUi(Engine& e, ui::Context& ui) {
    const float scale = e.uiScale();
    const auto screenW = static_cast<float>(e.renderer().framebufferWidth());
    const auto screenH = static_cast<float>(e.renderer().framebufferHeight());
    const core::settings::Settings& s = e.settingsTable();

    // Clamped to the window, not just scaled: a panel sized in logical units is 1120
    // pixels tall at 200%, taller than a 900-pixel window, so scaling alone puts half the
    // widgets off the bottom of the screen. The panel scrolls, so shrinking it loses nothing.
    const glm::vec2 panelPos{s.get(panelX) * scale, s.get(panelY) * scale};
    const glm::vec2 panelSize{std::min(s.get(panelWidth) * scale, screenW - panelPos.x - 8.0f * scale),
                              std::min(s.get(panelHeight) * scale, screenH - panelPos.y - 8.0f * scale)};
    drawSettingsPanel(ui, e, panelState, panelPos, panelSize);

    // Both panels cannot sit at the config's one position, so this one is placed to the
    // right of the first and clamped the same way.
    if (!inspectorOpen) return;
    const glm::vec2 inspectorPos{panelPos.x + panelSize.x + 8.0f * scale, panelPos.y};
    const float inspectorWidth =
        std::min(s.get(panelWidth) * scale, screenW - inspectorPos.x - 8.0f * scale);
    // Only when there is room for it. At 200% on a narrow window the first panel already
    // reaches the edge, and a second one clamped to a negative width draws a title bar
    // over the scene and nothing else.
    if (inspectorWidth <= 80.0f * scale) return;

    // The column is split rather than a third one opened beside it: at this width a third
    // column is off the right-hand edge on anything but a wide screen.
    const float half = (panelSize.y - 8.0f * scale) * 0.5f;
    ui::drawInstanceInspector(ui, e.instances(), inspectorState, inspectorPos, {inspectorWidth, half});
    ui::drawNodeInspector(ui, e.scene(), nodeInspectorState, {inspectorPos.x, inspectorPos.y + half + 8.0f * scale},
                          {inspectorWidth, half});
}

SUBSTRATE_GAME(DemoGame)
