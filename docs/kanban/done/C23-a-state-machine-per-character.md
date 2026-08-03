---
id: C23
title: A state machine per character
arc: C
size: M
verification: tests-4, golden-11
---

# C23 — A state machine per character

Afterwards `SceneAnimator::Character` owns the two things the animator holds one of today —
its state machine and its root-motion node — so two rigs in one scene can animate off
different clips, different parameters and different root joints. `setStateMachine` and
`setRootNode` gain a character to apply to, keeping an all-characters overload for the
single-rig case that is every scene in the tree right now.

## What is wrong today

`SceneAnimator` stores one of each for the whole animator:

```cpp
AnimationStateMachine machine;                  // Animation.h
uint32_t rootMotionNode = scene::kNoNode;
```

Every character shares them. A scene with a player and a guard cannot give the guard its own
clips; a rig that names its pelvis something other than `Hips` silently gets no root-motion
hold, because the name is resolved once and applied to everything.

It follows through into the game, where `DemoGame::driveLocomotion` reads *the player's*
speed, ground state and jump out of the solver and then broadcasts them to every character in
the animator. Two characters today would animate off one character's physics. That loop is
the game's to fix, but it cannot be fixed while the machine underneath it is shared.

**This is a scale limit rather than a defect**, and it is invisible at one rig, which is why
nothing has hit it: the demo has a player and a morphed banner, and the banner has no clips.
The engine is meant to be judged against a substantial game, and "one animated character per
scene" is not a limit such a game can live with.

## What it costs

The two fields move onto the per-character struct that already holds `state`, `parameters`,
`current`, `previous` and `fade` -- so the storage change is small and the surface change is
two signatures. What has to be got right is the ordering already documented on `setRootNode`:
it restarts a character's measurement, so a per-character version must not be called every
frame for characters that already have one. `DemoGame` guards that today with a comparison
against `animator.rootNode()`, and the per-character accessor has to keep that possible.

Expected to be wrong about: whether `findNode` should stay animator-wide. It searches the rig,
and the rig is currently one per animator -- so a per-character root node named by string is
only half the story until [[C22-a-rig-that-arrives-at-runtime]] gives each character its own
rig. This row should do the storage split and leave that seam visible rather than pretend to
close it.

## Verification

- `./test.sh` in four configurations. New tests: two characters given different machines end
  in different states from the same parameter writes; a character whose root node is set does
  not disturb one whose is not; the all-characters overload still does what the single call
  did.
- `scripts/golden.sh` -- eleven cases, byte-identical. `skin` and `physics` both animate a
  character, and both must be unmoved: a scene with one rig gets one machine either way, and
  a moved pixel here means the single-character path changed behaviour rather than moving.

## Reference update

[architecture/systems.md](../../architecture/systems.md) -- "Blending and state machines",
which describes the machine as the animator's. It becomes the character's.

## Outcome

Landed. `machine` and `rootMotionNode` moved onto `SceneAnimator::Character`; the animator
keeps `defaultMachine` and `defaultRootMotionNode` as *templates* so a character created after
an animator-wide install still starts on the entry state -- which is what keeps "install the
machine, then load the rig" and the reverse both working, and it is the only part of this that
was not a rename.

Two things worth having found out:

- **The per-character `setRootNode` needs a guard the animator-wide one does not.** Setting a
  root node restarts that character's measurement, and `DemoGame` already avoids calling it
  every frame by comparing against `rootNode()`. A per-character version invites the same
  mistake with no shared value to compare against, so it returns early when handed the node it
  already holds.
- **`enterMachine` is a private member, not a free function.** `Character` is private, so the
  obvious anonymous-namespace helper does not compile -- which is the type system pointing at
  the right scope rather than an obstacle.

`DemoGame` is unchanged and still makes the one-call install: it has one rig, and rewriting it
to the per-character API would demonstrate nothing it can exercise. Its broadcast loop -- the
player's solver state applied to every character -- is now *fixable* and still wrong, and that
belongs to whichever row gives the demo a second rig.

Verified: 927 tests in four configurations, `scripts/golden.sh` byte-identical, zero validation
errors, and the demo's locomotion trace unchanged at `0 changes over 200 steps, drift 0.02`.

**One golden run reported a difference, and the cause was a GPU use-after-free introduced
alongside this row.** It was chased the wrong way first -- seven clean re-runs, then a re-snap
of all eleven baselines, which between them destroyed the evidence and made a passing suite
mean nothing. The answer was in the code, not in the repetition: `Renderer::rebuildAccelIfStale`
is the only caller of `buildAccelerationStructures` that runs *inside* the frame loop, and that
function opens with `destroySceneAccelStruct`. With `kFramesInFlight = 2` there are up to two
submitted command buffers still holding the TLAS, the BLASes and the hit-record buffer it frees.
`physics` is the one golden case that trips it -- it is the only log carrying the
"Acceleration structure rebuilt" warning -- so a single case differed, on whichever run the
reallocation landed badly. `rebuildAccelIfStale` now takes `vkDeviceWaitIdle` before the
rebuild; all eleven cases match with it in, which is the expected result for a change that
moves a stall rather than a pixel.

C11 had already written this failure down: *"A missing barrier here does not look like a
rendering bug at all -- it looks like flaky hardware, and the only reason it was attributable
was that the failure moved between cases while the images stayed identical."* An intermittent
golden failure is a synchronisation question first.
