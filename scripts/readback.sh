#!/usr/bin/env bash
#
# The readback test (P2) -- the P arc's own standard, run as nine cases.
#
#   scripts/readback.sh [config]
#
# > **A texel authored is a texel presented.** Load a source PNG, present it, read the
# > swapchain back, and compare bit-exact against the source expanded by the integer scale.
#
# This is a stronger check than the golden suite and it is stronger for one reason: the
# expected image is *computed from the input* rather than snapped from a previous run. A
# failing golden case has an answer that is sometimes "the new image is right"; a failing
# case here has none, because there is nothing to re-snap against. The engine is simply
# wrong. That is what makes it a proof rather than a regression check, and it is why the
# comparison runs at tolerance 0 with zero pixels allowed to exceed it while the golden
# suite runs at 2.
#
# Everything the check needs is inside the binary: `--readback <image>` draws the named
# image at 1:1 in the top-left of the overlay's surface, and after the run the engine
# expands the same file by the presentation scale, places it at the letterbox offset, and
# holds the capture against it. The expected image and a diff are written beside the
# capture, so a failure can be looked at rather than only counted.
#
# The source has to be opaque: P2's check is about a value surviving a blend unchanged, and
# a translucent source would make the blend do something, which is the thing being ruled out.
# engine/assets/readback.png is committed, so the image itself is the record of that.
#
# Every case runs `--headless`, `--locked` and `--audio-null`, for the reasons golden.sh
# gives about all three.
set -euo pipefail

cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
DIR="debug_frames/readback"
IMAGE="res:/readback.png"
FRAME=60
FRAMES=90

# name | window | flags. The first five are P2's, across the two axes it has -- the scale, and whether
# the overlay is inside the virtual target or drawn onto the window after the blit -- plus
# the one that is neither, plus P4's.
#
# The scale-1 cases are not redundant with the scale-3 ones. Scale 1 with a virtual
# resolution equal to the window is the *elided* path -- the tonemap draws straight into
# the swapchain and no blit is recorded -- so it checks that the overlay, the sampler and
# the sRGB round trip preserve a texel before presentation is asked to. If it fails, the
# scale-3 failure would have been blamed on the blit.
#
# `letterbox` is the case that makes the row's central decision checkable. 1000x600 holds
# 320x180 three and a bit times over, so it presents at 3x with 20 columns and 30 rows of
# bars, and the image is expected 20 pixels in and 30 down. If the scale rounded up, if the
# leftover leaned the wrong way, or if the bars were an axis out, this is the case that
# says so -- and it says it in texels rather than by looking wrong.
#
# `sprite` and `sprite-letterbox` are P4's, and they are the reason this script is not a
# P2-only artefact. Every case above draws the source through the *overlay*, so all five
# prove the same five things -- the sampler, the sRGB round trip, the blit, the scale and
# the bars -- about a path a sprite does not take. `--readback-sprite` draws the same file
# as one sprite through an orthographic camera at one world unit per texel, which puts the
# projection, the quad, the texel-to-normalised divide and the premultiplied blend inside
# the same bit-exact comparison. A sprite pass checked only against snapped references is
# precisely what the P arc exists not to have.
#
# ------------------------------------------------------------------ the sheet cases (P5)
#
# The two `sheet` cases are the P arc's standard applied to *frame selection*, and they are
# the only check in the tree that can fail a sprite sheet showing the wrong cell. The source
# file is cut into its own four quarters -- 64x48 becomes four 32x24 cells -- a four-cell
# looping clip is played over it, and the capture is held against **one quarter** of the same
# file, bit-exact, at the presentation scale.
#
# Three things make it a check rather than a tautology:
#
#   1. **The expected cell is stated here, not read back out of the engine.** Cropping the
#      source to whatever cell the playback happened to select would compare frame selection
#      against itself and pass for every selection there is. `--readback-sheet-frame` is the
#      number this script computed; the engine reports a mismatch and names both.
#   2. **The two cases differ only in the rate**, and expect different cells from the same
#      run length. A frame index that came from anywhere but the clock -- a constant, the
#      frame counter, the sprite's index -- cannot satisfy both.
#   3. **The cells are asymmetric in different axes.** Cell 1 is column 1 row 0 and cell 2 is
#      column 0 row 1, over a cell that is 32 wide and 24 tall, so a slicing that transposed
#      the column and the row is a different rectangle in both cases.
#
# The arithmetic, and it is deterministic because `--locked` feeds the clock exactly one
# fixed step per frame: the capture is taken on frame 60, which is the 61st frame drawn, so
# 61 steps of 1/60 s have run and the clip is 1.017 s in.
#
#   at 1.5 fps  ->  1.525 cells  ->  cell 1, whose window is [0.667, 1.333) s -- 20 steps of
#                                   slack either side
#   at 2.5 fps  ->  2.542 cells  ->  cell 2, whose window is [0.800, 1.200) s -- 11 steps of
#                                   slack either side
#
# The slack is the point. Landing an assertion on a cell boundary would make this a question
# about float accumulation rather than about frame selection, and it would pass and fail at
# random -- which is worse than not checking at all.
CASES=(
    "native-inside|960x540|--virtual-resolution native"
    "native-outside|960x540|--virtual-resolution native --ui-outside-virtual"
    "scale3-inside|960x540|--virtual-resolution 320x180"
    "scale3-outside|960x540|--virtual-resolution 320x180 --ui-outside-virtual"
    "letterbox|1000x600|--virtual-resolution 320x180"
    "sprite|960x540|--virtual-resolution 320x180 --readback-sprite"
    "sprite-letterbox|1000x600|--virtual-resolution 320x180 --readback-sprite --ui-outside-virtual"
    "sheet-cell1|960x540|--virtual-resolution 320x180 --readback-sheet-fps 1.5 --readback-sheet-frame 1"
    "sheet-cell2|1000x600|--virtual-resolution 320x180 --readback-sheet-fps 2.5 --readback-sheet-frame 2"
)

# --------------------------------------------------------------- the lit case (P6)
#
# **The one case here whose expectation is not a value**, and the arc owes itself the reason.
# Every case above holds a texel against the file it came from. A *lit* sprite goes through
# the G-buffer, the lighting pass and the tonemapper, so its value is corrected four times
# over -- which is the whole reason a game would draw one that way. Bit-exactness is
# definitionally unavailable to it, and re-snapping is not the alternative: `golden.sh snap`
# is forbidden and a snapped reference is the standard this arc exists not to use.
#
# So the claim moves from the value to the **coverage**, which lighting cannot change. The
# silhouette of an alpha-cutout sprite is decided by the source's alpha, the cutoff, the
# pivot, the texel rect, the quad, the projection and the viewport transform -- every place a
# half-texel is lost -- and it is computed from the file rather than snapped.
#
# Two runs, and they differ in **one number**. `--readback-lit-cutoff 2` puts the cutoff
# above every alpha there is, so every fragment discards and the sprite disappears while the
# material, the instance, the indirect command and the pipeline stay exactly as they were.
# That is a stronger control than omitting the sprite, which would also change the draw list.
#
# The effects that bleed across a silhouette are off, and that is not a dodge: bloom, SSAO
# and SSR legitimately change pixels outside a sprite's outline, and the first property below
# is right to fail on them. What is being asserted is where the sprite is, not what a post
# pass does with it.
LIT_IMAGE="res:/cutout.png"
LIT_FLAGS="--virtual-resolution 320x180 --readback-lit-sprite --no-bloom --no-ssao --no-ssr --no-rt"

mkdir -p "$DIR"
failures=0

for entry in "${CASES[@]}"; do
    name="${entry%%|*}"
    rest="${entry#*|}"
    window="${rest%%|*}"
    flags="${rest#*|}"
    WIDTH="${window%x*}"
    HEIGHT="${window#*x}"
    log="$DIR/$name.log"

    # shellcheck disable=SC2086
    if timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null --frames "$FRAMES" \
        --width "$WIDTH" --height "$HEIGHT" \
        --readback "$IMAGE" --capture "$DIR/$name.png" --capture-frame "$FRAME" \
        --readback-expected "$DIR/$name.expected.png" --diff "$DIR/$name.diff.png" \
        $flags >"$log" 2>&1; then
        # The engine exits 0 on a match, and says so in a line worth surfacing: it names
        # the scale, so a case that silently presented at 1x when it meant 3x is visible
        # here rather than only in the log.
        echo "ok    $name -- $(sed -n 's/.*Readback: \(bit-identical[^$]*\)/\1/p' "$log" | head -1)"
    else
        echo "FAIL  $name -- see $DIR/$name.diff.png ($log)"
        sed -n 's/.*Readback: \(MISMATCH[^$]*\)/      \1/p' "$log" | head -1
        failures=$((failures + 1))
    fi
done

log="$DIR/lit-sprite.log"
# shellcheck disable=SC2086
if timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null --frames "$FRAMES" \
    --width 960 --height 540 --readback "$LIT_IMAGE" \
    --capture "$DIR/lit-sprite.bg.png" --capture-frame "$FRAME" \
    --readback-lit-cutoff 2 $LIT_FLAGS >"$DIR/lit-sprite.bg.log" 2>&1 &&
   timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null --frames "$FRAMES" \
    --width 960 --height 540 --readback "$LIT_IMAGE" \
    --capture "$DIR/lit-sprite.png" --capture-frame "$FRAME" \
    --readback-background "$DIR/lit-sprite.bg.png" --diff "$DIR/lit-sprite.diff.png" \
    $LIT_FLAGS >"$log" 2>&1; then
    echo "ok    lit-sprite -- $(sed -n 's/.*Silhouette: \(exact[^$]*\)/\1/p' "$log" | head -1)"
else
    echo "FAIL  lit-sprite -- see $DIR/lit-sprite.diff.png ($log)"
    sed -n 's/.*Silhouette: \(MISMATCH[^$]*\)/      \1/p' "$log" | head -1
    failures=$((failures + 1))
fi

# The sixth thing this has to prove, and it is not a case because it has no expected
# image: the scale is derived from the window, so it changes under the user's hands. A
# letterbox that is correct at 960x540 and a validation error at 1366x768 is the expected
# failure, and `--resize-every` is what drives it. Verdict is the validation layer's.
echo "resize soak (validation layer is the verdict; run in a debug build to see it)"
log="$DIR/resize.log"
if timeout -s TERM 120 ./run.sh "$CONFIG" -- --headless --locked --audio-null --frames 240 \
    --virtual-resolution 320x180 --resize-every 20 >"$log" 2>&1; then
    # Any error line at all. Validation messages arrive through Logger::error under
    # the Vulkan category, so this is the layer's verdict where layers are on and a
    # cheap smoke test where they are not.
    if grep -q "\[ERROR\]" "$log"; then
        echo "FAIL  resize -- error output in $log"
        failures=$((failures + 1))
    else
        echo "ok    resize -- 240 frames across 12 swapchains, no error output"
    fi
else
    echo "FAIL  resize -- the run did not complete ($log)"
    failures=$((failures + 1))
fi

if [[ $failures -eq 0 ]]; then
    echo "all ${#CASES[@]} readback cases bit-identical, plus the lit silhouette"
else
    echo "$failures case(s) failed"
    exit 1
fi
