# Substrate — architecture

Substrate is a Vulkan 1.3 deferred PBR game engine. It loads glTF, shades it with
per-sample MSAA or TAA, simulates rigid bodies and characters, mixes spatial audio, and
draws an immediate-mode interface over the result — in about 26,300 lines of `engine/`, with
**three base classes and two templates**: one because Jolt's API demands an override, one
because a game has to be called into somehow, and one because a camera controller is policy
about input and the engine should not assert one.

This directory replaces the roadmap that built it. The roadmap was a plan; these are the
answers it arrived at. Where a decision was made *against* an obvious alternative, the
alternative and the reason are recorded — because the second most useful thing to know
about a codebase is what it deliberately does not do, and why.

## The documents

| Document | Covers |
|---|---|
| [principles.md](principles.md) | The rules the code is held to: no abstraction over Vulkan, the Rule of Threes, the scale dispositions, and the concrete anti-patterns |
| [rendering.md](rendering.md) | The frame in order, every pass, every render target, the pipeline and descriptor model, and the shader variant system |
| [systems.md](systems.md) | The instance table, glTF loading, animation and skinning, particles, physics, audio, input and the UI |
| [tooling.md](tooling.md) | Build configurations, profiling, frame capture, the golden-image suite, validation, and how a performance claim is made |
| [limitations.md](limitations.md) | Every caveat, stated limit and deliberate omission, in one place |

These five documents are the whole of the engine's documentation. Where a number appears
it was measured on this machine and the tool that produced it is committed alongside it.

Three live plans sit outside them and are deleted when their last row lands:
the C arc for what the engine can do, the G arc
for how a game reaches it, and the P arc for the second dimension.

## The shape of the engine

```
engine/                the engine. Builds to a static library. Knows of no game.
  Engine.{h,cpp}    everything main() used to own, and the frame cut into four phases
  Game.h            the one interface the engine defines: five empty-defaulted methods
  Entry.{h,cpp}     main(), and the SUBSTRATE_GAME(T) macro that reaches it
  |                 all three at global scope: they are the edge, not a module
  |
  +-- gfx/          namespace gfx. Vulkan: Renderer, VulkanContext, Swapchain, Pipeline,
  |                 Resources (GPU buffers and images), FrameCapture, AccelStruct,
  |                 SpirvReflect, Ktx2, Light, GpuProfiler
  +-- scene/        namespace scene. Content and simulation: InstanceTable, GltfScene,
  |                 Camera (a pose and a projection) and CameraControllers (FlyCamera,
  |                 which a game installs or does not), Animation, ParticleSystem, Physics, Collider, Audio,
  |                 AudioSource, SceneTypes (the plain data), SceneData (C15's sidecar),
  |                 MeshLod (C17's chains and the coverage arithmetic),
  |                 SpatialIndex (C9's BVH), SpriteTable (P4's layers and order)
  +-- core/         namespace core. Config, Logger, Profiler, Resources (a file on disk --
  |                 unrelated to gfx/Resources, see below), Handle, AudioTap, Recorder,
  |                 and the four nested vocabularies core::{settings,options,input,json}
  +-- ui/           namespace ui. Ui, Font, FontMetrics, BindingMenu, Inspector
  +-- shaders/      every .vert/.frag/.comp the engine compiles, and the .glsl they share
  +-- assets/       Sponza and the three scenes scripts/golden.sh pins. Gitignored.

game/               games. Not built by build.sh.
  demo/             what main.cpp was: the actions, the panel, the animation drive
    shaders/        optional. Searched before the engine's, so a name here wins
    assets/         the demo's content, including the default scene. Gitignored.

tests/              the unit suite. Links the hosted sources only; never touches game/.
```

Three properties of that layout are load-bearing rather than tidy:

**`gfx/` depends on nothing above it.** The renderer receives a `const GltfScene*`, an
`InstanceTable*`, a `SceneAnimator*` and two plain vertex vectors (`debugLines`,
`overlayLines`) plus a `DrawList*`. Physics reaches the renderer through
`gfx/DebugLines.h`, a header of sixteen bytes that exists so that neither Jolt reaches the
renderer nor Vulkan reaches the physics world. Audio never reaches it at all.

**The hosted set is a real boundary, and nothing writes it down.** `CMakeLists.txt` globs
every `.cpp` under `engine/` into one static library and names no file. The unit suite,
the baker and the sim loop link that archive and link no volk and no glfw, so a member
that needs a driver is an undefined symbol at link — which is what lets the suite run
under ThreadSanitizer where the renderer cannot (the proprietary NVIDIA driver segfaults
inside `vkCreateDevice` under TSan). Getting a file into that set is done by removing its
device dependency, never by wrapping one and never by editing a list. Nothing under
`game/` ever joins it.

**`build.sh` produces no runnable binary, and that is the check on `game/`.** The engine
must build, test and sanitize with nothing under `game/` in the tree, so a dependency
leaking from a game into `engine/` becomes a link error rather than a code review.
`./build_game.sh <name>` is what produces a program; it records the name in the build
directory's CMake cache, which is why `run.sh`, `golden.sh` and `baseline.py` all kept
the signatures they had when the binary moved.

That check is about direction, not about size, and G10 is where the difference was measured
rather than assumed: **every game links every subsystem, and a game that uses none of them
is 97.8% the size of the demo that uses all of them.** `Engine.cpp.o` is what pulls them —
it is in every game and it names them all — and the mechanism that would have changed it was
declined with its numbers and its triggers in
[limitations.md](limitations.md#what-stays-declined-and-its-trigger--the-game-arc). What
holds today is one rung down: `Engine`'s constructor and destructor are out of line, so
`Entry.cpp.o` and a game's own translation units name no subsystem type.

## What it does

- **Deferred PBR** with a four-target G-buffer, per-sample MSAA resolve or TAA, octahedral
  normals, and edge-detect hybrid shading.
- **Shadows**: one scene-fitted map for the sun, a 24-layer atlas for points and spots, and
  an optional ray-query path.
- **Indirect light**: a flat author-set ambient, defaulting to zero. Split-sum IBL and a
  traced sky gather were both built and both removed; see `rendering.md`.
- **Screen-space and ray-traced reflections**, volumetric fog, SSAO, bloom, decals.
- **Scene scaling**: an instance table, GPU frustum culling, indirect submission, and
  instancing — recording a frame is O(passes), not O(draws).
- **Animation**: skins, morph targets, cross-fade blending, state machines, and BLAS refit
  for ray tracing deformed geometry.
- **Particles**: GPU simulation and a bitonic sort, with no atomic anywhere in the path.
- **Physics**: Jolt, colliders authored in glTF `extras`, a fixed-step accumulator with
  interpolated render state, and a `CharacterVirtual` controller.
- **Audio**: miniaudio, spatialised sources on scene nodes, ducking buses, raycast
  occlusion, and a device-less mode that mixes into a buffer for the test suite.
- **Interface**: an immediate-mode UI, a rebindable action layer with gamepad support, and
  an instance inspector.

The details, and the decisions behind each, are in [rendering.md](rendering.md) and
[systems.md](systems.md).

## Working on it

Always through the scripts. They encode environment fixes that are invisible when missing
and produce results that look correct but are worthless:

```bash
./setup.sh      [--no-assets]                     # submodules, dependencies, assets
./new_game.sh   <name>                            # scaffold game/<name>/
./build.sh      [debug|release|asan|tsan|clean]   # the engine and the unit suite
./build_game.sh <name> [debug|release|asan|tsan]  # the engine, plus one game
./run.sh        [debug|release|asan|tsan] -- [scene.gltf] [options]
./test.sh       [debug|release|asan|tsan] -- [--gtest_filter=...]
```

A first build is `./setup.sh && ./build_game.sh demo && ./run.sh`. `run.sh` takes no game
argument: the choice lives in the build directory.

See [tooling.md](tooling.md) for what each configuration can and cannot do, and
[../guides/building.md](../guides/building.md) for the mechanics.
