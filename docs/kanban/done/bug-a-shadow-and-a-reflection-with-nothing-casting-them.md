---
id: bug-a-shadow-and-a-reflection-with-nothing-casting-them
title: A shadow and a reflection with nothing casting them
arc: bug
size: S
verification: golden-11, inspection
---

# bug-a-shadow-and-a-reflection-with-nothing-casting-them — A shadow and a reflection with nothing casting them

An object occludes a traced shadow ray and appears in the mirror, and is not drawn in the
G-buffer. On the floor it reads as a hard black ellipse with nothing above it; in the mirror
sphere it reads as a dark shape the world does not contain.

Reproduced in the demo (the scale is hardcoded now; the flag the first version of this
line named is gone), on the floor to the left of the character:

```
./run.sh demo -- --headless --locked --audio-null --physics-debug --frames 200 \
    --capture out.png --capture-frame 180 --camera -3.8,0.5,0.5,86,-6,3
```

**Present in the TLAS, absent from the raster pass** is the whole shape of it, and it is the
mirror image of the defect the rigid acceleration-structure tier was opened for: there the
raster geometry moved and the traced copy stayed behind, here something is traced that is
never drawn. Both are one question — whether an instance's raster transform and its TLAS
instance agree — so the two are worth reading together even though this one is not a
regression of that fix. It reproduces with the tier working correctly.

**Root cause: an instance is baked into the static tier before it is told what it is.**
`Renderer::setInstances` builds the acceleration structure synchronously, and it is the call
a game makes when it has finished creating instances -- which is *before* the first
`Scene::update` writes node transforms into them, and before `initPhysics` sets
`kInstanceDynamic` on everything it gives a body to. Whatever the structure baked at that
moment is what the traced copy keeps.

Two ways it showed:

- The demo built each brazier's stem, bowl and coals with `placeCopy(..., glm::mat4(1.0f))`
  and gave them their place by attaching them to a node. Twelve unit cylinders were baked at
  the world origin; the raster followed the nodes and the traced copies did not. That is the
  hard black ellipse -- a unit cylinder's shadow, about a metre across, on the floor beside
  the character with nothing above it.
- `physics.gltf` is worse and had gone unnoticed: **15 static geometries, 0 moved** at load,
  because the twelve boxes are flagged dynamic only after `setInstances`. Every falling box
  traced from its spawn for the whole run, and the sphere cast no shadow at all.

Confirmed rather than argued. The defect reproduces on the demo world as it stood before the
braziers lost their geometry, is removed by `--no-rt` and by `--no-rt-shadows` (so it is a
traced shadow, not a raster one), and survives placing every prop on the floor instead of a
metre under it -- which eliminates the burial the scale pass was carrying at the time, a
hypothesis that looked strong and was wrong.

Candidates eliminated on the way, all three of the ones this card opened with:


- An instance destroyed after the structure was built, leaving its TLAS instance live. The
  structure is rebuilt by `setInstances`, and a game that destroys without re-handing the
  table would leave exactly this.
- An instance culled or LOD'd out of the draw list while still occupying a TLAS instance --
  the raster path has frustum culling and an LOD chain and the traced path has neither.
- A hit record resolving to the wrong slot. `instanceCustomIndex + geometryIndex` indexes
  `hitRecords`, and a wrong index shades a real hit as the wrong surface rather than
  inventing one, so this is the least likely of the three and the easiest to check.

Do not assume it is the scale. It was found at 2x, but the scale pass was also carrying an
assembly bug at the time and that one is now fixed; the ellipse survives the fix -- and it
survives un-burying the props too, which is the stronger of the two checks.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. No golden case reproduces this, which
  is part of the finding: the suite has no case with a mirror and a moving world in it.
- `inspection` — the honest token here. The fix is confirmed by the ellipse being gone from
  the capture above and by the caster being accounted for, not by a test that did not exist
  before and will not exist after.
- A frame capture through the `gpu-frame-inspection` skill is the tool: the TLAS instance
  count against the draw list is the comparison that names the culprit.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — only if the cause turns out
to be a rule about what enters the structure, rather than a plain bookkeeping slip.

## Outcome

Fixed. `SceneAccelStruct` now keeps, host-side, which instance slot each static geometry was
baked from and the transform that was baked; `staticTierStale` asks whether any of them has
since moved or been told it is dynamic, and `Engine` asks that once per frame, straight after
the scene sweep -- the first thing that can move an instance a game created and attached to a
node. When it fires the structure is rebuilt and the reason is logged once, because a game
that trips it every frame is a game missing `InstanceDesc::dynamic` and should be told so
rather than quietly charged for a rebuild.

The comparison carries a 1e-4 tolerance and that is load-bearing: a node writes its instance
through a `compose(decompose(m))` round trip, so an exact test would call every scene stale on
its first sweep and rebuild for nothing. With the tolerance, the demo's own scene rebuilds
zero times and `physics.gltf` rebuilds once.

**This moves a golden case, deliberately, and the baseline has not been re-snapped.** Ten of
the eleven are byte-identical. `physics` differs because it is the case carrying the defect:
before, its acceleration structure was `15 static geometries + 0 moved` and every falling box
traced from its spawn; after, it is `3 static + 12 moved over 3 shared structures` and the
boxes trace from where they are. The evidence that the new frame is the correct one is that
the sphere now has a shadow under it and had none before, and that a hard box-shaped shadow
out on empty floor at the left is gone. `--no-rt-shadows` was tried as a referee and is not
one here: the raster path draws almost no shadow at all in this scene, so agreement with it
measures brightness rather than correctness.

**The rebuild has to stall the device, and that was found the hard way.** `rebuildAccelIfStale`
is the only caller of `buildAccelerationStructures` that runs inside the frame loop, and that
function opens with `destroySceneAccelStruct`. With `kFramesInFlight = 2`, up to two submitted
command buffers still hold the TLAS, the BLASes and the hit-record buffer it frees, so the
first version of this fix was a GPU use-after-free that surfaced as one golden case differing
on one run in eight. It takes `vkDeviceWaitIdle` first. The stall is affordable precisely
because the warning beside it says this path should fire once at load or not at all.

`physics.png` has been re-snapped and all eleven cases match again. The baselines under
`debug_frames/` are gitignored -- they are a local before/after instrument, not a committed
authority -- so this rewrote nothing anybody else holds.
