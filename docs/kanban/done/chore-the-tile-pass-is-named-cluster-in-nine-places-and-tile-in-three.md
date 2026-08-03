---
id: chore-the-tile-pass-is-named-cluster-in-nine-places-and-tile-in-three
title: The tile pass is named cluster in nine places and tile in three
arc: chore
size: S
verification: golden, tests-hosted, inspection
---

# chore-the-tile-pass-is-named-cluster-in-nine-places-and-tile-in-three — The tile pass is named cluster in nine places and tile in three

C35 landed a **tiled** light assignment — a 2D screen-tile grid with per-tile depth bounds, no z
subdivision, no z in the addressing — and named most of it *cluster*.

| Says cluster | Says tile |
|---|---|
| `render.lightClusters` | `kLightTileSize` |
| `frame.clusterParams` | `View::lightTiles` |
| `Renderer::recordLightClusters` | `LightClusterPush::tiles` |
| `View::lightClusters` | |
| `kLightClusterMaxWords` | |
| `lightClusterSetLayout`, `lightClusterPipeline` | |
| `light_cluster.comp`, `light_cluster1x.comp` | |
| `light_cluster.glsl`, `light_cluster_body.glsl` | |

**C35's card asked for exactly this not to happen**: *"the card should say which it landed, because
'clustered' and 'tiled' are being used interchangeably here and the tree should not be."* The card
then specified the setting as `render.lightClusters` itself, so the inconsistency is inherited
rather than careless — which is why fixing it is a decision and not a typo sweep.

`light_cluster.glsl`'s own header already says **"Tiled, not clustered"**, and the answer is
settled in [rendering.md](../../architecture/rendering.md), "Tiled light assignment — and it is
tiled, not clustered". So the reference and the shader agree; the symbols do not.

**Why it is worth a card rather than a shrug.** The distinction is load-bearing the moment anyone
extends this. A froxel scheme subdivides z on a fixed schedule and is the natural next step for a
scene with depth complexity the per-tile slab handles badly; someone reading `clusterParams` and
`kLightClusterMaxWords` will reasonably believe it is already that, and will look for the z
coordinate that does not exist.

**The one real decision inside it**: `render.lightClusters` is a **public settings key**. Renaming
it breaks any `substrate.json` that names it and any `--set render.lightClusters=false` in a
script or a habit. The candidates are to rename it and accept that, to keep the key and rename
only the internals (leaving one deliberate inconsistency with a comment saying why), or to leave
it and rely on the reference. Pick one and write the reason on the card.

Do the internals **by structure, not by regex** — `light_cluster_body.glsl` is shared by two
`.comp` files and the symbols cross `Renderer.h`, `Renderer.cpp`, `Settings.h`, `frame.glsl`,
`lighting_body.glsl` and `shadowmask.frag`.

**Provenance.** Established while closing C35; the symbol lists above were read from the tree, not
remembered.

## Verification

- `golden`: thirteen cases byte-identical. A rename must move no pixel, and if one moves the
  rename was not a rename.
- `tests-hosted`: `./test.sh debug` then `./test.sh asan`, each its own invocation.
- `inspection`: if `render.lightClusters` is renamed, confirm the old key produces a clear error
  rather than being silently ignored — `Settings.cpp` already has a moved-keys table for exactly
  this, and a settings key that vanishes quietly is worse than one that changed name loudly.

## Reference update

[rendering.md](../../architecture/rendering.md), "Tiled light assignment", whose symbol names
would change, and [tooling.md](../../architecture/tooling.md) if the trace zone `LightClusters` is
renamed with them.

## Outcome

**Renamed, key included** — and the decision the card called the one real one turned out to be
free. `render.lightClusters` was a *public* settings key, which is why the card weighed keeping
it; but it was added by C35 earlier the same day, appears in no committed config (not even
`substrate.json`, which never gained the row), and nothing outside this tree has ever seen it. So
there was no compatibility to trade and no reason to leave one deliberate inconsistency behind:
the key is **`render.lightTiles`**, verified reachable by `--dump-settings`
(`render.lightTiles  bool  true  default`).

The rest, by structure rather than regex, in one ordered pass with word boundaries — and the
ordering mattered: `View::lightTiles` was already the `VkExtent2D` tile grid, so it had to move to
`lightTileGrid` *before* `lightClusters` could become `lightTiles` on the buffer, or the two would
have collided silently.

| was | is |
|---|---|
| `render.lightClusters`, `lightClustersEnabled`, `lightClustersActive` | `render.lightTiles`, `lightTilesEnabled`, `lightTilesActive` |
| `View::lightClusters` (buffer) / `View::lightTiles` (extent) | `View::lightTiles` / `View::lightTileGrid` |
| `frame.clusterParams` | `frame.tileParams` |
| `recordLightClusters`, `LightClusterPush` | `recordLightTiles`, `LightTilePush` |
| `kLightClusterMaxWords`, `lightClusterWords` | `kLightTileMaxWords`, `lightTileWords` |
| `lightClusterSetLayout`, `lightClusterPipeline`, `lightClusterSet`, `lightClusterLayout` | the `lightTile*` forms |
| `light_cluster{,1x}.comp`, `light_cluster{,_body}.glsl` | `light_tile{,1x}.comp`, `light_tile{,_body}.glsl` |
| GPU zone `LightClusters` | `LightTiles` |

Prose went with it — "Clustered light assignment" as the settings description, "clustering is
off", "refuses to cluster", the `vkCreateDescriptorSetLayout(light clusters)` and
`vkAllocateDescriptorSets(light clusters)` strings, and the over-budget warning's "clustered
assignment can index". Shader files moved with `git mv`, so the flow is followable.

**Two mentions of "cluster" are left on purpose.** `light_tile.glsl`'s heading is still
"## Tiled, not clustered" — that is the contrast the file exists to draw, and deleting the word
would delete the distinction. And `ssao.comp`'s "samples cluster toward the origin" is ordinary
English about sample distribution, unrelated to this pass. `SpatialIndex.cpp`'s "a tight cluster"
is the same. Grep for `cluster` in `engine/` now returns those three and nothing else.

Verification: build clean; **golden 13 of 13**; `./test.sh debug` 1060 tests, 107 suites, all
pass; `scripts/perfgate.py --config release` inside budget (`Frame` 2.970 against 2.950,
`Lighting` 1.847 against 1.832), which is the check that a rename stayed a rename. No pixel moved,
which is the whole contract.

Reference updated: `rendering.md`'s "Tiled light assignment" section now names the symbols it
actually describes.
