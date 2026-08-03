---
id: C28
title: More than one audio listener
arc: C
size: M
verification: tests-hosted, golden-11
---

# C28 — More than one audio listener

Afterwards a game can place the ears itself, and can have more than one pair. Today there is
exactly one listener, it is hardcoded, and the engine owns where it is:

```cpp
// engine/scene/Audio.cpp:261
engineConfig.listenerCount = 1;
```

`AudioEngine::setListener` ([`Audio.h:281`](../../../engine/scene/Audio.h#L281)) takes no
listener index and writes miniaudio listener 0. `Engine::beginFrame` writes it from
`cameraState` every frame at [`Engine.cpp:1775-1781`](../../../engine/Engine.cpp#L1775) —
*before* `Game::frameUpdate` runs, so a game cannot correct it for the frame it applies to.
Occlusion and the audio debug draw both read the one `listenerPosition()`.

miniaudio supports several listeners; the 1 is ours.

The camera-is-the-ears binding is *stated* (`Audio.h:279-281` says `forward` and `up` are the
camera's), so it is a decision rather than an accident — but it is the wrong decision for
three shapes that are not exotic. Split-screen has two players hearing one room from one
point. First-person wants the ears at the character's head, not at a camera that has moved
for a cutscene. A top-down or RTS camera hovers tens of metres up, so every sound is distant
and unpanned; the listener wants to be at the cursor or the selected unit.

Depends on nothing, and does not depend on [C25](C25-a-frame-renders-more-than-one-view.md) —
a single-view game still wants the ears somewhere other than the eye. C25 is what makes the
split-screen *case* reachable, not what this row needs.

Expected to be wrong about: how a spatial source's gain combines across two listeners.
miniaudio's own answer may not be the one a split-screen game wants, and "loudest wins" versus
"summed" is a decision this row has to make rather than inherit.

## Verification

- `./test.sh debug`, then `./test.sh asan`. `scene/Audio.cpp` is in
  `SUBSTRATE_HOSTED_SOURCES`, so listener placement and per-listener attenuation are testable
  with no device: one source, two listeners at different distances, both gains asserted.
- `scripts/golden.sh` — eleven cases, byte-identical. Audio moves no pixels, so any change is
  a defect elsewhere.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the audio section, which describes
the listener as the camera's.

## Outcome

**The card's "expected to be wrong about" was the whole row, and it was wrong in a sharper way
than it guessed.** It asked whether miniaudio's own combining rule is the one a split-screen
game wants, and assumed there was a rule to inherit. There is not.
`ma_engine_node_config_init` zeroes its config; zero is a **valid listener index** rather than
the `MA_LISTENER_INDEX_CLOSEST` sentinel, so miniaudio pins every sound to listener 0 for its
whole life. `ma_sound_set_pinned_listener_index` cannot put it back — it refuses any index at
or past the listener count and the sentinel is 255 — and `ma_sound_config` carries no field for
it. So a second pair of ears could be asked for, created, positioned and drawn, and would never
have heard anything.

Found by measuring rather than by reading: a probe with listener 1 one metre from a source and
listener 0 thirty metres away mixed at **exactly** the thirty-metre level, byte for byte the
same number as the one-listener arm. `AudioEngine::update` now picks the nearest listener per
spatial voice and pins it, skipping the loop entirely at one listener, which is every scene in
this tree.

Nearest rather than summed is the decision the card asked for, and the argument is that summing
makes a room louder as players are added and doubles a sound both of them can hear — a source's
loudness would stop being a property of the scene.

**Two things the card did not name.** The camera-is-the-ears binding needed a switch rather than
a re-ordering: the engine writes listener 0 before `Game::frameUpdate`, and moving it after
would overwrite the game's instead of the other way round. Neither order is right for both, so
`GameSetup::listenerFollowsCamera` says which of the two owns the ears. And occlusion had to
decide something too: the filter is one biquad on one voice, so a source is occluded only where
*every* listener is behind something — muffling a sound the second player can see plainly is
the worse mistake.

**Verification.** 992 tests, debug and ASan, three of them new: the listener count clamped at
both ends, an index past the count ignored rather than folded onto 0 (which would put both
players' ears in one place and read as a panning bug), and the nearest-ears rule as a level
measurement — the same source at the same place, thirty metres from the only ears in the first
arm and one metre from the second pair in the second. `scripts/golden.sh` — 11 of 11.
