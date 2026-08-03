---
id: bug-half-the-character-never-turned
title: Half the character never turned
arc: bug
size: S
verification: scripted-input, golden-11, inspection
---

# bug-half-the-character-never-turned — Half the character never turned

Reported from a running demo: "the submesh is facing/moving in a different direction of the
character." A screenshot showed the mannequin's body swung one way and its joint caps
another, the two halves of one character pointing apart.

## What was wrong

`DemoGame::driveLocomotion` resolved the node to turn by walking the player node's children
and taking the **first** one named `mesh`:

```cpp
if (e.scene().name(c) == "mesh") { facingNode = c; break; }
```

`Engine::bindPhysicsToScene` creates one child per *placement* bound to the body and names
every one of them `mesh`. A rig is routinely several meshes over one skeleton and one
capsule — the showcase character is `Beta_Surface` and `Beta_Joints`, two skinned meshes on
nodes 75 and 76 under one collider. The loop turned the head of that list and left the rest
at whatever the file authored.

Measured mid-strafe, before the fix:

```
DIAG mesh#1 at -1.32 0.00 0.90  +Z -1.00 0.00 0.00     <- turned to its heading
DIAG mesh#2 at -1.32 0.00 0.90  +Z  0.00 0.00 1.00     <- still facing the authored axis
```

`spawnExtraCharacters` already knew this — its comment says "the Mixamo rig is a body and a
separate set of joint caps... matching on the node copies one of them" — and it gathers every
part. The turn did not.

## The check that could not fail

`locomotion.sh` asserts `facing` between 0.85 and 1.01 on three arms, and its comment claims
the number is trustworthy because it is "read off the scene node rather than off the angle
the game wrote". That was true and insufficient. It read back **the one node the same
function had just written**, so it could not fall while that node turned, whatever the rest
of the character did. Eight arms passed with the character visibly split.

`alongFacing` now takes the **minimum** over every part, so one piece left behind drags the
ratio down. Counterfactual, run with the one-node rotation restored:

| | `facing` |
|---|---|
| one node turned (the bug) | **-0.98** |
| every node turned (the fix) | **+0.96** |

The arms did not need changing — the number did. -0.98 fails the existing bound by a mile.

## What was not wrong

The report also said neither was "moving in the wasd direction from the camera as expected",
and that half did not reproduce. The input basis is correct and was measured three ways:

- `along 0.98–1.00, across 0.00–0.17` on `camera-north`, `camera-south` and `camera-turning`.
- At the reporter's own camera (`yaw 57.9`), strafing measures `along 0.00 across 1.00` —
  exactly perpendicular to the camera's forward.
- The sign, which no arm covers: `D` at yaw 0 carries the character toward world **-X**, and
  `cross(forward, up)` at that yaw *is* `-X`, which is screen-right under `glm::lookAt`'s
  right-handed basis. Confirmed independently by the on-screen centroid of a strafe capture.

What the reporter was seeing was the split character: a body turning to its heading beside
joint caps locked to a fixed world axis reads as a character not going where it points.

## Verification

- **scripted-input** — `scripts/locomotion.sh release`, 8 of 8 arms, with the same numbers as
  before the change: after the fix the parts agree, so the minimum is the value the old
  single-node read returned. The claim rests on the counterfactual above rather than on the
  suite passing, which it did while broken.
- **golden-11** — `scripts/golden.sh check release`, 11 of 11. The eleven run engine scenes,
  where `world.built` is false and none of this executes; run because a change to the demo
  binary is a change to the binary every case launches.
- **inspection** — the two-mesh layout read out of `showcase.gltf` (nodes 75 and 76, one skin,
  one collider) and confirmed at runtime by a temporary per-part dump, since removed.

## Outcome

Two lines of behaviour and one measurement. The fix is a loop where there was a `break`; the
work was proving which of the two reported symptoms was real.

The lesson is the one this tree keeps re-learning, in its narrowest form yet: **a check that
reads back the state the same function just wrote is not a measurement.** `alongFacing` went
to the scene tree rather than to `facingYaw` — the right instinct, and the card that wrote it
said so proudly — but it went to the *one node it had written*, which is the same tautology
one level out. The suite that owns this behaviour ran 8 arms and reported full agreement
against a character that was visibly in two pieces.

It also cost a wrong first suspect. The report named the movement direction, so the movement
direction is where the investigation started: the camera basis, the strafe sign, root motion,
frustum culling and mesh LOD were each measured and each exonerated before the mesh list was
looked at. That order was not wasted — the "not wrong" section above is now on the record —
but the visible symptom named the wrong subsystem, and the split character explained both
halves of the report at once.

## Two things found on the way, neither fixed here

- **`kRestFacing` does not exist.** `driveLocomotion` cites it by name — "the axis
  `kRestFacing` says the rig looks down" — and no such constant is in the tree. The axis is
  correct (`+Z`, verified by rendering the rig from both sides: the yaw-0 camera sees its
  back), but the comment names a thing that was never written.
- **No arm presses `A` or `D`.** `acrossCamera` is absolute-valued and every arm walks
  forward, so a strafe mirrored left-for-right would pass all eight. It is right today and
  nothing in the suite says so.
