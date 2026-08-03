---
id: chore-blender-authors-the-pins-the-engine-reads
title: Blender authors the pins the engine reads
arc: chore
size: S
verification: scripts-fail, inspection
---

# chore-blender-authors-the-pins-the-engine-reads — Blender authors the pins the engine reads

`tools/blender/substrate_pins.py` is a Blender add-on that creates, paints and validates the
`_PIN_WEIGHT` attribute [C19](C19-cloth-that-hangs-from-where-it-is-pinned.md) reads, so the
convention is a tool an artist is handed rather than a string someone has to remember. It is the
first thing in `tools/`.

This row is pointless before C19 — the attribute's contract is C19's to define, and defining it
twice is how the two start disagreeing — but it is not blocked by it either, and it is deliberately
not a dependency of it. C19 verifies against a `cloth.gltf` that `scripts/make_test_scene.py`
writes directly, with no Blender anywhere in the loop, precisely so the runtime is testable without
this and this is testable without a runtime change.

## Why

Tethered had all of this working and none of it written down. Its `FABRIC_` prefix and its
`_PIN_WEIGHT` attribute drove a shipping cloth system, and grepping its docs, its `CLAUDE.md` and
its fifty-seven Blender scripts for either string returns nothing. Its physics guide says "special
mesh naming" and stops. The convention existed only inside one `.blend` file and the memory of
whoever exported it — which is the failure mode this board calls a row rather than a note.

An add-on is the cheapest thing that makes it survive. It is not documentation that can drift from
the code, it is the thing that writes the data, so the spelling cannot be wrong.

## The shape

Three pieces, and they are small:

- **A panel that creates and edits the attribute.** Float, `POINT` domain, named `_PIN_WEIGHT`.
  Blender's stock glTF exporter passes underscore-prefixed mesh attributes through verbatim, which
  is the entire reason the name has an underscore, and it is why no exporter patch is needed.
- **`Pin selected` and `Unpin selected` operators.** The common case is two clicks in edit mode,
  not an artist hand-defining an attribute and getting the domain or the type wrong.
- **A pre-export validator.** Every `FABRIC_`-prefixed mesh must carry `_PIN_WEIGHT`, with the right
  type and domain, and must have at least one vertex at weight 1 — a cloth pinned nowhere falls
  through the floor on frame one and looks like an engine bug. A mesh carrying `_PIN_WEIGHT`
  *without* the prefix is a warning rather than an error, because it is dead payload: Tethered's
  exporter wrote the attribute onto all twenty-nine accessors in its Sponza when one mesh needed
  it, and nobody noticed because nothing looked.

## The trap worth naming in the UI

**A Blender vertex group does not survive glTF export.** Groups leave only as `JOINTS_0` and
`WEIGHTS_0`, and only when an armature is present; a bare group on a curtain is silently dropped.
That is the thing an artist will reach for first, because weight-painting a group is the familiar
gesture, and the failure is silence rather than an error. The add-on writes an attribute and its
panel should say why — including an operator to convert an existing group to the attribute, since
that is what someone who already painted one will want.

## What this must not grow

- An exporter. Blender's own glTF exporter does this correctly and replacing it is not a chore.
- A `FABRIC_` rename or a scene-organisation tool. The prefix is C19's convention; this row reads
  it, it does not own it.
- Anything about animation, rigs or the fifty-odd other things Tethered's `.claude/tools/` grew.

## Verification

- `scripts-fail` — the validator is verified by the failures it produces rather than by a passing
  export. Each of these must refuse and must name the offending object: a `FABRIC_` mesh with no
  `_PIN_WEIGHT`; a `FABRIC_` mesh whose `_PIN_WEIGHT` is on the wrong domain or the wrong type; a
  `FABRIC_` mesh with no vertex at weight 1; and a non-`FABRIC_` mesh carrying the attribute, which
  warns rather than refuses. A run that reports nothing on a correct scene is the fifth case.
- `inspection` — the add-on runs inside Blender and nothing in this repository's suite can load
  `bpy`, so no test covers it and none will. Recording that gap is part of the finding, as
  [D8](../done/D8-shader-conventions-and-the-formula-in-four-copies.md) did for `decal.frag`. What
  it means concretely: the loop below is run by hand, and the card says who ran it and when.
- The loop closed end to end. Author a curtain in Blender, pin its top edge with the panel, export
  a glTF, and load it with `./run.sh demo`. It must hang from where it was pinned.
- The two producers agreed. Export the same curtain from Blender and generate the equivalent from
  `scripts/make_test_scene.py`, and check the engine reads identical `invMass` from both. That is
  the only check that catches the add-on and the generator drifting apart, and it is the reason
  both exist.

## Reference update

[`making-a-game.md`](../../guides/making-a-game.md) — the authoring recipe, beside its existing
*"glTF extras — one vocabulary, two front doors"* section, since a reader looking for how authoring
data reaches the engine will look there and this is the one thing that does not arrive through
`extras`.

[`tooling.md`](../../architecture/tooling.md) — `tools/` exists now, what is in it, and why it is
not `scripts/`: everything in `scripts/` runs from a shell against this repository, and this runs
inside another program.

## Outcome

**The tool landed on the other side of the exporter.** `scripts/check_pins.py` reads an exported
`.gltf` or `.glb` and refuses one that does not carry the pins the loader will look for;
`tests/check_pins_test.py` is thirteen cases covering every refusal, run directly and by CI beside
`manifest_test.py` and `make_composite_scene_test.py`. There is no `tools/blender/substrate_pins.py`
and no add-on, and that is a decision rather than a shortfall.

The argument is this card's own *"trap worth naming in the UI"*, taken one step further than the
card took it. **A Blender vertex group does not survive glTF export**, and that is the failure an
artist actually hits. A validator running *inside* Blender sees a painted group and a happy scene —
the thing that is missing does not exist yet. Nothing before the export can see it. The same holds
for the other three ways this goes wrong, and all four are only visible in the file:

- the exporter's **Data > Mesh > Attributes** checkbox left off, so the attribute is never written;
- a bare vertex group, dropped in silence;
- an attribute on the wrong domain or type, converted or dropped without a word;
- **the object renamed but not its mesh data-block** — glTF's `mesh.name` comes from the data-block
  and the node name from the object, and C19 reads `asset.meshes[meshIdx].name`. This one was not
  on the card and it is the worst of the four: every other check passes and the engine still sees no
  cloth. It gets its own refusal, naming Object Data Properties.

The secondary reasons all point the same way. Nothing in this repository can load `bpy`, so an
add-on would be the one file the suite could never run — the card said so itself and called it a gap
to record. Reading the bytes needs no Blender installed, takes no new dependency, and gives the
check a home in CI. And this is the file the engine reads, so the validator and the loader are
looking at the same thing.

### The convention, unchanged and still C19's

A mesh named `FABRIC_*`, carrying a `_PIN_WEIGHT` float attribute on the vertex domain, `1` pinned
and `0` free. Both strings are spelled once in `check_pins.py`, as constants, with the comment
saying they are read here and owned by C19 — which is the same "one predicate, one place" property
C19 asks of the loader, applied to the authoring side. The pin threshold `>= 0.999` is C19's
`invMass` mapping and is named as such; it is the one number this script had to borrow, and a cloth
whose highest weight is `0.99` is refused with the number printed, because that cloth is not pinned,
it is heavy.

**No fifth `extras` schema, and `systems.md`'s table is untouched.** `extras` has nowhere to put a
value per vertex, which is the whole reason cloth authors through a name and an attribute instead —
and the engine does not read either string yet, so an entry describing it would be describing
something that is not there. C19's reference update owns that paragraph, and writing it twice is
what this card was told not to do. There is no enum here either, so D12's `Named<E>` mechanism has
nothing to attach to; the nearest thing is the near-miss report below, which is D12's *shape*
without its machinery.

### What a person does in Blender

Written out in [`making-a-game.md`](../../guides/making-a-game.md), four steps: name the mesh
`FABRIC_Curtain` **in Object Data Properties** rather than only the outliner; add a Float attribute
on the Vertex domain named `_PIN_WEIGHT`; set it to 1 on the vertices to pin, and *not* a vertex
group; export with Data > Mesh > Attributes ticked. Then run the script, because every one of those
four fails quietly.

### How failure is reported, and the loud/fatal split

`scripts-fail`, in D7's sense: the script is worth exactly what it refuses, and it exits non-zero on
every refusal with one line naming the file, the mesh and the primitive. Three decisions inside
that:

- **A missing input file is fatal, not a skip.** `fetch_assets.sh` skips a generator whose source is
  absent because there the source is optional content that may legitimately not be in the tree. A
  validator's argument is a path a person typed; there is no legitimate absent case, and saying
  nothing about a name that does not exist is precisely the silent-nothing failure this token
  exists to prevent.
- **Dead payload is loud but not fatal** — a mesh carrying `_PIN_WEIGHT` without the prefix warns,
  prints, and exits 0, because the export is still correct for the engine even though the bytes are
  wasted. `--strict` promotes it, following `manifest.py`'s precedent, which is the switch a
  packaging step would want. That is Tethered's twenty-nine-accessor Sponza, caught.
- **A near-miss name is named rather than called absent.** `_pin_weight` produces "carries
  `_pin_weight` instead. The name is case-sensitive on both sides" rather than "attribute missing" —
  the error names the key, the text, the legal spelling and what is standing in its place, which is
  the standard D12 set for the settings parse.

### What C19 inherits

- The two strings and the `>= 0.999` threshold, already spelled in one place on the authoring side,
  and a card entry saying the runtime owns them.
- A validator it can point an artist at, and one CI can run over a game's assets.
- **Per-primitive, not per-mesh.** Blender splits a mesh by material, and C19's design is
  `Primitive::clothOffset`, so a curtain split across two materials is two soft bodies. The "at
  least one pinned vertex" check is therefore per primitive: a lower half with no pins is a separate
  body that falls, and checking per mesh would pass it.
- One thing it does **not** inherit: the card's *"two producers agreed"* check cannot be run. It
  needs `scripts/make_test_scene.py`'s `build_cloth()`, which is explicitly an item on C19, and
  comparing `invMass` from both producers needs a loader that reads `_PIN_WEIGHT`. That check
  belongs on C19's verification and is noted there by this sentence rather than duplicated.

### Corrections to what this card assumed

- **`tools/` already exists** — `tools/bake.cpp` has been there since D9 — so this was never "the
  first thing in `tools/`", and it does not go there anyway. `common.sh`'s rule decides it:
  everything in `scripts/` runs from a shell against this repository, and so does this.
  `tooling.md` needed no "`tools/` exists now" paragraph.
- The golden suite is **eleven** cases, not twelve; and it was not run, because nothing here can
  reach a rendered frame. The diff is two new Python files, three documentation and CI edits, and no
  engine source, shader or asset.

### A finding that is not this card's to fix

**`tests/manifest_test.py` is red at HEAD**, on an unmodified tree: `manifest.build` returns four
values and the test unpacks three, so fifteen of its twenty-four cases error. Neither file is
touched by this row. The CI `manifest` job has therefore been failing since whichever commit added
the fourth return value, which is worth its own card.

### Verification

- `python3 tests/check_pins_test.py` — **13 of 13**. Each of the card's five cases is one of them:
  the attribute absent, the wrong type, the wrong domain (which reaches the file as a count that
  disagrees with `POSITION` — the domain itself is gone by then, and that is stated in the test),
  nothing pinned, and the dead-payload warning; plus the correct export reporting nothing, the
  rename trap, the near miss, out-of-range weights, a missing input, several files in one run, a GLB
  read identically to a glTF, and a file that is neither refused by name.
- **Three mutations, six failures, no false passes** — the pin threshold zeroed, the near-miss fold
  disabled, and the `[0, 1]` range check removed.
- **Inspection.** Six fixtures written to a scratch directory by the test module's own builder and
  the script run over each by hand: four refusals exit 1 with the mesh and primitive named, the dead
  payload exits 0 and exits 1 under `--strict`, the correct curtain prints
  `mesh 0 'FABRIC_Curtain' primitive 0: 2 of 4 vertices pinned` and exits 0, and a path that does
  not exist exits 1 saying `not found`. Run by Jimmy Ray, 2026-07-31.
- Every scene in the tree — Sponza, the four engine test scenes and the eight demo scenes —
  `0 cloth primitives, 0 errors, 0 warnings`, exit 0. Nothing carries a `FABRIC_` mesh yet, which is
  what C19's card predicts.
- `./test.sh debug` — **869 of 869**. `./test.sh asan` — **869 of 869**.
  `python3 tests/make_composite_scene_test.py` — 3 of 3.
- `scripts/golden.sh check release` not run, per the paragraph above. `scripts/check_ascii.sh`
  clean.
