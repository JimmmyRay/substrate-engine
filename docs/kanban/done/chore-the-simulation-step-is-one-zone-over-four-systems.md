---
id: chore-the-simulation-step-is-one-zone-over-four-systems
title: The simulation step is one zone over four systems
arc: chore
size: S-M
verification: trace, tests-4, golden-11, validation
---

# chore-the-simulation-step-is-one-zone-over-four-systems — The simulation step is one zone over four systems

Afterwards the trace holds `simulate/SceneAnimator::update`, `simulate/PhysicsWorld::step`,
`simulate/SpriteTable::update` and `simulate/ParticleSystem::update`, and
`writeback` has the scene-graph flush broken out of it. This is
[chore-one-profiler-zone-covers-every-pass-the-cpu-records](../done/chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
applied to the other half of the frame.

`Engine::simulate` is one `Profiler::scope` wrapped around every mover the engine has.
Inside it, with no zone of their own, run
[`SceneAnimator::update`](../../../engine/scene/Animation.cpp#L607) — state machines, clip
sampling, fades, world transforms and joint matrices —
[`SpriteTable::update`](../../../engine/scene/SpriteTable.cpp#L498),
[`ParticleSystem::update`](../../../engine/scene/ParticleSystem.cpp#L183), and
[`PhysicsWorld::step`](../../../engine/scene/Physics.cpp#L989), which is itself `reclaim`, a
per-character `ExtendedUpdate` loop, Jolt's `system.Update`, contact collection and the
interpolation snapshot. `Animation.cpp`, `Physics.cpp`, `SpriteTable.cpp` and
`ParticleSystem.cpp` contain **zero profiler zones between them**.

So an animation regression, a physics regression and a particle regression are today the
same number, and the only way to tell them apart is the one the sibling card had to use
before it landed: add temporary scopes, run, revert. That is the cost this removes.

`endFrame`'s `writeback` has the same shape at smaller scale — a walk over every placement,
an emitter loop, and [`Scene::update`](../../../engine/scene/Scene.cpp#L339), the whole
scene-graph flush to instances, lights, audio, physics and particles. `Scene::update` is the
one that scales with a real project's node count and it is not separable today.

**The names follow the rule the sibling card set**, and it decides them here rather than
leaving twenty call sites to guess: a step with a `GpuScope` takes that zone's name spelled
identically, a step without one takes **its own function's name**. None of these four has a
GPU zone, so all four are function names — which is also what keeps them from colliding with
the renderer's existing `Sprites` and `Particles` GPU zones, a collision that would have been
invisible in the GPU table and wrong in it.

**The scope goes above the early-outs**, not below them. `PhysicsWorld::step` returns early
on `empty()` and `simulate` returns early when the audio engine is inactive; a system that
decides to do nothing must still cost a named zero, or a gap between `simulate` and the sum
of its children cannot be read as work no zone names.

Roughly eight scopes. The measured cost is 0.1-0.2 us each in release and 1.0-1.5 us in
debug, so this is under a hundredth of a millisecond and needs no gate — but the before/after
on `simulate` is still taken, because that is the arm that proves it.

## Verification

- `scripts/baseline.py --config debug --zones` and `--config release --zones` — the new
  `simulate/*` and `writeback/*` rows appear **with numbers**, and their `total/frame` sum
  accounts for `simulate` to within the profiler's own overhead. A scene with no animator, no
  sprites and no particles must still show all four rows, at 0.000, which is the check that
  the scopes sit above the early-outs.
- A before arm on `simulate` and `writeback` in both configurations. If eight scopes are a
  measurable share of the step, that is the finding and the count comes down.
- `scripts/golden.sh check release` — eleven cases, byte-identical. A CPU scope cannot move a
  pixel, so this is a null check, and a failure means something other than a scope was added.
- `./test.sh` in all four configurations. `tsan` is the one that matters: `Profiler.cpp` is a
  hosted translation unit and the unit suite is the only place its threading is exercised.
- Zero validation errors with layers on.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — the "What is instrumented" section names
the frame's spine and every pass `Renderer::record` calls. It gains the simulation step.

## Outcome

Seven scopes, not eight, and the count is lower because two of the eight already existed and
turned out to be wrong rather than missing. `SceneAnimator::update`, `SpriteTable::update`,
`ParticleSystem::update`, `PhysicsWorld::step`, `Scene::update` and `AudioEngine::update` are
new; `audioSources` and `audioOcclusion` were re-braced.

**The demo, debug, 239 frames, ms per frame:**

| Zone | before | after |
|---|---|---|
| `simulate` | 1.0935 | 1.1511 |
| `simulate/SceneAnimator::update` | — | **0.6541** |
| `simulate/AudioEngine::update` | — | 0.3688 |
| `simulate/PhysicsWorld::step` | — | 0.0872 |
| `simulate/ParticleSystem::update` | — | 0.0161 |
| `simulate/audioOcclusion` | — | 0.0173 |
| `simulate/audioSources` | 0.3725 | **0.0025** |
| `simulate/SpriteTable::update` | — | 0.0001 |
| `writeback` | 0.0866 | 0.0931 |
| `writeback/Scene::update` | — | 0.0896 |

The animator is 57% of the step, which is the number the card existed to produce and which
nothing in the trace could previously say.

**The card was wrong about `audioSources`, and finding out cost nothing.** It was not a
missing zone; it was a *function-lifetime* `Profiler::scope` opened above two more statements,
so it enclosed `audioOcclusion` and the mixer update and reported all three under one name.
The before-arm trace shows the nesting outright — `Frame/simulate/audioSources/audio` and
`Frame/simulate/audioSources/audioOcclusion` — and 0.3725 of that 0.3725 was the mixer.
Bracing it moved the number to 0.0025. **A scope not brace-scoped to the thing it names is a
distinct defect from a missing scope and does more damage**, because it produces a number
rather than a gap, and nobody audits a number that exists.

`AudioEngine::update`'s zone was likewise present, named `audio`, and sat *below* its
`!impl->running` return — the one zone in `simulate` that disappeared rather than reporting
zero. It is above the return now and named for its function.

**`Engine::simulate` no longer guards three of its four movers.** The `if (!empty())` tests at
the call sites made the zones vanish on a scene without an animator or particles, which is the
failure the "above the early-outs" rule exists to prevent — the rule is about the *call site*
as much as the function. Each mover already returned early on its own, so the guards were
duplicating a test one frame further out, and removing them changed the work done by nothing:
`ParticleSystem::empty()` is `emitterList.empty()` and `poolCapacity` is sized from that same
list, so the internal `poolCapacity == 0` return was already the same test. The audio early
return went with them; `AudioEngine::update` returns on `!impl->running` by itself, so a run
with no device costs a named zero, and a running engine with no sources now advances its bus
fades, which it always should have. Sponza confirms it: **all seven rows present, six at
0.000.**

**The seven scopes are not a measurable share, and the arithmetic says so twice.** The
apparent `simulate` rise of 0.058 ms sits inside the run-to-run spread — four repeats of the
after arm gave 1.1003, 1.1233, 1.1628, 1.1789, and the before arm's 1.0935 is at the bottom of
that range rather than outside it. The honest measurement is the *gap* between `simulate` and
the sum of its children, which is stable across every run and is exactly the scope cost:

| | gap, ms/frame | per scope |
|---|---|---|
| debug | 0.0045-0.0049 | ~0.67 us |
| release | 0.0010-0.0011 | ~0.15 us |

Release's 0.0010 is 0.8% of `simulate`. Both agree with the 0.1-0.2 us and 1.0-1.5 us the
guide quotes, and this is a better arm for that figure than the one the guide used, because
seven scopes' worth of gap is measured against the parent that contains them rather than
against a second run.

**One measurement the card asked for was not taken, and it is worth saying which.** A release
*before* arm needs the pre-change binary, and getting one means either `git stash` — forbidden
here, since sessions share the checkout — or a second worktree with its own fetched asset
tree, which is a fetch and a full build for a number debug already bounds at ten times the
per-scope cost. The debug before/after is exact and is the pessimal case.

`scripts/golden.sh check release`, eleven of eleven byte-identical. 929 tests in each of
debug, release, asan and tsan. Zero validation errors with layers on.
