---
id: chore-a-counter-in-the-trace-instead-of-a-regex-over-the-log
title: A counter in the trace instead of a regex over the log
arc: chore
size: M
verification: trace, tests-hosted, tests-4, golden-11
---

# chore-a-counter-in-the-trace-instead-of-a-regex-over-the-log — A counter in the trace instead of a regex over the log

Afterwards `core::Profiler::counter(name, value)` exists beside `scope`, the trace carries
Chrome `ph:"C"` events on their own tracks, `scripts/baseline.py` prints a counter block
beside its GPU and CPU ones, and the VRAM regex over stdout is deleted.

The profiler records durations and nothing else. Every event it writes is a `ph:"X"`
complete event ([Profiler.cpp:461](../../../engine/core/Profiler.cpp#L461)), so the trace can
say a zone got slower and can never say why. The quantities that would answer it — draw
calls, live instances, how many the cull rejected, body count, active voices, dropped
simulation steps, VRAM — are either logged, or held in a member nobody exports, or not
computed at all.

**The tell is already in the tooling.** `scripts/baseline.py` recovers VRAM with

```python
VRAM_LINE = re.compile(r"VRAM \[steady state\]: (\d+\.\d+) MiB")
```

— [baseline.py:64](../../../scripts/baseline.py#L64), a regular expression over the engine's
stdout standing in for instrumentation, and one that reports a single steady-state figure
because a log line is all there is to read. A counter gives the same number per frame, and
deleting that regex is a good share of this card's argument.

The engine already computes most of the values and throws them away: the cull-stat readback
at [Renderer.cpp:6762](../../../engine/gfx/Renderer.cpp#L6762), `InstanceTable::liveCount`,
`VulkanContext::memoryUsage`, and `simClock.droppedSteps()` at
[Engine.cpp:1773](../../../engine/Engine.cpp#L1773), which is compared against a previous
value purely so it can be logged when it changes. Those are the first call sites.

## The design, and the one thing that is easy to get wrong

```cpp
static void counter(const char* literalName, double value);   // core/Profiler.h
```

Same contract as `scope`: a string literal, stored by pointer, allocation-free, dropped when
the profiler is disabled. Recorded into the current `FrameData` and emitted from `writeTrace`
([Profiler.cpp:444](../../../engine/core/Profiler.cpp#L444)). Perfetto renders `ph:"C"` as a
track graph with no viewer work, which is why this format rather than a sidecar file.

**A counter's `ts` must be on the same synthetic cumulative timeline the scopes use.**
`writeTrace` does not emit wall-clock time — it concatenates frames by
`cumulativeUs += frame.durationUs` ([Profiler.cpp:447](../../../engine/core/Profiler.cpp#L447)),
so a counter stamped from `steady_clock` would draw a graph that does not sit above the zones
it explains. This is the whole reason the emit belongs inside `writeTrace` and not beside the
call site.

Unlike a scope, a counter is **last-value-per-frame** rather than a stack: two writes in one
frame is a caller correcting itself, not two events. That also bounds the storage, which is
what keeps the recording path allocation-free.

## Verification

- `scripts/baseline.py --config release --zones` prints a counter block, and the VRAM figure
  in it **agrees with the log line the regex used to read** — measured once with both in
  place, which is the only way to show the replacement is faithful. Then the regex goes.
- The trace loads in Perfetto with the counters on their own tracks, aligned over the frames
  they belong to. A trace where the counter graph is offset from the zones is the timeline
  bug above and is the specific thing to look for.
- `./test.sh` in all four configurations, with new cases in `tests/ProfilerTests.cpp`: a
  counter recorded with no frame open, two counters of the same name in one frame, a counter
  while the profiler is disabled, and the emitted JSON still parsing. **`tsan` in
  particular** — this adds a second kind of per-frame record to the thread slots, and the
  unit suite is the only place that threading is checked at all.
- `scripts/golden.sh check release` — eleven cases, byte-identical.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — the larger of the two. It describes
`scripts/baseline.py` and the trace format, and both change here; the VRAM regex is described
there as the way that figure is obtained.

[guides/profiling.md](../../guides/profiling.md) — a counters section beside the CPU scopes
and GPU zones ones, and the "Output" section's account of what the trace contains.

## Outcome

`core::Profiler::counter(name, value)`, `ph:"C"` events in the trace, a counter block in
`scripts/baseline.py --zones`, and the VRAM regex deleted. Eleven counters, all of them
quantities the engine was already computing and throwing away:

| Counter | median on the demo, release |
|---|---|
| `visibleTriangles` | 313,678 |
| `vramMiB` | 515.7 |
| `vramAllocations` | 132 |
| `liveInstances` | 121 |
| `drawCalls` | 113 |
| `visibleInstances` | 88 |
| `particles` | 504 |
| `nodes` | 40 |
| `bodies` | 13 |
| `audioSources` | 9 |
| `droppedSteps` | 0 |

**The faithfulness check the card asked for passed exactly.** One run with both the counter
and the regex in place: the log line said `VRAM [steady state]: 515.7 MiB` and the counter's
median over 239 frames was 515.7. The regex went afterwards, and `baseline.py`'s `VRAM` column
now reads `counters["vramMiB"]`. `run_once` no longer returns a value scraped from stdout at
all, so the tool has one parser rather than two — which was a good share of the card's
argument and is the part that will keep paying.

**The timeline trap was real and is pinned by a test rather than by an eyeball.** The card
warned that a counter stamped from `steady_clock` would draw a graph offset from the zones it
explains, because `writeTrace` concatenates frames by `cumulativeUs` and emits no wall-clock
time. `counter()` therefore takes a value and no timestamp, and the emit lives inside
`writeTrace`. `CountersSitOnTheSameTimelineAsTheScopesTheyExplain` asserts each frame's
counter is at or before the first zone of its own frame and strictly after the previous
frame's; a 239-frame demo trace gives **2,629 counter events, zero misaligned**.

**Storage is bounded by the overwrite rather than by a cap**, which is what keeps the
recording path allocation-free after the first frame that uses a name: `counter()` scans the
slot's handful of entries by pointer and overwrites in place, and `collectFrame` clears the
list rather than carrying it. Clearing matters for a reason the card did not raise — a caller
that *stops* writing a counter should see the track stop, not see the last value repeated
forever.

**Adding a phase broke a reader again**, exactly as the previous card's `ph:"M"` did.
`baseline.py` would have filed every counter into the CPU zone table, where a counter's
"duration" is meaningless. It now branches on the phase up front. This is the second card in
a row to find that the trace's consumers assume one event shape; anything else that reads a
trace has to branch on `ph`.

934 tests in each of debug, release, asan and tsan — four new cases: a counter with no frame
open, three writes of one name in one frame, a counter while the profiler is disabled, and
the timeline alignment. **tsan in particular**, since this adds a second kind of per-frame
record to the thread slots. `scripts/golden.sh check release`, eleven of eleven
byte-identical. The counter block and the sweep table both print, and the sweep's VRAM column
comes from the trace.
