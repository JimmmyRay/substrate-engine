#!/usr/bin/env python3
"""
Per-pass GPU and CPU cost, from the trace rather than the log line (5.5).

    scripts/baseline.py                        the MSAA baseline table: 1/2/4/8
    scripts/baseline.py --zones                one configuration, zone by zone, GPU and CPU
    scripts/baseline.py --samples 4 --runs 1   a quick before/after
    scripts/baseline.py --json out.json        every frame's numbers as well
    scripts/baseline.py -- --no-ssr --taa      everything after -- goes to the engine
    scripts/baseline.py -- --windowed          map the window; off, so a sweep of twelve
                                               runs does not take the keyboard in turn

The table it prints is published in docs/architecture/tooling.md. Committing the tool is
the point: that table is a claim about this machine, and a claim nobody can re-run is an
anecdote with a border around it.

**It reads the Chrome trace, not the `GPU @` log line, and that is the whole reason it
exists in this shape.** `Renderer::logGpuTimings` prints `GpuProfiler::lastZoneMs`,
which is the *most recent frame's* duration -- one frame, whichever one the last collect
happened to land on. A tool that runs the engine five times and takes the median of that
line is taking the median of five arbitrary frames, so a single hitched frame moves the
published number by a factor of two. That is exactly the "bimodal GBuffer and
Bloom" behaviour documented in docs/architecture/tooling.md, and measuring it properly is
most of what made it go away: over a trace's ~240 frames the same zones are stable to
within a few percent.

The log line is still the right thing to read while iterating -- it is one line and it
is already on screen. It is not the right thing to build a table from.

Two frame numbers are reported, and they are not interchangeable. `wall` is
frame-to-frame time: it is what FPS means, and it contains every block on the GPU.
`CPU busy` is wall less those blocks, and it is the only one that answers "would a
faster CPU help". On a GPU-bound scene wall equals the GPU frame by construction, so a
single column labelled CPU is not a measurement -- it is the GPU frame under another
name, which is the bug this pair of columns replaced.

`--zones` ends the GPU block with an `unnamed` row: per frame, `Frame` less the union of
the zones inside it. It is there because the obvious hand calculation -- median `Frame`
minus the sum of the medians above it -- is a different quantity, and a badly behaved
one. See `unnamed_gpu_ms`.

**`--zones` reports CPU zones as well as GPU ones, and for most of its life it did
not.** `read_trace` filed an event into the zone table only when `cat == "gpu"`; every
CPU scope in the trace landed in `wall`, in `CPU busy`, or nowhere. So `CPU busy` could
say the CPU had spent eight milliseconds and nothing in this tool could say on what --
which is a benchmark harness that can see a regression and not attribute it, and it is
how a 55x regression in `Renderer::record` survived a day in the tree. Every CPU-side
figure quoted in docs/architecture/tooling.md before this existed was read out of the
raw trace by hand.
"""

import argparse
import json
import pathlib
import statistics
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Columns of the emitted table, in the order the frame records them. An explicit list
# rather than "whatever the trace contains", so a zone that stops being recorded shows
# up as a missing column instead of a silently shorter table. Zones that are zero at
# every sample count under the defaults -- Decals, Fog, TAA, Overlay -- are left out of
# the table but still reported by --zones.
TABLE_ZONES = ["Cull", "Shadows", "PunctualShadows", "GBuffer", "SSAO", "Lighting", "SSR", "Bloom", "Tonemap"]

# There used to be an `ANSI` strip and a `VRAM \[steady state\]: (\d+\.\d+) MiB` regex
# here, reading the engine's stdout because a log line was the only place that number
# existed. `Profiler::counter("vramMiB", ...)` writes it every frame now, so the figure
# comes out of the trace with the rest and the parser is one thing rather than two. The
# two were measured against each other before the regex went: 515.7 MiB from the log line
# and a median of 515.7 over 239 frames from the counter.

# The frame's three blocks on the GPU. Subtracting them from the frame's wall time is
# what separates "the CPU was busy" from "the CPU was asleep waiting for the GPU" -- and
# without that subtraction a GPU-bound frame reports a CPU cost equal to its GPU cost,
# because that is precisely what wall time is. The engine's own HUD subtracts the same
# three, so its `cpu` figure and this column are cross-checkable; keep the lists in step.
BLOCKING_PATHS = ("Frame/Renderer::waitFence", "Frame/Renderer::acquire", "Frame/Renderer::present")


def configured_game(config):
    """The executable a build directory holds, or exit with what to run instead.

    `build_game.sh <name>` records the name in the CMake cache, which is why this script
    -- and run.sh, and golden.sh through it -- keeps the signature it has always had:
    the choice of game is a property of the build directory, not a flag on every tool.
    """
    build = ROOT / "build" / config
    cache = build / "CMakeCache.txt"
    name = ""
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith("SUBSTRATE_GAME:STRING="):
                name = line.split("=", 1)[1].strip()
                break
    if not name:
        sys.exit(f"error: build/{config} holds no game -- ./build.sh produces a library and a\n"
                 f"       test binary, and nothing to run. Run: ./build_game.sh <name> {config}")
    binary = build / name
    if not binary.exists():
        sys.exit(f"error: {binary} not built. Run: ./build_game.sh {name} {config}")
    return binary


def unnamed_gpu_ms(spans):
    """Per frame: `Frame` less the union of the GPU zones inside it, in ms.

    **Computed per frame and by union, because neither shortcut works.** Summing the
    per-zone *medians* and subtracting them from the median `Frame` is not the same
    quantity: several zones are strongly right-skewed and skewed together -- a heavy
    frame is heavy in `GBuffer`, `Particles` and `AsRefit` at once -- so the median of
    the sum sits well above the sum of the medians. On the demo scene that estimator
    alone reports half a millisecond of "unattributed" frame that no timestamp supports.
    The union rather than the sum is what makes a nested zone (`ParticleSort` inside
    `Particles`) count once instead of twice.

    Zones are clipped to `Frame`'s span, so a zone recorded outside it -- `InstanceUpload`
    -- neither counts as named time nor drags the residual negative.
    """
    out = []
    for spans_in_frame in spans.values():
        frame = [s for s in spans_in_frame if s[2] == "Frame"]
        if not frame:
            continue
        start, end, _ = frame[0]
        covered, cursor = 0.0, start
        for lo, hi, _ in sorted(s for s in spans_in_frame if s[2] != "Frame"):
            lo, hi = max(lo, cursor), min(hi, end)
            if hi > lo:
                covered += hi - lo
                cursor = hi
        out.append((end - start - covered) / 1000.0)
    return out


def read_trace(path):
    """Per-zone durations in ms -- GPU and CPU -- counters, and per-frame wall/CPU-busy.

    Frame 0 is dropped from the steady-state tables. It is the startup frame -- window
    creation, device init and asset load all happen inside it -- and the profiler pins it
    in the window on purpose (0.5), which is exactly why a benchmark has to take it back
    out. It is returned separately instead of thrown away: `--startup` is the mode that
    reads it, and until that existed the tool could not see startup at all, so every zone
    added to `Engine::init` would have been invisible to the thing meant to report it.

    Wall and CPU busy are returned separately because they are different measurements
    and only one of them is about the CPU. The depth-0 `Frame` zone is wall time; the
    CPU-busy figure is that less BLOCKING_PATHS, matched to the same frame number.

    **CPU zones are keyed by path, GPU zones by name, and that asymmetry is deliberate.**
    A CPU scope mirrors the GPU zone of the pass it records, so `GBuffer` names one of
    each and a single table keyed by name would silently pool the two. The path --
    `Renderer::record/GBuffer` -- is what the profiler already writes to disambiguate
    them, and it carries the nesting a flat name cannot. The leading `Frame/` is stripped
    because every zone in a frame has it.
    """
    events = json.loads(path.read_text())

    gpu, cpu, counters, wall_by_frame, blocked_by_frame = {}, {}, {}, {}, {}
    # (start us, end us, name) per frame, for unnamed_gpu_ms. Durations alone cannot
    # answer where the frame's unattributed time is; positions can.
    gpu_spans = {}
    # Frame 0's CPU zones, by path, as [total ms, count]. **Summed and counted rather
    # than kept as one sample**, because "there is one startup" does not mean "there is
    # one of each zone in it": the demo builds the acceleration structure five times
    # inside `Game::init`, and last-value-wins reported that as the cost of the last one.
    startup = {}
    for e in events:
        # Three phases now. `M` is Chrome's metadata -- the `thread_name` events that
        # label the tracks -- and carries neither a duration nor a frame. `C` is a
        # counter: a quantity rather than a span, so it goes in its own table and never
        # into `cpu`, where its "duration" would be meaningless.
        phase = e.get("ph")
        if phase == "C":
            if e["args"].get("frame", 0) != 0:
                counters.setdefault(e["name"], []).append(e["args"][e["name"]])
            continue
        if phase != "X":
            continue
        frame = e["args"]["frame"]
        ms = e["dur"] / 1000.0
        if frame == 0:
            if e["cat"] != "gpu":
                row = startup.setdefault(e["args"].get("path", e["name"]), [0.0, 0])
                row[0] += ms
                row[1] += 1
            continue
        if e["cat"] == "gpu":
            gpu.setdefault(e["name"], []).append(ms)
            gpu_spans.setdefault(frame, []).append((e["ts"], e["ts"] + e["dur"], e["name"]))
            continue

        path_str = e["args"].get("path", e["name"])
        if e["name"] == "Frame" and e["args"]["depth"] == 0:
            # Reported as `wall frame`, so listing it again as a CPU zone would be the
            # same number under two labels -- which is the mistake the wall/CPU split
            # exists to stop.
            wall_by_frame[frame] = ms
            continue

        cpu.setdefault(path_str[len("Frame/"):] if path_str.startswith("Frame/") else path_str, []).append(ms)
        if path_str in BLOCKING_PATHS:
            blocked_by_frame[frame] = blocked_by_frame.get(frame, 0.0) + ms

    wall, busy = [], []
    for frame in sorted(wall_by_frame):
        ms = wall_by_frame[frame]
        wall.append(ms)
        busy.append(max(0.0, ms - blocked_by_frame.get(frame, 0.0)))
    return gpu, cpu, counters, wall, busy, startup, unnamed_gpu_ms(gpu_spans)


def run_once(config, samples, frames, extra, timeout):
    """One run. Returns (gpu, cpu, counters, [wall ms], [busy ms], startup, [unnamed ms])."""
    with tempfile.TemporaryDirectory() as tmp:
        trace = pathlib.Path(tmp) / "trace.json"
        # --locked is pinned rather than inherited. The engine ships a realtime clock,
        # which steps the simulation from wall-clock time -- so the number of animation,
        # particle and physics steps in a frame would become a function of how fast the
        # frame rendered, and a per-pass table would be measuring its own frame rate.
        # --headless is passed unless the caller asked for a window. A sweep is twelve
        # runs, and twelve windows mapping in turn take the keyboard away from whoever is
        # working while it measures -- golden.sh, readback.sh and locomotion.sh all pass it
        # for the same reason. It costs nothing measurable: the window is unmapped rather
        # than absent, so the surface, the swapchain and the present path are the ones a
        # visible run uses. `--windowed` puts the window back for watching a run.
        headless = [] if "--windowed" in extra else ["--headless"]
        extra = [a for a in extra if a != "--windowed"]
        cmd = ["timeout", "-s", "TERM", str(timeout), "./run.sh", config, "--",
               "--locked", "--audio-null", *headless, "--msaa", str(samples),
               "--frames", str(frames), "--trace", str(trace), *extra]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)

        if not trace.exists():
            # A failed run must not quietly become a missing sample the median then
            # papers over.
            print(f"  run failed: no trace written (exit {proc.returncode})", file=sys.stderr)
            return None

        gpu, cpu, counters, wall, busy, startup, unnamed = read_trace(trace)

    if not gpu or not wall:
        print("  run failed: trace holds no frames past frame 0", file=sys.stderr)
        return None

    return gpu, cpu, counters, wall, busy, startup, unnamed


def sweep(config, samples, runs, frames, extra, timeout):
    """Run every sample count and pool each one's frames across its runs."""
    out = {}
    for n in samples:
        gpu, cpu, counters, wall, busy, startup, unnamed = {}, {}, {}, [], [], {}, []
        for _ in range(runs):
            result = run_once(config, n, frames, extra, timeout)
            if not result:
                continue
            g, c, k, w, b, st, un = result
            for name, values in g.items():
                gpu.setdefault(name, []).extend(values)
            for name, values in c.items():
                cpu.setdefault(name, []).extend(values)
            for name, values in k.items():
                counters.setdefault(name, []).extend(values)
            wall.extend(w)
            busy.extend(b)
            unnamed.extend(un)
            # Last run wins rather than pooled: there is one startup per run and averaging
            # two of them would hide that the second was warm.
            if st:
                startup = st
        if not wall:
            sys.exit(f"error: every run at {n}x failed")
        print(f"    {n}x: {len(wall)} frames", file=sys.stderr)
        out[n] = (gpu, cpu, counters, wall, busy, startup, unnamed)
    return out


def med(values):
    return statistics.median(values) if values else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="release", choices=["debug", "release"],
                    help="build configuration (default: release -- a Debug baseline is not a baseline)")
    ap.add_argument("--samples", type=int, nargs="+", default=[1, 2, 4, 8],
                    help="MSAA sample counts to sweep (default: 1 2 4 8)")
    ap.add_argument("--runs", type=int, default=3,
                    help="runs per sample count (default: 3 -- each contributes a trace window of frames)")
    ap.add_argument("--frames", type=int, default=900, help="frames per run (default: 900)")
    ap.add_argument("--timeout", type=int, default=300, help="seconds per run (default: 300)")
    ap.add_argument("--zones", action="store_true",
                    help="one row per zone with median/min/max, instead of the sweep table")
    ap.add_argument("--startup", action="store_true",
                    help="frame 0 alone -- one column, no medians, because there is one startup")
    ap.add_argument("--json", type=pathlib.Path, help="also write every frame's numbers here")
    ap.add_argument("engine_args", nargs="*", help="flags after -- are passed to the engine")
    args = ap.parse_args()

    extra = args.engine_args[1:] if args.engine_args[:1] == ["--"] else args.engine_args

    # Which game a build directory holds is recorded in its CMake cache, so this reads
    # the name instead of hardcoding one. `./build.sh` produces a library and a test
    # binary and nothing to run; `./build_game.sh <name>` is what produces a program.
    binary = configured_game(args.config)

    results = sweep(args.config, args.samples, args.runs, args.frames, extra, args.timeout)
    flags = " ".join(extra) if extra else "engine defaults"

    if args.startup:
        # Frame 0, which every other mode drops. **No medians**: there is exactly one
        # startup per run by construction, so a median column would be a median of one
        # and would read as though it were not.
        #
        # Indented by path depth and sorted by path, so a step sits under the one that
        # called it and a level sums against its parent by eye. The gap between `Frame`
        # and the sum of its children is work no zone names, and naming it is the point.
        for n in args.samples:
            startup = results[n][5]
            if not startup:
                print(f"\n{n}x MSAA, {args.config}: frame 0 holds no zones", file=sys.stderr)
                continue
            width = max([26] + [len(k) for k in startup])
            total = startup.get("Frame", [0.0, 0])[0]
            print(f"\n{n}x MSAA, {args.config}, {flags} -- startup (frame 0)")
            print(f"{'zone':<{width}} {'ms':>9} {'n':>4} {'% of frame 0':>13}")
            for path in sorted(startup):
                ms, count = startup[path]
                share = 100.0 * ms / total if total > 0 else 0.0
                depth = path.count("/")
                label = ("  " * depth) + path.rsplit("/", 1)[-1]
                print(f"{label:<{width}} {ms:9.3f} {count:4d} {share:12.1f}%")
            named = sum(v[0] for k, v in startup.items()
                        if k.startswith("Frame/") and "/" not in k[len("Frame/"):])
            share = 100.0 * (total - named) / total if total else 0.0
            print(f"{'unnamed':<{width}} {total - named:9.3f} {'':>4} {share:12.1f}%")
    elif args.zones:
        # bench.sh's view: one configuration, every zone, with the spread visible. The
        # min/max columns are the point -- a zone whose max is twice its median is a zone
        # whose median you should not be quoting.
        for n in args.samples:
            gpu, cpu, counters, wall, busy, startup, unnamed = results[n]
            width = max([18] + [len(k) for k in cpu])
            print(f"\n{n}x MSAA, {len(wall)} frames, {args.config}, {flags}")
            print(f"{'GPU zone':<{width}} {'median':>8} {'min':>8} {'max':>8}")
            for name in sorted(gpu, key=lambda k: (k == "Frame", k)):
                v = gpu[name]
                print(f"{name:<{width}} {med(v):8.3f} {min(v):8.3f} {max(v):8.3f}")
            # The row that says whether the list above is complete, and the only honest way
            # to ask it: per frame, `Frame` less the union of the zones inside it. **Do not
            # answer this by subtracting the sum of the medians above from `Frame`** -- see
            # unnamed_gpu_ms; that difference is about half a millisecond on a scene where
            # every timestamp says the frame is fully attributed.
            if unnamed:
                print(f"{'unnamed':<{width}} {med(unnamed):8.3f} {min(unnamed):8.3f} {max(unnamed):8.3f}")

            # Sorted by path, so a pass sits directly under the zone that called it and a
            # level can be summed by eye against its parent. `total` is the pooled sum
            # rather than a per-frame one: a zone that records twice in a frame -- the
            # G-buffer's two phases, the two cull dispatches -- would otherwise report a
            # median that understates what the frame paid for it by half.
            print(f"\n{'CPU zone':<{width}} {'median':>8} {'min':>8} {'max':>8} {'total/frame':>12}")
            traced = len(wall)
            for name in sorted(cpu):
                v = cpu[name]
                print(f"{name:<{width}} {med(v):8.3f} {min(v):8.3f} {max(v):8.3f} "
                      f"{sum(v) / traced:12.3f}")
            print(f"{'wall frame':<{width}} {med(wall):8.3f} {min(wall):8.3f} {max(wall):8.3f}")
            print(f"{'CPU busy':<{width}} {med(busy):8.3f} {min(busy):8.3f} {max(busy):8.3f}")

            # Quantities, not durations, so they get their own block and no total: summing
            # a draw-call count over the window says nothing anybody wants to know. The
            # spread is the point here as much as it is above -- a counter whose max is far
            # from its median is what a spike in the zone table is *about*.
            if counters:
                print(f"\n{'counter':<{width}} {'median':>8} {'min':>8} {'max':>8}")
                for name in sorted(counters):
                    v = counters[name]
                    print(f"{name:<{width}} {med(v):8.1f} {min(v):8.1f} {max(v):8.1f}")
    else:
        header = ["MSAA"] + TABLE_ZONES + ["GPU frame", "wall", "CPU busy", "FPS", "VRAM"]
        print("| " + " | ".join(header) + " |")
        print("|" + "---|" * len(header))
        for n in args.samples:
            gpu, cpu, counters, wall, busy, startup, unnamed = results[n]
            cells = [f"{n}x"] + [f"{med(gpu.get(z, [])):.3f}" for z in TABLE_ZONES]
            wallMs = med(wall)
            cells.append(f"{med(gpu.get('Frame', [])):.3f}")
            cells.append(f"{wallMs:.3f}")
            cells.append(f"{med(busy):.3f}")
            # FPS comes from wall time, which is the only one of the three that is a frame
            # rate: CPU busy would report the rate of a machine with an infinite GPU.
            cells.append(f"{1000.0 / wallMs:.0f}" if wallMs > 0 else "-")
            cells.append(f"{med(counters.get('vramMiB', [])):.1f}")
            print("| " + " | ".join(cells) + " |")
        print(f"\nAll figures in ms. Medians over every traced frame of {args.runs} runs of "
              f"{args.frames} frames, {args.config} build, {flags}. VRAM in MiB.")
        print("`wall` is frame-to-frame time and paces FPS; `CPU busy` is wall less the "
              "frame's waitFence, acquire and\npresent. Where they are far apart the GPU is "
              "the limiter and the CPU has that much headroom.")

    if args.json:
        args.json.write_text(json.dumps(
            {"config": args.config, "runs": args.runs, "frames": args.frames, "engineArgs": extra,
             "samples": {str(n): {"gpu": results[n][0], "cpu": results[n][1],
                                  "counters": results[n][2],
                                  "wallMs": results[n][3], "cpuBusyMs": results[n][4],
                                  "startupMs": results[n][5], "unnamedGpuMs": results[n][6]}
                         for n in args.samples}}, indent=2) + "\n")
        print(f"raw numbers written to {args.json}", file=sys.stderr)


if __name__ == "__main__":
    main()
