---
id: G9
title: The demo, as everything the engine can do
arc: G
size: M-L
verification: golden-11, scaffold, validation, trace, readback
---

# G9 — The demo, as everything the engine can do

M-L

## Notes the G9 row carries

**Why the demo is a row at all.** Everything above it is a claim that a game can be written
against this engine, and the only proof of that claim is a game that is. G1b makes half of
it checkable — a scaffolded game must build without including anything under `engine/` — and
that half only proves the surface is *reachable*, not that it is *sufficient*. G9 is the
other half: a game that uses every subsystem the engine has, in one scene, at the same time.

**It was going to replace content authored in the wrong place, and it could not.**
`showcase.gltf` is Sponza with a mirror, an orb, a character and two sounds grafted on by a
160-line Python function, and the card's original plan was to delete `build_showcase` on the
grounds that after G4 the same scene is a few calls in `init`. That plan is wrong at HEAD
and the reason is recorded below: `Engine::addModel` brings geometry and nothing else. A
model appended in code contributes vertices, materials and render instances; its colliders,
emitters, sounds, lights and rig are parsed and never handed to a subsystem, because
`Engine::loadScene` is the only place that does the handing. Deleting `build_showcase`
would have cost the demo its floor, its character, its rig and its ambience, and buying
them back is an engine change — which is exactly what this row must not make.

So `showcase.gltf` and `make_composite_scene.py` are **untouched**, and everything this row
adds is built in `Game::init` out of public calls. That turns out to be the sharper
version of the same claim: the content is not merely *in* a game, it is *expressible* by
one, and the places where it was not are the row's actual output.

**Three facts the card carried were stale, and are corrected here.**

- It said *"all fifteen cases pin Sponza …"* while its own verification block said twelve.
  The suite is **eleven** — it was twelve until `no-ibl` was retired, the flag having
  outlived the feature it tested. `golden-12` is `golden-11` throughout.
- It said *"after G4 it is a `loadModel` and twenty lines of `init`"*. There is no
  `loadModel`. The verbs are `Engine::addModel`, `Engine::createMesh` and
  `GltfScene::appendModel` — and see above for why the sentence was wrong beyond its
  spelling.
- It said the four brazier lights are *"declared non-shadowing"*. **Nothing can be declared
  non-shadowing.** `castsShadows` and the `substrate_light` extra that carried it were both
  removed with the shadow rewrite; `updateLights` assigns the 24-layer atlas first-fit in
  light order, and a light that does not fit illuminates without occluding *and logs an
  error*. The budget in the card is real and the mechanism is not, which changes what
  respecting it costs — see the two numbers below.

**What it adds, and which row each piece proves:**

| Content | Proves |
|---|---|
| Four braziers, each a node tree of a stem, a bowl, coals, a light, three emitters and a looping crackle | G3's attachment record and parenting, and the first particle emitters the demo scene has ever had |
| A crate stack, barrels, a ramp, and a sliding kinematic platform | The three `ColliderMotion` values nothing in the tree used |
| A crate landing plays a sound and throws dust | G7's `contacts()` and `playAt`, and C3's `spawnEffect`, on one event |
| Two hundred and forty instanced urns | GPU culling with something to cull, and G4's `createMesh` at a scale that finds its edges |
| Scorch decals under the braziers | Projected decals placed by a game — through `gfx::decalAt` rather than `GameSetup::decals`, and the reason is below |
| `game/demo/assets/audio/fire_crackle.wav`, generated | The decode side of S5.2's crossover taken by a *looping* source, four of them on one file |

**Two things the original table asked for moved to their own cards rather than being
half-built here** — a morphed banner (G11) and WASD locomotion through a six-state machine
(G12). The split and its seam are argued in the Outcome.

**Two numbers the row has to state rather than discover.** `render.particleBudget` defaults
to zero, meaning sized from data, so the pool is `ceil(rate x maxLifetime) + 1` summed over
twelve emitters — 4 x 84 for the fires, 4 x 88 for the smoke, 4 x 95 for the embers, which
is **1 068 particles rounding to a capacity of 2 048** — and `droppedSpawns()` must stay at
zero against it. And the punctual atlas is **24 layers**, of which the showcase already
spends 18: six on the orb light and six on each of the two fills the interior's shadows
come from. Four brazier *points* would need 24 more and evict all three. With no
`castsShadows` to decline with, the lever the engine actually offers is the light's
**type** — a spot takes one layer — so the braziers are wide upward spots at **22 of 24
layers used and no overflow**, which is also the more honest light for a bowl that occludes
its own fire downward.

**The golden set does not move, and that is checkable rather than hoped for.** All eleven
cases pin Sponza, `emissive.gltf`, `particles.gltf`, `skin.gltf` and `physics.gltf`;
`showcase.gltf` was never a case. But `scripts/golden.sh` runs *the configured game's
binary*, so a demo that built its world unconditionally would have moved all eleven images
and destroyed the claim. `demoWorldApplies` is the gate — the scene that loaded is the scene
`configure` named — and with it in place, a moved pixel during G9 means the row touched the
engine, which it did not.

**What this row does not wait for.** The C arc extends it, and the extensions have landed
around it: spawning and destroying crates (C1), a raycast on the interact key (C2), effects
at a contact point (C3), pause (C4), a HUD (C5), footsteps on the frame the foot lands (C7).
None of that gated G9 and G9 gates none of it: each lands as one more thing the scene does.

---

## Verification

Everything below must pass before this may enter `done/`:

- `scripts/golden.sh check release` — eleven cases, byte-identical. **The central claim**:
  the demo's own content must be invisible to every case, so a difference here is the
  signal that the row reached into the engine.
- `scripts/readback.sh release` — nine cases bit-identical plus the lit silhouette. These
  run *without naming a scene*, so unlike the golden cases they load the demo's own world;
  what they check is that a game's content does not disturb a texel the presentation path
  promised.
- `./test.sh debug` and `./test.sh asan` — the hosted suite under two configurations.
- A validation-layer run of the demo scene with **zero errors**.
- `scripts/baseline.py` — `Lighting` and `Frame`, quoted for the demo scene with the world
  on and off, so the cost of the content is attributable rather than absorbed.
- **The two numbers the row states rather than discovers**, both reported by the demo
  itself rather than asserted here:
  - `ParticleSystem::droppedSpawns() == 0` against the 2 048-slot pool the twelve emitters
    sized, over a run long enough for every emitter to have reached steady state.
  - The four brazier lights inside the **24-layer** punctual atlas: no
    `Punctual shadow atlas overflowed` in the log of the same run.

## Reference

[guides/making-a-game.md](../../guides/making-a-game.md), and
[architecture/limitations.md § The game API](../../architecture/limitations.md#the-game-api),
which is where what this row *found* is written down.

## Outcome

**Split, and part one landed as G9.** The seam the board suggested was content against
locomotion. The seam taken is sharper and was found by building rather than by planning:
**what a game can build through the engine's C++ API, against what still cannot be built in
code at all.** Everything on the table above is now a call in `game/demo/DemoWorld.cpp`.
The two things that came off it did so because each is blocked on something real —

- **G11, the morphed banner.** Morph targets have no code path. `MeshData` carries
  vertices, indices, a material and a transform and no targets; `createMesh` therefore
  cannot make one; and a morphed mesh loaded through `addModel` is not driven either,
  because weights come from a clip in the rig and `addModel` does not wire the rig. The row
  is a real one and it is an *engine* row, which is precisely why it must not be smuggled
  into G9.
- **G12, locomotion and the six-state machine.** The demo's machine has four states, and the
  two the card wants — `fall` and `land` — exist as clips on the rig (`falling idle`, `hard
  landing`). What that row needs and this one does not is a *reproducible* check, which is
  C16's `--input-script`; landing it beside 265 new instances would have meant a golden
  failure nobody could attribute.

Neither is a fraction of a card. Both have their own verification and both are in
`backlog/`.

**What landed**: `game/demo/DemoWorld.{h,cpp}` (~560 lines), one call in `DemoGame::init`,
one in `fixedUpdate`, one in `playImpacts`, one in `shutdown`, `unitCube` promoted out of
`DemoGame.cpp`'s anonymous namespace because the crates, the ramp and the platform became
its second caller, and `build_fire_crackle` in `scripts/make_test_scene.py`. **No file
under `engine/` was touched, and no scene file was regenerated.**

### The assessment, which is what the row was for

`order.md`: *"the golden set and the unit suite prove nothing broke, and nothing in that
check can tell you whether the API is any good to write against — only writing against it
can."* Concretely, in the order the awkwardness was met:

1. **A mesh made in code gets one instance and there is no verb for a second.** This is the
   sharpest finding and it cost the most lines. `createMesh` makes a model, a primitive and
   one placement, and `SceneTypes.h` states the position deliberately: *"a caller wanting
   forty makes forty instances, which is what the instance table is."* But making the
   fortieth means reading the `Primitive` back out of `gltfScene()` and copying seven GPU
   buffer offsets — `firstIndex`, `indexCount`, `baseVertex`, `vertexCount`, the two bounds,
   the primitive index — into an `InstanceDesc`. `addPlacementInstances` is that function,
   it is public, and it cannot be called: it walks a *placement range* rather than taking a
   transform. Two hundred and forty urns is a fourteen-line private helper that should be
   one call. **And the caller must then call `Renderer::setInstances` itself**, because
   `InstanceTable::create` cannot reach a renderer — miss it and the indirect buffer stays
   sized for the old count and the TLAS never sees the new geometry, which is a wrong
   picture rather than a crash.
2. **`GameSetup::decals` is the wrong door and always was.** `configure` runs before the
   scene is loaded, so a game filling `setup.decals` puts marks into whatever scene the
   command line eventually names — for this game, eleven golden cases. The field has been an
   empty vector since it was added and this row is why. `gfx::decalAt` from `init` is
   correct and is what shipped. A second half is worse: a decal's `textureIndex` is a slot
   in the *scene's* bindless array, which only a glTF writes to, so a game authoring in code
   cannot supply decal art at all — the demo tints texture 0 nearly black.
3. **A light on a node is aimed by the node, and the call that makes it says the opposite.**
   `gfx::makeSpotLight(position, direction, …)` takes a direction; `Scene::update`
   overwrites it from the node's -Z on the first sweep. So the argument survives exactly
   until the first frame and the thing that actually aims the light is a quaternion nowhere
   near the call. It took a screenshot to notice.
4. **Nothing inside a loaded model can be addressed.** The card says "Sponza's four hanging
   bowls, which ship empty". Sponza is *one unnamed node holding one mesh*: the file's node
   names do not survive the flatten and `GltfScene` exposes placements and primitives, not
   names. So the bowls cannot be found and the braziers are fractions of
   `boundsMin`/`boundsMax`, tuned by rendering and looking. **A bounding box does not know
   where the room is** — the first fractions put four braziers inside the arcade behind
   curtains, because Sponza is 18 units deep and its *nave* is a three-metre strip.
5. **Resizing the particle pool is two calls and the second is invisible.**
   `ParticleSystem::setEmitters` sizes the CPU pool; `Renderer::setParticles` sizes the GPU
   buffers from `capacity()`. A game that adds emitters after load must make both, in that
   order. `Engine::loadScene` makes the same pair, which is the evidence that it is one
   operation wearing two names.
6. **A kinematic body is moved by moving its node, and the physics verb is a trap.**
   `PhysicsWorld::setBodyTransform` exists, is documented as the way to move a kinematic
   body, and is the wrong call from a game with a scene tree: the sweep pushes the node's
   world transform into the body every frame, so the direct write is overwritten on the
   frame it is made. This one is *good* design — the attachment moves the mesh too — but
   nothing at either call site says which of the two verbs is the one to use.
7. **`Engine::config()` has no `const` overload**, so `demoWorldApplies(const Engine&)` does
   not compile. A predicate that reads two paths and writes nothing needs a mutable engine.
8. **A game cannot ask which scene it got.** The gate is a string comparison between
   `config().scene.path` and `gameSetup().scene`, correct only because `Engine::init` copies
   one into the other when nothing else claimed it. That is a rule a game has to know, and
   it is the load-bearing line of the row's central verification claim.
9. **`Engine::addModel` brings geometry and nothing else**, which is the finding that
   reshaped the card. Recorded in full at the top.

What was *good*, stated because a list of complaints is not an assessment: `GpuMaterial`
made in code needed no loader; the scene tree's parent/child sweep carried the light, the
sound, the emitters and the mesh of a brazier with no bookkeeping at all; `spawnEffect`
releasing its own slot meant a landing needed no cleanup; `contacts()` handing back a point,
a normal and a *pre-solve* closing speed meant the volume and the dust direction were both
already there; and the whole world builds and tears down with no engine change and no
allocation the game manages.

### The numbers

| | showcase, world off | showcase, world on |
|---|---|---|
| `Lighting` | 2.048 ms | **2.454 ms** |
| `Frame` | 3.717 ms | **4.292 ms** |
| new zones | — | `Decals` 0.052, `Particles` 0.266, `ParticleSort` 0.026 |
| CPU busy | 0.208 ms | 0.677 ms |

Release, 4x MSAA, medians over 717 traced frames in three runs each. The two arms are the
*same scene* — the control names `showcase.gltf` by absolute path, which loads the identical
file and trips the gate off. **`scripts/baseline.py` with no arguments still measures
Sponza** (`Lighting` 1.836, `Frame` 3.171), so the published table in `tooling.md` is
untouched by this row.

- `scripts/golden.sh check release`: **11 of 11 byte-identical.**
- `scripts/readback.sh release`: **9 of 9 bit-identical, plus the lit silhouette and the
  resize soak.**
- `./test.sh debug`: 778 of 778. `./test.sh asan`: 778 of 778.
- Validation, 400 frames of the demo scene: **zero errors.**
- `droppedSpawns()`: **0 over 400 steps**, pool 2 048, twelve emitters.
- Punctual atlas: **no overflow**; 22 of 24 layers, four brazier spots at one layer each.

**One thing found and deliberately not fixed.** `--sync-validation` reports thousands of
`SYNC-HAZARD-READ_AFTER_WRITE` on `depth_pyramid.comp`, `ssao.comp`, `decal.frag` and
`lighting_rt.frag` — and reports the same on bare Sponza with no demo content at all, so it
predates this row and belongs to the engine. It is off by default and `golden.sh` does not
enable it. Recorded here so the next reader who turns the flag on does not attribute it to
G9.

**Reference drift this row found while writing against the documents.**
`limitations.md` still said *"there is no `ContactListener`"*, *"there is no one-shot API"*
and *"kinematic bodies exist and nothing drives one"*. The first two were falsified by G7 and
never updated; the third is falsified by this row. All three are corrected, and the new
section — **The game API** — is where the nine findings above live so that the next game
does not rediscover them.
