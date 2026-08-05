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

/// Where an interactive F12 screenshot goes. Numbered rather than timestamped: a
/// sequence is what a before/after comparison wants, and a timestamp sorts the same
/// way while being much harder to name in a sentence.
std::string nextCapturePath() {
    static uint32_t counter = 0;
    char name[64];
    std::snprintf(name, sizeof(name), "debug_frames/capture_%03u.png", counter++);
    return name;
}


/**
 * @brief The locomotion state machine every character runs (S2.4, G12).
 *
 * Built by *name* against whatever clips the scene happens to have, because a state
 * machine that hard-coded clip indices would be a machine for exactly one file. A scene
 * missing a name gets no state for it, and a scene with no recognisable clip at all
 * gets an empty machine -- which the animator treats as "just play what you were told",
 * so the four test scenes with one clip each behave exactly as they did.
 *
 * **Six states, and the two G12 added are the airborne half.** `speed` alone cannot
 * express them: a character at a standstill in mid-air and one at a standstill on the
 * floor are the same number. So the machine takes a second parameter, and the sense of
 * it is the load-bearing detail -- it is `airborne`, not `grounded`, because every
 * parameter starts at zero and a machine nobody is driving must read as *standing on
 * something*. Spelled the other way round, a scene whose characters run off the fixed
 * step rather than off a controller would fall through the floor of a state machine on
 * the first frame.
 */
scene::AnimationStateMachine locomotionMachine(const scene::SceneAnimator& animator) {
    // Several spellings per state, since Mixamo, Khronos and Blender each name these
    // differently and the alternative is one machine per exporter.
    struct Candidate {
        const char* state;
        std::initializer_list<const char*> clips;
        scene::LoopMode loop;
    };
    const Candidate candidates[] = {
        {"idle", {"idle", "Idle", "Survey", "idle (2)"}, scene::LoopMode::Loop},
        {"walk", {"walking", "walk", "Walk", "left strafe walking"}, scene::LoopMode::Loop},
        {"run", {"running", "run", "Run"}, scene::LoopMode::Loop},
        // `jumping up` ahead of `jump`, and the order is G12's rather than cosmetic. On
        // this rig `jump` is the *whole* leap -- crouch, launch, hang and landing, 2.17
        // seconds of it -- while `jumping up` is the 0.25 s launch alone. With `fall` and
        // `land` now states of their own, the second is the one that composes; the first
        // would play its own landing over the top of theirs, a second and a half early.
        {"jump", {"jumping up", "jump", "Jump"}, scene::LoopMode::ClampToEnd},
        {"fall", {"falling idle", "fall", "Fall", "falling"}, scene::LoopMode::Loop},
        {"land", {"hard landing", "land", "Land", "landing"}, scene::LoopMode::ClampToEnd},
    };

    scene::AnimationStateMachine m;
    std::vector<std::string> found;
    for (const Candidate& c : candidates) {
        for (const char* name : c.clips) {
            const uint32_t clip = animator.findClip(name);
            if (clip == scene::SceneAnimator::kNoClip) continue;
            m.states.push_back({c.state, clip, c.loop, 1.0f});
            found.emplace_back(c.state);
            break;
        }
    }
    // One state is not a machine -- it is the clip the animator would have played
    // anyway, wearing a table.
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
    // value `AnimationTransition::from` uses to mean *out of everything*. Writing one of
    // those into a transition would turn a missing state into a wildcard, so a transition
    // between two named states goes through here and a wildcard is spelled out below.
    // A transition into the state it leaves is dropped rather than written: `stepStateMachine`
    // already refuses one, so keeping it would be three rows of a table that exist only to be
    // skipped once per step per character.
    const auto link = [&m](uint32_t from, uint32_t to, std::vector<scene::AnimationCondition> conditions, float fade,
                           bool waitForExit = false) {
        if (from == scene::kAnyState || to == scene::kAnyState || from == to) return;
        m.transitions.push_back({from, to, std::move(conditions), fade, waitForExit});
    };

    // Order is priority, and the jump is first on purpose: a trigger fired on the frame
    // the character also crossed a speed threshold should jump, not change gait. This is
    // the machine's one wildcard, and it is the case the sentinel was invented for.
    if (jump != scene::kAnyState) {
        m.transitions.push_back({scene::kAnyState, jump, {{kJump, scene::ConditionTest::Greater, 0.5f}}, 0.1f, false});
    }

    // Out of the launch, and only once it has played. A quarter of a second of `jumping
    // up` against the better part of a second in the air means the ordinary exit is the
    // first of these; the second is the hop so short the launch outlasts it.
    link(jump, fall, {{kAir, scene::ConditionTest::Greater, 0.5f}}, 0.2f, true);
    link(jump, land, {{kAir, scene::ConditionTest::Less, 0.5f}}, 0.15f, true);
    link(fall, land, {{kAir, scene::ConditionTest::Less, 0.5f}}, 0.1f);

    // Walking off something, which is the half of `fall` that has nothing to do with
    // jumping. Enumerated over the three grounded gaits rather than written as a wildcard:
    // a wildcard would also hold one step after the launch, and would cut the clip the two
    // transitions above exist to let finish.
    for (const uint32_t from : {idle, walk, run}) {
        link(from, fall, {{kAir, scene::ConditionTest::Greater, 0.5f}}, 0.15f);
    }

    // Gait. Every one of these carries `airborne < 0.5` as well as its speed band, so the
    // rule is stated once rather than implied by which states the table happens to leave
    // out: a character changes gait with its feet down.
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

    // Landing recovery, and it is deliberately not `gait(land, ...)`. `hard landing` is two
    // seconds; a character rooted for two seconds because they touched the floor is the
    // animation every game lets movement cancel, and only the standing case plays out.
    link(land, run, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Greater, 0.66f}}, 0.15f);
    link(land, walk,
         {{kAir, scene::ConditionTest::Less, 0.5f},
          {kSpeed, scene::ConditionTest::Greater, 0.2f},
          {kSpeed, scene::ConditionTest::Less, 0.66f}},
         0.15f);
    link(land, idle, {{kAir, scene::ConditionTest::Less, 0.5f}, {kSpeed, scene::ConditionTest::Less, 0.2f}}, 0.25f, true);

    // Last, and only reachable on a rig missing one of the two new clips: without them a
    // character that got into `fall` or `jump` has no way out and stays there. On the rig
    // that has all six these never fire, because everything above them does first -- and
    // `gait(jump, ...)` waiting for the clip to exit is exactly the `jump -> idle` the
    // four-state machine shipped with.
    gait(fall, 0.2f, false);
    gait(jump, 0.25f, true);

    std::string names;
    for (const std::string& n : found) names += (names.empty() ? "" : ", ") + n;
    core::Logger::status(core::LogCategory::Scene, "Animation: state machine over %zu states (%s)", m.states.size(),
                   names.c_str());
    return m;
}

/**
 * @brief Drive every character's state machine parameters for the frame (S2.4).
 *
 * A demo rather than a game, and it is deliberately a *function of the step index*:
 * driving this from input would make the animation a function of what somebody pressed,
 * and 5.3's golden images need frame N to be the same image on every run. So each
 * character walks a fixed gait cycle -- idle, walk, run, walk, idle, jump -- offset by
 * its own index so a row of them is never in unison, which is what makes a cross-fade
 * visible at all: five identical characters mid-transition look like one.
 *
 * A machine with no `speed` parameter is one `locomotionMachine` declined to build, and
 * this does nothing at all -- the animator keeps playing whatever clip it was given.
 */
void driveCharacters(scene::SceneAnimator& animator, uint64_t stepIndex, float step) {
    const uint32_t speed = animator.stateMachine().findParameter("speed");
    if (speed == scene::kAnyState) return;
    const uint32_t jump = animator.stateMachine().findParameter("jump");

    constexpr float kCycle = 9.0f;   ///< seconds for one idle-walk-run-walk-idle round
    constexpr float kStagger = 1.6f; ///< how far apart in that cycle two neighbours sit
    const float now = static_cast<float>(stepIndex) * step;

    for (uint32_t c = 0; c < animator.characterCount(); ++c) {
        const float phase = std::fmod(now + static_cast<float>(c) * kStagger, kCycle) / kCycle;
        // Triangular: up over the first half of the cycle, down over the second. A
        // sine would spend most of its time near the extremes, which is exactly where
        // a state machine is *not* transitioning and therefore shows nothing.
        animator.setParameter(animator.characterAt(animator.characterCount() > 0 ? c : 0), speed,
                              phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f);

        // One jump per cycle, at the moment the character is coming back down to idle.
        // Fired rather than held, because a trigger the machine consumed is the only
        // way "jump once" differs from "jump forever".
        if (jump != scene::kAnyState) {
            const float previous = std::fmod(now - step + static_cast<float>(c) * kStagger, kCycle) / kCycle;
            if (phase >= 0.9f && previous < 0.9f) animator.fire(animator.characterAt(c), jump);
        }
    }
}

/// One frame of "what did the user ask for". Every branch is `pressed`, which is an
/// edge: held keys do not re-fire, and a key tapped between two frames still counts.
void applyActions(Engine& e, const AppActions& a, bool& inspectorOpen) {
    // A game may profile its own code, and this is what it looks like: the engine's own
    // `Profiler::scope`, a string literal, the function's name. The demo's zones nest
    // under `Game::frameUpdate` and `Game::fixedUpdate` because the engine opens those
    // around the calls -- a game adds nothing to make that happen.
    auto s = core::Profiler::scope("applyActions");
    const core::input::InputMap& in = e.input();
    gfx::Renderer& r = e.renderer();

    if (in.pressed(a.quit)) e.requestQuit();

    // Through the settings table rather than through `setSampleCount`, and that is the
    // whole of G2 in one line: the key, the panel's slider and `"msaaSamples"` in
    // substrate.json now write one variable, so `--dump-settings` reports what the key
    // last did and a value set here survives a save. The engine applies it before the
    // draw. Assigning the renderer directly would still work and would be the bug -- the
    // table would say 4 while the image was 8.
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

    // Cycles every debug view, including the three the direct actions cannot reach.
    // Those stay direct because jumping straight to albedo is what they are for; this
    // is how the rest of the list becomes reachable without inventing three more.
    if (in.pressed(a.viewCycle)) {
        r.cycleDebugView(+1);
        core::Logger::status(core::LogCategory::Render, "Debug view: %s", debugViewName(r.currentDebugView()));
    }

    if (in.pressed(a.overlay)) {
        r.debugOverlay = !r.debugOverlay;
        core::Logger::status(core::LogCategory::Render, "Frame stats overlay: %s", r.debugOverlay ? "on" : "off");
    }

    // Each of these flips a specialisation constant, so it recompiles the pipelines
    // that read it -- one hitched frame, and then the disabled feature costs nothing
    // rather than costing a branch. This is what makes a per-pass number attributable:
    // toggle, read the overlay or the trace, toggle back.
    const auto flip = [&](core::input::ActionId id, bool& flag, const char* name) {
        if (!in.pressed(id)) return;
        flag = !flag;
        core::Logger::status(core::LogCategory::Render, "%s: %s", name, flag ? "on" : "off");
    };
    flip(a.ssao, r.ssaoEnabled, "SSAO");
    flip(a.bloom, r.bloomEnabled, "Bloom");
    flip(a.culling, r.cullingEnabled, "GPU frustum culling");
    flip(a.edgeMsaa, r.edgeMsaaEnabled, "Edge-detect hybrid MSAA");
    // The ray-traced three report what will actually happen rather than what the flag now
    // says. Every one of them is gated on `rayQuerySupported && accel.valid()` inside the
    // renderer, so on a device without the extensions the plain `flip` above announced
    // "on" while the specialisation constant stayed false -- a toggle that lies is worse
    // than one that is missing.
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
    // Inert at 1x as well as without ray query, and `flipRt` only reports the second --
    // but the row is honest about that in the renderer, and a third lambda for one
    // toggle would be the drift the second one already is.
    flipRt(a.rtShadowMask, r.rtShadowMaskEnabled, "Per-fragment shadow mask");
    flip(a.ssr, r.ssrEnabled, "SSR");
    flip(a.fog, r.fogEnabled, "Volumetric fog");
    flip(a.taa, r.taaEnabled, "TAA");
    flip(a.particles, r.particlesEnabled, "Particles");
    // Not a specialisation constant and so not a pipeline rebuild: the debug pass reads
    // an empty vertex vector and records nothing at all when this is off (S4.5).
    flip(a.physicsDebug, e.physicsDebugDraw, "Physics debug draw");
    // And the audio lines go into the same vector, which is the whole reason S5.5 needed
    // no drawing code of its own.
    flip(a.audioDebug, e.audioDebugDraw, "Audio debug draw");
    // The inspector rides the same context and the same draw list, so it is a second
    // panel rather than a second anything else (5.6).
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
        // Cycles rather than toggles: TONEMAP_OPERATOR is a selector, not a flag. Onto
        // the renderer's field, because D14 made the curve `GameSetup::tonemap` -- an
        // authored look with `--tonemap` as its one-run override -- and there is no row
        // left to write. A debug key that cycles a game's grade is a debug affordance and
        // does not write back to the setup, which is why nothing here persists.
        const auto next = static_cast<core::TonemapOperator>((static_cast<uint32_t>(r.tonemapOperator) + 1) %
                                                            static_cast<uint32_t>(core::TonemapOperator::Count));
        r.tonemapOperator = next;
        core::Logger::status(core::LogCategory::Render, "Tonemap: %s", core::tonemapKey(next));
    }

    if (in.pressed(a.screenshot)) r.requestCapture(nextCapturePath());

    // S7 from a key rather than only from `record.enabled`, which is the whole of what a
    // capability with no door looks like: the recorder, the frame tee and the audio tap
    // were all built and only a config value could start them. `stopRecording` reports the
    // file because a recording nobody can find is one nobody watches.
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

    // The one debug affordance that spans two subsystems, which is why the engine
    // exposes it as a call rather than leaving it as two.
    if (in.pressed(a.dumpProfile)) e.dumpProfile();

    // C6. One call each way, and the failure is reported rather than swallowed -- a save
    // that silently did not happen is the one bug in a save system that costs a player
    // something they cannot get back.
    if (in.pressed(a.save)) (void)e.saveGame("debug_frames/demo.sav");
    if (in.pressed(a.load)) (void)e.loadGame("debug_frames/demo.sav");
}

/**
 * @brief One frame of the settings panel.
 *
 * Deliberately in the game and deliberately long. This is the *game's* interface, not the
 * engine's: which toggles matter and what they are called is an application decision,
 * exactly as `AppActions` is, and a `ui/SettingsPanel.cpp` would be the engine growing
 * opinions about a particular set of renderer flags. It reads top to bottom, which is
 * also how it draws.
 *
 * Every widget here is a call that both draws and answers. There is no widget tree, no
 * `onClick`, and no place for the value on screen to disagree with the value in the
 * renderer -- because they are the same variable, passed by reference.
 */
void drawSettingsPanel(ui::Context& ui, Engine& e, PanelState& state, const glm::vec2& pos, const glm::vec2& size) {
    if (!ui.beginPanel("Substrate", pos, size)) return;

    gfx::Renderer& r = e.renderer();
    scene::AudioEngine& audio = e.audio();

    if (state.uiImage.valid()) {
        ui.labelDim("Overlay image (C5)");
        // Square, and three rows rather than one: at a row's height the 64x64 source is
        // minified past the point where a person can see whether it is right, and being
        // able to see that is the entire reason the demo draws it. The chequer and the
        // alpha ramp are what make a wrong sampler or a wrong blend visible rather than
        // plausible -- which is what each property of `res:/ui_test.png` is there to catch.
        ui.image(state.uiImage, 48.0f, 1.0f);
        ui.separator();
    }

    // Every render setting there is, generated from the table (G2). This is the block of
    // nine checkboxes and four sliders that used to be written out here by hand, and what
    // replaced it is not shorter by accident: the names, the ranges and which rows can
    // still be changed at all were already declared in `SUBSTRATE_SETTINGS`, for the JSON
    // parser and the dump, so writing them again here was a fifth spelling free to drift
    // from the other four. Sliders the table does not carry -- an authored exposure, an
    // authored sun -- stay below, because those are this game's, not settings.
    ui.labelDim("Rendering");
    if (ui.button("Save settings")) {
        // The file the run was launched with, as the binding menu writes to; empty means
        // no file was found, and defaults are what an unwritten config already means.
        const std::string path = e.config().sourcePath.empty() ? std::string("substrate.json")
                                                               : e.config().sourcePath.string();
        state.saveStatus = e.settingsTable().saveJson(path) ? "saved to " + path : "save failed: see the log";
    }
    if (!state.saveStatus.empty()) ui.labelDim(state.saveStatus);
    ui.separator();
    ui::drawSettings(ui, e.settingsTable(), "render");

    ui.separator();
    // Shown only where they do something. `ui::Context` has no disabled state, and a dim
    // line saying why beats a control that moves and changes nothing -- the same reasoning
    // the generated rows above apply to a row the table marks init-only.
    if (!r.rayTracingAvailable()) {
        ui.labelDim("Ray tracing unavailable on this device");
        ui.separator();
    }

    ui.labelDim("Exposure and light");
    ui.slider("Exposure", r.exposure, 0.1f, 4.0f);
    ui.slider("Sun", r.sunIntensity, 0.0f, 10.0f);

    ui.separator();
    ui.labelDim("Debug view");
    // The list is the widget this panel exists to exercise, and it is also the most
    // useful thing on it: nine views the keyboard reaches only by cycling.
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
            // A ducking bus moves its own gain, so this is the one row on the panel that
            // is a readout rather than a control -- and the cheapest possible check that
            // S5.4 is doing something.
            ui.labelDim(line);
        }
    }

    ui.separator();
    // D17, and it is the same call as the render block above with a different module name.
    // These two rows are not in `SUBSTRATE_SETTINGS` -- the game declared them before the
    // config file was read -- and nothing here, in `saveJson`, in `--dump-settings` or in
    // `--write-default-config` knows that. A game's setting that existed everywhere except
    // where a user would look for it would not be a setting.
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

// ============================================================================= actions

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
    // F9 was free -- it toggled IBL, and the environment term it gated went with the
    // split-sum lookup. It now flips the per-fragment shadow mask, which is the toggle
    // that pass's cost is attributed across: on, the rays are in `ShadowMask`; off, they
    // are back inside `Lighting`. Bloom keeps F10 rather than sliding up, so a saved
    // bindings file written before this still means what it says.
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
    // Was F11 and therefore unreachable: the tonemap handler above it returned
    // first, so the interactive RenderDoc trigger had never once fired. Two `case`
    // labels for one key is a bug a table makes visible and a switch hides.
    renderDoc = map.declare("Capture.RenderDoc", "PrintScreen");
    // Punctuation because every one of the twenty-six letters is already spoken for by the
    // table above, the camera or the player -- and `InputMap::conflicts` says so at startup
    // rather than leaving one key to drive two things.
    record = map.declare("Capture.Record", "Period");
    dumpProfile = map.declare("Debug.DumpProfile", "P");
    // F7 is the only function key this table has left, and F1-F5 were already the debug
    // views -- which is what "Game.Save" on F2 collided with until `InputMap::conflicts`
    // started saying so at startup. `L` for the other half, since a save and a load do not
    // have to be neighbours and every remaining F key is spoken for.
    navGo = map.declare("Nav.GoTo", "V");
    streamScene = map.declare("Scene.Stream", "X");
    addModel = map.declare("Scene.AddModel", "Z");
    // Punctuation for the same reason `Capture.Record` took one: the letters ran out. Both
    // of these landed on a letter an older toggle already held -- `B` with the physics
    // wireframe, `G` with the fog -- and neither shadowed the other, so one press ran both.
    // A rig import on every wireframe toggle is the expensive half of that.
    addRig = map.declare("Scene.AddRig", "Semicolon");
    dropModel = map.declare("Scene.DropModel", "H");
    carry = map.declare("Scene.Carry", "U");
    fetch = map.declare("Scene.Fetch", "Comma");
    spawnCube = map.declare("Scene.SpawnCube", "F");
    // Punctuation again, and `Slash` is what the table has left.
    ridePlatform = map.declare("Scene.RidePlatform", "Slash");
    save = map.declare("Game.Save", "F7");
    load = map.declare("Game.Load", "L");
    // C37. Punctuation for the reason `Capture.Record` and `Scene.RidePlatform` took some:
    // the twenty-six letters are spoken for by the table above, by the player and by
    // whichever camera is installed. `Tab` would have read better and belongs to the
    // binding menu.
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

/// Close enough to the torch to take hold of it, in metres (C24). An arm's length plus the
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
    // **The camera's own two vectors, flattened -- not a second derivation of them.** This
    // was `ahead = (sin yaw, 0, -cos yaw)` beside a `Camera::forward()` of
    // `(cos pitch sin yaw, sin pitch, cos pitch cos yaw)`, and the two agree only where
    // `cos yaw` is zero. `frameBounds` picks yaw = pi/2 whenever X is the longer horizontal
    // axis, the showcase scene is 30 units of X against 18 of Z, and `locomotion.sh` passed
    // no `--camera` -- so every check in the tree ran at the one yaw where a 180-degree
    // heading error cancels exactly. Deriving rather than restating is what stops that
    // being possible again; the arithmetic below is `Camera::update`'s, moved and flattened.
    const glm::vec3 view = camera.forward();
    const glm::vec3 flat(view.x, 0.0f, view.z);
    const float reach = glm::length(flat);
    // A camera looking straight down has no horizontal forward to resolve against. It
    // cannot happen through `Camera::update`, whose pitch is clamped short of the pole, but
    // `--camera` writes the four numbers directly and a game may too.
    if (reach <= 1e-4f) return glm::vec3(0.0f);
    const glm::vec3 ahead = flat / reach;
    // Screen-right, by the same cross product `Camera::update` takes and in the same order.
    // The old expression `(-ahead.z, 0, ahead.x)` *is* this cross product written out, which
    // is why the sign error above mirrored strafing as well as walking rather than only one.
    const glm::vec3 side = glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f));
    // `value` rather than `held`, and that is what makes a stick a stick: an action bound
    // to a pad axis carries how far it was pushed, so half a deflection is half a request
    // and the walk band is reachable without a modifier at all. A key contributes its full
    // value, so the keyboard needs one -- which is what `run` is.
    glm::vec3 d = ahead * (in.value(forward) - in.value(back)) + side * (in.value(right) - in.value(left));
    const float length = glm::length(d);
    if (length <= 1e-4f) return glm::vec3(0.0f);

    // Two diagonal keys sum to a length of 1.41 and would ask for 40% more speed than one
    // key does, so the length is clamped rather than normalised: normalising would push a
    // stick at rest-plus-a-nudge back out to full travel.
    const float fraction = std::min(length, 1.0f) * (in.held(run) ? 1.0f : kWalkFraction);
    return d / length * fraction;
}

// ============================================================================ the game

void DemoGame::declareSettings(core::settings::Settings& settings) {
    // D17, and it runs before `substrate.json` is read -- which is the whole reason it is a
    // separate method from `configure` below. The file is walked once, so a row that
    // arrived after it would have had its key reported as a typo and would keep its
    // built-in whatever the user wrote.
    //
    // `demo` is this game's module, and a game owns every module the engine does not name.
    // The handles are typed exactly as `core::options::render::ssao` is; only their ids are
    // assigned here rather than by the compiler.
    impactVolume = settings.declare("demo.impactVolume", 1.0f, "Impact volume", 0.0f, 1.0f);
    impactDust = settings.declare("demo.impactDust", true, "Impact dust");

    // D14. The panel's own geometry, which was four `ui.` rows the engine carried for one
    // game's benefit. Same bounds and same built-ins as the rows they replace, so a
    // `--set demo.panelWidth=400` reaches what `--set ui.panelWidth=400` used to.
    panelX = settings.declare("demo.panelX", 16.0f, "Panel x", 0.0f, 4096.0f);
    panelY = settings.declare("demo.panelY", 16.0f, "Panel y", 0.0f, 4096.0f);
    panelWidth = settings.declare("demo.panelWidth", 300.0f, "Panel width", 80.0f, 4096.0f);
    panelHeight = settings.declare("demo.panelHeight", 560.0f, "Panel height", 80.0f, 4096.0f);
}

void DemoGame::configure(GameSetup& setup, core::settings::Settings& /*settings*/) {
    // Everything here was a key in `substrate.json` before S1, and every one of them
    // failed the test that document applies: *is this a property of the person running the
    // program, or of the program?* A scene, a sun, a mix graph and an exposure are the
    // second, so they are C++ that ships with the game rather than JSON that ships beside
    // it -- and a key that is not in the file cannot silently do nothing.
    setup.name = "Substrate";
    // No scene named here, and no budgets. The world is composed in `init` out of imported
    // assets (C41), and the pools size themselves from what gets made (C40).

    // The sun, as a light in a list rather than three fields of its own (D20). It is the
    // only directional light here, so it is the one `initLights` promotes.
    setup.look.lights = {gfx::makeDirectionalLight({-0.35f, 0.85f, 0.4f}, {1.0f, 0.96f, 0.88f}, 3.0f)};
    // Sandstone, not sky. Sponza is a stone courtyard, so the light bouncing around its
    // arcades is the colour of the stone that bounced it -- which is the whole argument
    // for authoring this rather than sampling a cube built from the sky, and why it is a
    // colour rather than a brightness. Small: at 0.03 it lifts a shadowed bay off pure
    // black without pretending to be the indirect light a bake would give.
    setup.look.ambientColor = {0.0025f, 0.0021f, 0.0016f};
    setup.look.exposure = 1.0f;

    // The three every project invents anyway, spelled out rather than left to the
    // engine's fallback, because this is where a game's mix graph is supposed to be
    // readable.
    setup.audio.buses = {
        {"music", 1.0f, "sfx", 0.45f, 0.05f, 0.4f},
        {"sfx", 1.0f, "", 1.0f, 0.05f, 0.4f},
        {"ambience", 1.0f, "sfx", 0.7f, 0.05f, 0.4f},
    };

    // Nothing seeds a setting here. The demo has no opinion about anybody's window size,
    // MSAA or volume, and a game that overrode one would show up as `game` in the source
    // column of `--dump-settings` -- which is what makes an opinion visible rather than
    // mysterious.
}

void DemoGame::init(Engine& e) {
    // ------------------------------------------------------------------ the world (C41)
    //
    // The building is an *asset* the game imports onto a node it made, not a scene the
    // engine loaded on the game's behalf. First in `init`, because everything below reads
    // the bounds it establishes.
    //
    // Sponza is a courtyard built for a camera to fly through rather than for a 1.8 m
    // character to walk in, and at its authored size the nave is about four strides wide.
    // Doubling the building buys the room without touching the character, which keeps the
    // stride length, the jump arc and every collider tuned against them exactly as they
    // were. Stated before the import rather than written into the node's transform, because
    // it has to reach the props this game imports next -- and because a scale in a placement
    // matrix stretches a rig.
    e.setWorldScale(kDemoWorldScale);
    const scene::NodeId building = e.scene().create("sponza");
    e.scene().add<scene::Model>(building, {core::Resources("res:/Sponza/glTF/Sponza.gltf")});

    // Declared here rather than by the engine, which is the whole of G1 in one line: the
    // engine binds no keys. `Engine::run` applies the config's rebinds immediately after
    // this returns, so every action below is one `substrate.json` can name.
    actions.declare(e.input());

    // **And nothing has to be moved out of its way** (C37). This used to sit between an
    // `e.setCamera(&flyCamera)` above and five `setDefaultBindings` calls that pushed the
    // flycam's WASD onto the arrow keys, because both schemes were live at once. The camera
    // this game now installs declares one row -- `Camera.Orbit` -- and the free-fly one is
    // not running while a character is being driven, so W is asked for once.
    playerActions.declare(e.input());

    // G12. The control scheme this game *ships*, reported rather than assumed, because the
    // property that makes it shipped is invisible from the outside: `setBindings` and
    // `setDefaultBindings` both move the live list and only the second moves the declared
    // default with it. `isDefault` is what tells them apart, and a `*` marks any action off
    // it -- which after this line can only be a rebind the config asked for.
    //
    // The camera row is logged after the camera is installed rather than here, because
    // until then there is no controller and therefore no `Camera.*` row to report.
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

    // A file that ships its own lights already won -- the engine took the first
    // directional as the sun and kept the rest. What a scene *without* lights gets is a
    // game decision, and this is it. Before S1 the choice between the automatic placement
    // and an explicit list came from `lighting.autoPlacePointLights`; a game that wants
    // the explicit list writes the list, which is one branch fewer and one config key
    // fewer for the same two outcomes.
    //
    // **And not when this game is about to author its own**, which is the other half of the
    // same decision. `buildDemoWorld` lights Sponza with three points and four brazier
    // cones; placing a fallback set first does not get overwritten by that, it gets *added
    // to*, and the interior ends up lit by two key sets at once -- the side aisles the demo
    // means to leave dark read as fully lit, and the atlas is 23 layers deep for a scene
    // that wants 22. A file that ships lights suppresses the fallback for exactly this
    // reason; a game that composes a world in code has the same claim on it.
    // ------------------------------------------------------------ the decision layer (C24)
    // Two properties and two actions, and the route between them is derived rather than
    // authored. `G` hands over the *goal* -- a carried torch -- and from that moment nothing
    // is pressed: the planner works out that picking the torch up requires standing near it,
    // and the character walks over and does both.
    //
    // The costs are in metres, roughly, so a third action added later can be compared
    // against these without a unit conversion nobody would remember to make.
    propNearTorch = planner.declare("near_torch");
    propCarrying = planner.declare("carrying_torch");
    {
        ai::Action walk;
        walk.name = "walk to the torch";
        // No prerequisites at all, which is the point: this action does not know it is the
        // first step of anything.
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

    // G5, and the whole of what a game does to get its own shading. One call, one index,
    // and a material that names it -- no pipeline, no descriptor set, no pass. Registered
    // unconditionally: nothing is compiled until a draw carrying this index reaches the
    // G-buffer pass, so a run that never presses F pays a struct in a vector.
    {
        gfx::ShaderVariant hologram;
        hologram.name = "hologram";
        hologram.fragmentShader = "hologram.frag";
        // ENABLE_SCANLINES, at constant_id 8 -- the first id above the engine's reserved
        // block. Passed rather than left to the shader's default so the reflection check
        // has something to compare against, which is the half of this that would
        // otherwise go untested.
        hologram.constants = {1u};
        hologramVariant = e.renderer().addShaderVariant(std::move(hologram));
    }

    // G9, and it is one call because everything it needs is already a call. What it builds
    // and why each piece is there is in DemoWorld.cpp; what matters here is where it sits:
    // after the lights the scene decided, and gated on the scene being the one `configure`
    // named so that eleven golden cases running this same binary see nothing new.
    buildDemoWorld(e, world);

    // **The heuristic the engine used to run, moved to where it can be seen (G17).** A scene
    // that authors a `Character` collider -- `physics.gltf` among the golden eleven -- gets
    // that character driven, because for those scenes the demo builds nothing and would
    // otherwise have nobody. Taking every authored character rather than the first is what
    // makes this a decision instead of a latch: the demo drives player zero, and a game with
    // four splits the same list four ways.
    if (world.players.empty()) {
        for (const Engine::AuthoredCharacter& c : e.authoredCharacters()) {
            world.players.push_back({c.character, c.node, {}});
        }
    }

    // G3, and the smallest thing that shows what a hierarchy is for. The torch is a node
    // carrying one of the lights that already exist; `E` reparents it onto the character
    // and back. Nothing else changes -- the light does not know it moved, the renderer
    // does not know a tree exists, and the whole feature is two calls in `frameUpdate`.
    //
    // Attached to a light rather than adding one, deliberately: an extra light is an
    // extra light in every golden image, and what is being demonstrated is the node.
    //
    // **After the world, because the world is what lights this scene**, and the index is
    // therefore the world's to name. `lights[1]` is only a warm fill on a scene where the
    // fallback placement ran first, which is now every scene except the demo's own.
    {
        const uint32_t light = world.torchLight != DemoWorld::kNoLight ? world.torchLight : 1u;
        if (e.renderer().lights.size() > light) {
            torchLight = light;
            torchNode = e.scene().create("torch");
            e.scene().attachLight(torchNode, torchLight);
            // Seeded from where the light already is, so the first sweep writes it back
            // unchanged. A node that started at the origin would move the light on frame one.
            e.scene().setLocalPosition(torchNode, glm::vec3(e.renderer().lights[torchLight].position));
        }
    }

    // **After the world, because the machine is built from the clips the rig carries** and
    // the rig arrives with the import above. `locomotionMachine` resolves six states by name
    // against `SceneAnimator`; run before the character exists it finds no clips, builds
    // nothing, and every arm of `scripts/locomotion.sh` fails on a machine that is not there.
    // The composite made this invisible -- its character was in the file, so the clips existed
    // before `init` began.
    e.animator().setStateMachine(locomotionMachine(e.animator()));

    inspectorOpen = e.config().ui.inspector;
    panelState.capturePath = e.config().benchmark.capturePath;
    for (uint32_t i = 0; i < static_cast<uint32_t>(core::DebugView::Count); ++i) {
        panelState.debugViewNames.emplace_back(debugViewName(static_cast<core::DebugView>(i)));
    }
    panelState.debugView = static_cast<uint32_t>(e.renderer().currentDebugView());
    // C5, and the reason the demo loads one at all: `ImageTable` and the overlay's image
    // array need a caller outside the unit suite, or they ship with the CPU half tested
    // and the descriptor half only argued for. Committed, so a checkout always has it --
    // but a package built from a narrower `package.txt` need not, which is why the panel
    // checks before drawing rather than this aborting.
    panelState.uiImage = e.images().load("res:/ui_test.png");

    // G7. Built once and fired many times, which is the shape `playAt` is written for: a
    // desc is fifteen fields and an impact varies two of them. Committed alongside the
    // image above, and checked for the same reason -- a package built without it should
    // be a demo without a thud rather than a warning on every collision.
    {
        const core::Resources file("res:/audio/impact.wav");
        if (file.found()) {
            impactSound.name = "impact";
            impactSound.file = file.string();
            impactSound.bus = "sfx";
            impactSound.spatial = true;
            // A quarter of a second is well under the crossover, so this is the first
            // source in the repository to take the decode side of it without an author
            // naming a path -- which is what `"load": "decode"` in audio.gltf was standing
            // in for.
            impactSound.minDistance = 2.0f;
            impactSound.maxDistance = 60.0f;
            // Nothing worth occluding: it is over before a listener could walk behind a
            // wall, and the filter is inserted at load, so leaving it on would cost every
            // shot a biquad for a quarter of a second of nothing.
            impactSound.occlusion = false;
        }
    }

    // **C37, and it is the last thing `init` does that touches input**, because installing a
    // controller is what makes its rows exist and `Engine::run` applies the config's rebinds
    // the moment this returns. It is also after `buildDemoWorld` and after the authored
    // characters are adopted, which is what it asks about: a scene with a character gets the
    // third-person camera, and one without gets the flycam it always had.
    //
    // The follow target is the *demo's own* player node and only that, for the reason G13's
    // rig was gated the same way: `golden.sh` runs this binary against eleven scenes, one of
    // which authors a character, and a camera that re-aimed itself there would move a
    // baseline for a reason that has nothing to do with the renderer. A `ThirdPersonCamera`
    // with no target writes no `focus` at all, so those cases see a camera that does exactly
    // what the flycam did with nothing pressed.
    if (world.built && world.playerNode().valid()) followCamera.follow(&e.scene(), world.playerNode());
    installCamera(e, !world.playerCharacter().valid());

    // G12 moved the camera off WASD and the shift, so the line that advertises it has to
    // move with them -- a controls hint that names the keys the last version used is worse
    // than none, because a reader trusts it.
    // C37 moved it back, and the hint with it: the flycam is no longer running beside the
    // character, so it keeps the keys it declares and the line only has to say which camera
    // is in and how to change it.
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
    // **The pose does not come with the controller.** `Engine::setCamera` installs a
    // pointer and copies nothing, and only the camera that happened to be active during
    // `Engine::run` was ever framed -- so a swap without this drops the view to focus zero,
    // distance five, looking down +Z. The projection scalars come too: `frameBounds` sized
    // the near plane to the scene and `camera.fovDegrees` is the engine's row, and neither
    // is re-applied after startup.
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

    // **Every live `Camera.*` row, listed after every install**, which is the same walk
    // `ui::BindingMenu` does over `actionCount()`/`actionLive()` -- so this is the rebind
    // menu's contents rather than an argument about them. Two things are checkable from it
    // and neither is inferable: eight of the flycam's nine rows are absent while the follow
    // camera is in, so nothing stale is left behind by a switch, and a `*` on a row that
    // survives the round trip is a player's rebind that `retire` kept and `declare` revived.
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

    // C37, and the reason the demo ships two cameras rather than one: the swap runs
    // `setCamera`'s deactivate-then-activate against a real pair of control schemes, so the
    // flycam's nine rows appear and disappear from the binding menu with it and a rebind the
    // player made survives the round trip -- `retire` keeps the bindings and `declare` is
    // idempotent by name.
    if (e.input().pressed(actions.cameraToggle)) {
        core::Logger::status(core::LogCategory::Core, "Camera: %s",
                             flying ? "following the character" : "free-fly -- the character is not being driven");
        installCamera(e, !flying);
    }

    // C10's other half. `Z` appends a second glTF into the buffers the renderer already
    // binds, `H` gives the last one back -- which is the pair that proves the geometry
    // ranges are actually reclaimed rather than merely tracked.
    if (e.input().pressed(actions.addModel)) {
        // C21: placed where the camera is looking rather than dropped on the origin, which
        // is the difference between appending geometry and importing a *scene*. Successive
        // presses land in different places instead of stacking one file on top of itself,
        // and everything the document has -- its colliders, its lights -- arrives with it.
        const glm::vec3 at = e.camera().position() + e.camera().forward() * 6.0f;
        const auto id = e.addModel(core::Resources("res:/physics.gltf"), glm::translate(glm::mat4(1.0f), at));
        if (id != scene::GltfScene::kNoModel) loadedModels.push_back(id);
    }
    // C22, and the thing `appendModel` refused until it landed. A skinned file brings its
    // skeleton, its clips and its influences, and they are merged onto the rig the scene is
    // already animating rather than replacing it -- so the character already walking around
    // is still walking around afterwards.
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

    // C29, and the shortest demonstration of it there is: a respawn onto a moving thing.
    // The platform is the only surface in this scene a character cannot reach on foot, and
    // a rider is what `scripts/locomotion.sh`'s platform arm needs -- so the arm scripts
    // this press rather than the demo needing a spawn-position flag nothing else would use.
    //
    // The body's transform rather than the node's, because the body is what the character
    // will stand on; `halfExtent.y` is the platform's own 0.11, so the feet arrive on the
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

    // G4, and the whole of it from a game's side: a material made in code, geometry made
    // in code, into the buffers the renderer already binds. The cube is placed where the
    // camera is looking, one metre up, so it lands somewhere visible in any scene.
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
            // G5. The one field that decides which shader draws it; everything else about
            // the cube -- its buffers, its instance slot, its indirect command -- is what
            // it was before variants existed.
            m.shader = hologramVariant;
            // x band frequency in world units, y phase, z emissive gain, w roughness.
            // hologram.frag is the only thing that reads them.
            m.params = {6.0f, 0.0f, 2.5f, 0.25f};
            cubeMaterial = e.gltfScene().createMaterial(m);
        }
        if (cubeMaterial != 0xFFFFFFFFu) {
            // Two and a half metres up rather than one, and the extra metre and a half is
            // G7's: a cube that starts half a metre above the floor lands at walking pace
            // and a contact nobody can hear is a contact nobody can check.
            const glm::vec3 where = e.camera().focus + glm::vec3(0.0f, 2.5f, 0.0f);
            const auto id = e.createMesh(unitCube(cubeMaterial, glm::translate(glm::mat4(1.0f), where)));
            if (id != scene::GltfScene::kNoModel) {
                spawnedCubes.push_back(id);

                // G7, and the reason the cube is the demo's collision fixture rather than
                // anything in the scene: Sponza declares no collider at all, the demo's own
                // ground is one static box, and a `CharacterVirtual` is not in the broad
                // phase -- so before this there was nothing in the demo that could hit
                // anything.
                //
                // Three calls, and the middle one is G3's: a dynamic body attached to a
                // node pushes its transform *up* into the node, and the instance attached
                // below it follows. No offset, because the mesh was built around the
                // origin and placed at the same point the body was.
                scene::ColliderDesc desc;
                desc.name = "spawned cube";
                desc.shape = scene::ColliderShape::Box;
                desc.motion = scene::ColliderMotion::Dynamic;
                desc.halfExtent = glm::vec3(0.5f);
                desc.mass = 4.0f;
                desc.friction = 0.5f;
                // Enough to bounce once, so a single press produces a loud contact and a
                // quiet one and the volume scaling is audible rather than argued.
                desc.restitution = 0.35f;
                desc.transform = glm::translate(glm::mat4(1.0f), where);

                const scene::BodyId body = e.physics().createBody(desc);
                const std::span<const scene::InstanceId> instances = e.instancesOf(id);
                if (body.valid() && !instances.empty()) {
                    const scene::NodeId node = e.scene().create("spawned cube");
                    e.scene().attachBody(node, body);
                    e.scene().attachInstance(node, instances.front());
                    // **The instance a body drives is dynamic, and saying so is the
                    // caller's job.** `createMesh` builds every instance static -- nothing
                    // in a mesh says it will move -- so without this the cube is baked into
                    // the acceleration structure's static tier and then falls out of it,
                    // and `rebuildAccelIfStale` rebuilds the whole structure *every frame*
                    // for the rest of the run. The engine logs exactly that, once, and it
                    // took a trace to notice it had been.
                    e.instances().setFlags(instances.front(), scene::kInstanceDynamic, 0u);
                }
            }
        }
    }

    // And the mutable half. Written every frame while a cube exists, which is what the
    // material revision is for -- and never touched at all when none does, so a run that
    // spawns nothing costs nothing and renders what it always rendered.
    if (!spawnedCubes.empty() && cubeMaterial != 0xFFFFFFFFu) {
        scene::GpuMaterial m = e.gltfScene().material(cubeMaterial);
        const float t = static_cast<float>(e.renderer().frameCount()) * 0.03f;
        m.baseColorFactor = {0.55f + 0.45f * std::sin(t), 0.3f, 0.55f + 0.45f * std::cos(t), 1.0f};
        // The variant's own channel, advanced on the same path (G5). A material's
        // revision moves every frame here and the *variant assignment* does not, which is
        // the distinction the command builder's two counters exist to make: this costs a
        // memcpy of the material table and no regrouping at all.
        m.params.y = t * 0.5f;
        e.gltfScene().setMaterial(cubeMaterial, m);
    }

    // G3, and the whole of it from a game's side. Reparenting keeps the world transform,
    // so the torch does not jump on the frame it is picked up; the local position is then
    // written to put it in the character's hand rather than at their feet. The light
    // follows because it is attached to the node, and nothing in the renderer knows.
    if (e.input().pressed(actions.carry) && torchNode.valid() && world.playerNode().valid()) {
        carrying = !carrying;
        e.scene().setParent(torchNode, carrying ? world.playerNode() : scene::NodeId{});
        if (carrying) e.scene().setLocalPosition(torchNode, {0.3f, 1.3f, 0.2f});
        core::Logger::status(core::LogCategory::Scene, "Torch: %s", carrying ? "carried" : "put down");
        // A player who took the torch by hand has answered the goal, whatever the planner
        // was in the middle of. Cancelling here rather than letting `advance` notice keeps
        // the two from arguing over the same node on the same frame.
        fetching = false;
    }

    // C24, and the key that is *not* a movement. Everything else in this scheme says what
    // to do; this says what to want, and the character works out the rest.
    if (e.input().pressed(actions.fetch) && torchNode.valid() && world.playerNode().valid()) {
        ai::WorldState goal;
        goal.set(propCarrying, true);
        fetcher.setGoal(goal);
        fetching = true;
        fetchStep = ai::Agent::kNoAction;
        core::Logger::status(core::LogCategory::Scene, "Goal: carry the torch");
    }

    // C10's runtime caller. `X` re-streams the current scene on a worker: the parse comes
    // off the frame thread, the upload does not, and the log line reports both so the split
    // is visible rather than claimed.
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

        // C12's runtime caller. `V` asks the navmesh for a path from the character to
        // wherever the camera is standing, and the follower drives the same
        // `setCharacterInput` the movement keys do -- which is the point: navigation produces
        // a direction, not a teleport, and it goes through the character controller like
        // anything else. Manual input wins, so walking cancels the path rather than
        // fighting it.
        //
        // G13 changed what that sentence *means* in the demo's own scene without changing a
        // line of it: the camera now stands behind the character, so `V` there paths five
        // metres backwards rather than across the room. It is the same demonstration -- a
        // path, walked by the controller -- and the alternative was giving C12's call site a
        // target of its own, which is a decision belonging to a game with somewhere to go.
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

        // After `Camera::update` and not before it. The camera is resolved for this frame
        // before anything reads it: the engine ran its look, its zoom and -- since C37 --
        // its follow in `beginFrame`, and only then is "forward" asked for. Resolving before
        // the camera's own update would drive the character off the previous frame's yaw,
        // which is a lag no amount of the rest being right can remove.
        //
        // **Not while the flycam is in**, which is what lets both schemes ship on WASD. The
        // flycam declares `Camera.Forward` on W the moment it is installed; asking for a
        // movement direction underneath it would put one press on two things, and moving one
        // of them onto the arrow keys is precisely the dance C37 removed.
        const glm::vec3 manual =
            flying ? glm::vec3(0.0f) : playerActions.moveDirection(e.input(), e.camera());
        if (glm::length(manual) > 1e-4f) navFollower.reset({});

        glm::vec3 move = manual;
        if (!navFollower.done()) {
            const glm::vec3 desired = nav::steer(navFollower, here, 1.0f);
            // The controller wants a direction, not a velocity -- it owns the speed.
            if (glm::length(desired) > 1e-4f) move = glm::normalize(desired);
        }

        // ------------------------------------------------------- the goal (C24)
        // `G` hands over a goal and then stops being involved. From here the world state is
        // read off the world -- how far the torch is, and whether the tree says it is being
        // carried -- the planner is asked what to do about it, and the answer is executed
        // through the same `setCharacterInput` a key drives. **Manual input wins**, exactly
        // as it does over the navmesh follower: a player who takes hold of the character has
        // said what they want more recently than the goal did.
        if (fetching && torchNode.valid() && world.playerNode().valid()) {
            // Scoped so the *cost* of a decision layer is a number rather than an argument.
            // `advance` is cheap on the frames where nothing changed -- it compares one
            // action's prerequisites against the world -- and pays for a search only on the
            // frames where it re-plans, which is what "re-planned on event" means measured
            // rather than asserted.
            auto planZone = core::Profiler::scope("DemoGame::fetch");
            const glm::vec3 torch(e.scene().worldTransform(torchNode)[3]);
            const glm::vec3 toTorch = torch - here;
            const glm::vec3 flat(toTorch.x, 0.0f, toTorch.z);

            // Named `state` and not `world`, which is what it was called until the demo's own
            // `world` grew a `playerNode()` the block below reads -- a member the planner's
            // local was quietly shadowing.
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
                // The same two calls `Scene.Carry` makes. The planner decided *that* it
                // should happen; how a torch is picked up is still the game's.
                carrying = true;
                e.scene().setParent(torchNode, world.playerNode());
                e.scene().setLocalPosition(torchNode, {0.3f, 1.3f, 0.2f});
            }
        }

        // G12, corrected by C20. The demo used to keep a second flag here and animate the
        // jump off `flag && characterOnGround(...)` -- the controller's own decision,
        // re-derived from the outside, and correct only for as long as the two could not
        // disagree. They can now: a press inside the coyote window launches with no ground
        // under it, and a press inside the buffer launches a step or two after the frame it
        // arrived on. `characterJumped` replaced the flag entirely.
        e.physics().setCharacterInput(world.playerCharacter(), move, e.input().pressed(playerActions.jump));
    }
}

void DemoGame::playImpacts(Engine& e) {
    auto s = core::Profiler::scope("DemoGame::playImpacts");
    if (impactSound.file.empty() || !e.audio().active()) return;

    // D17. The game's own rows, read exactly as an engine row is read -- the handle carries
    // the type, so these are a `float` and a `bool` without a cast or a parse in sight.
    // Read per call rather than cached, because the generated panel writes them live.
    const float volumeScale = e.settingsTable().get(impactVolume);
    const bool dust = e.settingsTable().get(impactDust);

    for (const scene::PhysicsWorld::Contact& contact : e.physics().contacts()) {
        // A floor is touched constantly by anything resting on it, and a shot per graze is
        // a buzz. One metre a second is roughly a five-centimetre drop, which is the point
        // below which nothing sounds like it landed.
        if (contact.speed < 1.0f) continue;

        scene::AudioSourceDesc one = impactSound;
        // Loudness from the closing speed, saturating at 6 m/s -- a metre and a half of
        // fall. This is the whole reason `Contact::speed` is read before the solver
        // resolves the contact: taken afterwards every impact would be equally quiet.
        // ...scaled by this game's own row, which is a player's answer rather than the
        // engine's: `audio.masterVolume` moves the music and the ambience with it.
        one.volume = std::min(1.0f, contact.speed / 6.0f) * volumeScale;
        // And a pitch that varies, or a stack of crates settling sounds like a machine
        // gun. Hashed from the pair and the step rather than drawn from a generator, so a
        // locked run is still a function of the frame index and `--capture` still
        // reproduces -- the demo has no business being the one thing in the frame that is
        // not deterministic.
        const uint32_t noise = (contact.a.index * 2654435761u) ^ (contact.b.index * 40503u) ^
                               static_cast<uint32_t>(e.stepIndex());
        one.pitch = 0.9f + 0.2f * static_cast<float>(noise & 0xFFu) / 255.0f;

        e.audio().playAt(one, contact.point);

        // G9's other half of the same event, and it is C3's call rather than G7's: a
        // one-shot emitter aimed along the contact normal, which releases its own slot when
        // its last particle dies. The two live side by side here because a landing is one
        // event -- and because the arcs are explicit that `playAt` is G7's and spawning an
        // effect at a point is C3's, so a demo is where they finally meet.
        if (dust && world.dustReady) e.particles().spawnEffect(world.dust, contact.point, contact.normal);
        // At debug level, because a busy scene has several of these a second and a status
        // line each would drown the log -- but a run with `--log-level debug` is the only
        // way to see, headlessly, that a contact turned into a sound at all.
        core::Logger::debug(core::LogCategory::Scene, "Impact: bodies %u/%u at %.1f m/s, %.2f %.2f %.2f",
                            contact.a.index, contact.b.index, static_cast<double>(contact.speed),
                            static_cast<double>(contact.point.x), static_cast<double>(contact.point.y),
                            static_cast<double>(contact.point.z));
    }
}

void DemoGame::fixedUpdate(Engine& e, float step) {
    // G7 first, and outside the animator guard below deliberately: a scene with no rig in
    // it still has collisions, and putting this after the early return would make the
    // whole feature a property of whether the file happened to ship a skinned mesh.
    //
    // `fixedUpdate` runs *before* the engine's movers, so these are the previous step's
    // contacts -- one step of latency, and nothing is dropped, because every step's
    // contacts are drained by the next step's call to this.
    playImpacts(e);

    // G9. Ahead of the animator guard for the same reason `playImpacts` is: a scene with no
    // rig still has a platform, and putting this below the early return would make a
    // kinematic body a property of whether the file shipped a skinned mesh.
    stepDemoWorld(e, world, step);

    scene::SceneAnimator& animator = e.animator();
    if (animator.empty()) return;

    // A character with a physics controller under it has its gait driven by where it
    // actually is, rather than by the step-index triangle wave `driveCharacters` uses --
    // which is the payoff of having S2.4 and S4.4 in the same engine. A scene with an
    // animated mesh and no controller keeps the demo drive it always had.
    if (!world.playerCharacter().valid()) {
        driveCharacters(animator, e.stepIndex(), step);
        return;
    }

    driveLocomotion(e, step);
}

void DemoGame::driveLocomotion(Engine& e, float step) {
    auto s = core::Profiler::scope("DemoGame::driveLocomotion");
    scene::SceneAnimator& animator = e.animator();
    const scene::AnimationStateMachine& machine = animator.stateMachine();

    // **The parameter writing that used to be here is the engine's** (G15). It read
    // `characterSpeed`, divided by a magic 4.0, read `characterOnGround` and
    // `characterJumped`, then looped over every character writing three parameters and
    // firing one trigger -- eighty lines that worked for the one rig this demo has, because
    // the names, the divisor and the trigger set all belong to the rig rather than to a
    // game. `Engine::locomotion()` does it now, for every controller the scene paired with
    // a rig, and a second rig needs nothing added here.
    //
    // What is left is what genuinely is this game's: which joint holds root motion, how the
    // mesh turns to face where it went, and the trace G12 verifies the whole chain with.

    // ------------------------------------------------------------------ root motion (C7)
    // **The showcase clips are not authored in place, and this is the demo saying which node
    // carries that.** `walking` moves `Hips` 1.80 m down its own +Z over the clip, `running`
    // 3.20 m and the strafes 2.88 m sideways; only `mixamorig:Root` sits still, which is why
    // naming the rig's topmost joint would have named the one node with nothing on it.
    //
    // Left unnamed, the pose keeps that translation and the character is moved **twice** --
    // once by the controller below and once by the clip -- and then snaps back the instant the
    // machine blends to `idle`, whose hips stand still. That is a mesh sliding out from under
    // its own capsule and rubber-banding home on every release, and it is what this looked
    // like on screen.
    //
    // The demo's controller owns travel: `moveSpeed` comes off the collider, `locomotion.sh`
    // derives every distance from it, and the state machine reads the speed back out of the
    // solver. So this takes the motion **out of the pose and does not re-apply it** -- the
    // clips animate in place and the capsule is the only thing moving. A game that wanted the
    // animation to drive instead would hand `rootMotion(c) / step` to `setCharacterInput`,
    // which is the other half of the same call and is why it reports the delta at all.
    //
    // Set here rather than in `init` because C10 rebuilds the animator under `X`, which
    // returns this to `kNoNode`; guarded on that, so it is one call per scene rather than one
    // per step -- `setRootNode` restarts every character's measurement and calling it every
    // step would report a delta of zero forever.
    // `rootJoint` is resolved whether or not the hold is applied, and that is deliberate: the
    // `drift` number below measures this node, so a build with the `setRootNode` call taken out
    // still reports what the pose did. A check that only exists once the fix is in is not a
    // check on the fix.
    if (rootJoint == scene::SceneAnimator::kNoNode) rootJoint = animator.findNode("Hips");
    if (rootJoint != scene::SceneAnimator::kNoNode && animator.rootNode() != rootJoint) {
        animator.setRootNode(rootJoint);
        core::Logger::status(core::LogCategory::Scene, "Root motion: held at node %u (Hips)", rootJoint);
    }

    // **Nothing before the first sweep**, and this is now the *measurement's* guard rather
    // than the driver's. The engine writes its parameters after `PhysicsWorld::step`, so it
    // has no such moment to avoid; the numbers below read `characterTransform`, which before
    // the first step is the one the file authored rather than the one the solver resolved.
    if (e.stepIndex() == 0) return;

    const scene::PhysicsCharacterId player = world.playerCharacter();
    const bool grounded = e.physics().characterOnGround(player);
    const glm::vec3 at(e.physics().characterTransform(player, 0.0f)[3]);

    // ------------------------------------------------------------------- the trace (G12)
    const uint32_t state = animator.currentState(world.playerRig());
    if (!locomotion.active) {
        locomotion.active = true;
        locomotion.start = at;
        locomotion.previous = at;
    }
    // **A teleport is not travel.** `setCharacterTransform` moves the character without the
    // solver moving it, so the step that crosses the gap is one displacement the size of the
    // room -- and three of the sums below divide by exactly that. Re-seeded rather than
    // skipped, so the run's distances are what happened after the placement. `path`,
    // `changes` and `poseDrift` are left alone: none of them is a distance, and the machine's
    // answer either side of a teleport is one claim.
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
    // Horizontal, because gravity contributes nothing to it. A check on straight-line
    // displacement would be satisfied by a character that only ever fell.
    const glm::vec3 flat(moved.x, 0.0f, moved.z);
    const float carried = glm::length(flat);
    locomotion.travelled += carried;
    locomotion.peakRise = std::max(locomotion.peakRise, at.y - locomotion.start.y);
    locomotion.previous = at;

    // ------------------------------------------------------- the facing, and where it lives
    // **A rotation the game composes into the tree, not a `setCharacterFacing` on the
    // solver.** A setter would be a capability -- it belongs to whichever row wants Jolt to
    // know a heading, and this row wants a mesh turned. The node it goes on is the child the
    // loader made for the mesh, never `playerNode()` itself: the sweep writes
    // `characterTransform` straight into that node's world matrix and drops whatever TRS was
    // there, so a rotation set on it would be silently discarded every step. A child's local
    // transform is composed onto a parent whose world matrix is already final, which is a
    // turn about the character's own origin with nothing decomposed anywhere -- the property
    // G3 built `instanceOffset` to protect, kept.
    // **Every child named `mesh`, not the first one.** `Engine::bindPhysicsToScene` creates
    // one such child per *placement* bound to the body, and a rig is routinely several: the
    // Mixamo character here is `Beta_Surface` and `Beta_Joints`, two skinned meshes over one
    // skeleton and one capsule. Turning only the head of that list left the joint caps facing
    // whatever the file authored while the body turned to its heading -- the two halves of one
    // character pointing different ways, which is what this looked like on screen.
    //
    // Re-resolved when the list goes stale rather than cached forever: C10 replaces the whole
    // tree under `X`, which retires every id in here at once. Tested on the front rather than
    // on emptiness alone, because a list of stale handles is not empty -- the old single-node
    // spelling got that for free by re-resolving whenever its one id went bad.
    if (!facingNodes.empty() && !e.scene().valid(facingNodes.front())) facingNodes.clear();
    if (facingNodes.empty() && world.playerNode().valid()) {
        for (scene::NodeId c = e.scene().firstChild(world.playerNode()); c.valid(); c = e.scene().nextSibling(c)) {
            // By name rather than by position in the list, because the torch above is a
            // sibling of these and must not be turned with them.
            if (e.scene().name(c) == "mesh") facingNodes.push_back(c);
        }
    }
    if (world.built && !facingNodes.empty()) {
        // Where it *went*, not where it was asked to go. The two differ through every ramp
        // C20 added and through anything the solver slid the character along, and turning to
        // face the request would be the same mistake as animating a jump off the keypress.
        //
        // **`characterVelocity` and not the `flat` above, which is the same question asked of
        // the world.** The trace's displacement includes whatever is carrying the character,
        // and this demo has a kinematic platform sliding 2 m along Z: parked on it with
        // nothing pressed, the character turned to face the way the platform was travelling
        // and swung round again at each end of the run. The solver's velocity has the ground's
        // own motion taken out of it and is still a measurement of what happened.
        //
        // Below `kFacingFloor` the direction is rounding rather than a heading, so a
        // character standing still keeps the one it had rather than jittering about it.
        //
        // **The shortest arc, the rate clamp and the floor are `Scene::turnToward`'s** (C30),
        // and none of them was ever about being a character -- a boat, a turret and an agent
        // on a navmesh want the identical slew. What is left here is the game's half: which
        // nodes turn, and that they turn toward what the solver did rather than toward what
        // the input asked for. The offset is zero because the showcase rig is authored
        // looking down +Z, which is where the verb measures from; a rig authored otherwise
        // passes its own rather than having a sign flipped in here.
        const glm::vec3 gait = e.physics().characterVelocity(player);
        for (const scene::NodeId part : facingNodes) {
            e.scene().turnToward(part, gait, kTurnRate, step, kFacingFloor);
        }
    }

    // ------------------------------------------------------- what agreed with what (G13)
    // Three projections of the step above, each onto a direction sampled at this step. The
    // camera's are its own horizontal forward and right -- `Camera::forward()` and the cross
    // product `Camera::update` takes, which is the basis the movement was resolved against,
    // so a character that walked somewhere else lands in a different sum from the one that
    // grew. The facing comes back out of the *tree* rather than off the yaw the turn returned,
    // what makes it a check on the rotation having reached the scene at all.
    const glm::vec3 view = e.camera().forward();
    const glm::vec3 ahead(view.x, 0.0f, view.z);
    if (const float reach = glm::length(ahead); reach > 1e-4f) {
        const glm::vec3 forward = ahead / reach;
        locomotion.alongCamera += glm::dot(flat, forward);
        locomotion.acrossCamera += std::abs(glm::dot(flat, glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f))));
    }
    // **The worst of the character's parts, not the one the rotation was written to.** Reading
    // back the single node this function had just written made the number a restatement of
    // the angle just written, dressed as a measurement: it could not fall while that node
    // turned, whatever the rest of the character did. It is why eight arms passed with the
    // joint caps facing a fixed world axis and the body facing its heading. Taking the
    // minimum over every part makes one piece left behind drag the ratio down, so
    // `locomotion.sh`'s existing `facing >= 0.85` is what fails -- the arms did not need
    // changing, the number did.

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

    float worst = 1.0f;
    bool measured = false;
    for (const scene::NodeId part : facingNodes) {
        if (!e.scene().valid(part)) continue;
        // Column 2 is the node's +Z in world space, which is the axis the showcase rig is
        // authored looking down. As of the last sweep, so one step behind the displacement it
        // is projected onto -- the same step of latency every number in this function carries.
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

    // ------------------------------------------------- did the pose move it too? (root motion)
    // Read out of the animator's resolved transforms rather than off the clip, for the reason
    // `alongFacing` is read out of the tree: this has to be able to say that the *hold* took,
    // not that the engine was asked for one. `worldTransforms` is indexed by rig node and is
    // model space -- the character's own -- so a held root is a constant here and a clip
    // walking the rig forward is a ramp with a cliff at every loop and every blend.
    if (rootJoint != scene::SceneAnimator::kNoNode) {
        const std::vector<glm::mat4>& pose = animator.worldTransforms(world.playerRig());
        if (rootJoint < pose.size()) {
            const glm::vec3 here(pose[rootJoint][3]);
            if (!locomotion.posed) {
                locomotion.poseRoot = here;
                locomotion.posed = true;
            }
            // **Horizontal, and the vertical axis is the whole of why** -- the same
            // projection every other number in this function takes, and for the same
            // reason. `setRootNode` holds X and Z at the bind translation and *deliberately
            // keeps Y*: a rig binds in a T-pose and animates with bent knees, so holding Y
            // stood this character eight centimetres off the floor and flattened the bob
            // out of every walk cycle. A drift measured over all three axes therefore
            // counts the hips' authored rise and fall as a defect -- 0.05 m for a walk
            // cycle, 0.50 m through `jumping up` and `hard landing` -- against a bound of
            // 0.02 that only the standing arms could ever meet.
            //
            // The counterfactual is unchanged by this and is what says the number still
            // works: with `setRootNode` removed the clips walk the rig *along the floor*,
            // which is exactly what this projection keeps.
            const glm::vec3 moved = here - locomotion.poseRoot;
            locomotion.poseDrift = std::max(locomotion.poseDrift, glm::length(glm::vec3(moved.x, 0.0f, moved.z)));
        }
    }

    if (state == locomotion.state || state >= machine.states.size()) return;
    const std::string& name = machine.states[state].name;
    const bool entry = locomotion.state >= machine.states.size();
    // The entry state is not a transition and is not counted as one, but it is the head of
    // the path -- a sequence that did not say where it started is one nobody can read.
    if (!entry) {
        core::Logger::status(core::LogCategory::Scene, "Locomotion: %s -> %s at step %llu (%.2f m/s, %s)",
                             machine.states[locomotion.state].name.c_str(), name.c_str(),
                             static_cast<unsigned long long>(e.stepIndex()),
                             static_cast<double>(e.physics().characterSpeed(player)),
                             grounded ? "grounded" : "airborne");
        ++locomotion.changes;
    }
    locomotion.state = state;
    // Capped rather than unbounded: a run nobody is scripting changes state all day, and
    // the summary line this feeds is a claim about a bounded scripted sequence.
    if (locomotion.changes <= 32) locomotion.path += (locomotion.path.empty() ? "" : " > ") + name;
}

void DemoGame::shutdown(Engine& e) {
    // The engine's pointer is non-owning and both cameras are members of this object, so
    // handing it back is what stops a later frame updating freed memory. Nothing here
    // outlives the engine today; the rule is the method's, not this game's situation.
    e.setCamera(nullptr);

    // G9's first number, reported where it is finally answerable. `droppedSpawns()` counts
    // particles an emitter asked for and the pool had no room for since the last
    // `setEmitters`, so it is only meaningful at the end of a run -- and a budget that
    // truncated in silence is the thing 0.9 exists to forbid. The renderer reports it when
    // it *changes*; this reports it when there is nothing left to change it.
    if (world.built) {
        core::Logger::status(core::LogCategory::Scene, "Demo world: pool %u, %u spawns dropped over %llu steps",
                             e.particles().capacity(), e.particles().droppedSpawns(),
                             static_cast<unsigned long long>(e.stepIndex() + 1));
    }

    // G12, and the counterpart of the line above: the state machine's answer, reported
    // where the run is over and there is nothing left to change it. The path is what a
    // scripted check asserts against -- it is the *order* the transitions happened in,
    // which is the one thing a golden image of a character mid-blend cannot be a claim
    // about -- and the two distances are what say the character was carried rather than
    // merely animated. Emitted for a run that pressed nothing too, where it reads `idle`
    // and two zeroes, because that is the negative control.
    if (locomotion.active) {
        core::Logger::status(core::LogCategory::Scene, "Locomotion path: %s", locomotion.path.c_str());
        // **Appended to rather than rewritten**, and the four fields G13 added are on the
        // end for exactly that reason: `locomotion.sh` parses this line with a pattern that
        // ends in `.*`, so the three arms that predate the row keep matching unchanged. The
        // ratios are metres over metres and each is a direction the run can be asserted
        // against; a run that stood still divides nothing by nothing and reports zeroes.
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
                             // Metres, not a ratio: it is a distance the pose moved and there
                             // is nothing to normalise it against -- a clip drags the rig the
                             // same way whether the character walked a metre or ten.
                             static_cast<double>(locomotion.poseDrift),
                             // Radians, and appended for the reason G13's four were: the
                             // patterns that predate it end in `.*` and keep matching.
                             static_cast<double>(locomotion.turned));
    }

    // P1's other half, at a call site rather than only in the suite. The engine would
    // free the image anyway; what this demonstrates is that a game *can* give one back,
    // which is the whole of what `kMaxOverlayImages` and its missing unload could not do.
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
    // Absent, or written by a build that knew a shape this one does not. Neither is fatal
    // for a demo -- the engine's half already applied -- so it says so and leaves its own
    // state alone, which is the atomicity `Game::load` says is the game's to keep.
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

    // Clamped to the window, and this is where S6.5 stops being a multiplication. A panel
    // sized in logical units is 1120 pixels tall at 200%, which is taller than a
    // 900-pixel window -- so the scale that made every widget correct would have put half
    // of them off the bottom of the screen. The panel scrolls, so shrinking it loses
    // nothing.
    const glm::vec2 panelPos{s.get(panelX) * scale, s.get(panelY) * scale};
    const glm::vec2 panelSize{std::min(s.get(panelWidth) * scale, screenW - panelPos.x - 8.0f * scale),
                              std::min(s.get(panelHeight) * scale, screenH - panelPos.y - 8.0f * scale)};
    drawSettingsPanel(ui, e, panelState, panelPos, panelSize);

    // 5.6, beside the settings panel rather than inside it. Two panels is what S6's
    // carried-forward note said nothing had yet needed -- and the thing it predicted
    // would follow is here too: they cannot both sit at the config's one position, so the
    // inspector is placed to the right of the first and clamped the same way. Dragging
    // them is still not built; a second *fixed* panel is what an inspector needs, and a
    // movable one is what a third would.
    if (!inspectorOpen) return;
    const glm::vec2 inspectorPos{panelPos.x + panelSize.x + 8.0f * scale, panelPos.y};
    const float inspectorWidth =
        std::min(s.get(panelWidth) * scale, screenW - inspectorPos.x - 8.0f * scale);
    // Only when there is room for it. At 200% on a narrow window the first panel already
    // reaches the edge, and a second one clamped to a negative width draws a title bar
    // over the scene and nothing else.
    if (inspectorWidth <= 80.0f * scale) return;

    // The column is split rather than a third one opened beside it: what a table holds and
    // what the tree holds are two views of the same selection, and a reader compares them.
    // Two panels of half the height each is also what fits -- a third column at this width
    // is off the right-hand edge on anything but a wide screen.
    const float half = (panelSize.y - 8.0f * scale) * 0.5f;
    ui::drawInstanceInspector(ui, e.instances(), inspectorState, inspectorPos, {inspectorWidth, half});
    // G6, and it is here rather than in the engine for the reason `Inspector.h` gives about
    // `drawSettingsPanel`: the panel is the engine's, and where a game puts it is the
    // game's.
    ui::drawNodeInspector(ui, e.scene(), nodeInspectorState, {inspectorPos.x, inspectorPos.y + half + 8.0f * scale},
                          {inspectorWidth, half});
}

SUBSTRATE_GAME(DemoGame)
