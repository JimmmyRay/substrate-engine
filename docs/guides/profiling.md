# Profiling

Substrate profiles itself from frame zero. CPU scopes and per-pass GPU zones land on
a single Chrome Tracing timeline.

## CPU scopes

```cpp
#include "core/Profiler.h"

void Renderer::doThing() {
    auto s = Profiler::scope("Renderer::doThing");   // allocation-free
}
```

`Profiler::scope()` takes a **string literal** and stores it by pointer. Scopes nest
automatically by call stack and record into thread-local storage, so recording never
contends on a mutex and never allocates in steady state.

For genuinely dynamic names use `scopef()`, which formats and interns:

```cpp
auto s = Profiler::scopef("Shadow %u", cascadeIndex);
```

Interning means the allocation happens once per distinct name, not once per frame.
Prefer `scope()` where the name is fixed.

`Profiler::beginFrame()` marks a frame boundary and returns a scope covering the
whole frame; hold it until the frame ends.

### What is instrumented

The frame's own spine — `Renderer::record`, `waitFence`, `acquire`, `submit`, `present`,
`glfwPollEvents`, `input`, `simulate`, `writeback`, `Engine::spatialIndex` — the game's three
entry points, and **every pass `Renderer::record` calls**, one scope on the pass's first line.
Two rules:

| The pass | Its CPU zone is named |
|---|---|
| has a `GpuScope` | **that zone's name, spelled identically** — `SSR`, not `Ssr` |
| has none | its own function's name — `updateInstances`, `recordInstanceUpload` |

So `SSR` on the GPU track and `Renderer::record/SSR` on the CPU one are the same pass and can
be read against each other, and a name in the second style says there is no GPU row to look
for. `recordCull` and `recordGBuffer` each name two, by the same phase ternary their
`GpuScope` uses.

**A game gets `Game::frameUpdate`, `Game::fixedUpdate` and `Game::drawUi`, and they are three
zones rather than one.** Logic and UI fail differently, and `fixedUpdate` runs on the
deterministic clock rather than the frame one — so it may execute zero to four times in a
frame, and its `total/frame` column is a per-frame sum over however many steps the clock
consumed rather than a per-step median. A game's own scopes nest under whichever of the three
called them; `game/demo/` carries four as the worked example, and a game adds nothing beyond
`Profiler::scope` to make the nesting happen.

**What `drawFrame` does around `record` is named too, and those rows are read by max rather
than by median.** `syncImages`, `ensureSpriteCapacity`, `ensureInstanceCapacity`,
`pollShaderReload`, `pipelineRebuild` and `applyPendingScene` cost nothing on almost every
frame and milliseconds on one — a device wait, a buffer realloc, a `glslc` run, a scene
upload — so the `max` column `substrate bench --zones` prints is the number to look at
and the median says only how cheap the miss is. Measured on the demo in debug:

| Zone | median | max |
|---|---|---|
| `pollShaderReload` | 0.0003 | **3266** (a recompile) |
| `pipelineRebuild` | 0.0002 | **23.5** (a feature key change) |
| `syncImages` | 0.0001 | **0.76** |
| `ensureSpriteCapacity` | 0.0001 | **0.31** |

`updateUniforms` is the exception on that list and is read by median: it is not a spike but a
per-frame cost, four fifths of which is `updateUniforms/updateLights` — a CPU light cull and
two sorts whose cost is a function of light count. Invisible on Sponza at 0.035 ms; on a level
with a thousand lights it is a frame budget, and before this it had no row at all.

`ensureInstanceCapacity` sits under `Game::frameUpdate` rather than under `Renderer` — the
frame path never grows the instance buffer, `Engine::addModel`, `createMesh` and `removeModel`
do, and a game calls those.

**The simulation step is broken out the same way**, by the same two rules — none of these
has a GPU zone, so all of them are function names:

| Under `simulate` | Under `writeback` |
|---|---|
| `SceneAnimator::update`, `SpriteTable::update`, `ParticleSystem::update`, `PhysicsWorld::step` | `Scene::update` |
| `audioSources`, `audioOcclusion`, `AudioEngine::update` | |

`SceneAnimator::update` and `PhysicsWorld::step` are function names rather than `Animator`
and `Physics` for a reason worth stating: the renderer already owns the GPU zones `Sprites`
and `Particles`, and a simulation zone spelled the same would have pooled with a pass in the
GPU table and been wrong there invisibly.

The scope goes **above the early-outs**, so a pass that records nothing costs a named zero
instead of disappearing — which is what lets the children be summed against
`Renderer::record` and a shortfall read as work no zone names. `substrate bench --zones`
prints that sum as its `total/frame` column. This is a rule about the *call site* as much as
the function: a system whose `update` is guarded by `if (!empty())` one frame further out has
a zone that vanishes rather than one that reports nothing, and those two look identical in
the table. `Engine::simulate` calls all four of its movers unconditionally, and each returns
early on its own.

A scope costs **0.1-0.2 us in release and 1.0-1.5 us in debug**: two `clock_gettime` calls, an
FNV fold, a thread-local set lookup and a spinlocked `push_back`. Twenty of them execute in
a default frame, which is 0.12% of it — cheap enough to be unconditional, and worth knowing
before wrapping something that runs per draw.

The **whole** profiler — every CPU scope, the GPU query pool and its readback, and the trace
flush every 120 frames — costs **0.023 ms a frame in debug**, 3% of the debug CPU frame.
That one is measured by running two frame counts and dividing the difference rather than
from a trace, because `--no-profiler` is precisely the arm that writes none.

**That last clause was untrue for as long as the figure existed, and correcting it did not
move the figure.** `--no-profiler` reached `Profiler` and never reached `GpuProfiler`, so the
control arm still created the query pool, still wrote two `vkCmdWriteTimestamp2` per pass and
still read them back — it only discarded the result. `Renderer::init` now skips
`GpuProfiler::init` when the profiler is off, which leaves the pool null and every
`GpuProfiler` entry point on its early-out. Re-measured across a 3,800-frame difference and
five repetitions: **3.5745 ms on against 3.5526 off, 0.022 ms a frame**, which is 0.023 to
the resolution this method has. The GPU query path is a per-pass `vkCmd*` into the driver and
a non-blocking readback, and it does not show up. Worth knowing rather than assuming, and
worth knowing that it was never checked before.

The same fix applies one layer down: `Profiler::init` gated its truncate, its signal
handlers and its writer thread on the output *path* rather than on `enabled`, so
`--no-profiler --trace <path>` left a zero-byte file and a running thread. It now creates
neither.

What the flag does **not** switch off is `GpuScope`'s debug-utils label. That decoupling is
deliberate — see the note on `GpuScope` — and it applies unchanged to a run that asked for no
profiler: a capture still names every pass.

## GPU zones

CPU scopes cannot see GPU cost, and for a deferred renderer the per-pass GPU number
is the one that decides everything.

```cpp
GpuScope zone(gpuProfiler, cmd, frameSlot, "Lighting");
```

Two `vkCmdWriteTimestamp2` calls bracket the zone. Results are read back several
frames later, once that frame's fence has signalled, so the CPU never blocks on the
GPU — then **back-dated into the frame they belong to**, which is still inside the
profiler's rolling window.

GPU and CPU clocks are uncorrelated, so a zone's offset *within* a frame is relative
to that frame's first GPU timestamp. Durations are exact; the placement is not.

## Output

Set `ProfilerConfig::outputFile` and a Chrome Tracing JSON is written by a background
thread, rewritten in full each flush so the file is always valid:

```cpp
ProfilerConfig cfg;
cfg.outputFile = "debug_frames/profile.json";
cfg.autoFlushFrames = 120;
cfg.maxFrames = 240;      // rolling window
Profiler::init(cfg);
```

Open it in [Perfetto](https://ui.perfetto.dev). Tracks are labelled: the trace carries Chrome
`ph:"M"` `thread_name` metadata, so `main`, `scene load`, `texture decode`, `physics job`,
`log writer`, `audio device` and `recorder encode` appear as names rather than as integers.
GPU zones are track 1000, labelled `GPU`, category `gpu`.

**A thread names itself, and the name belongs to the acquisition rather than to the track.**

```cpp
core::Profiler::nameThread("texture decode");   // a string literal, stored by pointer
```

Profiler slots are recycled — that recycling is what bounds the registry — so one track is
the scene-load worker early in a run and the audio device later. A name attached to the slot
would label the second thread's work with the first thread's name, which is worse than no
label: an unlabelled track is unhelpful, a track labelled from a stale owner is *wrong*. So
every acquisition emits its own metadata event, an acquisition that never names itself emits
`thread <n>`, and Perfetto takes the latest. A real trace shows the handover directly — track
3 in the demo is `texture decode` thirty-one times over the load and `audio device` after it.

`nameThread` compares pointers and returns immediately once a name is set, which is what
makes it safe to call from a device callback that runs at the mix rate — miniaudio's thread
is created by the driver, so there is no spawn site to name it at.

The trace holds three event phases now, so anything reading it has to say which it wants:
`ph:"X"` is a timed zone and carries `args.frame`; `ph:"M"` is metadata and carries neither a
duration nor a frame; `ph:"C"` is a counter and carries a value rather than a span.
`substrate bench` tables `X` and `C` separately and skips `M`.

## Counters

A zone can say a frame got slower and can never say why. That answer is usually a quantity:

```cpp
core::Profiler::counter("visibleInstances", visibleInstances);
```

Same contract as `scope` — a string literal held by pointer, allocation-free, dropped when the
profiler is disabled — with one difference: a counter is **last value per frame** rather than
a stack, because two writes of one name in a frame are a caller correcting itself rather than
two events. That is also what bounds the storage. A counter recorded with no frame open is
dropped, not buffered: its whole meaning is "this was true during frame N".

Eleven are recorded per frame today — `drawCalls`, `visibleInstances`, `visibleTriangles`,
`liveInstances`, `vramMiB`, `vramAllocations`, `bodies`, `particles`, `audioSources`, `nodes`
and `droppedSteps`. Perfetto renders them as track graphs above the zones they explain, and
`substrate bench --zones` prints them as a block beside the GPU and CPU ones.

**The `ts` is the frame's, not the clock's**, and that is the one thing about this easy to get
wrong. `writeTrace` emits no wall-clock time — it concatenates frames by `cumulativeUs` — so a
counter stamped from `steady_clock` at the call site would draw a graph that does not sit
above the zones it belongs to. The emit is inside `writeTrace` for that reason, which is why
`counter()` takes a value and never a timestamp.

`droppedSteps` is worth calling out as the shape of the argument: the engine logs dropped
simulation steps *once*, when the count changes, because a warning at 60 Hz drowns its own
log. The counter says on which frames — which is the question anyone reading a stutter
actually has.

The window is FIFO, but **frame 0 is pinned in both trim loops**, so a long run evicts
everything else and keeps startup. That was true before anything read it and is now what
`--startup` rests on.

## Startup

Frame 0 is the whole of `Engine::init` *and* `Game::init` — window, device, renderer, scene,
every subsystem, and the world a game builds. `substrate bench --startup` prints it:

```
substrate bench --config release --startup --samples 1 --runs 1
```

One column and no medians, because there is one startup per run; a count column beside it,
because there is not one of each *zone* in it — the acceleration structure is rebuilt every
time a game adds a mesh, and reporting the last of five as though it were the only one is
the trap that column exists to close.

**The frame opens at the top of `init` and closes after `Game::init` returns**, and the
boundary is load-bearing rather than tidy. A scope opened while the thread's stack is empty
records at **depth 0, as a sibling of `Frame`**, with no `Frame/` prefix for anything to key
on — so it is in the trace and attributable to nothing. That is where every zone in
`initWindow`, `initRenderer` and `Game::init` used to land, because the frame used to open
inside `loadScene`.

The one thing still at depth 0 is `GltfScene::decode` on the texture-decode workers, and
that is inherent rather than a boundary bug: a worker thread has its own scope stack and
`Frame` was never on it. Its parent `GltfScene::decodeAll` is on the main thread and does
carry the path.

Release and debug disagree by a lot on exactly the parts that are our code —
`createPipelines` 3.3 ms against 20.4, `GltfScene::geometry` 22 against 122 — and agree
almost exactly on `VulkanContext::init` and `Swapchain::create`, which are the driver's.
That split is most of what the mode is for.

`Ctrl-C` and `SIGTERM` flush the trace before exiting. The handler itself only sets an
atomic and sleeps; the writer thread does the work, so nothing async-signal-unsafe
happens in signal context.

## In-app

| Key | Action |
|---|---|
| `P` | Print the rolling-average scope table and last GPU timings |
| `1` `2` `4` `8` | Switch MSAA sample count live |
| `F1`–`F5` | Lit / albedo / normal / ORM / depth |

## Benchmarking

Fixed frame count, fixed sample count, a separate trace per run — and through the script,
which does all three and never launches the binary itself:

```bash
substrate bench                              # the MSAA sweep, 1/2/4/8
substrate bench --config debug --zones       # one config, every zone, GPU and CPU
```

Take **medians**, not means — the first frames after a pipeline rebuild are outliers — and
read them out of the trace rather than the `GPU @` log line, which is one arbitrary frame.
[architecture/tooling.md](../architecture/tooling.md) is the argument for both.

**A Debug CPU figure is not a Release one, and the difference is attributed rather than
guessed at.** Debug costs 0.61 ms of `CPU busy` a frame — three quarters of it the
validation layers, the rest `-O0` — and no frame time at all wherever the GPU frame is over
about 0.75 ms, which is every scene in the tree but one. Debug `wall` on Sponza equals
Release `wall` to 0.3%. See
[what Debug costs](../architecture/tooling.md#what-debug-costs-and-when-it-costs-nothing)
before reading a Debug number as a regression.

## Wall time is not CPU time

The frame reports three numbers and only one of them is about the CPU:

| Figure | What it is |
|---|---|
| `wall` | Frame-to-frame time. What FPS means. **Contains the GPU blocks.** |
| `cpu` | `wall` minus `Renderer::waitFence`, `acquire` and `present`. CPU work alone. |
| `gpu` | The `Frame` GPU zone. |

When you are GPU-bound, `wall` equals `gpu` — waiting for the GPU is what makes it so.
So a single figure labelled "CPU" that is really wall time will track the GPU frame no
matter what the CPU does, and the closer the two sit the more GPU-bound you are. Read
the gap between `cpu` and `wall` as CPU headroom.

`substrate bench` subtracts the same three zones for its `CPU busy` column, so the
HUD and the table can be checked against each other. In the trace itself the same
subtraction is `Frame` minus those three paths, per frame number.

## Overhead

The recording path does no heap allocation: names are literal pointers, scope paths
are folded into a 64-bit FNV-1a hash rather than concatenated strings, per-thread
buffers are reserved up front, and thread slots are pooled and recycled. The readable
path string is materialised once per unique path, on the cold side.

Per-thread buffers are guarded by an `atomic_flag` spinlock rather than a mutex:
exactly one thread pushes to a slot and only the frame collector drains it, so the
access is single-producer/single-consumer and effectively uncontended — but it does
still need synchronising, or a worker recording mid-frame races the collector.
