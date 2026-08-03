---
id: C21
title: A glTF imported at a transform
arc: C
size: M
verification: golden-11, tests-4, validation
---

# C21 — A glTF imported at a transform

Afterwards the tree holds one engine verb — `addScene(path, transform)` — that loads a
second document into the running world at a place of the caller's choosing and returns a
handle it can be removed by. It merges what a scene *has*: placements, colliders, lights,
emitters and audio sources, each carried through the transform on the way in.

`GltfScene::appendModel` is the closest thing today and it brings geometry and materials and
nothing else. A file's `substrate_collider`, its `KHR_lights_punctual`, its
`substrate_emitter` and its `substrate_audio` are all dropped on the floor, and it appends at
the document's own coordinates because it takes no transform at all. So a game that wants a
mirror over there and a colonnade over here cannot say so.

**What this is really paying off is `scripts/make_composite_scene.py`.** Sponza and a Mixamo
rig are third-party files that cannot be committed, so a Python script grafts them into
`showcase.gltf` at build time and the demo loads the result. That script exists because the
engine cannot compose at runtime — it is a workaround wearing the shape of an asset
pipeline, and every scene the demo will ever want is another invocation of it.

The transform is the whole of why this is not just "appendModel, but more of it". A merged
collider has to arrive in world space with its shape's scale in the right factor — the split
`scene::scaleSceneData` already draws between what grows and what is only carried is the
same one, and this is its second caller.

Expected to be wrong about: whether one handle per import is enough granularity for removal,
or whether callers will immediately want to move an import after the fact. C10 shipped
unloading, so the removal half has precedent to copy rather than invent.

Deforming meshes are explicitly **not** in this row — see C22. An import carrying one is
refused with the message `appendModel` already prints.

## Verification

- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan`, each its own
  invocation. New hosted cases cover the transform composition and the collider/light merge.
- `scripts/golden.sh` — eleven cases, byte-identical. **No golden case loads a composed
  scene**: the suite pins `engine/assets/Sponza/glTF/Sponza.gltf` directly and the skinned
  case uses `skin.gltf`, so a moved pixel here is a defect in the single-document path.
- Zero validation errors with layers on, in a capture taken after an import and after the
  removal that follows it.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — "A second document, at a
transform", under glTF loading. The card named `architecture/scene.md`, which does not exist
and never did; the six files under `architecture/` are split by *system* rather than by
source directory, and the loader's remit lives in `systems.md`.

## Outcome

Landed as `Engine::addModel(path, transform)` rather than the `addScene` this card opened by
naming. The verb that takes a path and gives back a `ModelId` already existed and already had
that name; adding a second one differing only in whether it registers the file's lights would
have been two doors onto one room, so the transform and the rest of the document went onto the
verb that was there. The card's first paragraph is left as written -- what it asked for is
what got built, under the name the tree already used.

Verified: `tests-4` (922 tests), `golden-11` byte-identical, zero validation errors, and the
demo's `Z` places `physics.gltf` six metres down the camera's forward so successive presses
land in different places instead of stacking one file on the origin.
