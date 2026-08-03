---
id: C13
title: A measured load-time baseline
arc: C
size: S-M
verification: golden-12, trace
---

# C13 — A measured load-time baseline

**It reorders the load path.** `make_test_scene.py --scaling` sizes a scene on four independent axes (`--nodes`, `--meshes`, `--rings`, `--colliders`); with no arguments it writes the same ten fixed scenes it always did. `GltfScene::parse` is split into `mmap` / `extras` / `fastgltf`, reported on every load. **What it measured changes which row goes first — see below**

## The load path, and why it is three rows

Streaming a level in while the game runs is only possible if a level's load cost is bounded
and small, so C10 cannot be honest without this. The work splits three ways because the
three parts differ in every way that matters — size, risk, and what verifies them.

**C13 is a measurement, and it goes first.** The engine already reports
`parse`/`geometry`/`textures` per load — three hand-rolled `steady_clock` spans logged once
at [`GltfScene.cpp:1218`](../../../engine/scene/GltfScene.cpp#L1218) — but it can only report them
for a scene somebody has built, and **the argument for C13 is not that the tree lacks one.**
This engine is for projects whose scenes run orders of magnitude past anything the golden
suite pins, and a proportion measured at a hundred draws is not evidence about ten thousand
however carefully it was taken. What is missing is the ability to *vary* size, which is a
different thing from having one large scene and the reason a generator is the row rather
than an asset: `make_test_scene.py` takes no arguments and writes nine fixed scenes, and
[`build_instances(grid=16, spacing=2.5)`](../../../scripts/make_test_scene.py#L550) is the only
sizing parameter anywhere, hard-coded at its call site. Exposing it moves one axis —
`instances.gltf` is 4097 nodes with two meshes and ten accessors, so it cannot move `parse`
at all. The five load stages scale on five different axes, and a generator that varies them
independently is what turns a proportion at one scene size into a curve:

| Axis | Driven by | What it loads |
|---|---|---|
| Nodes and draws | placement count | `flatten`, `Cull`, the instance table |
| Accessors and bufferViews | distinct meshes, animation clips | `parse` |
| Vertices | mesh density | `geometry`, `upload` |
| Colliders | `substrate_collider` count, and their triangles | the Jolt cook, physics init |
| Textures | image count and dimensions | `textureCache`, `uploadTextures` |

`GltfScene::parse` also needs sub-zones. It is currently one number covering four rapidjson
documents, fastgltf's own structure building, base64 decoding and external `.bin` reads, and
nothing can be attributed to any of them.

## What C13 measured, and why C14 now goes before C15

Four runs of an 8000-node, 800-mesh scaling scene, release, medians of the `parse` split
the row added:

| Phase | Cost | Share of `parse` |
|---|---|---|
| `mmap` | 0.0 ms | — |
| **`extras` (three whole-document rapidjson scans)** | **~57 ms** | **~82%** |
| `fastgltf` (structure building, base64, external `.bin`) | ~12 ms | ~17% |

Total load was 79.9 ms, so **the three `extras` scans are about 73% of the entire load**.

That is the opposite of what the ordering assumed. C15 — the L row, the new file format,
the compatibility contract — replaces the ~12 ms that fastgltf costs. C14 — the M row, no
new format, a shared helper in a header that already exists — replaces two thirds of the
~57 ms, because collapsing three scans of the same bytes into one is a two-thirds saving on
the largest single item in the load.

**So C14 goes first, and it is no longer merely the cheaper of the two: it is also the
larger win.** C15 stays justified by shipping rather than by this measurement — see above —
but its size relative to what it buys is now a number rather than a guess, and it is a worse
number than C14's.

The axis that produced this is the one no scene in the tree could move: `instances.gltf` is
4097 nodes over two meshes and ten accessors, so `parse` is the same work at any node count
it reaches. Adding meshes is what makes the document big, and the document is what the
`extras` scans walk three times.

**C14 needs no new file format**, which is what separates it from C15.
[`GltfScene.cpp:219-231`](../../../engine/scene/GltfScene.cpp#L219-L231) runs `parseSceneEmitters`,
`parseSceneColliders` and `parseSceneAudioSources` before fastgltf starts, and each one
builds a complete *copying* `rapidjson::Document` over the whole file
([`ParticleSystem.cpp`](../../../engine/scene/ParticleSystem.cpp)) in order to read
`nodes[].extras` under one key. That is three redundant parses of the same bytes at any
scene size, and it was four when this row was written —
[`Light.h:139`](../../../engine/gfx/Light.h#L139) records `parseSceneLightOverrides` being deleted
outright for reasons of its own. **The fourth copy leaving does not weaken the row, it dates
it**: the count fell because a feature was removed, not because the duplication was
addressed, and the three that remain are the three that parse the most.

Parsing once and handing the nodes array to three extractors is the fix. The shared helper
belongs beside `gltfJsonSpan` in `core/Json.h` — note that the *scope* argument for that
placement died with the fourth copy, since all three survivors are under `scene/` and the
[CLAUDE.md](../../../CLAUDE.md) table would now select a header there. What keeps it in `Json.h` is
cohesion rather than reach: `gltfJsonSpan` is the first half of the same operation, and
splitting a two-step glTF-JSON entry point across two directories to satisfy a rule about
reach is the rule misapplied. `Json.h`'s own comment records three callers as the Rule of
Threes met rather than anticipated, and this is that same three.

**Landed, and it did what C13 said it would.** One parse, three readers, on the same
8000-node scaling scene:

| | `extras` | whole load |
|---|---|---|
| Before | ~57 ms | 79.9 ms |
| After | ~19 ms | ~44 ms |

The remaining ~19 ms is one parse plus three walks over the nodes, which is the floor for
reading three schemas out of one document. **A 45% cut in total load time, from the M row,
against the L row that would have addressed the ~13 ms fastgltf costs.**

A second deduplication fell out one level up: "are these bytes a glTF document" now has one
implementation, so the malformed-input, truncated-GLB and null-pointer cases moved out of
three test suites and onto `GltfDocument::parse`. The three suites share
[`tests/GltfExtras.h`](../../../tests/GltfExtras.h) to reach the readers, which is the Rule of
Threes met again rather than a fourth copy.

**The drift this predicts has already happened**, which is worth more than the argument from
redundancy. All three copies carry the same eighteen-line prologue *including its comment*,
and the name-fallback that follows it runs three lines in
[`Collider.cpp:72`](../../../engine/scene/Collider.cpp#L72) and
[`AudioSource.cpp:81`](../../../engine/scene/AudioSource.cpp#L81) but only two in
[`ParticleSystem.cpp:312`](../../../engine/scene/ParticleSystem.cpp#L312), which is **missing the
`if (e.name.empty()) e.name = "node " + std::to_string(n);` line** the other two end on. An
emitter on an unnamed node keeps an empty name where a collider on the same node reports
`node 7`, and nothing about three hand-maintained copies makes that visible. A copy that has
already diverged is a stronger case than a copy that might.

**This is also the row that answers "why clean up before building".** The divergence above
was not decided by anyone; it arrived because the copies were left standing, and it will be
inherited by whatever reads `extras` next. Every capability row in Part 1 adds a reader of
this kind. Deferring the extraction does not postpone its cost — it multiplies the surface
the cost is eventually paid over, which is the argument the reordering in
Recommended order now runs on.

C14's other half is the package. `scripts/ktx2.py` already solves textures — a warm BC7
cache takes image decode to about a millisecond — but `manifest.py` treats an absent `.ktx2`
as normal, which is right for a source tree and wrong for a release, where it means silently
shipping the decode path to someone who cannot rebuild the cache.

**C15 is justified by shipping, not by a measured proportion.** Worth stating plainly,
because the ordering below could be misread as making the bake contingent on what C13
reports. It is not. Re-deriving a scene from JSON on every launch — parsing the document,
de-interleaving every accessor, flattening the node tree and cooking every collision mesh —
is a development convenience, and no engine ships it to a player. That is why every engine
that ships has a cooked form of its scenes, and it is true at any size the measurement comes
back with. What C13 decides is *how much of C15 is worth writing first* and whether C14's
cheaper half lands ahead of it; what it cannot decide is whether a project that ships wants a
baked scene at all.

**C15 is a format, and formats carry a compatibility contract.** The rule is the one
`ktx2.py` states about its own cache, and it is what makes a cache a cache rather than a
second asset format to keep in step:

> Nothing rewrites the glTF, and deleting the cache restores the original behaviour exactly.

So: a sidecar beside the source, optional, falling back silently, invalidated by the source's
size and mtime and by a format version in its header, with a `static_assert` on every struct
written so a `Vertex` layout change is a build error rather than a corrupt scene.

Most of C15's size is a refactor rather than the format. `GltfScene::load` is one long
function taking `ctx` and `uploader`, and baking without a device means its CPU half must run
without touching either. It nearly already does — every use of `ctx` or `uploader` in that
function sits inside the `textures` block — so the split is bounded: parse, geometry, flatten
and animation move into a plain struct filled either from a path or from a blob, and
`GltfScene::load` keeps the GPU half. The writer is C++ sharing those structs. A Python
writer would be a second encoding of the vertex layout with no compiler to make the two
agree, which is the hazard `manifest.py`'s own docstring names about `Resources::Resources`.

**Checked against the file, and the estimate holds with one correction.** The device-touching
lines really are confined to the `textures` block and the `upload` block that follows it, so
the CPU half is not one range but two: `parse`/`geometry`/`flatten`/`animation`
([`GltfScene.cpp:191-787`](../../../engine/scene/GltfScene.cpp#L191)) and `materials`
([`:931-1018`](../../../engine/scene/GltfScene.cpp#L931)), with the texture pass sitting between
them. `materials` reads only `asset`, not any runtime slot -- `textureIndexOf` returns a
glTF *image* index, and the bindless array is indexed by that -- so it moves with the rest
and the ordering in the file is incidental rather than a dependency.

**What the struct has to carry that the row does not name: the image list.** A sidecar
holding decoded pixels would be a texture format, which is `ktx2.py`'s job and already done;
what the CPU half must hand over instead is *which images to load and whether each is sRGB*,
resolved against the document. That is the one piece of the texture block that is not device
work, and it is the boundary the split has to be drawn at. Getting it wrong in the other
direction -- a sidecar that embeds pixels -- would make the cache larger than the glTF and
put it out of step with the `.ktx2` files beside it.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/tooling.md](../../architecture/tooling.md).

## Outcome

Recorded above, under *The load path, and why it is three rows*.
