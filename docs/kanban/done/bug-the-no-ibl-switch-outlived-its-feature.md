---
id: bug-the-no-ibl-switch-outlived-its-feature
title: The no ibl switch outlived its feature
arc: bug
size: S
verification: golden-11, tests-hosted, validation
---

# bug-the-no-ibl-switch-outlived-its-feature — The no ibl switch outlived its feature

`debug_frames/golden/lit.png` and `debug_frames/golden/no-ibl.png` were byte-identical, and
had been through the corrupt reference set as well as the repaired one — so it predated
[the re-snap incident](chore-the-corrupt-golden-set-and-the-re-snap-that-caused-it.md)
rather than being caused by it. Twelve golden cases, and one of them pinned a copy of
another.

Two readings, with opposite consequences: either `--no-ibl` genuinely changed nothing about
that frame, in which case the case was worthless as written; or the flag had stopped
reaching the lighting pass, in which case a documented switch was dead and the case that
would have caught it was passing *because* of the bug. It is the second, with a third fact
that decides what the fix is: **the flag reaches everything it ever reached, and the thing
it switched no longer exists.**

## What was actually wrong

The plumbing is intact end to end. `--no-ibl` is a row in `Config.cpp`'s `kBoolFlags`
mapping to `Id::render_ibl`; `SettingsBind.cpp` bound that id live to `Renderer::iblEnabled`;
`featureKey()` mixed it in, so flipping it rebuilt the pipelines; `shadingConstants()` fed it
to specialisation constant 2. `--no-ibl --dump-settings` printed `render.ibl bool false cli
--no-ibl`. **G2 is not implicated** — none of this is a row that stopped being applied.

`features.glsl` declared `ENABLE_IBL` at constant_id 2. **No shader read it.** Commit
`9524dac` ("Replace the shadow maps with traced rays, and remove the ambient that was
standing in for a room", 2026-07-29, 27 commits before this one) deleted the single
`if (ENABLE_IBL) { ... }` block from the deferred lighting body along with the split-sum
environment term, and renumbered the declaration from id 4 to id 2 on the way past —
carrying the gate forward while removing the only thing behind it.

So the switch was six kinds of surface over an empty hole: a command-line flag, a settings
row, a JSON key, an F9 binding, a specialisation constant, and a golden case. Flipping it
flipped a setting, rebuilt every shading pipeline for a hitched frame, and moved no pixel.

Established rather than inferred, at HEAD before any change:

| | md5 |
|---|---|
| Sponza frame 60, default | `dfdede445c30363305e8bc72fe8e8eb7` |
| Sponza frame 60, `--no-ibl` | `dfdede445c30363305e8bc72fe8e8eb7` |
| `debug_frames/golden/lit.png` | `dfdede445c30363305e8bc72fe8e8eb7` |
| `debug_frames/golden/no-ibl.png` | `dfdede445c30363305e8bc72fe8e8eb7` |
| `emissive.gltf` frame 60, `--no-ibl` | `03e2c5718bd16cefcfe75e4fdd4572f1` |
| `debug_frames/golden/emissive.png` | `03e2c5718bd16cefcfe75e4fdd4572f1` |

A second scene, lit unlike Sponza, is there to close off "the case needs a better camera".
It does not: with no reader for the constant, **no scene and no camera can distinguish the
flag**, so no golden case could have been written that would.

## What was done

Retired, rather than re-pointed. There is no environment term to switch off, and inventing
one — gating the skybox, or the environment a reflection ray hits on a miss — would be a
second lie in place of the first, since neither is image-based *lighting*.

- `features.glsl` — `ENABLE_IBL` gone, id 2 left **vacant** and documented. Not renumbered:
  the index into `GraphicsPipelineDesc::constants` *is* the `constant_id`, so closing the
  gap moves ids 3–6 in four shaders. This is the choice `ibl.glsl` already made for bindings
  0 and 1 when the two cubes stopped being read. `shadingConstants()` keeps a `0u`
  placeholder at that index; a specialisation map entry naming an id no module declares is
  ignored by definition.
- `render.ibl`, `--no-ibl`, its `--help` line, the `bindLive`, `Renderer::iblEnabled`, its
  `featureKey()` bit, the `"ibl"` key in `substrate.json`, and the demo's `Toggle.Ibl` action
  are all gone. Bloom keeps F10 rather than sliding up into the freed F9, so a bindings file
  saved before this still means what it says.
- `scripts/golden.sh` — the `no-ibl` case is removed, with the reason in place of it. The
  suite is **eleven cases**. Its reference image was not touched; no re-snap was needed or
  taken, because removing a case changes no other case's output.
- The verification vocabulary is `golden-11`. `golden-12` stays accepted by
  `scripts/kanban.py` because the cards in `done/` ran against twelve and their record has
  to keep saying so. A third spelling is the point at which the count comes out of the token
  instead of accumulating in that set.

**A replacement case is deliberately not in this card.** One would need a baseline, and a
baseline is a separately authorized snap — so the honest move is to leave the suite smaller
and true rather than the same size and decorative. `--fog` is the strongest candidate: fog
is off by default, is a whole documented pass at 0.773 ms, and nothing in the suite covers
it.

## What makes it stay fixed

Nothing under `engine/gfx/` is in `SUBSTRATE_HOSTED_SOURCES`, so **no unit test reaches
this and none was written to pretend otherwise.** Three things hold it instead:

- A retired flag now *says so*. `--no-ibl` gets `Unknown option '--no-ibl' (try --help)`, and
  an older `substrate.json` carrying `"ibl": true` loads with
  `unknown setting 'render.ibl' -- see --dump-settings for every key there is` and applies
  everything else. Both were silent no-ops before; the loud version is what would have made
  this findable on the day it broke.
- The vacant id in `features.glsl` carries the whole story, so the next person to want
  constant 2 reads why it is empty before reusing it.
- `architecture/rendering.md` no longer describes the environment term as live, which is what
  made the flag look load-bearing to anyone reading the reference rather than the shader.

## Verification

- `scripts/golden.sh check release` — **all 11 cases match**, byte-identical. No reference was
  re-snapped or edited.
- `./test.sh debug` — 677 tests in 74 suites, 0 failures.
- `./test.sh asan` — 677 tests in 74 suites, 0 failures.
- `./run.sh demo debug -- --headless --frames 60 --validation on` — exit 0, zero validation
  errors, `Frame 4.460ms`. The pipeline that used to be specialised on the removed constant
  builds and records clean.
- `--dump-settings` — 90 rows where there were 91, and the removed one is the only difference.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the ambient term is a constant
rather than a lookup, what is left of the environment chain and which two of its four outputs
now have no reader, and `ENABLE_IBL` out of the feature-constant list.
[architecture/tooling.md](../../architecture/tooling.md) — eleven cases, and why a case that
cannot fail for its own reason is worse than no case.
[architecture/README.md](../../architecture/README.md) — the global-illumination bullet.

## Outcome

**The interesting part is which of the two readings it was, and that it was neither cleanly.**
The card was opened on a suspicion that a flag had stopped being applied. It is applied
perfectly; what went missing is the code that reads the result. That failure mode is invisible
to every check this project has — the setting dumps correctly, the pipeline rebuilds, the
validation layers are happy, and the golden case *passes* — and the only symptom it ever
produced is the one that started this: two reference images with the same hash.

**The golden suite reported twelve cases of coverage and had eleven.** That is worse than
having eleven, because the twelfth was not merely idle: it was the case specifically
responsible for noticing that IBL had stopped working, and it went green every single time
for exactly the reason it should have gone red. The suite's size is a number people trust.

**The estimate did not predict the documentation.** Four architecture files described the
split-sum environment term as the live ambient path, one of them in the top-level summary
under "Global illumination". Removing a feature is three deletions and a doc sweep, and
`9524dac` did the deletions. Half of this card is that sweep, and the reason it belongs here
rather than in its own card is that the reference is *why* the switch still looked real.

**Found and left alone:** `createIblResources` still bakes `irradianceCube` and
`prefilteredCube` at startup and still binds them at ibl set bindings 0 and 1, where nothing
has sampled them since `9524dac`. Two cube images and two compute shaders' worth of startup
work for no reader. That is code rather than documentation and it is its own card;
`docs/superpowers/specs/2026-07-29-baked-probes-and-maps-design.md` already plans to take
those two bindings for probe data.
