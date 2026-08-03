---
id: G5b
title: Shader search path
arc: G
size: S
verification: golden-12
---

# G5b — Shader search path

S

## G5b as landed

It came early, with the rename that made `engine/shaders/` a directory: moving the tree
and then moving the resolution rule would have been two edits to the same lines.

Three things the row did not predict, recorded because the next row inherits them:

- **The output directory is `shaders/game`, not `shaders/<name>`.** One build directory
  holds one game, so a fixed name keeps `SUBSTRATE_GAME_SHADER_DIR` the same string for
  every game — which is what stops the search path from being a reason a `build_game.sh`
  toggle recompiles the engine. Only the *source* path varies, and it is scoped with
  `set_source_files_properties` to `Renderer.cpp`, the one translation unit that reads it.
- **Two directories are two `if`s, not a list.** The Rule of Threes applies: a third tree
  is what would justify parsing a separator-packed define, and there is no third tree.
- **A compiled shader outlives its source, and in the game's tree that is a defect.**
  Deleting `game/demo/shaders/tonemap.frag` left `tonemap.frag.spv` behind and it kept
  winning the lookup — a wrong picture with no source anywhere to explain it. `CMakeLists.txt`
  now empties that one directory on every configure. This was found by the verification
  below rather than by review, which is the argument for the row having had one.

**What is still not checked continuously.** The demo ships no shaders, and it should not:
one the demo actually used would change every golden image, which is the check this work
was verified against. So the second tree exists and is exercised only by hand. **G1b's
template is what closes this** — a scaffolded game that ships one shader makes the search
path a property the build asserts rather than one someone remembers to test.

Verified by a temporary override: `engine/shaders/tonemap.frag` copied to
`game/demo/shaders/` with a green tint, which compiled (resolving `#include "frame.glsl"`
out of the engine's tree), won the lookup (R and B fell to 0.2x, G unchanged), and
hot-reloaded on touch — as did the engine's tree in the same run. Removed afterwards, and
all 11 golden cases are byte-identical to the pre-rename build.

**G6 shrank from an L to an S-M while this document was being written**, because the
inspector landed in the meantime and turned out not to need the thing it was waiting for.
That is worth recording rather than quietly amending: the row was sized on the assumption
that naming an object's properties required a property system, and the answer shipped was a
function that names them. G6 inherits that answer instead of replacing it.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.

## Reference

[architecture/tooling.md](../../architecture/tooling.md).

## Outcome

Recorded above, under *G5b as landed*.
