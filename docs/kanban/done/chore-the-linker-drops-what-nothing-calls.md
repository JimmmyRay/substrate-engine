---
id: chore-the-linker-drops-what-nothing-calls
title: The linker drops what nothing calls
arc: chore
size: S
verification: golden-11, tests-4, validation, readback, trace
---

# chore-the-linker-drops-what-nothing-calls — The linker drops what nothing calls

Build with `-ffunction-sections -fdata-sections` and link with `-Wl,--gc-sections`, so a
function no path reaches does not reach the binary. Measured while
[G10](../done/G10-a-game-links-only-the-subsystems-it-names.md) was deciding whether a game
should select its subsystems, and it is the reason that card declined the mechanism: this
buys thirty-four times what G10's own row would have, and it costs no architecture.

## The measurement

Taken on `Release`, stripped, with the flags added at directory scope in `CMakeLists.txt`
ahead of every `add_subdirectory` so that the dependencies get them too:

| | Stripped `build/release/demo` |
|---|---|
| today | 6,054,400 B |
| with the flags | **4,743,200 B** |
| | **−1,311,200 B, −21.7%** |

It reaches inside the dependencies, which is what the module table G10 refused could only
have removed wholesale:

| | today | with the flags |
|---|---|---|
| Jolt | 1,811,979 B | 1,424,698 B |
| miniaudio | 895,737 B | 620,735 B |
| fastgltf | 331,203 B | 200,589 B |

Relink of `demo` after touching `Entry.cpp`: **4.11 s → 4.20 s**. That is the whole cost
found so far.

## Why it is not what G10 wanted, and is worth having anyway

Different properties, and the difference should not be blurred. G10 wanted *no object file
that every game links may name a module*, so that the archive member stays out of the link
entirely. This drops **functions nothing calls**, which does not remove Jolt from a game with
no physics — `Engine::simulate` still calls `PhysicsWorld::step` — but does remove every part
of Jolt that game never reaches. The two compose rather than compete: if the size trigger on
G10's refusal ever fires, this is what shrinks the prize it would be chasing, and it should be
in place first so that the prize is measured against the right baseline.

## What this must not grow

- `-flto`. `INTERPROCEDURAL_OPTIMIZATION` is explicitly forced `OFF` at
  [`CMakeLists.txt:119`](../../../CMakeLists.txt#L119) and this card does not reopen it.
  Section GC is a link-time *deletion*; LTO is a whole-program recompile with its own build
  time and its own debugging cost, and bundling them would make one measurement answer for
  two decisions.
- A per-configuration split. If the flags are right they are right in all four; a `Release`-only
  variant is a build configuration the golden set does not exercise.

## The thing to check first

**Three of five golden runs on the day of the measurement failed one random case with
`vkCreateDevice failed: VK_ERROR_DEVICE_LOST`, and it happened both with the flags and
without them** — on the unmodified tree at the commit that closed G10, `no-rt` failed and a
re-run was 11 of 11. So it is environmental rather than caused by the flags, and it is
recorded here because the first two occurrences were seen *while* the flags were on and were
briefly, wrongly, attributed to them. Anyone running this card's verification should expect
it and re-run rather than conclude.

The real hazard to test for is different and specific: **section GC can collect a file-static
registrar whose only effect is its constructor.** Nothing in the tree depends on one today,
which is partly why G10's mechanism was declined, but `.init_array` retention is the property
to confirm rather than assume — and it is confirmed by the unit suite and the golden set
passing, not by reading the linker's documentation.

## Verification

- `scripts/golden.sh check release` — eleven cases, byte-identical. Deleting unreachable
  code must move no pixel, and a moved one is a defect rather than a surprise.
- `scripts/readback.sh release` — nine bit-identical, plus the lit silhouette.
- `./test.sh debug`, then `release`, then `asan`, then `tsan` — each its own invocation.
  ASan and TSan matter more than usual here: both instrument at a granularity section GC
  operates on.
- Zero validation errors with layers on.
- A trace, because the flags change code layout and therefore instruction-cache behaviour.
  `Lighting` and `Frame` from `scripts/baseline.py`, which are the two zones that are not
  bimodal run to run.
- `./build_release.sh demo` completes, and the packaged binary is the smaller one.

## Outcome

**Landed, in all four configurations, as thirty-three lines of `CMakeLists.txt` of which
two are the flags.** `add_compile_options(-ffunction-sections -fdata-sections)` and
`add_link_options(-Wl,--gc-sections)` at directory scope above every `add_subdirectory`,
which is where the card said to put them and is the only placement that reaches the
dependencies. Neither thing the card forbade grew: no `-flto`, and no per-configuration
split.

### G10's numbers reproduced, and the two places they moved

Measured the same way — `strip` a copy, `stat -c %s` — on `Release` `demo`:

| | G10 | measured here |
|---|---|---|
| before | 6,054,400 | **6,046,208** |
| after | 4,743,200 | **4,730,912** |
| Δ | −1,311,200, −21.7% | **−1,315,296, −21.8%** |

**The before row disagrees by 8,192 B and G10 is the reason.** That is exactly the two
pages its own out-of-line constructor and destructor moved, reported in its Outcome as
`6,054,400 → 6,046,208`. So the chore card's "today" row was written before the commit
that closed the card that opened it. Nothing is wrong with either number; the card was
simply stale by one commit on the day it was filed, and the honest baseline for this row
is the smaller one.

**The per-dependency figures reproduce almost exactly, once the attribution method is
recovered.** G10 did not say how it attributed a symbol to a dependency, and the obvious
reading — demangled top-level namespace — gives Jolt 1,518,496 B and does not match. What
matches is a **substring match on the demangled name**, which counts a
`std::vector<JPH::Body*>` instantiation against Jolt rather than against `std::`. That is
the more honest answer to *"what does this dependency cost"* anyway, and it lands on
G10's published figures to within the ctor/dtor move:

| symbol bytes | G10 before | measured before | measured after |
|---|---|---|---|
| Jolt | 1,811,979 | 1,819,522 | **1,432,241** |
| miniaudio | 895,737 | 895,737 | **620,735** |
| fastgltf | 331,203 | 331,332 | **200,718** |
| simdjson | 223,574 | 223,574 | **166,524** |
| meshoptimizer | — | 41,689 | **0** |

miniaudio and simdjson land on G10's numbers to the byte, which is what confirms the
method rather than a coincidence of magnitudes.

**meshoptimizer going to zero is the row worth keeping.** The simplifier is reachable only
through `scene::buildLodChains`, and only `tools/bake.cpp` calls that — a game receives LOD
chains already built, in the sidecar. So a runtime that *cannot* reach the simplifier
stopped carrying it, while `substrate-bake` still links every byte of it. That is the
difference between reachability and a guess, and it is checkable with `nm` in D9's style:
`meshopt_simplify` is present in `substrate-bake` and absent from `demo`.

**Link cost agrees; the absolute number does not and should not.** G10 quoted 4.11 s →
4.20 s. Here the same edit — touch `engine/Entry.cpp`, `./build_game.sh demo release` —
is **2.69 s → 2.79 s**, medians of three. The machine is faster than it was; the delta,
+0.11 s against G10's +0.09 s, is the figure that carries over.

### Establishing that nothing reachable was dropped

The card names the hazard precisely — *section GC can collect a file-static registrar
whose only effect is its constructor* — and asks for it to be confirmed rather than read
out of the linker's documentation. Four things, in increasing order of how much they
prove.

**1. `.init_array` is byte-identical.** `readelf -S` on the linked binary, before and
after, in both configurations that carry static construction:

| | `.init_array` | `.fini_array` |
|---|---|---|
| `Release`, before | 0xc8 — 25 entries | 0x08 |
| `Release`, after | **0xc8 — 25 entries** | **0x08** |
| `Debug`, before | 0xc8 — 25 entries | 0x08 |
| `Debug`, after | **0xc8 — 25 entries** | **0x08** |

Not one static initialiser left. `.eh_frame` does shrink — 0x561b0 → 0x3ad18 in `Release`
— which is the unwind data for functions that no longer exist and is the expected result
rather than a loss.

**2. The dropped set was read, not sampled.** `nm -C --defined-only` on both binaries,
sorted and diffed: **2,978 symbols dropped, 0 gained.** Of those, 182 belong to
`engine/` or `game/`, and every one is public API surface this particular game does not
call — `SpriteTable`'s twenty-odd setters (the demo is 3D), `SaveReader`/`SaveWriter`'s
typed accessors, `AudioEngine`'s queries, `SpatialIndex::raycast`, the `Renderer`'s
`destroy*` helpers — plus out-of-line copies of functions every call site inlined
(`ProfileScope`'s constructor appears here for that reason, while every `Profiler::scope`
in the tree still works). `gfx::Renderer::verifyShaderBindings` is in the list and belongs
there: it is `SUBSTRATE_DEBUG`-only and this is `Release`.

**The one entry that looked alarming and was not** is
`core::Profiler::writeToFile` and `toJson`. Losing those would silently break `--trace`
and therefore every number `scripts/baseline.py` produces. They are dead: `Profiler`
streams to its output file from the writer thread and closes it in `shutdown()`, and
`grep` finds no caller of either anywhere in `engine/`, `game/` or `tools/`. The proof is
not the grep, though — it is that `baseline.py` read **717 traced frames over three runs**
out of the flagged build.

**3. The volk pointers all survive**, which is the case the card's "reached in a way the
linker cannot see" warning is really about. Every `vk*` entry point is a global variable
that volk's own loader assigns, and the loader is live, so the relocation keeps them. Zero
dropped.

**4. The one case where GC removed a data table, and why it is right.** GLFW's
`_glfw_wl_display_interface` and `_glfw_wl_shell_interface` went, and so did the
`wl_display_requests`, `wl_display_events` and `wl_shell_requests` message tables they
point at — all five together, which is the internally consistent answer: GC follows
relocations, so a live interface descriptor would have kept its own message tables alive.
`wl_registry_interface` and the other 61 Wayland symbols stay, because GLFW marshals
through them. `wl_display` is handled inside libwayland-client and `wl_shell` is the
deprecated shell GLFW does not use.

### The debug decision, and why it went the other way

The obvious objection is that `Debug` wants a readable stack. It does not survive being
checked. `-ffunction-sections` changes *where* a function is emitted, not what DWARF says
about it, and `--gc-sections` deletes only what nothing reaches — so every backtrace a
`Debug` build could produce is a backtrace through live code, which was never a candidate
for collection. `addr2line` on the flagged `Debug` binary resolves arbitrary text
addresses to function, file and line, in `engine/` and in Jolt, GLFW and miniaudio alike.
`.debug_info` and `.debug_line` are intact and 50,205 subprograms still carry a
`DW_AT_low_pc`.

So the card's *"if the flags are right they are right in all four"* holds, and the reason
to obey it is the stronger half of its own argument: **a configuration built differently
from the one the golden set runs is a worse thing to have than a slightly larger `Debug`
binary.** `Debug` is 12,826,984 → 11,105,992 B, −13.4%, and gets it for nothing.

ASan and TSan were verified rather than assumed, because both instrument at a granularity
section GC operates on. Both pass. Neither needed the flags scoped out, and the ninja
files confirm all four configurations actually carry them — 370 compile rules and 3 link
rules each, rather than a cache that silently kept an old flag set.

### What the packaging step actually says

`./build_release.sh demo` completes, and it is where the finding is uncomfortable:

| | before | after | Δ |
|---|---|---|---|
| packaged binary, stripped | 6,033,920 | **4,714,528** | −1,319,392, **−21.9%** |
| the AppImage | 185,402,560 | **184,509,632** | −892,928, **−0.48%** |

**Twenty-two percent of the binary is half a percent of the package**, because the package
is 253 MB of asset tree and the executable is 2% of it. That is G10's own closing lesson —
*a proportion of a binary is not a proportion of a game* — arriving a second time, now
against the thing G10 held up as the better answer. It does not change the verdict: the
win is real and it is free. It does mean this is not a shipping-size strategy, and the
number to quote to anyone who asks what it bought a *player* is 0.48%.

### Verification

Each its own invocation, on the tree as committed.

- `scripts/golden.sh check release` — **11 of 11**, byte-identical. Run twice, once
  mid-work and once on the final build; clean both times.
- `scripts/readback.sh release` — **9 of 9 bit-identical, plus the lit silhouette**, and
  the 12-swapchain resize soak clean. Also run twice.
- `./test.sh debug`, `release`, `asan`, `tsan` — **869 of 869** each. (G10 records 859;
  the suite has grown since.)
- `./run.sh demo release -- --headless --locked --audio-null --frames 120 --validation on`
  — **zero errors, zero VUIDs, zero criticals**; the one known `VK_LAYER_PATH` warning.
- `scripts/baseline.py --samples 4 --runs 3`, both arms, 717 frames each:

  | | Lighting | GPU frame | wall | CPU busy |
  |---|---|---|---|---|
  | before | 1.839 | 3.172 | 3.242 | 0.151 |
  | after | **1.843** | **3.185** | **3.259** | **0.138** |

  A link-time deletion should not move frame time and does not: +0.004 ms on `Lighting`
  and +0.017 ms on `wall`, both inside the variance those two zones are quoted for.
  `SSAO` and `SSR` moved further in both directions, which is the bimodality
  [tooling.md](../../architecture/tooling.md#benchmarking--read-the-trace-never-the-log-line)
  already says not to read a before/after out of.
- `./build_release.sh demo` completes; sizes above.
- The link-level claims, in D9's style: `nm -C --defined-only` finds `meshopt_simplify` and
  `scene::buildLodChains` in `substrate-bake` and neither in `demo`, and finds no
  `scene::writeSceneCache` in `demo` — D9's own invariant, unmoved by section GC.

**The `VK_ERROR_DEVICE_LOST` the card warns about did not occur once**, across two golden
runs, two readback runs and eight further engine launches. That is not evidence it is
gone; it is a note that the warning cost nothing to carry and the instruction it gives —
read the log, re-run rather than conclude — was never needed today.

### What the estimate got right

`S` was correct, and for once the estimate and the work agree: two lines of build file, a
comment block arguing for them, and a day of verification. The card's own framing is what
made that possible — it arrived with the measurement already taken and the hazard already
named, so this row spent its time confirming rather than discovering. **A card that opens
with its own numbers and its own worst case is a card that can be closed in one pass**,
and that is the thing G10 did that is worth copying.
