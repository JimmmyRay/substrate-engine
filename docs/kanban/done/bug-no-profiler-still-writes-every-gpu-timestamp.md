---
id: bug-no-profiler-still-writes-every-gpu-timestamp
title: no-profiler still writes every GPU timestamp
arc: bug
size: S
verification: trace, inspection, tests-4
---

# bug-no-profiler-still-writes-every-gpu-timestamp — no-profiler still writes every GPU timestamp

`--no-profiler` says it turns off three things and turns off two. Its help text at
[Config.cpp:619](../../../engine/core/Config.cpp#L619) reads

> `--no-profiler            no CPU scopes, no GPU queries, no trace`

and the flag never reaches `GpuProfiler`. The query pool is created unconditionally at
[Renderer.cpp:397](../../../engine/gfx/Renderer.cpp#L397), and `GpuScope`
([GpuProfiler.h:140](../../../engine/gfx/GpuProfiler.h#L140)) still writes both
`vkCmdWriteTimestamp2` calls and still emits its debug-utils label. What the flag actually
suppresses is the *recording* — `Profiler::recordGpuZone` returns early
([Profiler.cpp:755](../../../engine/core/Profiler.cpp#L755)) — so the queries are written,
read back, and discarded.

**Why this matters more than a wrong sentence.** `--no-profiler` is the arm used to measure
what the profiler costs; [guides/profiling.md](../../guides/profiling.md) quotes 0.023 ms a
frame in debug and says it is measured by differencing two runs "because `--no-profiler` is
precisely the arm that writes none". If that arm still writes fifty timestamps and still
collects them, the figure it produced is an underestimate of the profiler's cost by whatever
the GPU query path costs — which is unknown, because the one measurement that would reveal it
is the one this flag was supposed to provide.

Two honest fixes, and the card should pick the first unless measurement says otherwise:

1. **Make the flag true.** `GpuProfiler::init` is skipped when the profiler is disabled;
   `enabled()` and `available()` then report false and `GpuScope` already handles a null pool
   by returning `UINT32_MAX` from `beginZone`. The debug-utils label must survive — it is
   deliberately not tied to the timestamp succeeding, because a device without timestamps is
   exactly the one whose frame you most want to read in a capture, and that reasoning applies
   unchanged to a run that asked for no profiler.
2. **Make the text true**, if it turns out something depends on the queries. Then the help
   text says "no CPU scopes, no trace" and the discrepancy is recorded rather than fixed.

Two smaller corrections travel with this, both found in the same pass and both one line:

- [guides/profiling.md](../../guides/profiling.md) calls `Profiler::initialize(cfg)` in its
  Output example. The API is `Profiler::init`; that name has never existed.
- [`game/demo/DemoGame.cpp`](../../../game/demo/DemoGame.cpp#L6) includes `core/Profiler.h`
  and uses nothing from it. If
  [chore-a-game-gets-one-zone-and-its-fixed-step-gets-none](chore-a-game-gets-one-zone-and-its-fixed-step-gets-none.md)
  lands first it makes the include true and this half is already done; whichever runs first,
  the other drops it.

## Verification

- `./run.sh release -- --no-profiler --frames 240` under a RenderDoc capture, or with the
  query pool logged — **no timestamp queries are written**, and the pass labels are still
  present in the capture. Both halves matter; a fix that also removes the labels has broken
  the thing the labels were deliberately decoupled for.
- The profiler-overhead figure is **re-measured** with a flag that now means what it says, and
  `guides/profiling.md`'s 0.023 ms is corrected if it moves. That number is the reason this is
  a bug rather than a doc fix.
- A `--no-profiler` run still writes no trace file and still renders identically:
  `scripts/golden.sh check release` with the flag, eleven cases, byte-identical.
- `./test.sh` in all four configurations.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — the `Profiler::initialize` typo, and the
overhead figure if it moves.

[architecture/tooling.md](../../architecture/tooling.md) — the flag table, which lists what
`--no-profiler` does.

## Outcome

**Fix 1 was taken, and it is one line.** `Renderer::init` now short-circuits
`core::Profiler::enabled() && gpuProfiler.init(...)`, so with the flag on the query pool is
never created — and every `GpuProfiler` entry point already guards on that null handle, so
`beginFrame`'s reset, both `vkCmdWriteTimestamp2` calls and `collect`'s readback all
early-out with no new branches anywhere. `GpuScope` was not touched at all, which is how the
debug-utils labels survive: they are emitted before `beginZone` is reached.

**The flag was lying twice, and the second one was found while verifying the first.**
`Profiler::init` gated its truncate, its `SIGTERM`/`SIGINT` handlers and its writer thread on
`!config.outputFile.empty()` and never on `config.enabled` — so `--no-profiler --trace <path>`
created a zero-byte file and started a thread to write nothing to it. Same defect, one layer
down, and the same one-line shape of fix. `ProfilerTest.DisabledProfilerCreatesNoTraceFile`
pins it; before the fix that case fails on a file that exists.

**The re-measurement is the finding, and it is a negative one.** The card predicted the
0.023 ms figure was an underestimate "by whatever the GPU query path costs". Measured with a
control arm that now means what it says — Sponza, debug, a 3,800-frame difference, five
repetitions, medians:

| Arm | ms/frame |
|---|---|
| profiler on | 3.5745 |
| `--no-profiler` | 3.5526 |
| **difference** | **0.022** |

0.023 stands, to the resolution this method has. Two `vkCmd*` calls per pass and a
non-blocking `vkGetQueryPoolResults` do not show up in a CPU frame. The figure did not need
correcting; the *claim about how it was obtained* did, and that claim is now true for the
first time.

**Both smaller corrections landed.** `guides/profiling.md`'s `Profiler::initialize(cfg)` is
`Profiler::init(cfg)`. `DemoGame.cpp`'s unused `core/Profiler.h` include was made true by
[chore-a-game-gets-one-zone-and-its-fixed-step-gets-none](chore-a-game-gets-one-zone-and-its-fixed-step-gets-none.md),
which ran first, exactly as the card said whichever ran first would.

**One thing the card asked for that the tooling cannot do.** `scripts/golden.sh` accepts a
mode and a configuration and nothing else, so "the golden set *with the flag*" is not a run
it can make. The eleven cases were replayed by hand against the same baselines with
`--no-profiler` added: **11 of 11 byte-identical**, alongside `scripts/golden.sh check
release` at 11 of 11 without it. A pass-through for extra flags would be a change to the
suite that this card has no business making, and it is not obviously wanted — a golden suite
whose flags vary per invocation is a suite whose baselines mean different things.

**One behaviour change beyond the flag**, and it is a message: `logGpuTimings`'s unavailable
branch said "no timestamp support" for both reasons. It now names `--no-profiler` when that
is the reason, because sending someone to look at their driver for a flag they passed is the
kind of wrong a status line should not be.

Verified: `--no-profiler` runs show no `GPU profiler ready (N queries, ...)` line where the
control arm shows `256 queries`; zero validation errors with layers on; no trace file where
`--trace` is also given. 929 tests in each of debug, release, asan and tsan.
`scripts/golden.sh check release`, eleven of eleven.
