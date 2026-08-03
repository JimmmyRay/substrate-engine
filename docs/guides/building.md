# Building

## Requirements

| Requirement | Verified on this machine |
|---|---|
| C++20 compiler | GCC 12.3 |
| CMake ≥ 3.20 + Ninja | CMake 4.2.1, Ninja 1.11.1 |
| Vulkan loader + headers | 1.3.280 (`libvulkan-dev`) |
| GLFW | `libglfw3-dev` |
| `glslangValidator` | 11.8 (`glslang-tools`) |
| `spirv-val` (optional, Debug) | present (`spirv-tools`) |
| GPU | Vulkan 1.3+; validated on RTX 3060 Ti, driver 580.159.03 |

On Debian/Ubuntu derivatives:

```bash
sudo apt install build-essential cmake ninja-build \
                 libvulkan-dev libglfw3-dev glslang-tools spirv-tools
```

## First run

```bash
./setup.sh                  # submodules, a dependency check, and the sample assets
./setup.sh --no-assets      # skip the download
```

Everything it does is idempotent, and every one of the three steps is available on its own
below. It refuses with a list rather than a build failure when `cmake`, `ninja`,
`glslangValidator`, `git` or `python3` is missing — a missing `glslangValidator` otherwise
surfaces as a shader that fails to compile several minutes in, a long way from the sentence
that explains it.

It deliberately does not build. Which configuration, and whether a game comes with it, are
the first real decisions.

## Starting a game

```bash
./new_game.sh mygame        # scaffolds game/mygame/ from scripts/template/game/
./build_game.sh mygame
./run.sh
```

The name becomes a directory, a CMake target and part of a C++ class name, so it must be
lowercase and identifier-shaped; `new_game.sh` says so rather than letting CMake discover
it three commands later. What lands is a `Game` subclass, a one-line `CMakeLists.txt` and a
README — it loads no scene and draws one generated settings panel, which is the smallest
thing that shows the loop running.

## Build

```bash
./build.sh                  # debug (default)
./build.sh release
./build.sh asan             # AddressSanitizer + UBSan
./build.sh tsan             # ThreadSanitizer
./build.sh clean
```

**This builds the engine and the unit suite, and produces no runnable binary** — a
static library and a test executable. The engine has to build, test and sanitize with
nothing under `game/` in the tree, which is the strongest available check that the
module boundary is real: a dependency leaking from a game into `engine/` becomes a link
error rather than a code review.

A program comes from a game:

```bash
./build_game.sh demo                 # debug build of game/demo/
./build_game.sh demo release
./build_game.sh --list               # every game/<name>/ in the tree
```

```bash
./run.sh                                    # the engine's test scene, debug
./run.sh demo                               # game/demo/, debug
./run.sh demo release                       # either order; `./run.sh release demo` is the same
./run.sh release -- --msaa 8 --frames 900 --trace bench/8x.json
./run.sh -- path/to/other.gltf
```

`run.sh` selects the matching build directory, sets sanitizer options, and wraps the TSan
config in `setarch -R`. Both leading arguments are optional and may be given in either
order: a name is a directory under `game/`, a configuration is one of the four.

**Naming no game opens the engine's own test scene**, Sponza — the same scene the golden
suite pins, so `./run.sh` on a fresh clone shows you the renderer rather than an
instruction. Naming one runs that game and lets the scene its `configure` names decide. A scene given
after `--` wins over both.

Which game a build directory holds is still a property of the directory:
`build_game.sh` writes the name into the CMake cache, and `scripts/golden.sh` and
`scripts/baseline.py` read it back, which is why neither grew a game argument. What
`run.sh` adds is that a mismatch is repaired rather than reported — ask for a game the
configuration does not hold and it calls `build_game.sh` for you. That matters most right
after `./build.sh`, which clears the name; before, every later `./run.sh` refused with a
stale binary still sitting in the directory. `./test.sh` deliberately preserves it,
because running the tests must not silently un-configure your game.

The engine's object files survive the toggle, so alternating between two games -- or
back to `./build.sh` -- costs a reconfigure rather than a rebuild.

<details><summary>Equivalent manual commands</summary>

```bash
git submodule update --init --recursive
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSUBSTRATE_GAME=demo
cmake --build build/debug
```

`-DSUBSTRATE_GAME=` (empty) is the engine-only build. `-DSUBSTRATE_ENTRY_POINT=OFF`
drops `main()` from the library, for a game that wants to drive `Engine` by hand.

</details>

Every configuration builds into its own subdirectory of `build/` — `build/debug`,
`build/release`, `build/asan`, `build/tsan` — so `./build.sh clean` is a single
`rm -rf build`.

The **first configure requires network access**: fastgltf downloads the simdjson
amalgamation via `file(DOWNLOAD)`. It is cached in the configuration's build
directory for later configures.

Release builds:

```bash
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUBSTRATE_GAME=demo
cmake --build build/release
```

## Assets

```bash
scripts/fetch_assets.sh
```

Blobless sparse clone of Khronos Sponza into `engine/assets/Sponza` (~41 MB, 73 files).

Sponza is © Crytek under the CryEngine Limited License Agreement — incompatible
with this repository's Apache-2.0 license. Both asset trees are gitignored; do not commit it.

## Configuration

Settings live in `substrate.json`, not in the command line:

```bash
./build/debug/demo --write-default-config  # writes a populated substrate.json
```

| Section | Controls |
|---|---|
| `window` | width, height, vsync |
| `render` | `msaaSamples`, every quality and artefact toggle, the SSAO / shadow / bloom / SSR / fog knobs, `debugFont` and `debugFontHeight` |
| `input` | gamepad deadzone, key repeat, and the `bindings` map |
| `camera` | fov, move speed, orbit sensitivity, zoom step |
| `physics` | `maxStepsPerFrame`, `workerThreads` |
| `audio` | on/off, sample rate, channels, master volume, stream threshold, decode budget |
| `ui` | `scale` |
| *(a game's own)* | whatever `Game::declareSettings` adds — the demo's section is `demo` |

Eight modules, and that is the whole list: a value that is not a property of the person
running the program is not in this file. The scene, the lighting, the exposure, the tonemap
and the four capacity budgets are the game's, in `GameSetup`; the validation layers, the
profiler, the recorder, the log, the debug windows and the frame limit are developer controls
with named flags and no key. `--help` prints those, `--dump-settings` prints these, and a
config still carrying a key that moved gets a sentence saying which of the two it became.

Missing keys keep their defaults; a missing file logs a warning and uses defaults
throughout. Malformed JSON is a hard error reporting the byte offset.

Overrides for per-invocation values: `--config`, `--msaa`, `--frames`, `--trace`,
`--debug-view`, `--log-level`, `--validation`. Anything else in the file is set for one
run with `--set <key>=<value>` — `--set window.vsync=true` — and `--dump-settings` prints
every key there is. A bare positional argument is the scene path.

## Run

```bash
./build/debug/demo engine/assets/Sponza/glTF/Sponza.gltf
```

## Shaders

Two trees compile, into two directories:

| Source | Output | Baked in as | Include path |
|---|---|---|---|
| `engine/shaders/*.{vert,frag,comp}` | `<build dir>/shaders/` | `SUBSTRATE_SHADER_DIR` | itself |
| `game/<name>/shaders/*.{vert,frag,comp}` | `<build dir>/shaders/game/` | `SUBSTRATE_GAME_SHADER_DIR` | itself, then the engine's |

`readShaderBinary` tries the game's directory first, so a game shader named after an
engine one replaces it for that game and leaves the engine's file untouched. The output
directory is `shaders/game` rather than `shaders/<name>` because one build directory holds
one game — which keeps both defines the same string whatever game is configured, and so
keeps a `build_game.sh` toggle from recompiling the engine.

`<build dir>/shaders/game/` is emptied on every configure, and it is the only directory
that is. A compiled shader outlives its source, and in that tree the presence of a file is
what decides which shader runs — so a deleted game shader would otherwise keep overriding
the engine's forever.

A shader that fails to compile fails the build rather than surfacing at runtime. In Debug
the SPIR-V is additionally checked with `spirv-val`.

Adding a shader requires re-running CMake configure, since the glob is evaluated at
configure time (`CONFIGURE_DEPENDS` makes Ninja re-check, but a fresh configure is
the reliable path).

## Releases

`./build_game.sh <name>` produces a program that runs out of the build tree it was
built in: the shader and asset roots are absolute paths baked in at compile time, which is
what lets it be launched from any working directory. `./build_release.sh` produces the other
thing -- a directory that carries everything it needs and can be moved to a machine that has
never seen this source tree.

```bash
./build_release.sh demo                    # Linux AppImage into releases/
./build_release.sh demo all --docker       # both platforms, in containers
./build_release.sh demo --strict           # fail if anything in it cannot be redistributed
./build_release.sh --list                  # every game in the tree
```

The difference is one CMake flag. `SUBSTRATE_PORTABLE=ON` swaps all four runtime roots for
relative names, which `executableDir()` anchors to the binary, and turns shader hot reload
off because its paths are this machine's shader sources and its glslang. All four move
together or none do; a package with relative shaders and an absolute asset root looks like
it works right up until it runs somewhere else.

**What goes in the package is not a list kept anywhere.** `scripts/manifest.py` works it out
by following what the engine would actually load -- the config, then every glTF it reaches,
then their buffers, images, `.ktx2` sidecars and audio -- and fails naming anything it
cannot find. That runs *before* the build, so a missing asset costs seconds rather than a
full compile. Note that a grep for `res:/` would find almost nothing: the real references
are runtime values from the config and, mostly, document-relative names inside the glTFs.

**The scene cache is baked too, after the textures.** C15's `<name>.gltf.scene` holds
everything a load derives between parsing the document and touching the device -- geometry,
placements, materials, the rig, colliders, emitters and sounds -- and a package that ships
one skips all of it. The ordering is not pedantry: a scene whose *embedded* images have no
`.ktx2` is refused a sidecar, because the sidecar could not be read back without the
document it exists to avoid opening, and baking the textures first is what stops that case
arising. A scene that cannot be baked is a warning, not a failure: the package still runs,
it just parses at launch.

**The baker is `scripts/bake.sh`, and it needs no GPU.** It builds `substrate-bake` and runs
it over every scene named, which is one invocation for the whole package. Until D9 this was
the engine -- `./run.sh release -- --headless --locked --frames 3 --bake-scene <scene>` --
which meant a Vulkan device, a swapchain and every texture upload had to come up to produce
a CPU-side artifact that touches none of them, so the one step of a package with no use for
a GPU was the step that could not run without one. `substrate-bake` links neither Vulkan nor
a window.

It is still C++ and still the engine's own structs, which is the part that was always
right: the writer shares `Vertex` and `Primitive` with the reader, so a field added to
either is a compile error in one place, and a Python writer would be a second encoding of
the same layout with no compiler to make the two agree.

**Run it by hand when you want to time a cold start against a warm one**, and nowhere else
in the development loop -- `build.sh` deliberately does not bake, because a build that
refreshed every sidecar would hide a stale one rather than let C15's stamp catch it. The
output is reproducible, so re-baking a scene that was already current costs time and nothing
else.

**The texture cache is baked before the manifest is taken, and then required.** A missing
`.ktx2` is normal in a source tree -- the loader finds no sidecar and decodes the source
image, which is the property that makes the cache a cache. In a package it is not normal:
it ships the decode path to someone with no `ktx` on their PATH and no way to rebuild what
they are missing. So `build_release.sh` resolves the manifest once to learn which scenes
are staged, runs `scripts/ktx2.py` over each, and then takes the real manifest with
`--require-cache`, which fails naming every image that came back cold. The list of scenes
to bake is decided by the same resolver as everything else, so it cannot fall out of step
with what is actually packaged.

The package mirrors the source tree's shape -- `shaders/`, `engine/assets/`,
`game/<name>/assets/` -- rather than something tidier. That is load-bearing: a glTF names
its buffers relative to itself, and the composite scenes reach across into the other tree
with `../../../engine/assets/...`, so the two trees have to stay the same distance apart in
the package as they are here.

### Windows

Cross-compiled from Linux with MinGW-w64, in the container from
`docker/windows.Dockerfile`. MSVC was rejected for one concrete reason: it cannot run in a
Linux container, so choosing it would have meant no reproducible cross-build. The
consolation is large -- MinGW *is* GCC, so the entire warning set is valid verbatim and
there is no `if(MSVC)` branch anywhere in `CMakeLists.txt`. **Do not add one.**

One thing is checked on every Windows build:

- **The import table.** Only system DLLs may appear. `libstdc++-6.dll`, `libgcc_s_seh-1.dll`
  or `libwinpthread-1.dll` mean `-static` stopped applying; `vulkan-1.dll` means volk is no
  longer loading the loader at runtime, and the exe would die in the Windows loader on a
  machine with no Vulkan instead of printing its own error.

That check reads a file. **Nothing executes the Windows build** -- the unit suite is compiled
and not run, the installer is produced and not installed, and the container has no way to run
a PE. Two checks that used to exist, both under wine, were removed: wine is a proxy for
Windows rather than Windows, and its prefix wrote a `dosdevices/z:` symlink to `/` inside
`build/`, which sent every editor and indexer that walks the repository into the whole
filesystem by way of `/proc/<pid>/cwd`.

**The Windows build is compile- and link-verified, and not runtime-verified at all.** See
[limitations.md](../architecture/limitations.md#the-windows-build-is-not-verified-on-a-windows-gpu)
for the full list of what that leaves open.

## Notes

- `CMAKE_EXPORT_COMPILE_COMMANDS` is on; `build.sh debug` symlinks
  `build/debug/compile_commands.json` to the repo root, where clangd looks.
- volk owns the Vulkan header, so `GLFW_INCLUDE_NONE` is defined globally. Include
  `volk.h` before `GLFW/glfw3.h` in any translation unit that needs both.

## Sanitizers

```bash
cmake -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan && setarch -R ./build/tsan/substrate_tests
```

`setarch -R` is **required**. This kernel's ASLR entropy exceeds what ThreadSanitizer
supports, and without it TSan aborts with `unexpected memory mapping` before `main()`
— which looks exactly like a clean run if you only grep for race warnings.

ASan + UBSan needs no such workaround:

```bash
cmake -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

## Validation layers

Substrate repairs a hostile `VK_LAYER_PATH` itself. That variable **replaces** the
loader's default explicit-layer search path rather than extending it, so a stale
`export VK_LAYER_PATH=$VULKAN_SDK/...` with `VULKAN_SDK` unset expands to a directory
that does not exist and silently disables all layer discovery — validation included.

`VulkanContext::init` detects this: if validation was requested but the layer is not
found, it appends the standard directories to `VK_LAYER_PATH` and re-enumerates. A
deliberately-set path stays first in the list, so it still wins. You will see:

```
[Vulkan] [WARNING] VK_LAYER_PATH hid the system layers; appended standard paths
```

If validation is still unavailable after that, the layer really is missing:

```bash
sudo apt install vulkan-validationlayers
```
