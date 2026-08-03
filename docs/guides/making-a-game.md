# Making a game

This guide is written one stage at a time, alongside the arc in
the G arc, because a guide written after six stages
is a guide written from memory.

**The arc is complete.** The module boundary and the loop (G1), a scaffold for a new game
(G1b), settings by name with a panel generated from the table (G2), a scene tree with
parents, reparenting and attachments (G3), meshes and materials made in code (G4), a game's
own shading (G5, G5b), node inspection (G6), contact events and one-shot audio (G7), the
bindings a control scheme needs (G8) -- and **G9**, a demo that uses all of it at once.

**Where to read the worked example**: `game/demo/DemoWorld.cpp` builds four braziers, a
crate stack, barrels, a ramp, a sliding kinematic platform and two hundred and forty urns
out of nothing but the calls this guide describes. What was *awkward* about writing it is
in [limitations.md, "The game API"](../architecture/limitations.md#the-game-api), and that
list is the point of the row rather than a footnote to it.

---

## The shape of it

```
engine/             the engine. A static library. Knows of no game.
  shaders/          the engine's GLSL
  assets/           what the golden suite pins. Gitignored.
game/
  demo/             the demo -- what main.cpp used to be
  <name>/           yours
    CMakeLists.txt  one line
    shaders/        optional. Yours, searched before the engine's
    assets/         optional. Yours
```

Both `shaders/` and `assets/` under a game are conventions the build looks for, not
requirements: a game with neither builds and runs.

## Naming an asset

Write `res:/` and the name, and let the lookup find the file:

```jsonc
// substrate.json
"scene": { "path": "res:/showcase.gltf" }
```

```cpp
gltfScene.load(ctx, up, Resources("res:/props/barrel.gltf"));
```

Your `assets/` is searched first, then the engine's, so `res:/Sponza/glTF/Sponza.gltf`
finds the engine's copy and a `Sponza/` of your own would win instead — the same
game-before-engine order your `shaders/` gets. A path *without* the scheme is used as
written, so nothing you already have stops working.

You do not need `res:/` inside a glTF. Its `uri` entries are relative to the file, which
is what the format says, and resolving the file is enough to anchor them.

## Starting one

```bash
./setup.sh                 # submodules, a dependency check, the sample assets
./new_game.sh mygame       # game/mygame/ -- a Game subclass, a CMakeLists, a README
./build_game.sh mygame
./run.sh                   # the build directory remembers which game it holds
```

What lands is a game that names itself, takes two keys and draws one generated settings
panel. It loads no scene, which is a real state rather than a placeholder — the engine
always starts with an empty world, and a game fills it in `init`.

**An empty world is still a scene.** The engine builds the GPU side of one — the shared
vertex, index and material buffers, the texture array and the descriptor set every pipeline
binds — with no document behind it. That is what makes it a configuration rather than a gap:
the renderer brings up and draws an empty frame, and `e.createMesh(...)` puts geometry into
those buffers exactly as a loaded file's goes into them. A game that authors everything in
code never names a scene at all.

```bash
./build.sh                 # the engine and the unit suite. No runnable binary.
./build_game.sh demo       # the engine, plus one game -> build/debug/demo
./run.sh demo              # runs it -- and builds it first if it has to
./run.sh                   # no game named: game/viewer on the engine's own test scene
```

`./build.sh` deliberately produces nothing you can run. The engine has to build, test and
sanitize with nothing under `game/` in the tree, so a dependency leaking from a game into
`engine/` is a link error rather than something a reviewer has to notice.

Which game a build directory holds is a property of the **build directory**:
`build_game.sh` writes it into the CMake cache, and `run.sh`, `scripts/golden.sh` and
`scripts/baseline.py` read it back. That is why none of them grew a `--game` flag.
`run.sh` takes a game *name* as an ordinary argument rather than a flag, and where the
name and the directory disagree it reconfigures instead of refusing — so the two ways of
saying which game you mean cannot leave you stuck.

---

## The smallest game that runs

Two files.

```cmake
# game/mygame/CMakeLists.txt
substrate_game(mygame MyGame.cpp)
```

```cpp
// game/mygame/MyGame.cpp
#include "Engine.h"
#include "Entry.h"
#include "Game.h"

class MyGame : public Game {
  public:
    void configure(GameSetup& setup, settings::Settings&) override {
        setup.name = "My Game";
    }

    void init(Engine& e) override {
        quit = e.input().declare("App.Quit", "Escape");

        const scene::NodeId world = e.scene().create("world");
        e.scene().add<scene::Model>(world, {core::Resources("res:/mygame.glb")});
    }

    void frameUpdate(Engine& e, float /*dt*/) override {
        if (e.input().pressed(quit)) e.requestQuit();
    }

  private:
    input::ActionId quit = input::kInvalidAction;
};

SUBSTRATE_GAME(MyGame)
```

```bash
./build_game.sh mygame && ./run.sh
```

`SUBSTRATE_GAME(T)` defines the one function the engine's `main()` calls. There is no
registry, no factory interface and no static initialiser appending to a list: one game per
executable is the whole requirement, and a link error naming `substrateCreateGame` is a
better diagnostic for "you forgot the macro" than an empty list discovered at runtime.

---

## The seven methods

Every one has an empty default, so a game overrides what it needs and nothing else.

| Method | When | What it is for |
|---|---|---|
| `declareSettings(Settings&)` | Once, before the **config file** is read | Add this game's own settings rows |
| `configure(GameSetup&, Settings&)` | Once, before **anything** exists | Name the scene and everything else the game authored |
| `init(Engine&)` | Once, before the first frame | Declare actions, place lights, set up state |
| `frameUpdate(Engine&, dt)` | Once per rendered frame | Input, and anything that happens at the frame rate |
| `fixedUpdate(Engine&, step)` | Once per simulation step, **before** the movers | Game intent: what should be true when physics runs |
| `drawUi(Engine&, ui::Context&)` | Once per frame, while `uiVisible()` | The panel |
| `shutdown(Engine&)` | Once, after the last frame | Anything the destructor cannot do |

The loop `Engine::run` runs, which is also the hand-driven form if you turn
`SUBSTRATE_ENTRY_POINT` off and write your own `main()`:

```cpp
while (engine.beginFrame()) {                     // poll, input, camera, the accumulator
    game.frameUpdate(engine, engine.dt());
    if (engine.uiVisible()) game.drawUi(engine, engine.ui());
    while (engine.consumeStep()) {
        game.fixedUpdate(engine, engine.step());  // game intent, then simulate
        engine.simulate(engine.step());           // animation, particles, physics, audio
    }
    engine.endFrame();                            // writeback, record, submit, present
}
```

**`frameUpdate` before the steps, `fixedUpdate` inside them.** Read input once per frame,
not once per step: a key held across two steps in one frame is one press, and reading
`pressed()` twice would fire a jump twice. Anything that must be true *per step* -- a
state machine parameter, a force -- goes in `fixedUpdate`.

---

## `declareSettings`: the game's own rows

Difficulty, mouse sensitivity, subtitle size, colourblind mode. Every one of those is a
preference by the test in the next section, and none of them belongs to the engine -- so a
game adds them to the same table the engine's rows live in, and gets every consumer of that
table for free.

```cpp
class MyGame : public Game {
    // Keep the handles. They carry the row's type exactly as the engine's
    // `options::render::ssao` does; only the *id* is assigned at run time, so
    // `s.set(difficulty, true)` on an integer row still does not compile.
    settings::Setting<int> difficulty{settings::Id::None};
    settings::Setting<float> subtitleSize{settings::Id::None};

    void declareSettings(settings::Settings& s) override {
        difficulty   = s.declare("mygame.difficulty", 2, "Difficulty", 1, 3);
        subtitleSize = s.declare("mygame.subtitleSize", 18.0f, "Subtitle size", 8.0f, 48.0f);
    }
};
```

That is the whole of it. Afterwards the rows are ordinary:

```jsonc
{ "mygame": { "difficulty": 3 } }                 // substrate.json, loaded and saved
```
```bash
./run.sh mygame -- --set mygame.difficulty=3      # every row reaches the command line
./run.sh mygame -- --dump-settings                # and every row is in the dump
./run.sh mygame -- --write-default-config out.json  # with a "mygame" section
```
```cpp
ui::drawSettings(ui, e.settingsTable(), "mygame"); // and a generated panel, in drawUi
const int level = e.settingsTable().get(difficulty);
```

Three things to know, and each of them is a rule rather than a caution.

**It is not `configure`, and the ordering is the reason.** `substrate.json` is walked
key-by-key so that a key nothing claims produces a message -- which means a row declared
*after* the file was read has already had its key reported as the user's typo, and keeps its
built-in whatever the user wrote. So declaring comes first, and the schema freezes the moment
`declareSettings` returns: this method may only **add** rows, `configure` may only **write**
them, and a `declare` from `configure` is refused with a message saying so. Nothing else is
available here -- no engine, no `GameSetup`, no file, no flags -- on purpose: a schema that
differed between runs is one the file, the dump and the panel cannot all assume.

**Name your module after your game.** A game owns every module the engine does not name, and
none that it does: `mygame.difficulty` is yours, `render.myThing` is refused. The module is
the unit rather than the key because the key *is* the JSON path -- a `render.` key you added
would be indistinguishable from an engine row in the file and in the panel, and an engine
release adding that key later would take over a value your user wrote for you. The refusal
names what would have been legal, and the handle it hands back reads and writes nothing, which
is why the members above are initialised to `Id::None`.

**A row you remove does not delete the user's answer to it.** A key in the file that nothing
declares this run is refused -- the value is not applied and the run continues -- and the key
is *kept*: the save merges into the file it read. So dropping a setting in one version and
restoring it in the next costs nobody anything, and two games sharing one `substrate.json`
do not erase each other's sections.

[tooling.md](../architecture/tooling.md#a-game-declares-its-own-rows) is the full refusal
ladder.

---

## `configure`: what the game authored

**Grouped by the subsystem each row configures** (D21): `setup.look`, `setup.present`,
`setup.sim`, `setup.audio`, `setup.tools`, with `name`, `characters` and `decals` at the top
because they are what the program *is* rather than how one subsystem is tuned. Nothing was
renamed; `setup.occlusion` -- which was audio and did not say so -- is
`setup.audio.occlusion.enabled`.

`configure` runs after the config file and **before the command line**, and before the first
subsystem exists -- no window, no device, no scene. Neither half of that is arbitrary. The
engine needs the scene path in order to load a scene, gravity in order to build a world, and
the mix graph in order to open an audio device, and all three happen before `init` could
possibly run; and sitting between the file and the flags is what makes the precedence below
hold without a rule anywhere. The cost is worth knowing: a setting *read* from here reflects
the user's file and not the flags.

```cpp
void MyGame::configure(GameSetup& setup, settings::Settings& s) override {
    setup.name = "My Game";               // the window title

    // The sun is a light in a list, not three fields of its own (D20) -- so a game that
    // wants a second light says so the same way. The first *directional* light becomes the
    // sun, whether it came from here or from the scene; a second is dropped and reported,
    // because there is one cascade set to shadow it against.
    setup.look.lights = {gfx::makeDirectionalLight({-0.35f, 0.85f, 0.4f}, {1.0f, 0.96f, 0.88f}, 3.0f)};
    setup.look.exposure = 1.2f;
    setup.look.tonemap = gfx::TonemapOperator::Aces;   // the curve that balances it

    // Shadow bias, in world units, tuned against this scene's geometry: too little is
    // acne and too much is peter-panning, and where the line falls is a property of how
    // thin your walls are.
    setup.look.shadowDepthBias = 0.02f;
    setup.look.shadowNormalBias = 0.04f;

    setup.sim.gravity = {0.0f, -9.81f, 0.0f};

    // No budgets. Every pool sizes itself and grows past what a game asks for (C40), so
    // there is nothing to state and nothing to keep in step with the content.

    // Optional: a setting this game ships differently from the engine's default.
    // `setDefault` is "unless the user said otherwise" -- their substrate.json wins.
    (void)s.setDefault(options::window::vsync, true);
}
```

**Everything in `GameSetup` used to be a key in `substrate.json`, and none of it should
have been.** The test is *is this a property of the person running the program, or of the
program?* A scene, a sun, a mix graph, an exposure, a tonemap curve and a capacity budget
are the second: there is no user for whom a different value is correct, and a user who edits
one has not configured anything, they have broken it. So they are C++ that ships with the
game. The tonemap and the four budgets were rows until D14 and are the newest arrivals; a
one-off still reaches the curve through `--tonemap <name>`, the way `--scene` reaches the
scene.

What stays in `substrate.json` is settings -- the window size, MSAA, the quality toggles,
volumes, sensitivities, key bindings. What is in neither is a **developer control**: the
validation layers, the profiler, the recorder, the log, the debug draws and the two debug
windows are named flags with no key at all, and `--help` is their list. A game reaches the
settings through `Settings`, by a typed handle:

```cpp
const bool ssao = e.settingsTable().get(options::render::ssao);
```

A game may write one too, and there are two doors because a game holds two different
intentions about the same row:

| Call | Means | Wins over |
|---|---|---|
| `s.setDefault(handle, value)` | my answer **unless you said otherwise** | the built-in only -- the user's file stands |
| `s.set(handle, value)` | my answer **regardless** | the built-in and the user's file |

`setDefault` is the one to reach for, and the reason is the rule the whole settings arc is
built on: a setting is a property of the person running the program, so a game shipping its
own answer for vsync or MSAA has to lose to a user who wrote one down. `set` keeps the other
meaning for the cases that genuinely want it -- a fixed-resolution game turning TAA off, a
benchmark harness pinning a value.

Both land as `Source::Game` and **both lose to the command line**, which is applied after
`configure` returns. `--dump-settings` names `game` in the source column when either
happens, so an opinion a game holds is visible rather than mysterious.

**A scene named on the command line loads into the world before `init` runs**, which is how
`./run.sh mygame -- other.gltf` opens one asset without the game's own content on top of it.

### Presentation: a fixed resolution, if the game has one

Three fields, and a game that wants none of them writes nothing:

```cpp
setup.present.virtualResolution = {320, 180};   // omit for native
setup.present.uiInsideVirtual   = true;         // the HUD is magnified with the world
setup.present.pixelExact        = true;         // no TAA, no jitter, no curve, nearest sampling
```

The engine renders into a 320x180 target and presents it at the largest **integer** scale
that fits the window, centred, with the leftover as black bars. There is no fractional
scale and there is no stretch mode: a nearest blit at 3.125x doubles every 33rd column of
texels and the pattern moves as the window is dragged, which is the artefact a fixed
resolution is chosen to avoid. A window too small to hold the target even at 1x crops to
the middle rather than shrinking, for the same reason.

`present.uiInsideVirtual` decides whether the HUD is part of that grid. True and an 8-pixel font
stays an 8-pixel font, magnified with everything else; false and the UI draws after the
scale, at the window's own resolution, over a letterboxed image. Either way `framebufferWidth()`
reports the surface the UI was laid out against and the cursor arrives in the same space, so
a hit test needs no arithmetic from the game.

`present.pixelExact` is one switch because it is one decision -- see
[rendering.md](../architecture/rendering.md#pixelexact-is-one-switch-because-it-is-one-decision).
It is neither implied by a virtual resolution nor implies one: a 3D game may render small
for cost and still want a temporal path, and a 2D game at native resolution still wants its
texels left alone.

`--virtual-resolution WxH` overrides the first of the three per invocation.

---

## Reaching the engine

`Engine` hands out references to concrete types and wraps none of them:

```cpp
e.renderer().exposure = 1.4f;       // gfx::Renderer&
e.camera().frameBounds(lo, hi);     // Camera&
e.input().pressed(action);          // input::InputMap&
e.physics().characterSpeed(id);     // PhysicsWorld&
e.gltfScene().boundsMin;            // GltfScene&
e.config().render.debugView;        // Config&
```

There is no `e.setExposure()`, and there will not be one. The rule the engine is held to:

> **`Engine` gets no method that merely forwards to one subsystem.**

The exceptions earn it by spanning two, or by being the only alternative to handing a game
the GLFW window: `e.dumpProfile()` (CPU profiler *and* GPU timings) and `e.requestQuit()`.

`gltfScene()` and `scene()` are two different things and the spelling is what keeps them
apart: `gltfScene()` is the **file** -- meshes, materials, images, rigs -- and `scene()` is
the **tree**, which is where things are and what follows what.

---

## Actions: the engine binds no keys

Every key in Substrate is a **named action with a default binding**, and the config can
rebind any of them. The rule for who declares what is one line:

> The thing that consumes an action is what names it.

So the camera, the binding menu and the UI's own click are declared by the *engine* --
they are its. Everything else is yours:

```cpp
void MyGame::init(Engine& e) {
    jump = e.input().declare("Player.Jump", "Space");
    fire = e.input().declare("Player.Fire", "Mouse.Left");
}
```

`Engine::run` applies `input.bindings` from `substrate.json` immediately *after*
`Game::init` returns, and never before: a config can only rebind an action that exists.
Declare in `init`, and every action you declared is one a player can rebind and one the
binding menu will list.

**The engine draws no panel of its own and binds no key of its own beyond those three.**
Debug capabilities are exposed as calls, and the game decides how -- or whether -- to
reach them:

```cpp
if (in.pressed(act.screenshot)) e.renderer().requestCapture("shot.png");
if (in.pressed(act.profile))    e.dumpProfile();
if (in.pressed(act.wireframe))  e.physicsDebugDraw = !e.physicsDebugDraw;
```

That keeps exactly one owner of the keyboard, which is what the duplicate `case` hidden
in the old `onKey` switch was about: two labels for one key is a bug a table makes visible
and a switch hides.

**One carve-out.** The engine acts on its own *config*. `--capture`, `--frames`,
`benchmark.captureFrame` and the resize drive are run modes rather than bindings, and
`scripts/golden.sh` and `scripts/baseline.py` depend on them working with no game
involvement at all.

### Picking: what the mouse is over

The cursor position is `e.input().cursorX()`/`cursorY()` in **window** pixels, which is not
what a ray wants: the scene is drawn into a virtual target that may be scaled and letterboxed
inside that window, and the projection's aspect is the target's. `Engine::cursorRay` applies
both and hands back a world ray; `PhysicsWorld::raycast` turns it into a surface.

```cpp
const scene::Ray ray = e.cursorRay();
if (const auto hit = e.physics().raycast(ray.origin, ray.at(200.0f))) {
    gfx::Decal mark = gfx::decalAt(hit.point, hit.normal, 1.6f, e.gltfScene().fallbackTextureSlot(),
                                   {0.1f, 0.45f, 1.0f, 0.85f});
    mark.round = true;                  // the disc inscribed in the footprint
    e.renderer().decals.push_back(mark);
}
```

**It hits bodies, not meshes.** Picking here is a physics query, so it reaches exactly what
the scene authored colliders for -- name a node `.collider` and it is pickable, leave it out
and the ray goes through it. That is a real difference from a depth-buffer pick, and it is the
trade: a query costs nothing per frame and needs no readback fence.

`scene::rayThrough` is the arithmetic underneath, for a game that wants a ray through some
other pixel -- a gamepad reticle, a fixed crosshair.

### Drawing your own world-space lines

`renderer().debugLines` is a plain `std::vector<gfx::DebugLineVertex>` of vertex *pairs* that a
game may push into from `frameUpdate`. The engine clears it in `beginFrame` and appends its own
-- the physics wireframe and the audio occlusion rays -- in `endFrame`, so a game's lines land
first and everything draws in the same frame.

```cpp
const uint32_t colour = gfx::packDebugColor({0.45f, 0.75f, 1.0f, 1.0f});
e.renderer().debugLines.push_back({from, colour});
e.renderer().debugLines.push_back({to, colour});
```

There are no `drawBox`/`drawSphere`/`drawArrow` helpers; a game pushes the pairs it wants. And
these are drawn **without a depth test**, deliberately -- a collider wireframe inside the mesh it
describes would otherwise be hidden or z-fighting. For a plan drawn over the world that is
usually what you want, and it is the first thing a reader of your code will mistake for a bug,
so say so where you push.

---

## The UI

Immediate mode: every widget is a call that both draws and answers, and the value on
screen is the variable -- there is no widget tree and no `onClick`.

```cpp
void MyGame::drawUi(Engine& e, ui::Context& ui) {
    if (!ui.beginPanel("MyGame", {16, 16}, {320, 480})) return;
    ui.checkbox("Shadows", e.renderer().shadowsEnabled);
    ui.slider("Exposure", e.renderer().exposure, 0.1f, 4.0f);
    if (ui.button("Screenshot")) e.renderer().requestCapture("shot.png");
    ui.endPanel();
}
```

`drawUi` runs only while `Engine::uiVisible()`, which the game toggles:

```cpp
if (in.pressed(act.panel)) e.setUiVisible(!e.uiVisible());
```

The engine reads that flag for one reason of its own: while a panel is open the binding
menu is not run at all, because two things cannot own one keyboard.

The UI context is begun *lazily*, on the first call to `Engine::ui()`. That is what makes
a panel opened this frame draw this frame -- the action that opens it is yours, and it
runs after `beginFrame()`.

---

## Composing a world out of assets

A `.glb` is not a scene. It is one asset, imported onto a node, alongside however many
others — so a game builds its world in `init` rather than naming a file the engine loads
on its behalf (C41).

```cpp
void MyGame::init(Engine& e) {
    const scene::NodeId arena = e.scene().create("arena");
    e.scene().setLocalPosition(arena, {0.0f, 0.0f, 0.0f});
    e.scene().add<scene::Model>(arena, {core::Resources("res:/arena.glb")});

    const scene::NodeId prop = e.scene().create("statue");
    e.scene().setLocalPosition(prop, {4.0f, 0.0f, -2.0f});
    e.scene().add<scene::Model>(prop, {core::Resources("res:/statue.glb")});
}
```

A model is a component, added with the same verb as any other — the scene has nodes, and a
node has components — and adding it is what performs the import. The node's world transform is
where it lands, so positioning the node first is how you place it. Everything the file carries
comes with it — geometry, materials, lights, emitters, sounds, colliders and a rig — and the
colliders it authors become bodies and rebake the navmesh.
`e.scene().get<scene::Model>(node)` answers what is there afterwards; the `id` on it is the
`GltfScene::ModelId`, or `scene::kNoModel` if nothing arrived.

**A scale goes through `e.setWorldScale(...)`, not through the node's transform.** Resizing
a document is not the same operation as placing one: the scale pass holds rigs and dynamic
colliders at their authored size and carries light ranges, intensities and audio falloff
with the factor, none of which a placement matrix does. Set it before the first import.

Everything sizes itself from what arrives. There is no budget to state and no order to get
right: the physics world, the particle pool, the light buffer, the voice list and the
bindless texture array all grow when a game asks for more than they hold (C40).

## The scene tree: saying that one thing follows another

`e.scene()` is a tree of nodes. A node has a parent, a local position/rotation/scale, and
one attachment of each kind — an instance, a light, a sound, a body or character, an
emitter. Move the node and everything on it moves.

```cpp
// A node carrying one of the lights the scene already has.
torch = e.scene().create("torch");
e.scene().attachLight(torch, 1);
e.scene().setLocalPosition(torch, {-4.0f, 2.2f, 0.0f});

// Pick it up. The world transform is kept, so it does not jump on the frame it is
// grabbed; then it is placed in the hand.
e.scene().setParent(torch, players[0].node);
e.scene().setLocalPosition(torch, {0.3f, 1.3f, 0.2f});

// Put it back down where it is standing.
e.scene().setParent(torch, scene::NodeId{});

// A light has no derived state, so it is a reference. A transform does, so it is a call.
e.scene().light(torch, e.renderer().lights).color.w = 40.0f + 6.0f * std::sin(phase);
```

**The rule about calls and references is the same everywhere in this API**: a value with
derived state behind it is written through a call, and a value with none is handed out by
reference. `setLocalPosition` invalidates a world transform, a world bounding box and a
normal matrix. A light's intensity invalidates nothing.

**The engine already built nodes for what the file said moves** — one per body or character
the scene authored, with the meshes and sounds on that glTF node hanging off it.
`e.authoredCharacters()` hands you every `Character` collider the file declared with the node
it drives; which of them is a player is yours to say, and so is the list you keep them in.
Everything else in the tree is yours too.

**Reading a world transform gives you the last `update()`'s answer**, not this instant's.
The sweep runs once a frame, in `endFrame`; making a read exact would mean recomputing a
chain per read, which is the cost the once-a-frame sweep exists to avoid.

## Where the demo does each of these

`game/demo/DemoGame.cpp` is the worked example, and it is the whole of what `main.cpp`
used to hold:

| In the demo | What it shows |
|---|---|
| `AppActions::declare` | 26 actions with default bindings, and no key code in a `case` label |
| `applyActions` | Debug affordances reached through `Engine&` |
| `PlayerActions::moveDirection` | Input resolved against `e.camera()` in `frameUpdate`, from the camera's own forward |
| the follow rig and the facing, in `frameUpdate` and `driveLocomotion` | **G13**: `focus` re-aimed at the character, and a heading composed onto the child node the solver does not own |
| `DemoGame::fixedUpdate` | A state machine parameter driven per step |
| `drawSettingsPanel` | `ui::drawSettings` for the whole `render` module, and hand-written widgets for what is authored rather than configured |
| `placeLights` | A game deciding what a scene with no lights in it gets |
| the torch, in `init` and `frameUpdate` | **G3**: a node carrying a light, reparented onto the character and back by one key |
| `DemoWorld.cpp`, `buildDemoWorld` | **G9**: the whole arc at once -- `createMaterial`, `createMesh`, `InstanceTable::create`, `Scene::attach*`, `createBody` at all three motions, `setEmitters`, `AudioEngine::create`, `gfx::decalAt` |
| `demoWorldApplies` | Why a demo gates its content on its own scene: the golden suite runs this same binary against eleven scenes of its own |
| `playImpacts` | **G7** and **C3** meeting on one event -- `playAt` for the sound, `spawnEffect` for the dust |
| `bannerCloth` and `stepDemoWorld` | **G11**: a mesh whose *shape* is a function of the step -- `MeshData::morphTargets` at build time, two `setMorphWeight` calls per step |
| the four `Profiler::scope` calls in `applyActions`, `playImpacts`, `driveLocomotion` and `stepDemoWorld` | A game profiling its own code with the engine's own profiler |

### Profiling a game's own code

`core::Profiler::scope` is the engine's and a game calls it directly — there is no game-side
wrapper and no registration:

```cpp
void DemoGame::driveLocomotion(Engine& e, float step) {
    auto s = core::Profiler::scope("DemoGame::driveLocomotion");
```

The engine opens `Game::frameUpdate`, `Game::fixedUpdate` and `Game::drawUi` around the three
calls it makes, so a scope opened inside any of them nests under it and appears in the trace
as `Game::fixedUpdate/DemoGame::driveLocomotion` without a game arranging anything. The name
must be a string literal — it is stored by pointer — and the scope goes **above** the
function's early-outs, so a step that decides to do nothing still costs a named zero rather
than vanishing from the table. `fixedUpdate` runs zero to four times a frame, which is why
`scripts/baseline.py --zones` reports `total/frame` beside the median: the median is one
step's cost and the total is the frame's.

A scope costs 0.1-0.2 us in release, so this is worth doing at the granularity of a system
and not of a draw. [profiling.md](profiling.md) is the whole of the rest.

### Content a game builds is not content a *golden case* wants

`scripts/golden.sh` runs the configured game's binary against its own eleven scenes, so
anything a game creates unconditionally in `init` appears in all eleven images. The demo
compares `e.config().scene.path` against `e.gameSetup().scene` and builds nothing when they
differ: a scene named on the command line is somebody else's scene. Any game with a
regression suite of its own wants the same gate.

**A camera is content by this rule too** (G13). One of those eleven scenes authors a character,
so a follow rig that re-aimed the camera wherever one existed would move a baseline that has
nothing to do with the renderer — and none of the eleven passes `--camera`, so there is no
explicit framing to override it. The demo's rig sits behind the same `demoWorldApplies` gate
its meshes do, which is the general shape: **anything that decides what the frame looks like
is content, not just anything that adds geometry to it.**

---

## What is not here yet

Stated plainly, because the alternative is discovering it:

- **Building a scene in code.** Built, and `game/demo/DemoWorld.cpp` is the worked example.
  A second copy of a mesh made in code is `e.addInstance(model, material, transform,
  scene::InstanceMotion::Dynamic)` — G14, and it used to be a private helper in the demo
  because there was no verb for it. The motion argument is an enum with no default on
  purpose: it decides whether the instance writes a velocity for TAA and which tier of the
  acceleration structure it lands in, and getting it wrong is a shadow that stays behind
  after the thing casting it has been knocked over.
- **Loading more than one model, or making a mesh from vertices.** Built --
  `e.addModel(path)` and `e.createMesh(MeshData)`. **`addModel` brings geometry and nothing
  else**: an appended file's colliders, emitters, sounds, lights and rig are parsed and not
  wired, because only `Engine::loadScene` hands those to their subsystems. Content that
  needs a subsystem other than the renderer still belongs in a file the game imports.
- **Making a mesh change shape.** Built -- fill `MeshData::morphTargets` with one delta
  array per target, each exactly as long as the mesh, and `createMesh` gives the model a
  weight block of its own. Drive it with
  `e.animator().setMorphWeight(e.morphCharacterOf(model), target, w)` from `fixedUpdate`;
  a weight is a coefficient rather than a fraction, so values outside 0..1 are legal and
  two targets sum. What is *not* built: a morphed mesh **appended from a file** is not
  driven, because `addModel` hands nothing to `SceneAnimator`.
- **Attaching your own shader to a material.** **G5**, with **G5b** for resolving a
  game's shader directory.
- **Naming a setting.** Built — `e.settingsTable().set(options::render::ssao, false)`, and
  `ui::drawSettings(ui, e.settingsTable(), "render")` draws the whole module. What is *not*
  a setting still is not: an exposure and a sun are authored, so they stay yours.
- **Scaffolding a new game with one command.** Built — `./new_game.sh mygame`, then
  `./build_game.sh mygame`. What it writes is the two files above and a one-line
  `CMakeLists.txt`.

---

## The call site, as the arcs designed it

These sketches were written backwards from what a game author types, and the capabilities
they show are built. Where a sketch describes something not yet in the tree, it has moved to
the card of the row that will build it — see [the board](../kanban/).

## The call site — the runtime capabilities

Designed backwards, the way the other roadmap is, because the point of this arc is that the
five subsystems stop looking like five.

### Symmetry: everything that can be created can be destroyed

```cpp
void DemoGame::fixedUpdate(Engine& e, float step) {
    Scene& scene = e.scene();

    // One verb per direction, on every subsystem, with a typed handle each.
    BodyId      crate  = scene.attachBody(node, BodyDesc{ .motion = Motion::Dynamic });
    SoundId     hum    = scene.attachSound(node, SoundDesc{ .file = "res:/hum.wav" });
    CharacterId enemy  = scene.attachCharacter(spawn, CharacterDesc{ .maxSpeed = 3.0f });

    scene.destroy(crate);          // not removeBody, not releaseBody, not freeBody
    scene.destroy(hum);
    scene.destroy(enemy);

    // Destroying a node cascades to its attachments. That is the only cascade, and it
    // exists because a node owns them -- nothing else in the engine owns anything else.
    scene.destroy(node);
}
```

`scene.destroy(crate)` and `scene.destroy(hum)` are overloads on the handle type, so there is
one verb to remember and the compiler routes it. A `BodyId` passed where a `SoundId` belongs
does not compile.

### A query returns what it hit

```cpp
// A miss is a falsy hit, not a bool plus three out-parameters.
if (const RayHit hit = e.physics().raycast(eye, eye + forward * 3.0f)) {
    scene.spawnEffect(effects::impact, hit.point, hit.normal);   // C3
    e.audio().playAt(sounds::thud, hit.point);                   // G7
    e.ui().prompt("Open");                                       // whatever the game does
}

// Bounded queries take the caller's storage and report what they wanted.
BodyId nearby[16];
const uint32_t found = e.physics().overlapSphere(at, 5.0f, nearby);
if (found > 16) LOG_WARN("%u bodies in range, room for 16", found);
```

```cpp
struct RayHit {
    BodyId body;
    glm::vec3 point{};
    glm::vec3 normal{};
    float distance = 0.0f;
    explicit operator bool() const { return body.valid(); }
};
```

`segmentBlocked` stays, unchanged and implemented over `raycast`. It has one caller — audio
occlusion — and its boolean return is right for that caller, which
[`Audio.h:221`](../../engine/scene/Audio.h#L221) already argues: the raycast is deliberately the
caller's, so "a game whose occlusion comes from a navmesh, a portal graph or nothing at all
calls the same function". What was wrong was never that door; it was that it was the *only*
one.

### A body is pushed, and can be told to stay in a plane

```cpp
// A 2D game is a 3D world with an axis taken away, not a second solver.
ColliderDesc desc;
desc.motion  = ColliderMotion::Dynamic;
desc.freedom = ColliderFreedom::Plane2D;      // X and Y translation, Z rotation
const BodyId crate = e.physics().createBody(desc);

e.physics().addImpulse(crate, {5.0f, 0.0f, 0.0f});          // kg m/s, at the centre of mass
e.physics().setLinearVelocity(lift, {0.0f, 1.0f, 0.0f});    // m/s, kinematic bodies too
const glm::vec3 v = e.physics().linearVelocity(crate);

// A respawn is a teleport and a stop, which is two calls on purpose: a portal is the
// first without the second.
e.physics().setBodyTransform(crate, spawn);
e.physics().setLinearVelocity(crate, {});
```

`freedom` is authored in the file as well — `"freedom": "plane2d"` beside `"motion"` in
`substrate_collider` — so a 2D level does not have to be built in code. The constraint is
Jolt's own `EAllowedDOFs`, which is what makes contacts, stacking, friction and every query
the ones the 3D world already had.

`addImpulse` refuses a kinematic or static body **with a reason in the log** rather than
absorbing it, which is Jolt's own behaviour and not one worth inheriting.

### Pause is a time scale, not a second concept

```cpp
e.setTimeScale(0.0f);    // pause
e.setTimeScale(0.25f);   // bullet time
e.setTimeScale(1.0f);    // resume
```

One concept rather than an `isPaused` flag beside a `timeScale` float, which would immediately
raise the question of what `setTimeScale(1.0f)` does while paused. The scale multiplies what
`FixedClock` accumulates, so animation, particles, physics and audio all inherit it from the
one place they already inherit their step from — and rendering, input and the UI are outside
the step and keep running, which is exactly what a pause menu needs.

**A stated consequence:** audio *sources* keep playing at the device level, because miniaudio
owns that clock. Silence during pause is a game's decision — `setMuted` already exists — and
not the time scale's business.

### Save and load are two virtuals and a byte stream

```cpp
// engine/Game.h -- 5 virtuals become 7
virtual void save(Engine&, SaveWriter&) {}
virtual void load(Engine&, SaveReader&) {}
```

The engine writes what it owns — the node tree, transforms, attachment descriptors, the
handles that key them — and calls the game for what it owns. One file, two sections, one
version number each.

**This adds methods, not a base class.** The count of base classes the engine defines stays at
two, three after G7's `ContactListener`, and the rule the other roadmap amended
(*"`Game` is the only base class the engine defines, and it sits at the outermost edge"*)
survives untouched. Worth stating because a save system is a classic place for an
`ISerializable` to appear, and it is refused here in writing.

### An image is a handle and a call

```cpp
void DemoGame::drawUi(Engine& e, ui::Context& ui) {
    ui.image(icons.health, {32.0f, 32.0f});
    ui.image(icons.health, {32.0f, 32.0f}, ui::Tint{0.4f, 0.4f, 0.4f, 1.0f});
}
```

`ImageId` comes from `e.images().load("res:/ui/health.png")` (P1) and names a slot in a
growable bindless array the engine maintains — `e.images().destroy(id)` gives it back, and a
handle to a slot that has since been reused draws the font atlas rather than the wrong icon.
The overlay pipeline carries a texture index per vertex; the font atlas is slot zero and every
existing draw keeps working unchanged.

### A flat world is an orthographic camera and a sprite table

```cpp
void MyGame::configure(GameSetup& setup, core::settings::Settings&) {
    setup.present.virtualResolution = {320, 180};
    setup.present.pixelExact        = true;      // sprites want a nearest tap; see above
}

void MyGame::init(Engine& e) {
    // One world unit per texel: the same number as the virtual height. Anything else is
    // a scale, and a scale is where a texel stops being a texel.
    e.camera().projectionMode = scene::Camera::Projection::Orthographic;
    e.camera().orthoHeight = 180.0f;
    // **Down -Z, and this is the one thing that is not the obvious value.** At yaw 0 the
    // camera looks down *+Z* and the world comes out mirrored -- a sprite placed on the
    // left edge draws on the right, which looks exactly like a sign error somewhere else.
    e.camera().yaw = glm::pi<float>();
    e.camera().pitch = 0.0f;
    e.camera().focus = {0.0f, 0.0f, 0.0f};

    atlas      = e.images().load("res:/sprites/dungeon.png");
    background = e.sprites().createLayer({.order = -10});
    actors     = e.sprites().createLayer({.order = 0});

    hero = e.sprites().create(actors, {
        .image = atlas,
        .uv    = {0.0f, 0.0f, 16.0f, 16.0f},   // texels, not normalised
        .size  = {16.0f, 16.0f},               // world units, which here are texels
        .pivot = {0.5f, 1.0f},                 // feet
    });
}

void MyGame::fixedUpdate(Engine& e, float step) {
    e.sprites().setPosition(hero, at);
    e.sprites().setFlip(hero, facingLeft, false);
}
```

With `orthoHeight` equal to the virtual height, world `(x, y)` lands on texel
`(x + width/2, height/2 - y)` — one for one, with the origin in the middle of the screen and
`+Y` up. Put a sprite on integer coordinates and its texels are the file's texels; the
`sprite` case in `scripts/readback.sh` is that claim checked in texels rather than asserted
here.

**The UV rect is in texels because an atlas is measured in pixels in the tool that drew it.**
The engine divides by the image's own dimensions in the fragment shader, so nothing in the
game has to know how big the file is — and a `{0, 0, 0, 0}` rect means the whole image, which
is the answer for art that is not in an atlas.

Layers are a sort key and nothing else: lower `order` draws first, ties inside one layer
break by creation order, and every layer goes out in **one** instanced draw. Ten thousand
sprites cost 0.05 ms on the GPU and moving all of them costs no sort at all, because a
position is not part of the key. `destroy` on a layer destroys its sprites with it.

**A sprite sheet is a rectangle on that same sprite**, not a second kind of thing:

```cpp
sheet = e.sprites().createSheet({.frame = {16, 16}, .columns = 8, .count = 24});
idle  = e.sprites().addClip(sheet, {.name = "idle", .first = 0, .count = 4, .fps = 6});
run   = e.sprites().addClip(sheet, {.name = "run",  .first = 8, .count = 6, .fps = 12,
                                    .events = {{0.25f, "left"}, {0.75f, "right"}}});
e.sprites().play(hero, sheet, run);

// After the step, never during it -- the same contract SceneAnimator::firedEvents has.
for (const auto& f : e.sprites().firedEvents()) {
    if (e.sprites().clip(f.sheet, f.clip).events[f.event].name == "left") playFootstep();
}

// A tile is a cell with no playback at all.
e.sprites().setUv(tile, e.sprites().frameUv(sheet, 17));
```

`LoopMode`, `AnimationEvent` and the event list are the **same** ones a skeleton uses, so
there is one animation vocabulary to learn rather than two. Frames advance on the fixed step,
which means a paused game has paused sprites and a time scale slows them without a game doing
anything. An event's `time` is in seconds, as it is on every other clip in the engine; cell
*f* begins at `f / fps`, so an event keeps its place in the motion when a clip is retimed.

A sprite drawn this way is **unlit**, and that is the trade: it is not fogged, bloomed,
reflected or occluded by 3D geometry, and in exchange the texel the artist drew is the texel
on the screen. A sprite that wants the lighting pass is a different thing and is not built
yet.

---

## The call site — a whole game, end to end

The arc is designed backwards from this. Everything after it is implementation.

### A game's entire entry point

```cpp
// game/demo/DemoGame.cpp
SUBSTRATE_GAME(DemoGame)
```

`SUBSTRATE_ENTRY_POINT` is a CMake option, **ON by default**. With it on, the engine
library compiles `main()`, and the macro above is the only thing connecting a game class
to it:

```cpp
// engine/Entry.h
#define SUBSTRATE_GAME(T)                                       \
    std::unique_ptr<Game> substrateCreateGame() {               \
        return std::make_unique<T>();                           \
    }

// engine/Entry.cpp -- compiled only when SUBSTRATE_ENTRY_POINT is ON
int main(int argc, char** argv) {
    Engine engine;
    if (!engine.init(argc, argv)) return engine.exitCode();
    std::unique_ptr<Game> game = substrateCreateGame();
    const int code = engine.run(*game);
    engine.shutdown();
    return code;
}
```

With it **off**, the game writes its own `main()`. The hand-driven loop stays public
because three existing things need it — `scripts/baseline.py`, `scripts/golden.sh`, and any
tool wanting to interleave work between phases:

```cpp
while (engine.beginFrame()) {                     // poll, input, clocks, hot reload
    game.frameUpdate(engine, engine.dt());
    while (engine.consumeStep()) {
        game.fixedUpdate(engine, engine.step());  // game intent, then simulate
        engine.simulate(engine.step());           // animation, particles, physics, audio
    }
    if (engine.uiVisible()) game.drawUi(engine, engine.ui());
    engine.endFrame();                            // record, submit, present
}
```

`Engine::run(Game&)` *is* that loop, and exists whether or not the entry point is compiled.
The option decides only whether a `main()` calling it is compiled too.

**`fixedUpdate` runs before the engine's movers**, which is the ordering `main.cpp` already
uses when it reads character input once per frame and steps physics per step: game intent,
then simulate.

### `engine/Game.h`

```cpp
/// The one interface in the engine. Every method has an empty default, so a game
/// overrides what it needs and nothing else.
class Game {
  public:
    virtual ~Game() = default;
    virtual void init(Engine&) {}
    virtual void frameUpdate(Engine&, float dt) {}    // once per rendered frame
    virtual void fixedUpdate(Engine&, float step) {}  // once per sim step, before the movers
    virtual void drawUi(Engine&, ui::Context&) {}
    virtual void shutdown(Engine&) {}
};
```

### `init` — building a scene in code

```cpp
void DemoGame::init(Engine& e) {
    Scene& scene = e.scene();
    Assets& assets = e.assets();

    // ---------------------------------------------------------------- settings
    // The same values substrate.json carries. Game code takes the typed door, so a
    // misspelling is a build error; the string door exists for the console, JSON and
    // the inspector. Both write the one variable.
    e.settingsTable().set(options::render::ssr, true);
    e.settingsTable().set(options::render::msaaSamples, 8u);

    // ------------------------------------------------------------------ import
    // glTF is a source of meshes, materials and rigs. It is not the scene format:
    // nothing here reads an `extras` key, and the file needs no engine schema in it.
    ModelId sponza = assets.loadModel("engine/assets/Sponza/glTF/Sponza.gltf");
    ModelId barrel = assets.loadModel("game/sample/assets/barrel.gltf");
    ModelId hero   = assets.loadModel("game/sample/assets/character.gltf");

    // ------------------------------------------------------------------- scene
    level = scene.instantiate(sponza);
    scene.setLocalScale(level, glm::vec3(0.008f));

    // A light and a sound on a node, so moving the node moves both. This is the case a
    // flat instance table cannot express at all.
    torch = scene.createNode("torch", level);
    scene.setLocalPosition(torch, {-4.0f, 2.2f, 0.0f});
    scene.attachLight(torch, PointLight{
        .color = {1.0f, 0.72f, 0.42f}, .intensity = 40.0f,
        .range = 12.0f, .castsShadows = true,
    });
    scene.attachSound(torch, SoundDesc{
        .file = "game/sample/assets/audio/torch.wav", .bus = "sfx",
        .loop = true, .spatial = true, .maxDistance = 14.0f,
    });

    // ------------------------------------------------------- geometry from code
    // No glTF involved. A mesh is vertices and indices, uploaded into the same shared
    // buffers the importer writes to, so it draws through the same pass.
    MeshId cube = assets.createMesh(boxMesh({0.5f, 0.5f, 0.5f}));
    MaterialId brass = assets.createMaterial(MaterialDesc{
        .baseColor = {0.79f, 0.60f, 0.21f, 1.0f}, .metallic = 1.0f, .roughness = 0.28f,
    });

    for (int i = 0; i < 8; ++i) {
        Node crate = scene.spawn(cube, brass, level);
        scene.setLocalPosition(crate, {float(i) * 1.4f, 0.5f, -3.0f});
        scene.attachBody(crate, BodyDesc{
            .shape = boxShape({0.5f, 0.5f, 0.5f}),
            .motion = Motion::Dynamic, .mass = 20.0f,
        });
    }

    // -------------------------------------------------- a shader on one material
    // The game supplies GLSL. It honours the G-buffer contract -- write albedo, normal,
    // ORM, emissive -- and is free inside it. Everything using this material draws in
    // its own indirect draw behind its own pipeline; everything else is untouched.
    ShaderId dissolve = assets.loadShader(ShaderDesc{
        .name = "dissolve",
        .gbufferFragment = "shaders/dissolve.frag",   // resolved against the game first
        .constants = {{"EDGE_GLOW", 1u}},
    });

    dissolving = assets.createMaterial(MaterialDesc{
        .baseColor = {0.2f, 0.9f, 0.6f, 1.0f}, .roughness = 0.6f,
        .shader = dissolve,        // the material selects the pipeline variant
        .params = {0.0f},          // free floats the custom shader reads
    });

    Node ghost = scene.spawn(assets.mesh(barrel, 0), dissolving, level);
    scene.setLocalPosition(ghost, {2.0f, 0.5f, 1.0f});

    // ------------------------------------------------------------------ player
    player = scene.instantiate(hero, level);
    scene.attachCharacter(player, CharacterDesc{
        .shape = capsuleShape(0.35f, 1.75f), .maxSpeed = 4.0f, .jumpSpeed = 5.5f,
    });
    scene.playAnimation(player, "Idle");

    // ----------------------------------------------------------------- actions
    // Named actions with default bindings; substrate.json overrides them and the
    // binding menu rebinds them. Exactly what AppActions does today, as a call.
    //
    // W, A, S and D are the *camera's* until something says otherwise. The next line is
    // built (G8) and called (G12); the three after it are still a sketch.
    // `setDefaultBindings` moves the declared default and the live list together, which is
    // what stops the binding menu reporting a shipped control scheme as something the
    // player edited -- see "Shipping a control scheme" below for the demo's whole version.
    e.input().setDefaultBindings(e.input().find("Camera.Forward"), "Up");   // and the rest
    act.move     = e.input().action("Game.Move", input::Axis2::Wasd);
    act.jump     = e.input().action("Game.Jump", input::Key::Space);
    act.interact = e.input().action("Game.Interact", input::Key::E);
}
```

### Who the players are is the game's, and needs no engine field

The engine has no notion of "the player" and will not acquire one (G17). What it offers is
`e.authoredCharacters()` — every `Character` collider the loaded file declared, each with the
node the engine attached it to — and what you do with that list is the game:

```cpp
// The game's own table. Three fields because three things are per player; the demo's is in
// game/demo/DemoWorld.h and holds exactly this.
struct Player {
    scene::PhysicsCharacterId character;  // what setCharacterInput drives
    scene::NodeId node;                   // what a carried thing is parented to
    scene::AnimatorId rig;                // what the locomotion pair blends
};
std::vector<Player> players;

// A character the file authored. `rig` stays invalid -- a collider brings no skin.
for (const Engine::AuthoredCharacter& c : e.authoredCharacters()) players.push_back({c.character, c.node, {}});

// Or one the game made, which is the case a file cannot cover.
const scene::PhysicsCharacterId controller = e.physics().createCharacter(desc);
const scene::NodeId node = e.scene().create("player");
e.scene().attachCharacter(node, controller);
e.locomotion().pair(controller, rig);
players.push_back({controller, node, rig});
```

**Four players is four entries and one `InputMap` player each** — `e.input().value(action, p)`
resolves against player `p`'s devices (C26), so the same action drives four characters without
a second binding table. The engine imposes no maximum and no minimum: an RTS keeps an empty
list and nothing anywhere asks it why.

The one thing that is still singular is the view. `scene::Camera` is one camera driven by
input player 0, so players today share a screen.

### The camera is the game's, and now so is installing one

**A game that installs no camera has none** (G18). `scene::Camera` is a pose and a projection
with three empty-defaulted virtuals, and the base doubles as the null camera — it looks at the
scene and takes no input — so `e.camera()` is never null and needs no branch anywhere. The
free-fly controller is `scene::FlyCamera` in `engine/scene/CameraControllers.h`, and it is two
lines to ask for:

```cpp
// Game::init. A member, not a local -- the engine holds a non-owning pointer and never deletes.
flyCamera.applySettings(e.settingsTable());
e.setCamera(&flyCamera);
```

`applySettings` is the second line rather than an oversight: `camera.moveSpeedScale`,
`orbitSensitivity` and `zoomStep` are the *controller's* rows, so with only a `Camera&` in hand
the engine cannot apply them. `camera.fovDegrees` stays the engine's and needs nothing from you.

`setCamera` is the only door, and it is `deactivate` then `activate`, so bindings cannot be
mis-sequenced or forgotten. **Bindings arrive on activation, not construction** — hold every
camera your game will ever use and you pay input surface only for the active one; two held
cameras declaring `Camera.Forward` in their constructors would be a conflict `InputMap` would be
right to report. `setCamera(nullptr)` goes back to the null camera and takes no input at all,
which is what a cutscene or a fixed 2D view wants.

**The engine never calls `activate` or `update` on a view camera.** There is one `InputMap`, so a
`gfx::ViewTable` camera you install with `views().setCamera(id, &cam)` is yours to drive in
`frameUpdate`, with whatever map you like — which is also the only route a second player's input
could take.

**Destroy an installed camera and you must `setCamera(nullptr)` first.** The pointer is
non-owning and the engine never deletes.

The rest of this section is about what to do with the camera once you have one, and is unchanged.

An earlier draft of the sketch above wrote `e.camera().follow(scene, player, {0, 1.6, 4.5})`,
and no row implemented it. It is struck rather than assigned, because the engine already
does everything a follow camera needs and the remaining line is the game's. G13 wrote it, and
this is the whole of the rig:

```cpp
// DemoGame::frameUpdate. The engine has already run the camera by the time this is called.
const glm::vec3 here(e.physics().characterTransform(players[0].character, 0.0f)[3]);
if (following) e.camera().focus = here + glm::vec3(0.0f, 1.2f, 0.0f);
```

`Camera::position()` is `focus - forward() * distance`, and `Camera::update` turns a drag by
holding the *eye* and swinging the focus. Overwriting the focus afterwards inverts that into
an orbit about the character, at no cost and with no mode flag: a game that stops writing
`focus` is flying again on the next frame.

**This is the second thing G1's ordering buys.** That stage moved `camera().update()` ahead
of `Game::frameUpdate` so a game resolving "forward" against the camera would get this
frame's yaw. The same ordering is what lets a game's write to `focus` win, and it is worth
recording here because the property is now load-bearing rather than incidental.

**Write `focus` and leave `yaw` alone.** That is not a style note, it is what stops the camera
and the character chasing each other. Aim the camera from the character's heading as well and
the two become integrators feeding each other — the character walks where the camera points,
the camera turns to where the character walks, and any error in either is a drift neither can
correct. Writing only the focus makes the coupling one-way by construction: the yaw comes from
the mouse, so the basis does not depend on the motion it produces, and nothing has to be
damped for it to be stable.

**Snap it rather than smoothing it, unless you have a fixed delta to smooth against.**
`frameUpdate` is handed a wall-clock delta even under `--locked`, so an exponential follow
makes the drawn frame a function of how fast the machine ran and `--capture` stops
reproducing. `characterTransform` already interpolates between fixed steps, which is where the
smoothness would have come from anyway.

**And resolve movement against the camera itself, never against a heading rebuilt from its
yaw.** `Camera::forward()` is `(cos p·sin y, sin p, cos p·cos y)`; the demo carried a
`(sin y, 0, -cos y)` beside it for four stages, which is the same vector only where `cos y` is
zero — and `Camera::frameBounds` picks exactly that yaw for any scene whose X extent is the
longer one. W walked the character toward the camera in every other scene and three checks
passed over it. Flatten the camera's own forward, take screen-right from the same
`cross(forward, up)` the camera takes, and there is one expression of the basis to be wrong:

```cpp
const glm::vec3 view = e.camera().forward();
const glm::vec3 ahead = glm::normalize(glm::vec3(view.x, 0.0f, view.z));
const glm::vec3 side  = glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f));
```

### Turning the character to face where it goes

The solver does not do this for you: `characterTransform` hands back
`CharacterVirtual::GetRotation()`, nothing in the engine ever calls `SetRotation`, and a
character with no facing strafes everywhere at whatever heading its rig was authored with.

**Put the rotation on the child node, not on the node the character drives.** The scene sweep
writes the solver's matrix straight into a driven node's world transform, so a
`setLocalRotation` there is overwritten every step and does nothing — no warning, no error,
and the number reads back fine from the variable you kept it in. The loader already creates a
child per mesh a body drives, and that child is an ordinary node:

```cpp
// The child the loader made, found once by name -- G3's torch is parented to the same
// node while it is carried, and setParent pushes at the head of the child list.
for (scene::NodeId c = e.scene().firstChild(players[0].node); c.valid(); c = e.scene().nextSibling(c)) {
    if (e.scene().name(c) == "mesh") facingNode = c;
}
// A yaw about +Y, measured from +Z, which is the convention Camera::forward() uses.
e.scene().setLocalRotation(facingNode, glm::angleAxis(facingYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
```

Three things that are easy to get wrong:

- **Turn toward where the character *went*, not where it was asked to go.** The two differ
  through every acceleration ramp and anywhere the solver slid the character along a wall. The
  displacement between two steps is the answer, and it is the same reasoning that says to ask
  `characterJumped` rather than reconstruct it.
- **Slew, do not snap.** A snap is a mesh flipping through 180° in one step, which happens
  every time the camera swings past the character. A fixed rate against the *fixed* step keeps
  it deterministic; a rate against the frame delta does not.
- **Do not add a setter to `PhysicsWorld` for it.** A `setCharacterFacing` is a capability and
  belongs to a row that wants the solver to know a heading. Composing the rotation into the
  tree needs nothing from the engine and decomposes no matrix, which is what keeps a physics
  scene rendering the bytes it rendered before.

What the camera *does* need is for its move actions to stop owning W, A, S, D, Q, E, Space
and LeftShift, which between them are the whole of a third-person control scheme. G8 built
the call that does it — `InputMap::setDefaultBindings`, from `Game::init`, before
`Engine::run` applies the config's rebinds — so this is now a line a game writes rather than
a row waiting on the board.

### Shipping a control scheme, as the demo now does it

G12 wrote those lines. They are the whole of `PlayerActions::declare`, and the ordering is
the point: **the camera gives the keys up in the same function that takes them**, or the two
tables collide for as long as it takes to write the second one and `InputMap::conflicts` is
right to say so at startup.

```cpp
void PlayerActions::declare(core::input::InputMap& map) {
    const auto rebind = [&map](const char* action, const char* keys) {
        map.setDefaultBindings(map.find(action), keys);
    };
    rebind("Camera.Forward", "Up");     // and Back, Left, Right
    rebind("Camera.Fast", "RightShift");

    forward = map.declare("Player.Forward", "W Pad.LeftY-");
    run     = map.declare("Player.Run",     "LeftShift Pad.RightBumper");
    jump    = map.declare("Player.Jump",    "Space Pad.A");
}
```

**The left stick goes with the keys**, which is the half a keyboard-shaped reading misses:
on a pad a third-person character is the thing the stick moves, and a flying debug camera
that kept it would be the same collision one input away.

Three things worth knowing before writing the equivalent for your own game:

- **`setBindings` is not this call and looks exactly like it.** It moves the live list and
  leaves `defaults` behind, so `isDefault()` goes false, the binding menu offers to "reset"
  the camera back onto W, and `saveBindings` writes rows the player never touched — a game
  impersonating a user who edited a binding. The demo logs a `Shipped bindings:` line
  marking any action that is off its default, because the difference is invisible otherwise.
- **A movement request carries a magnitude.** `setCharacterInput` multiplies the vector by
  the character's `moveSpeed` without normalising it, so `moveDirection` returns a vector
  whose *length* is the fraction of top speed being asked for. That is what makes a stick
  analogue — `InputMap::value` on an axis binding is how far it was pushed — and it is why
  the keyboard needs a run modifier rather than a second speed constant. Without one, every
  request is full travel and a `walk` state between two speed thresholds is unreachable.
- **Ask the solver whether the jump happened; do not work it out.** The obvious version of
  the animation trigger is `pressed(jump) && characterOnGround(...)`, and it is the
  controller's own decision re-derived from the outside. It was correct only while the two
  could not disagree. They can: a press inside the coyote window launches with no ground
  under it, and a press inside the jump buffer launches a step or two *after* the frame it
  arrived on. `characterJumped(id)` is true on the step after the solver applied a launch and
  is the whole of what a game needs.
- **`characterOnGround` means standing, and it is not the opposite of falling.** A character
  on a face steeper than its `maxSlopeAngle` answers false there — correctly, because it
  cannot jump — while being visibly in contact with a surface. `characterGround(id)` is the
  three-valued call, and `CharacterGround::Sliding` is the state a slide clip belongs to.

### Tuning a character in the scene rather than in the engine

Seven rows on `extras.substrate_collider`, all optional, all with working defaults:

```json
"substrate_collider": {
  "motion": "character", "radius": 0.3, "halfHeight": 0.6, "offset": [0, 0.9, 0],
  "moveSpeed": 3.2, "jumpSpeed": 4.2, "maxSlopeAngle": 50.0, "stepHeight": 0.35,
  "acceleration": 10.0, "deceleration": 40.0, "airControl": 0.35,
  "jumpBufferSteps": 10, "coyoteSteps": 6
}
```

- `acceleration` / `deceleration` are m/s², and which one applies is decided by whether the
  request is *faster* than the current motion — so a turn at speed decelerates through it and
  accelerates out of it. **A pair large enough to close the gap in one step is the old
  behaviour**, where the requested velocity was simply assigned; a game that wants a weightless
  character authors `1e6` rather than editing `engine/`.
- `airControl` scales both while the character is not standing. Below one, a jump carries the
  speed it launched with.
- `jumpBufferSteps` and `coyoteSteps` are **counts of fixed steps**, not seconds and not
  frames. That is deliberate: a window in seconds has to be divided by the step to be used,
  and one in frames changes size with the frame rate. Author them as integers and they mean
  the same thing on every machine.
- `stepHeight` is how high a step the character walks up rather than into. The step-down
  follows it, so a character authored at any size keeps the behaviour rather than inheriting
  Jolt's absolute 0.4 m.

`maxSlopeAngle` is degrees in the file and radians in the struct, like every other angle in
this schema.

## What each piece costs

### `Engine` — mostly motion, and one rule to keep it honest

`Engine` owns what `main()`'s locals own today, in the same order: `Config`, `Logger`,
`Profiler`, the GLFW window, `gfx::VulkanContext`, `gfx::Uploader`, `gfx::Renderer`, the
loaded scene, `InstanceTable`, `SceneAnimator`, `ParticleSystem`, `PhysicsWorld`,
`AudioEngine`, `input::InputMap`, `ui::Context`, `FixedClock`, `Camera`. Roughly 600 lines
*moved*, not written.

It returns references to those concrete types and **wraps none of them**. This is the
god-object seam, and the rule that keeps it shut is:

> **`Engine` gets no method that merely forwards to one subsystem.**

`e.dumpProfile()` earns its place because it spans two — the profiler and the renderer's
GPU timings — and `e.startRecording()` because it spans three, the `Recorder`, the
renderer's frame tee and the audio tap. `e.setExposure()` does not; that is
`e.renderer().exposure`. The day a forwarding method is added "for convenience" is the day
this becomes a facade over an engine rather than the engine's own front door.

### glTF extras — one vocabulary, two front doors

The extras are not deleted and are not the only door. `gltf::import` produces a `Model`
whose nodes carry attachments described in **engine terms** — the same `PointLight`,
`ColliderDesc`, `EmitterDesc` and `SoundDesc` structs the code API takes.
`substrate_collider` becomes one parser producing a `ColliderDesc`; `scene.attachBody(...)`
is another producer of the same struct. Authoring in a file and authoring in code converge
on one vocabulary, which is the actual fix — the complaint was never that the extras exist,
it was that they were the only way in.

### Cloth pins are a mesh attribute, not `extras`

Every authoring schema above is a node property, written into `extras` and placed by the
node's world transform. Cloth is the one exception, and it is an exception because the data
is *per vertex*: which corners of a curtain are nailed to the rail is a property of the
geometry, and `extras` has nowhere to put a value per vertex. So the convention is two
strings on the mesh itself — a `FABRIC_` name prefix, and a `_PIN_WEIGHT` float attribute
where `1` is nailed down and `0` swings free.

**What you do in Blender.** Four steps, and the third is the one people miss:

1. Name the mesh `FABRIC_Curtain`. Rename it in **Object Data Properties** — the green
   triangle tab — and not only in the outliner. The outliner shows the *object* name;
   glTF's `mesh.name` comes from the mesh **data-block**, and that is the name the loader
   compares. Rename both and the question does not arise.
2. In Object Data Properties > **Attributes**, add one named `_PIN_WEIGHT`, type **Float**,
   domain **Vertex**. The leading underscore is load-bearing: glTF reserves that prefix for
   application semantics, and it is why Blender's own exporter passes the attribute through
   verbatim and no exporter patch is needed.
3. Paint it. In edit mode, select the vertices to pin and set the attribute to 1 —
   Sculpt/Vertex Paint mode with the attribute active works too. **Do not reach for a
   vertex group.** Weight-painting a group is the familiar gesture and it is the one that
   silently loses your work: groups leave a glTF only as `JOINTS_0`/`WEIGHTS_0`, and only
   when an armature is present, so a bare group on a curtain is dropped without a warning.
4. Export with **Data > Mesh > Attributes** ticked. It is off by default in the glTF
   exporter, and with it off the attribute is simply not written.

Then check the file, because every one of those four fails quietly:

```bash
./scripts/check_pins.py game/mygame/assets/curtain.glb
```

It reads the export rather than the `.blend` — which is the only side of the exporter where
a dropped vertex group, an unticked checkbox or a half-renamed object is visible at all —
and refuses, naming the mesh and the primitive, anything the loader would not be able to
use. A mesh carrying `_PIN_WEIGHT` without the prefix is a warning rather than a refusal:
it is dead payload in every buffer that ships it, but the export is still correct. See
[tooling.md](../architecture/tooling.md#the-cloth-pin-check) for the full list.

There is no Blender add-on in this repository, deliberately. Nothing here can load `bpy`,
so an add-on would be the one file the suite could never run — and its most important
check, that the pins survived the export, is not one a pre-export validator can make.

**What the engine then does, and the four things to know.** Loading a scene with a valid
`FABRIC_` mesh needs no call from your game at all: the loader reads the attribute, the
physics world builds a Jolt soft body per primitive, and the solved vertices reach the same
buffer skinned characters draw out of. So a curtain collides with everything the scene's
colliders describe, casts shadows and appears in reflections, and your `Game` never mentions
it. The four consequences worth knowing before you author one:

- **Model it flat.** A sheet modelled hanging is already in its equilibrium pose and will
  visibly do nothing; a sheet modelled flat swings into its hang, which is both the more
  useful starting state and the easier one to UV. `game/demo/assets/cloth.gltf` is two of
  them — run `./run.sh demo -- game/demo/assets/cloth.gltf`
  to see a curtain drape over a crate and a flag fold.
- **The placement is baked in and the node stops mattering.** Where the node puts the mesh is
  where the cloth starts, and after that the node's transform is ignored — animating a
  curtain's parent moves nothing. Move what it is pinned *to*, not the cloth.
- **Split by material means split by body.** Blender splits a mesh by material and each
  primitive becomes its own soft body, so a curtain wearing two materials is two pieces of
  fabric that do not hold each other up. `check_pins.py` checks "at least one vertex pinned"
  per primitive for this reason: a lower half with no pins is a separate body that falls.
- **A partial weight makes a vertex heavy, not slow.** Anything at or above `0.999` is nailed
  down; anything below moves. Authoring `0.5` to get half-speed fabric does not work — see
  [limitations.md](../architecture/limitations.md) for the measurement and why.

Make it **double-sided**. A hanging sheet shows its back the moment it folds, and a
single-sided cloth is a cloth with holes in it.

---
