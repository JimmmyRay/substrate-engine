---
id: P8
title: Tilemaps
arc: P
size: S
verification: tests-hosted
---

# P8 — Tilemaps

**Reconsidered at the Phase 3 boundary, as `order.md` required, and declined.** A tilemap is
a loop over things the engine already has

## What the card used to say

> A chunked grid, a tileset, one draw per chunk, and colliders generated from the grid

Sized **L**, verified by twelve golden cases and a trace. None of that is what landed,
because the row was opened with an instruction attached to it: [order.md](../order.md) says
of the P rows, *"P8 last, and reconsidered before it is started."* This card is that
reconsideration and its result.

## The reconsideration

[arcs.md](../arcs.md) states the single argument that justified a tilemap as an engine
primitive rather than as game code, and it is a cost model:

> *"roughly 8,000 tiles are visible at 16 px on a 1080p screen, and the difference between
> one draw per chunk and one per tile is the difference between a tilemap and a slideshow.
> That is an engine's problem."*

**That sentence was written before P4 landed, and P4 did not land one draw per tile.** It
landed *one draw for all layers* — `vkCmdDraw(6, n)` over a single instance buffer, because
one blend state makes one global sort possible and one global sort makes one draw possible.
So the alternative to chunking was never 8,000 draws. It is one draw, and it is measured:

| | P4's arm | A screen of tiles |
|---|---|---|
| Sprites | 10,000, four layers, ~4x overdraw | 8,160 at 16 px on 1080p |
| `Sprites` zone | **0.053 ms** | inside it |
| CPU | **0.037 ms**, one 640 KB `memcpy` | inside it |
| Sort | **none** — a layer is the key and a position is not part of it | none |

The last row is the one that finishes the argument. A scrolling tilemap is the worst case a
CPU sort could have, and `prepare()` sorts on create, destroy and reorder — never on move.
Rewriting every visible tile's rectangle and position every frame re-sorts nothing, which
`TilemapTests.ScrollingTheMapReSortsNothing` now pins.

**With the performance argument discharged, three things were left. Each was weighed.**

### Chunking is a memory argument, and it argues the other way

Chunking earns its place when a grid is too large to materialise. It is — a Terraria-scale
world is 20M cells, and a `GpuSprite` is 64 bytes, so no game may hand the engine its whole
map whether or not a tilemap exists. Every such game keeps a compact grid and materialises
a window of it.

**A compact grid is 1–2 bytes a cell; the engine's is 64.** So the engine owning the grid
would have to own the compact form to beat what a game already has — which means choosing
the tile id's width, whether cells carry flags, how many grid layers there are, whether the
world is finite or streamed, and whether it is authored or generated. Every one of those is
a genre decision, which is what `arcs.md` warned about when it called this *"the row a genre
could make wrong"*. The engine would be picking, and picking wrongly for somebody.

### The authoring format is the half the conventions refuse hardest

This arc has already declined a format of exactly this shape, in `limitations.md`:

> **A sprite sheet *file* format** — *"Aseprite's JSON and TexturePacker's agree about
> almost nothing, neither is in the asset tree, and a parser for a format no asset uses is a
> parser nobody can test."*

Tilemaps are the stronger case of the same argument, not a weaker one. Tiled's TMX is XML
with base64-and-gzip layer payloads; LDtk's is JSON with a different world model entirely
(levels, entity layers, auto-rules). They agree about less than the two sheet formats do,
neither is in the asset tree, and an engine-invented third would be a format with one tool,
no asset and no test — while adding a fourth thing to the bake pipeline the C arc is
deliberately holding at three.

### Collision from the grid is arithmetic over the game's own array

P7 landed `ColliderFreedom::Plane2D`, which is the part that needed a solver. What remains
is merging runs of solid cells into boxes: twenty-one cells become three bodies in fourteen
lines, over an array the engine does not own and could not have merged better.

## The finding

> **A tile is a sprite with a UV rect from a sheet, and the engine already hands out both.**
> `frameUv` is public *precisely* so a caller can place one without any playback — its own
> header says so. What a tilemap would add over that loop is culling the frame does not
> need, chunking the game's own grid does better, and a format the conventions refuse.

Owning it would also have cost a **fourth dense table** with its own handle type, its own
`destroy`, its own layer and order concept and its own sheet reference — a second
`SpriteTable` wearing a grid. That is the "two subsystems in one struct" P6 refused on its
own card, and the "two vocabularies for one idea" that declined a second sprite animation
state machine.

## What a game writes instead

Not a sketch. [`tests/TilemapTests.cpp`](../../../tests/TilemapTests.cpp) is the whole
recipe, compiled against the real headers and run in the suite under ASan — because a
refusal whose replacement is a code fragment in a document is a refusal nobody can check.
Seven cases, and none of them is engine code.

```cpp
// The map is in the game's format. Sixteen characters a row here; a real game uses its
// own array, its own editor's export, or a generator. The engine reads none of them.
for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
        const char c = cellAt(x, y);
        if (!filled(c)) continue;
        tiles.push_back(sprites.create(layer, {
            .image    = tileset,
            .uv       = sprites.frameUv(sheet, tileCell(c)),   // P5, no playback attached
            .size     = {kTile, kTile},
            .pivot    = {0.0f, 0.0f},
            .position = cellOrigin(x, y),
        }));
    }
}
```

| The tilemap wants | What answers it | Row |
|---|---|---|
| A cell's rectangle in the tileset | `SpriteTable::frameUv(sheet, cell)` | P5 |
| Drawing the grid | `create` into one layer — one draw for all of it | P4 |
| Scrolling it | `setPosition` / `setUv`, which re-sort nothing | P4 |
| Unloading a level | `destroyLayer` — a layer already owns its contents' lifetimes | P4 |
| Collision from the grid | Merged runs, `ColliderFreedom::Plane2D` | P7 |
| A chunk as *lit* geometry | A `MeshData`, handed to `Engine::createMesh` | G4 |
| Unloading that chunk | `Engine::removeModel` | C10 |

The last two are the row's own chunking, available today and game-side: `chunkMesh` in that
file builds an 8x8 chunk into one `MeshData` with texel UVs normalised against the atlas,
and the only line the hosted suite cannot run is `e.createMesh(std::move(mesh))`, which
needs a device. It is worth noting *why* that mesh is not `scene::quadMesh`: P6's helper
puts the texel rect on the **material** and gives its quad 0..1 corners, which is right when
each quad has its own material and wrong for a chunk, where one mesh is one material over a
whole atlas. Eight lines of vertex writing is the difference, and it is the game's eight
lines because the game is what knows its atlas size.

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `scripts/golden.sh` — twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm — never the `GPU @` log line.

**Reduced with the scope, and the reduction is the reconsideration's own result.** The
golden set and the trace verified a pass; there is no pass. Nothing in `engine/` changed, so
there is nothing a moved pixel could be evidence of, and a trace arm would compare a build
against itself. What replaced them is the check the *example* needs, which is the only new
code this row produced: `tests-hosted`, so the recipe runs under ASan as well as debug —
Jolt is in the hosted set and the plane constraint is checked as an equality there.

## Reference

[architecture/limitations.md](../../architecture/limitations.md) — *A tilemap subsystem*, in
the 2D arc's declined table, with both triggers.

## Outcome

**The row was reconsidered as instructed and declined, and that closed it.** No engine code
was written. What landed is a recorded refusal with two named triggers, and seven hosted
test cases that hold the alternative to it honest.

**What decided it was a number that already existed and had not been read against this
row.** The card was justified by a cost model — one draw per chunk against one per tile —
and P4 had already made that comparison meaningless by landing one draw for *everything*,
measured at 0.053 ms for ten thousand sprites with no sort in it. The whole reconsideration
is one obsolete sentence in `arcs.md`, found by checking the argument against the tree
rather than against itself. **That is the general lesson worth carrying: a row deferred to
the end of an arc must have its justification re-read against what the arc landed, because
the rows in front of it are exactly the ones most likely to have answered it.**

**What the estimate did not predict.** The card said **L**, 800–2000 lines. The row is
**S** and none of those lines is in `engine/`. It is the first card on this board whose
delivery is a decision rather than a diff, and the shape it needed was not a card shape: the
work was reading four documents against one measurement, and then writing enough code to
prove the refusal was not a dodge.

**Two things were genuinely weighed and did not survive.** Chunking, which loses to the
game's own grid on memory by a factor of thirty-two and would have required the engine to
pick a tile id's width for every genre; and an authoring format, which this arc had already
refused at smaller scale for sprite sheets and which would have added a fourth artefact to a
bake pipeline the C arc is deliberately holding at three.

**One gap was found and deliberately not filled.** `SpriteTable` has **no view culling** —
every live sprite is written into the buffer and drawn. That is a property of sprites rather
than of tiles, and its answer is a cull on `SpriteTable`, not a subsystem; at 0.053 ms per
ten thousand it is also not binding, since a game bounds its live set by creating a window
rather than a world. It is recorded as the second trigger in `limitations.md` with a number
attached rather than left as a feeling.

**Verification.** `./test.sh debug` — **761 of 761**, seven new `TilemapTest` cases.
`./test.sh asan` — **761 of 761**. Nothing under `engine/` was touched, so the golden set,
the readback set and the trace have nothing to say about this row; the two suites are the
whole check and the card's `verification` line was reduced to say so.
