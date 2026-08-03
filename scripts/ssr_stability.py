#!/usr/bin/env python3
"""
Does a reduced-resolution reflection crawl? -- the check a golden image cannot be.

    scripts/ssr_stability.py [--config release] [--frames N] [--spin DEGREES]
                             [--rt on|off] [--bloom on|off]

> **A golden is one frame; temporal stability is a property of a sequence.** `render.ssrScale`
> is recommended at 0.5 on the strength of still captures, every one of them `--locked` with
> TAA off -- so nothing said whether the stepped silhouette the joint-bilateral upsample
> leaves inside a reflection sits still or swims a pixel a frame.

So this orbits a camera over `mirror.gltf` and reports **mean absolute difference between
consecutive frames**, in 0-255 units per channel, restricted to the pixels the reflection
pass actually writes. Three resolution scales, TAA off and on -- and beside the mean, the two
columns that say what a mean of this distribution leaves out.

## One frame per run, because `--locked` is what makes that legal

There is no sequence dump in the engine and this does not add one. `--locked` feeds the
clock exactly one fixed step per frame, `--camera-spin` advances the yaw by a fixed angle
per frame rather than per second, and the renderer is bit-identical run to run -- which is
the same three properties `golden.sh` rests on. So frame 61 captured by its own run really
is the frame that followed the frame 60 captured by the run before it, and a sequence is N
invocations of `--capture-frame`.

## The band, and why it is measured rather than assumed

The metric has to be restricted to the pixels below the roughness cutoff, and the reference
locates those by a scanline -- *"nothing above y=415 moves at all"* -- which is a fact about
one camera in one still frame and stops being true the moment the camera orbits.

So the band is measured per frame, from the engine, by the one experiment that defines it
exactly: render the frame with the reflection pass on and again with `--no-ssr`, and the
pixels that differ are the pixels the pass contributes to.

**The complement is the control**, and it is the control the reader needs: those pixels are
ones `render.ssrScale` cannot reach at all, so their inter-frame difference is what this
camera's motion alone looks like on this scene. It must read the same in every arm at a given
TAA setting. Where it does not, the harness is measuring something other than what it claims.

The mask for a pair of frames is the *union* of the two frames' bands. A silhouette that
crawls moves across the boundary, and an intersection would discard exactly the pixels the
card is about.

## A mean, and the two columns that exist because a mean is not enough

The mean is the number the question was posed in and it is reported first. It is also, on its
own, the wrong statistic for this artefact, and the run that produced these columns is what
says so: at 0.5 with TAA off the band's mean absolute difference is **0.97x** full resolution's
while the 99th percentile of the same distribution is **1.85x** it. Both are true, over twelve
frames of the same run, on the same pixels. A mean cannot tell an
edge that slid smoothly across four pixels from one that waited three frames and jumped four,
because the total change is the same either way -- and hold-then-jump is precisely what a
staircase locked to a reduced grid does.

So `band >= 8` counts the pixels taking a visible step each frame and `band p99` is how far
the worst hundredth of them moved. A scale that is genuinely stable matches full resolution
in all three.

## Bloom is off by default here, and that is the control talking

SSR runs *before* bloom precisely so that a reflection glares, which means the reflection's
light reaches pixels outside the band -- so with bloom on the control is not a control.
Measured, at scale 1.0 against 0.5 with TAA off: the control moved 0.217 to 0.270, a quarter
of its own value, on pixels the reflection pass never wrote. Bloom is also a low-pass filter,
and a low-pass filter can only ever *hide* the artefact this exists to find.

`--bloom on` runs the same measurement in the shipping configuration, and both belong in an
answer. Whichever is chosen, the mask runs and the arms agree about it -- a band measured
without bloom and a difference measured with it would be two different frames.
"""

import argparse
import functools
import pathlib
import subprocess
import sys

# A hundred engine launches is a quarter of an hour, and Python block-buffers stdout the
# moment it is not a terminal -- so redirected to a file, which is how a run this long is
# actually watched, the whole thing arrives at the end and a stall is indistinguishable from
# progress. The alternative is remembering `-u` on the command line, every time.
print = functools.partial(print, flush=True)  # noqa: A001

# **The one script here that takes a Python dependency, and the asset path still does not.**
# The generator that once wrote the test scenes hand-rolled a PNG writer rather than add a
# package to something every contributor runs on a fresh clone; those scenes are committed
# now, so that path takes no dependency at all. Nothing on it reaches this: it is a
# developer measurement tool like `baseline.py`, run by hand, and what it does is a masked
# statistic over ninety 1600x900 images. In pure Python that is minutes of unfiltering PNG
# scanlines a byte at a time before any arithmetic happens.
try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # pragma: no cover -- the message is the whole point
    sys.exit(f"error: {exc.name} is needed to compare captures -- pip install numpy pillow")

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENE = "engine/assets/mirror.gltf"
OUT = ROOT / "debug_frames" / "ssr-stability"

# Past the load hitch and inside every default profiler window, which is the frame
# golden.sh captures for the same reason.
FIRST_FRAME = 60

# The three settings `render.ssrScale` admits at the ends and the middle of its range, and
# they are the three the reference quotes a still-frame figure for.
SCALES = [1.0, 0.5, 0.25]

# A pixel this far apart on any channel between consecutive frames is a visible step rather
# than a rounding difference. One of the two columns that look past the mean -- the other is
# the percentile below, which has no threshold in it at all.
STEP_DELTA = 8

# **The mean is not the whole answer, and this is why there is a percentile beside it.** A
# staircase that jumps a step redistributes the same total change into fewer, larger
# per-pixel differences, and a mean absolute difference is exactly blind to that: it is the
# same number whether an edge slid smoothly across four pixels or waited three frames and
# jumped four. So the tail is reported too. Per pixel, over the band, worst channel.
TAIL_PERCENTILE = 99.0

# How far past the band a reduced-resolution composite can reach, in full-resolution pixels.
# The upsample gives one reduced texel to the pixels it covers, so at 0.25 a texel outside
# the band lands on four pixels inside it and the reverse -- and the *reference* band, which
# is measured at scale 1.0, would call those four pixels control while `ssrScale` was moving
# them. Measured on this scene rather than argued: dilating by 3 leaves 134 such pixels at
# 0.25, and dilating by 4 leaves none at either reduced scale.
BAND_DILATE = 4


def args_taa(taa):
    return "on" if taa else "off"


def arm_name(scale, taa):
    return f"scale{scale:g}-taa{args_taa(taa)}"


def dilate(mask, radius):
    """True wherever `mask` is true within `radius` pixels in x or y. Separable, so the cost
    is two passes of 2r+1 whole-array ORs rather than a (2r+1)^2 window per pixel."""
    out = mask
    for axis in (0, 1):
        pad = [(radius, radius) if a == axis else (0, 0) for a in (0, 1)]
        wide = np.pad(out, pad)
        shifted = [np.take(wide, range(i, i + out.shape[axis]), axis=axis) for i in range(2 * radius + 1)]
        out = np.logical_or.reduce(shifted)
    return out


def run(config, frame, out_png, extra, timeout=120):
    """One capture. Returns True if the PNG landed.

    Retried once, and only where the run never got a device. `vkCreateDevice` on this
    machine's proprietary driver fails every few hundred launches for reasons that have
    nothing to do with what is being measured -- either returning `VK_ERROR_DEVICE_LOST` or
    not returning at all -- and a sweep of a hundred runs meets it about once. The
    alternative to retrying is a harness that reports a rendering result as a failure at
    random. A run that reached a device and then failed is returned as the failure it is,
    which is what keeps this from papering over a real one.
    """
    out_png.parent.mkdir(parents=True, exist_ok=True)
    log = out_png.with_suffix(".log")
    cmd = ["timeout", "-s", "TERM", str(timeout), "./run.sh", config, "--",
           # `--headless` never maps a window, `--locked` pins the clock and `--audio-null`
           # runs the whole mixer into nothing -- golden.sh gives the argument for all three.
           "--headless", "--locked", "--audio-null",
           # One frame past the capture, so the run does not exit before the frame it is for.
           "--frames", str(frame + 2),
           "--capture", str(out_png), "--capture-frame", str(frame),
           *extra, SCENE]
    for attempt in (1, 2):
        out_png.unlink(missing_ok=True)
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        text = proc.stdout + proc.stderr
        log.write_text(text)
        if out_png.exists():
            break
        # 124 is `timeout`'s: the driver went away inside `vkCreateDevice` and never came
        # back. "Ray query:" is the last line device creation logs, so its absence is how
        # a run that never got that far is told from one that rendered and then broke.
        stuck = proc.returncode == 124 or "VK_ERROR_DEVICE_LOST" in text
        if attempt == 1 and stuck and "Ray query:" not in text:
            print(f"  frame {frame}: no device on this launch -- retrying once", file=sys.stderr)
            continue
        print(f"  run failed (exit {proc.returncode}): {log}", file=sys.stderr)
        return False
    # A run that fell through to a software rasteriser produces a whole sequence of
    # plausible images and a meaningless answer, which is the failure golden.sh checks for
    # by name. Same check, same reason.
    for line in text.splitlines():
        if "Device:" in line:
            if any(s in line for s in ("llvmpipe", "lavapipe", "softpipe", "SwiftShader")):
                print(f"  ran on a software device -- refusing the measurement: {line}", file=sys.stderr)
                return False
            break
    return True


def load(path):
    """RGB as uint8. Ninety of these are held at once, so they stay the size they were
    written; a difference widens to int16 where it is taken."""
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def absdiff(a, b):
    """|a - b| per channel, as int16 -- uint8 subtraction wraps, and a wrap here would
    read as a perfectly stable pixel."""
    return np.abs(a.astype(np.int16) - b.astype(np.int16))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="release")
    ap.add_argument("--frames", type=int, default=12, help="consecutive frames per arm (default: 12)")
    # Degrees of yaw per frame. 0.1 is about three pixels of screen motion a frame at this
    # scene's framing -- slow enough that a staircase swimming under it is the difference
    # being looked for, rather than something buried in the motion of everything else.
    ap.add_argument("--spin", type=float, default=0.1, help="camera yaw per frame, degrees (default: 0.1)")
    ap.add_argument("--rt", choices=["on", "off"], default="on",
                    help="traced reflections or the screen-space march (default: on, which is what ships)")
    ap.add_argument("--bloom", choices=["on", "off"], default="off",
                    help="see the docstring: on is the shipping frame, off is the one with a control")
    args = ap.parse_args()

    frames = list(range(FIRST_FRAME, FIRST_FRAME + args.frames))
    if len(frames) < 2:
        sys.exit("error: --frames must be at least 2 -- a difference needs two frames")

    common = ["--camera-spin", str(args.spin)]
    if args.rt == "off":
        common.append("--no-rt")
    if args.bloom == "off":
        common.append("--no-bloom")

    # ------------------------------------------------------------------ the band, per frame
    print(f"band: {len(frames)} frames x 2 runs (reflection on/off), bloom {args.bloom}")
    masks = {}
    for f in frames:
        on = OUT / "band" / f"on-{f}.png"
        off = OUT / "band" / f"off-{f}.png"
        ok = run(args.config, f, on, common + ["--set", "render.ssrScale=1.0"])
        ok = run(args.config, f, off, common + ["--no-ssr"]) and ok
        if not ok:
            sys.exit("error: the band could not be measured; nothing below would mean anything")
        masks[f] = dilate(np.any(load(on) != load(off), axis=2), BAND_DILATE)
        print(f"  frame {f}: {masks[f].sum()} of {masks[f].size} pixels "
              f"({100.0 * masks[f].mean():.2f}%) in the band")

    # ------------------------------------------------------------------------- the arms
    shots = {}
    for scale in SCALES:
        for taa in (False, True):
            name = arm_name(scale, taa)
            print(f"== {name}")
            extra = common + ["--set", f"render.ssrScale={scale}"] + (["--taa"] if taa else [])
            shots[name] = {}
            for f in frames:
                path = OUT / name / f"{f}.png"
                if not run(args.config, f, path, extra):
                    sys.exit(f"error: {name} could not be captured at frame {f}")
                shots[name][f] = load(path)

    def pair_mean(name, mask_of):
        """This arm's mean |delta| between consecutive frames, over whatever `mask_of` says
        the pair's pixels are. Averaged over pairs rather than pooled, so a frame where the
        mask is small counts the same as one where it is large."""
        return float(np.mean([absdiff(shots[name][b], shots[name][a])[mask_of(a, b)].mean()
                              for a, b in zip(frames, frames[1:])]))

    rows = []
    for scale in SCALES:
        for taa in (False, True):
            name = arm_name(scale, taa)
            band = pair_mean(name, lambda a, b: masks[a] | masks[b])
            control = pair_mean(name, lambda a, b: ~(masks[a] | masks[b]))
            whole = pair_mean(name, lambda a, b: np.ones_like(masks[a]))
            # Per pixel rather than per channel, in both: a step is a pixel that moved, and
            # it moved if any channel did.
            worst = [absdiff(shots[name][b], shots[name][a]).max(axis=2)[masks[a] | masks[b]]
                     for a, b in zip(frames, frames[1:])]
            stepped = float(np.mean([(w >= STEP_DELTA).mean() for w in worst]))
            tail = float(np.mean([np.percentile(w, TAIL_PERCENTILE) for w in worst]))
            rows.append((name, band, control, whole, stepped, tail))
            print(f"   {name}: band {band:.4f}  control {control:.4f}  whole {whole:.4f}  "
                  f"stepped {100.0 * stepped:.2f}%  p{TAIL_PERCENTILE:g} {tail:.1f}")

    # ---------------------------------------------------------- the harness checking itself
    #
    # **The control has to be one.** `render.ssrScale` changes the resolution of one buffer
    # composited into the band, so outside the band -- past `BAND_DILATE` -- the three scales
    # must produce the same pixels, not similar ones.
    #
    # Asserted bit-for-bit in the one configuration where it is exactly true, and reported as
    # a drift in the two where it is not. Both exceptions are the same mechanism, a filter
    # whose reach reaches further than a fixed radius can be drawn: TAA's history carries a
    # difference into its neighbours every frame, so by frame 60 a band pixel has had sixty
    # frames to spread; bloom's chain runs to a mip whose texels are a hundred pixels wide,
    # which is exactly why `--bloom off` is the default here. Asserting in either would fail
    # on the physics rather than on a defect -- and the printed drift is what keeps that from
    # being a licence, because it says how far from a control the run actually was.
    outside = ~masks[frames[0]]
    for taa in (False, True):
        base = shots[arm_name(1.0, taa)][frames[0]][outside]
        for scale in SCALES[1:]:
            moved = int((base != shots[arm_name(scale, taa)][frames[0]][outside]).sum())
            if not taa and args.bloom == "off" and moved:
                sys.exit(f"error: scale {scale:g} moved {moved} channel values outside the band with TAA off "
                         f"and bloom off -- the control is not one, and neither are the numbers above")
            if taa or args.bloom == "on":
                print(f"control drift, taa {args_taa(taa)}, bloom {args.bloom}, scale {scale:g}: "
                      f"{moved} of {base.size} channel values ({100.0 * moved / base.size:.3f}%) "
                      f"outside the band")

    # ------------------------------------------------------- the same question, sharpened
    #
    # The band is 36% of the frame and most of it is the mirror floor, whose reflection is a
    # smooth gradient that a resolution change barely touches. So a band mean can be diluted
    # by pixels the setting never moved, and the reader is entitled to the version that
    # cannot be: **the pixels this scale actually changes, and the two arms measured on
    # exactly those.**
    #
    # It is not circular. The mask says *where* reducing the resolution shows up in a still
    # frame -- which is what the reference already published -- and the comparison is then
    # between the two arms' *temporal* behaviour on that one set of pixels. A staircase that
    # swims makes the reduced arm noisier there; a staircase that sits still does not.
    affected = []
    for scale in SCALES[1:]:
        for taa in (False, True):
            name = arm_name(scale, taa)
            full = arm_name(1.0, taa)
            hit = np.zeros(masks[frames[0]].shape, dtype=bool)
            for f in frames:
                hit |= np.any(shots[name][f] != shots[full][f], axis=2)
            here = pair_mean(name, lambda a, b: hit)
            there = pair_mean(full, lambda a, b: hit)
            affected.append((name, float(hit.mean()), here, there))

    # ---------------------------------------------------------------------- the answer
    print()
    print(f"mirror.gltf, {len(frames)} frames from {FIRST_FRAME}, {args.spin} deg of yaw per frame, "
          f"rt {args.rt}, bloom {args.bloom}")
    print("mean |delta| between consecutive frames, 0-255 per channel")
    print()
    print(f"{'arm':<20}{'reflection band':>17}{'control':>10}{'whole frame':>14}"
          f"{'band >= ' + str(STEP_DELTA):>12}{'band p' + f'{TAIL_PERCENTILE:g}':>12}")
    for name, b, c, w, s, t in rows:
        print(f"{name:<20}{b:>17.4f}{c:>10.4f}{w:>14.4f}{100.0 * s:>11.2f}%{t:>12.1f}")
    print()
    # Stated as a ratio against full resolution in the same TAA arm, because that is the
    # comparison the question is: not whether a reflection moves -- the camera is moving --
    # but whether reducing its resolution makes it move *more*.
    by_name = {n: b for n, b, _, _, _, _ in rows}
    by_tail = {n: t for n, _, _, _, _, t in rows}
    print(f"{'on the whole band':<24}{'this arm':>10}{'scale 1.0':>11}{'ratio':>8}"
          f"{'p' + f'{TAIL_PERCENTILE:g} ratio':>12}")
    for scale in SCALES[1:]:
        for taa in (False, True):
            name = arm_name(scale, taa)
            full = arm_name(1.0, taa)
            print(f"{name:<24}{by_name[name]:>10.4f}{by_name[full]:>11.4f}"
                  f"{by_name[name] / by_name[full]:>7.2f}x{by_tail[name] / by_tail[full]:>11.2f}x")
    print()
    print(f"{'on the pixels it moves':<24}{'this arm':>10}{'scale 1.0':>11}{'ratio':>8}{'of frame':>10}")
    for name, frac, here, there in affected:
        print(f"{name:<24}{here:>10.4f}{there:>11.4f}{here / there:>7.2f}x{100.0 * frac:>9.2f}%")


if __name__ == "__main__":
    main()
