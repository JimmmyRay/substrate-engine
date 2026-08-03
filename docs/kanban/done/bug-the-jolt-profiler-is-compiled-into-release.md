---
id: bug-the-jolt-profiler-is-compiled-into-release
title: The Jolt profiler is compiled into release
arc: bug
size: S
verification: trace, golden-11
---

# bug-the-jolt-profiler-is-compiled-into-release — The Jolt profiler is compiled into release

`build/release/build.ninja` carries `-DJPH_PROFILE_ENABLED`, so every `PhysicsSystem::Update`
in a shipped build runs Jolt's own instrumentation. This card turns it off in release and
re-baselines, leaving the root `CMakeLists.txt` saying what it wants from Jolt's two
profiling options rather than one.

It reads as an oversight rather than a decision. `external/JoltPhysics/Build/CMakeLists.txt`
defaults **both** neighbouring options to `ON`:

```
:85  option(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE "..." ON)
:92  option(PROFILER_IN_DEBUG_AND_RELEASE      "..." ON)
```

and the root `CMakeLists.txt:141` forces the first deliberately, with three lines of comment
saying why S4.5 needs the debug renderer in release. It says nothing about the second, and
never touches it — so the profiler is in release by inheritance, not by argument. The
contrast between the two lines is the whole finding: one was chosen and one was not.

What it costs is unmeasured, which is why the card is opened rather than the flag simply
flipped. Jolt's profiler is per-`Update` rather than per-body, and `simulate` is 0.411 ms in
the trace that produced
[bug-the-debug-frame-spends-seven-milliseconds-recording-commands](../done/bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md),
so the expectation is that this is small. Small and unintended is still worth removing from a
shipped build, and the number is worth having either way — if it turns out to be free, that is
a reason to say so beside line 141 instead of leaving the next reader with the same question.

**This is the one card in this group that changes release.** Everything else found in that
investigation was debug-only. Re-baselining is therefore part of the work, not a formality.

## Verification

- `set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)` beside line 141, then
  `grep JPH_PROFILE_ENABLED build/release/build.ninja` returns nothing after a clean
  configure.
- `scripts/baseline.py --config release --zones --runs 3` before and after, on
  `physics.gltf` as well as the default — the physics scene is where a change would show,
  and `simulate` is the zone to read. Several runs an arm: a run settles into one of two
  states about 5% apart.
- `scripts/golden.sh` — eleven cases, byte-identical. A profiler flag must not move a pixel,
  and if it does the determinism claim in
  [limitations.md](../../architecture/limitations.md) is what actually needs looking at.
- `./test.sh release` — the suite in the configuration being changed.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — if the measurement moves the
release baseline table, that table is regenerated rather than edited.

## Outcome

**The premise held, and it was a build change rather than a doc correction.**
`build/release/build.ninja` carried `-DJPH_PROFILE_ENABLED` 270 times, and so did `debug`
and `asan`; `tsan` carried it 262. `systems.md`'s "Jolt's build defaults" listed three
options and said nothing about this one, so there was no recorded decision anywhere for the
card to be wrong about — exactly the contrast the card described between line 141 and its
silent neighbour.

What the flag actually costs is narrower than "Jolt's own instrumentation runs". Nothing in
`engine/`, `game/`, `tests/` or `scripts/` calls `JPH_PROFILE_START`, so
`Profiler::sInstance` is null, `ProfileThread::sGetInstance()` is null on every thread, and
each of the 193 `JPH_PROFILE_FUNCTION` sites takes the `mSample = nullptr` branch in
`ProfileMeasurement`'s constructor. **The cost is a thread-local load and a branch per site,
plus a 32-byte `alignas(16)` stack sample per scope that is never written** — not the
`GetProcessorTickCount()` path, which never executes. So it was collecting nothing, at a
price, in every configuration.

`set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)` and
`set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)` now sit beside
`DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` with the argument for the opposite answer written
between them. Both are named because either one on its own is enough to define
`JPH_PROFILE_ENABLED`, which is how the first got in. It is off in **all four**
configurations rather than release only: the option is one switch, nothing reads what it
collects in Debug either, and a flag that differs by configuration is one more way the
golden set and the developer build can disagree. `grep -c JPH_PROFILE_ENABLED
build/<cfg>/build.ninja` is 0 for `debug`, `release`, `asan` and `tsan`, and a plain
reconfigure was enough — the `FORCE` took effect without wiping a build directory.

### The numbers

`simulate` is the zone, and **`scripts/baseline.py` cannot report it**: it tabulates
`cat == "gpu"` events plus wall and CPU-busy from the depth-0 `Frame` zone, so every
CPU-side zone in the trace is outside its table. The figures below come from the same
Chrome traces read the same way, over five runs an arm of 900 frames on `physics.gltf`,
release, `--locked --audio-null --msaa 4` — 1195 traced frames an arm. `simulate` also
contains `audioSources`, its one child, so it is reported with and without it; with no
animation and no particles in that scene, the remainder is the solver.

| Zone | before | after | delta |
|---|---|---|---|
| `simulate` | 0.0175 | 0.0157 | **-0.0018 ms, -10.3%** |
| `simulate` less `audioSources` | 0.0140 | 0.0123 | **-0.0017 ms, -12.1%** |

Per-run medians do not overlap — before `{0.0171, 0.0180, 0.0172, 0.0176, 0.0175}`, after
`{0.0160, 0.0160, 0.0148, 0.0148, 0.0160}` — across ten separate process launches, which is
what makes 1.7 microseconds worth quoting at all. **It is a real win and a small one**: the
zone is ~2% of a 0.75 ms frame on that scene, so this is ~0.23% of the frame. The card's
expectation that it would be small was right; its 0.411 ms figure for `simulate` was a Debug
number from another scene, and Release on `physics.gltf` is 0.0175.

**On the rendering side the result is a null, as it should be.**
`scripts/baseline.py --config release --zones --runs 3 --samples 4`, 717 frames an arm:

| Scene | zone | before | after |
|---|---|---|---|
| default | `Lighting` | 1.852 | 1.832 |
| default | `Frame` | 3.253 | 3.292 |
| `physics.gltf` | `Lighting` | 0.225 | 0.225 |
| `physics.gltf` | `Frame` | 0.683 | 0.685 |

Every one of those is inside run-to-run spread. **The release baseline table in
[tooling.md](../../architecture/tooling.md) is therefore not regenerated** — its 4x row
reads `Lighting` 1.850, `Frame` 3.202, `wall` 3.282, `CPU busy` 0.149, and both arms
measured here sit on top of it. A physics build flag that moved a GPU zone would have been
the finding.

### Verification

- `scripts/golden.sh check release` — **11 of 11**, byte-identical.
- `scripts/readback.sh release` — 9 of 9 bit-identical plus the lit silhouette and the
  resize soak. Not required by this card; run because the change touches every
  configuration.
- `./test.sh release`, `./test.sh debug`, `./test.sh asan`, `./test.sh tsan` — **805 tests,
  86 suites, passing in each**. TSan is the one that matters here: `Physics.cpp` is hosted,
  so the fixed step is inside the suite, and turning off a profiler that writes a
  thread-local is exactly the kind of change that would show there or nowhere.
- Validation layers, debug build, 240 frames, on `physics.gltf` and on the demo's own
  scene — **zero errors**, the only output being the standing `VK_LAYER_PATH hid the system
  layers` warning.

### Deferred

`baseline.py` reporting no CPU zone but `Frame` is a real gap and not this card's: a
CPU-side regression anywhere in `simulate`, `game`, `writeback` or `Engine::spatialIndex`
is invisible to the project's benchmark harness, and the reading has to be done by hand
against the trace each time — which is what this card had to do.

**It also falsifies a premise in
[chore-one-profiler-zone-covers-every-pass-the-cpu-records](../backlog/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md).**
That card states "there is nothing to build on the tooling side: `scripts/baseline.py
--zones` already prints every zone the trace contains, so new scopes appear in the table the
run after they are added", and both of its verification steps rest on it. It does not:
`read_trace` files an event into the zone table only when `e["cat"] == "gpu"`, and every CPU
scope lands in `wall`/`CPU busy` or nowhere. Adding twenty-five `record/<Pass>` scopes to
`Renderer.cpp` would produce a table exactly as empty of them as today's. That card needs
`baseline.py` taught to report CPU zones before its verification can pass — noted here
rather than edited into it, because it is not this card's tree to change.
