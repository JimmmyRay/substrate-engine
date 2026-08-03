---
id: bug-the-follow-camera-moved-the-readback-cases
title: The follow camera moved the readback cases
arc: bug
size: S
verification: readback, golden-11, tests-hosted
---

# bug-the-follow-camera-moved-the-readback-cases — The follow camera moved the readback cases

Five of the nine readback cases — all four sprite cases and the lit silhouette — failed at
HEAD. `sprite` reported `26820/27648 texels differ, max delta 255 at (3,3), mean delta
22.1285`. Found by C19, which correctly reported it rather than absorbing it, and correctly
declined to treat it as its own.

## What was wrong

`Engine`'s `pixelPerfectCamera()` sets `cameraState.focus = glm::vec3(0.0f)` **once**, before
the frame loop, because for the whole life of the readback harness nothing else wrote the
camera. G13 then landed a third-person follow rig that writes `focus` every frame in
`DemoGame::frameUpdate`, snapping it to the character — which is what a follow camera is for,
and is gated on `world.built`, which is true in exactly the runs the readback harness makes.

So the sprite was placed against a camera aimed at the origin and rendered through one aimed
at the character.

**The five that kept passing are the tell.** `native-inside`, `native-outside`,
`scale3-inside`, `scale3-outside` and `letterbox` go through the overlay, which is screen
space and never sees the world camera. Every case that failed draws through the world camera:
the four sprite cases and the lit sprite. A regression that splits a suite exactly along a
structural line is describing its own cause.

## The fix

The frame loop re-asserts the pixel-perfect camera after `Game::frameUpdate` when a sprite or
lit-sprite readback is active. **Re-assert rather than forbid**: a readback case pinning the
camera and a game moving it are both correct, and the harness compares against an image
computed from the source file, so the projection it was computed for has to be the projection
that renders. Setting it once was sufficient only while nothing else wrote it.

## What this says about the checks

Three things, none of them comfortable:

- **G13 did not run the readback suite.** Its card names `golden-11, scripted-input,
  tests-hosted, validation` and it ran all four. The golden set passed because no golden
  scene sets a camera through this path. A row that changes the camera should run the suite
  whose subject is what the camera projects, and `verification:` did not say so.
- **C19 reported it and mis-attributed the scope.** It said the failure reproduced on
  unmodified HEAD in a detached worktree "with identical numbers to the digit". The numbers
  were identical because the cause was already committed; but a worktree built without the
  asset trees fails *more* cases than the main tree, so that environment cannot distinguish
  "pre-existing" from "mine" and the agreement was luck. Bisecting in a worktree needs the
  asset trees linked and a run that reproduces the main tree's pass set first.
- **Nothing else would have caught it.** No unit test can see a camera the renderer used, the
  golden set does not exercise this path, and validation layers report a correctly-drawn
  wrong picture as success.

## Verification

- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette and the
  resize soak. Five of these were failing before the fix.
- `scripts/golden.sh check release` — **11 of 11 byte-identical**, so re-asserting the camera
  changes nothing a golden case renders.
- `./test.sh debug` — **898 of 898**.

## Reference

[architecture/tooling.md](../../architecture/tooling.md) for the readback suite.

## Outcome

Fixed as above, one conditional in the frame loop. The lasting change is the note in
`tooling.md`: **a row that moves the camera runs the readback suite**, because the golden set
cannot see this and the readback set is the only thing that can.
