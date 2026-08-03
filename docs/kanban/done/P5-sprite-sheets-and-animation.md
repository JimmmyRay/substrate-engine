---
id: P5
title: Sprite sheets and animation
arc: P
size: M
verification: golden-11, readback, tests-hosted, validation
---

# P5 — Sprite sheets and animation

Atlas slicing, named clips, frame timing. **Reuses C7's `AnimationEvent` and `firedEvents()`** rather than inventing a second event model

## Scope

**The card above was a stub** — one sentence, three nouns and a constraint, with no statement
of what the surface is or what it excludes. A card that cannot be checked against is not a
card, so the scope was written before any code was, and this section is that scope.

### What the surface is

Everything lands on `scene::SpriteTable`, which is hosted and already owns the UV rect. A
sheet is a rectangle on an existing sprite:

- **`SpriteSheetId`** — a third `core::Handle<Tag>`, alongside `SpriteId` and
  `SpriteLayerId`, under C1's rules. `createSheet`, `destroySheet`, `valid`, `sheetCount`.
- **`SpriteSheetDesc`** — cell size, columns, count, origin and spacing, all in **texels**,
  for the reason `SpriteDesc::uv` is in texels: the numbers are the ones an artist reads off
  the tool that drew the sheet, and nothing CPU-side knows the file's dimensions.
- **`SpriteClip`** — name, `first`, `count`, `fps`, `LoopMode`, `events`. One struct going in
  and coming out; the duration is `count / fps` and is derived rather than stored, so a
  retimed clip cannot carry a stale one. `addClip`, `findClip` (→ `kNoClip`), `clip`,
  `clipCount`.
- **`frameUv(sheet, frame)`** and **`frameAt(sheet, clip, time)`** — the slicing and the
  frame selection, both public and neither needing a playback, so a tilemap is
  `setUv(t, frameUv(sheet, cell))` and nothing else.
- **Playback** — `play`, `stop`, `setPlaying`, `setSpeed`, `playing`, `frame`, `clipTime`,
  `animatingCount`, `update(dt)`, `firedEvents()`.

`Engine::simulate` calls `update`, beside `SceneAnimator::update`.

### What it deliberately excludes, and why

- **No per-frame hold times, no ping-pong, no arbitrary frame list.** A clip is a contiguous
  run at one rate, which is what every sheet in the asset tree is. Trigger: a sheet in the
  tree with a held cell, at which point `SpriteClip` grows a `frames` vector and
  `first`/`count` become its degenerate case.
- **No sheet file format.** Aseprite's JSON and TexturePacker's agree about nothing, neither
  is in the asset tree, and a parser for a format no asset uses is a parser nobody can test.
- **No state machine.** `AnimationStateMachine` is already generic over clip indices and
  would drive these clips unchanged. Writing a second would be the exact duplication the
  card's own constraint is about.
- **No `AnimatedSprite`, no second pass, no second shader, no second table.** `GpuSprite`
  already carries a UV rect; an animated sprite is one whose rect is rewritten on a frame
  change. The GPU side of this row is empty.
- **No blending between clips.** A flipbook has no in-between, so `blendPose` has nothing to
  blend and a cross-fade would be two cells drawn at once.

## Verification

Everything below must pass before this may enter `done/`:

- `scripts/golden.sh check release` -- eleven cases, byte-identical. **Necessary and not
  sufficient**: no golden scene holds a sprite, so this only proves the row changed nothing
  it was not meant to.
- `scripts/readback.sh release` -- the check that matters for a P row, and it needed a new
  case. A frame of a sheet selected by the animation must land bit-identically where the
  source file says it should, at tolerance 0.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation. The frame-selection
  arithmetic is Vulkan-free and therefore fully testable.
- A validation-layer run over the sheet path, zero errors.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**The card was a stub** — one sentence naming three nouns and one constraint, with no
statement of the surface, no exclusions and a `verification` line that named `golden-12`, a
count that has not existed since `no-ibl` was retired, and *nothing else*. The Scope section
above was written before any code, and writing it changed the row twice: it is what made the
sheet a rectangle on an existing sprite rather than a subsystem, and it is what produced the
exclusion list, which turned out to be more of the row's value than the feature was.

**It also changed the verification.** As named, the card would have been closed on a golden
set that contains no sprite at all and a unit suite — the same shape of gap P7's outcome
warned about one row earlier, in almost these words: *"a count of configurations is not a
claim about coverage"*. What was added is `readback`, with two new cases, and that is the
only check in the tree that can fail a sheet showing the wrong cell.

### What landed

`scene::SpriteTable` grew sheets, clips and playback. `SpriteSheetId` is a third
`core::Handle<Tag>`; `SpriteSheetDesc` is cell size, columns, count, origin and spacing in
texels; `SpriteClip` is name, `first`, `count`, `fps`, `LoopMode` and events, with the
duration derived rather than stored. `frameUv` is the slicing and `frameAt` is the frame
selection, both public and neither requiring a playback — so a tilemap is `setUv(t,
frameUv(sheet, cell))`, which is what P8 will build on. `Engine::simulate` calls
`SpriteTable::update` beside `SceneAnimator::update`.

**No shader changed and no pass was added.** `GpuSprite` already carried a texel UV rect, so
the GPU half of this row is empty — an animated sprite is a sprite whose rect is rewritten on
a frame change, and only on a frame change, which is what makes a thousand sprites at 12 fps
on a 60 Hz step four fifths of no work.

### The animation vocabulary was reused, and here is exactly how far

`LoopMode`, `ClipPlayback`, `AnimationEvent` and `FiredEvent`'s shape are C7's, unchanged.
`advance` and `crossedEvents` are C7's functions, called rather than reimplemented.

**One thing had to move for that to be true, and it is the row's only edit outside the sprite
files.** Both functions took a `const AnimationClip&` and read exactly two fields of it — the
duration and the event list. A flipbook clip has neither samplers nor channels, so there were
three options: hand them a synthetic `AnimationClip` with two permanently empty vectors,
which is a lie about what the thing is; copy the wrap-and-clamp arithmetic into
`SpriteTable`, which is the anti-pattern this project names outright as *"copying an existing
private helper into a second class instead of moving it up one rung"*; or take what they
actually read. They now have an overload taking `(duration)` and `(events, duration)`, with
the `AnimationClip` forms as one-line forwards. `SceneAnimator`'s call sites are untouched and
the golden suite is byte-identical across the change, including `skin`.

**Two things were deliberately *not* reused, and both are refusals rather than omissions.**
`AnimationStateMachine` is generic over clip indices and would drive these clips unchanged, so
a second one is not written and `limitations.md` says so with a trigger. And `blendPose` has
nothing to blend: a flipbook has no in-between, so `play` replaces rather than fades.

One judgement call worth recording because it reads as an inconsistency: **`play` does not
take a `LoopMode` and `SceneAnimator::play` does.** For a skeleton, a transition clip and a
locomotion clip share one rig and the mode belongs to the moment; for a flipbook, whether an
animation repeats is a property of the animation an artist drew. The mode is on `SpriteClip`.

### What the readback proved, and the two ways it nearly proved nothing

Two cases, `sheet-cell1` (960x540, 1.5 fps, cell 1) and `sheet-cell2` (1000x600 letterboxed,
2.5 fps, cell 2). The source is cut into its own four quarters and the capture is held against
**one quarter** of the same file — 6,912 texels each, zero differing, tolerance 0.

The first draft of this check would have passed for any implementation at all, twice over:

1. **Cropping the source to the cell the animation selected** compares frame selection against
   itself. Fixed by having the script *state* the expected cell (`--readback-sheet-frame`) and
   the engine refuse to compare when the playback disagrees. A negative control confirms it:
   asking for cell 3 in the 1.5 fps case exits 1 with *"the sheet was showing cell 1 at frame
   60, not the cell 3 asked for"*.
2. **Computing the crop with `SpriteTable::frameUv`** — the same call the draw used — makes a
   transposed slicing agree with itself: the wrong cell drawn, the wrong cell expected, case
   green. The comparison site now derives the quarter from the image size directly. That is
   the same argument `compareReadback` already made about expanding rather than resampling,
   arriving a second time.

The third property is the pair itself: the two cases differ **only** in the rate and expect
different cells from the same run length, so a frame index taken from a constant, the frame
counter or the sprite's index cannot satisfy both.

**Determinism is `--locked`, and the slack is deliberate.** The capture on frame 60 is the
61st frame drawn, so 61 fixed steps of 1/60 s have run — 1.017 s, which is 1.525 cells at 1.5
fps and 2.542 at 2.5. Both land mid-cell on purpose, with 20 and 11 steps of slack. **This is
not caution, it is a defect the unit suite caught first**: a test asserting the cell after
exactly 60 steps failed, because 60 additions of `1.0f/60.0f` sum to a hair *under* one second
and land on the wrong side of a boundary. An assertion on a cell boundary is a question about
float accumulation, and it would have passed and failed at random.

### Verification

- `scripts/golden.sh check release` — **11 of 11**, byte-identical. Necessary and not
  sufficient, and the card now says so: no golden scene holds a sprite. One run reported
  `skin` failing and the log said `vkCreateDevice failed: VK_ERROR_DEVICE_LOST` before a
  pixel was drawn; a rerun was 11 of 11.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the resize soak clean. The
  seven that existed are unchanged; the two new ones are 6,912 texels each at 3x, one at
  (0,0) and one at the (20,30) letterbox offset.
- `./test.sh debug` — **746 of 746**. `./test.sh asan` — **746 of 746**. Thirteen new cases in
  `SpriteTableTests.cpp`, all hosted: the slicing including margins and gutters, the refusal
  of a zero cell, the floor-of-time-times-fps boundaries, the degenerate clip, the wrap, the
  `ClampToEnd` hold, the pause, the events, the swap-remove out of the middle of the playback
  walk, and a sheet destroyed under a live playback.
- Validation — 120 frames of the sheet path in a debug build, **zero errors**. The one warning
  is `VK_LAYER_PATH hid the system layers`, which the engine emits and fixes itself.

### Deferred, with triggers

Per-frame hold times, ping-pong and arbitrary frame lists (trigger: an authored sheet in the
tree with a held cell — `SpriteClip` grows a `frames` vector and `first`/`count` become its
degenerate case); a sheet *file* format (trigger: an authored sheet description shipping with
an asset); a sprite state machine (trigger: a transition rule a skeleton's machine cannot
express). All three are in `limitations.md`.

### One thing found and left alone

`compareReadback` grew a source rectangle, and its default — zero width or height meaning the
whole file — is the same "zero means everything" convention `SpriteDesc::uv` uses. That is now
the second place in the P arc where a zero extent means the whole image. **The third is the
one to extract**, and it is worth knowing that two already exist.
