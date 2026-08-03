---
id: bug-a-clip-held-at-its-end-reads-as-its-beginning
title: A clip held at its end reads as its beginning
arc: bug
size: S
verification: tests-4, golden-11
---

# bug-a-clip-held-at-its-end-reads-as-its-beginning — A clip held at its end reads as its beginning

`sampleClip` wrapped its time with `fmod`, and `fmod(d, d)` is zero -- so a clip sampled at
exactly its duration answered with its **first** keyframe. A `ClampToEnd` playback sits on
exactly the duration for every frame it holds its last pose, so the held pose was the wrong
one, for one frame, at the end of every clip that ends.

On the showcase rig that is the jump. `jumping up` is 0.25 s and runs the hips from 0.969 to
0.939; on the frame the clip reached its end the hips snapped back to 0.9691 and the blend
into `falling idle` then re-ran the same descent. Measured, per fixed step, at the moment
the state machine leaves `jump`:

```
step=76 hips=0.9407   step=77 hips=0.9390   before: 0.9407 -> 0.9691 -> 0.9668
step=78 hips=0.9392   step=79 hips=0.9394   after:  0.9407 -> 0.9390 -> 0.9392
```

A 2.8 cm vertical pop on one frame, which is what "the jump animation has a snap in it" was.

The fix is `>` rather than `>=`: wrap only a time strictly past the duration. A looping clip
cannot tell the difference, because `advance` has already brought it into `[0, duration)`
before `sampleClip` ever sees it -- so the only playback that arrives here holding the
duration is a clamped one, and it means the end.

**This was not a regression of the root-motion change**, though that change is what made it
visible. Holding the root's Y at the bind translation pinned the vertical channel flat, so a
one-frame error in it could not move anything; letting the clip own Y (C7's fix for the
character standing eight centimetres off the floor) is what let the sampler's error reach
the screen.

## Verification

- `./test.sh` in four configurations -- 922 tests. `SampleClip.ExactlyTheDurationIsTheEndOf
  TheClipAndNotTheStartOfIt` is the new one, and it asserts both halves: the end pose at the
  duration, and the wrap still working a whole loop past it.
- `scripts/golden.sh` -- eleven cases, byte-identical. Bisected deliberately: with this fix
  in and the acceleration-structure change of
  [[bug-a-shadow-and-a-reflection-with-nothing-casting-them]] backed out, all eleven pass,
  which is what says this fix moves no pixel and that card's does.

## Reference update

None. The wrap is a two-line rule inside one function and `architecture/` does not describe
it; the trap now lives in the comment beside it, where a later edit would meet it.

## Outcome

Fixed in `engine/scene/Animation.cpp`. The measurement above is the whole of the evidence:
the discontinuity is gone and the hips run smoothly out of the jump into the fall.

What this row did **not** settle is the second half of the report that came with it -- that
the player still hovers. Measured against the debug capsule as a ruler, the character stands
2.3 cm proud of Sponza's floor, and the cause is content rather than code: `showcase.gltf`
authors its own `showcase_ground` box whose top is at y = 0, while Sponza's floor is at
-0.023 at `sceneScale` 2. The character stands on the invisible box. That is a separate
finding and wants its own card; 2.3 cm is also small enough that it may not be what was
being reported, and nothing here should be read as having answered it.
