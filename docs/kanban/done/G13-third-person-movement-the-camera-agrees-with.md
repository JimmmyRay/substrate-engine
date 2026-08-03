---
id: G13
title: Third-person movement the camera agrees with
arc: G
size: M
verification: golden-11, scripted-input, tests-hosted, validation
---

# G13 — Third-person movement the camera agrees with

A follow rig in `game/demo/` that aims the camera at the player instead of leaving the two
unconnected, a movement basis derived from the camera rather than written out a second time
beside it, and a character that turns to face where it is going. The row's centre of gravity is
a fourth arm in `scripts/locomotion.sh` that asserts a *direction* — which is the one thing
G12's three arms cannot, and the reason a defect has been sitting in `moveDirection` since it
was written.

**G12 shipped the control scheme; nothing shipped the camera it is relative to.** The demo took
W, A, S, D and left the free-fly orbit on the arrows, so playing it means driving two things at
once, and the character walks in a direction nothing checks. This row is the other half, and it
is a G row by the letter of the rule: `Camera`'s pose is public, `Engine::playerNode()` exists,
`PhysicsWorld` already reports a transform. Nothing here is a capability the engine lacks — it
is the demo reaching for three it already has and joining them up.

## The defect this row exists to fix

`Camera::forward()` is `(cos p·sin y, sin p, cos p·cos y)`, so its horizontal component is
**`(sin y, 0, cos y)`**. `PlayerActions::moveDirection` builds **`ahead = (sin y, 0, -cos y)`**
from the same yaw. The two are the same vector only where `cos y == 0`; everywhere else the
heading error is `π − 2·yaw`, and at yaw 0 it is a full 180° — W walks the character *toward*
the camera.

It has never been caught, and the reason is worth writing down rather than fixing quietly:

- `Camera::frameBounds` sets `yaw = 1.5708f` when X is the longer horizontal axis and `0.0f`
  when Z is. The showcase scene is 30 units of X against 18 of Z, so it takes the first branch —
  **the exact yaw where the bug cancels.** A scene one flip of that comparison away plays it at
  full strength.
- `scripts/locomotion.sh` passes no `--camera`, so all three arms inherit that same yaw. The
  main arm asserts 8.40 m of travel and gets it, because `travelled` is a horizontal *path
  length* summed per step — deliberately, so a character that only fell cannot pass — and a path
  length is the one quantity that is identical whichever way the character walked.
- No test covers `moveDirection` at all: `game/demo` is not in the unit build.

So three checks pass over it, each for its own good reason. That is what makes this a row rather
than a sign flip: the fix is one character, and the check that would have failed does not exist.

## Scope

- **A follow rig owned by the demo, not the engine.** The engine keeps its free-fly orbit
  untouched — it is what `./run.sh` with no game uses to look at Sponza, and eleven goldens are
  framed with it. The demo re-aims `focus` at `playerNode()` each frame and leaves yaw, pitch
  and distance to the mouse, which is the mode a third-person game actually has. Whether the
  focus snaps or lags, and where the shoulder offset goes, is the decision the row makes.
- **One basis, derived once.** The camera and the player must read the same horizontal forward,
  and the row says which of the two is authoritative instead of correcting one sign and leaving
  two expressions of the same thing in the tree. `Camera::forward()` is the candidate; a
  horizontal accessor beside it is the smallest thing that could serve both.
- **Facing.** `characterTransform` returns `CharacterVirtual::GetRotation()` and nothing in the
  engine ever calls `SetRotation`, so the mesh keeps whatever heading the rig was authored with
  and strafes everywhere. Prefer a rotation the game composes into the node over a
  `setCharacterFacing` on `PhysicsWorld`: a setter is capability, it belongs to whichever row
  wants the solver to know a facing, and putting it here blurs the split this card sits on.
- **Keep the magnitude contract intact.** `moveDirection` returns a vector whose *length* is the
  fraction of top speed, and G12's `run -> walk` transition at step 242 turns on exactly that —
  the movement key does not change there, only the modifier. A rewrite that normalises would
  break the one assertion proving the state machine reads the solver rather than the keyboard.

Deferred, with triggers: **camera occlusion.** `PhysicsWorld::sweepSphere` is a ray with
thickness and is what a spring arm would pull on, but nothing in the showcase scene puts
geometry between a follow camera and the player. Trigger: an interior the player can walk
behind. **Turn clips**, which G12 already deferred behind a direction parameter and a 2D blend —
facing the character is not the same as animating the turn, and this row does the first only.

## Verification

- **A fourth arm in `scripts/locomotion.sh`, and it must fail on today's tree.** Same forward
  press as the main arm, run under `--camera fx,fy,fz,0,pitch,dist` — **yaw 0**, chosen because
  it is where the current basis is exactly backwards. The assertion is on net displacement
  against the camera's own forward, so an implementation that fixed the sign in one place and
  not the other still fails it. Checking that it fails before the fix is part of the row, in the
  sense G12 meant: a check written after the code passes is a description of the code.
- **`LocomotionTrace` gains net horizontal displacement.** `travelled` is a path length and
  cannot tell forward from backward, so there is nothing to assert direction against today. The
  `Locomotion:` report line is appended to rather than rewritten — `locomotion.sh`'s `sed`
  pattern ends in `.*`, so the three existing arms keep parsing unchanged.
- **The existing three arms are unmoved.** `still` and `modifier`: `idle`, 0 changes, 0.00 m
  travelled, 0.00 m rise. The main arm: the same eight transitions, 8.40 m and 0.93 m. Those
  numbers are arithmetic off the character's 3.2 m/s and 4.2 m/s, and this row changes neither,
  so any movement in them is this row breaking something rather than tuning it.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. `game/demo` is not in the
  unit build, so this covers only what lands under `engine/`; if the row lands nothing there,
  the card says so on close rather than leaving a token unexplained.
- `scripts/golden.sh check release` — eleven byte-identical. The eleven press nothing and set
  their own camera, so a difference means the row moved something outside the input path.
- A validation-layer run of the demo scene, zero errors.

## Reference update

[architecture/systems.md](../../architecture/systems.md) for the rig and the facing,
[architecture/tooling.md](../../architecture/tooling.md) for the fourth arm, and
[guides/making-a-game.md](../../guides/making-a-game.md) for the call site — a follow camera is
the first thing a game copies out of the demo, and the guide currently shows it nothing.

## Against C20 and C16

**C16 is what makes this row checkable, and it is done.** `--input-script` and the harness
G12 built on it are the only way to assert a heading without a human at the keyboard; a golden
image of a character facing the wrong way is a picture of one frame, and the mirror is invisible
in a path length. This row is not blocked by anything — it inherits a harness that already
exists and extends it by one arm.

[C20](C20-a-character-that-accelerates-and-a-jump-that-forgives.md) is the other half of the
same complaint and touches the same script from the opposite side: this row asserts direction
and leaves every distance alone, C20 changes every distance and leaves direction alone. Doing
this one first means C20 re-derives its numbers against a check that already covers heading.

## Outcome

**The defect was real, it was exactly as described, and the fix is one expression.** Everything
above about `moveDirection` held up: at yaw 0 the character walked 180° away from where the
camera pointed, measured at `along` **−0.98** before the change and **+1.00** after. What the
card got wrong is smaller and is corrected below.

### What already existed

The scope check found more standing than the card assumed, and less than the last three rows
found. `Camera` has a public pose, `frameBounds`, an orbit that holds the eye and swings the
focus, and `--camera`/`--camera-spin` to write four of those numbers from a command line.
`Engine::playerNode()` and `characterTransform` were both there. G1's ordering — `camera()
.update()` ahead of `Game::frameUpdate` — was there and `making-a-game.md` had *already
written the follow camera down as a sketch*, in a section titled "The camera is the game's, and
needs no engine method", complete with the argument for why overwriting `focus` inverts the
orbit. Nothing implemented it, and the sketch called `scene.worldPosition(player)`, which does
not exist. So: the argument was made, the surface was ready, and the four lines had never been
written. That is not "already satisfied" — a documented intention with no call site is exactly
the state G6, G8 and D11 were *not* in.

The three deferred things were genuinely absent: no follow rig, no facing, no direction
assertion anywhere in the tree.

### What landed

- **`PlayerActions::moveDirection` takes the camera rather than a yaw**, flattens
  `Camera::forward()` and takes screen-right from the same `cross(forward, up)`
  `Camera::update` takes. The old `side` expression was already that cross product written
  out, which is why one sign error mirrored strafing as well as walking.
- **A follow rig in `frameUpdate`**: four lines, gated on `world.built`, snapping `focus` to
  the character's transform plus 1.2 m. No shoulder offset — an offset puts the focus off the
  thing being framed and makes the orbit asymmetric, which is a decision an aim mode makes and
  this demo has none.
- **Facing**, slewed at 16 rad/s toward the direction the *solver* moved the character,
  composed onto the child node the loader makes for the mesh.
- **`LocomotionTrace` gains four fields** — net displacement, and three ratios: `along` the
  camera, `across` it, and along the node's own facing. Appended to the report line, so the
  `sed` pattern that predates the row still matches and the five existing arms parse unchanged.
- **Three arms in `scripts/locomotion.sh`**, not one. Why three is below.

### The ordering, and why the chase problem does not arise

Input is resolved against the camera **after** `Camera::update` and **after** the rig, in that
order. The engine runs the camera in `beginFrame`, so the yaw is this frame's; the rig then
re-aims `focus`; only then is "forward" asked for. Resolving before the camera's own update
would drive the character off the previous frame's yaw, which is a lag nothing downstream can
remove.

But the ordering is not what makes the coupling safe — **the rig writing `focus` and never
`yaw` is.** A rig that also aimed the camera from the character's heading would be two
integrators feeding each other, and the usual answer is to damp one until the drift is slow
enough not to see. Here the basis does not depend on the motion it produces at all, so there is
nothing to damp and no rate to tune. That is the sentence worth keeping: *the coupling is
one-way by construction rather than by being damped.*

The focus is **snapped**, and that decision came out of a finding rather than taste: see (6)
below.

### The negative arm, and why one arm would have reproduced the original mistake

The counterfactual was run twice — once with the pre-G13 basis restored, once with the camera
ignored outright and a constant `(0, 0, 1)` substituted — before the code was fixed:

| Arm | pre-G13 basis | fixed yaw, camera ignored | landed |
|---|---|---|---|
| `camera-north` (yaw 0) | `along` −0.98 ✗ | `along` +1.00 **✓** | +1.00 |
| `camera-south` (yaw 180) | `along` −1.00 ✗ | `along` −1.00 ✗ | +0.98 |
| `camera-turning` (spin 1.5°/frame) | `along` −0.04 ✗ | `along` −0.20, net = travelled ✗ | +0.94, net 0.31 × travelled |

**`camera-north` passes a fixed basis.** At yaw 0 a constant world heading and the camera's
own coincide, so an arm at the yaw where the *old* bug is maximal is still not a check on
agreement — which is the original mistake in a new costume, and the reason the row has three
arms rather than the one it was scoped for. `camera-south` is the same six keystrokes under a
camera pointing the other way, and no fixed heading satisfies both. `camera-turning` is the
strongest of the three and the one that answers "turning while the camera turns": the camera
spins while the key is held, so a character that agrees with it walks most of a circle and ends
1.06 m from where it started after 3.47 m of walking, while one resolving against anything
fixed walks a dead straight 4.24 m. Both halves are asserted — the ratio *and* net over
travelled — so the arm fails on either.

The `still` and `modifier` arms gained the same fields at zero, which is what forbids a trace
that accumulated a projection while `travelled` stayed flat.

### What the card got wrong

- **"The eleven press nothing and set their own camera."** They press nothing; they do *not*
  pass `--camera`, and `engine/assets/physics.gltf` authors a character. An ungated follow rig
  would therefore have moved a golden baseline. The rig and the facing sit behind
  `demoWorldApplies` — G9's gate, reused rather than invented — and all eleven cases are
  byte-identical. The general rule is now in the guide: **a camera is content by the same test
  a mesh is**, because what a golden case pins is the frame and not the geometry.
- **"A fourth arm."** Three, for the reason above. The script had five before this row, not
  three; the eight-arm table is in `tooling.md`.
- **`--camera-spin` was not in the card's plan at all** and turned out to be the single most
  discriminating flag available to it.

### API findings (the fourth look — G9 found nine, G12 six, C20 four; this found seven)

1. **`Scene::setLocalRotation` on a body- or character-driven node is silently discarded.** The
   sweep writes the solver's matrix into `world` and `continue`s, so the local TRS is never
   composed. The call returns void, the node reports dirty, and reading the rotation back gives
   what was written — every observable says it worked. This cost nothing here only because the
   card said to look; it is the sharpest edge on the scene API and a candidate for a warning
   the first time a driven node's local transform is written.
2. **Nothing publishes the name of the child node the loader creates.** `Engine::loadScene`
   creates it as `"mesh"` under the driven node, and a game reaching the mesh a body drives has
   to walk `firstChild`/`nextSibling` and compare a string against a convention no header
   states. `playerNode()` returns the driven node; there is no accessor for what hangs off it.
3. **`Camera` has no horizontal forward and should not grow one** — one caller wants it, and
   the fix is to flatten `forward()` at that caller. What was missing was not an accessor but a
   sentence in the guide saying *derive it, never restate it*, which is now there.
4. **`--camera` cannot state a yaw alone.** All six numbers or none, so a run that only cares
   about the heading has to write a focus, and under a follow rig that focus is overwritten on
   the first frame. The three new arms write one for readability and it means nothing.
5. **A `Locomotion:` line is the only channel a scripted run has**, and it is a `printf`. Four
   fields were appended because the `sed` pattern ends in `.*`; a fifth row wanting structured
   output would be the moment to stop, and the trigger is the second consumer.
6. **`Game::frameUpdate` is handed a wall-clock delta even under `--locked`.** Nothing says so,
   and anything a game smooths against it stops reproducing under `--capture` — which is what
   decided snap over lag here, and is worth a line wherever `Game.h` describes the pair. The
   fixed step is the only deterministic delta a game gets, so per-frame smoothing has no
   correct rate to use.
7. **Camera occlusion cannot be built from `PhysicsWorld` in this scene, and the card's trigger
   for it was the wrong one.** The deferral named "an interior the player can walk behind" —
   the showcase scene already is one, and a captured frame at yaw 0 shows the follow camera
   standing inside Sponza's arcade curtains with the character invisible behind them. The
   reason it still cannot be done is the opposite of missing content: **Sponza declares no
   collider at all.** `showcase.gltf` authors one static ground box, and `DemoWorld` adds
   crates, so a spring arm sweeping with `PhysicsWorld::sweepSphere` would find nothing to
   retract from and the curtain would still be there. The real trigger is a scene whose walls
   exist in the physics world, or a spring arm pulled on C9's instance index instead — which is
   a different query against a different structure and a row of its own.

### Verification

- `scripts/golden.sh check release` — **11 of 11** byte-identical, `physics` included.
- `scripts/locomotion.sh release` — **8 of 8**. The five that predate the row are unmoved and
  match C20's recorded numbers exactly: `still` and `modifier` `idle`, 0 changes, 0.00 m, 0.00 m;
  `walk-run-jump` the same eight transitions, **8.21 m** and **0.93 m**; `jump-buffered` 7
  changes and `jump-eaten` 4, both 0.00 m travelled and 0.93 m of rise. Nothing about the
  character's motion could have moved: at the yaw `frameBounds` picks for this scene the old and
  new bases differ by 7 × 10⁻⁷ in Z, which is the whole reason the defect survived.
- `./test.sh debug`, `release`, `asan`, `tsan` — **876 of 876** in each. The row lands nothing
  under `engine/`, so `tests-hosted` covers it only as a regression check; that is the token
  explained rather than left dangling, as the card asked.
- A validation run of the demo scene under a spinning camera with run, strafe and jump —
  **zero errors**, one benign `VK_LAYER_PATH` warning the context handles itself.
- The facing was checked visually as well as numerically: a captured frame of the character
  strafing shows it in profile, facing the way it travels, which is what says the rig's authored
  heading is +Z and `atan2(x, z)` is the right conversion. The `alongFacing` ratio is read off
  `Scene::worldTransform` rather than off the angle the game wrote, so it is a check on the
  rotation having reached the tree — which, given finding (1), is the check that matters.
