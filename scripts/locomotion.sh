#!/usr/bin/env bash
#
# Scripted locomotion (G12, extended by C20 and G13) -- the check a golden image cannot be.
#
#   scripts/locomotion.sh [config]
#
# > **An animation state that depends on what somebody pressed is not checkable by
# > rendering it.** A golden frame of a character mid-blend is a picture of one moment; the
# > claim this row makes is about an *order* of transitions and the *steps* they happened
# > at, and neither survives being flattened into pixels.
#
# So this drives the demo's character with C16's `--input-script` over a fixed number of
# `--locked` steps and asserts what came back out of the far end of the chain: the state
# machine's path, the step each transition landed on, and how far the solver actually
# carried the character. The demo reports all four itself -- `DemoGame::driveLocomotion`
# reads `SceneAnimator::currentState` and `PhysicsWorld::characterTransform`, never the
# input map -- so nothing here inspects a key.
#
# ## Five arms, and three of them are the negative control
#
# The obvious version of this check passes for an implementation that does not work. Two
# ways, both of which have their own arm:
#
#  - **`still`** presses nothing. A character that arrived somewhere under gravity, a trace
#    that accumulated a settling twitch, a machine that entered `fall` because a parameter
#    defaulted to the wrong sense -- every one of those shows up here as a non-zero number,
#    and this arm is what forbids them. It is also what caught the two defects the row
#    found: `characterOnGround` before the first sweep, and the physics snapshot pair going
#    out of length behind a bounds check that only looked at one of them.
#
#  - **`modifier`** holds `Player.Run` and nothing else, for the whole run. The run key is
#    the one input in the scheme that names a *state* of the machine, so an implementation
#    that played `run` because the run key was down would light this arm up. It must be
#    byte-for-byte the same answer as `still`: the key changes the length of a vector, the
#    vector changes nothing while there is no direction to scale.
#
# The third arm is the row itself, and the transition it turns on is `run -> walk` at step
# 242: **the movement key does not change there.** Only the modifier is released, the
# controller is asked for 45% of travel instead of 100%, Jolt reports 1.44 m/s instead of
# 3.20, and the machine crosses its own threshold on the way down. A state read off a
# keypress cannot produce that transition, and a state read off `characterSpeed` cannot
# avoid it.
#
# `fall` and `land` are the same argument made twice more. Nothing is pressed within fifty
# steps of either; both are functions of a clip length and a ballistic arc, and this script
# asserts each against the arithmetic rather than against a number somebody observed.
#
# ## C20's two arms, and why they are also a pair
#
# `jump-buffered` and `jump-eaten` are the same run with the second press moved eighteen
# frames. **A jump buffer cannot be checked by pressing jump while grounded** -- that passes
# against a controller with no buffer in it, which is what this engine had. So both arms
# press jump while the character is in mid-air, several steps before it lands: one inside
# the ten-step window and one outside it, and only the first produces a second launch.
#
# The travel numbers in `walk-run-jump` moved when C20 landed, and they were meant to. Every
# expectation here is re-derived from the new model -- four ramps, each with a computable
# area against the step it replaced -- rather than read off a passing run, which is the same
# discipline the 8.40 m arithmetic had before it.
#
# ## G13's three arms, and why one of them would not have been enough
#
# The three above assert *distances*, and `travelled` is a horizontal path length summed per
# step -- deliberately, so a character that only fell cannot pass. A path length is also the
# one quantity that is identical whichever way the character walked, which is how a
# 180-degree heading error lived in `PlayerActions::moveDirection` from the day it was
# written: it built `(sin yaw, 0, -cos yaw)` beside a `Camera::forward()` whose horizontal
# part is `(sin yaw, 0, cos yaw)`, and the two agree only where `cos yaw` is zero.
# `Camera::frameBounds` picks yaw = pi/2 for the showcase scene, and no arm here passed
# `--camera`, so every run in this file was made at the one yaw where the error cancels.
#
# So these three name a camera and assert a *direction* against it: `along` is the run's
# horizontal displacement projected onto the camera's own forward and divided by the path
# length, so 1 is a character walking where the camera points and -1 is one walking away.
#
#  - **`camera-north`** and **`camera-south`** are the same six keystrokes under cameras
#    pointing opposite ways. Both are at a yaw where the old basis was exactly backwards,
#    which is what makes them regression checks; the pair is what makes them a check on
#    *agreement*, because a heading resolved against anything but the camera can satisfy at
#    most one of them.
#
#  - **`camera-turning`** spins the camera while the key is held, and is the arm a fixed
#    basis cannot survive at any yaw. The character has to keep turning into a camera that
#    keeps moving, so its path curves into most of a circle: the assertion is that `along`
#    stays near 1 *and* that the net displacement is a fraction of the distance walked.
#    Resolved against a fixed yaw the same run is a straight line -- measured, not argued:
#    net equal to travelled and `along` -0.20, against 0.94 and a third of travelled here.
#
# The counterfactual was run for all three before the fix and again with the camera ignored
# outright. `camera-north` passes a fixed basis (at yaw 0 the two coincide) and fails the old
# one; the other two fail both. **One arm would have reproduced the original mistake**, which
# is the whole reason there are three.
#
# ## The ninth arm, and the assumption the other eight share
#
# Every one of them stands the character on ground that does not move, and on ground that
# does not move a heading read off world displacement is right. `platform-ride` is the arm
# where the two answers differ: C29's `setCharacterTransform` puts the character on the
# demo's sliding platform -- nothing in the scene walks onto it -- and then nothing is
# pressed for 600 steps, through two reversals. It needs a tenth kind of number as well as a
# ninth arm, for the reason the drift check needed a ninth: every ratio the summary reports
# divides by `travelled`, and `travelled` is world displacement, which a rider accumulates
# exactly as a walker does. `turned` divides by nothing.
#
# Off `characterVelocity` the rig turns 0.27 rad, all of it in the few steps the drop takes
# to be dragged up to the platform's speed. Off world displacement it turns 3.14 -- the
# character facing the way the floor is going and swinging a half turn at each end of the
# run. Both measured, on this arm, one build apart.
#
# Every arm runs `--headless`, `--locked` and `--audio-null`, for the reasons golden.sh
# gives about all three.
set -euo pipefail

cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
DIR="debug_frames/locomotion"
mkdir -p "$DIR"

# The showcase character, from its own glTF: 3.2 m/s of travel and a 4.2 m/s launch, on the
# engine's -9.81 gravity. Everything asserted below is derived from these four numbers and
# from two clip lengths on the rig, so a tuning change moves the expectation here rather
# than silently invalidating it.
STEP_HZ=60
MOVE_SPEED=3.2
WALK_FRACTION=0.45  # PlayerActions::kWalkFraction
JUMP_SPEED=4.2
LAUNCH_CLIP=0.25    # `jumping up`
LANDING_CLIP=2.0    # `hard landing`

# C20's motion model. `showcase.gltf` authors none of these, so they are `ColliderDesc`'s
# defaults, and every number below that moved against G12's moved because of them.
ACCEL=10.0          # ColliderDesc::acceleration, m/s^2
DECEL=40.0          # ColliderDesc::deceleration, m/s^2
BUFFER_STEPS=10     # ColliderDesc::jumpBufferSteps

# The two gait thresholds, as speeds. The machine compares a normalised `speed`, and G15
# changed what it is normalised *against*: the demo used to divide by a literal 4.0, and the
# engine's driver divides by the collider's own `moveSpeed`, which is 3.2. So the same
# authored thresholds -- 0.66 and 0.2, untouched -- are now 2.11 m/s and 0.64 m/s rather
# than 2.64 and 0.80.
#
# **That difference is the row's whole argument arriving as a number.** `speed / 4.0` was
# the game asserting that the machine's `run` threshold sat at 4 m/s, and the collider it
# was asserting about tops out at 3.2 -- so the parameter could never exceed 0.8, the top
# fifth of every blend was unreachable, and nothing anywhere could have said so. Derived
# from `MOVE_SPEED` here so it cannot drift out of step again.
RUN_SPEED="$(awk -v m=$MOVE_SPEED 'BEGIN { printf "%.4f", 0.66 * m }')"
WALK_SPEED="$(awk -v m=$MOVE_SPEED 'BEGIN { printf "%.4f", 0.20 * m }')"

# name | frames | input script. An empty script is a bare run, which is what `still` is.
#
# The main arm's sequence, and what each entry is for:
#   60  walk      -- forward alone, so the request is `kWalkFraction` of travel
#   150 run       -- the modifier, with the movement key untouched
#   240 walk      -- the modifier released, still with the movement key untouched
#   300 idle      -- the movement key released
#   330 jump      -- a tap, from a standstill, so `jump`, `fall` and `land` are the only
#                    thing moving and the arc is a clean one to check against
# 600 frames outruns the last of it by the landing clip's two seconds and a margin.
#
# The last two are C20's, and they are a pair that differs in one number. Both jump from a
# standstill at frame 60 and press jump a second time *in mid-air*; the launch is at step 60
# and the touchdown is 2v/g = 51.4 steps after it, so the landing is around step 111 and the
# `fall -> land` the trace reports is two steps behind that.
#
#   108  inside the ten-step buffer, and three steps before the character is back on the
#        ground -- so the second launch cannot be the press being read on a grounded frame
#    85  the identical press, twenty-six steps early, which is outside every window
#
# **A jump pressed while grounded proves nothing about a jump buffer**: the controller this
# row replaced would have passed it. Neither of these presses is on a frame the character
# could act on, which is the only shape in which the window is what produced the answer --
# and the two arms are read against the landing the log reports rather than against the 111
# above, so a solver that lands a step differently fails on the placement rather than
# silently stops testing the feature.
#
# G13's three come last and carry a fourth field, which is why every entry now ends in a
# `|`: they are the only arms that name a camera, and the first five must keep inheriting
# `Camera::frameBounds` or their numbers stop being the numbers C20 derived.
#
# The focus in `--camera` is where the character stands and is written for readability
# only -- the demo re-aims it at the character on the first frame and every frame after.
# What the flag is really setting is the yaw, which the rig never touches.
#
# 180 steps of walking is 4.2 m, which keeps the character inside the atrium in every
# direction. The direction is the assertion; the distance is the main arm's job.
CAMERA_AT="0,1,0.9"
WALK="60:Player.Forward+,240:Player.Forward-"
ARMS=(
    "still|600||"
    "modifier|300|60:Player.Run+|"
    "walk-run-jump|600|60:Player.Forward+,150:Player.Run+,240:Player.Run-,300:Player.Forward-,330:Player.Jump|"
    "jump-buffered|400|60:Player.Jump,108:Player.Jump|"
    "jump-eaten|400|60:Player.Jump,85:Player.Jump|"
    "camera-north|400|$WALK|--camera $CAMERA_AT,0,-10,5"
    "camera-south|400|$WALK|--camera $CAMERA_AT,180,-10,5"
    "camera-turning|400|$WALK|--camera $CAMERA_AT,0,-10,5 --camera-spin 1.5"
    "platform-ride|600|30:Scene.RidePlatform|"
)

# The frames the two presses above sit on, so the assertions and the scripts cannot drift
# apart.
BUFFERED_PRESS=108
EATEN_PRESS=85

failures=0

fail() {
    echo "  FAIL: $*" >&2
    failures=$((failures + 1))
}

# The step a named transition landed on, or empty. The log line is
# `Locomotion: <from> -> <to> at step N (...)`.
transition_step() {
    sed -n "s/.*Locomotion: $1 -> $2 at step \([0-9]*\) .*/\1/p" "$3" | head -1
}

# `expr` in floats, as a predicate. awk rather than bc: bc is not installed everywhere and
# every other script here already assumes awk.
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
    if ! timeout -s TERM 180 ./run.sh demo "$CONFIG" -- "${args[@]}" >"$log" 2>&1; then
        cat "$log" >&2
        echo "$name failed to run" >&2
        exit 1
    fi
    # The logger colourises for a terminal and the redirect above is not one it can ask,
    # so every line arrives wrapped in escapes. Stripped once, here, rather than in each of
    # the eight patterns below -- a `$` anchor that silently never matches is exactly the
    # assertion-shaped hole this script exists to close.
    sed -i 's/\x1b\[[0-9;]*m//g' "$log"

    # A script naming an action this build does not have, and a script running past
    # `--frames`, both look exactly like a feature that does not work. C16 made each an
    # error rather than a silence; this is the other half of that, which is somebody
    # actually reading them.
    if grep -q "ERROR" "$log"; then
        fail "$name logged an error"
        grep "ERROR" "$log" >&2
    fi

    # The two clips the row added have to be *found by name on the rig*, which is what
    # makes the machine six states rather than four with two written into the source.
    grep -q "state machine over 6 states (idle, walk, run, jump, fall, land)" "$log" ||
        fail "$name: the machine is not the six states built by name"

    # The player takes W, A, S, D, shift and space, and takes them *unmoved* -- no `*`,
    # which is what tells a shipped default apart from a live rebind.
    #
    # **C37 replaced the other half of this check with a stronger one.** It used to assert
    # `Camera.Forward=Up`, because the demo installed a flycam beside the character and had
    # to push five of its rows onto the arrow keys before the player could have W. There is
    # no flycam running now: the demo installs a `ThirdPersonCamera`, whose whole input
    # surface is one drag, and the assertion below is that *nothing else* camera-shaped is
    # live. An arrow-key row reappearing here would mean the two schemes are overlapping
    # again, which is the condition the dance existed to survive.
    bindings="$(sed -n 's/.*Shipped bindings: //p' "$log" | head -1)"
    case "$bindings" in
    *"Player.Forward=W Pad.LeftY-"*"Player.Run=LeftShift"*) ;;
    *) fail "$name: the shipped scheme is not WASD for the player -- $bindings" ;;
    esac
    [[ "$bindings" != *"*"* ]] || fail "$name: an action is off its default -- $bindings"

    camera_rows="$(sed -n 's/.*Shipped camera (follow): //p' "$log" | head -1)"
    [[ "$camera_rows" == "Camera.Orbit=Mouse.Middle" ]] ||
        fail "$name: the follow camera is not the only live camera scheme -- '$camera_rows'"

    path="$(sed -n 's/.*Locomotion path: //p' "$log" | head -1)"
    read -r changes travelled rise < <(sed -n \
        's/.*Locomotion: \([0-9]*\) changes over [0-9]* steps, \([0-9.]*\) m travelled, peak rise \([0-9.]*\) m.*/\1 \2 \3/p' \
        "$log" | head -1)
    # G13's four fields, read by a second pattern rather than by widening the one above.
    # That line is appended to and never rewritten, so the pattern that predates the row
    # still ends in `.*` and still matches -- which is what keeps the five arms above
    # parsing exactly as they did. Every ratio is signed, so the classes admit a minus.
    read -r net along across facing < <(sed -n \
        's/.*Locomotion: .* net \([0-9.]*\) m, along \(-*[0-9.]*\) across \(-*[0-9.]*\) facing \(-*[0-9.]*\).*/\1 \2 \3 \4/p' \
        "$log" | head -1)
    [[ -n "$net" ]] || fail "$name: the summary line carries no net displacement"

    # **Asserted on every arm, including the ones that press nothing.** How far the *pose*
    # carried the rig's root, in metres, worst over the run -- a number about the animation
    # rather than about the solver, and the only one here that is.
    #
    # The showcase clips are not authored in place: `walking` moves `Hips` 1.80 m and `running`
    # 3.20 m down the rig's own +Z. Unheld, that motion is applied on top of the controller's,
    # so the drawn character travels at the sum of the two and snaps back to nothing every time
    # the machine blends to a clip that stands still -- a mesh sliding out of its own capsule
    # and rubber-banding home on every release. `DemoGame` names the node to
    # `SceneAnimator::setRootNode`, which holds it at its bind translation, and this is the
    # assertion that the hold took.
    #
    # Measured against the run's first step rather than against the bind pose, so it is a claim
    # about motion and not about where the rig sits. Counterfactual, with the `setRootNode` call
    # taken out and nothing else changed: **3.17 m** on `walk-run-jump`, against 0.00 with it in.
    # Every other number in this file was identical across that pair, which is exactly why the
    # eight arms could not see this and needed a ninth kind of number rather than a ninth arm.
    drift="$(sed -n 's/.*Locomotion: .* drift \([0-9.]*\).*/\1/p' "$log" | head -1)"
    [[ -n "$drift" ]] || fail "$name: the summary line carries no pose drift"
    within "$drift" 0 0.02 || fail "$name: the pose moved the rig's root $drift m -- root motion is not being held"

    # The tenth kind of number, added for the same reason the ninth was: the arm below is one
    # the other nine could not have. Radians of yaw the drawn character swung through, worst
    # over the run, against the heading it started with -- and unlike every ratio above it
    # divides by nothing, which is the whole point. `travelled` is world displacement, so a
    # rider carried past the camera fills in all three ratios exactly as a walker does.
    turned="$(sed -n 's/.*Locomotion: .* turned \([0-9.]*\).*/\1/p' "$log" | head -1)"
    [[ -n "$turned" ]] || fail "$name: the summary line carries no turn"

    case "$name" in
    still | modifier)
        # Both arms: the character is asked for nothing it can act on, so nothing happens.
        # Not "roughly nothing" -- the character is standing on a static box under a locked
        # clock, and a millimetre of drift here is a defect with a cause.
        [[ "$path" == "idle" ]] || fail "$name: path is '$path', expected 'idle'"
        [[ "$changes" == "0" ]] || fail "$name: $changes state changes, expected none"
        within "$travelled" 0 0.01 || fail "$name: travelled $travelled m with nothing pressed"
        within "$rise" 0 0.01 || fail "$name: rose $rise m with nothing pressed"
        # And it went nowhere in particular. A character nobody moved has no direction to
        # agree with anything, so all three ratios are the zero the report writes when
        # there is no distance to divide by -- which is also what catches a trace that
        # accumulated a projection while `travelled` stayed flat.
        within "$net" 0 0.01 || fail "$name: net displacement $net m with nothing pressed"
        within "$along" 0 0.01 || fail "$name: went $along along the camera with nothing pressed"
        ;;
    walk-run-jump)
        expected="idle > walk > run > walk > idle > jump > fall > land > idle"
        [[ "$path" == "$expected" ]] || fail "$name: path is '$path', expected '$expected'"
        [[ "$changes" == "8" ]] || fail "$name: $changes state changes, expected 8"

        # 90 steps of walk, 90 of run, 60 of walk, at 60 Hz. Stated as arithmetic because a
        # number lifted off a passing run is a number that cannot be wrong. This is what the
        # distance *was* before C20, and it is still the starting point: a ramp changes each
        # of the three products by the area it loses climbing into the speed and gains
        # coasting out of it, and nothing else about the schedule moved.
        instant="$(awk -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v hz=$STEP_HZ \
            'BEGIN { printf "%.4f", (90 * m * w + 90 * m + 60 * m * w) / hz }')"

        # The four ramps, and each one's area against the step that replaced it. A linear
        # ramp between two speeds takes |dv|/a seconds and covers dv^2/2a less ground than
        # arriving instantly would -- so the two accelerations are debits and the two
        # decelerations, which coast *past* the new request rather than dropping to it, are
        # credits. Walk is 3.2 x 0.45 = 1.44 m/s and run is 3.2.
        #
        #   rest -> 1.44   accelerate    -1.44^2 / 2a
        #   1.44 -> 3.20   accelerate    -1.76^2 / 2a
        #   3.20 -> 1.44   decelerate    +1.76^2 / 2d
        #   1.44 -> rest   decelerate    +1.44^2 / 2d
        want="$(awk -v i="$instant" -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v a=$ACCEL -v d=$DECEL 'BEGIN {
            walk = m * w; gap = m - walk;
            printf "%.2f", i - walk * walk / (2 * a) - gap * gap / (2 * a) + gap * gap / (2 * d) + walk * walk / (2 * d)
        }')"
        within "$travelled" "$(awk -v w="$want" 'BEGIN { print w * 0.97 }')" \
            "$(awk -v w="$want" 'BEGIN { print w * 1.03 }')" ||
            fail "$name: travelled $travelled m, expected about $want m"

        # **And it has to be short of the instant model by more than rounding.** The 3% band
        # above still contains G12's 8.40, because the four ramps very nearly cancel -- so on
        # its own it would pass for a controller with no ramp in it at all. Half the derived
        # deficit is the margin: comfortably outside a step of discretisation, comfortably
        # inside the difference the model actually makes.
        ceiling="$(awk -v i="$instant" -v w="$want" 'BEGIN { printf "%.4f", i - (i - w) / 2 }')"
        within "$travelled" 0 "$ceiling" ||
            fail "$name: travelled $travelled m, which is not below the $ceiling m a controller with no ramp would give"

        # A rise is the one thing gravity cannot supply, which is why the check is on the
        # apex rather than on the displacement. v^2 / 2g, plus up to one step of overshoot.
        apex="$(awk -v v=$JUMP_SPEED 'BEGIN { printf "%.3f", v * v / (2 * 9.81) }')"
        within "$rise" "$apex" "$(awk -v a="$apex" -v v=$JUMP_SPEED -v hz=$STEP_HZ 'BEGIN { print a + v / hz }')" ||
            fail "$name: peak rise $rise m, expected between $apex and one step above it"

        # ------------------------------------------------------- the steps, and the point
        # Two steps of latency throughout: the frame reads the key and asks the controller,
        # the next step's `fixedUpdate` reads the speed that came back, and `simulate`
        # steps the machine after that.
        jump_step="$(transition_step idle jump "$log")"
        fall_step="$(transition_step jump fall "$log")"
        land_step="$(transition_step fall land "$log")"
        recover_step="$(transition_step land idle "$log")"
        gait_up="$(transition_step walk run "$log")"
        gait_down="$(transition_step run walk "$log")"

        for pair in "idle->jump:$jump_step" "jump->fall:$fall_step" "fall->land:$land_step" \
            "land->idle:$recover_step" "walk->run:$gait_up" "run->walk:$gait_down"; do
            [[ -n "${pair#*:}" ]] || fail "$name: ${pair%%:*} never happened"
        done
        [[ $failures -eq 0 ]] || break

        # **C20's central assertion**, and the one number in this script that a controller
        # without a ramp cannot produce. The modifier goes down at frame 150 and the request
        # jumps from 1.44 to 3.20; the machine changes gait when the *solver* reports more
        # than 2.64. Assigning the request crosses that on the next step and G12 measured the
        # transition inside six of frame 150. A ramp at 10 m/s^2 takes (2.64 - 1.44) / 10 =
        # 0.12 s to get there, which is 7.2 steps that have to elapse before the same six of
        # pipeline latency even start.
        ramp_up="$(awk -v r=$RUN_SPEED -v m=$MOVE_SPEED -v w=$WALK_FRACTION -v a=$ACCEL -v hz=$STEP_HZ \
            'BEGIN { print (r - m * w) / a * hz }')"
        within "$gait_up" "$(awk -v r="$ramp_up" 'BEGIN { print 150 + r }')" \
            "$(awk -v r="$ramp_up" 'BEGIN { print 150 + r + 7 }')" ||
            fail "$name: walk -> run at step $gait_up, expected $ramp_up steps of ramp after 150"

        # **G12's central assertion, kept.** Nothing about the movement key changed at frame
        # 240; only the modifier was released. The gait came back down because the solver
        # reported 1.44 m/s instead of 3.20 and the machine crossed its own threshold, which
        # is a transition an implementation reading the keyboard cannot produce. The ramp
        # barely moves this one -- 0.56 m/s at 40 m/s^2 is under a step -- and that asymmetry
        # is the deceleration row doing what it is for.
        ramp_down="$(awk -v r=$RUN_SPEED -v m=$MOVE_SPEED -v d=$DECEL -v hz=$STEP_HZ \
            'BEGIN { print (m - r) / d * hz }')"
        within "$gait_down" "$(awk -v r="$ramp_down" 'BEGIN { print 240 + r }')" \
            "$(awk -v r="$ramp_down" 'BEGIN { print 240 + r + 7 }')" ||
            fail "$name: run -> walk at step $gait_down, expected $ramp_down steps of ramp after 240"

        # The launch clip is a quarter of a second, and leaving it is what puts the
        # character into `fall`. Nothing is pressed between here and the landing.
        launch="$(awk -v c=$LAUNCH_CLIP -v hz=$STEP_HZ 'BEGIN { print c * hz }')"
        within "$((fall_step - jump_step))" "$launch" "$(awk -v l="$launch" 'BEGIN { print l + 6 }')" ||
            fail "$name: jump -> fall $((fall_step - jump_step)) steps after the launch, expected about $launch"

        # And the landing is ballistics: 2v/g seconds in the air, fifty-odd steps after the
        # last thing anybody pressed.
        flight="$(awk -v v=$JUMP_SPEED -v hz=$STEP_HZ 'BEGIN { print 2 * v / 9.81 * hz }')"
        within "$((land_step - jump_step))" "$(awk -v f="$flight" 'BEGIN { print f - 6 }')" \
            "$(awk -v f="$flight" 'BEGIN { print f + 6 }')" ||
            fail "$name: fall -> land $((land_step - jump_step)) steps after the launch, expected about $flight"

        # The landing plays out because the character is standing still. Movement would
        # have cancelled it, which is the transition the `land -> run` row exists for.
        recovery="$(awk -v c=$LANDING_CLIP -v hz=$STEP_HZ 'BEGIN { print c * hz }')"
        within "$((recover_step - land_step))" "$(awk -v r="$recovery" 'BEGIN { print r - 6 }')" \
            "$(awk -v r="$recovery" 'BEGIN { print r + 6 }')" ||
            fail "$name: land -> idle $((recover_step - land_step)) steps after landing, expected about $recovery"
        ;;
    jump-buffered | jump-eaten)
        # Both arms jump at 60 from a standstill and press jump a second time in the air.
        # Nothing horizontal is ever asked for, so the travel is the `still` arm's and the
        # rise is one jump's -- neither is what these arms are about, but both being wrong
        # would mean the arm was not running the scenario it says it is.
        within "$travelled" 0 0.01 || fail "$name: travelled $travelled m with nothing but jump pressed"

        first_jump="$(transition_step idle jump "$log")"
        land_step="$(transition_step fall land "$log")"
        second_jump="$(transition_step land jump "$log")"
        [[ -n "$first_jump" ]] || fail "$name: the character never jumped at all"
        [[ -n "$land_step" ]] || fail "$name: the character never landed"
        [[ $failures -eq 0 ]] || break

        # `fall -> land` is reported two steps after the sweep that found the ground: the
        # step after it is where `fixedUpdate` sees `airborne` fall to zero, and the machine
        # is stepped after that. So the last step on which a press is still a press *in the
        # air* is `land_step - 3`, and the earliest one the buffer can still carry to the
        # landing is `land_step - 2 - BUFFER_STEPS`.
        #
        # Asserted rather than assumed. If the solver ever lands a few steps differently
        # these two arms stop testing the window, and this is what makes that a failure with
        # a legible message instead of a pair that quietly passes for the wrong reason.
        press="$BUFFERED_PRESS"
        [[ "$name" == jump-buffered ]] || press="$EATEN_PRESS"
        window_opens=$((land_step - 2 - BUFFER_STEPS))
        airborne_until=$((land_step - 3))
        [[ "$press" -le "$airborne_until" ]] ||
            fail "$name: the press at $press was not in the air -- the landing reported at $land_step"

        if [[ "$name" == jump-buffered ]]; then
            [[ "$press" -ge "$window_opens" ]] ||
                fail "$name: the press at $press is outside the buffer, which opens at $window_opens"
            expected="idle > jump > fall > land > jump > fall > land > idle"
            [[ "$path" == "$expected" ]] || fail "$name: path is '$path', expected '$expected'"
            [[ "$changes" == "7" ]] || fail "$name: $changes state changes, expected 7"
            [[ -n "$second_jump" ]] || fail "$name: the buffered press never became a jump"
            # And it launched at the landing rather than on the frame it was pressed, which
            # is the difference between a buffer and a controller that jumps in mid-air.
            [[ -z "$second_jump" || "$second_jump" -gt "$press" ]] ||
                fail "$name: the second launch was at $second_jump, on or before the press at $press"
        else
            # **The negative arm.** The only difference from the arm above is the frame the
            # second press sits on, and it sits outside the window rather than inside it.
            [[ "$press" -lt "$window_opens" ]] ||
                fail "$name: the press at $press is inside the buffer, which opens at $window_opens"
            expected="idle > jump > fall > land > idle"
            [[ "$path" == "$expected" ]] || fail "$name: path is '$path', expected '$expected'"
            [[ "$changes" == "4" ]] || fail "$name: $changes state changes, expected 4"
            [[ -z "$second_jump" ]] ||
                fail "$name: a press $((land_step - press)) steps before the landing still jumped, at $second_jump"
        fi
        ;;
    camera-north | camera-south | camera-turning)
        # One key, held for 180 steps, in all three. The gait is the same each time and is
        # checked so that an arm which stopped walking cannot pass its direction assertion
        # by having no direction: a straight walk is 4.2 m and the turning one loses some of
        # that to C20's deceleration through the turn, which is why the floor is 3 m.
        expected="idle > walk > idle"
        [[ "$path" == "$expected" ]] || fail "$name: path is '$path', expected '$expected'"
        within "$travelled" 3.0 4.5 || fail "$name: travelled $travelled m, expected a 180-step walk"
        within "$rise" 0 0.01 || fail "$name: rose $rise m with nothing but a movement key pressed"

        # **The row's assertion.** The displacement projected onto the camera's own forward,
        # over the distance walked. Not "the character moved" -- which every arm above
        # already proves and which a heading 180 degrees out satisfies exactly as well.
        within "$along" 0.85 1.01 || fail "$name: went $along along the camera's forward, expected about 1"
        # The axes are not merely un-flipped, they are the right two. A basis that swapped
        # forward for right passes the line above at any yaw where the two are symmetric and
        # lands here instead.
        within "$across" 0 0.30 || fail "$name: $across of the walk was across the camera, expected about 0"
        # And the character is looking where it went. Read off the scene node rather than
        # off the angle the game wrote, so a rotation the sweep discarded reads as zero here.
        within "$facing" 0.85 1.01 || fail "$name: faced $facing of the way it walked, expected about 1"

        if [[ "$name" == camera-turning ]]; then
            # **The arm a fixed basis cannot pass.** The camera turns 1.5 degrees a frame
            # while the key is held, so a character that agrees with it walks a curve and
            # ends up near where it started; one resolving against anything fixed walks a
            # straight line and ends up a whole path length away. Measured at 0.31 against
            # exactly 1.00 for a fixed yaw, so half is a margin neither can drift across.
            within "$net" 0 "$(awk -v t="$travelled" 'BEGIN { print t * 0.5 }')" ||
                fail "$name: net $net m of $travelled m walked -- that is a straight line, not a turn"
        else
            # And the two straight arms went somewhere, in a straight line, which is what
            # makes their `along` a claim about a heading rather than about an average.
            within "$net" "$(awk -v t="$travelled" 'BEGIN { print t * 0.9 }')" "$travelled" ||
                fail "$name: net $net m of $travelled m walked -- the path was not straight"
        fi
        ;;
    platform-ride)
        # **The arm the other nine could not be.** All of them stand the character on ground
        # that does not move, and the one thing a heading derived from world displacement gets
        # wrong is a character whose ground does. C29's placement verb is what puts a rider on
        # the platform at all -- nothing in this scene walks onto it.
        grep -q "Placed the character on the platform" "$log" ||
            fail "$name: the placement never happened, so this arm rode nothing"

        # Carried, not walking, and both halves matter. The distance says the platform really
        # took it somewhere; the machine says the character knows it did not walk there, which
        # is `characterSpeed` being ground-relative.
        [[ "$path" == "idle" ]] || fail "$name: path is '$path', expected 'idle' -- a rider is not walking"
        [[ "$changes" == "0" ]] || fail "$name: $changes state changes riding a platform with nothing pressed"
        within "$travelled" 4.0 9.0 || fail "$name: carried $travelled m, expected about 6 over a full reversal"
        within "$rise" 0 0.01 || fail "$name: rose $rise m riding a level platform"

        # **The row's assertion, and it is not zero.** The character is dropped onto a platform
        # already doing 0.9 m/s and is dragged up to its speed over C20's acceleration ramp, so
        # for those few steps its ground-relative motion really is 0.9 m/s backwards -- a
        # heading, and the rig faces it. That settle is 0.27 rad and nothing after it moves:
        # the reversal is a sinusoid whose peak acceleration leaves a residual two orders below
        # `kFacingFloor`. Reading the heading off world displacement instead scores **3.14** on
        # this arm, measured -- the character faces the way the floor is going and swings a
        # half turn at each end of the run.
        within "$turned" 0 0.5 || fail "$name: turned $turned rad with nothing pressed -- the platform is steering it"
        ;;
    esac

    echo "   path: $path"
    echo "   $changes changes, $travelled m travelled, peak rise $rise m"
    echo "   net $net m, $along along the camera, $across across it, $facing faced, $drift m of pose drift"
    echo "   turned $turned rad"
done

if [[ $failures -ne 0 ]]; then
    echo "locomotion: $failures assertion(s) failed" >&2
    exit 1
fi
echo "locomotion: ${#ARMS[@]} of ${#ARMS[@]} arms pass"
