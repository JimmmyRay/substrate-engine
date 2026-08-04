---
id: bug-a-blocked-character-reports-the-speed-it-asked-for
title: A blocked character reports the speed it asked for
arc: bug
size: S
verification: scripted-input, tests-hosted, golden
---

# bug-a-blocked-character-reports-the-speed-it-asked-for — A blocked character reports the speed it asked for

A character walked into a wall stops moving and goes on reporting a full-speed run.
`characterSpeed` and `characterVelocity` are read out of Jolt's stored linear velocity, which
is the velocity the engine *set* — so what comes back is `setCharacterInput`'s request scaled
by `moveSpeed`, with the ground's carry taken off it, and never what the sweep resolved.

```cpp
// engine/scene/Physics.cpp:1363 -- the request goes in
cv.SetLinearVelocity(JPH::Vec3(base.GetX() + relative.GetX(), vertical, base.GetZ() + relative.GetZ()));
...
// engine/scene/Physics.cpp:1388-1392 -- and the same number comes back out
const JPH::Vec3 v = cv.GetLinearVelocity();
c.velocity = glm::vec3(v.GetX() - under.GetX(), 0.0f, v.GetZ() - under.GetZ());
```

`CharacterVirtual::Update` slides the shape through the world with
`MoveShape(mPosition, mLinearVelocity, ...)`
([`CharacterVirtual.cpp:1461`](../../../external/JoltPhysics/Jolt/Physics/Character/CharacterVirtual.cpp#L1461)),
and `mLinearVelocity` is passed by value: the position is solved against every contact and the
velocity member is left exactly as the caller set it. Nothing between the two lines above can
have changed it.

**The header claims the opposite, in the detail that makes it load-bearing.**
[`Physics.h:414-416`](../../../engine/scene/Physics.h#L414) says it is "still what the solver
*did* rather than what it was asked for -- a ramp is climbed at the speed the ramp allows and a
wall slides the direction along it -- so it is a heading, not a restatement of
`setCharacterInput`". Every one of those three cases is wrong today, and a game reading the
call gets the restatement the sentence promises it will not.

Two callers are already built on the promise. `game/demo/DemoGame.cpp` drives the locomotion
machine's `speed` from it, and `game/battle_arena/ArenaWorld.cpp:1391-1418` drives both that
and the mesh facing — the second being C30's whole point, that a heading has to come from the
motion rather than from the input.

Measured on `battle_arena`, walking a fighter dead-centre into a column with the run modifier
held for 500 locked steps:

```
Arena: idle -> walk at step 35 (0.83 m/s, grounded)
Arena: walk -> run at step 44 (2.33 m/s, grounded)
Arena path: idle > walk > run
Arena: 2 changes over 500 steps, 9.07 m travelled, net 9.07 m
```

The fighter arrives at the column around step 200 and is stationary for the remaining three
hundred — 9.07 m is the 10.38 m to the column's centre less its 1.0 m radius and the capsule's
0.3 m — and the machine holds `run` throughout. Releasing the keys at step 400 produces
`run -> walk` at 403 and `walk -> idle` at 405, which is the proof that the state was tracking
the key and not the sweep: nothing about the fighter changed at step 400 except what was being
asked of it.

The fix is to difference the solved position across the step instead, and take the ground's
motion off *that* — which is what the header already describes and what
[`bug-a-carried-character-turns-to-face-what-carries-it`](../done/bug-a-carried-character-turns-to-face-what-carries-it.md)
established the subtraction for. A rider's position delta is its carry, so the difference stays
zero and that row's arm is unaffected in the steady state.

Not caught earlier because no arm anywhere presses a character into anything.
`scripts/locomotion.sh` walks the demo's character across open floor in every one of its nine
arms, and on open floor the request and the result agree to the last decimal — which is exactly
why the bug survived C20, C30, G15 and two rows that read the call.

Expected to be wrong about: whether the demo's numbers move. The acceleration ramp is the
engine's own integration and is reached by both readings, so `walk-run-jump` should be
untouched, but `platform-ride` crosses two reversals during which the rider is being dragged up
to the platform's speed and the two answers differ for those steps.

## Verification

- `scripts/locomotion.sh` — all nine arms, unchanged expectations. Any number that moves is
  re-derived from the model rather than read off the run.
- `scripts/arena.sh` — the new Phase 2 harness, whose `column` arm is this defect's regression
  check: the fighter stops at the column *and* the machine leaves `run`.
- `./test.sh debug`, then `./test.sh asan`: a character driven into a static body reports a
  speed of about zero while its input asks for `moveSpeed`.
- `scripts/golden.sh` — byte-identical. No golden case drives a character into anything.
- Zero validation errors.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the physics character surface, where
the solver-vector trio is described.

## Deferred

- **A mesh built in code cannot say that it moves** —
  [`chore-a-mesh-built-in-code-cannot-say-that-it-moves`](../backlog/chore-a-mesh-built-in-code-cannot-say-that-it-moves.md),
  opened in `backlog/`. `createMesh` takes no `InstanceMotion` while the `addInstance` two
  lines below it refuses to default one, so `battle_arena` sets `kInstanceDynamic` by hand
  twice. Found while establishing that the roadmap's `addModel` row is a different row: a
  skinned rig lands in the deformed tier and never touches the static one.

## Outcome

**Six lines of engine, and the interesting part is what it took to see them.** `c.velocity`
is the swept displacement now — the position taken before `ExtendedUpdate` and differenced
after it, with the ground's own motion subtracted from *that* rather than from Jolt's stored
velocity. The estimate held at S.

The fix is deliberately not fed back into C20's ramp. That integrator's state is the request,
and feeding the result into it would make a character that leaned on a wall for a second have
to re-accelerate out of it — a behaviour change nobody asked for, arriving inside a defect
fix. `characterVelocity` is a report; the ramp is state. Keeping them apart is what left the
demo's nine arms byte-identical, `platform-ride`'s 0.27 rad included.

**What the estimate did not predict was that the check had to be built before the defect could
be stated.** `scripts/locomotion.sh` hard-codes `run.sh demo` and every number in it is
derived from the demo's collider and the demo's scene, so there was nowhere to put an arm that
walks a fighter into a column. `scripts/arena.sh` is that harness — seven arms over
`battle_arena`, and the six that mirror `locomotion.sh` all passed on the first run against
expectations derived from the arena's own constants, which is what made the seventh's failure
readable as a defect rather than as a mis-derivation. The one thing that had to be exact was
the approach: a capsule a few degrees off centre slides around a cylinder instead of stopping
at it, so the camera yaw is computed from the spawn and the column's centre rather than
eyeballed, and the stop distance is arithmetic — 9.08 m predicted, 9.07 measured.

Verification, each its own invocation: `scripts/arena.sh debug` 7 of 7 arms;
`scripts/locomotion.sh debug` 9 of 9 arms, every figure unchanged; `./test.sh debug` and
`./test.sh asan` 1076 tests over 109 suites each, including the two-wall arm this card added
to `tests/PhysicsTests.cpp`; `scripts/golden.sh` 13 of 13 byte-identical; zero validation
errors and zero warnings over a 300-frame windowed run.

The counterfactual was measured rather than argued. The identical `column` arm against the
code this card replaced: `idle > walk > run`, two changes over 500 steps, 9.07 m travelled —
the same stop, and the machine never left `run`. Releasing the keys at step 400 produced
`run -> walk` at 403 and `walk -> idle` at 405, which is the state tracking the key. Both of
the arm's assertions fail against it.
