# Substrate — architecture

Substrate is a Vulkan 1.3 deferred PBR game engine. It loads glTF, shades it with
per-sample MSAA or TAA, simulates rigid bodies and characters, mixes spatial audio, and
draws an immediate-mode interface over the result — in about 26,300 lines of `engine/`, with
**three base classes at the edges, one interface per module, and two templates**. The three:
Jolt's API demands an override, a game has to be called into somehow, and a camera controller
is policy about input the engine should not assert. The interfaces are what keep `Engine.cpp`
from naming a subsystem, and each is its own do-nothing implementation.

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
  +-- scene/        namespace scene. Content, and the description a document authors:
  |                 InstanceTable, GltfScene, Scene, Simulation, Camera (a pose and a
  |                 projection) and CameraControllers, SceneTypes (the plain data),
  |                 SceneData (C15's sidecar), MeshLod (C17's chains and the coverage
  |                 arithmetic), SpatialIndex (C9's BVH), SpriteTable (P4's layers and
  |                 order), and one description header per module: Collider, AudioSource,
  |                 ParticleEmitter, AnimationRig, CharacterMotion, Body, Cloth
  +-- core/         namespace core. Config, Logger, Profiler, Resources (a file on disk --
  |                 unrelated to gfx/Resources, see below), Handle, Slot, Clip, AudioTap,
  |                 Recorder, and the four nested core::{settings,options,input,json}
  +-- ui/           namespace ui. Ui, Font, FontMetrics, BindingMenu, Inspector
  +-- Modules.h     one interface per module, each one its own null implementation
  |
  |                 the modules -- what root cannot reach, and what a game links by
  |                 naming it:
  +-- physics/      namespace physics. PhysicsWorld, ClothSystem
  +-- audio/        namespace audio. AudioEngine
  +-- anim/         namespace anim. SceneAnimator, LocomotionDriver
  +-- particles/    namespace particles. ParticleSystem
  +-- nav/          namespace nav. NavMesh
  +-- ai/           namespace ai. Planner
  +-- shaders/      every .vert/.frag/.comp the engine compiles, and the .glsl they share
  +-- assets/       Sponza and the three scenes scripts/golden.sh pins. Gitignored.

game/               games. Not built by build.sh.
  demo/             what main.cpp was: the actions, the panel, the animation drive
    shaders/        optional. Searched before the engine's, so a name here wins
    assets/         the demo's content, including the default scene. Gitignored.

tests/              the unit suite. Links the hosted sources only; never touches game/.
```

Three properties of that layout are load-bearing rather than tidy:

**A module is what `root` cannot reach, and the build says so.** `substrate-guard layers`
holds three tiers — `core`, then the engine cluster `gfx scene ui sim root`, then the
modules — and fails the build on an include running the wrong way. The cluster is one node
rather than four layers because anything `Engine.h` reaches is bidirectionally coupled with
it and *is* the engine; "`gfx/` depends on nothing above it" is what this line used to say,
and it described an intent the includes never had. The rules are that nothing in `core` or
the cluster may name a module, and that no module may name another; where two modules
appear to need each other, what they share is description and it stays in `scene/`.

The renderer therefore receives spans and PODs rather than subsystems — `gfx::SkinCharacter`,
`gfx::DeformedMesh`, `gfx::ParticleFrame` — and physics reaches it through `gfx/DebugLines.h`,
sixteen bytes that exist so neither Jolt reaches the renderer nor Vulkan the physics world.
Audio never reaches it at all. **This is a link-time property, not a tidiness one**: the
guard is what keeps `Engine.cpp` — an object file every binary carries — from naming a
subsystem and pulling it into games that never asked for one. See "Engine and game
separation" in [principles.md](principles.md).

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
`scripts/build_game.sh <name>` is what produces a program; it records the name in the build
directory's CMake cache, which is why `run.sh`, `golden.sh` and `baseline.py` all kept
the signatures they had when the binary moved.

That check is about direction, and G10 measured what direction alone did not buy: **every
game linked every subsystem, and a game using none of them was 97.8% the size of the demo
using all of them.** G10 was right about the cause and declined the fix — `Engine.cpp.o` is
in every game and it named them all, so the linker pulled Jolt, miniaudio and the rest into
binaries that never asked for them.

That is what `engine/Modules.h` closes. `Engine.cpp` calls `modules::physics->step(dt)`
through a pointer that starts at a do-nothing base, so it makes no call by name and pulls
nothing; the real subsystem arrives only because a module's own translation unit defines
`Engine::physics()`, and a game naming that accessor is the undefined symbol that drags it
out of the archive. **A game links what it names and nothing else.**

The honest witness for it is `nav`, not `viewer`. All thirteen golden cases are `viewer`
runs, so `viewer` deliberately names particles, audio, animation and physics — otherwise
those cases would render nothing and pass. `nav` is the module the golden set does not
need:

```
nm -C build/release/viewer       | grep -c nav::   ->    0
nm -C build/release/battle_arena | grep -c nav::   ->   43
```

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
scripts/setup.sh      [--no-assets]                     # submodules, dependencies, assets
scripts/new_game.sh   <name>                            # scaffold game/<name>/
scripts/build.sh      [debug|release|asan|tsan|clean]   # the engine and the unit suite
scripts/build_game.sh <name> [debug|release|asan|tsan]  # the engine, plus one game
scripts/run.sh        [debug|release|asan|tsan] -- [scene.gltf] [options]
scripts/test.sh       [debug|release|asan|tsan] -- [--gtest_filter=...]
```

A first build is `scripts/setup.sh && scripts/build_game.sh demo && scripts/run.sh`. `run.sh` takes no game
argument: the choice lives in the build directory.

See [tooling.md](tooling.md) for what each configuration can and cannot do, and
[../guides/building.md](../guides/building.md) for the mechanics.
