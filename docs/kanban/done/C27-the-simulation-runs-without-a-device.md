---
id: C27
title: The simulation runs without a device
arc: C
size: L
verification: tests-hosted, scaffold, inspection
---

# C27 — The simulation runs without a device

Afterwards there is a loop that steps a scene with no Vulkan device and no window, so a
dedicated server, a CI regression run on a GPU-less box, or a batch tuning job is a target
rather than a reimplementation. Today `--headless` does not do this and does not claim to:
`config.window.headless` is `glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE)` and nothing more, and
`initWindow` still calls `volkInitialize`, `glfwInit` and creates a real window and a real
surface ([`Engine.cpp:353-363`](../../../engine/Engine.cpp#L353-L363), whose comment says so:
"Unmapped, not absent").

**Every piece needed is already proven device-free.** `SUBSTRATE_HOSTED_SOURCES`
([`CMakeLists.txt:376-419`](../../../CMakeLists.txt#L376)) is 42 translation units naming no
Vulkan type — the card said 45; the list holds 42 — and it already includes `scene/Physics.cpp`, `scene/Scene.cpp`,
`scene/Animation.cpp`, `scene/InstanceTable.cpp`, `scene/NavMesh.cpp`, `scene/Audio.cpp`,
`ai/Planner.cpp` and `core/Config.cpp`. Both `substrate-bake` and `substrate_tests` link them
with no volk and no glfw, and the CMake comment at line 766 says "No volk, no glfw. That
absence is the check."

What is missing is the *loop*. `engine/Engine.cpp` is not in that set, so the ordering
`Engine.h:66-76` documents — the fixed-step budget, scene load, the physics/animation/particle/
audio call order, the save path — exists only inside a class that opens a device in its
constructor path. A server author reimplements all of it against private members and drifts.

The related refusal in [limitations.md](../../architecture/limitations.md) is about
*networking* ("the gate is a target, and none exists"), which is a different question: a
headless loop is useful for CI and batch work with no network anywhere near it. No document
contains the words "headless server", so this is unstated rather than declined.

Expected to be wrong about: how much of `Engine` survives the split. The honest answer may be
that the frame loop factors into a simulation half the hosted set links and a presentation
half it does not, and that the ~180 lines of benchmark content authoring currently inside
`Engine::run` have to move first — see the chore that names them.

**That chore does not exist.** Searched for while this card was executed: no card in any
column names the benchmark content in `Engine::run`, and the sentence above is a forward
reference to something never written. It also turned out not to be a blocker — see the
Outcome.

## Verification

- `./test.sh debug` and `./test.sh asan`. The new loop is hosted by construction, so the unit
  suite is where it runs; a scene stepped 10,000 times with no device is the case.
- A scaffolded game builds and runs without touching `engine/`, and the same game builds as a
  device-free target.
- `inspection` for the link itself: the server target must fail to link if it acquires a
  Vulkan dependency, the same way the hosted set does today. That absence is the check.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — "What each configuration can and
cannot do", which gains a row, and [limitations.md](../../architecture/limitations.md), which
currently says nothing about process shape.

## Outcome

`scene::Simulation` is the answer, and it is smaller than the card feared. It holds the ten
things a step moves — sprite table, animator, locomotion driver, particle system, physics
world, cloth, audio engine, fixed clock, scene tree, and the body each sound rides — and
`Simulation::step` is the one written copy of the order they move in. It names no Vulkan type,
so it went straight into `SUBSTRATE_HOSTED_SOURCES`. `Engine` holds one; `Engine::simulate` is
now a line that calls it.

**`Engine::simulate` was already 100% device-free, and that is what made the row an L rather
than an XL.** The frame loop did not have to factor in two. The card's expectation that ~180
lines of benchmark content in `Engine::run` had to move first was wrong: `run` was not touched
at all, because the thing worth sharing is the *step*, not the loop that drives it. A loop is
five lines a caller writes; a call order is what drifts.

`tools/sim.cpp` and the `substrate-sim` target are the second caller — `SUBSTRATE_HOSTED_SOURCES`
plus `scene/SceneParse.cpp`, and **no volk, no glfw**. `scripts/sim.sh` is the wrapper, shaped
after `scripts/bake.sh`. `--headless` is left exactly as it was: it unmaps the window and still
creates a real surface, which is the thing this is not.

Two decisions worth recording because both were tempting the other way:

- **Reference aliases rather than an accessor apiece.** `Engine` declares
  `scene::PhysicsWorld& physicsWorld = sim.physics;` and nine others, so roughly two hundred
  existing uses read as they always did and the diff is the declarations. An accessor per
  subsystem would have been the same change plus two hundred call sites to rewrite for
  nothing.
- **`sim` is declared exactly where `spriteTable` was.** Members are destroyed in reverse
  declaration order, cloth holds bodies in the physics world, and every subsystem absorbed was
  declared after that point — put it anywhere else and the teardown order silently changes.
  The relative order inside `Simulation` is the order they had inside `Engine`, for the same
  reason.

### What this does not do, and the card was wrong about one of them

**A scaffolded game cannot build as a device-free target.** `Game::init`, `frameUpdate` and
`fixedUpdate` all take an `Engine&`, and `Engine.h` reaches the renderer, so a `Game` subclass
cannot compile into a hosted target at all. The card's second verification bullet asked for
exactly that. Splitting `Game` in two to satisfy it would be a base-class hierarchy for a
consumer nobody has, so the honest answer is the one recorded in `limitations.md`: a headless
caller drives `Simulation` directly, which is what `tools/sim.cpp` demonstrates. The scaffold
check was run in the form that is meaningful — a scaffolded game still builds and runs with
nothing under `engine/` edited.

**The world build did not come along.** `Engine::initPhysics` creates a scene node per driven
body, binds each audio source to the body it rides, pairs rigs and builds the cloth *while* it
walks the collider table, and all of that is in the device-side translation unit. `tools/sim.cpp`
has a dozen lines of its own that create bodies and characters and nothing else — so a scene
whose behaviour depends on a sound following a body will not reproduce under `substrate-sim`.
That is a real divergence, it is in `limitations.md`, and the fix is
`Simulation::build(const SceneData&)` with `initPhysics` calling it rather than a second walk
kept in step by inspection.

### Verification

- `./test.sh debug` — 1007 tests from 103 suites, all passed.
- `./test.sh asan` — 1007 tests from 103 suites, all passed.
- Four new hosted cases in `tests/SimulationTests.cpp`. The one the card asked for is
  `TenThousandStepsReachASteadyStateAndStayThere`: 2,000 steps to settle, then 8,000 more, with
  the character and a dynamic crate held to a millimetre and **both table sizes asserted
  unchanged** — a step that appends a slot per iteration is the leak a long run exists to
  catch and it is invisible in a position. The others cover a drop-and-land, an empty world
  stepped 600 times, and two independent `Simulation`s agreeing exactly after 600 steps, which
  is the property a batch tuning job rests on.
- `scripts/golden.sh` — all 11 cases match, byte-identical. This was the check that mattered:
  `Engine`'s member layout changed and `simulate` became a delegation, and the pixels did not
  move.
- **`inspection`, and it is a real check rather than a reading.** `./build.sh debug --target
  substrate-sim` links, and `nm -uC build/debug/substrate-sim | grep -iE "vk[A-Z]|glfw|volk"`
  is empty against 179 undefined symbols in total. The day something device-side reaches
  `scene/Simulation.cpp`, this target stops linking — the same mechanism that guards
  `substrate-bake` and the unit suite.
- The loop runs: `scripts/sim.sh debug engine/assets/physics.gltf --steps 600` reports
  `14 bodies, 1 characters` and a grounded character at `3.400 0.900 3.000`, and `--steps 10000
  --quiet` completes in seconds. `physics.gltf` is the golden suite's own physics case.
- `scaffold` — `./new_game.sh scaffoldcheck`, `./build_game.sh scaffoldcheck debug`, then
  `./run.sh scaffoldcheck debug -- --headless --frames 60 --validation on`: all three clean,
  zero VUIDs, **nothing under `engine/` edited to make it build**. The card's other half —
  that the same game also build as a device-free target — is refused above with its reason.
