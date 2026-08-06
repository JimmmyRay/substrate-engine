#include "Harness.h"
#include "HarnessLog.h"

#include "Process.h"
#include "Repo.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr int kTimeout = 180;

/// The showcase character, from its own glTF: 3.2 m/s of travel and a 4.2 m/s launch, on the
/// engine's -9.81 gravity. Everything asserted below is derived from these numbers and from
/// two clip lengths on the rig, so a tuning change moves the expectation here rather than
/// silently invalidating it.
///
/// **These mirror `game/demo/DemoGame.cpp` and the harness cannot include it.** The CLI links
/// nothing under `game/` -- that boundary is what makes a game dependency reaching the engine
/// a link error -- so the compiler cannot hold these in step. Each carries the symbol it
/// copies; changing one means changing both.
constexpr double kStepHz = 60.0;
constexpr double kMoveSpeed = 3.2;    // DemoGame.cpp, the collider's moveSpeed
constexpr double kWalkFraction = 0.45; // DemoGame.cpp kWalkFraction
constexpr double kJumpSpeed = 4.2;
constexpr double kLaunchClip = 0.25;  // `jumping up`
constexpr double kLandingClip = 2.0;  // `hard landing`
constexpr double kGravity = 9.81;

/// C20's motion model. `showcase.gltf` authors none of these, so they are `ColliderDesc`'s
/// defaults -- `engine/scene/Collider.h`, which this could include and deliberately does not:
/// what is being asserted is the number the *demo's* collider ends up with, and reading it
/// from the header would make the check agree with itself when the two disagree.
constexpr double kAccel = 10.0;      // ColliderDesc::acceleration, m/s^2
constexpr double kDecel = 40.0;      // ColliderDesc::deceleration, m/s^2
constexpr int kBufferSteps = 10;     // ColliderDesc::jumpBufferSteps

/// The two gait thresholds, as speeds. The machine compares a normalised speed against the
/// collider's own `moveSpeed`, so the authored 0.66 and 0.2 are 2.11 m/s and 0.64 m/s.
/// Derived from `kMoveSpeed` rather than written out, because the demo used to divide by a
/// literal 4.0 against a collider that tops out at 3.2 -- so the top fifth of every blend was
/// unreachable and nothing anywhere could have said so.
constexpr double kRunSpeed = 0.66 * kMoveSpeed;

/// The frames the two mid-air presses sit on, so the assertions and the scripts cannot drift
/// apart.
constexpr int kBufferedPress = 108;
constexpr int kEatenPress = 85;

struct Arm {
    const char* name;
    int frames;
    const char* script;
    std::vector<std::string> extra;
};

std::vector<Arm> arms() {
    // 180 steps of walking is 4.2 m, which keeps the character inside the atrium in every
    // direction. The direction is the assertion; the distance is the main arm's job.
    const char* walk = "60:Player.Forward+,240:Player.Forward-";
    const char* at = "0,1,0.9";

    return {
        {"still", 600, "", {}},
        {"modifier", 300, "60:Player.Run+", {}},
        {"walk-run-jump", 600,
         "60:Player.Forward+,150:Player.Run+,240:Player.Run-,300:Player.Forward-,330:Player.Jump",
         {}},
        // Both jump from a standstill at frame 60 and press jump again *in mid-air*. A jump
        // pressed while grounded proves nothing about a jump buffer: the controller this row
        // replaced would have passed it.
        {"jump-buffered", 400, "60:Player.Jump,108:Player.Jump", {}},
        {"jump-eaten", 400, "60:Player.Jump,85:Player.Jump", {}},
        // The only arms that name a camera; the first five must keep inheriting
        // `Camera::frameBounds` or their numbers stop being the numbers C20 derived.
        {"camera-north", 400, walk, {"--camera", std::string(at) + ",0,-10,5"}},
        {"camera-south", 400, walk, {"--camera", std::string(at) + ",180,-10,5"}},
        {"camera-turning", 400, walk,
         {"--camera", std::string(at) + ",0,-10,5", "--camera-spin", "1.5"}},
        {"platform-ride", 600, "30:Scene.RidePlatform", {}},
    };
}

} // namespace

int cmdLocomotion(const std::vector<std::string>& args) {
    Config config = Config::Release;
    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate locomotion [config]\n"
                       "\n"
                       "Drives game/demo through nine scripted arms and asserts the state-machine\n"
                       "path and the step numbers the log reports against the motion model.\n",
                       stderr);
            return 0;
        }
        if (const std::optional<Config> parsed = parseConfig(arg)) {
            config = *parsed;
            continue;
        }
        std::fprintf(stderr, "error: unknown argument '%s' (want: %s)\n", arg.c_str(),
                     configList().c_str());
        return 1;
    }

    const fs::path dir = repoRoot() / "debug_frames" / "locomotion";
    std::error_code ec;
    fs::create_directories(dir, ec);

    Report report;
    const std::vector<Arm> all = arms();

    for (const Arm& arm : all) {
        const std::string name = arm.name;
        const fs::path logPath = dir / (name + ".log");

        std::printf("== %s (%d frames%s%s)\n", name.c_str(), arm.frames,
                    *arm.script ? ", " : "", arm.script);

        std::vector<std::string> command{selfPath().string(), "run", "demo", tool::name(config),
                                         "--", "--headless", "--locked", "--audio-null",
                                         "--frames", std::to_string(arm.frames)};
        if (*arm.script) command.insert(command.end(), {"--input-script", arm.script});
        command.insert(command.end(), arm.extra.begin(), arm.extra.end());

        RunOptions options;
        options.cwd = repoRoot();
        options.timeoutSeconds = kTimeout;
        const RunResult result = run(command, options);

        // Stripped once here rather than in each pattern below: an anchor that silently never
        // matches is exactly the assertion-shaped hole this harness exists to close.
        const std::string log = stripAnsi(result.out + result.err);
        std::ofstream(logPath, std::ios::binary) << log;

        if (!result.ok()) {
            std::fputs(log.c_str(), stderr);
            std::fprintf(stderr, "%s failed to run\n", name.c_str());
            return 1;
        }

        // A script naming an action this build does not have, and a script running past
        // `--frames`, both look exactly like a feature that does not work.
        if (log.find("ERROR") != std::string::npos) {
            report.fail(name + " logged an error");
        }

        // The two clips have to be *found by name on the rig*, which is what makes the
        // machine six states rather than four with two written into the source.
        if (log.find("state machine over 6 states (idle, walk, run, jump, fall, land)") ==
            std::string::npos) {
            report.fail(name + ": the machine is not the six states built by name");
        }

        // The player takes W, A, S, D, shift and space, and takes them *unmoved* -- no `*`,
        // which is what tells a shipped default apart from a live rebind.
        const std::string bindings = after(log, "Shipped bindings: ");
        if (bindings.find("Player.Forward=W Pad.LeftY-") == std::string::npos ||
            bindings.find("Player.Run=LeftShift") == std::string::npos) {
            report.fail(name + ": the shipped scheme is not WASD for the player -- " + bindings);
        }
        if (bindings.find('*') != std::string::npos) {
            report.fail(name + ": an action is off its default -- " + bindings);
        }

        const std::string cameraRows = after(log, "Shipped camera (follow): ");
        if (cameraRows != "Camera.Orbit=Mouse.Middle") {
            report.fail(name + ": the follow camera is not the only live camera scheme -- '" +
                        cameraRows + "'");
        }

        const std::string path = after(log, "Locomotion path: ");
        const std::string summary = lineWith(log, " changes over ");
        const std::optional<double> changes = number(summary, "Locomotion: ");
        const std::optional<double> travelled = number(summary, " steps, ");
        const std::optional<double> rise = number(summary, "peak rise ");
        const std::optional<double> net = number(summary, " net ");
        const std::optional<double> along = number(summary, " along ");
        const std::optional<double> across = number(summary, " across ");
        const std::optional<double> facing = number(summary, " facing ");
        const std::optional<double> drift = number(summary, " drift ");
        const std::optional<double> turned = number(summary, " turned ");

        if (!net) report.fail(name + ": the summary line carries no net displacement");

        // **Asserted on every arm, including the ones that press nothing.** How far the
        // *pose* carried the rig's root, worst over the run -- a number about the animation
        // rather than about the solver, and the only one here that is. The showcase clips are
        // not authored in place, so without the root hold the drawn character travels at the
        // sum of pose and controller and rubber-bands home on every release.
        if (!drift) {
            report.fail(name + ": the summary line carries no pose drift");
        } else {
            report.within(*drift, 0.0, 0.02,
                          name + ": the pose moved the rig's root " + format(*drift) +
                              " m -- root motion is not being held");
        }
        if (!turned) report.fail(name + ": the summary line carries no turn");

        const double travel = travelled.value_or(0.0);
        const int changeCount = static_cast<int>(changes.value_or(-1));

        if (name == "still" || name == "modifier") {
            // The character is asked for nothing it can act on, so nothing happens. Not
            // "roughly nothing" -- it is standing on a static box under a locked clock, and a
            // millimetre of drift here is a defect with a cause.
            if (path != "idle") report.fail(name + ": path is '" + path + "', expected 'idle'");
            if (changeCount != 0) {
                report.fail(name + ": " + std::to_string(changeCount) +
                            " state changes, expected none");
            }
            report.within(travel, 0.0, 0.01,
                          name + ": travelled " + format(travel) + " m with nothing pressed");
            report.within(rise.value_or(1.0), 0.0, 0.01,
                          name + ": rose " + format(rise.value_or(1.0)) + " m with nothing pressed");
            report.within(net.value_or(1.0), 0.0, 0.01,
                          name + ": net displacement " + format(net.value_or(1.0)) +
                              " m with nothing pressed");
            report.within(along.value_or(1.0), 0.0, 0.01,
                          name + ": went " + format(along.value_or(1.0)) +
                              " along the camera with nothing pressed");

        } else if (name == "walk-run-jump") {
            const std::string expected =
                "idle > walk > run > walk > idle > jump > fall > land > idle";
            if (path != expected) {
                report.fail(name + ": path is '" + path + "', expected '" + expected + "'");
            }
            if (changeCount != 8) {
                report.fail(name + ": " + std::to_string(changeCount) +
                            " state changes, expected 8");
            }

            // 90 steps of walk, 90 of run, 60 of walk, at 60 Hz. Stated as arithmetic because
            // a number lifted off a passing run is a number that cannot be wrong.
            const double walkSpeed = kMoveSpeed * kWalkFraction;
            const double instant =
                (90.0 * walkSpeed + 90.0 * kMoveSpeed + 60.0 * walkSpeed) / kStepHz;

            // A linear ramp between two speeds covers dv^2/2a less ground than arriving
            // instantly would -- so the two accelerations are debits and the two
            // decelerations, which coast *past* the new request, are credits.
            const double gap = kMoveSpeed - walkSpeed;
            const double want = instant - walkSpeed * walkSpeed / (2 * kAccel) -
                                gap * gap / (2 * kAccel) + gap * gap / (2 * kDecel) +
                                walkSpeed * walkSpeed / (2 * kDecel);
            report.within(travel, want * 0.97, want * 1.03,
                          name + ": travelled " + format(travel) + " m, expected about " +
                              format(want, 2) + " m");

            // **And it has to be short of the instant model by more than rounding.** The 3%
            // band still contains the pre-ramp figure, because the four ramps very nearly
            // cancel -- so on its own it would pass for a controller with no ramp in it at all.
            const double ceiling = instant - (instant - want) / 2;
            report.within(travel, 0.0, ceiling,
                          name + ": travelled " + format(travel) +
                              " m, which is not below the " + format(ceiling) +
                              " m a controller with no ramp would give");

            // A rise is the one thing gravity cannot supply, so the check is on the apex:
            // v^2/2g, plus up to one step of overshoot.
            const double apex = kJumpSpeed * kJumpSpeed / (2 * kGravity);
            report.within(rise.value_or(0.0), apex, apex + kJumpSpeed / kStepHz,
                          name + ": peak rise " + format(rise.value_or(0.0)) +
                              " m, expected between " + format(apex) + " and one step above it");

            const std::optional<int> jumpStep = transitionStep(log, "Locomotion", "idle", "jump");
            const std::optional<int> fallStep = transitionStep(log, "Locomotion", "jump", "fall");
            const std::optional<int> landStep = transitionStep(log, "Locomotion", "fall", "land");
            const std::optional<int> recoverStep = transitionStep(log, "Locomotion", "land", "idle");
            const std::optional<int> gaitUp = transitionStep(log, "Locomotion", "walk", "run");
            const std::optional<int> gaitDown = transitionStep(log, "Locomotion", "run", "walk");

            const std::pair<const char*, std::optional<int>> required[] = {
                {"idle->jump", jumpStep},  {"jump->fall", fallStep},
                {"fall->land", landStep},  {"land->idle", recoverStep},
                {"walk->run", gaitUp},     {"run->walk", gaitDown},
            };
            bool complete = true;
            for (const auto& [label, step] : required) {
                if (!step) {
                    report.fail(name + ": " + label + " never happened");
                    complete = false;
                }
            }
            if (!complete) break;

            // **C20's central assertion**, and the one number here a controller without a ramp
            // cannot produce. The modifier goes down at 150 and the request jumps from 1.44 to
            // 3.20; the machine changes gait when the *solver* reports more than the run
            // threshold. A ramp at 10 m/s^2 takes 7.2 steps to get there before the six steps
            // of pipeline latency even start.
            const double rampUp = (kRunSpeed - walkSpeed) / kAccel * kStepHz;
            report.within(*gaitUp, 150 + rampUp, 150 + rampUp + 7,
                          name + ": walk -> run at step " + std::to_string(*gaitUp) +
                              ", expected " + format(rampUp) + " steps of ramp after 150");

            // **G12's central assertion, kept.** Nothing about the movement key changed at
            // 240; only the modifier was released. The ramp barely moves this one, and that
            // asymmetry is the deceleration row doing what it is for.
            const double rampDown = (kMoveSpeed - kRunSpeed) / kDecel * kStepHz;
            report.within(*gaitDown, 240 + rampDown, 240 + rampDown + 7,
                          name + ": run -> walk at step " + std::to_string(*gaitDown) +
                              ", expected " + format(rampDown) + " steps of ramp after 240");

            const double launch = kLaunchClip * kStepHz;
            report.within(*fallStep - *jumpStep, launch, launch + 6,
                          name + ": jump -> fall " + std::to_string(*fallStep - *jumpStep) +
                              " steps after the launch, expected about " + format(launch));

            // The landing is ballistics: 2v/g seconds in the air.
            const double flight = 2 * kJumpSpeed / kGravity * kStepHz;
            report.within(*landStep - *jumpStep, flight - 6, flight + 6,
                          name + ": fall -> land " + std::to_string(*landStep - *jumpStep) +
                              " steps after the launch, expected about " + format(flight));

            // The landing plays out because the character is standing still.
            const double recovery = kLandingClip * kStepHz;
            report.within(*recoverStep - *landStep, recovery - 6, recovery + 6,
                          name + ": land -> idle " +
                              std::to_string(*recoverStep - *landStep) +
                              " steps after landing, expected about " + format(recovery));

        } else if (name == "jump-buffered" || name == "jump-eaten") {
            report.within(travel, 0.0, 0.01,
                          name + ": travelled " + format(travel) +
                              " m with nothing but jump pressed");

            const std::optional<int> firstJump = transitionStep(log, "Locomotion", "idle", "jump");
            const std::optional<int> landStep = transitionStep(log, "Locomotion", "fall", "land");
            const std::optional<int> secondJump = transitionStep(log, "Locomotion", "land", "jump");
            if (!firstJump) report.fail(name + ": the character never jumped at all");
            if (!landStep) report.fail(name + ": the character never landed");
            if (!firstJump || !landStep) break;

            // `fall -> land` is reported two steps after the sweep that found the ground, so
            // the last step on which a press is still a press *in the air* is landStep - 3,
            // and the earliest the buffer can still carry is landStep - 2 - kBufferSteps.
            //
            // Asserted rather than assumed: if the solver ever lands a few steps differently
            // these two arms stop testing the window, and this makes that a failure with a
            // legible message instead of a pair that quietly passes for the wrong reason.
            const int press = name == "jump-buffered" ? kBufferedPress : kEatenPress;
            const int windowOpens = *landStep - 2 - kBufferSteps;
            const int airborneUntil = *landStep - 3;
            if (press > airborneUntil) {
                report.fail(name + ": the press at " + std::to_string(press) +
                            " was not in the air -- the landing reported at " +
                            std::to_string(*landStep));
            }

            if (name == "jump-buffered") {
                if (press < windowOpens) {
                    report.fail(name + ": the press at " + std::to_string(press) +
                                " is outside the buffer, which opens at " +
                                std::to_string(windowOpens));
                }
                const std::string expected =
                    "idle > jump > fall > land > jump > fall > land > idle";
                if (path != expected) {
                    report.fail(name + ": path is '" + path + "', expected '" + expected + "'");
                }
                if (changeCount != 7) {
                    report.fail(name + ": " + std::to_string(changeCount) +
                                " state changes, expected 7");
                }
                if (!secondJump) {
                    report.fail(name + ": the buffered press never became a jump");
                } else if (*secondJump <= press) {
                    // It launched at the landing rather than on the frame it was pressed,
                    // which is the difference between a buffer and a mid-air jump.
                    report.fail(name + ": the second launch was at " +
                                std::to_string(*secondJump) + ", on or before the press at " +
                                std::to_string(press));
                }
            } else {
                // **The negative arm.** The only difference from the arm above is the frame
                // the second press sits on, and it sits outside the window.
                if (press >= windowOpens) {
                    report.fail(name + ": the press at " + std::to_string(press) +
                                " is inside the buffer, which opens at " +
                                std::to_string(windowOpens));
                }
                const std::string expected = "idle > jump > fall > land > idle";
                if (path != expected) {
                    report.fail(name + ": path is '" + path + "', expected '" + expected + "'");
                }
                if (changeCount != 4) {
                    report.fail(name + ": " + std::to_string(changeCount) +
                                " state changes, expected 4");
                }
                if (secondJump) {
                    report.fail(name + ": a press " + std::to_string(*landStep - press) +
                                " steps before the landing still jumped, at " +
                                std::to_string(*secondJump));
                }
            }

        } else if (name.rfind("camera-", 0) == 0) {
            // One key, held for 180 steps, in all three. The gait is checked so an arm that
            // stopped walking cannot pass its direction assertion by having no direction.
            const std::string expected = "idle > walk > idle";
            if (path != expected) {
                report.fail(name + ": path is '" + path + "', expected '" + expected + "'");
            }
            report.within(travel, 3.0, 4.5,
                          name + ": travelled " + format(travel) +
                              " m, expected a 180-step walk");
            report.within(rise.value_or(1.0), 0.0, 0.01,
                          name + ": rose " + format(rise.value_or(1.0)) +
                              " m with nothing but a movement key pressed");

            // **The row's assertion.** The displacement projected onto the camera's own
            // forward, over the distance walked. Not "the character moved" -- which every arm
            // above already proves and which a heading 180 degrees out satisfies as well.
            report.within(along.value_or(0.0), 0.85, 1.01,
                          name + ": went " + format(along.value_or(0.0)) +
                              " along the camera's forward, expected about 1");
            // The axes are not merely un-flipped, they are the right two: a basis that swapped
            // forward for right passes the line above at any yaw where the two are symmetric.
            report.within(across.value_or(1.0), 0.0, 0.30,
                          name + ": " + format(across.value_or(1.0)) +
                              " of the walk was across the camera, expected about 0");
            // Read off the scene node rather than off the angle the game wrote, so a rotation
            // the sweep discarded reads as zero here.
            report.within(facing.value_or(0.0), 0.85, 1.01,
                          name + ": faced " + format(facing.value_or(0.0)) +
                              " of the way it walked, expected about 1");

            if (name == "camera-turning") {
                // **The arm a fixed basis cannot pass.** The camera turns 1.5 degrees a frame
                // while the key is held, so a character that agrees with it walks a curve and
                // ends up near where it started; one resolving against anything fixed walks a
                // straight line and ends a whole path length away.
                report.within(net.value_or(travel), 0.0, travel * 0.5,
                              name + ": net " + format(net.value_or(travel)) + " m of " +
                                  format(travel) +
                                  " m walked -- that is a straight line, not a turn");
            } else {
                report.within(net.value_or(0.0), travel * 0.9, travel,
                              name + ": net " + format(net.value_or(0.0)) + " m of " +
                                  format(travel) + " m walked -- the path was not straight");
            }

        } else if (name == "platform-ride") {
            // **The arm the other nine could not be.** All of them stand the character on
            // ground that does not move, and the one thing a heading derived from world
            // displacement gets wrong is a character whose ground does.
            if (log.find("Placed the character on the platform") == std::string::npos) {
                report.fail(name + ": the placement never happened, so this arm rode nothing");
            }
            if (path != "idle") {
                report.fail(name + ": path is '" + path +
                            "', expected 'idle' -- a rider is not walking");
            }
            if (changeCount != 0) {
                report.fail(name + ": " + std::to_string(changeCount) +
                            " state changes riding a platform with nothing pressed");
            }
            report.within(travel, 4.0, 9.0,
                          name + ": carried " + format(travel) +
                              " m, expected about 6 over a full reversal");
            report.within(rise.value_or(1.0), 0.0, 0.01,
                          name + ": rose " + format(rise.value_or(1.0)) +
                              " m riding a level platform");

            // **The row's assertion, and it is not zero.** The character is dropped onto a
            // platform already doing 0.9 m/s and dragged up to its speed over the acceleration
            // ramp, so for those few steps its ground-relative motion really is 0.9 m/s
            // backwards -- a heading, and the rig faces it. Reading the heading off world
            // displacement instead scores 3.14 on this arm, measured.
            report.within(turned.value_or(9.0), 0.0, 0.5,
                          name + ": turned " + format(turned.value_or(9.0)) +
                              " rad with nothing pressed -- the platform is steering it");
        }

        std::printf("   path: %s\n", path.c_str());
        std::printf("   %d changes, %s m travelled, peak rise %s m\n", changeCount,
                    format(travel).c_str(), format(rise.value_or(0.0)).c_str());
        std::printf("   net %s m, %s along the camera, %s across it, %s faced, %s m of pose "
                    "drift\n",
                    format(net.value_or(0.0)).c_str(), format(along.value_or(0.0)).c_str(),
                    format(across.value_or(0.0)).c_str(), format(facing.value_or(0.0)).c_str(),
                    format(drift.value_or(0.0)).c_str());
        std::printf("   turned %s rad\n", format(turned.value_or(0.0)).c_str());
    }

    if (report.failures != 0) {
        std::fprintf(stderr, "locomotion: %d assertion(s) failed\n", report.failures);
        return 1;
    }
    std::printf("locomotion: %zu of %zu arms pass\n", all.size(), all.size());
    return 0;
}

} // namespace tool
