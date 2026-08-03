---
id: chore-a-thread-in-the-trace-has-no-name
title: A thread in the trace has no name
arc: chore
size: S
verification: trace, tests-hosted, tests-4
---

# chore-a-thread-in-the-trace-has-no-name — A thread in the trace has no name

Afterwards the trace carries Chrome `ph:"M"` `thread_name` metadata, so Perfetto labels its
tracks with what the thread is instead of an integer, and a slot handed on to a second thread
does not silently inherit the first one's name.

Every CPU event carries `"tid": <slot>` and nothing says what the slot is. There are six
threads that can record into the profiler:

| Thread | Created at |
|---|---|
| async scene parse worker | [SceneLoader.cpp:26](../../../engine/scene/SceneLoader.cpp#L26) |
| the `hardware_concurrency` texture-decode fan-out | [GltfScene.cpp:270](../../../engine/scene/GltfScene.cpp#L270) |
| Jolt's job threads | [Physics.cpp:352](../../../engine/scene/Physics.cpp#L352) |
| the video recorder's encoder | [Recorder.cpp:145](../../../engine/core/Recorder.cpp#L145) |
| the logger's writer | [Logger.cpp:198](../../../engine/core/Logger.cpp#L198) |
| miniaudio's device callback | [Audio.cpp:194](../../../engine/scene/Audio.cpp#L194) |

**The reuse is the sharp end, not the missing label.** `acquireSlot`
([Profiler.cpp:180](../../../engine/core/Profiler.cpp#L180)) walks the registry for a slot
whose `inUse` is false and hands it to the calling thread. That recycling is deliberate and
correct — it is what bounds the registry — but it means one `tid` is the scene-load worker
early in a run and the recorder later, with nothing in the trace marking the handover. A
reader who labels a track from what it did first will read the second thread's work as the
first thread's. An unlabelled track is unhelpful; a track labelled from a stale owner is
wrong, and this card must not produce the second while fixing the first.

So the name is recorded **per acquisition**, not per slot: a `thread_name` metadata event
emitted when a thread takes a slot, which is also the only moment the name is knowable. A
slot acquired twice emits twice, and Perfetto takes the latest — which is the honest
rendering of what actually happened.

This has not bitten yet, and the card says so plainly: `GltfScene::decodeAll` is the only
zone in the tree that spans workers, so today almost everything recorded is on the main
thread. It is worth doing before that changes rather than after, and
[chore-what-the-frame-pays-outside-record](chore-what-the-frame-pays-outside-record.md) plus
any async streaming work will change it.

**Windows is a known asymmetry, not a defect this card introduces.** MinGW does not run
`thread_local` destructors, so profiler slots are never released there — recorded already in
[architecture/limitations.md](../../architecture/limitations.md). The practical effect is
that reuse cannot happen on Windows and every thread gets a fresh slot, so the naming is
correct there for a different reason. Worth a line, not worth a workaround.

## Verification

- A trace from a run that loads a scene asynchronously, opened in Perfetto: the decode
  workers and the scene-load worker are labelled, and the main thread is labelled as such.
- A run that starts recording after an async load has finished, so a slot is genuinely
  recycled — the track shows the later name over the later events. This is the case the card
  exists for and it has to be constructed deliberately; it will not occur by accident.
- `./test.sh` in all four configurations, with a case in `tests/ProfilerTests.cpp` that
  acquires a slot on one thread, joins, acquires the same slot on a second, and asserts two
  metadata events rather than one. **`tsan` in particular** — this writes to the registry
  from every thread that profiles, and the registry mutex is what it is checking.
- The emitted JSON still parses with the metadata events interleaved.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — "CPU threads appear as tracks 1..N" is the
sentence this replaces.

[architecture/tooling.md](../../architecture/tooling.md) — the trace format description, which
lists the event kinds emitted.

## Outcome

`core::Profiler::nameThread(const char* literalName)`, `ph:"M"` `thread_name` events in the
trace, and seven call sites. The recycling case the card was written for is visible in a real
trace on the first attempt — the demo, one async `Scene.Stream` at frame 40:

| tid | names, in order |
|---|---|
| 1 | `thread 1`, `log writer` |
| 2 | `thread 2`, `main` |
| 3 | `thread 3`, `texture decode` — **thirty-one times** — then `thread 3`, `audio device` |
| 4 | `thread 4`, `scene load` |
| 1000 | `GPU` |

Track 3 is the whole argument: it was the texture-decode fan-out for the duration of the
load and is miniaudio's device thread afterwards. Labelled from what it did first, every
audio-device event on it would have read as texture decoding. A second run with
`--set physics.workerThreads=4` shows the same handover again — tid 4 goes `texture decode`
then `physics job` — plus `physics job` on three fresh tracks.

**Three things the card did not anticipate, and each was found by verifying rather than by
reasoning.**

*The default label has to be pushed by `acquireSlot`, not left implicit.* A thread that never
names itself would otherwise render under whatever the previous owner called the slot —
which is precisely the failure the card is about, arriving through the back door. Every claim
now pushes a `nullptr` entry that writes as `thread <n>`, and
`ProfilerTest.ARecycledTrackIsRenamedRatherThanInheriting` asserts exactly that: the track
the anonymous thread ended on is labelled `thread N` and not `second`.

*`nameThread` has to be idempotent, and on the pointer.* miniaudio's device thread is created
by the driver, so there is no spawn site to name it at — the only code of ours that runs on
it is the `onProcess` callback, at the mix rate. Comparing `const char*` and returning makes
naming from inside a callback free.

*A second `Profiler::init` in one process left every track unlabelled.* The registry
deliberately outlives `shutdown()`, so the main thread's slot came back already named `main`
and `nameThread` correctly declined to re-emit — into a *new trace* that therefore had no
metadata at all. Caught by the new test failing in the suite while passing alone, which is
the only way this would ever have shown up. `init` now re-announces every live acquisition.
This is a real defect for `Engine` too, not only for tests: a tool that re-inits the profiler
for a second trace would have written an unlabelled one.

**Two changes beyond the card's scope, both one line and both the same complaint.** Track
1000 is not a thread and never acquires a slot, so nothing would ever have named it — an
unlabelled `1000` beside labelled CPU tracks is the card's own objection one row down; it is
`GPU` now. And Jolt's pool is default-constructed and `Init`ed separately, because
`SetThreadInitFunction` must be set before the threads start and the convenience constructor
starts them.

**One reader had to change or the trace became unreadable**, which is the cost of adding a
phase. `scripts/baseline.py` did `e["args"]["frame"]` for every event and would have thrown
on the first metadata one; `tests/ProfilerTests.cpp` had two assertions with the same shape,
including one that asserted `doc[0]` is a timed zone — it is a `thread_name` now. Anything
else that reads a trace has to filter on `ph == "X"`.

**`recorder encode` is the one label not demonstrated in a trace.** The recorder refuses to
start in a headless run — *"there is nothing to record in a headless run"* — and every run
this verification can make is headless. The call is the same one line at the top of
`Recorder::encodeLoop` as the other five.

930 tests in each of debug, release, asan and tsan — **tsan in particular**, since this adds
a second per-frame record to the thread slots and writes to the registry from every thread
that profiles. `scripts/golden.sh check release`, eleven of eleven byte-identical.
`scripts/baseline.py --config release --zones` still reads a trace that now has metadata in
it.
