---
id: G18
title: The camera is data, and its controller is opt-in
arc: G
size: M
verification: golden, readback, scripted-input, tests-hosted, validation, scaffold
---

# G18 — The camera is data, and its controller is opt-in

Afterwards `scene::Camera` is a pose and a projection with three empty-defaulted virtuals, the
free-fly controller is a subclass a game installs by name, and a game that installs none has no
`Camera.*` rows in its binding menu at all. `Engine` holds a `Camera*` that is never null,
because the base type is also the null camera.

**This card should change no pixel.** That is why it is separate from C37: the golden set can
prove a pure structural change, and cannot prove one bundled with new behaviour.

## What is wrong now

`scene::Camera` is one specific camera — its own first line is *"Orbit camera with WASD fly
controls"* — and the engine installs it unconditionally. `declareActions` and `update` have
exactly one call site each, both in `Engine.cpp`, and neither is optional. Every game therefore
gets nine `Camera.*` actions including W, A, S and D, whether or not it wants a flycam, and they
appear in the rebind menu of a game that has none. The pointer grab is wired to
`cameraState.orbitAction()`, so a drag captures the cursor for a camera the game may not be
using.

The demo already fights this twice. `PlayerActions::declare` moves five camera actions onto arrow
keys before it can take WASD, and warns that both halves must happen in one call and in that
order or the two tables collide. The follow rig then overwrites `focus` *after* `update()` has
run, which works only because the game gets the last word.

Two things in the tree say the split is already real and merely unspelled:

- **`gfx::ViewTable` holds a `scene::Camera` per view** for mirrors, monitors and minimaps, and
  calls neither `update()` nor `declareActions()` on them. A view camera is already the data half
  with an inert controller attached — it is just not expressible as a type, which is why the main
  camera cannot opt out of what view cameras never opt into.
- **A second camera kind already exists as a lambda.** `pixelPerfectCamera` inside `Engine::run`
  sets an orthographic, origin-centred, yaw-π pose and its comment says it is what a pixel-exact
  2D game wants — while `cameraState.update()` runs underneath, free to wreck the pose it
  computed. It survives because the run modes that use it are non-interactive, and a real 2D game
  would not.

## The shape

```cpp
class Camera {
  public:
    virtual ~Camera() = default;
    virtual void activate(core::input::InputMap&) {}                 // declare my bindings
    virtual void deactivate(core::input::InputMap&) {}               // retire them
    virtual void update(const core::input::InputMap&, float dt) {}   // base: takes no input
    /* pose, projection mode, view(), projection(), depthLinear(), frameBounds() */
};
```

Three empty-defaulted virtuals, which is the shape `Game` already has and the same argument for
it: one indirection at an edge. **The base doubles as the null camera** — "looks at the scene and
takes no input" is simultaneously its definition and what a null object has to be, so there is no
`NullCamera` to write and `ViewTable` keeps holding it by value with nothing ever sliced into it.

`Engine::setCamera(Camera*)` is the only door, and `nullptr` installs the engine's own base
instance:

```cpp
void Engine::setCamera(Camera* c) {
    Camera* next = c != nullptr ? c : &nullCamera;
    if (next == activeCamera) return;
    activeCamera->deactivate(inputMap);
    activeCamera = next;
    activeCamera->activate(inputMap);
}
Camera& Engine::camera() { return *activeCamera; }
```

`camera()` keeps its signature and can never be null, so `drawFrame`, the audio listener,
`--camera` reproduction and `frameBounds` gain no branch. Deactivate and activate cannot be
forgotten or mis-sequenced because there is nowhere else to change the camera. **The engine holds
a non-owning pointer and never deletes** — a game destroying an installed camera must
`setCamera(nullptr)` first, and that belongs on the method as a trap rather than in this card.

**Bindings arrive on activation rather than construction**, which is what lets a game hold every
camera it will ever use and pay input surface only for the active one. Constructor-declared
bindings would give two held cameras duelling `Camera.Forward` sets and `InputMap::conflicts()`
would be right to say so. `deactivate` retires rather than clears — C36 is why this card is
blocked on it, because clearing leaves a row a player can still see and rebind in a menu, for a
camera that is not running.

`Camera::orbitAction()` and `declareActions()` are removed rather than moved: the grab loop asks
`core::input::mouseGrabbed()`, and the flycam declares its own actions when it activates.

## A view holds a camera, not a copy of one

`ViewTable::Entry` holds `scene::Camera camera` **by value**, and `create` copy-assigns a fresh
one into it. Give the base a vtable and `entry.camera = someFlyCamera;` still compiles — base
copy-assignment — and drops the derived half, so `update()` stays the base's no-op. A view could
then only ever hold the base type, and the failure would be a camera that does nothing with no
diagnostic. **This card creates that trap, so this card closes it.**

```cpp
struct Entry {
    scene::Camera camera;                ///< this slot's own pose, driven when nothing is installed
    scene::Camera* installed = nullptr;  ///< non-owning; a game's controller drives this view instead
    /* image, generation, live */
    const scene::Camera& active() const { return installed != nullptr ? *installed : camera; }
};

scene::Camera* camera(ViewId id);              // installed if there is one, else this slot's own
void setCamera(ViewId id, scene::Camera* c);   // nullptr goes back to this slot's own
```

The renderer reads `views->at(s).active()`. `camera(ViewId)` keeps its exact signature, so the
nine hosted `ViewTable` tests and the pattern its header documents are untouched, and the
by-value slot stays because it is what lets a mirror be four lines of pose writing with no
camera object to own.

**The default is per entry rather than one shared instance**, which a single engine-wide null
camera would get wrong: writing `focus` through one uninstalled view's handle would move every
other one. It also fixes something already broken — `camera(id)` returns `&entries[s].camera`
today, and a later `create()` can reallocate the vector out from under it. A camera the game
owns survives that.

**The engine calls neither `activate` nor `update` on a view camera**, and that is the trap
worth a line on `setCamera`. There is one `InputMap`; two cameras declaring `Camera.Forward` is
the collision `activate`/`deactivate` exists to prevent. A game that wants a view camera to read
input updates it in `frameUpdate` with whatever map it likes — which is also the only route a
second player's input could take.

## FlyCamera stays in the engine

The controller moves to `scene::FlyCamera` in `engine/scene/CameraControllers.h` with its
behaviour unchanged — same nine actions, same defaults, orbit-drag turning the camera where it
stands, scroll dollying, WASD translating the focus.

It belongs in `engine/` and is the clearest of the four cases, because it is the only one that
already exists as debugged engine code. Every project wants free-fly while developing, the
engine's own workflow depends on it — `./run.sh` with no game opens Sponza and flying is how you
look at it — and `--camera` reproduction assumes somebody flew somewhere first. What moving it to
`game/` would cost is the list of things in `Camera::update` that are only obvious once they have
been wrong: the frame the orbit button went down being skipped, because the pointer's travel while
the button was up is a position change and applying it snaps the view; the eye being held while
the focus swings, because the other reading of the same four numbers feels like rotating the scene
and made both axes seem inverted when only one was; the yaw negation from `cross(forward, up)` and
the pitch negation from the cursor's Y growing downward; and E and Q rather than Space, because
`Camera.Up` on Space flew the camera *and* jumped in a scene whose own HUD said "space=jump".

The objection that would apply — that the engine has WASD opinions again — does not survive
declare-on-activate. `FlyCamera` declares nothing unless a game constructs and activates it. The
defect was unconditional installation, not existence.

**The demo installs one in `init`**, which is what makes this card behaviour-neutral, and
`scripts/template/game/Game.cpp.in` gains the same two lines so a scaffolded game is not born
unable to look around. No config flag: the engine defaults to the null camera and a game that
wants fly controls asks for them.

## What this card is expected to be wrong about

`Camera::frameBounds` currently sets the pose, the near plane **and** `moveSpeed`. The first two
are the base's and the third is the flycam's, and the split has not been traced through every
caller. The expectation is that the flycam derives its speed from `distance` and the base carries
no `moveSpeed` at all; if a caller wants the old field, that is the thing this card discovers.

## Verification

- `scripts/golden.sh` — thirteen cases, byte-identical. **This is the card's central claim**, not
  a formality: the demo installing a `FlyCamera` must reproduce today's behaviour exactly.
- `scripts/readback.sh` — nine cases, bit-identical. Named rather than inferred because
  `pixelPerfectCamera` writes the camera's fields directly and this card moves what it writes to.
- `scripts/locomotion.sh` — eight arms. The card touches `game/demo/`, which is the standing
  reason to run it whether or not the row looks like it could move a character.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on.
- `./new_game.sh <name>` builds and runs without touching anything under `engine/`, and the
  scaffolded game flies.
- `--camera` reproduction lands the same pose it does today, checked on two of the golden poses.
- A run with `setCamera(nullptr)` takes no camera input: keys and drags move nothing.
- A game that installs no camera lists **no** `Camera.*` rows in its binding menu. This is the
  defect the card exists to fix and is checked directly rather than inferred.
- Hosted: a `FlyCamera` installed into a view comes back out of `camera(id)` as itself, and the
  view records through **its** pose rather than a sliced base's. `ViewTable` pulls in neither
  Vulkan nor a window, so this reaches the whole seam without a device.
- A view with nothing installed renders its slot's own pose exactly as it does today, which is
  what `golden`'s mirror cases already assert.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the camera, and the movement-basis
section that describes which way the coupling runs.
[architecture/principles.md](../../architecture/principles.md) — two sentences are false as
written and this card makes one of them worse before making it better: *"Not one key is bound and
not one panel is drawn in `engine/`"* is already contradicted by `Camera::declareActions`,
`BindingMenu` and `Ui.Click`, and `making-a-game.md` already carries the accurate version. The
base-class count in the layout section also moves from two to three; the prohibition names
`engine/gfx/` and the rationale is indirection over Vulkan, neither of which reaches a camera
controller, but the sentence cannot be left contradicting the tree.
[guides/making-a-game.md](../../guides/making-a-game.md) — "The camera is the game's, and needs no
engine method" is the section this changes, and the call-site sketch it contains.

## Outcome

**Landed behaviour-neutral, and it took two discoveries the card did not have to get there.**

`scene::Camera` is now a pose, a projection and `frameBounds` plus three empty-defaulted virtuals
and a virtual dtor; `declareActions`, `orbitAction()`, the `Actions` struct, the `update` body,
`moveSpeed`, `moveSpeedScale`, `orbitSensitivity` and `zoomStep` are gone from it.
`scene::FlyCamera` is new in `engine/scene/CameraControllers.{h,cpp}` (hosted), with the nine
actions unchanged, `deactivate` **retiring** them and calling `mouseRelease()`, and `update`
making the `mouseGrab()`/`mouseRelease()` ask itself. `ViewTable::Entry` gained `installed` and
`active()`; `camera(ViewId)` keeps its exact signature and the nine original hosted tests are
untouched. `Engine` holds `nullCamera` + `activeCamera`, `setCamera` is the card's body verbatim
with the non-owning trap on it, and the cursor-capture block moved after `camera().update()` and
now reads only `mouseGrabbed()`.

**Discovery one, which the card anticipated and undercounted.** It scoped the split to
`moveSpeed`. That field turned out to be private and read only by the old `Camera::update`, so
removing it broke no caller — but **three settings rows sat beside it**: `Engine` was writing
`moveSpeedScale`, `orbitSensitivity` and `zoomStep` from `camera.*`, and all three are the
*controller's*. With only a `Camera&` in hand the engine can no longer apply them, which is why a
game's opt-in is `applySettings` **and** `setCamera` rather than `setCamera` alone.
`camera.fovDegrees` stays the engine's. `FlyCamera::update` now derives
`max(distance, 0.25) * moveSpeedScale`, so speed tracks the dolly — 7.44 m/s on Sponza against
the old `max(radius*0.25, 0.25)` of about 4.66, a real behaviour change that nothing in golden,
readback or locomotion measures and that the card blessed.

**Discovery two, which the card does not mention and which would have broken its central claim.**
`setCamera` as specified copies no pose, and the engine framed the camera during `Engine::init`
while a game installs its camera in `Game::init` — so the demo's flycam would have started at
focus 0, distance 5, and **every golden would have moved.** Framing, fov and `--camera` are now
`Engine::applyCameraConfig()`, called from `run()` immediately before `applyBindings()`, for the
same reason `applyBindings` is there. The consequence to know: `Game::init` can no longer read a
framed camera, and nothing in the tree did.

**Both things the card said the tree does badly are fixed.** The demo's `PlayerActions::declare`
no longer performs the arrow-key dance — the five `setDefaultBindings` calls moved into
`DemoGame::init`, between `setCamera(&flyCamera)` (which is what makes `Camera.Forward` exist)
and `playerActions.declare(...)`, so the ordering constraint is visible at the three call sites it
actually constrains and `declare` only declares `Player.*`, which is what its name says. The
resulting table is byte-identical, no `*`. And `pixelPerfectCamera` now calls `setCamera(nullptr)`
before writing its eight fields, so nothing runs `update()` underneath the pose it computes.

Verification, all of it:

| Check | Result |
|---|---|
| `scripts/golden.sh check release` | **13 of 13, byte-identical** by `cmp` against the baselines. Run twice; identical both times. No device-lost. |
| `scripts/readback.sh` | **9 of 9 bit-identical**, lit silhouette exact, resize soak clean |
| `scripts/locomotion.sh` | **9 of 9 arms** — the script has nine, not the eight this card claims |
| `./test.sh debug` / `./test.sh asan` | 1039 tests, 106 suites, all pass, both |
| Validation | **0 errors** across demo headless 240 frames, demo windowed 180, scaffold 90 |
| `./new_game.sh` | Built and ran with no edit under `engine/`, and **flies**: a scripted `Camera.Forward` held 220 frames moved the focus 3.16 m along `forward()`. Both scaffolds deleted afterwards. |
| `--camera` on two poses | `-0.97,4.20,-0.61,90.0,-2.9,14.89` → eye `(-15.84, 4.95, -0.61)`; `2.5,6.0,3.0,-35.0,8.0,9.0` → eye `(7.61, 4.75, -4.30)`. Both match the pose arithmetic to the printed digit, repeat byte-identically, and differ from the default-framing golden — so nothing overwrites them. |

The two checks the card insisted be direct rather than inferred were both direct. **A game that
installs no camera lists no `Camera.*` rows**: a scaffold with the two lines removed logs its live
action table as `Menu.Bindings Ui.Click App.Quit App.Panel` — zero camera rows — against
`Menu.Bindings Ui.Click Camera.Forward … Camera.Orbit App.Quit App.Panel` with them, and
`BindingMenu` walks that same `actionCount()`/`actionLive()` pair. **`setCamera(nullptr)` takes no
input**: with `Camera.Forward` live, bound and scripted down for 220 frames, focus stayed at
`-0.4842 2.1002 -0.3095`, identical to an untouched run; drags are covered hosted by
`CameraTest.TheBaseTypeIsAlsoTheNullCameraAndTakesNoInput`. The view seam is covered by
`ViewTableTest.AnInstalledCameraComesBackOutAsItselfAndIsWhatTheViewRecords` — `camera(id)`
returns `&fly`, the `dynamic_cast` succeeds, and `at(s).active().focus` is the installed camera's
rather than a sliced base's.

**Executed alongside another session's uncommitted C35 work in this checkout.** This card needed
exactly one expression in a file they hold ~400 lines in — `Renderer.cpp`,
`views->at(s).camera` → `views->at(s).active()` — and that one line was staged on its own, from a
patch generated against `HEAD`, so their work stayed unstaged and untouched. Nothing else of
theirs was modified.

Reference updated, including two sentences this card was told were false. `principles.md`: the
base-class count moves two → three with `scene::Camera`'s own argument beside `Game`'s, and *"Not
one key is bound and not one panel is drawn in `engine/`"* is struck — it was never true
(`BindingMenu`, `Ui.Click`), and the rule that *is* true is now stated: **nothing in `engine/`
binds a key unless a game asked for it**, which G18 is what made literal. `systems.md` gains "The
camera is data; the controller is a game's opt-in" before the movement-basis section.
`making-a-game.md`'s camera section leads with the two-line opt-in and the four traps.
`architecture/README.md` and the root `CLAUDE.md` both carry the new count.
