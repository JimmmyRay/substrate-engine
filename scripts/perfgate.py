#!/usr/bin/env python3
"""
Refuse a commit that made the frame slower (5.7).

    scripts/perfgate.py                  check the tree against perf-budget.json
    scripts/perfgate.py --update         re-baseline, deliberately
    scripts/perfgate.py --config debug   check a different configuration

Exit 0 inside budget, 1 over it, 2 if it could not measure. Installed as a `pre-commit`
hook by `scripts/install_hooks.sh`, where it runs **only when a commit touches something
that can move the frame** -- `engine/gfx/`, `engine/shaders/` or `engine/scene/`. A doc
commit pays nothing, which is what keeps the hook from being the thing everyone passes
`--no-verify` to.

## It gates two zones, and the other four would make it lie

`Lighting` and `Frame` only. `GBuffer`, `SSAO`, `SSR` and `Bloom` settle into one of two
whole-run states about 5% apart -- tooling.md, "The bimodal zones" -- so a gate on them
fails on a coin flip, and a gate that fails at random is one nobody reads. `Lighting` and
`Frame` are the two that hold, which is why the reference quotes those and not the others.

## The minimum, not the median, and that is the whole trick

A pre-commit hook runs on a machine that is also doing something else: another session's
build, a browser, the shader compile that just finished. Load moves a median a long way and
a minimum barely at all -- a loaded run still contains frames that ran unobstructed. Taking
the per-frame minimum is what makes this measure the *code* rather than the machine, and it
is why the budget can be tight enough to catch something real.

The margin is **12%** over the recorded minimum. That is far outside the 5% bimodality the
reference documents and far inside the kind of regression worth blocking a commit for -- the
incident this was written after was about 2x. A change that genuinely costs 12% is one that
should be re-baselined on purpose rather than absorbed.

## Re-baselining is deliberate, exactly as a golden snap is

`--update` rewrites `perf-budget.json`, which is committed, so the new number arrives in a
diff somebody reads. There is no automatic drift: a budget that quietly followed the tree
upward would report green while the frame got slower every week, which is the failure this
exists to prevent.
"""

import argparse
import json
import pathlib
import statistics
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUDGET = ROOT / "perf-budget.json"

# The arm. Fixed on purpose: a gate whose scene or camera moved with the tree would compare
# two different pictures and call the difference a regression. `--locked` pins the clock,
# `--headless` never maps a window (so a hook cannot steal focus), and the camera is written
# out rather than left to `frameBounds`, which follows the scene's bounds and would shift the
# moment an asset did.
ARM = ["--locked", "--audio-null", "--headless", "--msaa", "4",
       "--camera", "0.00,1.16,1.80,63.6,-12.6,7.87"]
FRAMES = 300
ZONES = ("Lighting", "Frame")
MARGIN = 1.12


def measure(config: str) -> dict:
    """Per-frame minimum of each gated zone, in ms. Returns {} if nothing was traced."""
    binary = ROOT / "build" / config / "demo"
    if not binary.exists():
        print(f"perfgate: {binary} is not built. Run: ./build_game.sh demo {config}",
              file=sys.stderr)
        sys.exit(2)

    with tempfile.TemporaryDirectory() as tmp:
        trace = pathlib.Path(tmp) / "trace.json"
        cmd = ["timeout", "-s", "TERM", "180", "./run.sh", config, "--",
               *ARM, "--frames", str(FRAMES), "--trace", str(trace)]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if not trace.exists():
            print(f"perfgate: no trace written (exit {proc.returncode}); not measured",
                  file=sys.stderr)
            print(proc.stderr[-2000:], file=sys.stderr)
            sys.exit(2)
        events = json.loads(trace.read_text())
        events = events["traceEvents"] if isinstance(events, dict) else events

    per_zone: dict[str, list[float]] = {}
    for e in events:
        if e.get("ph") == "X" and e.get("cat") == "gpu" and e.get("name") in ZONES:
            per_zone.setdefault(e["name"], []).append(e["dur"] / 1000.0)

    # A zone that appears in under a third of the frames is a zone whose pass did not run
    # for most of the trace -- reporting a minimum over six samples as this tree's cost is
    # how a gate ends up defending a number that means nothing.
    return {k: min(v) for k, v in per_zone.items() if len(v) >= FRAMES // 3}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="release")
    ap.add_argument("--update", action="store_true", help="re-baseline, deliberately")
    args = ap.parse_args()

    now = measure(args.config)
    if not now:
        print("perfgate: the trace held none of the gated zones; not measured",
              file=sys.stderr)
        return 2

    if args.update or not BUDGET.exists():
        BUDGET.write_text(json.dumps(
            {"config": args.config, "frames": FRAMES, "arm": ARM, "marginPct": round((MARGIN - 1) * 100),
             "note": "Per-frame minimum in ms on this machine. scripts/perfgate.py --update rewrites it.",
             "zones": {k: round(v, 4) for k, v in sorted(now.items())}}, indent=2) + "\n")
        verb = "updated" if args.update else "created"
        print(f"perfgate: {verb} {BUDGET.name} -- " +
              "  ".join(f"{k} {v:.3f}" for k, v in sorted(now.items())))
        return 0

    budget = json.loads(BUDGET.read_text())
    if budget.get("config") != args.config:
        print(f"perfgate: budget is for {budget.get('config')}, measured {args.config}; "
              f"not comparable", file=sys.stderr)
        return 2

    over = []
    for zone in sorted(now):
        recorded = budget["zones"].get(zone)
        if recorded is None:
            continue
        limit = recorded * MARGIN
        flag = "OVER" if now[zone] > limit else "ok"
        print(f"  {zone:<10} {now[zone]:7.3f} ms   budget {recorded:.3f}  limit {limit:.3f}  {flag}")
        if now[zone] > limit:
            over.append((zone, now[zone], recorded))

    if not over:
        print("perfgate: inside budget")
        return 0

    print("\nperfgate: the frame got slower.", file=sys.stderr)
    for zone, got, recorded in over:
        print(f"  {zone} {got:.3f} ms against {recorded:.3f} "
              f"({(got / recorded - 1) * 100:+.1f}%)", file=sys.stderr)
    print("\nThis is a minimum over 300 frames, so it is not machine load. Either the change\n"
          "made the frame slower, or the cost is understood and intended -- in which case\n"
          "re-baseline on purpose with `scripts/perfgate.py --update` and say why in the\n"
          "commit. `git commit --no-verify` skips the gate and leaves no record that it did.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
