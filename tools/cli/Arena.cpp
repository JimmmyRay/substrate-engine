#include "Harness.h"
#include "HarnessLog.h"

#include "Process.h"
#include "Repo.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace tool {
namespace {

namespace fs = std::filesystem;

constexpr int kTimeout = 180;

/// The fighter's collider, from `game/battle_arena/ArenaWorld.cpp`. Everything asserted below
/// is derived from these, so a tuning change moves the expectation here rather than silently
/// invalidating it.
///
/// **These mirror the game and the harness cannot include it.** The CLI links nothing under
/// `game/` -- that boundary is what makes a game dependency reaching the engine a link error.
/// Each carries the symbol it copies; changing one means changing both.
constexpr double kStepHz = 60.0;
constexpr double kMoveSpeed = 3.2;      // ArenaWorld.cpp kMoveSpeed
constexpr double kJumpSpeed = 4.2;      // ArenaWorld.cpp kJumpSpeed
constexpr double kCapsuleRadius = 0.3;  // ArenaWorld.cpp kCapsuleRadius
constexpr double kWalkFraction = 0.45;  // BattleArenaGame.cpp kWalkFraction
constexpr double kGravity = 9.81;
constexpr double kPi = 3.14159265358979323846;

/// `ColliderDesc`'s defaults, which is what the arena's collider leaves them at.
constexpr double kAccel = 10.0;
constexpr double kDecel = 40.0;

/// Phase 3's numbers: `kPursuitStandOff` and `kPursuitDeadBand` from `ArenaWorld.cpp`, and the
/// distance between the two spawns it authors.
constexpr double kStandOff = 1.5;
constexpr double kDeadBand = 0.5;
constexpr double kSpawnGap = 12.0;

/// The `run` threshold `fighterMachine` authors, as a speed. Derived rather than written out,
/// because the whole point of the row that introduced it was that the divisor stopped being a
/// number a game guessed at.
constexpr double kRunSpeed = 0.66 * kMoveSpeed;

/// `kPlayerSpawn`, and the nearest column of the grid `arena.glb` authors. Read out of the
/// document rather than eyeballed: the `column` arm has to walk *dead centre* into one or the
/// capsule slides around it and the distance it stops at stops being arithmetic.
constexpr double kPlayerX = -6.0;
constexpr double kPlayerZ = 0.0;
constexpr double kColumnX = -9.0232;
constexpr double kColumnZ = 9.9297;
constexpr double kColumnRadius = 1.0;

/// Where the camera has to point for "forward" to be at that column, in degrees.
///
/// Yaw is measured from +Z -- `atan2(x, z)` and never `atan2(z, x)` -- which is where the rig
/// is authored looking and what `Camera::forward()` is built from.
double columnYaw() {
    return std::atan2(kColumnX - kPlayerX, kColumnZ - kPlayerZ) * 180.0 / kPi;
}

/// How far the fighter gets: to the centre, less the column's radius and the capsule's. A
/// capsule against a cylinder touches along the line between the two axes, so this is exact
/// rather than approximate, and it is what makes "it stopped" distinguishable from "it stopped
/// somewhere".
double columnReach() {
    const double dx = kColumnX - kPlayerX;
    const double dz = kColumnZ - kPlayerZ;
    return std::sqrt(dx * dx + dz * dz) - kColumnRadius - kCapsuleRadius;
}

struct Arm {
    const char* name;
    int frames;
    const char* script;
    std::vector<std::string> extra;
};

std::vector<Arm> arms() {
    const std::string at = format(kPlayerX, 1) + ",1," + format(kPlayerZ, 1);
    const char* walk = "60:Player.Forward+,240:Player.Forward-";

    // **Space is the jump only because the queue is empty.** `BattleArenaGame::frameUpdate`
    // routes the one key by whether a chain is waiting, and no arm here clicks.
    //
    // The three camera arms name a pose; the first three inherit the game's own opening pose,
    // which looks down +X -- along the line between the two fighters, and the one direction in
    // this arena with no column in it for forty metres.
    return {
        {"still", 600, "", {}},
        {"modifier", 300, "60:Player.Run+", {}},
        {"walk-run-jump", 600,
         "60:Player.Forward+,150:Player.Run+,240:Player.Run-,300:Player.Forward-,330:Player.Jump",
         {}},
        {"camera-north", 400, walk, {"--camera", at + ",0,-10,5"}},
        {"camera-south", 400, walk, {"--camera", at + ",180,-10,5"}},
        {"camera-turning", 400, walk, {"--camera", at + ",0,-10,5", "--camera-spin", "1.5"}},
        {"column", 500, "30:Player.Forward+,30:Player.Run+",
         {"--camera", at + "," + format(columnYaw(), 4) + ",-10,6"}},
        // Five seconds of running away under a camera looking down +Z, then a stop with enough
        // frames left for the enemy to close the gap it opened.
        {"pursuit", 700,
         "30:Player.Forward+,30:Player.Run+,330:Player.Forward-,330:Player.Run-",
         {"--camera", at + ",0,-10,6"}},
    };
}

} // namespace

int cmdArena(const std::vector<std::string>& args) {
    Config config = Config::Release;
    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::fputs("usage: substrate arena [config]\n"
                       "\n"
                       "Drives game/battle_arena through eight scripted arms: the fighter under a\n"
                       "follow camera, blocked by a column, and the enemy walking to it.\n",
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

    const fs::path dir = repoRoot() / "debug_frames" / "arena";
    std::error_code ec;
    fs::create_directories(dir, ec);

    Report report;
    const std::vector<Arm> all = arms();

    for (const Arm& arm : all) {
        const std::string name = arm.name;
        std::printf("== %s (%d frames%s%s)\n", name.c_str(), arm.frames,
                    *arm.script ? ", " : "", arm.script);

        std::vector<std::string> command{selfPath().string(), "run",   "battle_arena",
                                         tool::name(config),  "--",    "--headless",
                                         "--locked",          "--audio-null", "--frames",
                                         std::to_string(arm.frames)};
        if (*arm.script) command.insert(command.end(), {"--input-script", arm.script});
        command.insert(command.end(), arm.extra.begin(), arm.extra.end());

        RunOptions options;
        options.cwd = repoRoot();
        options.timeoutSeconds = kTimeout;
        const RunResult result = run(command, options);

        const std::string log = stripAnsi(result.out + result.err);
        std::ofstream(dir / (name + ".log"), std::ios::binary) << log;

        if (!result.ok()) {
            std::fputs(log.c_str(), stderr);
            std::fprintf(stderr, "%s failed to run\n", name.c_str());
            return 1;
        }

        if (log.find("ERROR") != std::string::npos) report.fail(name + " logged an error");
        // **Warnings too, and that is not the same assertion.** This game's warnings are all
        // about content that failed to arrive -- a model that did not load, a rig with too few
        // clips for a machine -- and every one leaves a fighter that still stands there and
        // still reports numbers. A run that half-loaded would otherwise pass every arm below.
        if (log.find("WARNING") != std::string::npos) report.fail(name + " logged a warning");

        if (log.find("battle_arena: 2 fighters, navmesh baked") == std::string::npos) {
            report.fail(name + ": the arena did not report two fighters over a baked navmesh");
        }

        // **"Baked" was true of a navmesh that could not see a single column.** The route
        // probed at load runs the length of one row of the grid, so it has to leave the
        // straight line; two waypoints is a straight line, which means the floor collider is a
        // bare quad again and it has not been cut around the columns.
        const std::optional<double> waypoints = number(log, "a route across the arena is ");
        if (!waypoints) {
            report.fail(name + ": the arena never probed a route across itself");
        } else if (*waypoints < 3) {
            report.fail(name + ": a route the length of the arena is " +
                        format(*waypoints, 0) +
                        " waypoints -- the columns are not in the navmesh");
        }

        // **Two imports of one file are two slices of the clip table, and the ranges must not
        // overlap.** `SceneAnimator::merge` renumbers an appended clip's channels onto the
        // appended nodes, so a second fighter handed the first one's clip indices stands in its
        // bind pose while the machine reports it walking -- which no distance here would notice.
        const std::string playerClips = lineWith(log, "'player' animates over clips ");
        const std::string enemyClips = lineWith(log, "'enemy' animates over clips ");
        const std::optional<double> playerFirst = number(playerClips, "clips ");
        const std::optional<double> playerLast = numberAfterDots(playerClips);
        const std::optional<double> enemyFirst = number(enemyClips, "clips ");
        const std::optional<double> enemyLast = numberAfterDots(enemyClips);
        if (!playerLast || !enemyLast) {
            report.fail(name + ": one of the fighters never reported a clip range");
        } else {
            if (*playerLast <= *playerFirst) report.fail(name + ": the player's clip range is empty");
            if (*enemyFirst < *playerLast) {
                report.fail(name + ": the fighters share clips " + format(*enemyFirst, 0) + ".." +
                            format(*playerLast, 0) + " -- the merge slices overlap");
            }
        }

        const std::string path = after(log, "Arena path: ");
        const std::string summary = lineWith(log, " changes over ");
        const std::optional<double> changes = number(summary, "Arena: ");
        const std::optional<double> travelled = number(summary, " steps, ");
        const std::optional<double> rise = number(summary, "peak rise ");
        const std::optional<double> net = number(summary, " net ");
        const std::optional<double> along = number(summary, " along ");
        const std::optional<double> across = number(summary, " across ");
        const std::optional<double> facing = number(summary, " facing ");
        if (!net) report.fail(name + ": the summary line carries no net displacement");
        if (path.empty()) report.fail(name + ": the run reported no path at all");
        if (report.failures != 0) break;

        const std::string enemy = lineWith(log, "Arena enemy: ");
        const std::optional<double> searches = number(enemy, "Arena enemy: ");
        const std::optional<double> searchFails = number(enemy, " searches, ");
        const std::optional<double> corners = number(enemy, "longest route ");
        const std::optional<double> enemyWalked = number(enemy, " waypoints, ");
        const std::optional<double> enemyNet = number(enemy, " net ");
        const std::optional<double> enemyClosest = number(enemy, " closest ");
        const std::optional<double> enemyGap = number(enemy, " final ");
        const std::optional<double> stall = number(enemy, " worst stall ");
        if (!stall) {
            report.fail(name + ": the run reported no pursuit at all");
            break;
        }

        // **`path ok` flickering is what this is.** A failed search keeps the route it had, so
        // the enemy goes on walking and the screen looks identical -- the count is the only
        // place a search that found nothing is visible.
        if (*searchFails != 0) {
            report.fail(name + ": " + format(*searchFails, 0) + " of " + format(*searches, 0) +
                        " searches found no route");
        }

        // **A fighter pressed into a column asks for its full travel and does not move.** Five
        // steps is the acceleration ramp out of a standstill; three hundred is a column.
        if (*stall > 10) {
            report.fail(name + ": the enemy asked to move and did not for " + format(*stall, 0) +
                        " steps running -- it walked into something");
        }

        // The band is the whole of the arrival rule: `scene::steer` gives its last waypoint up
        // short of the stand-off, and the pursuit holds until the player is a dead band past it.
        report.within(*enemyGap, kStandOff - 0.1, kStandOff + kDeadBand,
                      name + ": the enemy finished " + format(*enemyGap) +
                          " m away, expected to stand off at " + format(kStandOff, 1) + " m");

        const double travel = travelled.value_or(0.0);
        const int changeCount = static_cast<int>(changes.value_or(-1));

        if (name == "still" || name == "modifier") {
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

            // **The one pair of arms where the enemy's arithmetic is exact**, because the
            // target never moves: it walks the spawn gap less where it stopped, and the walk
            // agreeing with the displacement is what says it went at the player rather than
            // around anything. Within a percent rather than the centimetre, and the percent is
            // the route: the floor is cut into pieces around the columns, so a walk across it
            // is a polyline over a few portals rather than one segment.
            const double want = kSpawnGap - *enemyGap;
            report.within(*enemyWalked, want - 0.05, want + 0.15,
                          name + ": the enemy walked " + format(*enemyWalked, 2) +
                              " m, expected about " + format(want, 2) + " m");
            report.within(*enemyNet, *enemyWalked * 0.99, *enemyWalked,
                          name + ": the enemy's net " + format(*enemyNet, 2) + " m against " +
                              format(*enemyWalked, 2) + " m walked -- it did not walk straight");
            // The two capsules cannot collide, so nothing but the stand-off would stop it
            // standing inside the player.
            report.within(*enemyClosest, kStandOff - 0.05, 99.0,
                          name + ": the enemy came within " + format(*enemyClosest, 2) +
                              " m, inside the " + format(kStandOff, 1) + " m stand-off");
            // **The dead band, read as a cost.** Repathing on the step count alone would be one
            // search every 20 of the 600 this arm runs; it holds instead.
            if (*searches > 15) {
                report.fail(name + ": " + format(*searches, 0) +
                            " searches -- an arrived pursuit is still looking for a route");
            }

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

            const double walkSpeed = kMoveSpeed * kWalkFraction;
            const double instant =
                (90.0 * walkSpeed + 90.0 * kMoveSpeed + 60.0 * walkSpeed) / kStepHz;
            const double gap = kMoveSpeed - walkSpeed;
            const double want = instant - walkSpeed * walkSpeed / (2 * kAccel) -
                                gap * gap / (2 * kAccel) + gap * gap / (2 * kDecel) +
                                walkSpeed * walkSpeed / (2 * kDecel);
            report.within(travel, want * 0.97, want * 1.03,
                          name + ": travelled " + format(travel) + " m, expected about " +
                              format(want, 2) + " m");

            const double apex = kJumpSpeed * kJumpSpeed / (2 * kGravity);
            report.within(rise.value_or(0.0), apex, apex + kJumpSpeed / kStepHz,
                          name + ": peak rise " + format(rise.value_or(0.0)) +
                              " m, expected between " + format(apex) + " and one step above it");

            const std::optional<int> gaitUp = transitionStep(log, "Arena", "walk", "run");
            const std::optional<int> gaitDown = transitionStep(log, "Arena", "run", "walk");
            const std::optional<int> jumpStep = transitionStep(log, "Arena", "idle", "jump");
            const std::optional<int> landStep = transitionStep(log, "Arena", "fall", "land");
            const std::pair<const char*, std::optional<int>> required[] = {
                {"walk->run", gaitUp},    {"run->walk", gaitDown},
                {"idle->jump", jumpStep}, {"fall->land", landStep},
            };
            bool complete = true;
            for (const auto& [label, step] : required) {
                if (!step) {
                    report.fail(name + ": " + label + " never happened");
                    complete = false;
                }
            }
            if (!complete) break;

            // **The transition the arm exists for.** The movement key does not move at 240 --
            // only the modifier is released, the controller is asked for 45% of travel instead
            // of 100%, and the machine crosses its own `run` threshold on the way down. A state
            // read off a keypress cannot produce it.
            const double decelSteps = (kMoveSpeed - kRunSpeed) / kDecel * kStepHz;
            const double wantDown = 240 + decelSteps + 2;
            report.within(*gaitDown, wantDown - 4, wantDown + 4,
                          name + ": run -> walk at step " + std::to_string(*gaitDown) +
                              ", expected about " + format(wantDown, 1));

            // The launch is a trigger rather than a threshold -- `characterJumped`, which a
            // coyote window and a jump buffer make impossible to reconstruct from the key and
            // the ground state -- so it lands two steps after the press.
            report.within(*jumpStep, 330, 336,
                          name + ": idle -> jump at step " + std::to_string(*jumpStep) +
                              ", expected 332-ish");

            const double wantLand = *jumpStep + 2 * kJumpSpeed / kGravity * kStepHz + 2;
            report.within(*landStep, wantLand - 5, wantLand + 5,
                          name + ": fall -> land at step " + std::to_string(*landStep) +
                              ", expected about " + format(wantLand, 1));

        } else if (name.rfind("camera-", 0) == 0) {
            // 180 steps of walking at kWalkFraction, less the two ramps. The distance is the
            // main arm's job; what these three assert is the *direction*.
            const double walkSpeed = kMoveSpeed * kWalkFraction;
            const double want = 180 * walkSpeed / kStepHz -
                                walkSpeed * walkSpeed / (2 * kAccel) +
                                walkSpeed * walkSpeed / (2 * kDecel);
            report.within(travel, want * 0.95, want * 1.05,
                          name + ": travelled " + format(travel) + " m, expected about " +
                              format(want, 2) + " m");

            // **Signed, and this is the assertion.** 1 is a fighter walking where the camera
            // points. A basis rebuilt from `yaw` as `(sin, 0, -cos)` gives -1 under one of the
            // two fixed arms, which is why they are a pair rather than one arm.
            report.within(along.value_or(0.0), 0.95, 1.01,
                          name + ": went " + format(along.value_or(0.0)) +
                              " along the camera, expected about 1");
            report.within(across.value_or(1.0), 0.0, 0.1,
                          name + ": went " + format(across.value_or(1.0)) +
                              " across the camera, expected about 0");
            // Short of 1 because the first steps are spent slewing out of the opening pose,
            // which faces the other fighter.
            report.within(facing.value_or(0.0), 0.85, 1.01,
                          name + ": faced " + format(facing.value_or(0.0)) +
                              " of the way it went, expected about 1");

            if (name == "camera-turning") {
                // 1.5 degrees of yaw per frame over 400 frames is most of two turns, and the
                // fighter has to keep turning into it. Resolved against a fixed basis the same
                // run is a straight line, so net would equal travelled.
                report.within(net.value_or(travel), 0.0, travel / 2,
                              name + ": net " + format(net.value_or(travel)) + " m of " +
                                  format(travel) + " m walked -- the path did not curve");
            }

        } else if (name == "column") {
            // **The keys are never released in this arm.** Everything below happens while the
            // fighter is still being asked for a full-speed run, which is what makes it the
            // solver's answer rather than the input's.
            if (path != "idle > walk > run > idle") {
                report.fail(name + ": path is '" + path +
                            "', expected 'idle > walk > run > idle'");
            }

            const double reach = columnReach();
            report.within(travel, reach - 0.15, reach + 0.15,
                          name + ": travelled " + format(travel) + " m, expected to stop at " +
                              format(reach) + " m");
            // Straight in and not around: a capsule that slid off the column would keep
            // accumulating path length while its displacement stalled.
            report.within(net.value_or(0.0), travel - 0.02, travel,
                          name + ": net " + format(net.value_or(0.0)) + " m against " +
                              format(travel) + " m walked -- it slid rather than stopped");

            // **The assertion the defect was hiding behind.** `characterVelocity` used to be
            // Jolt's stored linear velocity, which is the request the engine set and not what
            // the sweep resolved, so a fighter flat against a column reported 3.2 m/s and held
            // `run` until the key came up. It is the swept displacement now.
            const std::optional<int> stopped = transitionStep(log, "Arena", "run", "idle");
            if (!stopped) {
                report.fail(name + ": the machine never left 'run' -- a blocked fighter is "
                                   "reporting the speed it asked for");
            } else {
                const double wantStop =
                    30 + (reach + kMoveSpeed * kMoveSpeed / (2 * kAccel)) / kMoveSpeed * kStepHz + 2;
                report.within(*stopped, wantStop - 8, wantStop + 8,
                              name + ": left 'run' at step " + std::to_string(*stopped) +
                                  ", expected about " + format(wantStop, 1) +
                                  " -- the column is elsewhere");
            }

        } else if (name == "pursuit") {
            if (path != "idle > walk > run > walk > idle") {
                report.fail(name + ": path is '" + path +
                            "', expected 'idle > walk > run > walk > idle'");
            }
            // **The clause this arm exists for.** A pursuit that searched once and walked the
            // answer would arrive where the player *was* -- sixteen metres short -- so the only
            // way to finish at the stand-off is to have re-searched the whole way.
            if (*searches < 15) {
                report.fail(name + ": " + format(*searches, 0) +
                            " searches over a five-second chase -- it did not repath");
            }
            // And it took the longer way round a moving target. A pursuit resolved once would
            // be a straight line, so path length and displacement would agree.
            report.within(*enemyWalked, *enemyNet * 1.05, 99.0,
                          name + ": the enemy walked " + format(*enemyWalked, 2) + " m for " +
                              format(*enemyNet, 2) +
                              " m of displacement -- it went straight");
        }

        std::printf("   path: %s\n", path.c_str());
        std::printf("   %d changes, %s m travelled, peak rise %s m\n", changeCount,
                    format(travel).c_str(), format(rise.value_or(0.0)).c_str());
        std::printf("   net %s m, %s along the camera, %s across it, %s faced\n",
                    format(net.value_or(0.0)).c_str(), format(along.value_or(0.0)).c_str(),
                    format(across.value_or(0.0)).c_str(), format(facing.value_or(0.0)).c_str());
        std::printf("   enemy: %s searches, longest route %s waypoints, %s m walked, net %s m, "
                    "closest %s m, final %s m\n",
                    format(*searches, 0).c_str(), format(*corners, 0).c_str(),
                    format(*enemyWalked, 2).c_str(), format(*enemyNet, 2).c_str(),
                    format(*enemyClosest, 2).c_str(), format(*enemyGap, 2).c_str());
    }

    if (report.failures != 0) {
        std::fprintf(stderr, "arena: %d failed assertion(s) over %zu arms\n", report.failures,
                     all.size());
        return 1;
    }
    std::printf("arena: %zu of %zu arms pass\n", all.size(), all.size());
    return 0;
}

} // namespace tool
