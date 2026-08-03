---
id: D20
title: The sun is a light in the list like every other light
arc: D
size: S-M
verification: golden, tests-hosted, validation
---

# D20 — The sun is a light in the list like every other light

Afterwards `GameSetup` has no `sunDirection`, `sunColor` or `sunIntensity`, and a game that
wants a sun pushes a `makeDirectionalLight` into the light list exactly as it pushes a point
or a spot. `Renderer` keeps one index saying which light the cascades are fitted to, instead
of three authored members that are a fourth way of spelling a light.

**The engine already believes this everywhere except in the game's declaration.**
`LightType::Directional` is a light type ([Light.h:39-43](../../../engine/gfx/Light.h#L39)),
`makeDirectionalLight` builds a `GpuLight` like its two siblings, and
`Engine::initLights` takes the first directional light *out* of the scene's list to fill the
three renderer fields ([Engine.cpp:1560-1580](../../../engine/Engine.cpp#L1560)) -- so a sun
authored in a glTF is a list entry and a sun authored by a game is three scalars, and the
engine converts between them at load. The renderer then converts back, pushing the fields into
`lightScratch` as a directional light before shading
([Renderer.cpp:3959](../../../engine/gfx/Renderer.cpp#L3959)). One concept, three
representations, two conversions.

The cost is paid in `GameSetup`, which is the surface a game reads to learn what it may
author. Three fields there say a sun is a different kind of thing from a light, and a game
that wants two directional lights, or none, or one that moves, has to discover that the
answer is somewhere else entirely.

**What the singleton is actually for, and what survives.** The cascade fit and the shadow
lookup take one direction: `updateCascades` fits to it and the shader routes every directional
light through the resulting map, so a second directional light would be lit correctly and
shadowed wrongly ([Engine.cpp:1562-1566](../../../engine/Engine.cpp#L1562)). That constraint is
real and this row does not remove it -- it moves it from the authoring surface into the
renderer, where it belongs, as "which entry in `lights` the cascades follow" plus the existing
first-one-wins rule and its log line. A game gains nothing it could not express before; it
stops having to learn a second spelling for something it already knows how to write.

## Deliberately not in scope

`ambientColor`. It is a flat radiance added to every surface with no direction, no position
and no falloff -- there is no light list entry that means it, and making one would be inventing
a fourth light type to hold a constant.

## Verification

- `scripts/golden.sh check` -- every case byte-identical. This is a refactor of where a number
  is written and not of what it is, so any moved pixel is a defect in the conversion.
- `./test.sh debug`, then `./test.sh asan`.
- Zero validation errors with layers on.
- Every game in the tree builds with `game/` present and `engine/` builds with it absent, since
  both games author a sun today.

## Expected to be wrong about

Whether the sun index belongs on `Renderer` or on the light list itself as an ordering rule
("the first directional light"), which is what `initLights` already does and would mean the
renderer stores nothing at all.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) on lighting and the cascade fit,
and the `GameSetup` walkthrough in
[guides/making-a-game.md](../../guides/making-a-game.md), which authors the three fields.

## Outcome

`GameSetup` lost `sunDirection`, `sunColor` and `sunIntensity` and gained
`std::vector<gfx::GpuLight> lights`. Both games author their sun with `makeDirectionalLight`
into that list. `Engine::initLights` walks the scene's lights and then the game's and promotes
the first directional into the three renderer fields, which are derived now rather than
authored — which is the part of the card's premise that survived; the "one index on the
renderer" shape did not, because the sky bake, `updateCascades` and `updateUniforms` all want
the three scalars and an index would have been a lookup at each.

**The estimate missed the case that actually mattered, and the golden set caught it.** The card
assumed the walk was "first directional wins, everything else is a list entry". Run that way,
`mirror` and `mirror-no-rt` failed at 112,050 pixels over tolerance: `mirror.gltf` ships its
own directional light, so the scene's sun won and the *game's* sun was appended as a second
directional — which the shader routes through the first one's cascades. The old code never hit
this because a game's sun could not enter the list at all. The rule is therefore **one
directional light, not one sun plus extras**: a second is dropped and counted, which is what
the surrounding comment had argued for all along without the code being able to enforce it
against a game. That is strictly more correct than what was there, and it restored
byte-identical output.

**Verification.** `scripts/golden.sh check release`: 13 of 13 byte-identical, after the fix
above. `./test.sh debug` and `./test.sh asan`: 1063 of 1063, 107 suites. Validation: zero
errors in both games over 120 locked frames each. `battle_arena` reports `Lights: 0 from the
scene, 1 from the game (one taken as the sun)`.

## Deferred

- **[bug-the-ibl-is-baked-from-a-sun-no-game-authored](bug-the-ibl-is-baked-from-a-sun-no-game-authored.md)**,
  opened by this row. `createIblResources` runs inside `Renderer::init`, before any sun is
  applied, so the environment cube and the irradiance map are baked from the renderer's own
  default direction. It was invisible while `GameSetup`'s default happened to be the same
  vector; `battle_arena` and `mirror.gltf` both differ from it. Not fixed here — the fix moves
  a blocking bake and is expected to move golden baselines, which is a claim of its own.
