---
id: chore-the-pose-resolve-allocates-once-per-character-per-step
title: The pose resolve allocates once per character per step
arc: chore
size: S
verification: trace, tests-4, golden-11, scripted-input, validation
---

# chore-the-pose-resolve-allocates-once-per-character-per-step — The pose resolve allocates once per character per step

`SceneAnimator::resolve` heap-allocates its visited set on every call:

```cpp
// engine/scene/Animation.cpp:530
const std::vector<SceneNode>& nodes = c.pose.nodes;
std::vector<bool> done(nodes.size(), false);
```

and `resolve` runs once per character per fixed step. This card makes it a reused member —
`SceneAnimator` already keeps `eventScratch` for exactly this reason, so the pattern is in the
file and the change is to follow it rather than to invent anything.

The reason it is worth doing is on the record already.
[limitations.md](../../architecture/limitations.md) names it in the standing answer to
"should the frame be threaded": CPU animation is the one cost in the engine that scales with
content — 2.4 ms at 256 characters, 7.7 ms at 1024 — and the section says to fix the
allocation and the repeat-until-stable resolve *before* reaching for threads. That makes this
a prerequisite of a question the project has already decided how to answer, not an
optimisation looking for a justification.

The loop above the allocation is the second half and this card does not take it. It repeats
until nothing is left because glTF does not require the node array to be topologically
ordered, so it is O(nodes) when the file is ordered and O(nodes²) when it is not. A cached
topological order is the real fix, it is a different change with a different risk, and
bundling the two would make one measurement stand for both. Note it here so the next card
has somewhere to start.

## Verification

- `scripts/baseline.py --config release --zones --runs 3 -- --characters 256`, then again at
  1024 — `simulate` is the zone, and the numbers to beat are the 2.4 ms and 7.7 ms in
  [limitations.md](../../architecture/limitations.md). Several runs an arm.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — each its own
  invocation. `SceneAnimator` is in `SUBSTRATE_HOSTED_SOURCES`, so the animation tests cover
  this directly, and a scratch buffer shared across characters is exactly the shape TSan
  exists to check.
- `scripts/golden.sh` — eleven cases, byte-identical. Reusing a buffer must not change a
  pose; if it does, it was not being cleared.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — the threading section names this
allocation as outstanding, and closing this card is what lets that sentence be rewritten to
name only the resolve order. **The card said `limitations.md` and that is wrong**: the
standing "should the frame be threaded" answer, its table and the sentence in question are
all in `tooling.md`; `limitations.md` carries the one-paragraph "multi-threaded command
recording is delegated" note and says nothing about animation. Corrected here rather than
followed.

## Outcome

**The premise held, the fix is two lines, and the number is smaller than the card's framing
implies but larger than an allocation-shaped guess.** `resolve` did heap-allocate a
`vector<bool>` per character per fixed step, G11 and G12 changed nothing about that path,
and it is now `resolvedNodes.assign(nodes.size(), false)` against a member. What it was
worth, measured rather than reasoned about: **179 ns per character per step.**

### The scale, which is the part worth writing down

**The ratio is scale-invariant and that is the whole answer to "at what N does this
matter".** The saving is per character per step and so is the work it sits next to, so it is
~2.4% of the animation cost at *every* character count — it never becomes a large fraction,
only a large absolute number.

| Characters | `simulate` before | after | delta | separable? |
|---|---|---|---|---|
| 1 (`skin.gltf`, the demo rig) | — | — | ~0.0002 ms | No, by four orders of magnitude |
| 256 | 2.024, 2.011, 1.973 | 1.973, 1.936 | ~-0.05 ms, -2.4% | **No** — the three before arms span 0.051 ms on their own |
| 1024 | 7.736, 7.719 | 7.552, 7.537 | **-0.183 ms, -2.4%** | Yes — 10x the within-arm spread |

`scripts/baseline.py --config release --zones --runs 3 --samples 1`, on
`--scene res:/character.gltf`, 717 traced frames an arm (the trace window caps at ~239
frames a run, so **more runs help and `--frames` does not**). Every arm was run twice, and
the 256 pair was run **A/B/A** — the third `before` arm came back at 1.973, identical to the
first `after` arm, which is what turns the 256 row from a 2.4% win into a null. Reporting
that row as a win would have been reading drift. The 1024 arms are the ones that resolve:
before `{7.736, 7.719}`, after `{7.552, 7.537}`, four separate three-run measurements with
no overlap.

**It buys no frame time at any count, and was never going to.** `wall frame` at 1024 is
45.43/45.50 before against 45.32/45.54 after, `Frame` 44.94/44.96 against 44.80/45.08, and
`Lighting` 1.154/1.159 against 1.156/1.157 — nulls, correctly, because the scene is GPU-bound
by 3.7x. This is CPU headroom against the threading question in
[tooling.md](../../architecture/tooling.md), which is exactly what the card claimed it was.

To reach 0.5 ms of saving takes ~2,800 characters; the largest scene anything here runs is
1,024. **So: real, reproducible, and negligible below about a thousand characters.**

### Aliasing and determinism

The card's hazard is the right one — this is the third defect shape of the session, two
arrays that get out of step when one grows — and the design answer is that **there is only
one array and it has no length of its own.** `assign(c.pose.nodes.size(), false)` sets the
buffer's length *and* writes every element from the character being resolved, in the
statement before the first read, and the loop that reads it is bounded by that same
`nodes.size()`. There is nothing laid out beside it to disagree with. `assign` rather than
`clear()` + `resize()` is load-bearing: `resize` on an already-sized vector is a no-op and
would leave every mark set.

That is proved rather than asserted, by
`SceneAnimator.ResolveDoesNotDependOnTheCharactersResolvedBeforeIt`: one character playing a
clip must reach the same world transforms whether it resolves first or fourth, with the three
ahead of it at different clip speeds so their poses genuinely differ, over a chain declared
child-before-parent so the marks are read across several passes of the loop. **The first
version of that test passed against a deliberately broken `resize`**, because both of its
arms put the observed character behind at least one other and both were therefore equally
corrupt; it was rewritten so the `alone` arm is character 0, and it then failed on every node
as it should. A determinism test that cannot fail is worse than none, and the only way to
know is to break the code under it.

### Verification

- `scripts/golden.sh check release` — **11 of 11**, byte-identical, `skin` included.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — **806 tests,
  86 suites, green in each**. 805 before this card; the new one is the 806th. TSan matters
  here for the reason the card gives: a scratch buffer shared across characters is what it
  exists to check, and `Animation.cpp` is hosted.
- `scripts/locomotion.sh release` — **3 of 3 arms**. The `walk-run-jump` arm reads
  `idle > walk > run > walk > idle > jump > fall > land > idle`, 8 changes, 8.40 m travelled,
  peak rise 0.93 m, and the script asserts every one of those against an expectation derived
  from the rig's clip lengths rather than lifted off a passing run. That is the end-to-end
  statement that the pose the state machine drives, and the distance the solver carries the
  character over it, are unchanged.
- Validation layers, debug, `character.gltf` with 8 characters, 240 frames — **zero errors,
  zero VUIDs.**

### Deferred

The repeat-until-stable resolve, exactly as the card scoped it, and it is now the *only*
thing that sentence in [tooling.md](../../architecture/tooling.md) names. It is also the
bigger half by a wide margin: 179 ns of a step that costs ~7.4 µs per character means
**97.6% of the animation cost is still in the resolve and the sampling**, and a cached
topological order is where the next measurement should go. Trigger: unchanged — the same
threading question, which stays closed until `CPU busy` comes within 2x of the GPU frame.
