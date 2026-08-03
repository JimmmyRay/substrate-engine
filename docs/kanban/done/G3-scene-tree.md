---
id: G3
title: Scene tree
arc: G
size: L
verification: golden-12, tests-hosted, validation
---

# G3 — Scene tree

L

## `fixedUpdate` — manipulating the tree at runtime

```cpp
void DemoGame::fixedUpdate(Engine& e, float step) {
    Scene& scene = e.scene();
    const input::InputMap& in = e.input().map();

    scene.setCharacterInput(player, in.axis2(act.move), in.pressed(act.jump));

    // Reparent. The torch's light and its sound follow, because they are attachments on
    // a node rather than entries in three unrelated flat lists.
    if (in.pressed(act.interact)) {
        carrying = !carrying;
        scene.setParent(torch, carrying ? player : level);   // keeps world transform
        scene.setLocalPosition(torch, carrying ? glm::vec3{0.3f, 1.3f, 0.2f}
                                               : glm::vec3{-4.0f, 2.2f, 0.0f});
    }

    // Mutate an attachment. A light has no derived state, so this is a reference; a
    // transform does, which is why setLocalPosition above is a call and not `=`.
    phase += step;
    scene.light(torch).intensity = 40.0f + 6.0f * std::sin(phase * 7.0f);

    // Mutate a material. The buffer is revision-counted and re-uploads when it moves,
    // the same way the instance table already does.
    e.assets().setMaterialParam(dissolving, 0, 0.5f + 0.5f * std::sin(phase));
}
```

**The mutable-reference-versus-call distinction is deliberate and is the rule everywhere in
this API:** a value with derived state behind it is written through a call, and a value
with none is handed out by reference. `setLocalPosition` invalidates a world transform, a
world bounding box and a normal matrix, which is exactly why `InstanceTable::setTransform`
is already a call rather than a reference handed out. A light's intensity invalidates
nothing — lights are re-uploaded every frame regardless.

## `Scene` — the new structure

`engine/scene/Scene.{h,cpp}`. Structure-of-arrays over dense storage — the shape
`InstanceTable` already is — with a `Node` handle of index plus generation, for the same
reason `InstanceId` has one: a stale handle must be detectable rather than a silent alias
onto whatever was created in the slot afterwards.

**Per node:** `parent`, local translation/rotation/scale, a cached `worldTransform`, a
dirty bit, a name, and one flat attachment record —
`{instance, light, body, sound, emitter, character}`, each an index defaulting to invalid.
Twenty-four bytes, no allocation, no container. The precedent is `Placement::colliderNode`,
which is one inherited index doing exactly this job for one attachment kind.

**Slots never move**, because handles key off them — the load-bearing property
`InstanceTable` already documents and which rules out swap-and-pop compaction. Reparenting
therefore cannot reorder the array, so a separate `order` array holds a topological
ordering, rebuilt only on a **structural** change (create, destroy, reparent). The
per-frame world-transform sweep walks `order` linearly, parent before child — which is
exactly what `Pose` already does for the animation rig.

**Downstream push, once per frame, dirty nodes only:** `InstanceTable::setTransform`, light
position and direction, audio source transform, kinematic body transform. **Upstream pull**
for dynamic bodies — which is what `drivenInstances` does in `main.cpp` today and what
`Scene` absorbs.

**The animation rig stays the animator's, and that resolves cleanly.** A skinned mesh is
*one* instance carrying a character index; joints never become instances, and joint
matrices reach the GPU through `skinning.comp` with no node existing for them. There is
therefore no second hierarchy to merge and no risk of two node arrays disagreeing. The one
case that genuinely wants rig nodes is attaching an object to a joint, and that is a lookup
— `scene.attachToJoint(node, player, "Hand.R")` — not a merge.

**What `Scene` deliberately is not:** an ECS, a component system, or a place for gameplay
fields. It holds transforms, hierarchy and attachment indices. A registry adopted later
sits *beside* it holding a `Node` as a component, which is the same argument already made
for why `entt` cannot own the instance table — its storage is paged and its removal is
swap-and-pop, and both properties are exactly what the slot-stability contract forbids.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**`engine/scene/Scene.{h,cpp}` landed as designed, and the two places the design was wrong
are the interesting part of this entry.**

### What the card got right and did not have to change

Structure-of-arrays over dense storage, a `NodeId` of slot plus generation, slots that
never move, a separate topological `order` rebuilt only on a structural change, and the
per-frame sweep walking it linearly. The flat attachment record. The rule that a value with
derived state is written through a call and a value without one is handed out by reference.
Downstream push for instance, light, sound and emitter; upstream pull for a body. All of it
survived contact.

### The first thing the card had wrong: the pull cannot decompose

The card said a node's local transform is translation, rotation and scale, and that a body
drives the node. Both are true and together they are a trap: a body's transform arriving as
a local TRS means `compose(decompose(m))` every frame, which is exact in mathematics and
not in floats. **The golden set compares images byte for byte**, so that is a moved pixel
with no cause.

What landed instead: a driven node's `world` **is** the solver's matrix, written directly,
and its local TRS is not written back at all. The consequence is stated on
`worldTransform` rather than hidden -- `localPosition` on a driven node is not meaningful.
The same argument decided `Attachments::instanceOffset`: `DrivenInstance::localOffset`
survives as a matrix on the attachment rather than dissolving into a child node's local
transform, for exactly the reason above. **All twelve golden cases came out byte-identical
on the first run after the physics path moved onto the tree**, which is the evidence that
this was the right call and not caution.

### The second: `resort` cannot search for children

The first version found a node's children by scanning every node for a matching parent
index. That is a scan per node per structural change -- a hundred million comparisons on a
ten-thousand-node scene, which is not a large scene. It was rewritten before it ever ran on
anything: `firstChild`, `nextSibling` and `prevSibling`, doubly linked because unlinking is
what reparenting does. Every structural operation is now linear in what it touches, and
`destroy` walks only the subtree it is destroying.

`markSubtreeDirty` disappeared in the same change and is worth recording as a deletion: the
sweep already walks parent before child, so a child sees a dirty parent on the pass that
computes its world transform. A separate subtree walk per write was the same work twice.

### What it deleted, and what it added by deleting it

`Engine::DrivenInstance` and `DrivenSource` are gone -- the structs, the two vectors, and
the two loops -- along with `Engine.h`'s own comment saying *"this exists because there is
no scene tree, and G3 deletes it."* What replaced them is strictly more: a light, an emitter
or a sound on a node follows it now, and so does anything a game parents to one, which is
what the demo's torch does in five lines.

**Two behaviour changes fell out and neither is an accident.** A body-driven *sound* now
reads the frame's alpha rather than alpha 1, and the occlusion pass reads a position set at
the end of the previous frame. The old code deliberately used alpha 1, on the grounds that
a mixer wants where a source is rather than where it is drawn; the tree gives one transform
per node per frame, and having audio see a different one would put back exactly the
divergence the tree exists to remove. The difference is one step of motion.

The light push writes only what a light's *type* has -- position for a point, direction for
a directional, both for a spot. Found by asking what a node attached to a point light
should do with its `direction`, and the answer is nothing.

### What this needed from physics

`PhysicsWorld::setBodyTransform` and `bodyKinematic`, because the card's downstream-push
table has a kinematic row and there was no way to move a body from outside the solver at
all. It refuses a dynamic body with a reason, teleports rather than sweeping -- Jolt's
`MoveKinematic` derives a velocity from a target and a step length, and this is called from
a frame -- and writes both interpolation snapshots, because a body that moved without the
solver moving it has no previous state worth interpolating from.

### What is not in it

`scene.attachToJoint(node, player, "Hand.R")`. The card argues correctly that the rig stays
the animator's and that attaching to a joint is a lookup rather than a merge, and that
remains true; nothing asked for it yet, and the lookup it needs is a name-to-joint search
the animator does not currently expose. `Assets::setMaterialParam` from the card's sketch is
G4 and stayed there.

### Verification

- `scripts/golden.sh check release` -- **all 12 byte-identical**, including `physics`,
  which is the case that exercises the whole driven path. One run reported `particles`
  differing and the log said `vkCreateDevice failed: VK_ERROR_DEVICE_LOST` -- a driver
  failure before a frame was drawn rather than a rendering difference; the re-run matched.
- `./test.sh debug` and `./test.sh asan` -- 641 tests, both green. Fifteen are new and
  every one pins a property the sweep depends on: order is parent before child after any
  structural change, a destroyed subtree leaves no live handle, a reparent keeps the node
  where it is, an unchanged node writes nothing.
- `./run.sh demo release -- --headless --locked --frames 120 --validation on
  engine/assets/physics.gltf` -- zero validation errors.

The validation run also caught something the compiler could not: `E` was already
`Camera.Up`, and the input map said so at startup -- *"'Camera.Up' and 'Scene.Carry' both
fire on E"*. The torch is on `U`.
