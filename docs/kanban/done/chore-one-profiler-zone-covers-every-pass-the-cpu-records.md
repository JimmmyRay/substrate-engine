---
id: chore-one-profiler-zone-covers-every-pass-the-cpu-records
title: One profiler zone covers every pass the CPU records
arc: chore
size: S-M
verification: trace, golden-11, tests-4, validation
---

# chore-one-profiler-zone-covers-every-pass-the-cpu-records — One profiler zone covers every pass the CPU records

`Renderer::record` is a single `core::Profiler::scope` wrapped around twenty-five passes.
This card gives it the same per-pass CPU zones the GPU side has had since the profiler
landed, so the tree afterwards holds `record/GBuffer`, `record/Lighting`, `record/Ssr` and
the rest beside the `GpuScope` names they mirror.

The argument is a measurement that already happened.
[bug-the-debug-frame-spends-seven-milliseconds-recording-commands](../done/bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md)
found 7.5 ms of CPU inside `record` and could say nothing at all about *which* pass held it —
the trace has one box with twenty-five GPU children hanging off it and no CPU structure in
between. The regression sat in the tree for a day at 55x its old cost and was only noticed
when the frame rate halved, because there was no zone whose median could move.

**That card has since landed, and it is the argument for this one rather than a dependency
on it.** It found its mechanism — an overlay descriptor array declared at the device's
1,044,480-descriptor ceiling and charged per draw by the validation layer — and it found it
by adding *temporary* per-pass `Profiler::scope` calls to `record`, over three throwaway
rounds, all reverted at the end. `recordOverlay` at 8.48 ms of a 9.80 ms `record` was the
whole answer, and it took one debug run to see once the zones existed. So the shape of this
card is settled by demonstration: the instrumentation works, it is what turned "somewhere in
twenty-five passes" into a name, and the only thing wrong with it was that it was thrown
away afterwards and has to be rebuilt by the next person who needs it.

**There is a tooling half, and this card said there was not.** Its first draft read *"there
is nothing to build on the tooling side: `scripts/baseline.py --zones` already prints every
zone the trace contains, so new scopes appear in the table the run after they are added"* —
and that is false. `read_trace` files an event into the zone table only when `cat == "gpu"`;
every CPU scope in the trace lands in `wall`, in `CPU busy`, or nowhere at all. Adding
twenty-five `record/<Pass>` scopes today would produce a table exactly as empty of them as
the current one, so **both verification steps below rested on a claim about the harness that
was not true of it**. The finding is
[bug-the-jolt-profiler-is-compiled-into-release](../done/bug-the-jolt-profiler-is-compiled-into-release.md)'s:
that card had to read `simulate` out of the raw traces by hand for exactly this reason, and
named the gap in its Deferred section rather than editing a card that was not its to change.

So `scripts/baseline.py` learns to report CPU zones *first*, and the scopes follow — the
order matters, because it is what makes the before arm of the cost measurement possible at
all.

The GPU zone names in `Renderer.cpp` are the list to mirror, and mirroring them exactly is
the point — a CPU zone and a GPU zone that share a name can be read against each other, and
one that invents its own spelling cannot. Steps inside `record` with no GPU zone —
`updateInstances`, the command builders, the instance upload — need names too, or the sum
below cannot account for the parent; those take their own function's name, which is what
tells a reader there is no GPU row to compare against.

Worth being explicit that this is instrumentation, not a fix: it makes the next regression
of this shape attribute itself instead of needing a log archive and a bisect.

## Verification

- `scripts/baseline.py --config debug --zones` — the new zones appear in the table *with
  numbers*, which is the check that could not be made before the tooling half of this card,
  and their total-per-frame sum accounts for `Renderer::record` to within the profiler's own
  overhead.
- The same run at `--config release`, **and a before arm taken with the tooling change but
  not the scopes** — that difference on `Renderer::record` is what a scope costs, and it is
  the only way to measure it. `Profiler::scope` is allocation-free in steady state but is
  not free: two clock reads, a hash, a thread-local set lookup and a guarded push. If the
  cost is a measurable share of the frame, that is the finding and the zone count comes back
  down or the scopes are gated.
- `scripts/golden.sh check release`, `./test.sh` in all four configurations, and a
  validation run. A CPU scope cannot move a pixel and cannot reorder a command, so all three
  are null checks — but `./test.sh tsan` is not: `Profiler.cpp` is a hosted translation unit
  and the unit suite is the only place its threading is exercised at all.

## Reference update

[guides/profiling.md](../../guides/profiling.md) — the CPU scopes section lists what is
instrumented, and the per-pass CPU zones belong in it.

[architecture/tooling.md](../../architecture/tooling.md) — added by the correction above, and
the larger of the two: it is where `scripts/baseline.py` is described, and the description
says GPU zones because until this card that is all the tool could read.

## Outcome

**The premise was false, and correcting it doubled the card.** The first draft said there was
nothing to build on the tooling side. There was: `read_trace` in `scripts/baseline.py` filed
an event into its zone table only when `cat == "gpu"`, so every CPU scope in the trace landed
in `wall`, in `CPU busy`, or nowhere, and twenty-five new `record/<Pass>` scopes would have
produced a table exactly as empty of them as the old one. **Both** of the original
verification steps rested on that sentence. The finding is not this card's — it is
[bug-the-jolt-profiler-is-compiled-into-release](bug-the-jolt-profiler-is-compiled-into-release.md)'s,
which had to read `simulate` out of the raw traces by hand for the same reason and wrote the
falsification into its Deferred section rather than editing a card it did not own. The order
this card ran in follows from it: the tool first, then the scopes — which is also the only
way the cost of a scope could be measured, since the before arm needs a harness that can
already see `Renderer::record`.

### What landed

**`scripts/baseline.py --zones` now prints a CPU block beside the GPU one.** CPU zones are
keyed by the trace's `args.path` with the leading `Frame/` stripped, and **that asymmetry
against the GPU block's keying by name is the whole design**: a CPU scope mirrors its pass's
GPU zone name, so `GBuffer` names one of each, and a single table keyed by name would pool
the two silently. Sorting by path puts a pass directly under the zone that called it. The
block carries a fourth column the GPU block does not — **`total/frame`, the pooled sum over
the pooled frame count** rather than a median, because a zone that records twice in a frame
(the G-buffer's two phases, the two cull dispatches) has a median that understates what the
frame paid for it by half. That column is what makes a level summable against its parent, and
it is the accounting check below.

**Twenty-seven CPU scopes in `Renderer.cpp`, spelling twenty-nine names** — `recordCull` and
`recordGBuffer` each name two, by the same phase ternary their `GpuScope` already uses.
Twenty-three sit on the first line of a `record*` method, three on the CPU-only steps
(`updateInstances`, `buildBlendedCommands`, `buildVelocityCommands`), and one at the call
site of `refitSceneAccelStruct`, which is a free function in another translation unit. Two
rules, written once above `Renderer::record` rather than guessed at twenty-seven call sites:
a pass with a GPU zone takes **that zone's name spelled identically** — `SSR`, not `Ssr` — so
the two rows can be read against each other; a step without one takes its own function's
name, so the name itself says there is no GPU row to compare against.

**The scope goes above the early-outs, not beside the `GpuScope` below them.** That was not
the first instinct and it is the one that makes the sum work: a pass that decides to record
nothing still costs a named zero rather than vanishing, and `Present`, `Forward`, `Skinning`,
`Sprites` and `Particles` all sit at 0.000 under the defaults for exactly that reason. A gap
between `Renderer::record` and the sum of its children is then work the list does not name,
which is a question a reader can ask; without the rule it would be indistinguishable from a
pass that returned early.

### What a scope costs

Two independent estimates, and they bracket each other. Sponza, 4x, `--locked --audio-null`,
717 frames an arm, `total/frame` in ms, twenty of the twenty-seven scopes executing under the
defaults:

| Config | `Renderer::record` before | after | delta | per scope |
|---|---|---|---|---|
| `release` | 0.095 | 0.099 | +0.004 | **~0.20 us** |
| `debug` | 0.553 | 0.582 | +0.029 | **~1.45 us** |

The second estimate needs no before arm and comes out of the same traces: the parent less the
sum of its children is the overhead the parent sees plus whatever is genuinely uninstrumented,
so it is an upper bound. Release, 0.099 against 0.097 summed — **0.002 ms, ~0.10 us a scope**,
97.9% accounted. Debug, 0.582 against 0.561 — **0.021 ms, ~1.05 us a scope**, 96.4%
accounted. So a scope costs **0.1-0.2 us in release and 1.0-1.5 us in debug**, which is what
two `clock_gettime` calls, an FNV fold, a thread-local set lookup and a spinlocked
`push_back` should cost, unoptimised in the second case.

**They stay unconditional, and the numbers are why.** Twenty scopes are 0.004 ms of a 3.39 ms
release frame — **0.12% of the frame**, and 4% of the zone they subdivide, which is itself
2.9% of the frame. `wall` moved 3.405 to 3.393 and `Frame` 3.322 to 3.333 across the arms,
both inside the run-to-run spread the bimodal zones already impose; two independent release
arms after the change agreed on `Renderer::record` to 0.001 ms, which is what makes the
0.004 ms delta worth quoting at all. Debug pays 0.9% of its frame, and debug is the
configuration where the attribution is worth having. Gating them behind a build flag would
buy a tenth of a percent and cost the one thing the card exists for — that the next
regression of this shape names itself in the *first* run rather than after three rounds of
temporary instrumentation.

### The attribution, now that it exists

Debug, Sponza, 4x — the five most expensive CPU passes of the twenty, `total/frame`:

| Pass | debug | release |
|---|---|---|
| `Overlay` | 0.097 | 0.011 |
| `Cull` | 0.091 | 0.024 |
| `Bloom` | 0.069 | 0.012 |
| `GBuffer` | 0.059 | 0.016 |
| `HiZ` | 0.053 | 0.007 |

`Overlay` is still the most expensive pass on the CPU in debug even after the descriptor fix,
at 17% of `record` — which is the sibling card's mechanism reduced from 8.48 ms to 0.097 ms
and still visible, and it is visible *by name* now rather than by hand-instrumenting.

### Verification

- `scripts/baseline.py --config debug --zones` and `--config release --zones`, `--samples 4
  --runs 3`, 717 frames an arm — **twenty `Renderer::record/*` rows with numbers**, which is
  the check the card could not previously make at all. A second run with `--no-rt --taa
  --fog` brings `Shadows`, `PunctualShadows`, `Velocity`, `TAA` and `Fog` in for **twenty-five
  rows**; `Decals`, `AsRefit`, `DebugLines` and `buildVelocityCommands` need scene content
  the default run has none of and are unobserved rather than absent.
- `scripts/golden.sh check release` — **11 of 11**, byte-identical.
- `./test.sh debug`, `release`, `asan`, `tsan` — **805 tests, 86 suites, passing in each**.
  TSan is the one that matters: `Profiler.cpp` is hosted, and twenty-seven new scopes a frame
  is twenty-seven more pushes through the thread-slot spinlock.
- Validation layers, debug, 240 frames, `--locked --audio-null`, twice — once on the defaults
  and once on `--no-rt --taa --fog` so the raster shadow, velocity, TAA and fog passes record
  — **zero errors**, the only output the standing `VK_LAYER_PATH hid the system layers`
  warning.

### Deferred, and one correction made in passing

The `debug` row of tooling.md's `Renderer::record` table read 1.29 ms against a `CPU busy` of
0.718 — a child larger than the parent that contains it, because the two figures were
measured in different trees either side of the descriptor fix. It is re-measured here as a
consistent pair, 0.582 and 0.770, both after this card's own instrumentation.

The default sweep table is **not** regenerated: it is a GPU table and no GPU zone moved. CPU
zones are deliberately kept out of it, because that table is published and adding twenty
columns to it would make it unreadable for a figure `--zones` already gives.

`scripts/baseline.py` still takes only `debug` and `release`, so a CPU regression that only
appears under a sanitizer is outside it. Nothing asks for that yet.
