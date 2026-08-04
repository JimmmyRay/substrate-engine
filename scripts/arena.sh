#!/usr/bin/env bash
#
# battle_arena, Phase 2 -- the scripted check that a fighter moves under the camera it is
# looking through, and stops at what is in the way.
#
#   scripts/arena.sh [config]
#
# > **The milestone is "WASD walks the fighter around the arena under a follow camera that
# > agrees with it, the gait animates from what the solver did, and it cannot walk through a
# > column."** Three of those four are claims about an *order* of events and a *direction*,
# > and none of them survives being flattened into pixels. The golden set cannot make them.
#
# `scripts/locomotion.sh` makes the same shape of argument about `game/demo`, and this is not
# that script with a game argument bolted on. Every number below is derived from *this* game's
# collider, *this* game's gait thresholds and *this* arena's geometry, and the two games are
# separate programs over one engine -- a `$GAME` parameter would be one script asserting two
# unrelated sets of arithmetic, which is the bundling the conventions refuse. What is shared is
# the shape: press keys under `--locked`, and assert what came back out of the far end.
#
# `BattleArenaGame::traceStep` reports all of it from `SceneAnimator::currentState`,
# `PhysicsWorld::characterTransform` and `Scene::worldTransform`, and never from the input map.
# Nothing here inspects a key.
#
# ## The arms, and which one is the negative control
#
#  - **`still`** presses nothing for 600 steps. A fighter that spawned interpenetrated, a
#    machine whose `airborne` defaulted to the wrong sense, a trace that accumulated a settling
#    twitch -- every one of those is a non-zero number here. It is also what makes the spawn
#    height a decision rather than a habit: at the 0.2 m the fighters were first placed at, both
#    entered `fall` on step 2 and played two seconds of `hard landing` before anybody pressed
#    anything.
#
#  - **`modifier`** holds `Player.Run` and nothing else. The run key is the one input in the
#    scheme that names a state of the machine, so a fighter playing `run` because the run key
#    was down lights this arm up. It must be byte-for-byte `still`: the modifier scales a
#    direction, and there is no direction.
#
#  - **`walk-run-jump`** is the row itself, and the transition that carries it is `run -> walk`
#    at around step 243, where **the movement key does not change** -- only the modifier is
#    released. A state read off a keypress cannot produce that transition.
#
#  - **`camera-north`** and **`camera-south`** are the same six keystrokes under cameras
#    pointing opposite ways. A heading rebuilt from `yaw` as `(sin, 0, -cos)` rather than taken
#    from `Camera::forward()` satisfies at most one of them, and the demo carried exactly that
#    error through three checks because every one of them ran at the yaw where it cancels.
#
#  - **`camera-turning`** spins the camera while the key is held, and is the arm no fixed basis
#    survives at any yaw: the fighter has to keep turning into a camera that keeps moving, so
#    its path curves and its net displacement comes out a fraction of the distance walked.
#
#  - **`column`** is the milestone's last clause and the regression check for
#    `bug-a-blocked-character-reports-the-speed-it-asked-for`. It runs a fighter dead-centre
#    into a column and **never releases the keys**, so every number after the impact is the
#    solver's answer and not the input's. Two things are asserted, and the second is the one
#    that was broken: the fighter stops at the column's face, *and* the machine leaves `run`.
#    Before the fix it stopped and went on running on the spot for three hundred steps.
#
# Every arm runs `--headless`, `--locked` and `--audio-null`, for the reasons golden.sh gives.
set -euo pipefail

cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
DIR="debug_frames/arena"
mkdir -p "$DIR"

# The fighter's collider, from `ArenaWorld.cpp` -- `kMoveSpeed`, `kJumpSpeed`, `kCapsuleRadius`.
# Everything asserted below is derived from these and from the two numbers under them, so a
# tuning change moves the expectation here rather than silently invalidating it.
STEP_HZ=60
MOVE_SPEED=3.2
JUMP_SPEED=4.2
CAPSULE_RADIUS=0.3
WALK_FRACTION=0.45 # BattleArenaGame.cpp, kWalkFraction

# `ColliderDesc`'s defaults, which is what the arena's collider leaves them at. C20's motion
# model reaches every distance below through them.
ACCEL=10.0
DECEL=40.0

# The `run` threshold `fighterMachine` authors, as a speed. The machine compares a `speed`
# normalised against `characterMoveSpeed`, so the authored 0.66 is this -- derived rather than
# written out, because the whole point of G15 was that the divisor stopped being a number a
# game guessed at.
RUN_SPEED="$(awk -v m=$MOVE_SPEED 'BEGIN { printf "%.4f", 0.66 * m }')"

# ------------------------------------------------------------------- the arena's geometry
#
# `kPlayerSpawn`, and the nearest column of the 7x4 grid `arena.glb` authors -- centres 10 m
# apart in x and irregularly in z, each a metre in radius and ten tall. Read out of the
# document rather than eyeballed on screen: the `column` arm has to walk *dead centre* into one
# or the capsule slides around it and the distance it stops at stops being arithmetic.
PLAYER_X=-6.0
PLAYER_Z=0.0
COLUMN_X=-9.0232
COLUMN_Z=9.9297
COLUMN_RADIUS=1.0

# Where the camera has to point for "forward" to be at that column, in degrees. Yaw is measured
# from +Z -- `atan2(x, z)` and never `atan2(z, x)` -- which is where the rig is authored looking
# and what `Camera::forward()` is built from.
#
# `atan2(1, 1)` is a quarter of pi, which is where awk keeps the constant it has no name for.
COLUMN_YAW="$(awk -v px=$PLAYER_X -v pz=$PLAYER_Z -v cx=$COLUMN_X -v cz=$COLUMN_Z \
    'BEGIN { printf "%.4f", atan2(cx - px, cz - pz) * 45 / atan2(1, 1) }')"

# How far the fighter gets: to the centre, less the column's radius and the capsule's. A
# capsule against a cylinder touches along the line between the two axes, so this is exact
# rather than approximate, and it is what makes "it stopped" distinguishable from "it stopped
# somewhere".
COLUMN_REACH="$(awk -v px=$PLAYER_X -v pz=$PLAYER_Z -v cx=$COLUMN_X -v cz=$COLUMN_Z \
    -v cr=$COLUMN_RADIUS -v pr=$CAPSULE_RADIUS \
    'BEGIN { printf "%.3f", sqrt((cx - px) ^ 2 + (cz - pz) ^ 2) - cr - pr }')"

# name | frames | input script | flags. An empty script is a bare run, which is what `still` is.
#
# The main arm's schedule, and what each entry is for:
#   60  walk      -- forward alone, so the request is `kWalkFraction` of travel
#   150 run       -- the modifier, with the movement key untouched
#   240 walk      -- the modifier released, still with the movement key untouched
#   300 idle      -- the movement key released
#   330 jump      -- a tap from a standstill, so the arc is a clean one to check against
# 600 frames outruns the last of it by `hard landing`'s two seconds and a margin.
#
# **Space is the jump only because the queue is empty.** `BattleArenaGame::frameUpdate` routes
# the one key by whether a chain is waiting, and no arm here clicks, so every press below is a
# jump. An arm that queued an action would be testing Phase 4.
#
# The three camera arms name a pose; the first three inherit the game's own opening pose, which
# is a yaw of pi/2 looking down +X -- along the line between the two fighters, and the one
# direction in this arena with no column in it for forty metres.
CAMERA_AT="$PLAYER_X,1,$PLAYER_Z"
WALK="60:Player.Forward+,240:Player.Forward-"
ARMS=(
    "still|600||"
    "modifier|300|60:Player.Run+|"
    "walk-run-jump|600|60:Player.Forward+,150:Player.Run+,240:Player.Run-,300:Player.Forward-,330:Player.Jump|"
    "camera-north|400|$WALK|--camera $CAMERA_AT,0,-10,5"
    "camera-south|400|$WALK|--camera $CAMERA_AT,180,-10,5"
    "camera-turning|400|$WALK|--camera $CAMERA_AT,0,-10,5 --camera-spin 1.5"
    "column|500|30:Player.Forward+,30:Player.Run+|--camera $CAMERA_AT,$COLUMN_YAW,-10,6"
)

failures=0

fail() {
    echo "  FAIL: $*" >&2
    failures=$((failures + 1))
}

# The step a named transition landed on, or empty. The log line is
# `Arena: <from> -> <to> at step N (...)`.
transition_step() {
    sed -n "s/.*Arena: $1 -> $2 at step \([0-9]*\) .*/\1/p" "$3" | head -1
}

# `expr` in floats, as a predicate. awk rather than bc, which is not installed everywhere.
within() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'; }

for arm in "${ARMS[@]}"; do
    IFS='|' read -r name frames script extra <<<"$arm"
    log="$DIR/$name.log"
    echo "== $name ($frames frames${script:+, $script})"

    args=(--headless --locked --audio-null --frames "$frames")
    [[ -n "$script" ]] && args+=(--input-script "$script")
    # Split on purpose: `extra` is a flag list and an arm that names none adds none.
    # shellcheck disable=SC2206
    [[ -n "$extra" ]] && args+=($extra)
    if ! timeout -s TERM 180 ./run.sh battle_arena "$CONFIG" -- "${args[@]}" >"$log" 2>&1; then
        cat "$log" >&2
        echo "$name failed to run" >&2
        exit 1
    fi
    # The logger colourises for a terminal and the redirect above is not one it can ask, so
    # every line arrives wrapped in escapes. Stripped once, here, rather than in each pattern
    # below -- a `$` anchor that silently never matches is exactly the assertion-shaped hole
    # this script exists to close.
    sed -i 's/\x1b\[[0-9;]*m//g' "$log"

    # A script naming an action this build does not have, and a script running past `--frames`,
    # both look exactly like a feature that does not work. C16 made each an error rather than a
    # silence; this is somebody actually reading them.
    if grep -q "ERROR" "$log"; then
        fail "$name logged an error"
        grep "ERROR" "$log" >&2
    fi
    # **Warnings too, and that is not the same assertion.** This game's warnings are all about
    # content that failed to arrive -- a model that did not load, a rig with too few clips for a
    # machine -- and every one of them leaves a fighter that still stands there and still
    # reports numbers. A run that half-loaded would otherwise pass every arm below.
    if grep -q "WARNING" "$log"; then
        fail "$name logged a warning"
        grep "WARNING" "$log" >&2
    fi

    # Both fighters exist, the floor they stand on came out of `.collider` nodes, and the
    # navmesh baked from those. One line, and it is the whole of Phase 1 still holding.
    grep -q "battle_arena: 2 fighters, navmesh baked" "$log" ||
        fail "$name: the arena did not report two fighters over a baked navmesh"

    # **Two imports of one file are two slices of the clip table, and the ranges must not
    # overlap.** `SceneAnimator::merge` renumbers an appended clip's channels onto the appended
    # nodes, so a second fighter handed the first one's clip indices stands in its bind pose
    # while the machine reports it walking -- which no distance in this file would notice.
    read -r player_first player_last < <(sed -n \
        "s/.*battle_arena: 'player' animates over clips \([0-9]*\)\.\.\([0-9]*\).*/\1 \2/p" "$log" | head -1)
    read -r enemy_first enemy_last < <(sed -n \
        "s/.*battle_arena: 'enemy' animates over clips \([0-9]*\)\.\.\([0-9]*\).*/\1 \2/p" "$log" | head -1)
    if [[ -z "$player_last" || -z "$enemy_last" ]]; then
        fail "$name: one of the fighters never reported a clip range"
    else
        [[ "$player_last" -gt "$player_first" ]] || fail "$name: the player's clip range is empty"
        [[ "$enemy_first" -ge "$player_last" ]] ||
            fail "$name: the fighters share clips $enemy_first..$player_last -- the merge slices overlap"
    fi

    path="$(sed -n 's/.*Arena path: //p' "$log" | head -1)"
    read -r changes travelled rise < <(sed -n \
        's/.*Arena: \([0-9]*\) changes over [0-9]* steps, \([0-9.]*\) m travelled, peak rise \([0-9.]*\) m.*/\1 \2 \3/p' \
        "$log" | head -1)
    read -r net along across facing < <(sed -n \
        's/.*Arena: .* net \([0-9.]*\) m, along \(-*[0-9.]*\) across \(-*[0-9.]*\) facing \(-*[0-9.]*\).*/\1 \2 \3 \4/p' \
        "$log" | head -1)
    [[ -n "$net" ]] || fail "$name: the summary line carries no net displacement"
    [[ -n "$path" ]] || fail "$name: the run reported no path at all"
    [[ $failures -eq 0 ]] || break

    case "$name" in
    still | modifier)
        # The fighter is asked for nothing it can act on, so nothing happens. Not "roughly
        # nothing": it spawns at rest on a static floor under a locked clock, and a millimetre
        # here is a defect with a cause.
        [[ "$path" == "idle" ]] || fail "$name: path is '$path', expected 'idle'"
        [[ "$changes" == "0" ]] || fail "$name: $changes state changes, expected none"
        within "$travelled" 0 0.01 || fail "$name: travelled $travelled m with nothing pressed"
        within "$rise" 0 0.01 || fail "$name: rose $rise m with nothing pressed"
        # And it went nowhere in particular. A fighter nobody moved has no direction to agree
        # with anything, so the ratios are the zero the report writes when there is no distance
        # to divide by -- which is what catches a trace accumulating a projection while
        # `travelled` stays flat.
        within "$net" 0 0.01 || fail "$name: net displacement $net m with nothing pressed"
        within "$along" 0 0.01 || fail "$name: went $along along the camera with nothing pressed"
        ;;
    walk-run-jump)
        expected="idle > walk > run > walk > idle > jump > fall > land > idle"
        [[ "$path" == "$expected" ]] || fail "$name: path is '$path', expected '$expected'"
        [[ "$changes" == "8" ]] || fail "$name: $changes state changes, expected 8"

        # 90 steps of walk, 90 of run, 60 of walk, at 60 Hz. Stated as arithmetic because a
        # number lifted off a passing run is a number that cannot be wrong.
        instant="$(awk -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v hz=$STEP_HZ \
            'BEGIN { printf "%.4f", (90 * m * w + 90 * m + 60 * m * w) / hz }')"

        # C20's four ramps, each against the step it replaced. A linear ramp between two speeds
        # takes |dv|/a seconds and covers dv^2/2a less ground than arriving instantly -- so the
        # accelerations are debits and the decelerations, which coast *past* the new request
        # rather than dropping to it, are credits. Walk is 1.44 m/s and run is 3.20.
        want="$(awk -v i="$instant" -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v a=$ACCEL -v d=$DECEL 'BEGIN {
            walk = m * w; gap = m - walk;
            printf "%.2f", i - walk * walk / (2 * a) - gap * gap / (2 * a) + gap * gap / (2 * d) + walk * walk / (2 * d)
        }')"
        within "$travelled" "$(awk -v w="$want" 'BEGIN { print w * 0.97 }')" \
            "$(awk -v w="$want" 'BEGIN { print w * 1.03 }')" ||
            fail "$name: travelled $travelled m, expected about $want m"

        # A rise is the one thing gravity cannot supply, which is why the check is on the apex
        # rather than on the displacement. v^2 / 2g, plus up to one step of overshoot.
        apex="$(awk -v v=$JUMP_SPEED 'BEGIN { printf "%.3f", v * v / (2 * 9.81) }')"
        within "$rise" "$apex" "$(awk -v a="$apex" -v v=$JUMP_SPEED -v hz=$STEP_HZ 'BEGIN { print a + v / hz }')" ||
            fail "$name: peak rise $rise m, expected between $apex and one step above it"

        # ------------------------------------------------------- the steps, and the point
        # Two steps of latency throughout: the frame reads the key and asks the controller, the
        # next step's `fixedUpdate` reads the speed that came back, and the machine steps after
        # that.
        gait_up="$(transition_step walk run "$log")"
        gait_down="$(transition_step run walk "$log")"
        jump_step="$(transition_step idle jump "$log")"
        land_step="$(transition_step fall land "$log")"
        for pair in "walk->run:$gait_up" "run->walk:$gait_down" "idle->jump:$jump_step" \
            "fall->land:$land_step"; do
            [[ -n "${pair#*:}" ]] || fail "$name: ${pair%%:*} never happened"
        done
        [[ $failures -eq 0 ]] || break

        # **The transition the arm exists for.** The movement key does not move at 240 -- only
        # the modifier is released, the controller is asked for 45% of travel instead of 100%,
        # and the machine crosses its own `run` threshold on the way down. A state read off a
        # keypress cannot produce it. The step is where the deceleration reaches
        # `RUN_SPEED` from `MOVE_SPEED`, plus the two of latency.
        decel_steps="$(awk -v hi=$MOVE_SPEED -v lo="$RUN_SPEED" -v d=$DECEL -v hz=$STEP_HZ \
            'BEGIN { printf "%.1f", (hi - lo) / d * hz }')"
        want_down="$(awk -v s=240 -v n="$decel_steps" 'BEGIN { printf "%.1f", s + n + 2 }')"
        within "$gait_down" "$(awk -v w="$want_down" 'BEGIN { print w - 4 }')" \
            "$(awk -v w="$want_down" 'BEGIN { print w + 4 }')" ||
            fail "$name: run -> walk at step $gait_down, expected about $want_down"

        # The launch is a trigger rather than a threshold -- `characterJumped`, which a coyote
        # window and a jump buffer make impossible to reconstruct from the key and the ground
        # state -- so it lands two steps after the press and not a clip length later.
        within "$jump_step" 330 336 || fail "$name: idle -> jump at step $jump_step, expected 332-ish"

        # And the touchdown is arithmetic: 2v/g after the launch, plus the latency.
        want_land="$(awk -v j="$jump_step" -v v=$JUMP_SPEED -v hz=$STEP_HZ \
            'BEGIN { printf "%.1f", j + 2 * v / 9.81 * hz + 2 }')"
        within "$land_step" "$(awk -v w="$want_land" 'BEGIN { print w - 5 }')" \
            "$(awk -v w="$want_land" 'BEGIN { print w + 5 }')" ||
            fail "$name: fall -> land at step $land_step, expected about $want_land"
        ;;
    camera-north | camera-south | camera-turning)
        # 180 steps of walking at `kWalkFraction`, less the two ramps. The distance is the main
        # arm's job; what these three assert is the *direction*.
        want="$(awk -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v hz=$STEP_HZ -v a=$ACCEL -v d=$DECEL 'BEGIN {
            walk = m * w
            printf "%.2f", 180 * walk / hz - walk * walk / (2 * a) + walk * walk / (2 * d)
        }')"
        within "$travelled" "$(awk -v w="$want" 'BEGIN { print w * 0.95 }')" \
            "$(awk -v w="$want" 'BEGIN { print w * 1.05 }')" ||
            fail "$name: travelled $travelled m, expected about $want m"

        # **Signed, and this is the assertion.** 1 is a fighter walking where the camera points.
        # A basis rebuilt from `yaw` as `(sin, 0, -cos)` gives -1 under one of the two fixed
        # arms, which is why they are a pair rather than one arm.
        within "$along" 0.95 1.01 || fail "$name: went $along along the camera, expected about 1"
        # Unsigned, against the camera's right. A basis that swapped its axes rather than
        # flipping one lands here instead of in the sum above.
        within "$across" 0 0.1 || fail "$name: went $across across the camera, expected about 0"
        # Read off the scene node rather than off the angle that was written to it, and the
        # worst of the rig's parts rather than the first -- one mesh left behind drags it down.
        # Short of 1 because the first steps are spent slewing out of the opening pose, which
        # faces the other fighter.
        within "$facing" 0.85 1.01 || fail "$name: faced $facing of the way it went, expected about 1"
        ;;& # fall through: the turning arm adds one assertion to the three above
    camera-turning)
        # 1.5 degrees of yaw per frame over 400 frames is most of two turns, and the fighter has
        # to keep turning into it. Resolved against a fixed basis the same run is a straight
        # line, so net would equal travelled; here the path curves and it does not.
        within "$net" 0 "$(awk -v t="$travelled" 'BEGIN { print t / 2 }')" ||
            fail "$name: net $net m of $travelled m walked -- the path did not curve"
        ;;
    column)
        # **The keys are never released in this arm.** Everything below happens while the
        # fighter is still being asked for a full-speed run, which is what makes it the
        # solver's answer rather than the input's.
        [[ "$path" == "idle > walk > run > idle" ]] ||
            fail "$name: path is '$path', expected 'idle > walk > run > idle'"

        # It stopped at the column's face, and the face is arithmetic: the distance between the
        # two axes less the two radii.
        within "$travelled" "$(awk -v r="$COLUMN_REACH" 'BEGIN { print r - 0.15 }')" \
            "$(awk -v r="$COLUMN_REACH" 'BEGIN { print r + 0.15 }')" ||
            fail "$name: travelled $travelled m, expected to stop at $COLUMN_REACH m"

        # Straight in and not around: a capsule that slid off the column would keep accumulating
        # path length while its displacement stalled, so the two agreeing is what says the
        # approach was dead centre and the stop was the column rather than a graze.
        within "$net" "$(awk -v t="$travelled" 'BEGIN { print t - 0.02 }')" "$travelled" ||
            fail "$name: net $net m against $travelled m walked -- it slid rather than stopped"

        # **The assertion the defect was hiding behind.** `characterVelocity` used to be Jolt's
        # stored linear velocity, which is the request the engine set and not what the sweep
        # resolved, so a fighter flat against a column reported 3.2 m/s and held `run` until the
        # key came up. It is the swept displacement now, and this is what says so.
        stopped="$(transition_step run idle "$log")"
        if [[ -z "$stopped" ]]; then
            fail "$name: the machine never left 'run' -- a blocked fighter is reporting the speed it asked for"
        else
            # And it left `run` *at the column*, not somewhere. The key goes down at step 30,
            # the reach is covered at `MOVE_SPEED`, the ramp into that speed costs its own
            # `v^2 / 2a` of ground, and the machine is two steps behind the sweep.
            want_stop="$(awk -v r="$COLUMN_REACH" -v m=$MOVE_SPEED -v a=$ACCEL -v hz=$STEP_HZ \
                'BEGIN { printf "%.1f", 30 + (r + m * m / (2 * a)) / m * hz + 2 }')"
            within "$stopped" "$(awk -v w="$want_stop" 'BEGIN { print w - 8 }')" \
                "$(awk -v w="$want_stop" 'BEGIN { print w + 8 }')" ||
                fail "$name: left 'run' at step $stopped, expected about $want_stop -- the column is elsewhere"
        fi
        ;;
    esac

    echo "   path: $path"
    echo "   $changes changes, $travelled m travelled, peak rise $rise m"
    echo "   net $net m, $along along the camera, $across across it, $facing faced"
done

if [[ $failures -ne 0 ]]; then
    echo "arena: $failures failed assertion(s) over ${#ARMS[@]} arms" >&2
    exit 1
fi
echo "arena: ${#ARMS[@]} of ${#ARMS[@]} arms pass"
