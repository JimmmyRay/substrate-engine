# Substrate

A Vulkan game engine. The renderer is deferred PBR with per-sample MSAA resolve,
built on Vulkan 1.3 dynamic rendering, with CPU and GPU profiling wired in from
frame zero.

Current state: renders Sponza at **305 FPS** (RTX 3060 Ti, 1600×900, 4x MSAA, 3.202 ms
GPU) with cascaded and punctual shadows, SSAO, bloom, SSR, TAA and fog, alongside
Jolt physics, miniaudio, skeletal animation, a GPU particle system and an immediate-mode
UI. That number is the 4x row of the
[committed baseline](docs/architecture/tooling.md#the-current-baseline) and is regenerated
by `substrate bench`, not read off the `GPU @` log line.

## Build and run

```bash
scripts/setup.sh                 # submodules, a dependency check, and Sponza (~41 MB, gitignored)
scripts/build.sh                 # engine + unit suite; also release | asan | tsan | clean
scripts/run.sh                   # the engine's test scene; also release | asan | tsan
scripts/run.sh demo              # game/demo/, built on demand
scripts/test.sh                  # unit suite; also release | asan | tsan
```

Starting your own instead:

```bash
scripts/new_game.sh mygame       # scaffolds game/mygame/ from the template
scripts/build_game.sh mygame
scripts/run.sh                   # the build directory remembers which game it holds
```

**`scripts/build.sh` produces no runnable binary** — a library and a test executable. The
engine lives in `engine/` and games live in `game/`, and the engine has to build, test and
sanitize with nothing under `game/` in the tree; that is what makes a dependency leaking
from a game into the engine a link error rather than a code review. `scripts/run.sh` with no
game name opens the engine's test scene; `scripts/run.sh demo` runs that game, and builds it
first if the configuration does not already hold it.

`scripts/test.sh tsan` is where the threaded code actually gets checked: the suite links only
the translation units that need neither Vulkan nor a window, and `scripts/run.sh tsan` cannot
run the renderer at all — the proprietary NVIDIA driver segfaults inside `vkCreateDevice`
under ThreadSanitizer.

Requires a C++20 compiler, CMake ≥ 3.20, Ninja, a Vulkan 1.3 driver, and
`glslang-tools`. See [docs/guides/building.md](docs/guides/building.md) for detail.

## Controls

Every one of these is a **named action with a default binding**, not a hardcoded key.
`Tab` opens the binding menu, which lists all 49 of them with the controls they are on;
`F2` there writes what you changed to `substrate.json`, so a rebind survives a restart.
A gamepad drives the same actions — the camera ships bound to both.

| Input | Action |
|---|---|
| Mouse drag | Orbit |
| Scroll | Dolly |
| W A S D / Q E | Move |
| Shift / Ctrl | Faster / slower |
| `Tab` | Binding menu: Up/Down select, Enter rebind, F5 default, F2 save, type to filter |
| `1` `2` `4` `8` | MSAA sample count |
| `F1`–`F5` | Lit / albedo / normal / ORM / depth |
| `F6` | Frame stats overlay |
| `F7` / `L` | Save / load (C6) |
| `V` | Path the character to the camera, over the navmesh (C12) |
| `X` | Re-stream the scene on a worker thread (C10) |
| `Z` / `H` | Append another glTF into the live scene / drop the last one (C10) |
| `F8` / `F10` | SSAO / bloom |
| `F11` | Tonemap operator: ACES, Reinhard, clamp |
| `` ` `` | Cycle debug view |
| `R` / `G` / `T` / `N` | SSR / fog / TAA / particles |
| `Y` | Ray tracing |
| `C` / `M` | GPU frustum culling / edge MSAA |
| `F12` / `PrintScreen` | Screenshot / RenderDoc capture |
| `.` | Start or stop recording the session to mp4 (S7) |
| `B` | Physics collision-shape wireframe (S4.5) |
| `J` / `K` | Mute audio / draw listener-to-source lines, red where occluded (S5.5) |
| `I` | Settings panel: toggles, sliders, a debug-view list, a text field (S6) |
| `O` | Node inspector |
| Arrows / `Space` | Walk / jump, where the scene declares a character (S4.4) |
| `P` | Print profiler table and GPU timings |
| `Esc` | Quit |

`F8`–`F11` flip **specialisation constants**, so each one recompiles the pipelines that
read it — one hitched frame, after which the disabled feature costs nothing at all
rather than costing a branch that is never taken. That is what makes a per-pass cost
attributable: toggle, read the overlay, toggle back.

## Configuration

`substrate.json` holds window, scene, render, input, lighting, camera, physics, audio,
ui, profiler and logging settings. Regenerate a fully populated one with:

```bash
./build/debug/demo --write-default-config
```

Command-line flags override it for per-invocation values — `--msaa`, `--frames`,
`--trace`, `--debug-view`, `--overlay`, `--no-ssao`, `--no-bloom`,
`--tonemap`, `--config`. Run `./build/debug/demo --help` for the list.

The simulation clock is `realtime` by default: animation, particles, physics and audio all
step from wall-clock time. `--locked` takes exactly one fixed step per rendered frame
instead, which makes frame N a function of N alone — that is what golden images and
per-pass benchmarks need, and `scripts/golden.sh` and `substrate bench` pass it
themselves. Running interactively with a locked clock simulates at the frame rate, which
at several hundred FPS is roughly ten times too fast.

## Shader hot reload

`--hot-reload` (on by default in Debug) watches both shader trees — `engine/shaders/` and
the configured game's `shaders/` — and, on any change, recompiles every shader and
rebuilds every pipeline: about a second, including a re-bake of the environment maps. A
shader that fails to compile logs the error and leaves the running one in place.

A game may supply its own GLSL in `game/<name>/shaders/`. It compiles with the engine's
tree on its include path, so it can `#include "frame.glsl"`, and it is looked up *first* —
so a game shader named after an engine one replaces it, for that game only. The engine's
copy is untouched and still what every other game gets.

## Test scenes

Sponza cannot exercise everything: it has no emissive or blended materials, declares no
lights at all, places every primitive exactly once and never moves. Ten small scenes do,
each self-contained in a single file — geometry, animation and textures all inline as
base64, so none of them reaches for anything beside it.

**They are committed**, unlike everything else in the two asset trees. A scene written
from nothing is first-party, and a file is a better record of it than a script that
rebuilds it: the golden suite pins these byte for byte, and a checkout gets exactly what
the baselines were taken against without running anything. That is also why the two trees
are gitignored by their *contents* rather than as directories — see `.gitignore`.

They are split across the two trees by who depends on them. `engine/assets/` holds
the three the golden suite pins — a scene a regression suite names is the engine's,
because deleting the demo must not cost the engine the ability to check itself. The rest
exist for looking at a feature by hand, which is the demo's business, and live in
`game/demo/assets/`. Sponza is the engine's for the same reason: eight of the eleven
golden cases render it.

| Scene | Exercises |
|---|---|
| `game/demo/assets/materials.gltf` | Emissive and blended materials (1.4, 1.8) |
| `game/demo/assets/lights.gltf` | `KHR_lights_punctual`, one of each type (2.6) |
| `game/demo/assets/stress.gltf` | More lights than any budget — a ring of 40, alternating bright-warm and dim-cold so a wrong ranking tints the frame blue (0.9, 0.10) |
| `game/demo/assets/instances.gltf` | 4096 placements of one mesh, spread wide (4.2, 4.5) |
| `engine/assets/skin.gltf` | A skinned, animated joint chain (4.4) |
| `game/demo/assets/morph.gltf` | A grid with two morph targets and a `weights` animation (S2.1, S2.2) |
| `engine/assets/particles.gltf` | Four emitters over a floor and a wall — a colliding lit plume, an additive spark fountain, a self-overlapping dust field and a jet aimed by its node (S3) |
| `engine/assets/physics.gltf` | Colliders in `extras`: a triangle-mesh floor, a scaled and rotated ramp, a stack of convex hulls being hit by falling boxes, rolling spheres and a character capsule (S4) |
| `game/demo/assets/audio.gltf` | Sounds in `extras`: a room split by a solid wall, a streamed non-spatial bed, a spatial source behind the wall to walk around, and a rattle bolted to a falling crate (S5) |
| `engine/assets/mirror.gltf` | Reflections either side of the SSR roughness cutoff — a mirror floor at 0.02, spheres at 0.05/0.25/0.35/0.65, a wall at 0.90 — with a pylon whose shadow lands on the wall, so it is visible in the reflection as well as directly |

Two more are *grafted* onto files this repository cannot ship, by
`scripts/make_composite_scene.py`. **These are the ones that stay generated**, and the
reason is the same licenses: a graft is a deep copy of its input, so `character.gltf`
carries the rig's whole node, accessor and animation table and `reflect.gltf` carries
Sponza's. Committing either would commit the file the license excludes. Each is skipped
when its source is absent:

| Scene | Grafted onto | Exercises |
|---|---|---|
| `game/demo/assets/character.gltf` | Any skinned, animated glTF — `--character <path>` | Blending, state machines and per-character playback (S2.3–S2.5), on a floor and a backdrop, because `Camera::frameBounds` frames a room and not a figure |
| `game/demo/assets/reflect.gltf` | Sponza | A mirror sphere and a row of rising roughness (1.9, 3.11) — Sponza has no smooth surface anywhere, so reflections had nothing in it to be seen *in* |

There was a third, `showcase.gltf`, and it was the default scene: Sponza with a mirror, an
orb, a character and two sounds grafted on at build time. The demo builds all of it in code
now and loads Sponza itself, which is what C21 and C22 made possible — a glTF that deforms
can be imported into a running world, so a composite document is no longer the only way to
put a rig in a building. See [docs/architecture/](docs/architecture/).

`substrate fetch-assets` writes these two after fetching Sponza, so a fresh clone gets a
default scene rather than an error.

`--characters N` places N copies of a scene's skinned mesh, each its own animator
character with its own clip, time and state machine. One is the file as authored.

`particles.gltf` and `physics.gltf` are also golden cases, among the few that are not
Sponza. The first is there because the set needs a scene lit from a direction Sponza never
exercises: Sponza's floor is lit by the sun and four auto-placed punctual lights, so an
interior lit from below still reads as an interior lit.
The second pins something no other case can — a settling stack of rigid bodies is the most
sensitive thing in the suite to a step order or a thread count changing, and it drifts
rather than breaks.

## Documentation

[docs/README.md](docs/README.md) is the index. `docs/architecture/` is the reference —
what the engine is, how the frame is built, what it deliberately does not do, and what
was measured. `docs/guides/` covers [building](docs/guides/building.md),
[profiling](docs/guides/profiling.md) and
[making a game](docs/guides/making-a-game.md). `docs/kanban/` is the board — every piece of
planned work as a card, with the directory it sits in as its status.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) and
[NOTICE](NOTICE).

Sponza is fetched separately and is **not** part of this repository — it is © Crytek
under the CryEngine Limited License Agreement, which is not compatible with Apache
2.0. Both asset trees are gitignored.

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in this work shall be licensed as above, without any additional
terms or conditions.
