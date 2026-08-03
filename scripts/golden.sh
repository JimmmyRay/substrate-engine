#!/usr/bin/env bash
#
# Golden-image regression (5.3).
#
#   scripts/golden.sh snap [config]    capture the current build as the baseline
#   scripts/golden.sh check [config]   capture again and compare against it
#
# The baselines live in debug_frames/golden/, which is gitignored. That is
# deliberate: Sponza is not in the repository either, so a golden committed here
# could not be reproduced on a fresh clone, and a regression suite that cannot be
# reproduced is worse than none -- it fails for reasons nobody can attribute.
#
# What this is for is the change you are making right now: snap before, check after,
# and read the diff for anything you did not intend. Every capture is taken at a
# stated frame from a fixed camera, and the renderer is bit-identical run to run,
# so a non-zero mean delta is a real difference and not sampling noise.
#
# Every case runs `--headless`, so the eight launches never map a window and never take
# focus. The window still exists and still presents; it is only unmapped, which is what
# keeps a headless capture byte-identical to a windowed one. Snap and check the same way
# regardless -- a baseline is only comparable to a run made like it.
#
# Every case also runs `--locked`, and pins it rather than inheriting it for the same
# reason every case names its own scene: the engine ships a realtime clock because that is
# what a game wants, and a suite that depends on frame 60 being the same frame 60 must say
# so itself. Inheriting `physics.clock` from substrate.json would fail thirteen baselines
# the day that value changed, for a reason with nothing to do with the renderer.
#
# And `--audio-null`, which is about the person running it rather than the image. Every
# decoder, filter and bus still runs -- the null device is a mix that goes nowhere, not a
# disabled subsystem -- so the suite still covers the audio path. What it stops is thirteen
# launches playing an ambience bed and a contact one-shot through the speakers of whoever
# asked for a pixel comparison.
set -euo pipefail

cd "$(dirname "$0")/.."

MODE="${1:-check}"
CONFIG="${2:-release}"
DIR="debug_frames/golden"
FRAME=60
FRAMES=90

# One entry per configuration worth pinning: name, then the flags that produce it.
# Kept small on purpose. Each entry is a full run of the engine, and a suite nobody
# waits for is a suite nobody runs.
# Every case names its scene explicitly, including the eight that are Sponza. They used
# to rely on `scene.path` from substrate.json, which made eight baselines depend on a
# config value -- so the day the default scene changed, eight cases would have failed for
# a reason that had nothing to do with the renderer. A regression suite must pin what it
# renders.
SPONZA="engine/assets/Sponza/glTF/Sponza.gltf"

# Ray tracing is on by default and now covers shadows as well as reflections, so `lit`
# exercises both; `no-rt` pins the fallback, which is one switch over two passes -- the
# shadow maps, and an SSR march across a scene with nothing smooth in it to march for.
# `mirror-no-rt` at the end of this list is where the second half is actually covered.
CASES=(
    "lit|$SPONZA"
    "albedo|$SPONZA --debug-view albedo"
    "normal|$SPONZA --debug-view normal"
    "depth|$SPONZA --debug-view depth"
    # The AO buffer, upsampled into the full-resolution swapchain by the debug view
    # rather than captured at its own size -- so this stays diffable against its baseline
    # if the pass ever changes resolution, which a --capture-target dump would not.
    "ssao|$SPONZA --debug-view ssao"
    # There was a `no-ibl` case here and it was retired rather than re-pointed. Its flag
    # stopped changing a pixel the day the environment term left the lighting pass, so it
    # pinned a byte-for-byte copy of `lit` and reported coverage it did not have -- which
    # is worse than no case, because the suite counted it. Whatever brings indirect light
    # back brings its own case, with its own baseline.
    "msaa1|$SPONZA --msaa 1"
    "no-rt|$SPONZA --no-rt"
    # A light inside the emissive mesh that represents it. Emissive geometry is built
    # non-opaque in the BLAS so a shadow ray passes through it, and until this case
    # existed nothing in engine/assets/ set emissiveFactor at all -- so the whole
    # non-opaque path, and the bug it was added to fix, had no golden behind it. A
    # regression here takes the ground from lit to black.
    "emissive|engine/assets/emissive.gltf"
    # The one case that is not Sponza, and it is here because S2 asked for it: the
    # cascade defect it carried forward "is the strongest argument this project has yet
    # produced for 5.3's golden set covering more than one scene -- Sponza has never
    # shown it." S3 found that defect by rendering this scene and it was right.
    #
    # It also pins the particle subsystem's determinism, which is not free: the sort is
    # a fixed comparison network and the slot allocator runs on the CPU precisely so
    # that frame 60 is the same frame 60 on every run. This case is what would notice
    # if either stopped being true.
    "particles|engine/assets/particles.gltf"
    # The only case with a skinned caster, and it exists because the suite could not see
    # a bug that corrupted every skinned cascade shadow: `recordShadows` drew the whole
    # command list with the scene's vertex buffer bound, while the skinned half of that
    # list carries a `vertexOffset` into `skinnedVertices`. Sponza has no skin, so nine
    # cases agreed on a frame that was wrong everywhere it mattered.
    #
    # `skin.gltf` is the right scene for it rather than `character.gltf`: it is 386
    # triangles, it has a `ground` for the column to cast onto, and its one clip is
    # driven off the fixed step, so frame 60 is the same frame 60 on every run.
    "skin|engine/assets/skin.gltf"
    # The only case with a solver in it (S4). It pins two things at once: that the
    # colliders a file authors still produce the same bodies in the same places, and that
    # Jolt is deterministic run to run -- a settling stack is the most sensitive thing in
    # this suite to a step order or a thread count changing, and it would drift silently
    # rather than break.
    "physics|engine/assets/physics.gltf"
    # The only two cases with a smooth surface in them, and the reason they are two is
    # that the reflection pass is two algorithms rather than one setting. `mirror` pins
    # the ray-traced path and `mirror-no-rt` pins the screen-space march, over the
    # identical scene.
    #
    # `mirror` is also where `rayshadow.glsl`'s "both paths, one calculation" becomes a
    # claim about pixels rather than about the source: the pylon's shadow on the wall and
    # that same shadow inside the wall's reflection in the floor are two answers to one
    # question, in one image, and a regression that splits them moves one edge and not
    # the other.
    #
    # `no-rt` above does not cover the march and never did. It renders Sponza, which has
    # no smooth surface anywhere, so the pass it is named for contributes almost nothing
    # to the image it pins. The two arms here differ over most of the frame: with the ray
    # query the floor reflects the wall and the smooth spheres reflect the sky, and
    # without it a ray that leaves the screen finds nothing, so the same floor is black
    # and the same spheres are dark. Pinning one of those images says nothing about the
    # other.
    "mirror|engine/assets/mirror.gltf"
    "mirror-no-rt|engine/assets/mirror.gltf --no-rt"
)

mkdir -p "$DIR"
failures=0
failed_names=()
harness_names=()

# Substrate logs the device it selected. Check it rather than trust it, because the
# failure this catches is otherwise silent: anything that costs the discrete card its
# presentation support makes VulkanContext skip it and fall through to a software
# rasteriser, and eight cases of CPU-rendered pixels compared against CPU-rendered
# baselines is a suite that passes while testing nothing.
software_device() {
    local dev
    dev="$(sed -n 's/.*Device: \([^(]*\).*/\1/p' "$1" | head -1)"
    if [[ -z "$dev" ]]; then
        echo "no device line"
        return 0
    fi
    case "$dev" in
    *llvmpipe* | *lavapipe* | *softpipe* | *SwiftShader* | *"Software Rasterizer"*)
        echo "$dev"
        return 0
        ;;
    esac
    return 1
}

# The counterpart to software_device, for the same class of mistake. A device without the
# ray-query extensions runs every `rt-*` case through the raster fallback and produces a
# baseline that looks fine and tests nothing -- worse than a failure, because it passes.
# VulkanContext logs this line on every run, in exactly these three forms.
ray_query_missing() {
    local state
    state="$(sed -n 's/.*Ray query: \(.*\)/\1/p' "$1" | head -1)"
    [[ "$state" != available* ]]
}

for entry in "${CASES[@]}"; do
    name="${entry%%|*}"
    flags="${entry#*|}"
    golden="$DIR/$name.png"
    log="$DIR/$name.log"

    if [[ "$MODE" == "snap" ]]; then
        # shellcheck disable=SC2086
        if ! timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null \
            --frames "$FRAMES" --capture "$golden" --capture-frame "$FRAME" $flags >"$log" 2>&1; then
            cat "$log" >&2
            echo "snap $name failed" >&2
            exit 1
        fi
        if dev="$(software_device "$log")"; then
            echo "snap $name ran on $dev -- refusing to baseline it" >&2
            exit 1
        fi
        if [[ "$name" == rt-* ]] && ray_query_missing "$log"; then
            echo "snap $name has no ray query on this device -- refusing to baseline the raster path" >&2
            exit 1
        fi
        echo "snap  $name"
        continue
    fi

    if [[ ! -f "$golden" ]]; then
        echo "MISS  $name (no baseline; run: scripts/golden.sh snap)"
        failures=$((failures + 1))
        failed_names+=("$name")
        continue
    fi

    # Nothing from a previous run may survive into this one. A case that never draws
    # leaves the last run's actual.png and diff.png sitting at these paths, and the keep
    # step below then files them under this run's number -- so an intermittent failure was
    # investigated by pixel-diffing two images from a run that had rendered correctly.
    # Absence is the honest artifact of a run that produced nothing.
    rm -f "$DIR/$name.actual.png" "$DIR/$name.diff.png" "$log"

    # shellcheck disable=SC2086
    if ! timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null \
        --frames "$FRAMES" \
        --capture "$DIR/$name.actual.png" --capture-frame "$FRAME" \
        --golden "$golden" --diff "$DIR/$name.diff.png" $flags >"$log" 2>&1; then
        # An image that changed and a run that never happened are different failures, and
        # only one of them is a regression. Every comparison the engine actually performs
        # logs a `Compare:` verdict, so a non-zero exit without one came from upstream of
        # the comparison -- a vanished binary, a timeout, a scene that would not load, a
        # lost device. Reporting those as "1 of 11 cases differ" is what cost an
        # investigation to classify as not-a-rendering-change.
        if grep -q "Compare: MISMATCH\|Compare: size mismatch" "$log"; then
            echo "FAIL  $name -- see $DIR/$name.diff.png ($log)"
            failures=$((failures + 1))
            failed_names+=("$name")
            continue
        fi
        echo "HARNESS  $name -- the run did not reach a comparison ($log)"
        sed 's/^/         | /' <<<"$(tail -n 3 "$log")"
        harness_names+=("$name")
        # Stop rather than carry on. Whatever stopped this case from running will stop the
        # rest for the same reason, and ten more identical failures read as a total
        # regression instead of as one broken harness.
        break
    fi

    # After the comparison, not instead of it: a match on the wrong device is still wrong.
    if dev="$(software_device "$log")"; then
        echo "FAIL  $name -- ran on $dev, not the GPU ($log)"
        failures=$((failures + 1))
        failed_names+=("$name")
        continue
    fi

    if [[ "$name" == rt-* ]] && ray_query_missing "$log"; then
        echo "FAIL  $name -- no ray query on this device, so this compared the raster path ($log)"
        failures=$((failures + 1))
        failed_names+=("$name")
        continue
    fi

    echo "ok    $name"
done

if [[ "$MODE" == "check" ]]; then
    # **Kept, because the next run overwrites them.** A failing case writes
    # `<name>.actual.png`, `<name>.diff.png` and `<name>.log` into the same paths the
    # next run uses, so re-running to "see if it happens again" destroys the only
    # evidence of the run that failed -- which is exactly what happened to an
    # intermittent failure that then took seven clean runs to not explain.
    #
    # The directory name is the run's own sequence number rather than a timestamp, so
    # this needs no clock and sorts in the order the failures happened.
    keep_artifacts() {
        keep="$DIR/failed"
        n=1
        while [[ -e "$keep/$n" ]]; do n=$((n + 1)); done
        mkdir -p "$keep/$n"
        for f in "$@"; do
            # The log is the one artifact that always exists, and the one a harness
            # failure is diagnosed from -- so it is copied on its own terms rather than
            # in a brace expansion whose failure would be swallowed with the images'.
            cp -f "$DIR/$f.log" "$keep/$n/$f.log"
            cp -f "$DIR/$f".{actual.png,diff.png} "$keep/$n/" 2>/dev/null || true
            cp -f "$DIR/$f.png" "$keep/$n/$f.expected.png" 2>/dev/null || true
        done
    }

    if [[ ${#harness_names[@]} -gt 0 ]]; then
        keep_artifacts "${harness_names[@]}" ${failed_names[@]+"${failed_names[@]}"}
        echo "harness failure: ${harness_names[*]} -- the suite did not run to completion"
        echo "kept: $keep/$n"
        # Distinct from 1 so a caller can tell "the renderer changed" from "the suite
        # never rendered". They are not the same news and only one of them is stop-the-line
        # for the change under test.
        exit 2
    fi

    if [[ $failures -eq 0 ]]; then
        echo "all ${#CASES[@]} cases match"
    else
        keep_artifacts "${failed_names[@]}"
        echo "$failures of ${#CASES[@]} cases differ"
        echo "kept: $keep/$n (${failed_names[*]})"
        exit 1
    fi
fi
