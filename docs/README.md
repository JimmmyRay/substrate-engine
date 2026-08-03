# Substrate Documentation

A Vulkan 1.3 deferred PBR game engine. It loads glTF, shades it with per-sample MSAA or
TAA, simulates rigid bodies and characters, mixes spatial audio, and draws an
immediate-mode interface over the result.

## Architecture

**Start here.** [architecture/](architecture/) is the reference: what the engine is, how
it is put together, and what it deliberately does not do.

| Document | Covers |
|---|---|
| [Overview](architecture/README.md) | The shape of the engine, module layout, what it does |
| [Principles](architecture/principles.md) | The rules the code is held to, and the scale dispositions |
| [Rendering](architecture/rendering.md) | The frame in order, targets, passes, pipelines, shader variants |
| [Systems](architecture/systems.md) | Instance table, glTF, animation, particles, physics, audio, input, UI |
| [Tooling](architecture/tooling.md) | Build configs, profiling, capture, golden suite, validation |
| [Limitations](architecture/limitations.md) | Every caveat, stated limit and deliberate omission |

## The board

Every piece of planned work is a card in [kanban/](kanban/), and **the directory the card
sits in is its status** — `backlog/`, `ready/`, `in-progress/`, `verifying/`, `blocked/`,
`done/`. There is no second copy of the status to disagree with it, and moving a card is
`git mv`, so its history is the flow log.

A card is the whole record of its row: what it is, why it is worth doing, what would prove it
done, and what it cost. Four arcs, distinguished by what a row changes:

| Arc | A row here | Where |
|---|---|---|
| **C** | Adds a capability the engine does not have — object lifetimes, spatial queries, persistence, streaming, navigation | [kanban/](kanban/) |
| **D** | Changes neither capability nor reach; makes what exists look like one engine. Each verified by the golden set being byte-identical, since a D row that changes output has failed | [kanban/](kanban/) |
| **G** | Changes how a game *reaches* a capability that already exists — the module boundary, a scene tree, shaders per draw, settings by name | [kanban/](kanban/) |
| **P** | Correctness judged in **texels**: a texel the artist authored is the texel presented | [kanban/](kanban/) |

[kanban/arcs.md](kanban/arcs.md) argues each arc and states the boundaries that keep two of
them from building the same thing twice. [kanban/order.md](kanban/order.md) is what to do
next, which directories cannot express.

These were once three roadmap documents. They are gone, retired the way the S arc was —
*the answers move into the reference and the plan is deleted rather than left to rot*. Each
row's argument went to its card; the rules the arcs held themselves to went to
[principles.md](architecture/principles.md), the refusals and their triggers to
[limitations.md](architecture/limitations.md), the verification protocol to
[tooling.md](architecture/tooling.md), and the call-site sketches to
[making-a-game.md](guides/making-a-game.md).

| | Answers | Goes stale |
|---|---|---|
| [architecture/](architecture/) | What the engine *is* | No — it is the reference |
| [kanban/](kanban/) | What is being done to it, and where that work stands | It cannot — a card's directory is its status |

## Guides

- [Making a game](guides/making-a-game.md) — the `Game` interface, the loop, and what a
  game reaches, plus the call site the arcs designed backwards from. Written one row at a
  time, and explicit about what is not built yet.
- [Building](guides/building.md)
- [Profiling](guides/profiling.md)

## Configuration

`substrate.json` at the repo root is the source of truth **for settings**. Regenerate a
fully populated one with `./build/debug/demo --write-default-config`; it is generated from
the settings table, so it cannot claim a default the build does not hold.

Every key is optional — an absent key keeps its built-in default, so an old config still
loads against a new build. Command-line flags override the file for the things that vary
per invocation; `--help` lists them all.

**`--dump-settings` prints every setting there is, its value, and where that value came
from** — `default`, `config`, `game` or `cli`. That last column is the one that answers
"did my edit take". `--dump-settings=json` is the same thing for a bug report.

What is *not* in the file is deliberate: a scene, a sun, a mix graph, gravity and an
exposure are authored decisions and live in the game's `GameSetup`, not in a file a user
can edit — see [principles.md §7](architecture/principles.md#7-a-setting-is-a-property-of-the-person-running-the-program)
for the rule that decides which, and
[architecture/tooling.md](architecture/tooling.md#configuration) for the table itself.

## Core decisions

| Area | Choice | Why |
|---|---|---|
| Language | C++20 | GCC 12.3 on jammy lacks `std::expected` / `<print>`; no C++23 feature was needed |
| Vulkan | 1.3 via volk | Dynamic rendering + synchronization2; volk drops the loader trampoline |
| Shaders | GLSL → SPIR-V | `glslangValidator --target-env vulkan1.3`, compiled at build time |
| MSAA | Per-sample deferred lighting | Averaging is only valid on radiance, after the nonlinear BRDF |
| glTF | fastgltf | Fastest available, and `MappedGltfFile` already does real `mmap` |
| Allocator | VMA | Sponza is ~70 images plus buffers; suballocation stops being optional |
| Architecture | No abstraction layers | No RHI, no render graph, no material system. Two base classes in `engine/`: one Jolt's API demands, and `Game` at the outermost edge |
| Engine / game split | `engine/` is a library, `game/<name>/` is an executable | `./build.sh` builds no game and produces no runnable binary, so a leak across the line is a link error rather than a code review |
| Asset and shader ownership | `engine/{shaders,assets}/` and `game/<name>/{shaders,assets}/` | The engine keeps what its golden suite pins; everything else is the game's. A game shader is searched before the engine's, so it overrides by name |
| Shader variants | Specialisation constants, rebuilt on change | A disabled feature is dead-stripped, not branched around. One live variant per pipeline — no cross-product, and no cache until something selects one per draw |
| Descriptor layouts | Hand-written, reflection-*checked* | Generating them from SPIR-V would hide what is bound; asserting they agree catches a mismatch without the indirection |
| Config | `substrate.json` (rapidjson) | Source of truth; CLI overrides only per-invocation values |
| Build / run | `./build.sh`, `./run.sh`, `./test.sh` | Encode config-specific build dirs and the `setarch -R` TSan workaround |
| Tests | googletest, over the hosted sources only | A test framework is a solved problem. Linking no Vulkan is what lets the suite run under TSan, where the renderer cannot |
| Benchmarks | `scripts/baseline.py`, reading the trace | The `GPU @` log line is one frame; a table built from it is a median of arbitrary frames |

## The MSAA problem, in one paragraph

MSAA rasterizes coverage at N sample points but runs the fragment shader once per pixel.
Deferred shading moves lighting to a fullscreen pass, by which time a pixel is N samples
that may straddle a geometric edge — and lighting is a *nonlinear* function of surface
attributes, so `average(light(x)) != light(average(x))`. Resolving the G-buffer before
lighting averages normals into directions no surface faces and depths where no geometry
exists, producing halos exactly along the edges MSAA was meant to fix. Substrate therefore
keeps the G-buffer multisampled and shades every sample in the lighting pass, averaging
the resulting radiance. See
[architecture/rendering.md](architecture/rendering.md#msaa-and-the-deferred-problem).
