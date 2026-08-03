---
id: G10
title: A game links only the subsystems it names
arc: G
size: M
verification: golden-11, tests-4, validation, readback, scaffold
---

# G10 — A game links only the subsystems it names

`Engine` gains nullable callback slots and a small table of `void (*)(Engine*)` module
pointers; a subsystem's `.cpp` registers itself into that table, and `Engine.cpp` stops
naming the subsystem's type. This row builds the mechanism and migrates **navmesh** with
it, so the property has something to assert against. Afterwards `Engine.cpp.o` carries no
undefined `scene::NavMesh::*` symbol, and a game that never mentions navigation does not
link `NavMesh.cpp.o`.

## Why

The linker pulls an archive member only to resolve an undefined symbol. Today
[`Engine.cpp`](../../../engine/Engine.cpp) names eight subsystem types and
[`Entry.cpp`](../../../engine/Entry.cpp) three more — the latter only because the inline
`~Engine() { teardown(); }` destroys by-value members in the caller's translation unit — so
every game links every subsystem. Measured on the stripped release demo: **Jolt is 1.91 MB
of 5.36 MB of symbol bytes.** Physics is the payoff; this row is the mechanism that makes
it reachable.

The rule the arc exists to make true, and the one sentence worth keeping:

> **No object file that every game links may name a module.**

## The shape

`Engine.h` holds function pointers taking `Engine*`, which name no module type:

```cpp
namespace core::modules {
inline void noInit(Engine*) {}                      // the unfilled value
inline void (*initNavigation)(Engine*) = &noInit;
}
```

`Engine::init` calls them where its current private `initNavigation()` already sits, so the
documented init order stays the engine's and no game has to know it. **Registration lives
in the module's `.cpp`, never its header** — a file-static registrar in `NavMesh.cpp` runs
if and only if that object file is pulled into the link. In a header, any transitive
`#include` would register the module and relink the dependency.

Frame steps use one small struct, since `void* ctx` plus a function pointer recurs five-plus
times and the Rule of Threes says the third occurrence is when to extract:

```cpp
struct Slot {
    void* ctx = nullptr;
    void (*fn)(void*, float) = [](void*, float) {};   // default: do nothing
    void operator()(float dt) const { fn(ctx, dt); }
    void set(void* c, void (*f)(void*, float)) { ctx = c; fn = f; }
};
```

No-op defaults rather than null checks, so call sites need no `if` and there is one
convention rather than two. `simulate` keeps its order, its comments and its profiler zone
literals.

The dependency runs **module → engine**: `NavMesh.h` may include `Engine.h`. One line
governs every edge:

> **A module may name anything that is always linked. Nothing always linked may name a
> module.**

## Why navmesh is the row that proves it

Measured with `nm --undefined-only` per object file: navmesh is named by
`Engine.cpp.o` **and nothing else**, across two functions (`initNavigation` 5 references,
`bakeNavMesh` 1). Every other candidate needs a second object file changed too. Migrating
it end to end costs two slots and makes the `nm` assertion unambiguous.

## What this must not grow

- A `Component` base, a virtual `update()`, or any second base class in `engine/gfx/`.
  The virtual-function count stays at **two**.
- A templated slot map (`emplace<T>` / `get<T>`). It is a service locator, and it takes the
  template count from two to five to solve a problem module-owned state does not have.
- A `std::vector<Hook>` per phase. Frame order would depend on install order rather than on
  the visible sequence in `simulate`.
- Registration in a module's header, for the include-hygiene reason above.
- A CMake option or an `#ifdef`. Neither is needed and both add a build configuration
  nothing exercises.

## What is deliberately not a module

Measured, not assumed. **Audio** — miniaudio is 546 KB against Jolt's 1.91 MB, a third of
the payoff for the dearest migration on the board (six or seven slots plus two cross edges:
eight references inside `Engine::initPhysics` binding sources to bodies, seven across
`start`/`stopRecording`). Nearly every game wants sound, and keeping it always-linked makes
the physics row *cheaper*, because by the directional rule the physics module may name
`AudioEngine` freely. **`SpatialIndex`** is culling. **`Recorder`** is a core service.
**`InstanceTable`** is named by `GltfScene`, `Scene`, `WorldSave` and `Inspector` — it is
the geometry table itself.

## The sharp edge

The opt-in is implicit: a module registers because something in the game referenced it. A
game whose only use of a subsystem is authored `extras` — no C++ calls — never references
it, so nothing registers and nothing happens, silently. The engine already parses collider
`extras` during scene load, so it can say so:

```cpp
if (sceneHasColliders && core::modules::initPhysics == core::modules::noInit)
    core::Log::warn("scene has collider extras but no physics module is linked");
```

This row establishes the pattern with the navmesh equivalent; each later row adds its own.

## The obstacle, and what I expect to be wrong about

Module object ownership is unresolved. A function-local `static` in the module is simplest
and matches one-game-per-executable, but it is a singleton whose end-of-program destruction
has to be checked against Vulkan teardown order. The alternative is the module allocating
and handing `Engine` a `void*` plus a destroy pointer. Navmesh is the cheapest place to get
this wrong, which is another reason it is first.

The estimate most likely to be wrong is that later rows need slots on `Scene` and
`Renderer`, not only `Engine` — `Scene.cpp` does the G3 node-attachment transforms and
`Renderer.cpp` reads skinning matrices and calls `writeGpuEmitters`. All are data hand-offs
rather than control, so they should convert cleanly, but three classes carrying injection
points is more surface than this row alone suggests.

## Follow-on rows

Opened as this proceeds, not now: **particles** and **animation** (each Engine plus one
`Renderer` hand-off, sharing that mechanism), then **physics** (Engine, the three
`Scene.cpp` transform calls, the ray-query slot for audio occlusion, and the fixed-step
interleave) — the row that takes 1.91 MB off any game that does not simulate.

## Verification

- `scripts/golden.sh` — eleven cases, byte-identical. This row changes no output; a moved
  pixel is a defect. (The card said twelve; the `no-ibl` case has since been retired.)
- `./test.sh debug`, then `./test.sh asan`, then `release`, then `tsan` — each its own
  invocation, plus a new `tests/SlotTests.cpp`.
- Zero validation errors with layers on.
- The property itself, which is a link-level claim and is checked at that level:

  ```bash
  nm -C --undefined-only build/release/libsubstrate.a |
      sed -n '/^Engine\.cpp\.o:/,/^$/p' | grep NavMesh   # must print nothing
  ```

## Reference update

[`principles.md`](../../architecture/principles.md) gains the invariant this row exists to
make true — **no object file every game links may name a module** — and the argument that a
`{ctx, fn}` pair set once at startup is the `extern "C"` struct of function pointers the
`Game` justification already blessed, not the `virtual void execute()` rule 1 bans.

[`architecture/README.md`](../../architecture/README.md)'s load-bearing properties gains
*"`Engine.cpp.o` names no module."*

## Outcome

**Declined, and the decline is the deliverable.** The mechanism above was not built. What
landed is four lines of code, three reference updates and a set of numbers, and the numbers
are the point: this row's own migration is worth 0.64% of a game's binary, and a build flag
that costs no architecture at all is worth 21.7%.

### The premise held, and is worth keeping

Measured on `Release`, stripped, so the two are like for like:

| | Bytes |
|---|---|
| `game/demo` — Sponza, physics, audio, animation, navigation | 6,054,400 |
| A game from `./new_game.sh` — loads nothing, plays nothing, simulates nothing | 5,923,328 |
| **The whole demo game** | **131,072 — 2.2%** |

A game that draws one settings panel carries **97.8%** of what the demo carries. Jolt is
1,811,979 B of symbol bytes in it, miniaudio 895,737 B, fastgltf and simdjson 554,777 B —
about half the binary, none of it reachable. The card was right about the disease.

### Four measurements declined the cure

**1. The row that would have established the pattern is worth 37,877 bytes.** The card
picked navmesh because `Engine.cpp.o` is the only object file naming it, and that is still
true — sharper than the card knew, in fact: it is **one** undefined symbol, `NavMesh::bake`,
not the five references to `initNavigation` plus one to `bakeNavMesh` the card counted, because
those are calls to `Engine`'s own private members. Every `scene::NavMesh` symbol in the linked
demo totals 37,877 B: **0.64%** of the scaffolded game, and about 0.02% of a package whose
asset trees are 253 MB. Two slots and a migration to move six thousandths of what ships.

**2. A linker flag is worth thirty-four times the whole row.** `-ffunction-sections
-fdata-sections` with `-Wl,--gc-sections`: **6,054,400 → 4,743,200 B, −1,311,200 B, −21.7%**,
for 0.09 s of link time (4.11 s → 4.20 s). It reaches *inside* the dependencies the module
table could only have removed whole — Jolt 1.81 → 1.42 MB, miniaudio 0.90 → 0.62, fastgltf
0.33 → 0.20. It is a different property from the one this card wanted, and it answers most of
the same want with none of the design. Opened as
[`chore-the-linker-drops-what-nothing-calls`](../backlog/chore-the-linker-drops-what-nothing-calls.md)
rather than smuggled in here.

**3. Carrying an unused subsystem costs nothing measurable at run time.** On the scaffolded
game: audio, physics and navigation bring-up over an empty scene is ~1 ms of an 11 ms world
build inside a 1.1 s launch, and `--no-physics --set audio.enabled=false` moves that by about
1 ms. Per frame, stepping an empty physics world and updating an audio engine with no sources
is **~4 µs of a 110 µs CPU frame** — 0.114 ms against 0.110 ms median, three runs of 400
frames each arm, inside run-to-run variance. So the cost of carrying what you do not use is
bytes on disk; unused text pages are never resident. The card's *"physics is the payoff"* is
a disk-space payoff and nothing else.

**4. The mechanism is not the indirection the `Game` argument blessed.** The card leans on
principles.md's *"one indirection at the outermost edge"*, and the words doing the work there
are **at the outermost edge**. Module slots sit between the engine and its own parts, where no
boundary is crossed. Three specifics, of which the card anticipated only the first:

- **It lands injection points on three classes, not one.** The card predicted this
  (*"later rows need slots on `Scene` and `Renderer`"*) and it is confirmed by `nm`:
  `Scene.cpp.o` names `scene::PhysicsWorld` three times and `Renderer.cpp.o` names
  `scene::SceneAnimator` four and `scene::ParticleSystem` once.
- **`Engine::physics()` has nothing honest to return.** It hands out a
  `scene::PhysicsWorld&` today. Under module ownership it becomes a cast off a pointer that
  is null in exactly the games the row exists for, and the compiler has nothing to say. The
  card's "module object ownership is unresolved" is this, and it is not a detail — it is the
  public accessor.
- **The opt-in is a diagnostic standing in for a guarantee.** The card names this as "the
  sharp edge" and answers it with a warning per module. A game whose only use of a subsystem
  is authored `extras` links nothing and gets a log line.

### What actually landed, and why its size is the finding

The card's premise names one thing that was a defect rather than a design:

> `Entry.cpp` three more — the latter only because the inline `~Engine() { teardown(); }`
> destroys by-value members in the caller's translation unit

True, and worse than stated — it was five, and the constructor did it too. `Engine::Engine()`
and `Engine::~Engine()` are now defined in `Engine.cpp`. Before and after, from
`nm -C --undefined-only` on `Entry.cpp.o`, whose eight lines mention no subsystem:

| | `Entry.cpp.o` names |
|---|---|
| before | `scene::PhysicsWorld::PhysicsWorld`, `~PhysicsWorld`, `scene::AudioEngine::AudioEngine`, `~AudioEngine`, `scene::SceneLoader::~SceneLoader`, `core::Recorder::~Recorder`, `core::settings::Settings::Settings`, `core::ProfileScope::~ProfileScope` |
| after | **no subsystem type at all** — `Engine::{Engine, ~Engine, init, run, shutdown}` and `core::seedExecutablePath`, in both `debug` and `release` |

**The stripped binary went 6,054,400 → 6,046,208 B.** That is 8,192 bytes, exactly two pages,
and it is alignment rather than content. **That is the fourth measurement and the most useful
one on the card:** the coupling the row blamed is not what pulls the subsystems in. Those
object files were already linked to satisfy `Engine.cpp.o`, which is in every game and names
them all — so nothing short of the full mechanism moves a byte, and the full mechanism is what
the three measurements above declined. The change is kept anyway, because a translation unit
should not reference types it does not mention.

### What the estimate got wrong

`M` was the estimate for building the mechanism. What the row actually cost was a day of
measurement and four lines of code, which no size letter describes — and the card had no
way to know that, because **it argued from a ratio and never from a denominator.** *"Jolt is
1.91 MB of 5.36 MB of symbol bytes"* is true and is the whole case; what it does not say is
that the 5.36 MB is 3% of what a player downloads, so the headline figure is 1% of a package.
The lesson is narrow enough to state: a proportion of a binary is not a proportion of a game.

The second thing it did not predict is that the cheapest tool was one nobody had tried.
`-Wl,--gc-sections` is not in the build, has never been in the build, and buys more than
every migration this card and its three follow-on rows would have produced together. **Reach
for the build before reaching for the design** is now in
[principles.md §1](../../architecture/principles.md#one-crossing-at-the-edge-is-not-a-licence-for-many-inside).

### Where the argument now lives

- [principles.md §1](../../architecture/principles.md#one-crossing-at-the-edge-is-not-a-licence-for-many-inside)
  — *one crossing at the edge is not a licence for many inside*, which is the durable half.
  It bounds the `Game` justification, which was being quoted without its boundary.
- [principles.md, engine/game separation](../../architecture/principles.md#what-the-boundary-is-and-what-checks-it)
  — the out-of-line ctor/dtor and the measurement that it moves nothing.
- [limitations.md](../../architecture/limitations.md#what-stays-declined-and-its-trigger--the-game-arc)
  — the refusal, the numbers, **three triggers** and the list of shapes that stay refused
  whatever the trigger.
- [architecture/README.md](../../architecture/README.md) — the load-bearing property gains
  what it is *not*: the build split checks direction, not size.

**The three triggers**, in short: a game in the tree that ships without a subsystem and has a
stated size ceiling; a subsystem whose bring-up costs something on a game that does not use
it (audio is closest, and is only free because the null device path exists); or a second
engine-side consumer wanting the same shape, which is the Rule of Threes applied to
injection points.

### Verification

Every item its own invocation, on the tree as committed.

- `scripts/golden.sh check release` — **11 of 11**, byte-identical.
- `scripts/readback.sh release` — **9 of 9 bit-identical, plus the lit silhouette**, and the
  12-swapchain resize soak clean.
- `./test.sh debug`, `release`, `asan`, `tsan` — **859 of 859** each.
- `./run.sh demo release -- --headless --locked --audio-null --frames 120 --validation on` —
  **zero errors, zero VUIDs, zero criticals**; one known `VK_LAYER_PATH` warning.
- **The engine builds with `game/` absent.** `game/` moved out of the tree, `./build.sh debug`
  exits 0 and produces `libsubstrate.a` and no runnable binary; the directory restored after.
- **A scaffolded game builds and runs.** `./new_game.sh g10probe`, built `release`, 120
  headless frames with layers on and zero errors. It is what every size figure above is
  measured on, and it was deleted afterwards.
- **The link-level claim, in D9's style.** `nm -C --undefined-only` on `Entry.cpp.o` in both
  configurations names no subsystem type, where before it named five. Stated as a before/after
  above because the check is the *absence*, and an absence is only evidence beside what it
  replaced.

One thing to expect when re-running this: **`vkCreateDevice failed: VK_ERROR_DEVICE_LOST`
took out one random golden case in three of five runs today, with and without any change to
the build.** It is environmental. Two of the three occurred while the `--gc-sections`
measurement was in the tree and were briefly attributed to it; the unmodified tree then did
the same thing, which is what corrected that. Re-run rather than conclude — and the note is
on the chore card too, because that is where somebody will hit it next.
