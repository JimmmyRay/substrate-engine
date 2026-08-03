---
id: C37
title: First-person, third-person and isometric cameras
arc: C
size: M
verification: golden, readback, scripted-input, tests-hosted, inspection
---

# C37 — First-person, third-person and isometric cameras

Afterwards `engine/scene/CameraControllers.h` holds three more `Camera` subclasses beside
`FlyCamera`, each with working default behaviour rather than a hook a game fills in, and the demo
drives its character through `ThirdPersonCamera` instead of the four-line follow rig it writes
today.

G18 made a camera controller something a game installs. This is the card that makes that worth
doing, because a split with one implementation on either side of it is a rename.

## Why these three, and why in the engine

The engine already anticipates two of them and serves neither. `Projection::Orthographic` exists
and has no controller that drives it — the only thing that sets it is `pixelPerfectCamera`, a
lambda inside `Engine::run` poking fields. A third-person rig is what the demo hand-writes, and
`G13` recorded that the movement basis it needs was wrong once by a sign, in a way visible at
every yaw except the one the test happened to use.

The bar each clears is the one C36 and G18 clear: a game cannot write these correctly without
first writing them incorrectly. Pitch clamping, the cursor-Y sign, when to grab the pointer, and
reading a follow target at the right point in the frame are all things that look right until they
are not.

What stays out is policy. None of these decides what the character does, which camera is active,
or which key switches them.

All three are installable into a `gfx::ViewTable` view through the `setCamera` G18 adds, which is
what a picture-in-picture inset or a second player's half of the screen wants. **The engine drives
none of them there** — a view camera is updated by the game, because there is one `InputMap` and
the engine has no notion of whose input a second view is showing. That is a sentence in the header,
not scope on this card; what a second view still costs is `C38`.

## `FirstPersonCamera` — look, and deliberately not walk

`distance = 0`, so the eye sits at the focus. **Continuous mouselook with no held button**:
`core::input::mouseGrab()` in `activate`, `mouseRelease()` in `deactivate`, one line each. This
is the case a declarative "this action grabs the pointer" flag could not have expressed, and is
the reason C36 made grabbing a verb.

Yaw and pitch come from `cursorDeltaX/Y` times a sensitivity, pitch clamped short of the poles.
It declares `Camera.Look` and **no movement actions at all**. Position comes from an optional
`scene::NodeId target` plus an eye-height offset, read from the tree during `update`; with no
target the game writes `focus` itself.

It does not walk, and that is the design rather than an omission. In a first-person game the
*character* moves under the solver and the camera follows it; a camera that also walked would
fight the controller and would need its own collision. The camera that flies is `FlyCamera`.

## `ThirdPersonCamera` — follow a target, orbit around it

Holds a `scene::NodeId target` and **reads its world transform itself, during `update`**. It has
to: `update` runs before `Game::frameUpdate` deliberately, so that a game reading `camera().yaw`
to resolve "forward" gets this frame's yaw rather than last frame's. A position pushed in by the
game would therefore always be one frame stale, and the ordering is not the thing to change.

`focus = targetWorld + heightOffset`, defaulting to the 1.2 m the demo currently uses. Drag-to-
orbit by default, which is what the demo does today and what keeps the feel unchanged; a
`continuousLook` flag switches to grabbed mouselook for games that want it. Scroll moves
`distance` between a min and a max.

**No spring arm and no collision.** Pulling the camera in when a wall comes between it and the
target needs physics queries and a policy about what to do when there is nowhere to go, which is
a separate decision and a separate card. A camera that clips through geometry is a stated gap
here rather than something discovered later.

This preserves the one-way coupling the demo depends on: the camera writes `focus`, the game
never writes `yaw`.

## `IsometricCamera` — orthographic, fixed pitch, panning focus

Sets `projectionMode = Orthographic` and drives `orthoHeight` from scroll rather than `distance`,
which is the difference an orthographic projection makes and the thing a controller written for
perspective gets wrong. Pitch is fixed, defaulting to 35.264° — the true isometric angle, where a
unit cube's three visible faces project to equal areas. Yaw snaps in 90° steps on
`Camera.RotateLeft` / `Camera.RotateRight`. Pan moves `focus` in the ground plane, on WASD by
default and on a drag that uses the C36 grab verbs.

It is the first real consumer of a projection mode the engine has carried since P3. It does
**not** replace `pixelPerfectCamera`: that is a 2D pixel-exact setup — one world unit per texel,
origin centred, yaw π — and formalising it into a `PixelPerfectCamera` is the obvious next card,
not this one. Conflating them would produce a camera that is bad at both.

## The demo

`DemoGame` replaces its follow rig with a `ThirdPersonCamera` targeting the player node, and
keeps a `FlyCamera` on a toggle. The toggle is the point: it exercises `setCamera`'s
deactivate-then-activate path, and therefore C36's retirement, against a real pair of control
schemes rather than a unit test. Which key it is bound to is the demo's business and belongs in
`AppActions`.

`PlayerActions::declare` should lose its five-key rebinding dance, because a demo that installs
`ThirdPersonCamera` never activates the flycam's WASD at the same time as the player's. If it
does not, that is worth reporting in the outcome — it is the clearest single measure of whether
this arc achieved anything.

## Verification

- `scripts/locomotion.sh` — eight arms, every number identical to the run before the follow rig
  moved into `ThirdPersonCamera`. This is the check that the extraction changed nothing, in the
  same shape C30 used, and the arms that press the wrong thing on purpose are what stop it
  passing under gravity alone.
- `scripts/golden.sh` — thirteen cases, byte-identical. No golden case drives a character, so a
  difference here means something moved that should not have.
- `scripts/readback.sh` — nine cases, bit-identical. `IsometricCamera` is the first controller to
  drive `Projection::Orthographic`, which is the projection the presentation path is written
  against.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. Hosted where it can be: the
  isometric yaw snap lands on multiples of π/2 across the seam, and the pitch clamp holds at both
  poles.
- Switching fly ↔ third-person and back leaves **no stale rows** in the rebind menu, and a
  binding the player moved before the switch is still moved after it.
- `IsometricCamera` renders an orthographic view: parallel edges stay parallel across the frame,
  and the depth buffer linearises through `depthLinear()` without a perspective divide artefact.
- Inspection of the first-person path: the pointer is grabbed for exactly as long as the camera
  is active, and is given back when a panel opens.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the camera section gains the three
controllers and what each owns; the movement-basis section is where the third-person coupling
argument now lives.
[architecture/rendering.md](../../architecture/rendering.md) — the orthographic projection now has
a caller, which the P3 material describes as hypothetical.
[architecture/limitations.md](../../architecture/limitations.md) — record the two stated gaps: no
spring arm or camera collision, and `pixelPerfectCamera` still being a lambda rather than a
`PixelPerfectCamera`.
[guides/making-a-game.md](../../guides/making-a-game.md) — the camera section, which currently
tells a game to write its own follow rig and shows the four lines.

## Outcome

**Three controllers landed, and the demo's five-key rebinding dance is finally gone — but only
because the card's premise had to be *made* true rather than relied on.**

`FirstPersonCamera` declares `Camera.Look` and nothing else, ships it **unbound** (so it looks
continuously) and becomes hold-to-look the moment anyone binds it; `activate` is declare plus
`mouseGrab()`, `deactivate` retire plus `mouseRelease()`. It forces `distance = 0`, turns from
`cursorDeltaX/Y * lookSensitivity` with pitch clamped to ±1.55334, and takes
`focus = targetWorld + 1.7 m` when given a tree. It does not walk, strafe, fly or collide, and
does not turn at all before activation.

`ThirdPersonCamera` declares only `Camera.Orbit`, on `Mouse.Left Mouse.Right` — deliberately the
same row name and default `FlyCamera` uses, so a rebind is shared. Drag-to-orbit by default with
a `continuousLook` flag; scroll multiplies `distance` by `zoomStep`, clamped to [1.5, 12], and
nowhere else, so `frameBounds` and `--camera` are not overruled. `focus = targetWorld + 1.2 m`,
read from the tree inside `update`. No spring arm, no collision, and it never writes `yaw`.

`IsometricCamera` declares `Camera.Forward/Back/Left/Right` (WASD + left stick),
`Camera.RotateLeft`/`RotateRight` (Q/E + bumpers) and `Camera.Pan`
(`Mouse.Middle Mouse.Right`). `projectionMode = Orthographic` in the constructor, pitch pinned at
−35.264° every update, scroll driving `orthoHeight` clamped to [1, 200] and never `distance`. The
yaw snap re-derives `lround(yaw / (π/2))` at the moment of a press and rebuilds from the integer,
so it is **exact across the seam** rather than accumulating.

**The card's premise was false as stated, and that is the finding.** It says "a demo installing
`ThirdPersonCamera` never activates the flycam's WASD at the same time as the player's" —
toggling to the flycam puts `Camera.Forward` on W while `Player.Forward` is also on W. Removing
the dance on that reasoning alone would have re-created a five-key collision (W/A/S/D/LeftShift,
plus the left stick and `Pad.RightBumper`). The demo now stops driving its character while
flying, so exactly one control scheme is live at a time and the arrow-key push has nothing left
to prevent. `PlayerActions::declare` declares six `Player.*` rows and nothing else, and
`DemoGame::init` contains **no `setDefaultBindings` at all**. That was the card's stated single
measure of whether this arc achieved anything.

**One `locomotion.sh` assertion had to change, and it is strictly stronger afterwards.** It
asserted `Camera.Forward=Up` in the shipped-bindings line — the dance's own output, which no
longer exists. It now asserts the player rows unchanged and unmarked **plus**
`Shipped camera (follow): Camera.Orbit=Mouse.Left Mouse.Right` exactly, so the follow camera's
one row is the only live camera row and an arrow-key row reappearing fails the suite.

Verification, all of it. **Locomotion: byte-identical**, baseline captured before any demo edit
and diffed twice, once mid-way and once on the final binary — all nine arms, including
`walk-run-jump` at `idle>walk>run>walk>idle>jump>fall>land>idle`, 8 changes, 8.21 m, rise 0.93,
along 1.00, turned 1.57, and `pose drift 0.00 m on every arm`. **Golden 13 of 13 byte-identical**
by `cmp` against the baselines rather than the suite's tolerance-2 verdict; no device-lost.
**Readback 9 of 9 bit-identical**, lit silhouette exact, resize soak clean. `./test.sh debug` and
`./test.sh asan` both **1051 tests, 106 suites, all pass** (1039 + 12 new). **Validation 0
errors**, headless 240 frames and windowed 180, both scripting live camera switches, with the
layer confirmed loaded and zero `conflicts()` warnings at startup.

Both checks the card insisted be direct were direct. **No stale rows across a switch**, measured
in the running binary by the same `actionCount()`/`actionLive()` walk `BindingMenu` uses:
`follow: Camera.Orbit` → `fly: Camera.Orbit Camera.Forward … Camera.Slow` → `follow:
Camera.Orbit`, and with a config rebinding `Camera.Orbit` to `Mouse.Middle` every listing across
the round trip reads `Camera.Orbit=Mouse.Middle*` — the player's edit survives. **Orthographic
renders orthographic**, measured on a real frame rather than inferred: fitting the ground plane's
silhouette edges over 888 scanlines gives slopes **−0.00001 and +0.00001 px/px** with width 827 at
the top row and 827 at the bottom, against **−0.385 / +0.385** and 353 → 847 for the perspective
camera at the same pose. The hosted counterpart asserts the perspective case *does* skew, so the
check can fail.

**Deferred, with a destination.** Declare-on-activate makes a config rebind of a row belonging to
an *uninstalled* camera unreachable: the flycam is not installed when `applyBindings` runs, so
`Config binds unknown action "Camera.Forward"; ignored` and the next `saveBindings` loses it.
`Camera.Orbit` is safe only because both controllers declare it — luck, not design. This is a
different cause from the retirement bug closed earlier (there the row exists but is dead and
`findDeclared` reaches it; here it has never been declared at all), so it is
[bug-a-rebind-for-a-camera-that-is-not-installed-yet-is-dropped](../backlog/bug-a-rebind-for-a-camera-that-is-not-installed-yet-is-dropped.md).

Two smaller decisions recorded because the card left them open. `Camera.Look`'s semantics are not
the card's — it names the action and says "no held button" without saying what the row does; it
ships unbound and becomes hold-to-look if bound, rather than declaring a row nothing reads. And
the demo installs `ThirdPersonCamera` whenever a player character exists but gives it a follow
target only when it built its own world, which keeps `physics.gltf` — the one golden scene with
an authored character — pixel-identical while leaving WASD walking working there.

Executed alongside another session's uncommitted C35 and collider work; none of their files was
touched.

Reference updated: `systems.md`'s camera section gains the four controllers, what each
deliberately does not do, the stated absence of a spring arm, and the one-scheme-at-a-time
correction to the card's premise.
