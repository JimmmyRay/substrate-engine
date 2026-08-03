---
id: bug-attachments-on-every-rig-follow-the-first-character
title: Attachments on every rig follow the first character
arc: bug
size: S
verification: tests-hosted, golden-11
---

# bug-attachments-on-every-rig-follow-the-first-character — Attachments on every rig follow the first character

With two animated characters in a scene, the second one's node-attached particle emitters,
sounds and rigid placements follow the **first** character's joints. A torch on character 1
burns at character 0's hand; a bell on character 1 rings from character 0's chest.

Both sweeps read character 0 unconditionally:

```cpp
// engine/Engine.cpp:2043  (placements, then emitters)
const std::vector<glm::mat4>& world = sceneAnimator.worldTransforms(sceneAnimator.characterAt(0));
// engine/Engine.cpp:1957  (audio sources)
const std::vector<glm::mat4>& world = sceneAnimator.worldTransforms(sceneAnimator.characterAt(0));
```

A `ParticleEmitter::node` and an `AudioSourceDesc::node` are joint indices into *some* rig's
node array, and nothing records which rig. The index is then resolved against character 0's
transforms whatever it was authored against.

**The comment above the first sweep defends a narrower case than the code covers.** It argues
"Character 0's pose is the one they follow, since a rigid node belongs to the scene rather than
to any one copy of a character" — which is correct for `spawnExtraCharacters`' clones, where
every copy shares one skin and the rigid nodes genuinely belong to the scene. It is not correct
for two *different* rigs, and it silently mixes a third case: a scene with an animated
hierarchy of its own (a drawbridge, a clock tower) merged alongside a character rig, where the
building's nodes are character 0's and the character is character N.

Blocks any game with two animated characters that attach anything — which is most games with
two animated characters. Four-player co-op puts every player's effects on player one.

Related to but distinct from [D19](D19-a-rig-locomotion-parameters-belong-to-the-rig.md): that
row is one `Parameters` struct shared across pairs, this one is a hardcoded index 0. Both are
the plural animator C23 built being read by a singular caller.

Expected to be wrong about: whether the fix is a rig id on the attachment or a per-character
sweep. The first is a wider data change than it looks — `ParticleEmitter` and
`AudioSourceDesc` both carry the bare node index through the glTF `extras` schema, so the
authoring side has to say which rig too, and today a file cannot.

## Verification

- `./test.sh debug`, then `./test.sh asan`. `scene/Animation.cpp`, `scene/ParticleSystem.cpp`
  and `scene/Audio.cpp` are all in `SUBSTRATE_HOSTED_SOURCES`: two characters posed
  differently, an emitter and a sound attached to a joint on the second, both asserted to sit
  at the second character's joint and not the first's.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case has two animated
  characters, which is why this has never shown up as a moved pixel.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the animation section, where node
attachment is described.

## Outcome

**Neither of the two answers the card expected.** It offered a rig id on the attachment or a
per-character sweep, and correctly called the first a wider data change than it looks — the
authoring side cannot say which rig, so a field for it would arrive empty from every file that
exists. What landed needs no data change at all: **the rig is already recoverable from the node
index**, because a skin lists its joints and a clip names its channels. `characterForNode`
resolves it and `Engine::poseFor` is the one place both sweeps ask.

The map is rebuilt at the top of every `update` in two passes, and the order is the whole
design. Skins first, so a node a skin lists is that skin's character's. Clips second and only
for what the skins left, so a clip that happens to mention a joint cannot take it from its own
skin. First claim wins, which is what keeps N copies of one rig agreeing on a shared joint — the
case the old comment was right about, and now the case a test pins.

**The third case the card said was silently mixed in is closed rather than deferred**, and it is
what the second pass is for: an animated hierarchy of its own — a drawbridge, a clock tower —
merged beside a character rig has nodes no skin claims, and the character that moves one is the
character playing the clip that names it. Writing that test found the rule's honest edge: a
character plays clip 0 until something says otherwise, so with the tower's clip as the only clip
in the rig *every* character claimed its nodes. The test now gives both rigs a clip of their own
so clip 0 is the base scene's, and the edge is stated rather than papered over.

**Verification.** 985 tests, debug and ASan, three of them new: a joint on the second of two
merged rigs resolving to the second character and reading its posed transform rather than the
bind pose character 0 leaves it at; two copies of one skin agreeing across frames; and a rigid
node following whoever plays its clip. `scripts/golden.sh` — 11 of 11, byte-identical, as the
card predicted: no golden case has two animated characters, which is why this never moved a
pixel.
