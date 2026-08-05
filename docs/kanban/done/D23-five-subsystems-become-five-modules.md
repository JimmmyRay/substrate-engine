---
id: D23
title: Five subsystems become five modules
arc: D
size: XL
verification: tests-4, golden, scripted-input, leak, validation
---

# D23 — Five subsystems become five modules

[D22](D22-a-module-is-what-engine-h-cannot-reach.md) built the guard and proved the shape on
`nav`. This row applies it to the four that were left — particles, audio, animation, and
physics owning cloth — one module per phase, each landing green before the next begins.

**A phase is a module migrated end to end**, not a concern swept across all of them: move,
split the description back into `scene/`, declare the interface, add the registrar, define
the accessor in the module's own translation unit, point `Engine.cpp` at
`modules::<name>->`, and delete the member from `Simulation`. Each phase is independently
verifiable and small enough to finish, and `Simulation` empties a little further each time
rather than all at once. The order is smallest first, so the pattern is proved on a module
that fits in one sitting before it is applied to one that does not.

The engine-facing interface is the measurement that makes this affordable: physics 11
methods, anim 8, particles 8, audio 25 — about 12 once the diagnostics readouts collapse
into a `stats()` POD. Had the interface also had to serve *games* it would be a parallel
copy of each subsystem's whole public API, which is the maintenance cost that sinks this
pattern elsewhere. Games keep the concrete type and every call site they had; a game adds
one include.

## Verification

- `./test.sh debug`, `release`, `asan` — each its own invocation. ASan matters most in the
  physics phase, because teardown order is what that one can get wrong.
- `scripts/golden.sh check release` — 13 of 13 byte-identical, every phase. Nothing here
  changes behaviour, so a moved pixel is a defect in the move and re-snapping is not an
  available answer.
- `scripts/locomotion.sh debug` — 9 of 9 arms, every figure unchanged, for the animation
  phase.
- `scripts/arena.sh release` — 8 of 8 arms; the nearest thing to coverage the audio
  placement and occlusion paths have.
- `leak` — two create/destroy runs at different cycle counts, steady state compared.
- Zero validation errors and zero warnings.
- `nm -C --undefined-only` on `Engine.cpp.o`: no module symbol, after each phase.

## Reference update

- [architecture/systems.md](../../architecture/systems.md) and
  [architecture/limitations.md](../../architecture/limitations.md) — corrected where a
  phase falsified a line.
- The rest is [D22](D22-a-module-is-what-engine-h-cannot-reach.md)'s, written once.

## Outcome

Landed as `7403eab` (particles), `c84fd7b` (audio), `dafa1f9` (anim) and `7b5106f`
(physics + cloth). `Simulation` is three members now — sprites, the clock and the tree — and
its header names no module type. `check_layers.sh` is clean with `ACCEPTED` empty.

**Four things the plan got wrong, and each cost real time:**

1. **An include does not link a module.** The plan's step 7 said a game adds one include; an
   include creates no undefined symbol, and what pulls the archive member is *calling* the
   accessor defined in that translation unit. `nav` never showed it because nothing in the
   golden set bakes a navmesh. The `particles` case is a `viewer` run, so viewer had sixteen
   emitters spawning nothing into a pool of zero, in a build that reported no error. Found by
   `FAIL particles -- 1 of 13 cases differ`, not by anything earlier.
2. **The interface counts were low.** anim measured 8 and is 11 — `create`, `createMorphed`
   and `merge` are engine-facing, because `Engine::createMesh` and `Engine::addModel` both
   mutate the character set and neither may name the animator.
3. **`AnimationEvent` is not description.** The plan sent `LoopMode`, `ClipPlayback`,
   `advance` and `crossedEvents` to `core/`; `crossedEvents` takes a vector of
   `AnimationEvent` and `SpriteClip` stores one, so the type had to move or `scene::` would
   still own a flipbook's vocabulary.
4. **A span alone cannot replace the animator pointer.** `totalJoints` and `totalWeights` are
   sums over slot *capacities*, not over the live spans — a retired slot keeps a block larger
   than its live vector — so they are passed alongside or the renderer's buffers size short.

**What the migration surfaced that was not migration work:** all thirteen golden cases are
`viewer` runs, which makes `viewer` the worst available witness for "a game links only what it
names" — it now deliberately names four modules through a `linkModules()` helper, or those
cases would render nothing and pass. `nav` is the honest witness and the only one: viewer 0
symbols, battle_arena 43.

## Deferred

- **A second witness for the linking property.** A game out of `./new_game.sh` names no
  module and would demonstrate it without `viewer`'s harness obligations. Not measured;
  worth a `measure-` card if the figure is ever quoted.
