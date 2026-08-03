---
id: chore-every-sprite-is-uploaded-every-frame-changed-or-not
title: Every sprite is uploaded every frame, changed or not
arc: chore
size: M-S
verification: trace, golden-11, readback, tests-hosted, validation
---

# chore-every-sprite-is-uploaded-every-frame-changed-or-not — Every sprite is uploaded every frame, changed or not

`Renderer::recordSprites` memcpy's the whole dense array into mapped memory on every frame:

```cpp
// engine/gfx/Renderer.cpp:619
std::memcpy(frames[slot].spriteBuffer.mapped, draws.data(),
            static_cast<size_t>(count) * sizeof(scene::GpuSprite));
```

There is no dirty tracking, so a completely static screen of sprites pays the same ~640 KB of
write-combined traffic per frame as one where every sprite moved. This card gives
`SpriteTable` a dirty range and has the upload honour it, so the copy is proportional to what
changed rather than to what exists.

**Nothing is broken today and the measurement declines the urgency.**
[P4](../done/P4-the-sprite-pass.md) measured ten thousand sprites at 0.037 ms of CPU
*including* this upload, and `limitations.md` cites that number twice — once to decline a GPU
sort and once to decline a tilemap subsystem. So this is not a card about a frame time that
hurts; it is a card about a cost that is O(live sprites) when it could be O(changed sprites),
in the one subsystem the engine has that is meant to scale to tens of thousands. The trigger
`limitations.md` already names for reopening the tilemap row — a game whose live sprite set
exceeds 50,000 — is the same point at which this stops being free.

The precedent to follow is in `SpriteTable` itself. `prepare()` already tracks `sortDirty`
and does nothing on a frame where sprites only moved, and the image-slot rebind is gated on
`ImageTable::revision()`. Both are the same idea applied to different work; the upload is the
one place that still copies unconditionally. That makes it the third occurrence rather than
the second, and the sort key argument P4 recorded — a layer is the sort key and a position is
not part of it — is exactly what makes a dirty *range* tractable: the array stays in draw
order across a frame where things merely moved.

Worth expecting to be wrong about: whether a range beats a whole-array copy at all. A memcpy
of 640 KB is one linear write the prefetcher handles perfectly, and a scattered set of small
copies may lose to it below some density. The card should measure the crossover and, if the
answer is that the whole-array copy wins under a dirty *fraction* threshold, keep both and say
where the line is.

Adjacent and deliberately not taken here: `ensureSpriteCapacity` calls `vkDeviceWaitIdle` when
it grows (`Renderer.cpp:565`). That is correct at level load and a visible hitch if a game
ever streams sprites in mid-play. Different problem, different card.

## Verification

- `scripts/baseline.py --config release --zones --runs 3 -- --sprites 10000`, static
  and moving arms — the point is that the static arm must now be cheaper than the moving one,
  which is a property the current code cannot have. Several runs an arm.
- The crossover measured and written on the card: dirty fraction against whole-array copy, at
  1k, 10k and 50k sprites.
- `scripts/golden.sh` — eleven cases, byte-identical.
- `scripts/readback.sh` — nine cases. **Four of them draw sprites**, and a stale upload shows
  up there as a wrong or unchanged cell. The card as written did not name this and it is the
  check that mattered; see the Outcome.
- `./test.sh debug` and `./test.sh asan` — `SpriteTable` is hosted, so a dirty range is
  unit-testable without a device, and a range that under-reports is a stale sprite on screen
  rather than a crash. That is the failure mode the tests need to name.
- A validation-layer run across a sprite-buffer growth event, zero errors — the growth path
  destroys the very buffers the gate reasons about.

## Reference update

[architecture/rendering.md](../../architecture/rendering.md) — the sprite pass section
describes the upload as one memcpy of an array already in draw order, which is what changes.
[architecture/limitations.md](../../architecture/limitations.md) cites the 0.037 ms figure in
two declined rows; if the number moves, both citations move with it.

## Outcome

### What the cost actually was, measured before anything changed

The card asserted the cost and did not have a number for it, because P4's `0.037 ms` was the
whole CPU delta of ten thousand sprites — creation, `prepare()` and the copy together — and
the copy on its own had never been separated out. `Renderer::record/Sprites` now exists as a
CPU zone, so it can be, and at HEAD it was:

| `--sprites N` | bytes copied | `Renderer::record/Sprites` | share of `Renderer::record` |
|---|---|---|---|
| 1,000 | 64 KB | 0.006 ms | 9% of 0.064 |
| 10,000 | 640 KB | **0.036 ms** | **36% of 0.099** |
| 50,000 | 3.2 MB | **0.171 ms** | **73% of 0.234** |

Linear, at about 18 GB/s into write-combined memory, and **it was the single largest thing
`Renderer::record` did in this scene at ten thousand sprites** — larger than `Cull`, `GBuffer`
and `Bloom` put together. So the card's own framing was too modest: this is not only a cost
that is O(live) where it could be O(changed), it was already the dominant term.

Where it is small: below about a thousand sprites it is under a hundredth of a millisecond
and under a tenth of command recording, which is where a game that draws a HUD and a few
dozen actors lives. **A thousand is the count below which this was genuinely free**, and
`limitations.md`'s 50,000 trigger is the count at which it was a sixth of a millisecond of
pure memory traffic for a screen that may not have moved.

### A revision counter, not a dirty range — and the card asked to be told which

The card said *"worth expecting to be wrong about: whether a range beats a whole-array copy
at all"*, and the answer is that it does not, for a reason that is not about the prefetcher.
**A dirty range would have to be a range per frame in flight.** Each of the three slots last
uploaded at a different revision, so what each one needs is the *union* of every range since
its own last copy — three range sets to maintain, merge and retire, to replace one linear
write with several scattered ones into write-combined memory. `InstanceTable` declined that
trade in exactly those terms, and the crossover the card asked for does not need measuring
because the winning arm is not a range at all: **a static screen skips the copy entirely**,
which is zero, and no dirty fraction beats zero.

That makes this the *fourth* occurrence of the pattern, not a new one — `InstanceTable`'s
`revision()`, G4's `materialRevision()`, P1's `ImageTable::revision()`, and now this — so the
shape was copied rather than designed.

**The card asked which kind the sprite buffer is, against `systems.md`'s one deliberate
exception.** It is the gateable kind. The instance history array is exempt because it changes
at the end of every frame whether the table did or not; nothing in `GpuSprite` has that
property. Every one of its 64 bytes changes only when a game or a playback writes it, so the
buffer is a pure function of the revision and the gate is total.

### The mutators, enumerated, and how each is covered

Thirteen writers of `gpu`, in three groups. The enumeration is the deliverable here — a
counter that misses one is a sprite that stops updating on one frame slot in three, and that
failure is invisible in a still image.

| Writer | Bumps | Covered by |
|---|---|---|
| `setPosition`, `setSize`, `setPivot`, `setRotation`, `setUv`, `setTint`, `setFlip`, `setImage` | in `at()` | `EveryPerSpriteSetterBumpsIt` — all eight individually |
| `create` | at the `push_back` | `EveryLifetimeChangeBumpsIt` |
| `destroy` (and `destroyLayer` through it) | after the swap-remove | `EveryLifetimeChangeBumpsIt` |
| `sort()` | unconditionally, so `createLayer` and `setLayerOrder` are covered by the sort they force | `EveryLifetimeChangeBumpsIt` |
| `applyFrame` | **inside** the frame-changed guard | `PlayAndAFrameChangeBumpItAndAHeldCellDoesNot`, and `readback`'s `sheet-cell1`/`sheet-cell2` |
| `prepare()`'s image reconcile | when `ImageTable::revision()` moves | `TheImageReconcileBumpsIt` |
| `shutdown()` | bumps rather than resets | `ShutdownBumpsItRatherThanResettingIt` |

**The eight setters have a structural guarantee and the other five do not, which is why the
five are enumerated.** `at(SpriteId)` is the only way to reach a writable `GpuSprite`, and it
bumps on the way in — so a ninth setter written next year gets it by construction. It bumps
*after* refusing a stale handle, so a game holding a dead id forces no copy
(`AStaleHandleChangesNothingAndSaysSo`).

Six things deliberately do **not** bump, and each is asserted the other way rather than left
unstated: `createSheet`, `addClip`, `destroySheet`, `stop`, `setPlaying`, `setSpeed`. None
writes a byte the pass reads — `destroySheet` in particular leaves every sprite on the cell it
was showing, which P5 chose and this row must not undo.

### The check proved rather than assumed, twice

The hazard named on this card was that *a test which only moves a sprite passes for an
implementation that misses the sheet-frame path*. Both halves were verified by breaking them:

1. **`++rev` removed from `applyFrame`.** `SpriteSheetRevisionTest` failed on `play` and on
   the step that changes cell; **every other sprite test passed**, including
   `MovingSpritesDoesNotResort` and `EveryPerSpriteSetterBumpsIt`. Then the same build through
   `scripts/readback.sh release`: `sheet-cell1` and `sheet-cell2` failed at **6912 of 6912
   texels, max delta 255**, while `sprite`, `sprite-letterbox` and the seven others passed
   clean. A row that had checked the sprite path with a *static* sprite would have shipped it.
2. **`++rev` removed from `at()`.** `EveryPerSpriteSetterBumpsIt` failed; the other five
   revision tests and all thirty-one older sprite tests passed.

Both were restored and re-verified green. A test that cannot fail is not a check, and these
two runs are the evidence that these can.

### The numbers, with the arms

`--sprites-move` was added because the card's own verification asked for a moving arm and no
such arm existed: **`--sprites N` alone is a static screen, which is the arm that can only get
faster.** It nudges every sprite by a fraction of a texel every frame — same overdraw, same
draw, different revision — so it pins what the copy costs when the gate does not fire.

1600x900, 1x MSAA, release, `--locked`, medians over four runs of 900 frames per arm:

| | 1,000 | 10,000 | 50,000 |
|---|---|---|---|
| `Renderer::record/Sprites`, **moving** | 0.006 ms | **0.036 ms** | **0.176 ms** |
| `Renderer::record/Sprites`, **static** | 0.001 ms | **0.002 ms** | **0.002 ms** |
| `Renderer::record`, moving | 0.065 ms | 0.097 ms | 0.237 ms |
| `Renderer::record`, static | 0.060 ms | 0.061 ms | 0.062 ms |
| `Sprites` (GPU) | 0.007 ms | 0.049 ms | 0.278 ms |
| `Lighting` | 0.034 ms | 0.034 ms | 0.035 ms |
| `Frame` | 0.319 ms | 0.365 ms | 0.599 ms |

**The moving arm reproduces the pre-change numbers to a thousandth of a millisecond** — 0.036
against 0.036 at ten thousand, 0.176 against 0.171 at fifty thousand — which is the
cross-check that the gate costs nothing when it does not fire and that the two arms differ in
the upload and nothing else. `Renderer::record` on the static arm is now **flat in the sprite
count**: 0.060 / 0.061 / 0.062 across a fiftyfold range, where it used to be 0.064 / 0.099 /
0.234.

**A/B/A was run and it mattered.** The static 10,000 arm was measured, then the moving arm,
then the static arm again: 0.002 / 0.036 / 0.001–0.002. The third arm landing back on the
first is what says the difference is the change rather than drift.

`Sprites`, `Lighting` and `Frame` are unmoved across every pair, and that is the correct
result rather than a disappointing one: the copy is CPU work feeding a draw whose cost is
overdraw, so a frame that is GPU-bound on 4x overdraw does not get shorter. What this buys is
**a third of command recording back at ten thousand sprites and three quarters of it at fifty
thousand**, which is CPU headroom for the game rather than frame time for the renderer.

### Verification

- `scripts/golden.sh check release` — **11 of 11 match**, on three separate runs. A fourth
  run reported `1 of 11 cases differ` and the two runs either side of it were clean; that is
  the same intermittency P5's outcome recorded, and no golden scene contains a sprite, so
  nothing this row touched can reach those pixels. Recorded rather than smoothed over.
- `scripts/readback.sh release` — **9 of 9 bit-identical**, plus the lit silhouette and the
  resize soak clean. This is the check that mattered and the card had not named it.
- `./test.sh debug` — **814 of 814**. `./test.sh asan` — **814 of 814**. Eight new cases:
  six in `SpriteRevisionTest`, two in `SpriteSheetRevisionTest`.
- Validation, debug build, layers on: 240 frames at 1000x600 with `--sprites 10000` and
  `--resize-every 20` — **zero errors** across 12 swapchain recreates and **3 sprite-buffer
  growth events**, which is the path that destroys the buffers the gate reasons about. A
  second run, 180 frames at `--sprites 5000 --sprites-move` — zero errors.
- A capture at frame 150 of a 400-sprite static run: all 400 present and correct, a hundred
  and forty frames after the last upload. The still image is the weakest of these checks and
  it is listed last for that reason.
- `scripts/baseline.py --config release --zones` — the table above, six arms, A/B/A on the
  10,000 pair.

### What the estimate did not predict

- **The card sized this M and it is S.** Thirty lines of engine change; the tests and this
  Outcome are most of the diff. Sizing it M assumed a dirty range, and the range was the
  wrong answer.
- **`at()` was already the funnel and nobody had noticed.** P4 wrote it to refuse a stale
  handle once rather than eight times; that it is also the single choke point through which
  every setter reaches the buffer is what made the hazard structural instead of a checklist.
  The Rule of Threes says extract at the third occurrence — this is the case where the
  extraction had already happened for a different reason and paid a second time.
- **The measurement was worth taking before the fix and not only after.** P4's 0.037 ms was a
  bundle; separating the copy out of it turned "a cost that could be smaller" into "the
  largest single thing the renderer records", which is a different card. That separation was
  only possible because CPU zones per pass landed first.

### Deferred, with the trigger stated

- **A dirty range.** Declined by the argument above rather than by measurement, and the
  trigger is a shape rather than a count: a game where one sprite in ten thousand changes per
  frame, *every* frame, so the gate fires every frame and copies 640 KB to move 64 bytes. At
  that point the range is per frame slot and the union bookkeeping is the work.
- **A view cull on `SpriteTable`.** Untouched and still absent — every live sprite is drawn.
  `limitations.md` already names it ahead of a tilemap subsystem at 50,000 live sprites, and
  the gate makes it *more* attractive rather than less: with the upload free for a static
  screen, the remaining O(live) cost is the draw itself.
- **`ensureSpriteCapacity`'s `vkDeviceWaitIdle`.** The card named it adjacent and not taken,
  and it is still not taken. Trigger unchanged: a game that streams sprites in mid-play. This
  row touched the function only to zero the per-slot revisions it invalidates.
- **`stats.particles`' staleness**, found and left alone by P4, is still there. Still one
  line, still in a pass this row did not touch.
