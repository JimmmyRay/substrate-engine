---
id: bug-empty-scene-bring-up
title: Empty scene bring-up
arc: bug
size: S
verification: scaffold, validation, tests-hosted
---

# bug-empty-scene-bring-up — Empty scene bring-up

A game scaffolded by `./new_game.sh`, run with the `setup.scene.clear()` the template ships
with, segfaulted during bring-up. Given a scene on the command line the same binary ran
clean, so this was the *no scene* configuration specifically -- the one
[G1b](../done/G1b-project-scaffolding.md) declared supported and the one every scaffolded
game starts in.

## What broke

`vkCreatePipelineLayout` for the shadow pass, with a null `VkDescriptorSetLayout` in set 1.
The validation layers named it -- `VUID-VkPipelineLayoutCreateInfo-pSetLayouts-parameter`,
`Invalid VkDescriptorSetLayout Object 0x0` -- and the driver then dereferenced it and died.
Instrumenting `Renderer::createLayout` put it beyond inference: `createLayout(shadow) set 1 =
(nil)`, and the line logging what that call returned never printed.

Set 1 is the scene's. `GltfScene` creates its descriptor set layout in `buildDescriptors`,
which is called from `upload` and from nowhere else, so a `GltfScene` that never loaded a
document holds `VK_NULL_HANDLE`. `Renderer::setScene` registers that handle and calls
`createPipelines()`, whose first layout is the shadow one -- so bring-up died on the first
pipeline layout the engine builds, before a frame was recorded.

G1b found half of this and fixed half of it. It stopped `Engine::loadScene` from exiting
through `Logger::critical` on an empty path and left a comment claiming an unloaded
`GltfScene` "is the same shape as one holding a file with no meshes in it". It is not: a file
with no meshes still goes through `upload`, and an unloaded scene owns no buffers, no
descriptor pool, no set and no layout. That is the assumption this card corrects.

## What was done

`GltfScene::createEmpty` hands a default-constructed `SceneData` to `upload` -- the same code
path a document takes, not a second one that builds "the parts an empty scene needs".
`Engine::loadScene` calls it on the empty branch. Every zero is a case the loader already had
to survive, because a glTF with no images, no materials and no primitives is legal, and the
capacities `upload` always adds (a quarter over, plus 1024 vertices, 1024 indices and 64
materials) mean the buffers are real rather than zero-sized.

What that buys beyond bring-up: `createMesh` and `appendModel` sub-allocate out of those
buffers, so a game with no scene can now build geometry in code -- which is the workflow the
scaffolded template exists for and was, until this, unreachable. Verified rather than
asserted; see below.

`upload` reports on a document it does not have when the path is empty, so it prints one
line naming the room and the texture slots instead of eight lines of zeroes.

## What makes it stay fixed

Nothing under `engine/scene/GltfScene.cpp` is in `SUBSTRATE_HOSTED_SOURCES`, and neither is
`Engine.cpp`; both need a `VkDevice`. **This defect is not reachable from the unit suite and
no test here would touch it.** Said plainly rather than covered by a test of something
adjacent. What holds it is the `scaffold` token itself, now that it means what it says: the
verification below runs a scaffolded game with no scene for sixty frames rather than stopping
at "it builds and links", which is the reading that let this through.

## Verification

- `./test.sh debug` -- 677 tests in 74 suites, 0 failures.
- `./test.sh asan` -- 677 tests in 74 suites, 0 failures.
- `./new_game.sh emptycheck && ./build_game.sh emptycheck debug`, then
  `./run.sh emptycheck debug -- --headless --frames 60 --validation on` -- exit 0, zero
  validation errors, `prims 0 (0 visible)`.
- The same game with a scene: `./run.sh emptycheck debug -- res:/emissive.gltf --frames 30
  --validation on` -- exit 0, `Instances: 3 live`.
- `./run.sh demo debug -- --headless --frames 60 --validation on` -- exit 0, zero validation errors,
  `prims 107 (81 visible)` -- the loaded-scene path unchanged.

## Reference update

[guides/making-a-game.md](../../guides/making-a-game.md) -- what a game with no scene gets.

## Outcome

**A one-line call and a five-line method.** The whole fix is `GltfScene::createEmpty`
delegating to `upload`, and the size of it is the finding: the empty scene needed no special
case anywhere downstream, because everything downstream had always been written against a
scene with nothing in it. The one thing genuinely missing was the GPU-side scene itself.

**The estimate did not predict the second half.** The card was opened on a segfault, and the
same fix turns `createMesh` from "crashes at bring-up" into "works" for every scaffolded game
-- checked with a temporary triangle built in the template's `init`, which drew as `prims 1 (1
visible) | tris 1` with the layers on. G4 shipped that API and no game with no scene could
call it.

**A checker rule the board had never exercised.** This is the first kinded card, and
`scripts/kanban.py` computed its filename as `<id>-<slug of title>.md` -- which for an id that
already *is* the title's slug names the file twice over. A kinded card's filename is now
`<id>.md`, and the id is checked against the slug of its title instead, so id, title and
filename still cannot drift.

**Found and left alone:** the scaffolded template declares `App.Panel` on Tab while the
engine's own `Menu.Bindings` already holds it, and every scaffolded game logs
`Input: 'Menu.Bindings' and 'App.Panel' both fire on Tab` on startup. Real, unrelated to this
defect, and its own card.
