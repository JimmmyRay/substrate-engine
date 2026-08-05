---
id: D22
title: A module is what Engine.h cannot reach
arc: D
size: L
verification: tests-4, golden, scripts-fail
---

# D22 — A module is what Engine.h cannot reach

`engine/scene/` was 53 files and 19,757 lines: the scene graph, every simulation subsystem
and both glTF loaders in one directory. "Is physics coupled to the renderer?" was answered
by reading includes rather than by the build, and the answer drifted — `core/Config.h` had
grown an include of `gfx/DebugView.h` and another of `scene/AudioBackend.h`, two layers
above it, and nothing said so for as long as it compiled.

This row builds the guard, gets `core` back to the bottom of the graph, and migrates one
module end to end as the pattern the rest follow.

**The definition is the whole design.** What `root` can reach is not a module: anything
`Engine.h` reaches is bidirectionally coupled with it and *is* the engine, so `gfx`,
`scene`, `ui` and `sim` are one cluster with `root` rather than four layers. A module is
exactly what root cannot reach. Two rules follow — nothing in `core` or the cluster may
name a module, and no module may name another — and the first is the point of the exercise,
because `Engine.cpp.o` is in every binary and a subsystem it names is a subsystem every
game links.

`nav` goes first because it is the smallest complete example: `NavMesh` includes only
`core/Profiler.h`, names no Vulkan and no `gfx::` symbol, `Engine` is the only thing in the
engine that names it, and there is no description to split. If the shape is wrong it is
wrong here, cheaply.

**This reverses two of G10's refusals** and they have to be answered rather than ignored —
see the Reference update below.

## Verification

- `./build.sh debug`, and the guard's own failure modes by hand: a hand-added upward
  include reported at the right `file:line`, a stale `ACCEPTED` entry, a cycle in the tier
  table rejected before any file is read, a directory in neither list.
- `./test.sh debug` — the count unchanged.
- `scripts/golden.sh check release` — 13 of 13 byte-identical. A navmesh draws nothing and
  this is what says so rather than assuming it.
- `nm -C --undefined-only` on the two binaries: `viewer` names no navmesh symbol,
  `battle_arena` names them all.

## Reference update

- [architecture/tooling.md](../../architecture/tooling.md) — `check_layers.sh`, the graph it
  holds, and the `ACCEPTED` ratchet.
- [architecture/principles.md](../../architecture/principles.md) — the Namespaces section:
  "there are four" becomes the cluster-and-modules definition.
- [architecture/README.md](../../architecture/README.md) — the tree, and the graph replacing
  "`gfx/` depends on nothing above it", which described an intent the includes never had.
- [architecture/limitations.md](../../architecture/limitations.md) — G10's two reversals,
  each answering the reason it was refused for.
- `CLAUDE.md` — the "exactly three base classes" sentence.

## Outcome

Landed as `d37140f`. The guard is `scripts/check_layers.sh`, beside the ASCII guard, holding
two lists rather than a row per directory because that is what the definition is. All six of
its failure modes were checked by hand. `core/Config.h`'s two edges were the only ones it had
to excuse, so `ACCEPTED` was empty within one phase and has stayed empty through every phase
since — the ratchet never had to hold anything.

`DebugView` and `AudioBackend` moved into `core/`, and `audioBackendNames()` moved with the
latter into `core/AudioBackend.cpp`, or `core` would have linked against audio.

`nav` migrated end to end. The wiring is a separate translation unit, and that is the finding
worth keeping: folding it into `NavMesh.cpp` gives that file an `Engine`, and the unit suite
could then no longer link it at all. `engine/Modules.h` declares one interface per module
whose base class *is* the null implementation — no `NullNav` subclass — so "what happens when
nothing is linked" is written beside the declaration and no call site needs an `if`.

    nm -C build/debug/viewer       | grep nav::  ->    0
    nm -C build/debug/battle_arena | grep nav::  ->  332

`./test.sh debug` 1081 passed, `scripts/golden.sh` 13 of 13 byte-identical, `scripts/arena.sh`
8 of 8 arms.

**What the estimate did not predict:** nothing here, but it hid something that cost the next
row a golden failure to find — an include does not link a module, and `nav` could not show it
because nothing in the golden set bakes a navmesh. See [D23](D23-five-subsystems-become-five-modules.md).
