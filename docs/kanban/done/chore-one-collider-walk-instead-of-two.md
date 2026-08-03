---
id: chore-one-collider-walk-instead-of-two
title: One collider walk instead of two
arc: chore
size: M
verification: golden, tests-hosted, validation, scripted-input
---

# chore-one-collider-walk-instead-of-two — One collider walk instead of two

Afterwards there is one function that turns a document's colliders into bodies, characters and
navmesh triangles, and both `Engine::initPhysics` and `Engine::addModel` call it.

## Why

The arithmetic is written twice and the second copy says so in its own comment: matching a
collider node to the instance slot its placement produced, resolving `auto` by motion, building
the body at the node's world transform, binding the driven node. `initPhysics` does it for a
document `--scene` loaded ([Engine.cpp:1386](../../../engine/Engine.cpp#L1386)); `addModel`
does it for every import ([Engine.cpp:702](../../../engine/Engine.cpp#L702)).

Two occurrences are a coincidence and the Rule of Threes says duplicate freely. C41 made the
import path the *only* one a game uses, so the copies are no longer symmetrical: the one every
game exercises is the second, and the first now runs for the harness alone. That asymmetry is
what makes this worth doing before a third caller arrives rather than after — a fix applied to
one copy and not the other is invisible until a game composes something the golden suite does
not.

The navmesh half is already unified in effect and not in code: `addModel` rebakes when
`m.colliderCount > 0` ([Engine.cpp:850](../../../engine/Engine.cpp#L850)) and `initPhysics` is
followed by `initNavigation`. One walk makes that one call.

## What to be careful of

The two differ in one real way and the extraction has to keep it: `initPhysics` runs before any
instance exists and builds its placement-to-slot table by re-walking the placements in the order
`addSceneInstances` used, while `addModel` has `modelInstances[id]` already. That agreement is
load-bearing and is why the walk is repeated rather than guessed at — the shared function takes
the slot list as an argument instead of deriving it.

## Verification

- `scripts/golden.sh check` — `physics` and `mirror` are the two cases with colliders.
- `./test.sh debug`, then `./test.sh asan`.
- `scripts/locomotion.sh` — nine arms. The character controller is bound by this walk.
- Zero validation errors with layers on.
- `game/battle_arena` still reports `2 colliders bound` and `Nav: 850 triangles`.

## Outcome

Two functions rather than one: `Engine::createColliderBodies(first, count, out)` and
`Engine::bindDrivenNodes(added, slots)`, with `DrivenBody` and `DrivenSlot` as the types
between them. `initPhysics` and `Engine::addModel` each call both, and each keeps its own
ordering visible at the call site.

**The split point was the finding.** The duplicated comment argued the two walks could not
share code because `initPhysics` runs `initCloth()` between the body loop and `finalize()` and
`addModel` must not -- "merging them behind a flag would be one function with two orderings",
and it was right about the flag. Cutting at that seam instead gives two functions with no
flags: the cloth call, the `sourceBody.assign` vs `.resize`, the `authored.clear()` and the
two different ways of pairing placements to instances all stay in the callers, where they are
the four lines that genuinely differ. About 90 lines of duplicated body became 30 shared plus
those four.

The other difference survived intact: `initPhysics` builds its slot list by re-walking the
whole placement table in `addSceneInstances` order, because no instance list exists yet, while
`addModel` reads `modelInstances[id]`, which is already in placement order. Both now produce
the same `std::vector<DrivenSlot>` and hand it to the same loop.

### Verification

| Check | Result |
|---|---|
| `scripts/golden.sh check release` | **13 of 13, byte-identical** -- `physics` and `mirror` are the cases with colliders |
| `./test.sh debug` | **1070 tests, 108 suites** |
| `./test.sh asan` | **1070 tests, 108 suites** |
| `scripts/locomotion.sh release` | **9 of 9 arms**, every figure unchanged |
| `validation`, `./run.sh demo debug` | 0 errors over 30 frames |
| `validation`, `./run.sh battle_arena debug` | 0 errors over 30 frames |
| `scripts/check_ascii.sh` | clean |

`game/battle_arena` still reports `2 colliders bound`, `Nav: 850 triangles, 1812 vertices, 30
regions`, and walks 5.68 m over 240 steps at `along 1.00, facing 1.00` -- identical to before
the extraction.

## Reference update

[architecture/systems.md](../../architecture/systems.md), "Colliders and bodies", which now
names the pair and the seam; and [architecture/limitations.md](../../architecture/limitations.md),
where the `addModel` entry no longer points at a duplication.
