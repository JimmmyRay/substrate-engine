---
id: G12
title: Locomotion through a six-state machine
arc: G
size: M
verification: golden-11, readback, tests-hosted, validation, scripted-input
---

# G12 — Locomotion through a six-state machine

M

## Why

Split out of **G9**, whose content table asked for *"WASD locomotion through a six-state
machine: idle, walk, run, jump, fall, land"* and named it as the thing that proves G8's
default bindings and S2.4's blending driven by a controller rather than by a triangle wave.

It came off that card for one reason, and it is a verification reason rather than a scope
one: **nothing about it is checkable without reproducible input.** G9 added 265 instances,
twelve emitters and thirteen bodies to the demo scene; landing a control scheme in the same
change would have meant that a moved pixel or a drifted trace had two candidate causes.
C16's `--input-script` is what makes this row provable on its own, and it landed after the
G9 card was written.

## What is already true

- `locomotionMachine` in `game/demo/DemoGame.cpp` builds a machine **by clip name** over
  whatever the scene happens to carry, and today it looks for four states: `idle`, `walk`,
  `run`, `jump`. The two missing ones are on the rig already — `Mana.gltf` carries
  `falling idle` and `hard landing` beside `jump`, `jumping up`, `walking` and `running`.
- `PlayerActions` declares `Up`/`Down`/`Left`/`Right` and `Space`, **deliberately not
  WASD**, because W, A, S and D are the camera's. G8 built
  `InputMap::setDefaultBindings` precisely so a game can take them, and *nothing calls it* —
  the guide's sketch shows the line and no row has written it.
- `fixedUpdate` already normalises `characterSpeed` against a jog and drives the `speed`
  parameter per step. What it does not have is anything airborne.

## Scope

- Move the camera off WASD with `setDefaultBindings` and rebind the player onto it, so the
  demo ships the control scheme a third-person game actually has. The binding menu must
  report the result as *shipped* rather than as something the player edited, which is the
  property `setDefaultBindings` exists to hold.
- Two more states and the parameters that reach them. `jump` is already a trigger; `fall`
  wants a grounded predicate and `land` wants the frame it goes back to true. Whether the
  predicate is `PhysicsWorld`'s or the game's is the decision the row makes.
- Keep the machine built by *name*, and keep the no-recognisable-clip case producing an
  empty machine — four test scenes rely on that.

## Verification

- `--input-script` driving a fixed sequence — walk, run, jump, land — with the state the
  machine settles in asserted at stated steps. **This is the row's central check** and the
  reason it is its own card: an animation state that depends on what somebody pressed is
  not checkable any other way, and a golden image of a character mid-blend is a picture of
  one frame rather than a claim about the transitions.
- `scripts/golden.sh check release` — eleven byte-identical. The eleven cases press
  nothing, so a difference means the row changed something outside the input path.
- A validation-layer run with zero errors.
- The two clips the machine gains must be *found by name on the rig*, verified by the
  demo's own `Animation: state machine over N states` line reporting six.

### How the scripted check avoids passing for the wrong reason

Written before any code, because the first draft of the bullet above would have passed for
an implementation that does not work. Two ways, and each is now an arm of its own in
`scripts/locomotion.sh`:

1. **Under gravity alone.** A check on where the character ended up is satisfied by one
   that only fell. So the distance asserted is a *horizontal path length* summed per step,
   and there is an arm — `still` — that presses nothing over the same 600 steps and must
   report `idle`, no transitions, and 0.00 m of travel and rise. Nothing about the row is
   allowed to move a character nobody is driving.
2. **A machine reporting the state it was told rather than the state it computed.** The
   parameters come from `characterSpeed` and `characterOnGround` and the trace comes from
   `currentState` and `characterTransform` — the far end of the chain, none of which knows
   a key exists. Three properties make that checkable rather than asserted:
   - an arm — `modifier` — holding `Player.Run`, **the one input in the scheme that names a
     state of the machine**, and nothing else. It must produce the same answer as pressing
     nothing at all: the modifier scales a vector, and a vector with no direction to scale
     changes nothing.
   - the `run -> walk` transition in the main arm, where **the movement key does not
     change**. Only the modifier is released; the request drops to 45% of travel, Jolt
     reports 1.44 m/s instead of 3.20, and the machine crosses its own threshold on the way
     down. An implementation reading the keyboard cannot produce that transition.
   - `fall` and `land`, neither of which has an input event within fifty steps of it. Both
     are asserted against the arithmetic they come from — a 0.25 s launch clip, a 2v/g arc,
     a 2 s recovery clip — rather than against a number lifted off a passing run.

This is P5's lesson applied a second time: *"cropping the source to the cell the animation
selected compares frame selection against itself."*

## Reference

[architecture/systems.md](../../architecture/systems.md) for the machine and the character
controller, [architecture/tooling.md](../../architecture/tooling.md) for the check,
[architecture/limitations.md § The game API](../../architecture/limitations.md#the-game-api)
for what this row found out, and
[guides/making-a-game.md](../../guides/making-a-game.md) for the call sites.

## Outcome

**Six states, WASD, and a check that can fail.** The machine is `idle`, `walk`, `run`,
`jump`, `fall`, `land`, still built by name against whatever clips the rig carries; the
camera gave up W, A, S, D and LeftShift along with the left stick; and
`scripts/locomotion.sh` drives the result through `--input-script` in three arms, two of
which press the wrong thing on purpose.

The scripted path, byte for byte and identical in debug and release:

```
idle > walk > run > walk > idle > jump > fall > land > idle
8 changes over 600 steps, 8.40 m travelled, peak rise 0.93 m
```

Every number in it is arithmetic rather than observation. 8.40 m is
`(90 x 3.2 x 0.45 + 90 x 3.2 + 60 x 3.2 x 0.45) / 60` to the centimetre. 0.93 m is `v²/2g`
= 0.899 plus under a step of overshoot. `fall` lands 15 steps after the launch, which is the
0.25 s of `jumping up` exactly; `land` 53 steps after it, against a ballistic `2v/g` of 51.4;
`idle` 121 steps after that, against the 120 of `hard landing`.

**What landed**: `game/demo/DemoGame.{h,cpp}` — the machine grew two states, a third
parameter and a helper that refuses a sentinel; `PlayerActions` grew `run` and a `declare`
that rebinds the camera before it takes the keys; `moveDirection` returns a *scaled* vector;
`DemoGame::driveLocomotion` is the new method and `LocomotionTrace` what it reports.
`scripts/locomotion.sh` is new, `scripted-input` is a new token in `scripts/kanban.py` and
in the closing-a-card skill, and one test was added to `tests/PhysicsTests.cpp`. **Two files
under `engine/` were touched, and the next section is the justification.**

### The row found a memory-safety defect in the engine, and it is fixed here

`PhysicsWorld` keeps two interpolation snapshots as one array each, laid out **bodies first
and characters after them**. Every accessor bounds-checks `current` and then indexes both,
which rests on the pair being the same length and under the same layout — true because
`finalize()` starts them equal and `step()` assigns one to the other.

**A body created after `finalize()` breaks both halves at once.** `createBody` grows
`bodies`, so the new body does not append: it *shifts every character's slot along*.
`previous` is then too short, which makes `characterTransform` read off the end of a vector,
and mislabelled from the first new body onward, which would interpolate a character against
wherever a crate happened to be made.

It has been reachable since G9 built fifteen bodies in `Game::init`, and it had no symptom
until something read a character transform on the step after — which is what this row did.
It reported a distance of **`inf`**. The fix is in `snapshot()`: hold the pair the same
length and the same layout, and start every moved slot equal to `current`, which costs those
objects one step without interpolation on the step a game created something.

`PhysicsWorld.ABodyCreatedAfterFinalizeKeepsBothSnapshotsTheSameLength` is the regression,
and it was checked the only way that means anything: **with the fix removed it fails under
ASan with `heap-use-after-free`, `READ of size 12`.** The golden set and the readback suite
are unmoved by the fix, which is the other half — the arrays are already the same length for
any scene whose bodies all come from its file, so nothing but a game building its own props
ever took the branch.

**Why this was not left as a finding.** It is undefined behaviour on a path the demo takes
every frame, and the row could not have produced a trustworthy number without fixing it —
the check's whole claim is that the distance came from the solver.

### The four things the estimate did not predict

1. **`walk` was unreachable and had been since it was written.** `moveDirection` returned a
   unit vector, the controller assigns that velocity outright rather than accelerating into
   it, and `characterSpeed` therefore only ever read 0 or 3.2 m/s — 0.0 or 0.8 normalised,
   either side of the band `walk` occupies. The four-state machine shipped with a state
   nothing could enter, and no check in the tree could have said so. The fix is that a
   movement request now carries a magnitude, which the controller already respected and
   nothing documented.
2. **The airborne parameter had to be spelled the other way round.** Written as `grounded`
   it defaults to zero, and a machine nobody is driving — `driveCharacters`' triangle wave,
   for a scene with a rig and no controller — falls through the floor on its first frame.
   As `airborne` the default is *standing on something*, which is what a parameter starting
   at zero must mean.
3. **`findState` returns `kAnyState` for a name the rig does not have, and `kAnyState` is
   also the wildcard.** A missing state written straight into a transition is therefore a
   *wildcard* rather than a no-op — a rig without `fall` would have got "from everything to
   nothing" and a rig without `idle` a live transition into state 0, which the four-state
   version wrote as `idle != kAnyState ? idle : 0u` without saying why. Named transitions
   now go through a helper that refuses the sentinel, and the one genuine wildcard is
   written by hand.
4. **`fall` cannot be entered from `kAnyState`.** It would hold one step after a launch and
   cut the jump clip that the whole airborne half depends on finishing. It is three
   enumerated transitions, from the three grounded gaits.

Two smaller ones, both recorded in `limitations.md`: a `CharacterVirtual` has no ground
state until it has been swept and `Game::fixedUpdate` runs before the first sweep — which is
why the demo skips step zero, and which the `still` arm caught as a character that fell and
landed in the first three steps of every run; and `jumping up` had to be preferred over
`jump` as the launch clip, because on a Mixamo rig `jump` is the whole leap including its
own landing and would play that landing a second and a half before `land` does.

### What was good

The machine needed no engine change to gain two states — it is data, and six states with
twenty-two transitions read as a table rather than as a graph. `waitForExit` turned out to be
exactly the right primitive for "let the launch play", and the `t.to == c.state` skip in
`stepStateMachine` is what makes a self-satisfying condition safe to write. And the whole of
G8's `setDefaultBindings` argument survived contact: one lambda, five lines, and the binding
menu reports a shipped scheme as shipped.

### The numbers

- `scripts/golden.sh check release` — **11 of 11 byte-identical.**
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette and the
  resize soak. Run although the card did not name it, because the fix is in `engine/`.
- `./test.sh debug` — **779 of 779**, up from 778. `./test.sh asan` — **779 of 779**.
- `scripts/locomotion.sh release` — **3 of 3 arms.** `still` and `modifier`: `idle`, 0
  changes, 0.00 m, 0.00 m. `walk-run-jump`: the path above.
- Validation, 600 scripted frames of the demo scene in a debug build, **zero errors**. The
  one warning is `VK_LAYER_PATH hid the system layers`, which the engine emits and fixes
  itself. The debug run produced the same path and the same 8.40 m and 0.93 m as release.
- No trace. The row adds no GPU work and no pass; its per-step cost is three accessor reads
  and a compare, and a benchmark of that would be a measurement of the noise floor.

### Deferred

- **Strafe and turn clips.** The rig carries `left strafe`, `right strafe`, `left turn 90`
  and their walking variants, and the machine drives none of them: `speed` is a magnitude
  and has no sign, so a character walking sideways plays the forward walk. Trigger: a
  direction parameter, which is the point at which the machine wants a 2D blend rather than
  a fourth state, and `blendPose` blends two poses. `limitations.md` already refuses blend
  trees and this is the first concrete case that would want one.
- **Root motion.** `setRootNode` exists and the demo does not call it, so the run cycle
  slides against the controller's speed at whatever ratio the clip was authored at. That is
  a tuning job with no verification attached to it, and doing it inside this row would have
  put a number nobody can check into the middle of one every number is checked in.
- **A `characterMaxSpeed` accessor.** The demo divides by a hard-coded 4.0 for a character
  the scene file gave 3.2 m/s. Trigger: a second character with a different top speed.
