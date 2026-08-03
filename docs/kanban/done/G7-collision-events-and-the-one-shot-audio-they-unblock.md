---
id: G7
title: Collision events, and the one-shot audio they unblock
arc: G
size: M
verification: tests-hosted, validation
---

# G7 — Collision events, and the one-shot audio they unblock

M

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- `./test.sh debug`, then `./test.sh asan`, each its own invocation.
- Zero validation errors with layers on, in every capture.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

`PhysicsWorld::contacts()` returns the last step's contacts — both bodies as handles, the
manifold centroid, the normal and the impulse — and `AudioEngine::playAt` fires a sound at a
point and forgets it. The two landed together because separately neither is worth much: a
contact stream a game cannot react to audibly, or a one-shot with nothing to trigger it.

**Recorded, not dispatched.** Jolt calls a `ContactListener` from inside the solver with the
body lock held, so a game running there could not create or destroy a body — the one thing a
collision handler most wants to do. Contacts are recorded during the step and drained after
it. The per-contact cost inside the step is a few flops and a `push_back`.

**A contact cannot name a destroyed body**, and by construction rather than by luck: C1's
deferred reclaim moves the generation on `destroy` but only frees the slot at `reclaim()`,
at the top of the next step — which is exactly where the contact list is cleared. The two
lifetimes coincide. Pairs are canonicalised lower-slot-first and the list ordered, so a game
can key a cooldown on a pair without the key having two spellings.

`scripts/make_test_scene.py` grows a generated `impact.wav` — a quarter second, three
layers, from a hand-rolled LCG so it is byte-identical on every clone. It is also the first
short audio asset in the tree: both existing ones are multi-minute ambience beds, so every
source took the streaming side of the decode/stream crossover and the decode path ran
nowhere outside the unit suite.

Verification, both checks this card names: `./test.sh debug` and `./test.sh asan`, each its
own invocation — **654 tests passing in both**, up from 641, the 13 new ones covering contact
ordering, pair canonicalisation, a contact whose body is destroyed in the same step, and the
one-shot retirement path. Validation layers on, two captures — the demo scene at 120 frames
and `physics.gltf` at 200 — **zero errors**. The golden set is not part of this card's
contract and was not run for it.

**What was reverted before this landed.** The first pass at this row also rewrote
`tonemap.frag` — replacing the default ACES curve with Hill's matrixed fit and adding AgX,
PBR Neutral, Hable and Uchimura — with matching changes to `DebugView`, `Config` and their
tests. That is a rendering change on a collision-events card, and it moves pixels in every
lit golden case. It was removed from this row and kept; it wants its own card, where its
effect on the reference images is the subject rather than a side effect.
