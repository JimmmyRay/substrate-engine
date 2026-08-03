---
id: chore-the-debug-build-carries-a-tax-nobody-measured
title: The debug build carries a tax nobody measured
arc: chore
size: S
verification: trace, tests-4
---

# chore-the-debug-build-carries-a-tax-nobody-measured — The debug build carries a tax nobody measured

`./run.sh` defaults to debug, so debug is the configuration the engine is actually looked at
in — and nobody has put a number on what it costs. This card measures each contributor
separately and then changes the ones worth changing, leaving `build/debug` a configuration
whose frame time can be reasoned from rather than one that is merely slower by an unknown
factor.

What is known to be in there, from reading `build/debug/build.ninja` and the config:

- **`-O0`.** `CMAKE_CXX_FLAGS_DEBUG` is `-g`, and `CMakeLists.txt` adds `-g3`. There is no
  `-O` flag at all and no `-DNDEBUG`. In ~6800 lines of `Renderer.cpp` built on glm, every
  `vec3` operator is an out-of-line call.
- **Validation layers**, from `render.validation: auto` — plus `VK_EXT_debug_utils`, which
  `VulkanContext` enables whenever validation is on, so every pass also emits labels and
  object names.
- **The profiler and its trace writer**, always on, flushing every 120 frames.
- **~26 GPU timestamp zones** per frame with a query-pool reset and readback, gated by no
  setting whatsoever.

The candidate changes are `-Og` for the debug config, and letting an interactive
`./run.sh demo` skip validation while `scripts/golden.sh` and the suite keep it. Neither is
obviously right — `-Og` moves what a debugger can show, and validation off by default is how
a validation error survives to the golden run — which is why this card measures first and
decides second. It may well conclude that `-Og` alone brings debug back into a usable range
and the validation default should not move at all.

**This is a different question from
[bug-the-debug-frame-spends-seven-milliseconds-recording-commands](../done/bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md),
and conflating the two wasted an afternoon already.** That card is a step change on top of
this tax: both sides of its bracket are the same debug build with the same flags, so nothing
here explains it and nothing here fixes it. This card is about the floor debug sits on; that
card is about a commit that raised the ceiling seven milliseconds above it.

**It lands after that card has its measurement**, not before — its arms are compared against
history in `debug_frames/substrate.log` recorded under the current flags, and changing them
first would make that comparison meaningless.

## Verification

- `scripts/baseline.py --config debug --zones --runs 3` before and after, and per
  contributor: one arm each for `-Og`, for `--validation off`, and for
  `profiler.enabled: false`. The point of the card is the attribution, so a single
  before/after number does not discharge it.
- `./test.sh debug`, `./test.sh release`, `./test.sh asan`, `./test.sh tsan` — each its own
  invocation. `-Og` reorders enough to be worth running the sanitizers against.
- `scripts/golden.sh` — eleven cases, byte-identical. A build-flag change that moves a pixel
  is a finding, not a re-snap.

## Reference update

[architecture/tooling.md](../../architecture/tooling.md) — the table of what each
configuration can and cannot do says debug has "validation on by default"; it should also say
what debug costs relative to release, so the next person reading a debug frame time knows
what they are holding.

## Outcome

**Nothing changed in the tree, and the measurement is the deliverable.** The card's own
framing is what it disproves: `build/debug` is not "merely slower by an unknown factor",
because on the scene the engine is actually looked at in it is not slower at all.

### The premise, re-measured at HEAD before anything was believed

Sponza, 1600x900, 4x, `--locked --audio-null`, 717 frames an arm over three runs,
`scripts/baseline.py --config <cfg> --zones --samples 4 --runs 3 --frames 900`:

| | `debug` | `release` |
|---|---|---|
| GPU `Frame` | 3.286 | 3.277 |
| `Lighting` | 1.851 | 1.846 |
| **wall** | **3.351** | **3.360** |
| `CPU busy` | 0.758 | 0.151 |
| `Renderer::record` (`total/frame`) | 0.570 | 0.100 |
| `Renderer::waitFence` | 2.641 | 3.260 |

**Debug's wall time is Release's**, to 0.009 ms — 0.3%, and in Debug's favour. `Lighting` and
GPU `Frame` are identical between the configurations, which is the check that nothing about
this comparison is a GPU story. The tax is 0.607 ms of `CPU busy`, and `waitFence` is the row
that absorbs it: the CPU is asleep for 79% of a Debug frame and 97% of a Release one.

That is entirely the doing of the two cards this one was told to re-measure against.
[bug-the-debug-frame-spends-seven-milliseconds-recording-commands](bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md)
took Debug's `CPU busy` from 8.024 ms to 0.718 — from well *over* the 3.3 ms GPU frame to
well under it — which is exactly the transition from "Debug is half the frame rate" to "Debug
is the same frame rate". Its 0.718 and the 0.758 here are the same number three days apart.
And
[chore-one-profiler-zone-covers-every-pass-the-cpu-records](chore-one-profiler-zone-covers-every-pass-the-cpu-records.md)
built the instrument: every arm below is a `--zones` CPU block, and without it this card would
have had one number per arm and no way to say what moved inside it.

The `Renderer::record` pair in tooling.md — 0.582/0.770 Debug against 0.099/0.148 Release —
reproduced on a different day against fresh runs to within 0.012 ms on every cell. It is
restated to this card's arms so the document holds one set of numbers rather than two.

### The attribution — a 2x2, which is what the card asked for

The card's own candidate list was `-O0`, the validation layers, the profiler and the GPU
timestamp zones. Each was measured separately rather than by difference from the total.
`CPU busy`, then `Renderer::record` as `total/frame`, same arms throughout:

| `CPU busy` | validation on | validation off |
|---|---|---|
| `-O0` (HEAD) | 0.758 | 0.306 |
| `-Og` | 0.619 | 0.177 |
| `-O3` (Release) | — | 0.151 |

| `Renderer::record` | validation on | validation off |
|---|---|---|
| `-O0` (HEAD) | 0.570 | 0.182 |
| `-Og` | 0.491 | 0.115 |
| `-O3` (Release) | — | 0.100 |

| Contributor | ms a frame | Share of the 0.607 |
|---|---|---|
| Validation layers and `VK_EXT_debug_utils` | 0.452 | **74%** |
| `-O0` against `-Og` | 0.129 | 21% |
| `-Og` against `-O3` | 0.026 | 4% |

The layer cost is 0.452 at `-O0` and 0.442 at `-Og`, so the two are additive rather than
multiplicative and the rows genuinely decompose the total.

**The result that was not expected: with the layers off, Debug records commands at Release
cost.** `Bloom` 0.011 against 0.012, `Cull` 0.023 against 0.024, `GBuffer` 0.017 against
0.016, `Lighting` 0.005 against 0.005, `SSR` 0.005 against 0.005, `Renderer::submit` 0.009
against 0.009. The card expected `-O0` to dominate — *"in ~6800 lines of `Renderer.cpp` built
on glm, every `vec3` operator is an out-of-line call"* — and that reasoning is about the wrong
code. `record` is `vkCmd*` calls into a library this build does not compile, and the glm in it
is a handful of matrix products per frame, not per draw. **The one pass where `-O0` shows is
`Overlay`** — 0.081 at `-O0`, 0.026 at `-Og`, 0.011 in Release — whose text layout is our own
code, and it is very nearly the whole 21%. That is the same pass the sibling card found the
descriptor defect in, for an unrelated reason.

**The profiler is 0.023 ms a frame**, 3% of the Debug CPU frame, and it is charged in Release
too, so it is not a Debug tax at all. It is the one arm `scripts/baseline.py` cannot take —
`profiler.enabled: false` writes no trace — so it was measured by timing two frame counts and
dividing the difference, which cancels startup, shutdown and `run.sh`'s no-op rebuild. Three
pairs an arm at N=1500 and N=13500: Sponza 3.423 on against 3.399 off, `physics.gltf` 0.8796
on against 0.8563 off. Two scenes, the same 0.023, non-overlapping ranges. The method was
validated first against the trace — 3.417-3.430 ms/frame from the clock against a 3.351 ms
`wall` median, the offset being a mean over all frames against a median over a window.

The **~26 GPU timestamp zones** the card lists separately are inside that figure: the
`vkCmdWriteTimestamp` calls are `vkCmd*` calls counted in `record`, and the query-pool reset
and readback are in the same 0.023.

**Four candidates contribute nothing measurable**, and each was checked rather than assumed:

- **Assertion density.** `engine/` contains **no `assert(` at all** — the count is zero. The
  three `#ifdef SUBSTRATE_DEBUG` blocks in the tree are the `kDebugBuild` constant,
  `GltfScene`'s texture free-list self-check and `verifyShaderBindings`, and all three run at
  startup or at pipeline creation. None is in a frame.
- **Sanitizer-adjacent flags.** `build/debug`'s `CMAKE_CXX_FLAGS` is empty; `-fsanitize`
  reaches only `build/asan` and `build/tsan`. There is nothing adjacent to strip.
- **Jolt's debug renderer.** `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` is ON in *both*
  configurations by `CMakeLists.txt:142`'s stated argument, so it is symmetric and cannot be
  a Debug tax by construction. Not disturbed.
- **The `VK_LAYER_PATH hid the system layers` path.** `VulkanContext.cpp:156`, once, at
  instance creation. It is a `setenv` and a warning, not a per-frame branch.

### Where the tax bites, which is a threshold and not a ratio

Sponza is GPU-bound by 4.4x and hides the whole thing. `engine/assets/physics.gltf` — 15
bodies, the lightest scene in the tree — is the one that does not, and it is the honest answer
to "what does Debug cost":

| `physics.gltf`, 4x | `debug` | `release` |
|---|---|---|
| GPU `Frame` | 0.681 | 0.681 |
| wall | 0.814 | 0.745 |
| `CPU busy` | 0.745 | 0.135 |
| `Renderer::record` | 0.449 | 0.090 |
| `Renderer::waitFence` | 0.064 | 0.665 |

Identical GPU frame to three places, and Debug is **9.3% slower** — because `waitFence` has
collapsed from 0.665 ms to 0.064 and the CPU has gone from 89% asleep to 8%. The tax has
eaten the entire slack and begun paying out of frame time.

So the number to reason from is: **Debug costs 0.607 ms of CPU a frame everywhere, and costs
frame time only below a GPU frame of about 0.75 ms.** Every scene in tooling.md's threading
table except `physics.gltf` is above that line. Startup is the part actually felt and it is
small: Debug reaches its first frame 110 ms behind Release, 1914 ms against 1804 through
`run.sh` including its rebuild check, medians of three at `--frames 1`.

### Both candidate changes are refused, on this measurement

The card offered `-Og` and a validation default that moves, and said neither was obviously
right. Neither is right.

- **`-Og` is refused.** It buys 0.129 ms of CPU, which is 21% of a tax that costs no frame
  time on any scene above the threshold and 0.129 of 0.814 on the one below it. Against that
  it moves what a debugger can show — and, because `build/asan` and `build/tsan` are also
  `CMAKE_BUILD_TYPE=Debug`, a `$<$<CONFIG:Debug>:-Og>` in `SUBSTRATE_WARNINGS` silently
  applies to the two configurations whose entire product is a readable stack. The card
  guessed `-Og` alone might "bring debug back into a usable range"; Debug is already in
  Release's range, so there is nothing for it to buy.
- **Validation off by default is refused**, and it is the larger 74%. The reason is the
  sibling card's finding rather than an opinion: a cost the layer charges *per draw* is
  invisible to the golden suite (which turns the HUD off), to the readback suite (which
  passes at either width) and to the baseline table (which is Release). 1,044,480 declared
  descriptors were found because a human noticed the frame rate halve. On-by-default is what
  makes the next validation error survive to a run somebody reads, and 0.452 ms of a CPU
  that is asleep 79% of the frame is not a price worth trading it for.

### Verification

- `scripts/baseline.py --config debug --zones --samples 4 --runs 3 --frames 900` and the
  same at `--config release`, plus one arm each for `--validation off`, for `-Og` (built,
  measured, reverted), for `-Og` **and** `--validation off`, and for `--scene
  engine/assets/physics.gltf` in both configurations — 717 frames an arm, nine arms.
  `profiler.enabled: false` by two-point wall clock, two scenes, three pairs an arm.
- `scripts/golden.sh check release` — **11 of 11**, byte-identical.
- `./test.sh debug`, `release`, `asan`, `tsan`, each its own invocation — **805 tests, 86
  suites, passing in all four**.
- Validation layers, debug, `--validation on --locked --audio-null --msaa 4 --frames 240` —
  **zero errors and zero VUIDs**, the only output the standing `VK_LAYER_PATH hid the system
  layers` warning.

The tree is byte-identical to HEAD apart from the documentation, which is what a card that
changes nothing should leave behind. The `-Og` arm was a two-line `CMakeLists.txt` edit,
built, measured and reverted before the golden suite ran; `substrate.json` was restored from
a copy after each profiler arm and checked clean.

### Reference

[architecture/tooling.md](../../architecture/tooling.md) gains **What Debug costs, and when
it costs nothing** — the pair, the 2x2, the decomposition, the four non-contributors, the
threshold and the refusal of both changes — and its configuration table's `debug` row now
names the cost instead of only the features. The `Renderer::record` pair is restated to this
card's arms, and the sentence that deferred the question to this card now points at the
answer. [guides/profiling.md](../../guides/profiling.md) gains the whole-profiler figure
beside its per-scope one, and a warning under Benchmarking against reading a Debug CPU
number as a regression.

### Deferred

`scripts/baseline.py` still takes only `debug` and `release`, and there is still no
`--no-profiler` flag, so the profiler arm needed a config edit and an out-of-tree script.
Adding the flag would be one row in `Config.cpp`'s bool table, and nothing needs it twice
yet — a second caller is the trigger. Neither is this card's, and neither is in the tree.
