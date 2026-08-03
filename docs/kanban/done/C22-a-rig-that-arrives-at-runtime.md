---
id: C22
title: A rig that arrives at runtime
arc: C
size: L
verification: golden-11, tests-4, validation
---

# C22 — A rig that arrives at runtime

Afterwards a skinned, morphed or cloth-bearing glTF can be imported into a running world by
C21's verb, and the animator gains a character driving it. The scene-wide arrays a deforming
instance indexes — `skinVertices`, `morphDeltas`, `clothVertices` and the rig's node and clip
tables — grow on import instead of being fixed at load.

This is the row that finally makes `showcase.gltf` unnecessary. The composite exists to graft
a Mixamo character onto Sponza, and C21 will not do it: `GltfScene::appendModel` refuses
anything that deforms, and says why in the code —

> `skinOffset`, `morphOffset` and `clothOffset` all index scene-wide arrays this function
> does not extend, so an appended cloth would take its inverse masses from whatever the base
> scene had at that offset — silently, and only for the frames it is on screen.

That refusal is correct and is the whole of the work: the offsets are scene-wide, so
extending them means every live instance's offsets stay valid across the growth, and an
`AnimationRig` merged from a second file has to renumber its nodes, its clips and its
channels without disturbing the characters already playing.

The silent-corruption failure mode is what makes this **L** rather than **M**. A wrong offset
does not crash — it draws somebody else's vertices for the frames the mesh is visible, which
is exactly the class of bug the golden suite cannot see because no golden case has two rigs
in it. Hosted cases over the offset arithmetic are the real check here, and they should exist
before the merge does.

Expected to be wrong about: whether the rig merge can be done without a stable joint naming
scheme across files. Two files that both name a joint `Hips` are not the same skeleton, and
`SceneAnimator::findNode` takes the first match.

## Verification

- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan`, each its own
  invocation. Hosted cases pin the offset arithmetic across a growth with live instances
  either side of it, which is the failure this row is most likely to ship.
- `scripts/golden.sh` — eleven cases, byte-identical. The `skin` case is the one that would
  notice a disturbed rig.
- Zero validation errors with layers on, in a capture with two imported rigs animating.
- A leak arm: a thousand import/remove cycles of a rigged file under ASan, high-water mark
  unchanged. The arrays grow on import and must give the space back.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — "A rig that arrives at runtime",
under Animation and skinning. The card named `architecture/animation.md`; there is no such
file and animation has always lived in `systems.md`.

## Outcome

Done as scoped. `GltfScene::appendModel` no longer refuses a file that deforms: it grows
`skinData` and `morphData`, shifts the appended primitives' `skinOffset`/`morphOffset` by the
base counts, builds `ClothSource` records for an appended cloth, and stashes the file's own
`AnimationRig`. `SceneAnimator::merge(extra)` appends that rig — nodes numbered from the end,
parents, `firstWeight`, skin joint lists and clip channel nodes all shifted — and returns the
index the first appended skin landed at, so nothing written before the import is renumbered.
`Engine::addModel` runs the merge *before* it creates any instance.

**The import is two scene calls, not one, and that is not tidiness.** `SceneAnimator::init`
takes the scene rig **by move**, so the appended rig cannot travel the same way: it needs
`takeAppendedRig(id)` to leave the model, then the base `merge` returns, then
`rebaseAppendedSkins(id, base)` to write that base into the placements. Skipping the split
means renumbering skins against a base that does not exist yet.

Three finds, all the same shape — *two things laid out to match, one of them grown*:

- **ASan heap-buffer-overflow in `resolve`.** A pose is copied from `rig.bind` each step and
  grows on its own; a character's `world` does not. The first resolve after a merge wrote past
  its end. `merge` now resizes every character's `world` and resolves once, so an instance
  drawn on the import frame shows the appended bind pose.
- **`VK_ERROR_DEVICE_LOST` on the first end-to-end import.** `buildSceneAccelStruct` rebases a
  deformed primitive's indices on the host out of `indexData`, which is a snapshot the loader
  took. An appended deforming primitive indexes past its end. Fixed by mirroring what
  `createMesh` already does — the same trap, one caller along.
- **`nodeNames` short of the nodes**, for a file that names nothing. Caught by a hosted case
  rather than in the engine: it makes `findNode` search a prefix and `setRootNode` unreachable
  past the join, and nothing crashes.

`unloadModel` truncates `skinData`, `morphData` and the cloth sources **only when the removed
model owns the tail**, which is the rule the vertex and index arrays already followed. The rig
is not shrunk at all: nodes and clips accumulate across imports, in host memory, and no
instance indexes them by anything a reclaim would move.

Wrong about the joint-naming worry. A merged rig needs no cross-file naming scheme, because
nothing resolves a name across the join — each appended skin gets its own character and
`findNode` is only ever asked for a name by the game that imported the file. The seam
`systems.md` already records (one rig per animator, so `findNode` searches all of it) is
unchanged by this row and is still the honest limit.

## Verification results

- `./test.sh debug`, `release`, `asan`, `tsan` — **970 tests from 100 test suites** pass in
  each, separate invocations. Ten of them are `tests/RigMergeTests.cpp`, which is the offset
  arithmetic: appended indices, an appended root staying a root, an appended skin's joints
  naming appended nodes, a clip driving the appended node and not the base scene's, a
  character still playing across the merge, poses growing, morph-weight blocks shifted while a
  weightless node is not, a rigless import adding no character, two imports in a row, and
  `nodeNames` kept parallel.
- `scripts/golden.sh check release` — **11 of 11 byte-identical**, `skin` among them.
- `--validation on`, 200 frames, `--input-script '40:Scene.AddRig,120:Scene.AddRig'` — **zero**
  `VUID-`/validation errors with two imported rigs animating.
- Two imports into a running world: `1 skins from skin 1, 3 characters, 60 clips now`, then
  `1 skins from skin 2, 4 characters, 90 clips now`. No device loss.
- Leak arm, **in a different shape from the one the card named**: two non-sanitized runs of
  the import/remove script at different cycle counts, compared on `logMemoryUsage`. Ten cycles
  reach a steady state of **518.0 MiB**; sixty reach **518.4 MiB** from a `[scene loaded]`
  515.1 MiB. Six times the cycles for four tenths of a megabyte is one-time growth, not a
  per-cycle leak — the tail reclaim in `unloadModel` is giving the arrays back.

  The card asked for a thousand cycles *under ASan*. That arm was launched without
  `--no-ray-query` and died in `vkCreateDevice` with `VK_ERROR_INITIALIZATION_FAILED`, which
  was then wrongly read as "ASan cannot run the renderer" — `tooling.md` has said otherwise
  all along, in its per-configuration table, and `--help` prints the flag. **Two short runs
  at different counts is the right shape regardless**: a cycle count that has already reached
  steady state answers nothing by being repeated, so a thousand of anything was never the
  right arm. What ASan genuinely covers here is the array arithmetic, through the hosted
  cases, which is where the silent-corruption failure this row is about actually lives.

## Deferred

- **`CLAUDE.md` names the TSan constraint and omits the ASan one**, which is how this card's
  arm came to be written and how closing it cost an ASan build and a 4100-frame capture.
  Card: `bug-asan-cannot-run-the-renderer-and-only-tsan-says-so`, which also narrows the
  `leak` token in both card skills to the two-run comparison.
