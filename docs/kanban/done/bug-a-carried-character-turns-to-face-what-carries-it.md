---
id: bug-a-carried-character-turns-to-face-what-carries-it
title: A carried character turns to face what carries it
arc: bug
size: S
verification: scripted-input, tests-hosted, golden-11
---

# bug-a-carried-character-turns-to-face-what-carries-it — A carried character turns to face what carries it

A character standing still on the demo's sliding platform turns to face the direction the
platform is sliding, and spins 180° every time the platform reverses. Nobody is touching a key.

The facing is derived from the capsule's **world** displacement:

```cpp
// game/demo/DemoGame.cpp:1418-1422, then :1468-1470
const glm::vec3 moved = at - locomotion.previous;
const glm::vec3 flat(moved.x, 0.0f, moved.z);
const float carried = glm::length(flat);
...
if (carried > kFacingFloor * step) {
    facingYaw += std::clamp(shortestTurn(facingYaw, std::atan2(flat.x, flat.z)), ...);
}
```

World displacement includes anything carrying the character. Jolt adds the ground's velocity as
the base of the character's own ([`Physics.cpp:1097`](../../../engine/scene/Physics.cpp#L1097)),
and the demo's platform slides at up to about 1 m/s against a `kFacingFloor` of 0.16
([`DemoGame.cpp:649`](../../../game/demo/DemoGame.cpp#L649)) — so the gate opens and the rig
turns. `stepDemoWorld` drives the platform on `sin(clock * 0.55f)`
([`DemoWorld.cpp:988`](../../../game/demo/DemoWorld.cpp#L988)), and each half-cycle reverses the
heading.

**The engine already draws the right distinction and the demo cannot reach it.**
`characterSpeed` is deliberately ground-relative — "standing still on a platform moving at
2 m/s is `0`" ([`Physics.cpp:1140-1150`](../../../engine/scene/Physics.cpp#L1140)) — but only
the magnitude is kept, so the *direction* is unrecoverable and differencing transforms is the
only thing a game can do. That gap is
[C30](C30-the-solver-vectors-and-turning-toward-a-heading.md), and this row is its first
caller rather than a separate fix: gating on the scalar alone would stop the spin while
leaving the heading skewed whenever the character walks *while* riding, and pointing somewhere
arbitrary when it walks against the platform at its own speed (world displacement near zero,
direction pure noise).

Not caught earlier because `scripts/locomotion.sh` walks a character on static ground; no arm
puts one on the platform. A fifth arm that does is part of the fix.

Expected to be wrong about: whether `kFacingFloor` survives as a distance-per-step compared
against a displacement, or becomes a speed in m/s compared against a velocity. It reads as the
latter today and is spelled as the former.

## Verification

- `scripts/locomotion.sh` gains an arm that stands the character on the platform through at
  least one reversal and asserts the facing does not change. The existing `facing >= 0.85`
  arms must still pass — they read the rotation back out of the scene tree rather than off the
  angle the game wrote.
- `./test.sh debug`, then `./test.sh asan`: a character on a kinematic body reports a
  ground-relative velocity of about zero while its world displacement is not zero.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case runs the demo world.

## Reference update

None expected — this is a defect in `game/demo/` plus C30's accessor. If `kFacingFloor` changes
units, [architecture/systems.md](../../architecture/systems.md)'s locomotion section says so.

## Outcome

`kFacingFloor` survived as spelled, and the card's "expected to be wrong about" was answered the
other way: the comparison became a speed against a speed rather than a distance against a
distance, because what it is compared to is now `characterVelocity` — a velocity — rather than a
per-step displacement. The constant did not move and its units did not change; the thing on the
other side of the `>` did.

The demo now reads its heading off C30's accessor, which is the half of this row that was a
one-line change once the accessor existed. **The half that was not is the arm**, and it needed
two things the card did not name.

**A character cannot walk onto that platform.** Nothing in the demo could put one there, so the
arm was unreachable until C29 landed `setCharacterTransform`. The demo gained
`Scene.RidePlatform` on `Slash` rather than a `--spawn` flag: a scripted press is a path
`--input-script` already drives, and a respawn onto a moving thing is the shortest
demonstration of C29 there is.

**Nine arms could not have seen it and a tenth kind of number was needed.** Every ratio the
summary reports divides by `travelled`, and `travelled` is world displacement — which a rider
accumulates exactly as a walker does, so `facing` reads 0.97 for a character being steered by
the floor and looks like a pass. `LocomotionTrace::turned` is radians of yaw off the scene tree,
against the heading the run started with, divided by nothing. That is the same shape as the pose
drift check, and for the same reason: eight arms could not see a defect and needed a ninth
*number* rather than a ninth arm.

**The arm's assertion is not zero, and the reason is worth keeping.** The character is dropped
onto a platform already doing 0.9 m/s and is dragged up to its speed over C20's acceleration
ramp; for those few steps its ground-relative motion really is 0.9 m/s backwards, which is a
heading and the rig faces it. That settle is 0.27 rad and nothing after it moves — the sinusoid's
peak acceleration leaves a residual two orders below `kFacingFloor`. The counterfactual was run
one build apart on the same arm: **3.14 rad**, the character facing the way the floor is going
and swinging a half turn at each reversal.

The trace also re-seeds itself at a placement. A teleport is not travel, and the step that
crosses the gap would otherwise be one displacement the size of the room in every sum that
divides by `travelled`.

**What ran.**

- `scripts/locomotion.sh debug` — **9 of 9 arms**, `platform-ride` among them: carried 6.06 m
  through two reversals, path `idle`, 0 state changes, `turned 0.27`.
- `./test.sh debug` and `./test.sh asan` — 976 tests. `RidingAPlatformIsNeitherWalkingNorAHeading`
  is the hosted half and predates this row's arm.
- `scripts/golden.sh` — 11 of 11, byte-identical.

One run of the suite failed on `vkCreateDevice ... VK_ERROR_DEVICE_LOST` before any assertion
ran, which is `bug-the-device-is-lost-part-way-through-a-suite` and not this row. The re-run was
clean.
