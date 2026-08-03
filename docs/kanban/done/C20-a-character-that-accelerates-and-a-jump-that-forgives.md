---
id: C20
title: A character that accelerates, and a jump that forgives
arc: C
size: M
verification: golden-11, scripted-input, tests-hosted
---

# C20 — A character that accelerates, and a jump that forgives

`PhysicsWorld`'s character step gains a motion model where it currently has an assignment: a
velocity that ramps toward the request instead of arriving at it, a jump that survives being
pressed a few steps early or late, an answer for ground too steep to stand on, and step
settings scaled to the character rather than to whatever Jolt's header happened to default to.
`ColliderDesc` grows the rows that tune all four, beside the three it already has.

**This is capability, and it is the reason `walk` was unreachable for as long as it was.** G12
found that `characterSpeed` only ever read 0 or 3.2 m/s, because the controller assigns the
requested velocity outright — there is no value between the two for the machine to see. The fix
there was to make the *request* carry a magnitude, which was the right fix for that row and left
the underlying model untouched: the character still reaches full speed on the frame a key goes
down and stops dead on the frame it comes up. Every one of the four behaviours below is a thing
a game would otherwise write for itself against a solver that will not let it.

## What is already true

`PhysicsWorld::step` writes the velocity rather than integrating toward it, and the comment
already concedes the shape of the complaint:

```cpp
} else {
    // Full air control on the horizontal, gravity on the vertical. A game
    // wanting momentum instead changes this line; the engine's default is the
    // one that feels responsive rather than the one that is physical.
    velocity = JPH::Vec3(desired.GetX(), velocity.GetY(), desired.GetZ()) + gravity * dt;
}
```

"A game wanting momentum instead changes this line" is a game editing `engine/`, which is the
thing the engine/game split exists to prevent. The row makes it a number.

Three more facts the row collects on:

- **`c.jump` is latched and then thrown away.** `setCharacterInput` is careful — its comment
  explains that a jump pressed between two simulation steps would otherwise be overwritten — but
  the step clears `c.jump = false` unconditionally, whether or not the launch was applied. A
  jump pressed one step before touching down is silently eaten, and the launch itself is only
  added inside the `grounded` branch.
- **`characterOnGround` tests only `EGroundState::OnGround`.** `OnSteepGround` — sliding down a
  face steeper than the collider's 50° — reports false, so the state machine plays `fall` and a
  jump is refused. The engine has a slope limit and no behaviour on the far side of it.
- **`stepHeight` is parsed and never read.** `Collider.cpp` reads it out of glTF extras into
  `ColliderDesc::stepHeight`, documented as "how high a step the character walks up rather than
  into", and nothing anywhere consumes it: the step constructs a default
  `ExtendedUpdateSettings` and hands it to every character. Jolt's defaults are
  `mWalkStairsStepUp = {0, 0.4, 0}` and `mStickToFloorStepDown = {0, -0.5, 0}` — absolute
  metres, sitting two hundred lines from an `mSupportingVolume` the same function explicitly
  *does* scale to the capsule's radius. So the authorable row is dead, and the live one is
  wrong for any character that is not roughly human-sized.

## Scope

Four behaviours. Each is a defect above, and each gets a `ColliderDesc` row so the tuning is
authored in the scene rather than compiled in.

1. **Acceleration and deceleration.** Ground velocity ramps toward the request. Air keeps its
   own, smaller coefficient — the point is not to take responsiveness away but to make the
   current choice a value a game can set, which is what the comment quoted above asks for.
2. **A jump buffer and a coyote window, both counted in steps.** The buffer holds a press that
   arrived before the character landed; the window keeps the launch available for a few steps
   after it left the ground. Neither is a feel preference: without them a fixed-step solver
   sampled by a variable-rate game drops inputs at a rate that depends on the frame rate.
3. **Steep ground.** The row decides whether steep is a third answer from `characterOnGround`,
   a slide the solver applies, or both, and says which on this card. What it must not do is
   leave a >50° face indistinguishable from mid-air.
4. **Step settings scaled to the character.** `stepHeight` reaches `mWalkStairsStepUp`, and the
   step-down follows it rather than a constant. This is the smallest of the four and the one
   with a live dead row already in the schema.

Out of scope, stated so it is not re-argued: **crouch** and **double jump** (feel features with
no engine gap behind them); **character contact events** — that is G7's `ContactListener` and
its `playAt`, and this card must not restate it; **root motion**, which G12 deferred behind a
trigger that has not fired.

## Verification

- **`scripts/locomotion.sh`, and every distance in it moves.** This is the row's main risk and
  the one way it can pass for the wrong reason. G12's 8.40 m is arithmetic —
  `(90 × 3.2 × 0.45 + 90 × 3.2 + 60 × 3.2 × 0.45) / 60` to the centimetre — and a ramp turns
  each of those three products into an integral with a rise and a fall on either end. The new
  expectations are **derived from the new model before the code runs, and compared against the
  run afterwards.** Re-reading them off a passing run would delete the property the script was
  built to have, and G12 wrote a section explaining why.
- **The `still` arm does not move, and stays the negative control.** `idle`, 0 changes, 0.00 m
  travelled, 0.00 m rise, over the same 600 steps. Acceleration is a change to what a character
  being driven does; a character nobody is driving must be untouched by it, and a ramp with a
  sign error or a drifting residual shows up here first.
- `./test.sh debug`, then `./test.sh asan`. **Two existing tests change meaning and must be
  edited deliberately rather than deleted**: `PhysicsCharacter.MovesWhereItIsToldAndReportsItsSpeed`,
  whose tail asserts the character's speed reaches zero within 0.1 of the input being zeroed —
  that instant stop is precisely what this row removes, and the replacement asserts the *ramp*,
  not merely that it eventually stops — and `PhysicsCharacter.AJumpIsLatchedRatherThanMissed`,
  which pins the current latch and must keep passing beside the buffer rather than being
  subsumed by it. New tests for the buffer window, the coyote window, the steep-ground answer,
  and `stepHeight` reaching the solver.
- `scripts/golden.sh check release` — eleven byte-identical. The eleven press nothing, so this
  is the check that a motion model does not move a character nobody is driving; it is the same
  argument as the `still` arm, made against pixels instead of metres.
- **No `trace`.** The row adds no GPU work and no pass. Its per-step cost is a few arithmetic
  operations inside a loop over live characters, and a benchmark of that would report the noise
  floor.

## Reference update

[architecture/systems.md](../../architecture/systems.md) for the motion model and the new
collider rows, [architecture/limitations.md](../../architecture/limitations.md) for whatever the
row ends up declining, and [guides/making-a-game.md](../../guides/making-a-game.md) if the glTF
`extras.substrate_collider` schema grows — an authorable row nobody documents is how
`stepHeight` became a dead one.

## Against G13 and C16

**C16 is what makes this row checkable, and it is done.** A motion model is a claim about what
happens over a sequence of steps; `--input-script` and `scripts/locomotion.sh` are the only
thing in the tree that can drive one reproducibly. Nothing here is blocked — the harness exists
and this row re-derives its numbers.

[G13](G13-third-person-movement-the-camera-agrees-with.md) is the other half of the same
complaint from the game's side, and the two touch this script from opposite directions: G13
asserts a *direction* and leaves every distance alone, this row changes every distance and
leaves direction alone. If this row lands first, G13's heading arm is written against the
distances this one produced.

## Outcome

**All four behaviours, seven new `ColliderDesc` rows, and two negative arms per window.**

### The motion model

The horizontal velocity ramps toward the request instead of being assigned it, written against
the velocity *relative to what the character is standing on* so a ramp on a moving platform is
a ramp against the platform. Which of the two rates applies is decided by whether the request
is faster than the current motion rather than by whether it is zero, so a turn at speed
decelerates through it and accelerates out of it with no special case.

| Row | Default | Why that number |
|---|---|---|
| `acceleration` | 10 m/s² | 0 to the demo character's 3.2 m/s in 0.32 s. Deliberate rather than instant, and slow enough that the ramp is a *measurable* number of steps rather than a rounding difference. |
| `deceleration` | 40 m/s² | Four times the other, because the forces are not symmetric: getting going is limited by traction, stopping is limited by friction, and friction is the larger. 3.2 m/s to rest in 0.08 s, so a character still plants when you let go. |
| `airControl` | 0.35 | The fraction of both that applies airborne. Below one is what makes a jump carry the speed it launched with — the "momentum instead" the old comment said a game had to edit `engine/` for — while still steering. |
| `stepHeight` | 0.35 m | Unchanged; it now *reaches* `mWalkStairsStepUp`, with `mStickToFloorStepDown` at 1.25× it, which is Jolt's own ratio. |

**A pair large enough to close the gap in one step reproduces the old assignment exactly**, and
`PhysicsCharacter.AHugeAccelerationIsTheOldAssignment` pins it. That is what makes this a
number rather than a feel imposed on every game: the line the card quoted said "a game wanting
momentum instead changes this line", and a game changing a line in `engine/` is precisely what
the engine/game split exists to prevent.

### The two windows

`jumpBufferSteps` **10** (⅙ s) and `coyoteSteps` **6** (⅒ s), both `uint32_t` counts of fixed
steps and advanced only by the character loop.

- **Steps, not seconds.** G12 measured that sixty additions of `1.0f/60.0f` land just under a
  second, so a window compared against an accumulator would be off by a step at random. Steps,
  not frames, for the reason the card gives: a window in frames changes size with the frame
  rate, which is the defect the buffer exists to remove in the first place.
- **6 for the coyote window** is the value most platformers converge on, and it has an
  argument here beyond convention: at the demo character's 3.2 m/s, ⅒ s is 32 cm of overhang.
  Under half a stride, so it forgives a late press and cannot be used to cross a gap.
- **10 for the buffer, longer on purpose.** A buffered press only ever *delays* a jump the
  player asked for; a coyote window *grants* one the world did not offer. The asymmetry is the
  justification, not a preference.
- **The window a launch used is spent and refilled only by standing.** Without it a held jump
  key buys a second launch out of the air, which is a double jump — declined. The ground state
  lags a launch by one sweep, so `Character::launched` keeps the step *after* a jump from
  refilling what the jump just spent. `TheCoyoteWindowIsSpentByTheJumpThatUsedIt` is that.

### Steep ground, and what it did not change

`characterGround` answers `InAir` / `OnGround` / `Sliding`; `characterOnGround` is now
`== OnGround` and keeps its meaning. **The solver's behaviour on a steep face did not change
and did not need to** — steep already took the airborne branch, so gravity accumulates and the
character slides. What the row added is that the case is *visible*: a >50° face is no longer
indistinguishable from mid-air, which is what the card required. `Sliding` also absorbs Jolt's
`NotSupported`, because "there is something there and you are going down it" is one answer and
a fourth enum value would say nothing a game acts on differently.

### How the false pass was ruled out

Three separate ways, because the row had three separate ways to pass for the wrong reason.

1. **The total distance is not the acceleration assertion, and pretending it was would have
   been the false pass.** The four ramps nearly cancel — two accelerations lose `dv²/2a`, two
   decelerations coast `dv²/2d` past the request — so the derived 8.21 m sits 1.3% under
   G12's 8.40 m and a 3% band contains both. Two assertions carry it instead: the travel must
   be below the instant model by at least half the derived deficit (8.30 m), and **`walk ->
   run` must land 7.2 steps of ramp after frame 150.** The second is the sharp one. The
   modifier goes down at 150 and the request jumps 1.44 → 3.20; a controller that assigns
   crosses the 2.64 m/s threshold on the next step and G12 measured the transition inside six
   of frame 150. The run measured **159**, and the window is [157.2, 164.2] — the old
   behaviour is outside it.
2. **A jump pressed while grounded proves nothing about a jump buffer.** Both scripted arms
   press jump *in mid-air*, three and twenty-six steps before the landing respectively, and
   differ in nothing but that frame number. Where each press sits relative to the landing is
   asserted **from the log** rather than assumed, so a solver that lands a step differently
   fails on the placement instead of quietly ceasing to test the feature.
3. **The `still` and `modifier` arms are untouched.** `idle`, 0 changes, 0.00 m, 0.00 m over
   the same 600 steps — a motion model is a change to what a driven character does, and a
   character nobody is driving must be unmoved by it. The eleven golden cases press nothing
   and are the same argument against pixels; `physics.gltf` has a character in it and is
   byte-identical.

**The coyote window has no scripted arm, and the reason is content rather than rigour.** It is
a claim about walking off a ledge and the demo's scene has no authored one; manufacturing it
means editing `make_composite_scene.py`, which eleven golden cases and the readback suite also
read. So the hosted suite carries it, with a two-metre platform and nothing under it — a rise
after the ledge can only be a launch — and runs the *same* positive/negative pair the card
asked for twice over: the window set to zero, and the press moved past it. The buffer gets both
shapes: scripted arms for the window's size, hosted arms for the window at zero.

### Locomotion, before and after

```
G12   idle > walk > run > walk > idle > jump > fall > land > idle
      8 changes over 600 steps, 8.40 m travelled, peak rise 0.93 m
      walk -> run at 152-ish        run -> walk at 242

C20   idle > walk > run > walk > idle > jump > fall > land > idle
      8 changes over 600 steps, 8.21 m travelled, peak rise 0.93 m
      walk -> run at 159            run -> walk at 242
```

Every number that moved was derived before the code ran and matched to the centimetre.

- **8.40 → 8.21 m.** `8.40 − 1.44²/20 − 1.76²/20 + 1.76²/80 + 1.44²/80 = 8.206`. The two
  accelerations lose ground climbing into the speed; the two decelerations gain it coasting
  out. The observed 8.21.
- **`walk -> run` 152 → 159.** `(2.64 − 1.44) / 10 = 0.12 s = 7.2 steps` of ramp before the
  six of pipeline latency G12 already allowed for.
- **`run -> walk` unmoved at 242.** `(3.20 − 2.64) / 40 = 0.014 s`, under a step. The
  asymmetry between the two rates is visible here and nowhere else, which is the deceleration
  row doing exactly what it is for.
- **Peak rise 0.93 m, unmoved.** The launch speed did not change, and a jump from a standstill
  has no horizontal ramp to be affected by.
- **`jump -> fall` at 15 steps and `land -> idle` at 121, both unmoved.** Replacing the demo's
  guessed jump flag with `characterJumped` shifted the trigger by exactly zero steps, which
  the arithmetic predicted: the old flag was read at the `fixedUpdate` after the launch step,
  and so is the new accessor.
- **`fall -> land` at 52 steps against G12's 53.** One step, and it is `stepHeight`: the
  step-down went from Jolt's absolute −0.5 m to −0.4375 m, so the descending sweep finds the
  floor a step earlier. Against a ballistic `2v/g` of 51.4 it is the closer of the two.
- **The two new arms.** `jump-buffered`: `idle > jump > fall > land > jump > fall > land >
  idle`, 7 changes, 0.00 m travelled, 0.93 m rise — the second press at frame 108 landed 3
  steps before touchdown and launched at step 115, one step after `fall -> land` at 114.
  `jump-eaten`: the identical run with the press at 85, `idle > jump > fall > land > idle`, 4
  changes. Both 0.00 m travelled, because neither ever asks for horizontal motion.

### The numbers

- `scripts/golden.sh check release` — **11 of 11 byte-identical**, `physics` included.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette and the
  resize soak. Run although the card did not name it, because `engine/` changed.
- `scripts/locomotion.sh release` — **5 of 5 arms**, the paths above.
- `./test.sh debug` / `release` / `asan` / `tsan` — **876 of 876** in each, up from 869. Seven
  added: five character behaviours, the huge-acceleration equivalence, and the collider
  parser's new rows. TSan matters here and was run for it: the fixed-step character loop is
  hosted and nothing else checks its threading.
- Validation, 400 scripted frames of the demo scene in a debug build, **zero errors**. The one
  warning is `VK_LAYER_PATH hid the system layers`, which the engine emits and fixes itself.
  The debug run produced the same path and the same steps as release.
- **No trace.** The row adds no GPU work and no pass; its per-step cost is a vector length, a
  compare and two integer decrements inside a loop over live characters.
- One environmental note: `vkCreateDevice failed: VK_ERROR_DEVICE_LOST` killed the
  `walk-run-jump` arm twice in a row before the third run completed cleanly. The same arm run
  directly produced the numbers above. Nothing was re-snapped.

### The three things the estimate did not predict

1. **The demo could not be left alone, and the reason is an API defect rather than a tidy-up.**
   G12's `jumpRequested && characterOnGround(...)` is the controller's own decision re-derived
   from outside the controller, and it was correct only for as long as the two could not
   disagree. With a coyote window a press launches with no ground under it; with a buffer a
   press launches a step or two after the frame it arrived on. Neither is visible from the
   outside, so the flag had to be replaced by `characterJumped` rather than adjusted. **A game
   that had shipped G12's line would have got a silently wrong animation from an engine
   upgrade** — which is the sharpest argument the row produced for why the launch decision has
   to be reportable.
2. **The distance assertion could not discriminate on its own**, and finding that out before
   writing the code is what saved the check. The card said "every distance in it moves" and
   every distance did — by 1.3%, inside the 3% band the script already used. The transition
   *step* is where a ramp is unmistakable, because a ramp is a duration and a duration is what
   an assignment does not have.
3. **`stepHeight` reaching the solver moved no pixel**, which was not obvious in advance. The
   `physics` golden case has a character standing on a floor and `mStickToFloorStepDown` went
   from −0.5 m to −0.4375 m; a resting character's down-cast finds the same floor either way,
   so eleven cases are unchanged. The row a scene could author was dead for four stages and
   turning it on cost nothing.

### Deferred, and why

- **Crouch and double jump.** Feel features with no engine gap behind them, stated out of
  scope on this card and unchanged by anything it found. The coyote window being spent by the
  launch that used it is what forecloses double jump *accidentally* arriving.
- **A setter for a character's tuning.** Recorded in `limitations.md`: the eleven rows are
  fixed at `createCharacter`, so a power-up that lengthens a window or a surface that changes
  acceleration is out of reach. It is also the reason the "window at zero" arms are hosted
  rather than scripted. Trigger: a second thing that wants to change a character mid-run.
- **A ledge in the demo scene.** Would give the coyote window a scripted arm beside the
  buffer's. Trigger is a scene change that has its own reason to happen, because touching
  `make_composite_scene.py` re-generates an asset eleven golden cases read.
