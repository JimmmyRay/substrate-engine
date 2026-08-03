---
id: bug-the-clips-moved-the-character-a-second-time
title: The clips moved the character a second time
arc: bug
size: M
verification: scripted-input, tests-hosted, golden-11, readback
---

# bug-the-clips-moved-the-character-a-second-time — The clips moved the character a second time

Reported from a running demo, immediately after
[[bug-half-the-character-never-turned]]: "the character is rubber-banding back to origin when
I stop walking", and then the diagnosis, which was correct — "root motion isn't being applied".

## What was wrong

The showcase clips are not authored in place. Decoded out of `showcase.gltf`:

| clip | `Hips` displacement | `mixamorig:Root` |
|---|---|---|
| `walking` | **+1.80 m** in Z | 0 |
| `running` | **+3.20 m** in Z | 0 |
| `right strafe` | **−2.88 m** in X | 0 |
| `idle` | ~0 | 0 |

Nothing in the tree called `SceneAnimator::setRootNode`, so the pose kept that translation.
The character was therefore moved twice — once by the controller, once by the clip — and the
drawn mesh slid out of its own capsule and snapped home the instant the machine blended to
`idle`, whose hips stand still. That snap is the reported rubber-band, and it is to the
*clip's* origin rather than the world's.

## Why the feature existed and had never run

`setRootNode` had been in the engine since C7, with six unit tests and **no caller**, and the
reason is a gap rather than an oversight: it takes a node index, a rig's joints belong to the
file, and nothing in the tree could turn a joint's name into an index. The six tests name a
synthetic rig's node 0, which is a number no real game can produce. So:

- `AnimationRig::nodeNames` keeps what the file called each node, and `SceneAnimator::findNode`
  turns one into an index. That is the whole of the engine change.
- `DemoGame` names `Hips`. Not the skeleton's first joint, which is `mixamorig:Root` and the
  one node in the rig with nothing on it — a rule of "hold the topmost joint" would have held
  the wrong node and measured a clean zero while nothing changed.
- The motion is taken out of the pose and **not re-applied**. The demo's controller owns travel
  — `moveSpeed` comes off the collider and `locomotion.sh` derives every distance from it — so
  handing `rootMotion()` back to `setCharacterInput` would be a design change, not a fix.

**The baked scene had to carry the names too**, and finding that out was the middle of this
row: the first working build printed nothing, because `showcase.gltf.scene` is the fast path
and `SceneCacheFormat` did not serialise the new field. A cache that dropped them would leave
root motion working from a document and silently off from every bake — the normal path. Fixed
with the field in `put`/`get`, `kSceneCacheVersion` bumped 4 → 5, and a round-trip assertion.

**The layout digest could not have caught it.** It folds `sizeof` over the PODs written
verbatim, and a `std::vector<std::string>` on a hand-serialised struct changes none of those
numbers — so this is precisely the case the "a payload changing shape bumps the version" rule
exists for, with the belt silent.

## The check, and it is a new kind rather than a ninth arm

Every number `locomotion.sh` asserts describes where the *solver* put the capsule. Not one of
them can see the pose, so all eight arms passed against a character visibly sliding and
snapping. `LocomotionTrace::poseDrift` is the missing kind: how far the pose carried the rig's
root, worst over the run, read out of `SceneAnimator::worldTransforms` rather than off the clip
— so it says the hold *took* rather than that the engine was asked for one.

Counterfactual, with only the `setRootNode` call removed:

| | `drift` |
|---|---|
| unheld (the bug) | **3.17 m** |
| held (the fix) | **0.00 m** |

Every other figure on both runs was identical to the digit — `8.21 m travelled, net 8.21,
along 1.00, across 0.00, facing 1.00`. That is the argument for the new number in one line.

It is asserted on all eight arms rather than the three that walk, because a clip dragging the
rig is not a property of anything being pressed.

**The measurement is resolved whether or not the hold is applied.** It was gated on
`rootNode()` at first, which made it vanish along with the bug it was meant to catch and report
a clean zero — a tautology of the same shape as the one
[[bug-half-the-character-never-turned]] had just been fixed for, written an hour later by the
same hand.

## Verification

- **scripted-input** — `scripts/locomotion.sh release`, 8 of 8, `drift 0.00` on every arm and
  every pre-existing figure unchanged. The claim rests on the counterfactual above.
- **tests-hosted** — `./test.sh debug`, 900 of 900, up from 898: `findNode` by name, a missing
  name leaving the hold off rather than holding node 0, and `nodeNames` surviving the cache
  round trip including an unnamed node.
- **golden-11** — 11 of 11, and they matter here because the cache version bump invalidates
  every sidecar: the eleven re-parsed their documents and produced the same bytes.
- **readback** — 9 of 9 plus the lit silhouette and the resize soak, for the same reason.
- Re-baked `showcase.gltf` and confirmed the hold still takes off the cached path
  (`cache=26.1ms parse=0.0ms`), which is the failure this row nearly shipped.

## Outcome

One engine capability made reachable, one game opting into it, one new measurement.

**The pose bobs less.** `setRootNode` pins all three components of the node's translation, so
the walk loses the vertical rise the hips carried with it. That is what "in place" means
everywhere and it is the right trade against a character teleporting 1.8 m on every release,
but it is a real change to how the walk reads and a future row may want the hold to be
horizontal-only. Recorded here rather than discovered later.

The theme of both of today's bugs is one thing: **the demo's locomotion suite measures the
solver and nothing else.** It reads `characterTransform`, `characterSpeed`, `characterOnGround`
and `characterJumped`, and it was right to — that is what made it able to fail a state machine
driven off a keypress. But a character is a capsule *and* a rig, and every check in the tree
was pointed at the capsule. Both bugs lived entirely on the rig side, both were plainly visible
on screen, and neither moved a single number in a suite of eight arms.
