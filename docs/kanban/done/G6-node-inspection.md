---
id: G6
title: Node inspection
arc: G
size: S-M
verification: golden-11, tests-hosted, validation
---

# G6 — Node inspection

**This card arrived as a stub.** Its whole body was the string `S-M` — no surface, no
scope, no exclusions — and its verification was `golden-12, scaffold`, neither of which can
see anything this row does. What follows was reconstructed from the retired
`ROADMAP-GAME-API.md` row it came from, which said it in one sentence:

> `drawNodeInspector` beside the existing `drawInstanceInspector`: hierarchy, local TRS, and
> the attachment record. **A second function, not a registry** — which is exactly what
> `Inspector.h` predicts the second one will be. Needs G3 to have nodes at all.

## What G3 left, and what it did not

G3 landed `scene::Scene` with `name`, `parent`, `find`, `attachments`, `valid`,
`localPosition`/`localRotation`/`localScale`, `worldTransform`, `liveCount`, `slotCount` and
`order`. Every one of those answers a question about a node the caller **already holds**.

Nothing answered *which nodes are there*. `order()` hands out `uint32_t` slots and a slot
cannot be passed back to any call on the class, so a panel, a listing or a console had no
way into the tree at all — the data was complete and unreachable. That is the gap, and it is
what makes this a **reach** row under [arcs.md](../arcs.md) rather than a capability one: it
adds nothing the engine could not already represent.

## Scope

**In:**

- `Scene::idAt(slot)`, `firstRoot()`, `firstChild(id)`, `nextSibling(id)` — the walk, over
  the sibling list `resort` already maintains. `idAt` is spelled the way
  `InstanceTable::idAt` is because it is the same operation on the same shape of table.
- `Scene::structureRevision()` — bumped by create, destroy, reparent and clear, and by
  nothing else. It is what lets a listing that costs a `std::string` per node be rebuilt
  when the tree moves rather than every frame.
- `ui::drawNodeInspector`, `ui::nodeCaption`, `ui::NodeInspectorState` in
  `engine/ui/Inspector.{h,cpp}` — hierarchy, local TRS, and the attachment record.
- The demo as its consumer, so the panel has a caller outside the unit suite.

**Out, and each for a reason rather than for room:**

- **A property registry.** `Inspector.h` refused one and named the third inspected thing as
  the trigger. This is the second, and it is a second function exactly as that refusal
  predicted. Bringing the registry forward to two would be abstracting at the coincidence.
- **A second base class, a `Panel` interface, or anything a third panel would inherit.**
- **Selection by picking in the viewport.** That is a spatial query against instance bounds
  and it belongs to C9's index, not to a panel.
- **Editing the hierarchy** — no reparent-by-drag, no create, no destroy. A node inspector
  that restructures the scene is a scene editor, which is a different row and not one that
  exists.
- **Rotation editing.** Three sliders over a quaternion's components make an unnormalised
  quaternion between any two frames of a drag, and Euler angles are a second representation
  to keep in step with the first. Stated in the header rather than left to be discovered.

## Verification

Everything below must pass before this may enter `done/`:

**The card's original verification was wrong and is replaced.** `golden-12` names a suite
that is now eleven cases, and byte-identical output proves only that a read-only panel
changed no pixel — it cannot tell whether a query returns the right node. `scaffold` proves
a game links. Neither has ever checked the thing this row is.

- `scripts/golden.sh check release` — eleven cases, byte-identical. Kept as the regression
  guard it is: nothing here may move a pixel.
- `./test.sh debug` and `./test.sh asan` — green, including **hosted tests that assert the
  queries answer correctly**: a slot resolves to the handle issued for it and a dead slot to
  no handle at all; the sibling walk reaches every live node exactly once and agrees with
  `parent()`; a reparent leaves neither child list broken; a destroyed subtree leaves the
  walk entirely; the revision moves for structure and not for motion; `find` answers with
  the node that has the name, and stops answering when it dies. Then the panel on top of
  them: the listing is depth-first, the caption names the node and what hangs off it, the
  detail pane reads the *selected* node, and an edit goes through `Scene`.
- A validation run of the demo with the panel open, since the overlay is something the
  renderer sees.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**A stub, and the first job was to find out what it meant.** The body was the string `S-M`.
The row's actual scope was recovered from the retired `ROADMAP-GAME-API.md` — one sentence,
quoted at the top of this card — and the card was given a body and a real verification
before any code was written.

**Outcome 1 of the three: G3 built most of it and one specific gap remained.** G3's `Scene`
already had every query about a node a caller holds — `name`, `parent`, `find`,
`attachments`, `valid`, all three local components, `worldTransform`, `liveCount`,
`slotCount`, `order` — and it had `Attachments` carrying all six kinds plus `instanceOffset`.
None of that needed rebuilding or revisiting. What it had no answer for was *which nodes are
there*: `order()` yields slots, a slot cannot be passed back into any call on the class, and
there was no accessor for a node's children. The tree was complete and unreachable from
outside, which is precisely the shape of a **reach** row.

### What landed

**Four accessors and a counter on `Scene`, and no new structure.** `idAt(slot)`,
`firstRoot()`, `firstChild(id)` and `nextSibling(id)` expose the sibling list `resort` was
already walking — so a depth-first listing costs one visit per node rather than a scan per
node, and `order()` stays breadth-first and stays the sweep's. `structureRevision()` moves on
create, destroy, reparent and clear and not when a node moves, which is what keeps an
animated scene from rebuilding a `std::string` per node per frame. The private member
`firstRoot` became `rootList` to free the name; nothing outside the class could see it.

**`idAt` on a dead slot yields an invalid handle rather than the generation the slot is
holding**, and that is the one decision here worth arguing. The alternative compares equal to
the handle the *next* `create` in that slot issues — the silent alias generations exist to
stop — reached through the one call a listing makes on every node it prints. It has a test.

**`ui::drawNodeInspector`, in `Inspector.cpp` beside the function that predicted it.** A
depth-first listing with an indent and one letter per attachment kind, then identity,
hierarchy, local transform, world position and the attachment record for the selection.
Three things differ from the instance panel and each has its own reason, recorded in the
header: selection is a *row of the listing* rather than a slot, because a reparent reorders
the listing without destroying anything and only a row survives that; position and scale are
editable where the instance panel's are not, because a node stores TRS as TRS and there is no
lossy round trip to avoid; and there is a **`driven`** row, because `Scene` takes a solver's
matrix verbatim and never writes the local TRS back, so on a node with a body or a character
the sliders move a number nothing reads. The last of those is the same call the instance
panel's `visible: gpu-side` row makes: a fact about the object, stated, rather than a widget
that silently does nothing.

**The demo splits its inspector column between the two panels** — what the table holds and
what the tree holds are two views of the same object, and a reader compares them. The engine
still owns neither position: where a game puts a panel is the game's, which is the same split
`Inspector.h` already argued about `drawSettingsPanel`.

### What was deliberately not built

No property registry. `Inspector.h` refused one and named *the third* inspected thing as the
trigger; this is the second, and it is a second function exactly as that refusal predicted.
What the two panels actually share is a caption cache keyed on a revision and a selection
index that clamps — four lines each, and neither is a schema. Every property either of them
names is unique to it, because an instance is a flat row of a table and a node is a tree.
`limitations.md` records that the trigger is now one away rather than fired.

No viewport picking (a spatial query, and C9's), no hierarchy editing (a scene editor, and
not a row that exists), no rotation slider (an unnormalised quaternion between any two frames
of a drag).

### Verification

- **`scripts/golden.sh check release` — 11 of 11 byte-identical.** The suite is eleven cases;
  the card said twelve, which is what `no-ibl`'s retirement left behind.
- **`./test.sh debug` — 778 tests, 83 suites, all green. `./test.sh asan` — the same 778, all
  green.** Seventeen are new: nine in `tests/SceneTests.cpp` under a `walking it` heading,
  and eight in `tests/InspectorTests.cpp` under a new `NodeInspector` suite — the
  eighty-third. **These are the check the card never had.** A byte-
  identical golden image cannot tell you that a listing names the right node — the panel is
  not in any golden frame at all — and `scaffold` cannot tell you anything beyond "it links".
  The suite pins that a slot resolves to the handle issued for it, that a dead slot resolves
  to nothing, that the walk reaches every live node exactly once and agrees with `parent()`,
  that a reparent leaves neither child list broken, that a destroyed subtree leaves the walk,
  that the revision moves for structure and not for motion, that `find` answers with the node
  that has the name and stops when it dies, that the listing is depth-first with a subtree
  contiguous under its parent, and that the detail pane reads the *selected* node rather than
  the first one.
- **`./run.sh demo debug -- --headless --locked --frames 150 --validation on --input-script
  "20:Toggle.Inspector" engine/assets/physics.gltf`** — exit 0, `Inspector: on` at frame 20,
  **zero validation errors**. `physics.gltf` on purpose: it is the scene where the engine
  builds a real hierarchy, a body node with a `mesh` child and sound children under it, so
  the panel is drawing a tree rather than a flat list of roots. C16's input script is what
  makes that reproducible.
