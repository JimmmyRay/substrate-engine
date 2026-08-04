# Tooling

How the engine is built, measured, verified and inspected. Most of it exists because the
corresponding claim is not reliable without it.

---

## Build and run

**Always through the scripts.** They encode environment fixes that are invisible when
missing and produce results that look correct but are worthless.

```bash
./setup.sh         [--no-assets]                     # submodules, dependencies, assets
./new_game.sh      <name>                            # scaffold game/<name>/
./build.sh         [debug|release|asan|tsan|clean]   # the engine and the unit suite
./build_game.sh    <name> [debug|release|asan|tsan]  # the engine, plus one game
./run.sh           [<name>] [debug|release|asan|tsan] -- [scene.gltf] [options]
./test.sh          [debug|release|asan|tsan] -- [--gtest_filter=...]
scripts/bake.sh    [config] <scene.gltf> ...         # the scene sidecar; see below
./build_release.sh <name>                            # a movable package; see guides/building.md
```

`setup.sh` is a wrapper over three commands the README already listed, and it exists so
the first instruction a new developer reads is one line rather than four. It deliberately
does not build: which configuration, and whether a game comes with it, are the first real
decisions, and a setup script that guessed them would be the last place anyone looked when
the wrong thing got built.

`new_game.sh` copies `scripts/template/game/` into `game/<name>/`, substituting the name —
a `Game` subclass, a one-line `CMakeLists.txt` and a README. The template loads no scene
and draws one generated settings panel, which is the smallest thing that proves the loop.
**Its value is that it must not edit anything under `engine/`**: the day it needs to in
order to do something ordinary, that is a defect report about the public surface rather
than a note in the template.

Each configuration builds into its own subdirectory of `build/`. `build.sh` initialises
submodules, links `compile_commands.json` at the repo root, and runs the ASCII guard.

**`build.sh` produces no runnable binary** — a static library and a test executable. The
engine has to build, test and sanitize with nothing under `game/` in the tree, which is
the check that the module boundary is real. `build_game.sh <name>` writes the name into
the build directory's CMake cache, and `run.sh` and `baseline.py` read it — so none of their
signatures changed when the binary moved from `build/<cfg>/substrate` to `build/<cfg>/<name>`.
Naming *no* game is `game/viewer`, not the cached one: a suite that renders whichever game was
built last is a suite whose baselines depend on the last thing somebody did. `build.sh` clears the value; `test.sh` preserves it, so running the
tests cannot silently un-configure a game.

### What each configuration can and cannot do

| Config | Renderer | Unit suite | Simulation | Notes |
|---|---|---|---|---|
| `debug` | yes | yes | yes | Validation on by default, `verifyShaderBindings` active, [hot reload](#hot-reload-leaves-nothing-behind) on. Costs 0.61 ms of CPU a frame over `release` and **no frame time at all** where the GPU frame is over ~0.75 ms — [what Debug costs](#what-debug-costs-and-when-it-costs-nothing) |
| `release` | yes | yes | yes | What every performance number is quoted from |
| `asan` | yes, **with `--no-ray-query`** | yes | yes | Requesting the acceleration-structure extensions makes the proprietary NVIDIA driver fail `vkCreateDevice` under ASan |
| `tsan` | **no** | yes | yes | The same driver segfaults inside `vkCreateDevice` under TSan. `run.sh tsan` wraps in `setarch -R`; without it TSan aborts with `unexpected memory mapping` before `main()`, which is indistinguishable from a clean run if you only grep for race warnings |

**TSan is for CPU-only paths**, which in practice means `./test.sh tsan` — and, since C27,
`scripts/sim.sh tsan`. The unit suite links only `SUBSTRATE_HOSTED_SOURCES`, so it runs under
every sanitizer, and it is the only place the profiler's and the logger's threading gets
checked.

The **Simulation** column is `substrate-sim`, and it says yes in all four rows for the reason
the unit suite does: it links neither Vulkan nor a window, so no driver is involved and no
sanitizer has a device to break. That is the whole difference between it and `--headless`,
which unmaps the window and still creates a real surface against a real driver — a distinction
worth stating because the flag's name suggests otherwise and CI cannot run one frame of the
engine because of it. See [the simulation without a device](#the-simulation-without-a-device).

`--no-ray-query` exists because losing ray tracing under a sanitizer beats losing the
sanitizer.

**The validation layers want it too, on a scene that ray-traces.** `./run.sh debug --
--validation ... engine/assets/mirror.gltf` does not finish thirty headless frames inside 90
seconds with the ray-query extensions on, and finishes in a couple of seconds with
`--no-ray-query`. The same scene without validation completes either way, so this is the
layers' per-command cost on that path rather than a hang. A validation check on a ray-tracing
scene takes the flag; one on a raster scene does not need it.

### The linker drops what nothing calls

Every configuration compiles with `-ffunction-sections -fdata-sections` and links with
`-Wl,--gc-sections`. One function per section, one variable per section, and a linker told
to delete the ones no path reaches. The flags sit at **directory scope in the top-level
`CMakeLists.txt`, above every `add_subdirectory`**, which is the part that matters: a
linker pulls a whole archive member to resolve one undefined symbol, so without this every
game carries all of Jolt's solvers, all of miniaudio's decoders and all of fastgltf's
extensions whether or not it reaches any of them. Put on `substrate` alone the flags would
collect almost nothing, because the engine's own code is the part a game does reach.

Stripped `Release` `demo`, which is what every size figure here is measured on:

| | Stripped | Δ |
|---|---|---|
| `Release` | 6,046,208 → **4,730,912** | −1,315,296 B, **−21.8%** |
| `Debug` | 12,826,984 → **11,105,992** | −1,720,992 B, −13.4% |
| Packaged (`build_release.sh demo`) | 6,033,920 → **4,714,528** | −1,319,392 B, −21.9% |

Most of it is inside the dependencies, by symbol bytes in the linked `Release` binary:

| | before | after |
|---|---|---|
| Jolt | 1,819,522 | 1,432,241 |
| miniaudio | 895,737 | 620,735 |
| fastgltf | 331,332 | 200,718 |
| simdjson | 223,574 | 166,524 |
| meshoptimizer | 41,689 | **0** |

meshoptimizer going to zero is the clearest illustration of what this collects. The
simplifier is reachable only from `scene::buildLodChains`, which only `tools/bake.cpp`
calls, so a *game* cannot reach it — LOD chains arrive in the sidecar already built.
`substrate-bake` still links all of it, which is the check that the collection is
reachability and not a guess.

**The cost is 0.11 s of link time** — 2.69 s to 2.79 s for the touch-`Entry.cpp`
edit-build cycle, medians of three. It moves no frame time, which is what a link-time
deletion should do: `Lighting` 1.839 → 1.843 ms and `wall` 3.242 → 3.259 ms over three
900-frame runs each arm, both inside the run-to-run variance those two zones are quoted
for.

**All four configurations, deliberately.** `Debug` included, because the objection to it
does not survive being checked: `-ffunction-sections` changes where a function is emitted,
not what DWARF says about it, and `--gc-sections` only removes sections nothing reaches —
so every backtrace a `Debug` build could ever produce is a backtrace through live code.
`addr2line` on the flagged `Debug` binary still resolves an arbitrary text address to
function, file and line, in the engine and in every dependency. The sanitizers are in for
the same reason and were verified rather than assumed: `./test.sh asan` and `./test.sh
tsan` both pass with the flags on. A configuration built differently from the one the
golden set runs is a worse thing to have than a slightly larger `Debug` binary.

**What section GC could get wrong is `.init_array`, and that is checked rather than
argued.** A file-static registrar whose only effect is its constructor is reached by
nothing the linker can see as a call; dropping one is a runtime behaviour change and not a
link error. The default linker script `KEEP`s `.init_array`, and the binary agrees:
**0xc8 — 25 entries — before and after, in `Release` and in `Debug` alike**, with
`.fini_array` likewise unchanged. `.eh_frame` shrinks in proportion to the code that left,
which is unwind data for functions that no longer exist.

The same diff is the evidence that nothing reachable went: **2,978 defined symbols
dropped, none gained**, and the engine's share of them is public API surface this
particular game does not call — the sprite table's setters, the save reader and writer,
the audio queries, `Profiler::toJson` and `writeToFile` (the trace is streamed by the
writer thread, so nothing calls them) — plus out-of-line copies of functions every call
site inlined. The volk entry-point pointers all survive, because volk's own loader assigns
them and the loader is live.

**Not `-flto`.** `INTERPROCEDURAL_OPTIMIZATION` is forced `OFF` beside Jolt for its own
stated reasons. Section GC is a link-time deletion of what is already compiled; LTO is a
whole-program recompile with its own build time and its own debugging cost. Bundling them
would make one measurement answer for two decisions.

**A proportion of a binary is not a proportion of a game**, which is the figure to keep
beside the headline. The same 21.9% off the packaged binary is **0.48%** off the AppImage
— 185,402,560 → 184,509,632 B — because the package is 253 MB of asset tree and the
executable is 2% of it. The win is real, it is free, and it is not a shipping-size
strategy.

### Hot reload leaves nothing behind

`--hot-reload auto|on|off`, `auto` being on in Debug. `pollShaderReload` watches both shader
trees and, on any change, recompiles every source and rebuilds every pipeline — the game's
as well as the engine's, since a game has its own tree and its per-material variants are
rebuilt on the engine's own path. What the loop does is in
[rendering.md](rendering.md#shader-hot-reload); where its output goes belongs here, because
the answer is a property of the build directory.

**It goes nowhere.** `glslangValidator` is given one temp file in the OS temp directory,
`gfx::overrideShaderBinary` reads it back and unlinks it, and `readShaderBinary` answers
from that in-memory table ahead of either shader directory — so every consumer of a module
picks the new bytes up without knowing the table exists, `loadShader` and the Debug
`verifyShaderBindings` reflection alike. `build/<cfg>/shaders/` therefore holds only what
the build put there, and a cold start can only run SPIR-V CMake produced.

It used to rename each recompiled `.spv` into the build's own shader directories, and that
made a reloaded module outlive the **process**: press reload, quit, relaunch, and the engine
booted SPIR-V no build had made and no source in the tree necessarily still matched. Two
writers shared one directory and nothing could tell which files the build owned, so both
things the golden suite is trusted for — that eleven cases are byte-identical, and that the
result is reproducible from a clean tree — quietly depended on nobody having pressed reload
since the last build. Nothing reported the difference, which is the failure shape a cache
has: a cache that is wrong looks exactly like a cache that is fast.

A session-scoped overlay directory searched ahead of the build's was the obvious smaller
fix, and is the wrong one. It keeps the artifact, so it keeps the bug in a smaller box: a
crash leaves the overlay behind and the next launch is back to running shaders no build
produced.

**The `.png.ktx2` cache is the same class of artifact and takes the opposite answer, for a
reason worth stating.** That one is *meant* to outlive the run, so it is kept honest by
being produced outside the runtime and by a loader that warns when a stale cache disagrees
with the slot it is used in ([systems.md](systems.md#textures-and-ktx2)). A recompile loop
has nothing to keep honest once it produces no artifact at all, so there is no version stamp
and no staleness warning here — the file that would have needed one does not exist.

What this does not remove is `file(REMOVE_RECURSE ${GAME_SHADER_OUT_DIR})` at configure
time. That wipes CMake's *own* stale outputs when a game shader source is deleted — nothing
removes an output whose `add_custom_command` stopped being generated — and it is independent
of hot reload, which now writes nothing for it to clean. The engine's tree has no equivalent
wipe, and currently carries two orphaned `.spv` from sources that were deleted; that is the
same CMake-side problem and not this one.

### The scene bake is a tool, not a flag

`scripts/bake.sh [config] <scene.gltf> ...` builds `substrate-bake` and runs it, writing
`<scene>.gltf.scene` beside each document: C15's parsed scene and C17's LOD chains, in the
form `scene::readSceneCache` takes. `build_release.sh` is the one caller, once per scene the
manifest resolves, after `scripts/ktx2.py` and before `manifest.py --require-cache`.

**`substrate-bake` links no Vulkan and opens no window.** It is the second binary built from
`SUBSTRATE_HOSTED_SOURCES` -- the first is the unit suite -- plus the two translation units
the runtime does not get: `scene/SceneParse.cpp`, the CPU half of a glTF load, and
`scene/SceneCacheWrite.cpp`, the writer. The property that lets the suite run under
ThreadSanitizer is the one that lets a baker run on a machine or in a container with no
driver, and it is checked by the linker rather than by review: the day something device-side
is pulled into a hosted translation unit, this target stops linking.

It used to be `--bake-scene`, handled inside `GltfScene::loadCpu`. Two things followed and
D9 exists to end both. **A packaged game shipped the writer**, and the flag that reached it,
so a player who typed it wrote a `.scene` into the install directory -- `SUBSTRATE_PORTABLE`
had nothing to compile out, because unlike hot reload the bake had no gate at all. And
**the baker was the renderer**: `build_release.sh` ran `./run.sh release -- --headless
--locked --frames 3 --bake-scene`, standing up a device, a swapchain and every texture
upload to produce a CPU-side artifact that touches none of them, so the one step of a
package with no use for a GPU was the step that could not run without one.

A static library links by object file, which is why the writer had to move *files* and not
merely be gated. While `writeSceneCache` shared `SceneData.cpp` with `readSceneCache`, every
binary that could read a sidecar carried the code to write one. Now `nm` on a game finds no
`scene::writeSceneCache`, and `ar t libsubstrate.a` has no `SceneCacheWrite.cpp.o`. The wire
format still lives in one place, `scene/SceneCacheFormat.h`, where each `put` sits beside its
`get` -- the two halves of a serializer are the classic place for a field to reach one and
not the other.

**The bake is reproducible, and that is the check.** Two runs over one unchanged document
produce identical bytes, so `cmp` is a usable verdict on a `.scene` exactly as it is on a
`.png.ktx2`. Making that true cost two fixes, both of which the old in-process bake got
wrong: the durations a bake measures are zeroed before `SceneStats` is written -- two runs
of `--bake-scene` on Sponza differed in 34 bytes, all of them inside five `double`s the
reader throws away on a cache hit anyway -- and the padding inside a struct written as a byte
range is zeroed too, which is worth 16 more bytes on a scene with particle emitters and
depended on nothing but what else the process had allocated.

**Not `build.sh`.** A development build parses the document, on purpose: that is what makes a
stale sidecar impossible to hide behind, and C15's invalidation is what catches one --
editing a glTF changes its size or mtime and the stamp in the header stops matching, so the
load falls back to the document, silently and correctly. A cache that does not apply is not
an error, is not logged and is not rebuilt.

### The simulation without a device

`scripts/sim.sh <scene.gltf> [--steps N] [--gravity G] [--quiet]` builds a physics world from
the colliders a document authored, advances it N fixed steps and prints where everything ended
up, plus a checksum a CI run can diff. `substrate-sim` is the binary, and like `substrate-bake`
it links `SUBSTRATE_HOSTED_SOURCES` with **no volk and no glfw** — so it runs in a container,
on a build machine, and under every sanitizer.

**This is what `--headless` is not.** That flag creates the window *unmapped*, not absent, and
still creates a real surface against a real driver — which is why CI runs the unit suite and
not one frame of the engine. `substrate-sim` creates neither, which makes a dedicated server, a
GPU-less regression run and a batch tuning job targets rather than reimplementations.

**The step order is not repeated in the tool.** `scene::Simulation::step` is the same call
`Engine::simulate` makes — `Engine` holds a `Simulation` and delegates to it in one line. That
is the property the row was for: a headless loop with its own copy of the call order is a loop
that will disagree with the drawn one on the frame it matters, and it will disagree quietly.

Two things it does *not* share with the engine, both deliberate and both narrower than they
sound. The **collider walk** is the tool's own dozen lines rather than `Engine::initPhysics`,
which also builds scene nodes, binds audio sources to bodies and pairs rigs — none of which is
reachable from a hosted translation unit today. And there is no `Game`: `Game::init` takes an
`Engine&`, so a game class cannot compile without a device, and the tool drives `Simulation`
directly. Both are recorded in [limitations.md](limitations.md#a-headless-loop-steps-a-world-it-did-not-build).

### Launching the GUI from a script

Wrap it in `timeout -s TERM N`, so a failure part-way through cannot orphan a window.
**SIGTERM specifically** — the profiler installs a handler that flushes the Chrome trace.

### `VK_LAYER_PATH`

Needs no special handling. `VulkanContext::init` detects the case where it hides the
system layers and appends the standard directories itself, so validation works however the
binary is launched.

### The ASCII guard

`scripts/check_ascii.sh` runs every build over `engine/`, `game/` and `tests/`, skipping `assets/` in each. It exists
because a stray non-English word reached a source comment and was only caught by eye.

### The cloth pin check

`scripts/check_pins.py <scene.gltf|scene.glb>` reads an export and refuses one that does
not carry the pins the loader will look for: a `FABRIC_`-named mesh with no `_PIN_WEIGHT`,
an attribute that is not a non-normalized `SCALAR` of `FLOAT`, a count that disagrees with
`POSITION`, weights outside `[0, 1]`, no vertex at or above the pin threshold, or a node
named `FABRIC_` whose *mesh* is not. A mesh carrying the attribute without the prefix is a
warning — dead payload rather than a broken export — and an error under `--strict`. Exit
non-zero on any refusal, one line each naming the file, the mesh and the primitive.

**It checks the export, not the `.blend`, and that is the design.** The failure worth
catching is a Blender vertex group, which does not survive glTF export at all: groups
leave only as `JOINTS_0`/`WEIGHTS_0` and only with an armature, so a bare group on a
curtain is dropped in silence. A validator running inside Blender sees a painted group and
a happy scene; nothing before the export can see the thing that is missing after it. The
same holds for the exporter's Attributes checkbox left off and for an object renamed
without its mesh data-block. Reading the bytes also means this needs no `bpy`, runs where
no Blender is installed, and is covered by `tests/check_pins_test.py` — none of which
would be true of an add-on this repository shipped and could not run.

A missing input file is fatal here rather than skipped. `scripts/fetch_assets.sh` skips a
generator whose source is absent because there the source is optional content; a validator's
argument is a path a person typed, and saying nothing about a name that does not exist is
the failure the check exists to prevent.

The authoring half — what a person does in Blender — is in
[making-a-game.md](../guides/making-a-game.md#cloth-pins-are-a-mesh-attribute-not-extras).

---

## Configuration

`substrate.json` at the repo root is the source of truth **for settings**. Regenerate a
fully populated one with `--write-default-config`, which is generated from the settings
table itself and therefore cannot claim a default the build does not hold. It groups the
file by module in first-appearance order rather than emitting a section whenever the module
changes while walking rows, which is what used to write `"render"` twice: the list
interleaves, rapidjson tolerates a duplicate member, and every other JSON reader rejects the
file.

**Every key is optional** — an absent key keeps its built-in default, so an old config
still loads against a new build. That contract is a stated one and the parser is tested
against it.

**What is *not* in the file is as deliberate as what is.** A scene path, a sun, a mix
graph, gravity, the simulation step, the exposure, the tonemap curve, the two shadow biases
and the four capacity budgets are authored decisions and live in the game's `GameSetup`. The
validation layers, the shader reload loop, the frame-stats overlay, the debug
draws, the two debug windows, the profiler, the recorder, the log and the frame limit are
developer controls and live on `Config` behind named flags. The scene bake was on that list
until D9 and is now on none of them: it is not a setting, not a flag, and not something a
running engine does at all -- `substrate-bake` is a separate program. A config still carrying any of
them gets a message saying where it went — a key that parses and silently does nothing is the
failure the table exists to prevent.
[principles.md §7](principles.md#7-a-setting-is-a-property-of-the-person-running-the-program)
is the rule that decides which of the three homes a value gets, and D14 is the audit that
applied it: **thirty-nine keys left**, and what is left is fifty-seven rows in eight modules.

| Module | Rows | What a player recognises in it |
|---|---|---|
| `window` | 3 | Size and vsync |
| `render` | 36 | MSAA, every quality and artefact toggle, the fog and bloom and SSR knobs, the UI font |
| `input` | 3 | Gamepad deadzone, key repeat. Plus `input.bindings`, which is an aggregate rather than a row |
| `camera` | 4 | Field of view, move speed, orbit sensitivity, zoom step |
| `physics` | 2 | The step budget and the worker count — machine properties, not the world |
| `audio` | 6 | On/off, rate, channels, volume, the stream threshold, the decode budget |
| `ui` | 1 | `ui.scale` |
| `engine` | 2 | Engine-owned live state. No JSON key at all |

**Five modules were emptied and are now a game's to take**, which is D17's namespace rule
meeting D14's audit: `scene`, `profiler`, `record`, `logging` and `benchmark` name no engine
row, so `declare("logging.verbosity", …)` is accepted while every key those modules *used* to
hold is refused by name out of `removedKeys()`. That is why `logging.categories` left the file
with the three `logging` rows beside it rather than staying as an aggregate: a module the
table does not claim while `Config` still parses a key out of it is two owners of one section.

**`render.debugFont` and `render.debugFontHeight` were on the list and stayed**, which is the
one place the audit came out against its own plan. The name says *debug* because the overlay
asked for it first, and one atlas serves the overlay, the panel, the inspector and every
string a game draws — so a typeface and a size are a property of whoever is reading them.
`ui.scale` magnifies the embedded bitmap font in integer steps only, and
[limitations.md](limitations.md#only-integer-ui-scaling) already names a TTF here as the
answer for anyone that does not fit. Removing them would have taken an accessibility escape
hatch out of the file to tidy a module boundary.

### The table

`engine/core/Settings.h` is one X-macro list. Each row generates an id, the key string, a
metadata record and a `constexpr` typed handle, so the two spellings of a setting cannot
drift:

```cpp
settings.get(options::render::msaaSamples);                    // typed door, for code
settings.setFromString("render.msaaSamples", "8", source);     // string door, for JSON,
                                                               // the flags, the console
```

The typed door is a `uint16_t` in a type wrapper, so a misspelling is a build error and
`set(options::render::exposure, true)` does not compile. Both land in one setter, and so
does every refusal: an unknown key, a value that does not parse, one outside the row's
stated range, and an `initOnly` row written after the thing it sized was sized.

**The table is not a fixed array, and the compile-time handles did not pay for that.**
`Settings::declare(key, builtIn, label, min, max)` adds a row at run time — this is what
`Game::declareSettings` calls — and every consumer treats it as ordinary: it loads, saves,
dumps, clamps, takes `--set` and draws in a generated panel. What it does not do is turn the
engine's own rows into string lookups. The storage is two containers and the split is the
whole design:

| | Engine rows | Declared rows |
|---|---|---|
| Metadata | `constexpr Row kRows[]`, generated from `SUBSTRATE_SETTINGS` | a deque the table owns |
| Id | an `Id` enumerator, below `Id::Count` | assigned at declaration, at or above `Id::Count` |
| Handle | `core::options::<module>::<row>`, `constexpr` | the `Setting<T>` `declare` returns |
| Typed | yes | yes — only the *id* stopped being a constant |

So *"a game adds rows, it does not edit the engine's"* is a fact about storage rather than a
policy somebody has to remember, and the `static_assert` tying the row table to the id enum
still holds. `row` and `find` are members for the same reason: a process-global registry
would make *"one instance, owned by `Engine`"* false and would leak one table's rows into
another's save.

**`Id::Count` is a boundary and `Id::None` is the sentinel, and they used to be one value.**
A declared row takes `Count` as its id, so a `find` that answered `Count` for *no such key*
would make the first declared row read as unknown at every `!= Count` test — in every build
that ships a game and no build the unit suite runs. Every getter also stopped indexing the
slot array unchecked: a handle naming no row reads the *first* row, says so once rather than
once a frame, and refuses to be written or bound through.

**What growth may not move is anything the table has already handed out.** `getString` and
`origin` return a `const std::string&` into a slot, `row` returns a `const Row&`, and a
declared row's `key` is a `const char*` into a string the table owns. A `std::vector` moves
all of those on the reallocation the next `declare` causes, so both growable containers are
`std::deque`, which never moves what it already holds. A `bindLive` address is the one thing
growth could never have touched — it points at a field *outside* the table.

**The schema freezes, and it is the twin of the init-only freeze.** One refuses a *write*
after the thing it sized was sized; `freezeRows` refuses a *row* after everything that reads
rows by name has read them. `freezeInitOnly` still implies it, and `Engine::init` calls it
the moment `Game::declareSettings` returns — which is the next section.

### A game declares its own rows

`Game::declareSettings(Settings&)` is the hook, and it runs **before `substrate.json` is
read**. Nothing else is available from it — no `GameSetup`, no engine, no file, no flags —
and the schema freezes the moment it returns.

```cpp
void MyGame::declareSettings(core::settings::Settings& s) {
    difficulty = s.declare("mygame.difficulty", 2, "Difficulty", 1, 3);
}
```

Every consumer treats what it added as ordinary. The row loads from its own section of
`substrate.json`, saves back to it, appears in `--dump-settings` and in
`--write-default-config`, takes `--set mygame.difficulty=3`, draws in
`ui::drawSettings(ui, settings, "mygame")` and clamps to its declared bounds. None of those
consumers learns that a game exists, and none of them needed a change to serve one.

**The ordering is the whole design, and the split between the two hooks is enforced rather
than documented.** `loadJson` walks the *file* rather than the table, precisely so that a key
nothing claims produces a message — so a row declared after the file was read has already had
its key reported as the user's typo, and keeps its built-in whatever the file said. Declaring
first is what makes the four sources apply to a game's row exactly as they apply to an
engine's. `declareSettings` may therefore only **add** rows and `configure` may only **write**
them, and a `declare` from `configure` is refused with a message naming the right method. It
is also why `--write-default-config` and `--help`, which exit from inside the command-line
parser, still see a game's rows: the schema is complete before either door opens.

**A game owns every module the engine does not name.** `mygame.difficulty` is a game's;
`render.msaaSamples` is refused, and so is every other `render.` key whether or not a row
claims it today. The unit of ownership is the **module** rather than the key, because the key
is the JSON path: a `render.` key a game invented would be indistinguishable from an engine
row in the config file, in the generated panel and in `--write-default-config`, all three of
which group by module — and an engine release adding that key later would take over a value
the user wrote for the game, with nothing anywhere to say so. Module granularity is also the
only rule answerable at the moment of declaration, which *"is this a row today"* is not.
`engine.` needs no special case; it is an engine module like any other.

The refusal ladder, in the order `declare` applies it. Every entry logs an error naming what
would have been legal, and hands back a handle that reads the first row and writes nothing:

| Refused | Because |
|---|---|
| Anything at all after the freeze | The schema is what the file, the dump and the panel were about to read |
| A key that is not `<module>.<name>` | Every consumer splits a key to find its JSON section |
| A module the engine names | The namespace rule above, `engine.` included |
| A key that *used to be* an engine setting | Every `substrate.json` still carrying it would silently start feeding a row it was never written for. `lighting.sun` is the shape that gets past the module test, because the engine no longer names `lighting` at all — and D14 added thirty-nine more of exactly that shape, all five of the modules it emptied included |
| A row asking for `kEngine` | That flag means live state the engine reports through no JSON key. A game's row wearing it is one the string door refuses, the save never writes and the panel draws as a readout |
| A key some row already claims | `find` answers the first, so a second is a shadow |

**A key belonging to a game row nothing declared this run is an orphan, and it is kept.** A
game ships a setting, the user sets it, the game removes it in the next version — or the same
`substrate.json` is opened by a second game. The value is refused and the run continues, which
is the convention every refusal in this arc follows, and the message says *nothing declares
this module* rather than *unknown setting*, because the two call for different actions. What
makes the answer safe is the save: `saveJson` writes what differs from the default plus every
key the file already had, merging into the file it read rather than rewriting it from the
table — so an orphan survives, a setting dropped in one version and restored in the next does
not cost the user their answer, and one game's section is not another game's to delete.

**No macro, and no enum row yet.** The engine's list is an X-macro because a name appeared by
hand in four places; for a declared row all four are generated from the one `declare` call,
leaving the member and the call itself. Two is a coincidence. An enum row is the same
argument: `declare` would take a `Named<E>` list, reusing the name tables in
[Names.h](#names-and-the-parse-that-refuses) rather than a parallel mechanism, and the table
grows the type when a game actually wants `easy|normal|hard`. Both open as cards when there is
something to serve.

**A row's built-in lives on the row.** `saveJson` and `--write-default-config` each need to
answer *does this differ from its default*, and each used to answer it by constructing a
whole throwaway `Settings` — ninety-odd slots and as many strings, built to compare one
value, on the save path. `Row::builtIn` and `settings::defaultString(row)` replace both. It
also buys a check the list could not make: the stored default carries the type the *literal*
had, and a `static_assert` per row compares that against the type the row declares, so
`X(render, x, Float, float, 0, ...)` — a zero landing in the integer field and reading back
correctly for the wrong reason — is now a build error naming the row.

**Four sources can answer one row, and the order is the order `Engine::init` opens the
doors in:** the built-in default, then `substrate.json`, then `Game::configure`, then the
flags. (`Game::declareSettings` comes before all four and is not one of them: it decides
which rows exist, not what any of them holds.) Nothing in the setter enforces it — a higher tier winning is simply a later write,
which is also what lets a panel toggle or a keypress beat a value a flag set at startup.

The one thing an ordering cannot express is a game's *default*, which has to lose to a file
read before it, so the game gets two doors: `settings.setDefault(handle, value)` writes only
where nothing has claimed the row, and `settings.set(handle, value)` writes regardless.
Both record `game`. [principles.md](principles.md#four-sources-answer-one-row-and-the-order-between-them-follows-from-the-rule)
is the rule that decides the order; [making-a-game.md](../guides/making-a-game.md) is which
door to reach for.

**Two key namespaces, and the prefix says which door a value came in through:**

| Prefix | Meaning | Spelling |
|---|---|---|
| *(module)* | Present in `substrate.json`; the key **is** the JSON path | the JSON key's own — camelCase |
| `engine.` | Engine-owned live state; no JSON key at all | snake_case |

The casing difference is deliberate rather than an oversight. The second half of a mirrored
key is *forced* to be the JSON key's own spelling, so rather than respell every JSON key or
spell an `engine.` key in a casing nothing forces, the difference is left to carry the same
distinction the prefix makes. `engine.current_scene_path` is not something to go looking for
in the config file, and cannot collide with a JSON key because no top-level `engine` section
exists or will.

**Where a value lives is where it lives.** `settings::bindRenderer`
(`engine/gfx/SettingsBind.cpp`) points each renderer-backed row at the renderer's own field
rather than copying into it, so there is one storage location per setting. That is why
`--dump-settings` reports the value `F8` last toggled rather than the one the file asked
for, and why there is no *"the config says one thing and the renderer holds another"* class
of bug to have. Refresh stays polling — `Renderer::drawFrame` compares its feature key each
frame, which is stricter than a notification and already existed.

Two rows have no plain field to bind — `msaaSamples`, a call that rebuilds render targets,
and `tonemap`, a name resolved to a specialisation constant. They are **polled** by two
lines at the end of `Engine::endFrame` rather than dispatched to, which is the same
mechanism `featureKey()` already is. They were applied by hand until the generated panel
arrived, and the panel is what made that untenable: a control over a row nothing applies
moves, reports success and changes nothing. `lightBudget` is bound like any other row and
is additionally `initOnly`, because the light storage buffer is sized from it before
`Renderer::init` returns.

**A built-in is spelled once, in the table.** A bound row used to have its default written
twice — in `SUBSTRATE_SETTINGS` and again as the member initialiser on `gfx::Renderer` —
and the two had already come apart: `render.debugOverlay` was `true` in the table and
`false` on the field, which nothing caught because `bindRenderer` runs before the first
frame reads it. `core::defaults::<module>::<row>` is generated from the same list as
`core::options::<module>::<row>`, and every field the renderer binds now initialises from
it, so there is nothing left for a second spelling to drift from. `SettingsTest` compares
the two, one row at a time, generated from the list they are both generated from.

The same argument decided where a *removed* row's default goes. `gfx::kDefaultLightBudget`
used to derive from `core::defaults::render::lightBudget`; the row is gone, so the constant
is the number.

**No budget is authored anywhere any more** (C40, then D21). They were settings rows, then
`GameSetup` fields, then floors on `GameSetup` fields, and now they are the starting sizes
each subsystem holds privately: nothing past one is refused, because the physics world
rebuilds, the particle pool and the light buffer reallocate, the bindless texture array
doubles and the voice count grows to `AudioEngine::kMaxVoices`. A number a game states is a
number that goes stale against its own content, which is what four cards' worth of drift
was.

The constants are `constexpr auto` rather than the row's declared type, which is the one
place this does not mirror `options`: `constexpr std::string` does not exist, so a `String`
row's default comes out as the `const char*` the macro wrote. What that buys is a default
usable in a constant expression, which is how `gfx::kDefaultLightBudget` derived from
`render.lightBudget` until D14 removed that row. It is a literal in `Renderer.h` now, and it
is the *only* spelling.

**A world-unit constant in the engine is derived from the scene, or it is a settings row,
never a literal in a header.** Which of the two a value gets is decided by what the right
answer is a function of, not by whether the value is a length:

| Value | Home | Why |
|---|---|---|
| `particleSortRange`, `particleCollisionThickness` | derived, in `Renderer::setScene` | A sort key quantises the whole visible depth span, so the right answer is a function of scene size and of nothing else |
| `fogBaseHeight` | derived, from the scene's lower bound | Same |
| `Camera::moveSpeed`, `Camera::nearPlane` | derived, in `frameBounds` | Crossing a room should take about as long whatever the room is. The user's opinion is the *ratio*, and that is `camera.moveSpeedScale` — a dimensionless multiplier, not a second spelling of the speed |
| `render.ssaoRadius`, `render.ssaoBias`, `render.ssrThickness` | rows | Contact scale, a depth bias and a wall's thickness are properties of the content, not of the box around it. All three were literals no key, flag or panel could reach |
| `render.lodThreshold` | row | A coverage *fraction*. Dimensionless, so it never wanted a derivation |
| `render.fogDensity` | row | Extinction *per* world unit — it scales inversely rather than with the lengths beside it |

**The test that separates them is whether the value would be recomputed.** A row over a
field that `setScene` or `frameBounds` rewrites is a setting that appears to work and
reverts on the next scene load, which is the same failure as a flag nothing reads. That is
why no derived value above has a key, and why a `Scene` source tier was considered and
declined — with nothing in the engine deriving a *row*, it would have been a fifth
precedence tier with no writer.

**`initOnly` marks a row that is applied once, not only a row that sizes something.** The
UI font is baked when the renderer comes up and the mixer's rate is chosen when the device
opens, and nothing re-reads either; before they said so, setting one from the console did
nothing and said nothing. Marked, the write is refused with a sentence and the generated
panel draws them as readouts. The validation layers and sync validation used to be the
headline examples and are no longer rows at all — D14 found that a flag with no key is the
stronger answer to *"this is decided before you can reach it"* than a row that says so.

### Names, and the parse that refuses

**A value spelled by name has exactly one list of names, and a name that list does not hold
is refused with the legal values printed.** `engine/core/Names.h` is the mechanism: a
`Named<T>` list per enum, living beside the enum it names, with `nameOf` and `parseName`
both derived from it. That makes the round trip total by construction — `nameOf(parseName(n))
== n` for every canonical `n`, `parseName(nameOf(v)) == v` for every `v` — rather than by
two authors agreeing, which is what a `key()` function and a separate alias table were.

| Enum | Names | Where the list lives |
|---|---|---|
| `gfx::TonemapOperator` | `aces`, `reinhard`, `clamp` (`none`) | `gfx/DebugView.cpp` |
| `gfx::DebugView` | `lit`, `albedo`, `normal`, `orm`, `depth`, `emissive`, `ssao`, `edges` | `gfx/DebugView.cpp` |
| `core::LogLevel` | `critical`, `error`, `warn` (`warning`), `status`, `debug` | `core/Logger.cpp` |
| `core::LogOutput` | `terminal`, `file`, `both` | `core/Logger.cpp` |
| `core::LogCategory` | `Core`, `Vulkan`, `GLTF`, … — the same word the log line prints | `core/Logger.cpp` |
| `core::Tristate` | `auto`, `on` (`true`), `off` (`false`) | `core/Config.cpp` |
| `scene::AudioBackend` | `auto`, `device`, `null` | `scene/Audio.cpp` |

**The first entry naming a value is canonical and the rest are input conveniences.** `none`
parses as `clamp` and `warning` as `warn`, and neither survives into the table: a save
writes the canonical spelling, so one value never has two spellings in a file.

**The value is refused; the run is not.** An unrecognised name leaves the setting holding
what it held — the previous value at the command line, the built-in default in a config
file — and logs an *error* naming the flag or the key, the text, every spelling that would
have worked, and what is standing instead:

```
--tonemap: `reinhardt` is not one of aces, reinhard, clamp, none -- keeping `aces`
```

A hard exit over a typo in a hand-edited config file is a different kind of damage from the
one this fixes, and the argument the old parser made for its fallback — *"refusing to start
over a typo in a benchmark sweep costs more than the typo"* — was right about the run and
wrong about the value: a sweep that silently measured ACES because `reinhardt` fell back to
it produces a number nobody can tell is wrong. Both doors behave identically, because
`--tonemap` and `"tonemap":` are the same setting and a rule that differed by door is a rule
nobody would remember.

**A totality guard sits beside each list.** `static_assert(core::namesEveryValue(table))`
makes an enumerator added to the enum and not to the list a compile error. The two masks —
`LogCategory` and `LogOutput` — have no `Count` for it to walk and are guarded by the unit
suite instead, over `AllLogCategories` and over `LogOutput::Both`. Every one of those checks
is a loop over the enum rather than a list of names, because a list of names in a test is
exactly the thing that stops being maintained.

### The generated panel

`ui::drawSettings(ui, settings, "render")` (`engine/ui/SettingsUi.h`) draws one widget per
row of a module — a checkbox per bool, a slider per bounded number, a field per string —
with the name, the range and the label read from the table. The module argument is the JSON
section, so a game draws its own rows with the same call and a different name, and nothing in
the function learns that a game exists. Every write goes through `set`,
so a value arriving from a panel is clamped by the same bounds, recorded with the same
provenance and pushed into the same live field as one arriving from the file.

This is **not** the property registry `ui/Inspector.h` refused, and the distinction is the
one that decides when a table is worth having: an instance's fields are named nowhere, so
inspecting one by table means inventing a schema; a setting's name, type and range are
already declared, for the JSON parser and the dump, so drawing it by hand means writing a
fifth copy of what four consumers already share.

Two kinds of row are drawn as readouts rather than controls — `engine.` rows, which the
setter refuses by design, and `initOnly` rows once `freezeInitOnly()` has run. Which rows
those are is the table's declaration, not the panel's opinion: a row that takes effect only
at startup and does not say so is a defect in the table.

### Saving

`Settings::saveJson(path)` writes the live values back, and is the other half of
`loadJson`. It writes **what differs from the default, plus every key the file already
had** — the same rule `input::saveBindings` follows for a rebound action, and for the same
reason: a config listing all ninety-odd rows is one nobody can read a diff of, and one that
dropped a key somebody typed by hand would be a config that edits itself. A file that fails
to parse is refused rather than replaced, since it still holds settings the parser could not
reach.

Both writers, the save file and the scene cache go through `core::writeFileAtomically`
(`engine/core/FileWrite.h`): the bytes land in `path.tmp` and are renamed over `path`, so a
full disk leaves the old file rather than half a new one.

**There is no list of the settings in this document, deliberately.** `--dump-settings`
prints every one there is, and `settings::removedKeys` in `Settings.cpp` holds every key
that has ever left the file together with the sentence saying where it went. A table here
would be a third copy, and the one that goes stale first.

**One thing in the file is not a row**, because a table of typed scalars cannot hold it:
`input.bindings`, a map of action name to binding list. It stays hand-parsed in `Config`, and
the table knows it by name so it does not report it as unknown. Bindings staying
configuration is also why `input` keeps a real JSON section for `ui::BindingMenu` to write
back to — and `input` keeps three rows of its own, which is what makes the arrangement safe.
`logging.categories` was the second and it left with D14: an aggregate parsed out of a module
no row claims is a section a game may declare into while `Config` is still reading a key from
it, so it is `--log-categories a,b,c` now.

**`--capture` and `--rdoc-capture-path` default from the game, not from the table.** Where
a tool writes is not a preference, so it is not a setting — but it is still something a
project may want to point elsewhere, so `GameSetup` carries the default and the flags
override it.

### `--dump-settings`

Every setting, its type, its value and **where that value came from**:

```
$ ./run.sh demo release -- --dump-settings
render.msaaSamples        uint32  8                   cli      --msaa
render.ssao               bool    true                default
engine.current_scene_path string  res:/Sponza/glTF/Sponza.gltf  game
demo.impactVolume         float   0.25                cli      --set
```

The last line is a row the *game* declared, and it is printed by the same loop as the three
above it. A game's setting that existed everywhere except in the dump would not be a setting.

The last two columns are the reason it exists. Knowing a value is `4` does not tell you
whether your edit took; knowing it is `4` *from `default`* when you set it to `8` in the
file tells you immediately. Four sources, in increasing precedence:

| Source | Means |
|---|---|
| `default` | The built-in, from the table's own row |
| `config` | `substrate.json`, and the origin column names the file |
| `game` | The game's `configure` — a default it stated where the file said nothing, or a value it forced over the file. Either way it loses to the flags |
| `cli` | A flag, and the origin column names which one |

`--dump-settings=json` emits the same thing as one object per setting, with numbers still
numbers. The consumer is a bug report: a diff of two dumps is how "works on my machine"
gets resolved — so **the JSON form takes the terminal to itself** and sends the log to the
file for that run. Warnings and status lines go to stdout, and a document with a log line
in the middle of it does not parse. The table form is for a human and keeps them, which
matters because "`scene.path` moved into game code" is exactly what somebody dumping
settings wants to read.

An `engine.` key is engine-owned live state with no JSON key at all — `current_scene_path`
is the one to know. It is readable and dumpable and cannot be assigned by name, because
loading a scene is not assigning a string.

### Any setting from the command line, and when a named flag is correct

**Every row of the settings table reaches the command line by its JSON key:**

```bash
./run.sh demo release -- --set render.fogHeightFalloff=12 --set window.vsync=true
```

`--set <key>=<value>`, split on the *first* `=` so a value may contain one, and it parses
nothing else: the table's string door already refuses an unknown key, an `engine.` key, a
value of the wrong type and an `initOnly` row after the freeze, each with the message it
already has, and answers a key that has *moved* with the sentence saying where it went. The
value is recorded as `cli` with `--set` in the origin column, so `--dump-settings` traces it
like any other.

The branch used to consult a second table before delegating, because seven rows held a *name*
and had to be refused and canonicalised through the same list `--tonemap` consults. D14 moved
all seven out of the table, so there is no row type left for `--set` to hold an opinion about:
the whole branch is the split on the first `=` and one call. The refusal did not go anywhere —
it moved to the six flags that carry those names, where it happens before the field is
written.

That answers the coverage half — some sixty rows had no flag at all, so *"set
`render.fogHeightFalloff` to 12 and tell me if it still happens"* was a reproduction step
nobody could follow without editing the config file the report depended on. It also
settles when a **named** flag is correct, which the flag list had been growing without:

> A named flag is correct only for a **developer control with no JSON key** — something a
> measurement script pins rather than inherits. A preference gets a row in the table and
> reaches the command line through `--set`.

The one deliberate exception is a switch a **runtime key already spells**: `--no-ssao`
beside F8 is one idea spelled twice for one reason, and the `--no-*` family is the
attribution vocabulary a trace is read with. Those rows have JSON keys and keep their
flags.

Twelve flags that failed the test were retired with D13 — `--vsync`, `--ui-scale`,
`--stream-threshold`, `--bloom-strength`, `--ssr-roughness`, `--taa-blend`,
`--rt-distance`, `--particle-budget`, `--body-budget`, `--physics-threads`,
`--lod-threshold` and `--hot-reload`. Each was a line in three places — the table, the
parser and `--help` — to save typing a key `--dump-settings` already prints.

**Five arrived with D14, and that is the same rule read the other way.** `--hot-reload` is
one of the twelve above coming back, which looks like a reversal and is not: it was retired
because `render.shaderHotReload` was a row and `--set` reached it, and it returns because the
row is gone. A shader recompile loop is a developer control, and *"a named flag is correct
only for a developer control with no JSON key"* makes the flag correct the moment the key
stops existing. The other four are `--log-file`, `--log-output`, `--log-categories` and
`--no-profiler` — the log's three and the arm
[profiling.md](../guides/profiling.md#cpu) measures the profiler's own cost against, which
used to live in the measurer's own `substrate.json`.

**The percent scaling went with them**, and that is a win of its own. `--bloom-strength 5`
meant 0.05 through a `scale` column that existed because *"150% is a scale nobody would
type as 1.5 twice"* — so one setting had two representations, the file's and the flag's.
`--set render.bloomStrength=0.05` is the number the file holds and the dump prints.

### Flags

Command-line flags override the file for the things that vary per invocation. The full
list is in `--help`; the ones worth knowing:

| Flag | Purpose |
|---|---|
| `--set <key>=<value>` | Any settings row, by its JSON key. The door every preference has |
| `--frames N` | Exit after N frames. Required for `--headless` |
| `--headless` | Create the window **unmapped**, not absent — the surface still exists |
| `--capture-frame N` / `--capture <path>` | Screenshot at a stated frame |
| `--capture-target <name>` | Read an intermediate render target back to a PNG; `list` prints the names |
| `--record [seconds]` | Record the session to an mp4, keeping the last N seconds. The number is optional |
| `--record-file <path>` | Where it goes. Default `debug_frames/session.mp4` |
| `--trace <path>` | Chrome Tracing output |
| `--validation on\|off\|auto` | Layers. No JSON key: a developer's checking apparatus |
| `--hot-reload on\|off\|auto` | Recompile changed shaders in place. `auto` is on in Debug |
| `--tonemap <name>` | The curve, for one run. `GameSetup::look.tonemap` is where a game's own lives |
| `--log-file <p>` / `--log-level <n>` / `--log-output <n>` / `--log-categories <a,b>` | The log. None of the four is a key any more |
| `--no-profiler` | No CPU scopes, no GPU queries, no trace. All three, since the fix — the query pool is not created, so `GpuProfiler`'s entry points all early-out. The debug-utils labels stay, so a capture is still readable |
| `--frames N` above, `--record`, `--no-physics`, `--physics-debug`, `--physics-contacts`, `--audio-debug`, `--panel`, `--inspector`, `--overlay` / `--no-overlay` | The rest of what D14 re-homed. `--bake-scene` was on this list until D9 removed it outright -- see [the scene bake](#the-scene-bake-is-a-tool-not-a-flag) |
| `--sync-validation` | Adds synchronization validation. Implies `--validation on` |
| `--resize-every N` | Drive a swapchain recreate every N frames |
| `--virtual-resolution WxH` | Render here and present at the largest integer scale that fits, letterboxed; `native` for the window. Overrides `GameSetup::present.virtualResolution` |
| `--ui-outside-virtual` | Draw the HUD and UI *after* the scale, at the window's resolution |
| `--readback <res:/img>` | Draw it at 1:1 and compare the capture against the same file expanded by the presentation scale. See [the readback test](#the-readback-test) |
| `--readback-sprite` | Run that same check through the **sprite pass** rather than the overlay: one sprite, one world unit per texel, top-left corner on texel (0, 0) |
| `--readback-lit-sprite` | The same corner and the same camera through the **G-buffer**: one lit quad. Its value cannot be bit-exact, so `--readback-background <png>` holds it to a *silhouette* instead, and `--readback-lit-cutoff <f>` is what produces the background — above 1 every fragment discards while the draw stays identical |
| `--sprites N` | Draw N sprites through an orthographic camera, spawned in eight batches so the buffer growth path runs. The trace arm behind `rendering.md`'s sprite table; `--sprite-image` chooses the art |
| `--sprites-move` | Nudge every one of them every frame by a fraction of a texel, so the sprite table's revision moves every frame and the upload pays its whole copy. The second arm of that table: same overdraw, same draw, different revision |
| `--width N` / `--height N` | The window, pinned rather than inherited. A check whose answer depends on the presentation scale has to state the window it derived it from, for the reason `--locked` states the clock |
| `--panel` / `--inspector` | Open the UI at startup |
| `--characters N` | Place N copies of the scene's first skinned mesh |
| `--camera fx,fy,fz,yaw,pitch,dist` | Start where the overlay's cam line says |
| `--camera-spin <deg>` | Yaw per frame, for repeatable motion |
| `--input-script <steps>` | Press actions on stated frames. No JSON key, for the reason `--locked` has none |
| `--audio-null` | Mix with no device — **not** the same as `--no-audio` |
| `--scene <path>` | Load this instead of the game's own. A bare path does the same |
| `--no-lod` | Draw every mesh at LOD 0. The coverage below which LOD 1 is selected is a row — `--set render.lodThreshold=<f>`, default 0.000244 — and is the arm a measurement uses: raising it selects the level a more distant camera would, which in Sponza is the only way to reach one |
| `--dump-settings` | Every setting, its value and where it came from; then exit |
| `--locked` / `--realtime` | The simulation clock. No JSON key: a tool that needs determinism must *pin* it |
| `--debug-view <name>` | Which buffer is on screen. No JSON key, for the same reason |
| `--no-*` | Feature switches, for attribution |

**`--headless` is unmapped, not absent.** `GLFW_PLATFORM_NULL` was tried and is the wrong
one: it routes `glfwCreateWindowSurface` through `vkCreateHeadlessSurfaceEXT`, which this
driver does not implement.

**A flag that takes a value and is given none keeps what the setting already holds**, and
does *not* consume the next token. Both halves were wrong before the flag tables landed, and
both were silent: six of fifteen numeric flags fell back to a literal restating the default,
so one at the end of a line reset a configured value to that literal while `--msaa` in the
same position kept its; and a flag with no value read the *next flag* as zero and swallowed
it, so `--msaa --frames` set MSAA to 1 and dropped the second instruction entirely.
`--set` with no `=` is the same rule stated for the new door: it assigns nothing, logs an
error, and does not swallow what follows.

### Scripted input

`--input-script <frame>:<action>[+|-][@<pad>],...` — for example
`60:Game.Save,90:Camera.Forward+,150:Camera.Forward-`. `+` presses, `-` releases, and a bare
action taps: down and up on the same frame, which is what most actions are actually used as
and the one case a level test cannot see. Commas separate steps so the whole argument needs
no shell quoting; whitespace separates them too, so one pasted out of a log parses.

`@<pad>` states which joystick slot the step drives, defaulting to pad 0. It is what lets a
script exercise two players from one run — a binding a step produces names no pad, so the
pad is a property of the *feed*, and a step naming a pad no player holds drives nothing,
which is the failure a two-player scenario is meant to catch.

It names **actions, not keys**, and goes in through the map's ordinary event feed, so a run
states what the player did rather than what their keyboard did. A rebind therefore moves what
a script drives — which is the property that makes this a test *of* the binding table rather
than a way around it, and the reason it is a flag on the engine rather than a fixture in the
suite.

Addressed by **frame index and never by elapsed time**, for the reason `--camera-spin` is:
frame 60 has to be the same frame 60 on every run. Pair it with `--locked`.

Two ways to waste a run have no symptom at all — a script that presses nothing looks exactly
like a feature that does not work — so both are checked once every action has been declared
and logged as errors: a step naming an action this build does not have, and a script whose
last frame is past `--frames`. A malformed step is refused whole and empties the script
rather than loading the part that parsed, because a run that pressed half a scenario reports
against half a scenario.

**What the golden suite says about this, and what it does not.** Every golden case runs
`--headless --locked` and passes no script, so thirteen byte-identical images say only that the
feed does nothing when it is unused. That is worth having — it is what makes the feed safe to
leave in the engine — and it is not evidence that the feed works. That evidence is in
`tests/InputTests.cpp`: a scripted press driven into an `InputMap` and read back through
`pressed()` by a stand-in for a game's frame loop, on the frame the script named and no
other, with the rebind and text-mode arms that a shortcut implementation would fail.

### The scripted locomotion check

`scripts/locomotion.sh [config]`, and it exists because **an animation state that depends on
what somebody pressed cannot be checked by rendering it.** A golden frame of a character
mid-blend is a picture of one moment; the claim is about an *order* of transitions and the
*steps* they landed on, and neither survives being flattened into pixels. This is the only
check in the tree that can fail a state machine driven from input.

Nine arms of the demo, each `--headless --locked --audio-null`, all of them reading what the
demo reports about itself — a `Locomotion:` line per transition and a `Locomotion path:`
summary, both filled from `SceneAnimator::currentState` and `PhysicsWorld::characterTransform`
and never from the input map:

| Arm | What it presses | What it must produce |
|---|---|---|
| `still` | nothing | `idle`, no transitions, 0.00 m travelled, 0.00 m of rise |
| `modifier` | `Player.Run` held for the whole run | the same answer, exactly |
| `walk-run-jump` | forward, the modifier on, the modifier off, release, jump | `idle > walk > run > walk > idle > jump > fall > land > idle` |
| `jump-buffered` | jump, then jump again in mid-air **inside** the buffer | a second launch, at the landing |
| `jump-eaten` | the identical run with the second press 23 frames earlier | one launch, and only one |
| `camera-north` | forward, under `--camera` at yaw 0 | 4.2 m walked, all of it along the camera's forward |
| `camera-south` | the identical run under a camera pointing the other way | the same answer, against a world heading 180° away |
| `camera-turning` | the same run again with `--camera-spin 1.5` | still along the camera, and a path that curved |
| `platform-ride` | `Scene.RidePlatform`, and nothing else for 600 steps | carried 6 m through two reversals, still `idle`, and 0.27 rad of turn |

**The last three assert a direction, which the five above cannot** (G13). `travelled` is a
horizontal path length summed per step — deliberately, so a character that only fell cannot
pass — and a path length is the one quantity that is identical whichever way the character
walked. A 180° heading error therefore lived in `PlayerActions::moveDirection` from the day it
was written and cleared every check in the tree, because `Camera::frameBounds` picks the one
yaw where it cancels and no arm named a camera. Each of the three reports a signed ratio,
`along`: the run's horizontal displacement projected onto the camera's own forward, over the
distance walked. 1 is a character walking where the camera points and −1 is one walking away.

**`camera-turning` is the arm a fixed basis cannot survive**, and the reason there are three
rather than one. A heading resolved against a constant yaw satisfies `camera-north` exactly —
at yaw 0 the two coincide — which is the original mistake reproduced. Under a spinning camera
it walks a straight line instead of a curve: measured at `along` −0.20 and a net displacement
equal to the whole path length, against 0.94 and a third of it. Both halves are asserted.

**`platform-ride` is the arm where the ground moves**, and the eight above all share the
assumption it breaks: on ground that stays put, a heading read off world displacement is right.
C29's `setCharacterTransform` is what puts a rider on the demo's sliding platform — nothing in
the scene walks onto it — and then nothing is pressed at all. It also needed a *tenth kind of
number*, `turned`: every ratio in the summary divides by `travelled`, and `travelled` is world
displacement, which a rider accumulates exactly as a walker does. `turned` is radians of yaw
the drawn character swung through against the heading it started with, divided by nothing. Off
`characterVelocity` it reads 0.27 — all of it in the few steps the drop takes to be dragged up
to the platform's speed, which is real relative motion and really is a heading. Off world
displacement the same arm reads 3.14.

**The four negative arms are the check, not the padding.** `still` is what forbids a trace that
accumulated a settling twitch, a machine that entered `fall` because a parameter defaulted to
the wrong sense, or a character that arrived somewhere under gravity — and it caught two
defects doing it. `modifier` holds the one input in the scheme that names a *state*, so an
implementation that played `run` because the run key was down lights it up; the key changes the
length of a vector, and a vector with no direction to scale changes nothing.

The main arm's load-bearing transition is `run -> walk`: **the movement key does not change
there.** Only the modifier is released, the controller is asked for 45% of travel instead of
100%, Jolt reports 1.44 m/s instead of 3.20, and the machine crosses its own threshold on the
way down. `fall` and `land` are the same argument twice more — nothing is pressed within fifty
steps of either, and each is asserted against the arithmetic it comes from (a 0.25 s launch
clip, a 2v/g arc, a 2 s recovery clip) rather than against a number somebody observed.

**The jump pair is the same trick applied to a timing window** (C20). A jump buffer cannot be
checked by pressing jump while grounded — that passes against a controller with no buffer in
it, which is what this engine had. So both arms press jump *in mid-air*, several steps before
the character lands: one inside the ten-step window, one twenty-three frames earlier and
outside it, and the two scripts differ in nothing else. Where each press sits relative to the
landing is asserted from the log rather than assumed, so a solver that lands a step
differently fails on the placement instead of quietly ceasing to test the feature.

The coyote window is checked in `tests/PhysicsTests.cpp` instead, and the reason is content:
the window is a claim about walking off a ledge, and the demo's scene has no authored one —
manufacturing it would mean changing a shared asset that eleven golden cases and the readback
suite also read. The hosted arms build a two-metre platform with nothing under it, so a rise
after the ledge can only be a launch, and they run the same positive/negative pair twice over:
the window at zero, and the press moved past it.

**Every distance in the main arm moved when C20 landed, and that was the point.** The
expectations were re-derived from the new model — four ramps, each with a computable area
against the instant step it replaced — before the code was run, which is the same discipline
the original 8.40 m arithmetic had. The 3% band on the total is not the discriminating
assertion: the ramps very nearly cancel, so the band still contains the old number. Two things
carry it. The travel must be below the instant model by at least half the derived deficit; and
`walk -> run` must land 7.2 steps of ramp after frame 150 rather than on the next step, which
is a number a controller without a ramp cannot produce.

**`facing` is the worst of the character's parts, and it has to be.** The three camera arms
also assert that the character *looks* where it went, as a ratio read out of the scene tree
rather than off the angle the game wrote — so a rotation the physics sweep discarded reads as
zero. That was the right instinct and it was still a tautology: it read back the single node
`driveLocomotion` had just written, which cannot disagree with the write. A character is
routinely several meshes — `Engine::bindPhysicsToScene` makes one child per placement and
names every one of them `mesh` — and eight arms reported full agreement while the demo's
rig's joint caps sat locked to a fixed world axis beside a body turning to its heading. The
ratio is now the **minimum** over every part, so one piece left behind drags it down: −0.98
against the +0.96 the fix produces, either side of a bound of 0.85. **Read a number back from
somewhere the code under test does not write, or it is a restatement.**

**`drift` is the one number here that is about the animation rather than the solver**, and it
is asserted on all eight arms, including the ones that press nothing. Every other figure
describes where the solver put the capsule; this one describes how far the *pose* carried the
rig's root inside it, and the two are independent — a clip authored with the feet planted walks
the rig forward whether or not anything asked, so the drawn character travels at the sum of the
two speeds and snaps home whenever the machine blends to a clip that stands still. The demo
names the node to `SceneAnimator::setRootNode`, which holds it, and this is the assertion that
the hold took: under 2 cm, against **3.17 m** measured on `walk-run-jump` with the one call
removed and nothing else changed. Every other number in this file was identical across that
pair — which is why eight arms could not see it, and why it needed a new *kind* of number
rather than a ninth arm.

**Nothing here presses `A` or `D`.** Every arm walks forward, and `across` is absolute-valued,
so a strafe mirrored left-for-right satisfies all eight. It is correct today — `D` at yaw 0
carries the character toward world `-X`, which is what `cross(forward, up)` gives and what
`glm::lookAt`'s right-handed basis makes screen-right — but that was established by hand, and
the suite does not say it.

### The scripted arena check

`scripts/arena.sh [config]`, and it is the same argument made about `game/battle_arena`.
Seven arms, the same five flags, and the same shape of assertion: press keys under `--locked`
and read back a `Arena:` line per transition and an `Arena path:` summary, all of it filled
from `SceneAnimator::currentState`, `PhysicsWorld::characterTransform` and
`Scene::worldTransform`.

**It is a second script and not `locomotion.sh` with a game argument**, which is the question
worth answering because the two files look alike. Every number in either is derived from *that
game's* collider, *that game's* gait thresholds and *that game's* scene geometry — the demo's
character walks a showcase atrium and the arena's fighter walks a floor with a 7×4 grid of
columns on it — so one script would be two unrelated sets of arithmetic behind a mode flag,
which is the bundling the conventions refuse. What the two share is a shape, not a body.

| Arm | What it presses | What it must produce |
|---|---|---|
| `still` | nothing | `idle`, no transitions, 0.00 m travelled |
| `modifier` | `Player.Run` held for the whole run | the same answer, exactly |
| `walk-run-jump` | forward, the modifier on, the modifier off, release, jump | `idle > walk > run > walk > idle > jump > fall > land > idle` |
| `camera-north` | forward, under `--camera` at yaw 0 | 4.24 m walked, all of it along the camera's forward |
| `camera-south` | the identical run under a camera pointing the other way | the same answer, against a world heading 180° away |
| `camera-turning` | the same run again with `--camera-spin 1.5` | still along the camera, and a path that curved |
| `column` | forward and the modifier, **never released**, aimed dead-centre at a column | stopped at the column's face, and out of `run` |

**`column` is the arm neither game had, and it is what found
`bug-a-blocked-character-reports-the-speed-it-asked-for`.** Every arm in `locomotion.sh` walks
across open floor, where the request and the result agree to the last decimal, so nothing in
the tree pressed a character into anything — and `characterVelocity` was Jolt's stored linear
velocity, which is the request. The fighter stopped at the column, correctly, and went on
reporting 3.2 m/s and holding `run` for the three hundred steps it stood there; releasing the
keys was the only thing that ever moved the machine. Both halves are asserted now, and the
second is the one that was broken.

The stop distance is arithmetic rather than observation: a capsule against a cylinder touches
along the line between the two axes, so it is the distance to the column's centre less the two
radii — 9.08 m from `kPlayerSpawn`, against 9.07 measured. The camera yaw that aims at that
column is computed from the same two positions, because an approach a few degrees off centre
slides around the column instead of stopping at it and the distance stops being derivable.
Both come out of the column grid `arena.glb` authors, read out of the document.

---

## Profiling

### CPU

`Profiler::scope` / `scopef`, emitting a Chrome trace. Frame 0 is **pinned** in both trim
loops, so startup and asset load stay in the trace rather than falling out of the rolling
window.

**Frame 0 now holds all of startup rather than the scene load alone**, and both halves of
that took work. The frame used to open inside `Engine::loadScene`, which put `initWindow` and
`initRenderer` before it and `initAudio`, `initPhysics`, `initNavigation` and `initCloth`
after it — and a scope opened with the calling thread's stack empty records at **depth 0 as a
sibling of `Frame`**, with no `Frame/` prefix. Zones like that are in the trace and
attributable to nothing, which is why adding them without moving the boundary would have
achieved nothing. It opens at the top of `init` and closes once `Game::init` has returned;
it lives on the `Engine` rather than the stack because it has to span two calls.

`scripts/baseline.py --startup` is what reads it — and until it existed the tool dropped
frame 0 outright, so *the instrumentation would have been invisible to the thing meant to
report it*. That is the same discovery `--zones` made about CPU scopes, and it has the same
consequence: the tooling half goes first. The mode prints one column and no medians, because
there is one startup per run, plus a **count**, because there is not one of each zone in it.

Startup on this machine, release, Sponza — 683 ms:

| | ms | share |
|---|---|---|
| `VulkanContext::init` | 274 | 40% |
| `Swapchain::create` | 146 | 21% |
| `Engine::initWindow` | 93 | 14% |
| `GltfScene::textures` | 78 | 11% |
| `GltfScene::geometry` | 22 | 3% |
| `createIblResources` | 19 | 3% |
| unnamed at depth 1 | 0.031 | 0.005% |

**Two thirds of startup is the driver**, and it is the same two thirds in debug — which is
the split the mode is for. What differs between configurations is only what we compile:
`createPipelines` 3.3 ms release against 20.4 debug, `GltfScene::geometry` 22 against 122.

`scopef` interns names, capped at 4096 with the contract stated in the header — a name
containing a frame number would otherwise leak steadily.

**The trace holds three event phases**, and anything that reads it has to say which it wants.
`ph:"X"` is a timed zone: `cat` is `cpu` or `gpu`, and `args` carries `frame`, `depth` and
`path`. `ph:"M"` is Chrome metadata — `thread_name` — and carries neither a duration nor a
frame, so a reader that indexes `args["frame"]` unconditionally fails on it. `ph:"C"` is a
counter: `cat` is `counter`, and `args` holds one series keyed by the counter's own name plus
the `frame` it belongs to. `baseline.py` tables `X` and `C` separately and skips `M`.

**A counter answers what a duration cannot**, and `Profiler::counter(name, value)` is the
whole surface. Last value per frame rather than a stack — two writes of one name are a caller
correcting itself — which is what bounds the per-frame storage and keeps recording
allocation-free. Dropped when no frame is open, because "this was true during frame N" has no
meaning without an N.

Its `ts` comes from `writeTrace` and never from the call site. The trace carries no
wall-clock time: frames are concatenated by `cumulativeUs`, so a counter stamped from
`steady_clock` would draw a graph that does not sit above the zones it explains. Verified over
a 239-frame demo trace: 2,629 counter events, none of them after the first zone of its own
frame.

**The VRAM regex is gone with it.** `baseline.py` recovered that figure with

```python
VRAM_LINE = re.compile(r"VRAM \[steady state\]: (\d+\.\d+) MiB")
```

— a regular expression over the engine's stdout standing in for instrumentation, reporting one
steady-state number because one log line was all there was to read. `vramMiB` is a counter now
and the figure comes out of the trace with everything else, per frame. The two were measured
against each other before the regex went: **515.7 MiB from the log line, median 515.7 over 239
frames from the counter.**

**A `thread_name` is emitted per *acquisition*, not per slot.** Slots are recycled — that is
what bounds the registry — so one `tid` is the scene-load worker early in a run and the audio
device later, with nothing else in the trace marking the handover. Naming the slot would
label the second thread's work with the first thread's name, which is a worse failure than
an unlabelled track: it is confidently wrong. So `Profiler::nameThread` records against the
acquisition, `acquireSlot` pushes a `thread <n>` event on every claim so an unnamed thread
cannot inherit, and Perfetto takes the latest for a `tid`. `Profiler::init` re-announces
every live acquisition, because a second `init` in one process is a second *trace* and the
first one's metadata went to the first file.

Six threads name themselves at their spawn sites — the scene-load worker, the texture-decode
fan-out, Jolt's job pool through `SetThreadInitFunction`, the recorder's encoder, the logger's
writer — and one names itself from inside its callback, because miniaudio's device thread is
created by the driver and has no spawn site to name it at. That is why `nameThread` is
idempotent on the pointer: it is called at the mix rate.

Windows is the asymmetry, and it is not a defect this introduces: MinGW does not run
`thread_local` destructors, so a slot is never released and recycling cannot happen there.
Every thread gets a fresh slot and the naming is correct for a different reason. Recorded in
[limitations.md](limitations.md).

### GPU

`GpuProfiler`, 64 zones per frame, warning once on overflow rather than silently dropping.

**Zones sit on the CPU's own clock.** `VK_EXT_calibrated_timestamps` is requested where
the device offers it *and* offers `CLOCK_MONOTONIC` as a host domain — both halves matter,
because the extension against any other domain hands back a time on a clock nothing else
in the process reads. The pair is sampled **per frame and kept per frame slot**, not once
at startup: the two oscillators drift, so a startup calibration is accurate for about a
second and then slowly wrong, which is the worst kind.

**The measurement: GPU zones sit a median 7.3 ms behind their CPU frame**, against 11 us
uncalibrated — about two frames of queue latency, which the old placement asserted was
zero. Both arms were verified against a build with the extension forced off, because a
calibration that silently does nothing and one that works look identical in a trace nobody
diffs.

### The commit gate: a regression is refused, not noticed later

`scripts/perfgate.py`, installed as a `pre-commit` hook by `scripts/install_hooks.sh` (which
`setup.sh` runs, because `.git/hooks` is not cloned and a hook nobody installs is a check nobody
runs). It measures one 300-frame headless locked run and compares two zones against
`perf-budget.json`, which is committed.

**It gates `Lighting` and `Frame` and nothing else.** `GBuffer`, `SSAO`, `SSR` and `Bloom` settle
into one of two whole-run states about 5% apart — see "The bimodal zones" below — so a gate on
them fails on a coin flip, and a gate that fails at random is one everybody learns to pass
`--no-verify` to.

**It compares the per-frame minimum, not the median, and that is what makes it usable.** A
pre-commit hook runs on a machine that is also doing something else — another session's build,
the shader compile that just finished. Load moves a median a long way and a minimum barely at
all, because a loaded run still contains frames that ran unobstructed. Measured twice in
succession the minimum drifts **0.2% on `Frame` and 0.4% on `Lighting`**, which is what lets the
margin be **12%**: far outside the documented bimodality, far inside a regression worth blocking
a commit for.

**It runs only when the commit can move the frame** — staged paths under `engine/gfx/`,
`engine/shaders/` or `engine/scene/`. A doc or script commit pays nothing.

Re-baselining is deliberate in the same way a golden snap is: `--update` rewrites the committed
budget, so the new number arrives in a diff somebody reads. A budget that quietly followed the
tree upward would report green while the frame got slower every week, which is the whole failure
this exists to prevent.

It is verified by its own failure mode, not by a green run: dropping the recorded `Frame` budget
to 2.40 against a measured 2.945 exits **1** with `+22.7%`, and staging a shader edit makes the
hook measure where a docs-only commit exits 0 without building anything.

### Benchmarking — read the trace, never the log line

**This is the most important thing in this document.**

The `GPU @` log line prints `GpuProfiler::lastZoneMs` — the duration of **one frame**,
whichever the last collect happened to land on. Every "median of five runs" in this
project's history before the harness existed was a median of five *arbitrary frames*.

`scripts/baseline.py` reads the Chrome trace instead and emits the table below directly.
`scripts/bench.sh` is a fifteen-line front end for it, so there is one parser rather than
two. A trace window is ~240 frames, so a three-run sweep is 717 samples a row instead of
three.

**Every harness here runs `--headless`, and the reason is the keyboard rather than the
pixels.** A `baseline.py` sweep is twelve runs; twelve windows mapping in turn take input
focus away from whoever is working while it measures, which made it the one script that could
not be left running. It is equivalent because the window is *unmapped, not absent* — the
surface, the swapchain and the present path are the ones a visible run uses, which is the same
property that makes a headless golden capture comparable to an on-screen one. Measured on
Sponza at 4x: `Lighting` 1.870 against 1.843 windowed, `GBuffer` 0.472 against 0.479, `unnamed`
0.009 either way. `-- --windowed` puts the window back for watching a run.

The engine holds the second half of that rule itself: **a run with `--frames N` never asks for
focus.** A frame budget is what every harness passes and nothing interactive does, so it is the
signal to set `GLFW_FOCUSED` and `GLFW_FOCUS_ON_SHOW` false. It does not *bind* a window manager
configured to focus whatever it maps, which is why the harnesses pass `--headless` as well.

### `--zones` reports both sides, and for two years it reported one

`scripts/baseline.py --zones` prints a **GPU block and a CPU block**. Until it did, the tool
filed an event into its zone table only when `cat == "gpu"` — so every CPU scope in the trace
landed in `wall`, in `CPU busy`, or nowhere at all. `CPU busy` could say the CPU had spent
eight milliseconds and nothing in the harness could say on what. That is a benchmark that
sees a regression and cannot attribute it, and it is how a 55x regression in
`Renderer::record` survived a day in the tree; every CPU-side figure in this document
predating the CPU block was read out of a raw trace by hand.

**GPU zones are keyed by name and CPU zones by path, and the asymmetry is load-bearing.** A
CPU scope mirrors the name of the GPU zone of the pass it records, so `GBuffer` names one of
each and a single table keyed by name would pool the two silently. The path the profiler
already writes — `Renderer::record/GBuffer`, with the leading `Frame/` stripped because every
zone in a frame carries it — separates them and carries the nesting a flat name cannot.
Sorting by path puts a pass directly under the zone that called it.

The CPU block carries a column the GPU block does not: **`total/frame`, the pooled sum over
the pooled frame count rather than a median.** A zone recorded twice in a frame — the
G-buffer's two phases, the two cull dispatches — has a median that understates what the frame
paid for it by half. That column is the one that sums, so a level can be added up and checked
against its parent, and a gap between the two is work no zone names.

```
CPU zone                                    median      min      max  total/frame
Renderer::record                             0.572    0.408    0.955        0.582
Renderer::record/Bloom                       0.067    0.049    0.141        0.069
Renderer::record/Cull                        0.087    0.063    0.171        0.091
Renderer::record/Overlay                     0.096    0.070    0.163        0.097
...
Renderer::waitFence                          2.445    1.702    3.819        2.560
```

### What sits between the passes: 0.012 ms, and the half-millisecond that never existed

The GPU block ends with an **`unnamed`** row — per frame, `Frame`'s span less the union of the
GPU zones clipped to it. It reads **0.012 ms median on the demo arm (0.22% of `Frame`) and
0.009 ms on the control arm (0.28%)**, and every one of the eighteen inter-zone holes measures
0.001 ms in every frame. That residual *is* the bottom-of-pipe timestamp writes. **The frame is
fully instrumented; nothing unnamed runs inside it.**

It is a row in the table rather than something a reader computes, because the obvious hand
calculation answers a different question and answers it wrongly. Σ(median of each zone) is not
median(Σ of the zones): `GBuffer` (median 0.475, p90 1.048), `Particles` (0.305 / 0.778),
`AsRefit` (0.129 / 0.799) and `SSR` (0.637 / 0.902) are strongly right-skewed **and skewed
together**, because a heavy frame is heavy in all of them at once. Sum-of-medians therefore
undershoots median-`Frame` by ~10% on the demo arm and ~1.4% on the control arm — a phantom
0.54 ms. It also explains the two results that made the phantom look real: `--no-rt` "removing
0.25 ms of gap" is `AsRefit`, the second-most-skewed zone, leaving the trace; `--no-particles`
removing another chunk is `Particles`; and `SSR` "swinging ±0.2 ms between arms with identical
settings" is the same skew read through a median.

Two consequences worth keeping. **A gap between a level and its parent is a claim about
distributions, not about arithmetic** — check it per frame before believing it. And the union
matters as much as the per-frame part: it is what makes a nested zone (`ParticleSort` inside
`Particles`) count once rather than twice.

What is genuinely outside `Frame` is small and now named: **`InstanceUpload`** — the instance
table copy and the material updates, 0.013 ms demo and 0.011 ms control — is recorded before
the frame scope opens, plus the optional capture and record copies and the final `PRESENT_SRC`
transition at the tail of `drawFrame`. `InstanceUpload` was left outside rather than folded in,
because moving it would change every `Frame` number this document publishes.

`kMaxZonesPerFrame` is not a factor here and the measurement says so: the demo arm records 19
zones against the cap of 64, and the overflow warning appears zero times in a run.

### Every pass records a CPU zone, and what that costs

`Renderer::record` used to be one zone around twenty-five passes, which is a box with GPU
children hanging off it and no CPU structure in between. Every pass it calls now opens a
`Profiler::scope` on its first line, under two rules:

- A pass with a GPU zone takes **that zone's name, spelled identically** — `SSR`, not `Ssr`.
  The pair then reads as `SSR` on the GPU track against `Renderer::record/SSR` on the CPU
  one. A CPU zone that invents its own spelling cannot be read against anything.
- A step with no GPU zone — `updateInstances`, `buildBlendedCommands`, `recordInstanceUpload`
  — takes its own function's name, so the name says there is no GPU row to compare against.

**Above the early-outs, not beside the `GpuScope` below them.** A pass that decides to record
nothing costs a named zero rather than vanishing, which is what makes the children sum to the
parent and a shortfall mean uninstrumented work rather than a pass that returned early.

The instrumentation is not free and the number is small: **0.1-0.2 us a scope in release,
1.0-1.5 us in debug**, measured two ways that bracket each other — the before/after on
`Renderer::record` across the change, and the parent-less-children gap in a single trace,
which needs no before arm and is an upper bound. Twenty scopes execute under the defaults, so
the whole of it is 0.004 ms of a 3.39 ms release frame — **0.12% of the frame**, 4% of the
zone it subdivides. It is unconditional in every configuration: gating it would buy a tenth
of a percent and cost the reason it exists, which is that the next regression of this shape
names itself in the first run rather than after three rounds of temporary instrumentation.

### Wall time is not CPU time

Two frame numbers, and mistaking one for the other cost a whole investigation. **`wall`**
is frame-to-frame time — what FPS means, and what the renderer must report as FPS, because
the span of `drawFrame` alone excludes event polling and camera update and so describes a
frame nobody experiences. But wall time *contains* the frame's three blocks on the GPU:
`waitFence`, `acquire` and `present`. **`CPU busy`** is wall less those three, and it is
the only one of the pair that moves with CPU work.

The failure mode is specific and it looks like a broken counter rather than a wrong label.
On a GPU-bound scene wall time *equals* the GPU frame, because that is what waiting for the
GPU makes it. A HUD that labels wall time `CPU` therefore shows CPU and GPU tracking each
other no matter what either does — and the tighter they track, the more GPU-bound you are,
which is the opposite of what it reads as. The engine now reports `wall`, `cpu` and `gpu`
separately, and `baseline.py` subtracts the same three zones so its column and the HUD are
cross-checkable. If they disagree, one of them is measuring the wrong span.

### Should the frame be threaded? No, and here is the measurement

Standing answer, so the question is not re-opened without data. Release, RTX 3060 Ti, 900
frames, `--locked`, medians over a trace window:

| Scene | wall | GPU frame | CPU busy | GPU : CPU |
|---|---|---|---|---|
| `physics.gltf`, 15 bodies | 0.81 | 0.59 | 0.15 | ~4x |
| `instances.gltf`, 4097 instances -> 65 commands | 1.66 | 1.48 | 0.12 | 12x |
| Sponza, 103 draws | 3.14 | 3.05 | 0.14 | 22x |
| the demo's scene | 4.01 | 3.89 | 0.34 | 11x |
| `stress.gltf`, 40 lights | 4.89 | 4.81 | 0.13 | 36x |
| `character.gltf` x256 | 14.23 | 13.96 | 3.69 | ~4x |
| `character.gltf` x1024, 1x MSAA, no post | 86.9 | 86.5 | 10.3 | ~8x |

Every scene that exists is GPU-bound by 4x or more, so per-frame threading buys no frame
time — and there is no serial CPU span to interlace against either, because the CPU spends
78-90% of every frame asleep in `waitFence`. Two structural facts hold the CPU side flat:
command recording is O(passes) not O(draws), so `Renderer::record` stays ~0.08 ms from 103
objects to 4097; and culling is a GPU compute pass, so no view walks the scene on the CPU.

**Every number in that table is Release, and the `~0.08 ms` is the one that had to say so.**
It is a claim about the optimised, unvalidated build, and for a day nothing in the tree
held Debug to anything at all — during which `Renderer::record` sat at 9.8 ms, 55x its own
stated cost and 94% of the CPU frame, and was noticed only because the frame rate halved.
Debug is the configuration the engine is developed in, so the pair is now stated together:

| Config | `Renderer::record` | CPU busy | wall | Notes |
|---|---|---|---|---|
| `release` | 0.100 | 0.151 | 3.360 | Sponza, 4x, `--locked` |
| `debug` | 0.570 | 0.758 | 3.351 | The same, with layers on and `-O0` |

Both rows are `total/frame` from the CPU block, re-measured as a consistent pair. The
predecessor read 1.29 against a `CPU busy` of 0.718 — a child larger than the parent that
contains it, because the two halves of the row were measured in trees either side of the
descriptor repair below and there was no tool that would have printed them together. The
pair was then measured a second time, on a different day and against three fresh runs an
arm, and reproduced to within 0.012 ms on every cell; those are the numbers above.

The gap between them is the standing Debug tax, and it is
[attributed below](#what-debug-costs-and-when-it-costs-nothing) rather than left as a
ratio. What *is* this table's question is a step change on top of it, and the lesson from
the one that happened is that **a per-draw cost the validation layer charges is invisible
to every check in this document**: the golden suite turns the HUD off, the readback suite
passes either way, and the baseline table is Release. See
[bug-the-debug-frame-spends-seven-milliseconds-recording-commands](../kanban/done/bug-the-debug-frame-spends-seven-milliseconds-recording-commands.md)
for the mechanism — a descriptor array declared to the device's ceiling, charged per draw
at its *declared* width rather than the width anything occupies.

The one cost that scales with content is CPU animation — `simulate` is 2.0 ms at 256
characters and 7.5 ms at 1024, on `character.gltf`, release, 1x, `--locked --audio-null` —
and `SceneAnimator::resolve` still resolves the hierarchy by repeat-until-stable rather
than by a cached topological order. Fix that before reaching for threads. The heap
allocation that used to sit beside it is gone: `resolve` marks placed nodes in a buffer the
animator owns rather than a `vector<bool>` per character per step, which is **0.183 ms of
the 1024-character step, -2.4%**, and 179 ns per character per step. At 256 the same
proportion is 0.05 ms and does not separate from run-to-run spread, and at one character it
is 0.2 µs — **the ratio is scale-invariant, so this buys CPU headroom rather than frame
time, at any count.** Revisit threading when `CPU busy` comes within 2x of the GPU frame;
until it does, this is a measurement looking for a problem.

### What Debug costs, and when it costs nothing

`./run.sh` defaults to Debug, so Debug is the configuration the engine is looked at in, and
what it costs is a number rather than a feeling. Sponza, 1600x900, 4x, `--locked
--audio-null`, 717 frames an arm over three runs, `scripts/baseline.py --zones`:

| | `debug` | `release` |
|---|---|---|
| GPU `Frame` | 3.286 | 3.277 |
| `Lighting` | 1.851 | 1.846 |
| **wall** | **3.351** | **3.360** |
| `CPU busy` | 0.758 | 0.151 |
| `Renderer::waitFence` | 2.641 | 3.260 |

**Debug's frame time is Release's.** The two walls differ by 0.009 ms, which is 0.3% and in
Debug's favour. The tax is entirely inside `CPU busy` — 0.607 ms a frame — and it is paid
out of the slack the CPU already spends asleep, which is why `waitFence` is the row that
moves instead. That is only true after the overlay descriptor repair; before it, Debug's
`CPU busy` was 8.024 ms against the same 3.3 ms GPU frame, and Debug ran at 124 FPS.

**Where the 0.607 ms goes**, from a 2x2 of the two candidate causes against the same arms —
`CPU busy`, then `Renderer::record` as `total/frame`:

| `CPU busy` | validation on | validation off |
|---|---|---|
| `-O0` (what Debug is) | 0.758 | 0.306 |
| `-Og` | 0.619 | 0.177 |
| `-O3` (Release) | — | 0.151 |

| `Renderer::record` | validation on | validation off |
|---|---|---|
| `-O0` (what Debug is) | 0.570 | 0.182 |
| `-Og` | 0.491 | 0.115 |
| `-O3` (Release) | — | 0.100 |

| Contributor | ms a frame | Share |
|---|---|---|
| Validation layers and `VK_EXT_debug_utils` | 0.452 | **74%** |
| `-O0` against `-Og` | 0.129 | 21% |
| `-Og` against `-O3` | 0.026 | 4% |

The layer cost is 0.452 at `-O0` and 0.442 at `-Og`, so the two contributors are additive
rather than multiplicative and the table can be read as a decomposition.

**With the layers off, every pass records at its Release cost.** `Bloom` 0.011 against
0.012, `Cull` 0.023 against 0.024, `GBuffer` 0.017 against 0.016, `Lighting`, `SSR` and
`Renderer::submit` identical to three places. Command recording is `vkCmd*` calls into a
library this build does not compile, so the optimisation level has almost no purchase on
it. **The one exception is `Overlay`** — 0.081 at `-O0`, 0.026 at `-Og`, 0.011 in Release —
because its text layout is our own code, and it is very nearly the whole of the 21%.

**The profiler is 0.023 ms a frame**, which is 3% of the Debug CPU frame and is charged in
Release too. Switching it off leaves no trace to read, so it is the one arm `baseline.py`
cannot take: it is measured instead by running two frame counts and dividing the difference,
which cancels startup, shutdown and `run.sh`'s rebuild. Sponza 3.423 on against 3.399 off,
`physics.gltf` 0.880 against 0.856 — two scenes, the same answer.

Those figures were taken with a control arm that was **not** the arm they claimed: `--no-profiler`
did not reach `GpuProfiler`, so the "off" run still created the query pool and still wrote and
read every timestamp. The flag now skips `GpuProfiler::init`, and the re-measurement across a
3,800-frame difference and five repetitions is **3.5745 ms on against 3.5526 off, 0.022 ms a
frame** — unchanged, because the GPU query path is `vkCmd*` calls and a non-blocking readback
and costs nothing the CPU frame can see. The correction is worth recording anyway: a control
arm that does not do what its name says cannot be *assumed* to be a small error.

**Four candidates contribute nothing measurable**, and are listed so they are not
re-investigated:

- **Assertions.** `engine/` contains no `assert(` at all. The three `#ifdef SUBSTRATE_DEBUG`
  blocks — the `kDebugBuild` constant, `GltfScene`'s texture free-list self-check and
  `verifyShaderBindings` — run at startup or at pipeline creation, never inside a frame.
- **Sanitizer flags.** `build/debug` has an empty `CMAKE_CXX_FLAGS`; `-fsanitize` reaches
  only `build/asan` and `build/tsan`.
- **Jolt's debug renderer.** `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` is ON in both
  configurations, so it is symmetric and cannot be a Debug tax.
- **The `VK_LAYER_PATH hid the system layers` warning.** Once, at instance creation.

**The number to reason from is a threshold, not a ratio.** Debug costs 0.607 ms of CPU a
frame everywhere, and costs *frame time* only where the GPU frame is shorter than Debug's
`CPU busy` of about 0.75 ms. `physics.gltf` is the one scene in the tree below it:

| `physics.gltf`, 4x | `debug` | `release` |
|---|---|---|
| GPU `Frame` | 0.681 | 0.681 |
| wall | 0.814 | 0.745 |
| `CPU busy` | 0.745 | 0.135 |
| `Renderer::waitFence` | 0.064 | 0.665 |

Identical GPU frame, and Debug is 9.3% slower — because `waitFence` has collapsed from
0.665 ms to 0.064 and the CPU has gone from 89% asleep to 8%. Every other scene in the
threading table above is far enough over the threshold that Debug and Release are the same
frame. Startup is the part actually felt: Debug reaches its first frame 110 ms behind
Release, 1914 ms against 1804 through `run.sh` including its no-op rebuild.

**Nothing was changed, and the two candidate changes are refused on this measurement.**
`-Og` buys 21% of a tax that costs no frame time on any scene above the threshold, and it
would silently apply to `build/asan` and `build/tsan` as well — both are
`CMAKE_BUILD_TYPE=Debug` — whose entire product is a readable stack. Validation off by
default is the larger 74%, and it is refused for the reason the descriptor repair above made
concrete: a cost the layer charges per draw is invisible to the golden suite, to the readback
suite and to the Release baseline, so on-by-default is what makes an error survive to a run
somebody reads.

### The current baseline

Sponza, 1600x900, release, every feature on. **A claim about this machine**, which is why
the tool that produces it is committed: a number nobody can re-run is an anecdote with a
border around it. Regenerate with `scripts/baseline.py`.

| MSAA | Cull | Shadows | PunctualShadows | GBuffer | SSAO | Lighting | SSR | Bloom | Tonemap | GPU frame | wall | CPU busy | FPS | VRAM |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1x | 0.022 | 0.000 | 0.000 | 0.271 | 0.147 | 1.061 | 0.392 | 0.087 | 0.038 | 2.072 | 2.158 | 0.129 | 463 | 415.9 |
| 2x | 0.021 | 0.000 | 0.000 | 0.365 | 0.151 | 1.275 | 0.413 | 0.088 | 0.038 | 2.444 | 2.517 | 0.141 | 397 | 453.4 |
| 4x | 0.021 | 0.000 | 0.000 | 0.480 | 0.152 | 1.850 | 0.445 | 0.088 | 0.038 | 3.202 | 3.282 | 0.149 | 305 | 508.1 |
| 8x | 0.022 | 0.000 | 0.000 | 0.979 | 0.125 | 3.027 | 0.591 | 0.088 | 0.038 | 5.089 | 5.164 | 0.169 | 194 | 625.3 |

All figures in ms except FPS and VRAM in MiB. Medians over every traced frame of 3 runs of
900 frames.

`Lighting` is what scales with the sample count, and scales almost linearly, because it is
the pass that runs per sample. Everything else is roughly flat.

**The two zero columns are a reading, not a gap.** Ray query is on where the device offers
it, and `recordShadows` and `recordPunctualShadows` run only when it is not — so on this
machine the raster shadow passes are not recorded at all under the defaults and their cost
is inside `Lighting`, which is most of why `Lighting` is now where the old table's
`Shadows`, `Punctual` and `Lighting` used to sit separately. `--no-rt` is the arm that puts
them back, and the `no-rt` golden case is what keeps that path drawn.

The predecessor of this table predated the `wall` and `CPU busy` columns and Tier 1 both,
and stood at 265 FPS at 4x while the tree it described had moved on. It was replaced by a
re-run rather than half-updated. Nothing about this row of work moved these numbers — it is
a CPU-side Debug repair — so read the table as a re-measurement of today's tree.

### The bimodal zones

`GBuffer` and `Bloom` swing about 5% run to run. The characterisation took three attempts
and it took three passes to characterise. The first two readings are superseded and are
kept because each is the obvious one to reach for:

1. Read as noise on `Lighting`. That is the wrong zone — `Lighting` is the *stable* one.
2. Read as a redistribution, because in a shadows-off run both roughly doubled for
   identical work.
3. Actually **bimodal per run**: a process settles into one of two states about 5% apart
   and holds it for every frame, and whole-frame time moves with the split — 3.67 ms
   against 3.86 — so the slow mode is genuinely slower rather than attributing the same
   work differently. It lands on the bandwidth-bound passes and spares `Lighting`, which
   holds to 1.4% across both.

Two measurement methods were compounding it — taking one frame per run, and before that
timing the wrong pipeline stage. Both are corrected; the guidance below survives them.

**Quote `Lighting` and `Frame`. Distrust `GBuffer` and `Bloom`** unless you have several
runs of each arm. Timestamp calibration was the standing hope for fixing this and did not
deliver it, which was the correct outcome — calibration fixes where a zone sits, not how
long it took.

**Attribution beats quotation.** The feature constants make it possible to toggle a
feature and read the delta, against a 0.05 ms noise floor established by toggling a
feature the shader does not read. That is a far better use of these numbers than quoting
one.

### The overlay

FPS, then `wall`, `cpu` and `gpu` frame time as three separate figures, then draw calls,
primitives, triangles, velocity draws. `F6` or `--overlay`. **0.0010 ms.** A run that
captures a frame turns it off regardless, so golden images stay free of a counter that
changes every frame — which is also why editing the overlay text cannot move a golden.

---

## Frame capture and visual regression

### Capture

`engine/gfx/FrameCapture.{h,cpp}` — three free functions, no class. `F12`, `--capture`, or
`benchmark.captureFrame`.

**The copy is recorded *inside* the frame**, between the last pass and the present
transition, because reading the image back afterwards is a race `vkDeviceWaitIdle` does
not cover.

"Any attachment" is one readback rather than eight: the debug-view mechanism already
routes every G-buffer target through the lighting pass, so the swapchain capture reaches
all of them — as resolved pixels, which is the only thing a PNG can hold.
`--capture-target` reads an intermediate target directly, including a whole mip chain or
cascade array in one request.

### Recording a session

`--record` writes an mp4 of the session with the game's own audio, while it runs.
`engine/core/Recorder.{h,cpp}` owns the encoders; `engine/core/AudioTap.{h,cpp}` is the
ring the audio thread writes into; the readback lives in `Renderer`.

**It records what Substrate presented, not what is on the screen.** The frames come out
of `vkCmdCopyImageToBuffer` on the swapchain image and the samples come out of the mix.
There is no path by which anything else on the display reaches the file — which is a
property of the design rather than a promise about behaviour, and it is why this exists
instead of a wrapper around `ffmpeg -f x11grab`. A screen recorder records the screen.

**Nothing blocks the render thread.** The copy goes into a per-frame-slot staging buffer
and is read at the top of the frame that reuses that slot, by which point the fence the
loop already waits on has made it ready — the same trick the cull counters use, and it
needs no second fence. `--capture` does block, which is correct for one screenshot and
unusable thirty times a second.

**Both clocks are real time, and neither is the game's.** `framesOwedAt` derives the
frame count from elapsed seconds rather than accumulating per-frame deltas, so a game
above the recording rate has most of its frames ignored and one below it has frames
repeated, and neither changes how long the file claims to be. Audio the ring could not
fit is replaced by *exactly that much silence* rather than spliced out; splicing would
shorten the sound against the picture, and that drift grows.

The window is the **end** of the session, not the start: recording runs for as long as
the game does, and `stop()` trims to the last N seconds with `-sseof` and a stream copy.
You know you want a recording of the thing that just happened after it happens.

**`--record` is not the only way in.** `Engine::startRecording(path)` and
`Engine::stopRecording()` are public, so a game can put a recording on a key — the demo has
it on `.` — and take several takes in one run by naming a path per take. They are on
`Engine` rather than left as three calls because starting one spans the renderer's frame
tee, the audio tap and the `Recorder`; a caller that assembled those by hand would get a
silent file and no warning. Everything above still holds either way, and so does the rule
underneath it: the engine acts on its own config and binds no key to this itself.

Two ffmpeg processes with one pipe each, plus a mux at exit. One process would need two
input streams, which means a named FIFO and an opening-order deadlock to get wrong; two
`popen` calls have neither problem and the mux is a `-c copy`.

#### What it costs

Measured over 717 traced frames per arm, release, 4x MSAA, recording at 30 fps while the
game ran at ~250:

| | off | on |
|---|---|---|
| GPU frame (median) | 3.879 ms | 3.873 ms |
| CPU frame (median) | 3.969 ms | 3.957 ms |
| CPU frame p90 | 4.986 ms | 5.508 ms |
| CPU frame p99 | 9.334 ms | 7.352 ms |
| FPS | 251 | 249 |

**The median frame is untouched**, because at 250 fps roughly seven frames in eight are
owed nothing and record no copy at all. The cost lands on the p90 — about half a
millisecond on the frames that do carry a readback, dominated by the 5.8 MB memcpy out of
the staging buffer. The tail does not grow, so this shows up as about 1% of the frame
rate and not as hitching.

#### What it will not do

- **A resize ends the recording.** rawvideo carries no dimensions, so a differently
  shaped frame would not fail — it would silently shear the picture from that point on.
  What was recorded up to the resize is still written.
- **Headless is refused**, with a reason. There is no swapchain to read back, and
  `--frames N --capture` already answers what a headless run drew.
- **Ten-bit swapchains are refused**, also with a reason. They decode into a PNG one
  texel at a time and have no rawvideo equivalent without a conversion pass.

### The golden suite

`comparePng` plus `scripts/golden.sh`: per-channel tolerance, a count of pixels allowed to
exceed it, a difference image, and a non-zero exit code. **Thirteen cases**: `lit`, `albedo`,
`normal`, `depth`, `ssao`, `msaa1`, `no-rt`, `emissive`, the cascade scene, `particles`,
`skin`, `physics`, and `mirror` with `mirror-no-rt`. It runs through `./run.sh` and names no
game, which since C41 means `game/viewer` -- a game that opens the scene it was given and
composes nothing.

**That last part is the whole reason the viewer exists.** `run.sh` used to fall back to
whichever game the build directory's CMake cache held, so the suite rendered the demo's world
if `demo` was built last and the arena if `battle_arena` was; a game builds its world in
`Game::init` now, so "no game named" had to stop meaning "some game". The viewer carries the
lighting the baselines were captured against -- the game's sun, ambient and exposure, plus the
fallback point lights a file that ships none gets -- and nothing else. Change one of those and
thirteen images move.

**A case has to be able to fail for its own reason, and one could not.** `no-ibl` was the
twelfth and was retired rather than re-pointed: the environment term its flag switched off
had been removed from the lighting pass, so the case rendered a byte-for-byte copy of
`lit`, passed on every run, and reported coverage the suite did not have. A case that
cannot distinguish its own flag is worse than no case — the count says otherwise, and the
green line is the same green line a working case produces.

#### The two mirror cases, and why reflections needed both

`mirror.gltf` is committed like the other per-subsystem scenes: a mirror floor at
roughness 0.02, four metallic spheres at 0.05, 0.25, 0.35 and 0.65,
a rough wall at 0.90 and a pylon at 0.75. **The spread is the point.** `render.ssrRoughnessCutoff`
is 0.4 with a fade from 0.2, so the scene puts surfaces at full SSR strength, in the tail of
the fade, and past the early-out, in one frame — a case entirely on one side of that threshold
pins half the pass. **The shadow is cast onto the wall rather than the floor**, because a
metallic floor has no diffuse term for a shadow to darken; the floor reflects the wall, so the
pylon's shadow appears once directly and once in the reflection, which is what
`rayshadow.glsl`'s "both paths, one calculation" is about.

It is two cases because **the arms are not the same picture.** With ray query the floor
reflects the wall and the smooth spheres reflect sky; under `--no-rt` a ray that leaves the
screen finds nothing, so the same floor is black across most of its area and ragged stair-step
fringes appear where the march does find the sphere-floor contacts — exactly the fragile
artefact a resolution change to SSR would move. The existing `no-rt` case renders Sponza, which
has no smooth surface anywhere, so it pins the march barely at all; pinning only the ray-traced
arm would have left the screen-space path as uncovered as it was before.

It established that **the renderer is bit-identical run to run** — `0/1440000, mean
0.0000` — which is what makes every per-pass number attributable rather than noise. That
property is now load-bearing; anything that breaks it breaks the suite's ability to
attribute a difference.

**Every case pins `--locked` as well as naming its scene.** The engine's default clock is
`realtime`, because a locked clock takes one simulation step per *frame* and therefore
runs the whole simulation at the frame rate. Determinism is a property this suite needs
and asks for, not one the engine imposes on everyone who runs it.

Goldens live in gitignored `debug_frames/golden/` on purpose: Sponza is not in the
repository either, and a regression suite nobody can reproduce fails for reasons nobody
can attribute. `scripts/fetch_assets.sh` generates the test scenes.

**Every case names its scene.** Eight of them used to rely on `scene.path` from
`substrate.json`, so changing the default scene would have failed eight baselines for a
reason with nothing to do with the renderer.

**Prove the subsystem changes nothing, then measure it.** Physics established this by
re-baselining ten cases from a worktree of the pre-physics commit and matching byte for
byte — a stronger statement than a re-snap, and the reason a one-off-by-one that would
have moved two of them was found rather than shipped.

Three changes in the engine's history are **not** pixel-neutral and required re-snapping:
BC7 compression (mean delta 1.88/255, because block compression is lossy), the punctual
shadow atlas policy (2.39% of pixels, because the shadow-casting light changed), and the
inverted sun fix.

#### A failed comparison and a failed run are different news

The suite reports three outcomes, not two. `FAIL` means the engine compared an image and
the image had changed. **`HARNESS` means no comparison happened** — the binary was missing,
the run timed out, the scene would not load, the device was lost. Only the first is a
rendering regression, and the exit codes say which: 1 for differing cases, 2 for a harness
failure.

The discriminator is the log rather than the exit code, because every failure upstream of
the comparison exits 1 as well: a genuine comparison always logs a `Compare:` verdict, so a
non-zero exit without one came from before the engine ever looked at a pixel. A harness
failure also **stops the suite** rather than continuing, since whatever prevented one case
from running will prevent the rest for the same reason, and ten more identical failures
read as a total regression.

The cost of not drawing this line was an investigation: a case reported `1 of 11 cases
differ` when nothing had rendered at all. Two things made it expensive. `build.sh` printed
`==> done: <binary>` unconditionally — an assertion nobody had checked, and it is now
derived from the file existing. And the kept-failure directory held a **stale**
`actual.png` from an earlier run, because a case that never draws leaves the previous run's
images at the same paths; each case now clears its own artifacts before running, so absence
is what a run that produced nothing leaves behind.

#### Builds against one build directory are serialised

`build_lock` in [`scripts/common.sh`](../../scripts/common.sh), on `build/<config>/.build.lock`.
Two sessions in one checkout is the normal state here and ninja takes no lock of its own,
so two builds against `build/release` write the same objects and link the same executable
at once. The consequence that is not obvious is what it does to a build that has already
finished: **the linker unlinks its output before writing it**, so while one session links
`demo` the file does not exist for anybody — which is what produced a `still does not exist
after building` one statement after a successful build. `run.sh` holds the lock across the
build *and* the check for the binary, because the check is inside the window, and closes
the descriptor before `exec` so a capture does not hold it for two minutes.

#### A reference is never re-snapped to resolve a failure

**`snap` is not a way to make `check` pass.** A reference image is the record of what the
renderer produced when somebody last established that it was *right*. Re-snapping to clear
a failing case replaces that record with whatever the tree rendered at that moment — so an
unverified rendering change becomes the definition of correct, every later run agrees with
it, and the suite reports green while measuring nothing.

The three re-snaps above are what a legitimate one looks like: the change was understood
first, the new image was decided to be the correct one, and the delta was quoted and
argued for. A failing check is the *question*, and the answer is either a defect in the
change or a stated reason the new image is right. It is never the baseline.

**The script cannot tell the two apart, and no version of it could.** `snap` and `check`
run the same thirteen invocations of the same binary and differ only in where the PNG lands;
a legitimate re-snap is byte-for-byte the same operation as an illegitimate one. A
heuristic that guessed at intent — refusing a snap from a dirty tree, or one that moves
more than some fraction of pixels — would refuse the three cases above as well. So the
defence is procedural rather than mechanical: **a snap is a deliberate, separately
authorized act**, taken by someone who has decided the new image is correct and has
recorded why. Anything else is a broken suite that has stopped announcing itself.

This is stated here, as a property of the suite, rather than only in the per-arc sections
below — those name it for the rows held to byte-identical output (C1, C14, C15, D8, G1,
G2, P1, P3), and phrased only that way it reads as though a row whose card does not name
`golden` were exempt. None is. See
[chore-the-corrupt-golden-set-and-the-re-snap-that-caused-it](../kanban/done/chore-the-corrupt-golden-set-and-the-re-snap-that-caused-it.md)
for the incident that made the distinction load-bearing: a shader change reverted from the
code, whose images outlived it by four commits and blocked three cards that never touched
a shader.

**If a re-snap does happen, the check that it did not bless something unnoticed** is
whichever cases the change provably cannot reach — for a tonemap change, the four
`--debug-view` cases, which never enter the operator. Those must come back byte-identical
across the repair. It is a partial check and worth taking; there is no equivalent for a
lit case, which is why the references are evidence that is expensive to re-establish.

### The demo's scene is Sponza, and the rest of it is code

The default scene is `res:/Sponza/glTF/Sponza.gltf` and everything else the demo shows is
built by `buildDemoWorld`: a mirror, an emissive orb with a light inside it, a ground box,
an ambience bed, a spatial hum on the orb, and a character imported at runtime through
`Engine::addModel` and given a controller the game makes.

It was `showcase.gltf` until C21 and C22 made the import possible — a document
`make_composite_scene.py` grafted onto Sponza at build time, because neither Sponza nor a
Mixamo rig can be committed and the engine could not then append a file that deforms. The
generator still writes `character.gltf` and `reflect.gltf`; only the composite is gone.
That `reflect.gltf` is the demo's, and it is the scene the ambient-asymmetry measurement in
`lighting_body.glsl` was taken on — it is still generated and still re-renderable. The golden
suite's reflection case is a different scene under a different name for that reason: `res:/`
searches the game tree first, so a second `reflect.gltf` under `engine/assets/` would silently
shadow it.

Per-subsystem test files — `particles.gltf`, `physics.gltf`, `audio.gltf`, `skin.gltf`,
`mirror.gltf` —
are the right shape for a **regression** case, because each pins one thing so a failure
names its own cause. It is also why none of them reaches an interaction that needs two
subsystems in the same room. The demo covers exactly those: a light inside its own emissive
mesh, a rig whose meshes hang several levels below the controller driving it. Neither is
reachable from a suite of single-subsystem scenes.

**Composing it in code moved four defects into the light**, all of which the composite had
been answering by accident: the physics world was never built for a scene declaring no
collider, the locomotion state machine was built before the rig it names clips on had
arrived, `characterAt(0)` was only ever the player because the composite's rig was the one
skinned thing in the scene, and the shadow atlas overflowed once the demo's auto-placed light
set came back — a file that declares any light replaces that set, and the composite declared
two fills where the heuristic places three. `scripts/locomotion.sh` caught every one; no
golden image showed any of them.

### Temporal stability is a sequence, and no image can hold one

`scripts/ssr_stability.py` orbits a camera over `mirror.gltf` and reports the mean absolute
difference between consecutive frames, restricted to the reflection band, for a given
`render.ssrScale`. It exists because a golden image structurally cannot answer whether an
artefact crawls: a golden is one frame, and a staircase that sits still is invisible while one
that swims a pixel per frame is the classic half-resolution tell.

Three properties the golden suite already rests on are what let a *sequence* be assembled from
independent runs rather than needing a new capture path: frame N is a function of N under
`--locked`, `--camera-spin` is per frame and not scaled by `dt`, and the renderer is bit-identical
run to run. So the harness is N invocations of `./run.sh` with `--capture-frame`, and it reuses
`--no-ssr`, `--no-bloom`, `--taa`, `--set render.ssrScale` and `golden.sh`'s software-device
refusal rather than adding a flag.

**The band is measured, not located.** It is the set of pixels that change when `--no-ssr` is
passed — the pass's own definition of where it acts — dilated by 4 px, because a quarter-res
texel reaches four full-res pixels past the scale-1.0 band. A scanline constant stops being true
the moment the camera turns. Its complement is the control, and the control being *bit-identical*
across scales is what makes the band number mean anything.

It takes numpy and Pillow, which nothing else here does, and it is a hand-run measurement tool
rather than a gate — it is not on `fetch_assets.sh`'s path and no card's `verification:` runs it
automatically. **Quote the tail as well as the mean.** The finding that justified building it is
that on the same pixels in the same frames, half-resolution SSR reads 0.97x full resolution on
the mean and 1.85x at p99: a mean cannot distinguish an edge sliding smoothly across four pixels
from one that waits three frames and jumps four.

### The readback test

`scripts/readback.sh [config]`, and it is a different **kind** of check from everything
above it:

> **A texel authored is a texel presented.** Load a source PNG, present it, read the
> swapchain back, and compare bit-exact against the source expanded by the integer scale.

The expected image is **computed from the input** rather than snapped from a previous run.
That is the whole difference. A failing golden case has an answer that is sometimes "the
new image is right"; a failing case here has none, because there is nothing to re-snap
against — the engine is simply wrong. So it is a proof rather than a regression check, and
it runs at tolerance 0 with zero pixels allowed to exceed it while the golden suite runs
at 2. There is no filtered tap anywhere in the path by construction, so the driver-noise
allowance the golden suite makes has nothing to protect here.

Everything it needs is in the binary. `--readback <image>` loads the named image through
`gfx::ImageTable` and draws it at exact texel coordinates in the top-left of the overlay's
surface — placed by `Renderer`, not through `ui::Context`, because routing it through the
widget layout would put the theme's padding and the UI scale between the source file and
the answer. After the run, `gfx::compareReadback` expands the same file by the presentation
scale, places it at the letterbox offset, and holds the capture's rectangle against it. The
expanded expectation and a diff are written beside the capture, so a failure can be looked
at rather than only counted.

**Nine cases** — five across the two axes the presentation step has plus the one that is
neither, then two through the sprite pass, then two through a sprite *sheet*:

| Case | Window | What it pins |
|---|---|---|
| `native-inside` | 960x540 | The **elided** path — scale 1, no blit recorded. So it checks the overlay, the sampler and the sRGB round trip *before* presentation is asked to preserve anything; without it, a scale-3 failure would have been blamed on the blit |
| `native-outside` | 960x540 | The same, with the overlay drawn onto the window |
| `scale3-inside` | 960x540 | The arc's Phase 1 milestone: 320x180 presented at 3x, bit-identical to the source scaled by three |
| `scale3-outside` | 960x540 | The overlay after the blit, so the image is at 1:1 in the corner over a scaled world |
| `letterbox` | 1000x600 | P2's central decision. 1000x600 holds 320x180 three and a bit times, so it presents at 3x with 20 columns and 30 rows of bars, and the image is expected 20 in and 30 down. A scale that rounded up, a leftover leaning the wrong way, or bars an axis out all say so here — in texels rather than by looking wrong |
| `sprite` | 960x540 | **P4, and the reason this file is not a P2-only artefact.** `--readback-sprite` draws the same source as one sprite through an orthographic camera at one world unit per texel, with its top-left corner on texel (0, 0). The five cases above prove a texel survives the overlay, the sampler, the sRGB round trip and the blit — a path a sprite does not take. This one puts the projection, the quad, the texel-to-normalised divide and the premultiplied blend inside the same bit-exact comparison |
| `sprite-letterbox` | 1000x600 | The same through the bars, with the UI outside the virtual target — which the sprite pass ignores, because a sprite is world-space content and only the overlay has that choice |
| `sheet-cell1` | 960x540 | **P5, and the only check in the tree that can fail a sheet showing the wrong cell.** The source is cut into its own four quarters, a four-cell looping clip is played over it at 1.5 fps, and the capture is held against **one quarter** of the same file. Cell 1 is column 1, row 0 |
| `sheet-cell2` | 1000x600 | The same at 2.5 fps, expecting cell 2 — column 0, row 1 — through the bars |
| `lit-sprite` | 960x540 | **P6, and the one case here whose expectation is not a value.** A lit sprite goes through the G-buffer, the lighting pass and the tonemapper, so bit-exactness is definitionally unavailable to it and re-snapping is not the alternative. The claim moves to *coverage*, which lighting cannot change: two runs differing in one number, and every pixel outside the source's alpha silhouette must be bit-identical between them while the changed set's bounding box must be exactly the silhouette's. See [the lit exception](#the-lit-exception) |

**Three properties make the sheet cases a check rather than a tautology**, and each answers
a way this could have passed for any implementation at all:

1. **The expected cell is stated by the script, never read back out of the engine.**
   `--readback-sheet-frame` is a number the script computed; cropping the source to
   whatever cell the playback selected would compare frame selection against itself.
2. **The crop is a second, independent statement of the layout.** The comparison site
   computes the quarter from the *image size* rather than calling `SpriteTable::frameUv` —
   the same argument `compareReadback` makes about expanding rather than resampling. With
   one statement, a transposed slicing would draw the wrong cell, expect the wrong cell and
   agree with itself.
3. **The two cases differ only in the rate and expect different cells from the same run
   length.** A frame index that came from anywhere but the clock — a constant, the frame
   counter, the sprite's index — cannot satisfy both.

The timing is deterministic rather than approximately so, which for a time-driven feature is
the difference between a check and a coin toss. `--locked` feeds the clock exactly one fixed
step per frame, so the capture on frame 60 is 61 steps of 1/60 s in — 1.017 s, which is 1.525
cells at 1.5 fps and 2.542 at 2.5. **Both land mid-cell on purpose**: an assertion on a cell
boundary would be a question about float accumulation rather than about frame selection, and
would pass and fail at random. The slack is 20 steps either side in the first case and 11 in
the second.

Then a resize soak: `--resize-every 20` over 240 frames with a virtual resolution set. It
has no expected image and is not a case, because what it proves is that the scale is
derived from the window *while the window is moving*. A letterbox correct at 960x540 and a
validation error at 840x510 is the expected failure.

**The source image has to be opaque.** `engine/assets/readback.png` is committed, and
every property of it is a claim
being checked: opaque, so the blend is the identity and the expectation needs no
linear-space composite between the file and the answer; not square, so a transposed blit
cannot agree with it; a one-texel border with four different corner colours, so an
off-by-one in the offset is a whole row of differing pixels that says which way it moved; a
per-column ramp that is not a multiple of any scale, so a doubled column cannot hide; and a
single-texel diagonal, which any filter tap smears.

It was available from **P2 onward, before a single sprite existed**, which is the strongest
argument for P2's position in the P arc's order — and P4 then paid that ordering back by
adding two cases rather than retrofitting the pass into a check it was not written against.

**A sprite case is a stronger statement than an overlay case, and the difference is worth
naming.** The overlay draws an axis-aligned quad in pixel coordinates with a `1 / extent`
push constant; a sprite goes through a world position, a pivot, a rotation matrix, an
orthographic projection, a viewport transform and a divide by `textureSize`. Every one of
those is a place a half-texel can be lost, and none of them is exercised by drawing a
rectangle at pixel (0, 0).

---

## Validation

### Standard

On by default in Debug. **Zero validation errors, in every capture**, is the standard.

### SPIR-V reflection check

`verifyShaderBindings` compares reflected bindings and `SpecId` decorations against the
hand-written layouts and aborts in Debug on a mismatch. See
[rendering.md](rendering.md#reflection-as-a-check-not-a-generator).

### Synchronization validation

`--sync-validation`, which implies `--validation on`. Off by default because it tracks every
access against every barrier.

It catches a class of mistake standard validation does not report at all: a barrier whose
source stage does not cover the operation it is meant to order reads as correct and orders
nothing.

**It cannot be a gate on this machine, and the reason is precise.** The installed layers are
1.3.204.1; SPIR-V access classification arrived in 1.3.239, so every `sampler2D` fetch is
reported as a storage read and the count can never reach zero. Installing layers ≥ 1.3.239 is
what would make it a gate — the 1.3.280 loader is already here.

**Do not read the count. Read each message's stages.** Take the stage out of `usage:` and check
whether `write_barriers:` names `SYNC_<that stage>_SHADER_SAMPLED_READ`. Present means the
barrier already grants the access that actually occurs — misattribution. Absent means the
reading stage was never made visible — a real hazard. That filter found two the previous
count-based baseline had been hiding; see
[limitations.md](limitations.md#synchronization-validation-over-reports-on-older-sdks).

### RenderDoc

`scripts/rdoc.sh` sets `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`; nothing else does, and a run
without it logs that it wrote nothing. `--rdoc-capture-frame N` arms a capture at a stated
frame — a stated frame for the same reason a PNG needs one, with a sharper edge: an `.rdc`
is only worth comparing against another `.rdc` if both name the frame they hold.

The frame index in the filename is RenderDoc's count of presents, not `frameCount()`.
Guessing an offset to make those agree would be inventing a precision the API does not
offer; what the flag guarantees is that the capture is taken at a stated point well past
the load hitch.

Debug object names are attached throughout, which is what turns a RenderDoc capture from a
list of handles into something readable — and they are enabled independently of
validation, because a capture is worth naming whether or not layers are on.

---

## The unit suite

`tests/`, googletest, run by `./test.sh`. **869 tests** across forty files.

**It links only the hosted sources**, which is what lets it run under TSan where the
renderer cannot — and TSan over the profiler's per-thread slots and the logger's writer
queue is most of why the suite exists.

Coverage is deliberately shaped by what is *hard to see in a screenshot*: the profiler
(frame-0 pinning in both trim loops, GPU zone back-dating, the scopef pool cap, thread-slot
recycling), the logger (the filter pair, `vformat`'s two-pass sizing, `critical` as a death
test), the clip sampler (slerp against nlerp at a quarter, CUBICSPLINE's three-values-per-key
stride, the parent-before-child pass, the cycle that must terminate), the instance table's
free list and motion history, the collider parser, the physics character, the audio
streaming threshold, the input map's edges, the UI's layout and hit testing, and the
inspector's selection and transform writes.

**Every assertion was checked against deliberate breakage** — three mutations, seven
failures, no false passes.

What is **not** covered is anything needing a device. `GltfScene`'s texture free list has
a Debug self-check instead: extracting a slot allocator to reach the testable half would
be the Rule of Threes broken at two.

### The three suites beside it

`tests/manifest_test.py`, `tests/make_composite_scene_test.py` and
`tests/check_pins_test.py` are Python because the things they test are, and `./test.sh`
builds and runs a C++ binary. Run each directly; CI runs all three in one job that checks
out without submodules and never builds, since each writes the tree it needs into a temp
directory. That is deliberate — a generator or a validator has to work on a fresh clone,
before any asset exists.

---

## GPU frame inspection

When a rendering question needs GPU-level evidence the ordinary tools cannot give — the
contents of an intermediate render target, the exact state bound at one draw, the barrier
and layout stream, or which pipeline a pass actually selected — there are two routes.

`--capture-target list` names every readable target,
`--capture-target <name>` writes it as a PNG including whole mip chains and cascade
arrays, and `scripts/rdoc.sh` plus `--rdoc-capture-frame` produces a `.rdc` with every
object named.

**One caution when reading a capture:** a readback of a signed, near-zero target is a
black PNG whether it wrote the whole screen or nothing at all. The velocity target is
therefore counted on the stats line as well as being capturable, because the count is the
only cheap thing that tells the two apart.

---

## How work gets verified

The verification protocol the arcs held every row to. [Closing a card](../../.claude/skills/closing-a-card/SKILL.md)
is the executable form of this; what follows is the argument for why each check is the one
that catches the defect it is aimed at.

## How work gets verified — the C and D rows

The pattern that has caught real defects in this project repeatedly. Each configuration is
built and run as its **own** invocation, never chained.

- **Zero validation errors** with layers on, in every capture.
- **The golden image set.** For **C1, C14 and C15: byte-identical, all eleven cases.** All
  three change how something is produced without changing what it is, so a moved pixel is a
  defect and a re-snap is not an available answer. This is the same argument the other roadmap
  makes for G1 and G2. For C15 it has to hold *twice* — once with every sidecar present and
  once with all of them deleted — because that pair is the entire proof that the blob is a
  cache and not a second asset format.
- **A stale-sidecar check, for C15 and D9.** Touch the source glTF and confirm the blob is
  refused; corrupt the blob and confirm the same; delete it and confirm the load falls back
  to the document. D9 added a fourth arm, and it is the one a tool makes possible: the same
  scene baked twice must be **byte-identical**, because a build artifact nobody can `cmp` is
  one nobody can check. A stale `.ktx2` announces itself, both in
  the sRGB mismatch warning `GltfScene::load` already emits and in the frame. A stale geometry
  blob has no visible symptom at all, so the rejection has to be tested rather than observed.
- **A number, not an impression** — per-pass GPU cost before and after, from trace medians
  rather than single frames. Quote `Lighting` and `Frame`; `GBuffer` and `Bloom` settle into
  one of two whole-run states about 5% apart and need several runs of each arm. C8 and C11 are
  the rows this bears on.
- **The unit suite in four configurations.** C1 changes five subsystems that are all in
  `SUBSTRATE_HOSTED_SOURCES`, so TSan and ASan both have something to say about it.
- **A leak check per lifetime row.** C1, C3 and C10 all end in "and the memory goes back".
  Create and destroy a thousand of each under ASan and compare the high-water mark.
- **A scaffolded game builds and runs without touching anything under `engine/`** — the
  standing check on the public surface, inherited from G1b.
- **A section of [guides/making-a-game.md](../guides/making-a-game.md) per stage**, written while
  the stage is fresh rather than from memory six rows later.

### The D rows specifically

**Two things D1 turned out not to need.** `Input::clear()` reads as a teardown spelled the
wrong way and is not one -- it clears a text buffer, which is what `clear` should be called.
And the index-parameter inconsistency the row names largely dissolved under C1: `i`, `id` and
`character` became typed handles named for what they are, so what was left was three
slot-taking accessors on `InstanceTable`, now named `slot`/`index`.

**Byte-identical golden output is the whole verification of a D row**, and it is available for
every one of them, because no D row changes what anything produces. A moved pixel is a defect
and a re-snap is not an available answer — the same standard C1, C14 and C15 are held to. Four
additions:

- **D4 needs zero validation errors with layers on, in every capture.** Descriptor writes,
  pipeline layouts and barriers are precisely what the layer exists to check, and it is a
  stronger check on this row than the golden set is.
- **D6 changes bounds behaviour**, so it needs the unit suite in four configurations rather
  than the golden set alone. Both affected subsystems are in `SUBSTRATE_HOSTED_SOURCES`, so
  ASan and TSan both have something to say.
- **D7 is verified by its own failure modes**, not by a passing build: `./test.sh releas` must
  exit non-zero the way `./run.sh releas` already does, and a Windows build must show the
  suite compiled with the flags the engine was. A green build proves nothing here, because a
  green build is what the defect produces.
- **D8 is the row where byte-identical output is not merely required but provable in advance.**
  The four `worldFromDepth` copies are the *same expression* — three differ only in the name
  of a local, the fourth writes it on one line — so unifying them cannot move a bit, and a
  golden diff that is anything but empty means the edit was wrong rather than the pixels were.
  The spelling half of the row (`pc`, `writeonly`, `local_size_z`) compiles to the same SPIR-V
  by definition.

  **Two corrections from doing it.** `fog.comp` was described as open-coding the same
  expression and does not: it unprojects at `1e-5` rather than at `FAR_DEPTH`, because this
  projection is reverse-Z *infinite* and depth 0 unprojects to `w = 0`. It folds anyway --
  the helper takes depth as a parameter -- but the difference was load-bearing and is now
  commented where it happens. And **`decal.frag` has no test coverage of any kind**: no
  golden case draws a decal and `game/demo` authors none, so its substitution is verified by
  inspection alone. The fog change was A/B'd against a real capture and is byte-identical;
  the decal change could not be, and that gap is the finding rather than the edit.

### Where a landed C or D row gets written down

There is still no `plan/` directory. There is a board — [kanban/](../kanban/) — but it holds
*state*, one card per row, and a card is not a record of what a stage did. A stage that lands
moves its card to `done/` **and updates the reference**:

| A stage that changes | Updates |
|---|---|
| The rules, the module layout, the handle convention | `architecture/principles.md`, `architecture/README.md` |
| Pipelines, culling, the frame | `architecture/rendering.md` |
| The scene, assets, physics, audio, animation | `architecture/systems.md` |
| Build configurations, the golden suite, packaging and the bake step | `architecture/tooling.md` |
| Anything it deliberately did not do | `architecture/limitations.md` |
| **A frame-time number** | `architecture/tooling.md`'s baseline table **and** `README.md`'s headline quote, which cites it |

**That last row is there because it failed.** The README quoted 632 FPS for long enough to
outlast shadows, SSAO, IBL, bloom, SSR, TAA and fog, against a committed baseline in the same
repository reading 265. The number is `scripts/baseline.py`'s output and never the `GPU @` log
line, for the reason [CLAUDE.md](../../CLAUDE.md) gives — that line is one arbitrary frame, so a
median over runs of it is a median over arbitrary frames.

**Every stage here removes a row from `limitations.md`.** A stage that lands without touching
that file has probably not finished.

When the last row lands, this document is retired the way its predecessors were: the answers
move into the reference and the plan is deleted rather than left to rot.

## How work gets verified — the G rows

Follow the pattern that has caught real defects repeatedly. Each configuration is built and
run as its **own** invocation, never chained.

- **Zero validation errors** with layers on, in every capture.
- **The golden image set**, and for G1 and G2 specifically: **byte-identical**, all cases.
  These stages add no capability, so a moved pixel is a defect and a re-snap is not an
  available answer. G3 must leave the static scene unchanged and adds a case for a
  reparented light.
- **A number, not an impression** — per-pass GPU cost before and after, from trace medians
  rather than single frames. Quote `Lighting` and `Frame`; `GBuffer` and `Bloom` settle into
  one of two whole-run states about 5% apart and need several runs of each arm.
- **The unit suite in four configurations.** TSan is the one that proves the hosted-source
  list survived G1's CMake change.
- **Sanitizers** for anything touching threads or lifetimes. ASan needs `--no-ray-query`.
- **A scaffolded game builds and runs without touching anything under `engine/`** — the G1b
  check, and the standing check on the public surface thereafter.
- **For G9, one subsystem that was previously unexercised, per slice** — and the two stated
  numbers: a particle pool of 2 048 with `droppedSpawns()` at zero, and `decodedCount` going
  from 0 to 1 the first time a scene here holds an asset short enough to decode.
- **For G9, `./run.sh demo -- engine/assets/physics.gltf` still runs**, which is the check
  that content authored in code stayed conditional on the scene it was handed.
- **For anything driven from input, `scripts/locomotion.sh`** — described above. G12 is the
  row that needed it, and the general rule it stands for is that a capability reached through
  a keypress needs a run that presses the key *and an arm that presses the wrong one*. G13
  added the other half of that rule: **when the answer is a direction, the arm has to assert
  one.** A summed path length, a travel time and a state name are all invariant under a
  heading error, and three checks passed over one for a year because none of them was.
  `scripts/arena.sh` is the same suite over `game/battle_arena` and carries a rule of its own:
  **an arm has to press the character into something.** Nine arms across open floor could not
  see that a blocked character was reporting the speed it asked for.
- **A section of `docs/guides/making-a-game.md`** per stage, written while the stage is
  fresh.

### Where a landed G row gets written down

There is no `plan/` directory any more — the staged plan documents were retired into
[architecture/](), which is the reference rather than a history. The board,
[kanban/](../kanban/), is not one either: it holds *state*, one card per row, and a card says
where a stage is rather than what it did. So a stage that lands moves its card to `done/`
and **updates the reference** rather than appending a record beside it:

| A stage that changes | Updates |
|---|---|
| The module layout, the rules, the virtual-function count | `principles.md`, `architecture/README.md` |
| Pipelines, variants, the frame | `rendering.md` |
| The scene, assets, physics, audio | `systems.md` |
| Build configurations, the golden suite | `tooling.md` |
| Anything it deliberately did not do | `limitations.md` |

**Every stage in this arc updates `limitations.md`**, because every one of them removes a
row from it. A stage that lands without touching that file has probably not finished.

When the last row here is done, this document is retired the way its predecessor was: the
answers move into the reference, and the plan is deleted rather than left to rot.

## How work gets verified — the P rows

Everything the other two documents require — zero validation errors with layers on, the unit
suite in four configurations, a leak check per lifetime row, per-pass GPU cost from trace medians
rather than single frames, and a section of
[guides/making-a-game.md](../guides/making-a-game.md) per stage — plus the one that is this arc's
reason for existing.

### The readback test

> **A texel authored is a texel presented.** Load a source PNG, present it, read the swapchain
> back, and compare bit-exact against the source expanded by the integer scale.

**Built by P2 and described in full above** — `scripts/readback.sh`, nine cases plus a
resize soak. It was specified here before it existed and the specification held: scale 1 and
scale 3, the UI inside the virtual target and outside it. What the specification did not name
and P2 added is the `letterbox` case, which is the only one of those five that exercises the
bar offsets — and the bars are that row's central decision, so a suite without it would have
verified everything except the thing that was actually decided.

**P4 added `sprite` and `sprite-letterbox`, and the same argument applies one row along.**
All five of P2's cases draw the source through the *overlay*, so a sprite pass closed on
them would have been verified against a path it does not take. `--readback-sprite` draws
the same file as one sprite through an orthographic camera at one world unit per texel;
27,648 texels, zero differing, at 3x with and without bars.

**P5 added `sheet-cell1` and `sheet-cell2`, and this is where the argument stops being about
the path and starts being about the *content*.** P4's two cases prove the pass draws the
rectangle it was given; neither can say a word about whether the rectangle was the right one,
because both hand it the whole file. A sheet case computes the expected crop from the source
independently of the code that drew it and asserts a cell index the script stated, so a wrong
cell, a transposed slicing and a frame index that ignores the clock are three separate ways to
fail it. 6,912 texels, zero differing, in each of the two.

**The general rule the three rows arrived at**: a P row's readback case has to differ from its
predecessor in the thing the row decides, not merely in the code it exercises. ~~P8 should expect
the same — a tilemap checked only against snapped references is precisely what this arc exists
not to do.~~ **P8 declined and added no case**, so P5's is the arc's last: a tilemap is
`SpriteTable` entries placed by `frameUv`, which the `sheet` cases already cover from the
source. The rule stands for whatever row comes next.

**A row that moves the camera runs this suite, whatever its own card says.** Four of these
cases and the lit silhouette place a sprite against the world camera and compare against an
image computed from the source file, so the projection they were computed for has to be the
projection that renders. G13's follow camera wrote `focus` every frame and broke all five while
the golden set stayed byte-identical — no golden scene aims a camera through this path, and the
five overlay cases are screen space and never see it. The split *is* the diagnosis: a
regression that divides this suite exactly into "through the world camera" and "not" is naming
its own cause. Nothing else catches it — no unit test sees the camera the renderer used, and
validation layers call a correctly-drawn wrong picture a success.

#### The lit exception

**P6 is the row where the arc's own rule needs a stated exception, and it is stated rather than
quietly skipped.** The rule is *a texel authored is a texel presented, checked against the source
file*. A lit sprite cannot satisfy it: exposure, a BRDF, shadow attenuation, ambient occlusion,
fog and a tonemap curve are six corrections applied to the value in the file, and applying them
is the entire reason a game would draw one that way.

Re-snapping is not the alternative — `golden.sh snap` is forbidden and a snapped reference is the
standard this arc exists not to use. So the claim moves from the *value* to the *coverage*, which
is the largest property lighting leaves alone. The silhouette of an alpha-cutout sprite is decided
by the source's alpha, the cutoff, the pivot, the texel rect, the quad, the projection and the
viewport transform — every place a half-texel is lost — and it is computed from the file.

`gfx::compareSilhouette` asserts two things, and both are needed:

1. **Outside the expected silhouette, the two runs are bit-identical.** Zero tolerance, zero
   pixels. A sprite a texel too wide, a texel offset, mirrored or rotated puts a differing pixel
   outside the mask, and so does bleed — which is why the case runs `--no-bloom --no-ssao
   --no-ssr`. Post passes legitimately change pixels outside a silhouette and property 1 is right
   to fail on them; what is being asserted is where the sprite is.
2. **The bounding box of the differing set is exactly the mask's.** Property 1 alone is satisfied
   by a sprite that drew nothing, and by one that came out too small.

The pair of runs differs in **one number**. `--readback-lit-cutoff 2` is above every alpha there
is, so every fragment discards while the material, the instance, the indirect command and the
pipeline stay exactly as they were — a stronger control than omitting the sprite, which would
change the draw list too.

The source is `engine/assets/cutout.png`, generated beside `readback.png` rather than instead of
it, because the two checks want opposite things: P2's has to be opaque so a blend is the identity,
and this one has to have a shape. It is an L — asymmetric in both axes, so a mirror, a transpose
and a quarter turn each move the bounding box differently — whose concave notch is a large
transparent region *inside* the box, which is the half of the check a filled silhouette could not
make. Alpha is 0 or 255 and nothing between, so the cutoff is a threshold rather than a question
about where a soft edge landed.

### Row-specific additions

- **P1 needs validation layers clean across a growth event**, which is the only moment the
  descriptor array is rewritten and the only way this row can be wrong that no picture would
  show. Force it with a small initial capacity rather than waiting to reach one naturally — a
  growth path first exercised in a game is a growth path that has never been tested.
- **P1 and P3 need the golden set byte-identical.** Both change how something is produced without
  changing what it is: P1 moves where a slot comes from, and P3's perspective path must survive
  three shaders giving up their own copy of a formula. A moved pixel is a defect and a re-snap is
  not an available answer.
- **P3's byte-identical result is provable in advance**, the way D8's was: the unified
  `viewDistance` must reduce to `nearPlane / depth` for the perspective coefficients, so a
  non-empty golden diff means the edit was wrong rather than the pixels were.
- **P1 needs a leak check** — load and destroy a thousand images under ASan and compare the
  high-water mark. This is also where the release path that
  [`limitations.md`](limitations.md) records as untested finally gets cover, because
  P1 moves it somewhere a hosted test can reach the slot arithmetic even though the descriptor
  write still needs a device.
- ~~**P4** and **P8** need~~ **a number, not an impression** — a sprite count and a tile count
  against `Frame` and `Lighting` medians from `scripts/baseline.py`, never the `GPU @` log
  line, for the reason [CLAUDE.md](../../CLAUDE.md) gives. **P4 paid this** with
  `--sprites <N>`, a run mode rather than a game somebody has to write: at 10,000 sprites
  `Sprites` is 0.053 ms and `Frame` moves 0.202 -> 0.256, with `Lighting` unmoved at 0.019,
  which is itself the check that the pass is downstream of it. **P8 needed no arm of its own,
  because P4's answered it**: a tile is a sprite, so the tile count *is* the sprite count, and
  a 1080p screen of 16 px tiles is 8,160 of the 10,000 already measured. That is the
  measurement that declined the row — see
  [limitations.md](limitations.md#what-stays-declined-and-its-trigger--the-2d-arc). A number
  re-runnable by whoever reads the card is what let a later row be decided by an earlier row's
  arm rather than by an opinion.

  **`--sprites-move` is the second arm the first one needed and did not have.** `--sprites N`
  alone spawns a *static* screen, which was the right arm while the upload was unconditional
  and the wrong one the moment it stopped being: with the revision gate in place the static
  arm's copy vanishes from the trace, which proves the gate fires and says nothing about what
  it costs when it does not. The moving arm nudges every sprite every frame by a fraction of a
  texel — same overdraw, same draw, different revision — and it is what pins the "changed"
  cost at the pre-gate number. **An arm that can only get faster is not an arm**; the pair is.
- **P4 needed validation clean across a buffer growth event**, for the reason P1 did, and it
  is the same class of hazard: `ensureSpriteCapacity` doubles behind a `vkDeviceWaitIdle`,
  which never runs if every sprite is created before the first frame. `--sprites` therefore
  spawns in eight batches rather than one, so the doubling path runs three or four times
  under layers on every stress run.
- **P7 needs the unit suite in four configurations.** `Physics` is in
  `SUBSTRATE_HOSTED_SOURCES`, so ASan and TSan both have something to say about a new mutation
  path into a body.
- **P2 and P4 need a resize test.** The integer scale is derived from the window, so it changes
  under the user's hands; a letterbox that is correct at 1920x1080 and wrong at 1366x768 is the
  expected failure. `scripts/readback.sh` ends with one, and P2 ran it with layers on: 240
  frames across 12 swapchain recreates, the scale moving between 3x and 2x, zero validation
  output.

### Where a landed P row gets written down

There is still no `plan/` directory. There is a board — [kanban/](../kanban/) — but it holds
*state*, one card per row. A row that lands moves its card to `done/` **and updates the
reference**:

| A row that changes | Updates |
|---|---|
| The image lifetime, the module layout | `architecture/principles.md`, `architecture/README.md` |
| A pass, a pipeline, the frame, presentation | `architecture/rendering.md` |
| Sprites, tilemaps, the camera, physics | `architecture/systems.md` |
| The golden suite, the readback test, packaging | `architecture/tooling.md` |
| Anything it deliberately did not do | `architecture/limitations.md` |
| **A disposition** | `architecture/principles.md`'s discharge table — **P1 changes one**, from Delegated to Generalized, which no C row did |

**Every row here removes a line from `limitations.md`.** A row that lands without touching that
file has probably not finished.

When the last row lands, this document is retired the way its predecessors are: the answers move
into the reference and the plan is deleted rather than left to rot.
