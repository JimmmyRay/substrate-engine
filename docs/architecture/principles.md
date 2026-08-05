# Principles

The rules this codebase is held to. They are constraints rather than preferences, and the
engine's shape is mostly a consequence of them.

---

## 1. No abstraction layers over Vulkan

Substrate targets Vulkan and only Vulkan. Passes record their own commands inline and call
`vkCmd*` directly.

**Do not build:**

- A `RenderPass` base class, or any `virtual void execute()`.
- A render graph with declared reads/writes and automatic barrier insertion.
- A `ResourceManager` / `TextureCache` owning GPU resources behind handles.
- A `RenderDevice` / `IRenderer` wrapping `VkDevice` "in case we add DX12".
- A material class hierarchy, or templated `Buffer<T>` / `Pipeline<V, P>` machinery.

**Do instead:**

- Render targets are named `GpuImage` members of `Renderer`, created in
  `createRenderTargets()`. Ten named members is fine; it reads top to bottom. Revisit only
  past roughly fifteen.
- A pass is a method — `recordShadows(cmd, slot)` — not a class.
- Prefer one longer function to several small ones you must jump between. A 200-line
  `recordShadows` that reads top to bottom beats six methods you have to chase.
- Reach for a third-party library that solves a solved problem. VMA, fastgltf, rapidjson,
  stb, googletest, Jolt, miniaudio and meshoptimizer are dependencies, not architecture.
  What to avoid is indirection *we* write over Vulkan.

A second graphics backend would be the moment to abstract. Until then, don't.

### The virtual function count

The engine defines **three** base classes, and they are allowed for three different reasons.

The first is a file-local `PhysicsDebugRenderer` in `scene/Physics.cpp` implementing two
of `JPH::DebugRendererSimple`'s methods, because that is how Jolt hands out the wireframe
of a convex hull or a triangle mesh — shapes with no parameters a procedural outline
could be built from.

**A dependency's API demanding one override is not an abstraction we wrote**, which is the
distinction this rule is actually drawing. Everything else Jolt normally asks you to
derive — the broad-phase layer interface, the two layer filters, the job system, the temp
allocator — has a concrete table-driven implementation shipped in Jolt itself, and those
are what the engine uses.

The second is `Game` (`engine/Game.h`), and that argument does not cover it. The one that
does is the one the engine/game separation section below already made about a module
boundary spelled as an `extern "C"` struct of function pointers: *one indirection at the
outermost edge, not a layer over Vulkan.* A base class with empty-defaulted methods is the
same indirection, in the same place, with better ergonomics — **three** virtual calls per
frame (`frameUpdate`, `fixedUpdate`, `drawUi`), none in any inner loop, none between the
engine and Vulkan. The rest are not per-frame at all: `declareSettings`, `configure` and
`init` run once each at startup, `shutdown` once at the end, and `save` and `load` only when
a save happens. D17 is what made the run-once group three rather than two, and the argument
for the seventh method is on that card: the split between *adding* a row and *writing* one
has to be enforced by which method you are in, or the ordering that makes a game's row take
the same four sources as an engine's is a convention rather than a mechanism. So:

> **`Game` sits at the outermost edge, and that is what earns it.** It was the only base
> class the engine defined until G18; `scene::Camera` is the second, on its own argument below.

The ban is otherwise untouched. A `RenderPass` with `virtual void execute()`, an
`IMaterial`, a `RenderDevice` wrapping `VkDevice` — all still refused, and a second base
class appearing anywhere inside `engine/gfx/` is still the thing to stop.

**The third arrived, and it is `scene::Camera` (G18).** The reason is the same shape as
`Game`'s and had to be as specific: a camera controller is *policy about input*, and the engine
was asserting one unconditionally — every game got nine `Camera.*` actions including WASD, listed
in the rebind menu of a game that had no flycam. Three empty-defaulted virtuals (`activate`,
`deactivate`, `update`) make the controller opt-in, and **the base doubles as the null camera**:
"looks at the scene and takes no input" is simultaneously its definition and what a null object
has to be, so there is no `NullCamera` and `Engine::camera()` can never be null. None of the three
is called in an inner loop — `update` is once a frame, the other two only when a game changes
camera.

It also sits at an edge rather than inside one. `scene::Camera` is data the renderer reads; the
prohibition names `engine/gfx/`, and its rationale is indirection over Vulkan, neither of which
reaches a camera controller. **The count to watch is now three**, and a Jolt `ContactListener`
remains the one anticipated fourth, standing on the first justification rather than needing a new
one.

### One crossing at the edge is not a licence for many inside

`Game`'s justification is quoted often enough that it needs its boundary stated, because
G10 tried to borrow it and the borrowing is where the argument breaks. The words that do
the work above are **at the outermost edge**, and they are load-bearing rather than
decorative:

> **An indirection is paid for by the boundary it crosses, not by the syntax it is
> written in.** `Game` is one crossing, at the one place the engine stops and something
> else begins. A function pointer between the engine and its *own* subsystems crosses no
> boundary at all — it is the same program on both sides — so it earns nothing and costs
> the same.

The concrete proposal that failed this test was a table of `void (*)(Engine*)` module
slots, filled by a file-static registrar in each subsystem's `.cpp`, so that `Engine.cpp`
could stop naming the subsystem's type and the linker would leave the object file out of a
game that never asked for it. Nothing in it is a base class, nothing is `virtual`, and the
`extern "C"` struct of function pointers the section above blesses is exactly what it looks
like — which is why it is worth being precise about why it is not the same thing:

- **It is not one crossing.** `Engine::simulate` is only the first site. The subsystems
  worth removing are also named by `Scene.cpp` (three physics transform calls) and
  `Renderer.cpp` (skinning matrices, `writeGpuEmitters`), so the mechanism lands injection
  points on **three** classes rather than one, and each new module adds a slot to all of
  them that apply.
- **It replaces a checked relationship with an unchecked one.** Today `Engine::physics()`
  returns a `scene::PhysicsWorld&` that exists. Under module ownership it is a cast off a
  pointer that is null in exactly the games the mechanism was built for, and the compiler
  has nothing to say about it.
- **It makes the subsystem set a property of what symbols a game happened to reference.**
  That is the opt-in, and it is implicit: a game whose only use of physics is authored
  collider `extras` never names the type, so nothing registers and nothing runs. The
  proposal's answer was a warning per module, which is a diagnostic standing in for a
  guarantee.

The measurements that decided it are on
[G10's card](../kanban/done/G10-a-game-links-only-the-subsystems-it-names.md), and the
short version is that the rule the mechanism existed to make true — *no object file that
every game links may name a module* — is worth **0.64%** of a scaffolded game's binary on
the row that would have established it, against **21.7%** available from a linker flag that
costs no architecture at all. **Reach for the build before reaching for the design.**

### The template count

Two: `AveragingBuffer` and one glTF helper. Neither is machinery over Vulkan.

---

## 2. The Rule of Threes

**The third time you need a semi-complex pattern, refactor it.** Two occurrences are a
coincidence; duplicate freely. The third is evidence of a real abstraction.

### Choosing scope

Use the **narrowest scope every current caller can reach**. Walk this list top-down and
stop at the first rung that works:

| Scope | Use when |
|---|---|
| Local function | All uses live inside one function |
| Private method | Uses are confined to one class |
| Public method | Callers exist outside the class |
| Global function | Callers span modules, and it's stateless |
| Global class | It needs its own state or identity |

**Scope is promoted by callers, never by count.** A pattern used ten times inside a single
function stays a local function. It moves up a rung only when a new caller genuinely
cannot reach it where it lives — and then only far enough to reach that caller.

### What counts as "semi-complex"

Roughly: more than a couple of lines, or one line carrying non-obvious intent — a magic
constant, an ordering dependency, a subtle cast, an easy-to-get-wrong formula. A repeated
`i++` is not a pattern. A repeated three-line buffer alignment calculation is.

### Anti-patterns

- **Abstracting at two.** Speculative generality. Wait for the third.
- **Over-promotion.** Extracting straight to a global helper when a private method would
  do. Every rung up is surface area someone must maintain.
- **Refusing to promote.** Copying an existing private helper into a second class instead
  of moving it up one rung.
- **Bundling.** Three similar-looking patterns that differ in intent are three things, not
  one parameterized thing with a `mode` flag.

### Where it has actually fired

Each of these reached three callers and was promoted exactly one rung:

- `transitionImage` (13 uses) and `setViewportScissor` (5) — helpers over `vkCmd*`.
- `GraphicsPipelineDesc` — a parameter struct, 4 uses.
- `singleImageSetLayout` — `fontSetLayout` and the SSR composite layout collapsed into it
  when the decal pass became the third user of a one-sampled-image layout.
- `core/Json.h` — five JSON readers that reached three callers. The four readers with one
  caller each were deliberately left where they were.
- `gltfJsonSpan` — moved to `core/Json.h` on its third caller. The two comments that had
  predicted the move now record it.
- `SUBSTRATE_KEY_LIST`, an X-macro generating an enum, a name table and a `static_assert`
  per row — three uses at birth, which is the rule met rather than anticipated.

---

## 3. Designing for scale

Substrate is a game engine. Sponza is a test scene. **A property of the test scene is not
an engineering argument**, and "the sample scene only places four" is not a reason to ship
a limit that fails silently at five.

A capacity, cost or fidelity limit may only be deferred if the deferral is recorded as one
of three dispositions, each carrying an obligation:

1. **Generalized** — one code path at every scale. *Obligation:* no fixed capacity that
   can be exceeded silently. A limit is either sized from data, or it degrades by a stated
   policy and reports that it did.
2. **Tiered** — two paths with a stated crossover. *Obligation:* the crossover is measured
   rather than guessed, and the selector is data rather than a build flag. The Rule of
   Threes still applies: the second path is not written on speculation, but the crossover
   and the selector are decided now.
3. **Delegated** — the engine states the limit and a specific game implements past it.
   *Obligation:* name the architectural properties that keep it implementable, and
   **verify each holds today**. A delegation with no named, checked properties is a
   handwave wearing a disposition's clothes.

> "The test scene doesn't need it" is not a disposition. It is a statement about the test
> scene.

Content-conditional items — terrain, water, networking — are the one legitimate exception,
and they say so in their own words rather than borrowing one of the three.

### How each disposition was discharged

| Limit | Disposition | What was done |
|---|---|---|
| Light count | Generalized | `kMaxLights` deleted, and since D21 a game states nothing at all: the buffer starts at `gfx::kDefaultLightBudget` and grows. Over it, `updateLights` ranks by `lightImportance()` for the one frame it is short, records what the view wanted, and `growLightBuffer` reallocates before the next |
| Punctual shadow atlas | Generalized policy | Layers go to the highest-importance casters by first fit; the rest are counted and reported. Still a real ceiling — every layer is a full scene re-render |
| Draw submission | Generalized | One `vkCmdDrawIndexedIndirect` per pass. Recording is O(passes), not O(draws) |
| Particle pool | Generalized | Sized from `sum(rate x maxLifetime)` over the **live** emitters and grown to it by `Engine::growParticles`, which resizes the system and the renderer's buffers together (C40). One stated ceiling at 65,536, because the sort key packs distance and slot into 32 bits |
| Collider count | Generalized | Nobody states one since D21. The world sizes itself from the colliders it has and `PhysicsWorld::grow` rebuilds Jolt's fixed-size world at double the size, carrying every handle across |
| Fixed-step catch-up | Generalized | Past `maxStepsPerFrame` the remaining steps are dropped, counted and reported — the spiral of death answered by a stated policy |
| Particle lighting | Generalized | Per particle, reading the same budgeted light list surfaces read |
| Physics threading | Tiered, measured | Single-threaded wins below ~150 bodies, breaks even at 142, loses 2.02x at 526. Selector is `physics.workerThreads` — a row, and it stayed one, because a thread count is a property of the machine — and the default is 0 because determinism needs the count fixed |
| Audio stream vs decode | Tiered, measured | 5 seconds, from measured cost: 0.96 us per voice per step to stream, 375 KiB and ~0.25 ms of load per second to decode. An explicit `stream`/`decode` beats the threshold |
| Acceleration structures | Tiered | Three tiers, split by *how* a thing moves: static is one baked-transform BLAS; rigid is BLAS-per-primitive in model space, shared between movers and placed by a TLAS instance; deformed is BLAS-per-instance with `ALLOW_UPDATE`. All under a TLAS rebuilt every frame. Selector is the instance's flags |
| TAA motion | Tiered in cost | Static geometry reprojects from depth; dynamic instances write a correction. At zero dynamic objects the cost is a clear |
| Decal count | Delegated, verified | `decal.frag` is unchanged by a switch to rasterised boxes — only the vertex source and depth test differ |
| Many-lights culling | Delegated, verified | The light list is already a storage buffer whose length the shader reads, and the budget is explicit data |
| Texture residency (the scene's) | Delegated, verified | All four properties hold: bindless indices, a stable slot free list, a transfer queue with a fence, and a reserved fallback slot. Still Delegated, and the row below is why the two are now listed apart |
| Resident images (`gfx::ImageTable`) | Generalized — *was Delegated* | P1. `kMaxOverlayImages` is **deleted rather than raised**: the array is a `std::vector` with a free list and a generation, the descriptor set doubles, and the only ceiling left is the device's own limit on sampled images in one set. A load past it is refused and reports. `GltfScene`'s array keeps its own disposition above -- nothing outside that file calls `acquireTextureSlot`, so unifying them would be promotion with no caller |
| Sprite count | Generalized | P4. Dense arrays with a free list and a generation; the per-frame storage buffer doubles behind a `vkDeviceWaitIdle` and **logs the reallocation**, the way the geometry buffers do. There is no cap and nothing to refuse: 10,000 sprites are one instanced draw costing 0.053 ms, and the CPU cost is one `memcpy` because the sort runs on a change of *order*, not on a change of position |
| Octree / CPU spatial queries | Delegated | The GPU tests all instances in parallel in 0.010 ms; a tree for the renderer would be a structure with no caller |
| Particle collision | Delegated, verified | The test is one branch inside the simulate dispatch, and position and velocity live in one storage buffer a physics query could be handed |
| Multi-threaded recording | Delegated | After indirect submission, recording scales with passes rather than draws, so threading it buys far less than it would have |
| Networking | Content-conditional | No target exists. See [limitations.md](limitations.md) for the six properties audited |

The *Octree / CPU spatial queries* row carries a caveat the others do not. Its 0.010 ms is
measured on a scene of about a hundred draws, which is the largest this repository can
currently generate — so it establishes that the GPU cull is free at that size and nothing
about where it stops being free. the C arc's **C13** builds the generator
that varies node count independently and is what re-tests this line. If it shows the cull
ceasing to be free, the row that answers it is **C9**, which builds the tree against a query
that wants it — not a tree baked in advance for a caller that does not exist yet. The
disposition stays Delegated until there is a measurement that moves it.

---

## 4. Each pass records its own commands inline

In a `recordX(cmd, slot)` method, the way `recordGBuffer` / `recordLighting` /
`recordTonemap` do. A pass is a method, not a class implementing an interface.

---

## 5. Prefer a longer function to a new indirection

See rule 1. This is the same rule stated for the inside of a file rather than the outside.

---

## 6. A dependency that solves a solved problem is not an abstraction

VMA, fastgltf, rapidjson, stb, googletest, Jolt, miniaudio, meshoptimizer and glfw are all
fine. Layers *we* write over Vulkan are the thing to avoid.

**meshoptimizer is the most recent one, and C17 took it on exactly this rule.** That card
left the choice open between a submodule and an in-tree quadric edge collapse and asked for
the decision to be taken as a dependency decision rather than by default. Mesh
simplification is the definition of a solved problem; a simplifier takes an index array and
returns a shorter one, so it is neither indirection nor over Vulkan; and the only thing
reimplementing it would buy is ownership of the bugs.

Where a dependency's header is large or invasive it goes behind an `Impl` pointer —
`PhysicsWorld` and `AudioEngine` both do this, for the same reason and to the same effect:
keeping a 100k-line header off every translation unit that touches a scene. That is
containment of a dependency, not an abstraction over a concept.

---

## 7. A setting is a property of the person running the program

`substrate.json` is for the user. Everything else the program needs to know is code.

The test is **not** "would someone want to change this" — someone wants to change
everything. It is:

> **Is this a property of the person running the program, or of the program?** A value
> that different users legitimately hold different values for is configuration. A value
> with one correct answer that the game's author chose is not.

A scene path, a sun, a mix graph, a gravity vector, an exposure and the tonemap curve that
balances it fail that test. There is no user for whom a different value is correct, and a
user who edits one has not configured anything — they have broken it. So they are C++ in the
game's `GameSetup` ([Game.h](../../engine/Game.h)), where nobody can edit them into nonsense
and no key can silently do nothing.

That leaves three homes, and which one a value gets is decided once:

| Home | For | Example |
|---|---|---|
| **A row in the settings table** | Preferences and machine properties — window size, MSAA, quality toggles, volumes, sensitivities, bindings | `render.ssao`, `camera.fovDegrees` |
| **`GameSetup`, in game code** | Authored decisions | the scene, the sun, the mix graph, gravity, the simulation step |
| **A command-line flag with no JSON key** | Developer controls a *tool* drives | `--locked`, `--debug-view`, `--validation`, `--no-profiler`, the whole capture block |

The third exists because a benchmark harness is not "the person running the program", and
because a tool that depends on determinism must **pin** it rather than inherit it —
`scripts/golden.sh` and `scripts/baseline.py` pass `--locked` for exactly that reason.

**The third home is the one that grows without a rule, so the rule is stated for it:**

> **A named flag is correct only for a developer control with no JSON key.** A preference
> gets a row in the first home and reaches the command line through `--set <key>=<value>`,
> which every row has.

Thirty-six flags existed to assign a settings row, and the next preference added would have
argued for a thirty-seventh by symmetry — while some sixty rows had no flag at all, so a
reproduction step naming one could not be followed without editing the config file the
report depended on. One door for every row answers both, and it needs no per-row line
anywhere. The one deliberate exception is a switch a **runtime key already spells**:
`--no-ssao` beside F8 is one idea spelled twice for one reason.
[tooling.md](tooling.md#any-setting-from-the-command-line-and-when-a-named-flag-is-correct)
is the mechanism and the
twelve flags this retired.

### The audit, and the rule the first home did not have

The rule above was stated once and then the *first* home kept absorbing things anyway, because
a row is the easiest thing to add: it is one line, and it comes with a parser, a flag, a panel
widget and persistence for free. **Convenience is not the test**, and two findings made the
drift measurable rather than a difference of opinion. `ProfilerConfig` existed as a plain
struct whose comment documented a *deliberate* divergence between its default and the settings
default for the same field — two spellings of one value, the exact failure the table was built
to prevent, now caused by the table. And `benchmark.exitAfterFrames` gave a module named for
the test harness a key in the user's config file.

D14 walked the list a row at a time. **Thirty-nine keys failed and left**, and every one of
them is in `settings::removedKeys` with a sentence naming the flag or the field that replaced
it — *"is now `--no-physics`; it is a developer control, so it has no config key"* is the
shape, and a row that left silently would be this section's first prohibition committed by
the fix for it. What survives is fifty-seven rows in **eight** modules — `window`, `render`,
`input`, `camera`, `physics`, `audio`, `ui` and the keyless `engine` — and the unit suite
walks every row and refuses a ninth, so the drift cannot recur unnoticed.

> **A developer control never gets a key.** Not the validation layers, not a shader recompile
> loop, not a debug overlay or a debug window or a debug draw, not the profiler, not the
> recorder, not the log, not a bake step and not a frame limit. Every one of those was a row.

Three lines the audit drew are worth stating, because each is a place two people would
reasonably disagree:

- **`audio.enabled` stays and `physics.enabled` goes.** Muting is a preference — a player
  legitimately wants silence. Skipping the physics world is a *measurement*: it is how a run
  attributes frame time to the solver, and it is `--no-physics`.
- **A budget is a floor, and the engine sizes its own pools.** `lightBudget`,
  `particleBudget`, `bodyBudget` and `voiceBudget` are `GameSetup` fields, and since C40 none
  of them is a ceiling: each subsystem grows itself and says so, so the number only decides
  how much is allocated before that first happens. They were refuse-and-report until a game
  reached for `bodyBudget` to size something that was not a body at all -- a field whose first
  real caller uses it wrongly is a field that should not need reaching for. A user who lowered
  `lightBudget` did not configure anything, they made lights vanish. `physics.workerThreads`
  and `audio.decodeBudgetBytes` stayed rows on the opposite argument, and it is a real one:
  those are properties of the *machine*, and two users legitimately hold different values.
- **`render.debugFont` and `render.debugFontHeight` were on the list and stayed.** The name
  says *debug* because the frame-stats overlay asked for it first, but one atlas serves the
  overlay, the panel, the inspector and every string a game draws — so a typeface and a size
  are a property of whoever is reading them. `ui.scale` magnifies the embedded bitmap font in
  integer steps only, and [limitations.md](limitations.md#only-integer-ui-scaling) already
  names a TTF here as the answer for anyone that does not fit. Removing them would have taken
  an accessibility escape hatch out of the file to tidy a module boundary.

**Emptying a module hands it to a game**, which is the audit meeting the namespace rule below.
`scene`, `profiler`, `record`, `logging` and `benchmark` name no engine row now, so a game may
declare into them — while every key they *used* to hold is refused by name out of
`removedKeys()`. That is also why `logging.categories` left the file with the three `logging`
rows beside it rather than staying behind as an aggregate: a module the table does not claim
while `Config` still parses a key out of it is two owners of one JSON section.

**The first home is no longer a fixed array, and it did not buy that with the property that
made it worth having.** A row can be added at run time, because difficulty, subtitle size
and colourblind mode pass the test at the top of this section as squarely as MSAA does and
none of them belongs to the engine. **The first home is therefore no longer the engine's
alone**: `Game::declareSettings` adds a game's own rows to the same table, and they load from
`substrate.json`, save back to it, dump, clamp, take `--set` and draw in a generated panel
without any consumer learning that a game exists. The alternative was a game hand-rolling a
second config file — a second parser, a second save, a second panel, and two files a user has
to know about — or shipping without settings at all.

Two rules bound it, and both follow from the sentence at the top of this section rather than
from convenience:

> **A game owns every module the engine does not name.** The unit is the module, not the key,
> because the key *is* the JSON path — a `render.` key a game invented would be
> indistinguishable from an engine row in the file, in the panel and in the written default
> config, and an engine release adding that key later would take over a value the user wrote
> for the game.

> **A value the file holds and no row claims is refused and kept.** Refused, because a key
> that parses and does nothing is this section's first prohibition. Kept, because a game
> dropping a setting must not cost the user their answer to it, and one game's section is not
> another game's to delete. The save merges into the file it read.

[tooling.md](tooling.md#a-game-declares-its-own-rows) is the hook, its ordering and the rest
of the refusal ladder.

The rule that made a run-time row safe to build in the first place:

> **A row known at compile time keeps a compile-time handle. A design where every access
> becomes a string lookup passes "not a fixed array" and fails the engine.**

So the engine's rows stay a `constexpr` array indexed by an enumerator, `core::options::…`
stays `constexpr`, and a typo stays a build error. A declared row's *id* is assigned at run
time and its handle is returned from the call that declares it — still typed, so
`set(handle, true)` on an integer row still does not compile. What changed is where an id
comes from, not what a handle is worth. The growth hazard that comes with it is answered in
[tooling.md](tooling.md#the-table): every reference the table hands out lives in a container
that does not move what it already holds, and the schema freezes before anything reads rows
by name.

### Four sources answer one row, and the order between them follows from the rule

A row in the first home can be answered by the table's built-in, by the game's `configure`,
by the user's `substrate.json` and by the command line. Which of them wins is not a
preference, because the sentence at the top of this section already decides it: the file and
the flags are the person running the program, and the game is the program.

> **A game's default loses to the user's file, and the user's file loses to the user's
> command line.**

| Source | What it is | Loses to |
|---|---|---|
| `default` | the built-in, from the table's own row | everything |
| `config` | `substrate.json` | `game`'s *override*, and the flags |
| `game` | `Game::configure` | the flags |
| `cli` | a flag, or `--set <key>=<value>` | nothing |

The middle row is the one that needs two doors rather than one, because a game holds two
different intentions and a `bool respectUser` parameter at the call site reads as neither:

- **`settings.setDefault(handle, value)` — *my answer unless you said otherwise*.** Writes
  only where nothing has claimed the row, so the user's file wins. This is what almost every
  game wants, and it is the one line the guide teaches.
- **`settings.set(handle, value)` — *my answer regardless*.** Over the file, not over the
  flags. A fixed-resolution game turning TAA off, or a benchmark harness pinning a value,
  does this deliberately.

Both record `game` in the source column, so a user asking why their file did not take gets
the same answer either way.

**The order is the order the doors are opened in, not a rule inside the setter.**
`Engine::init` reads the file, calls `Game::configure`, then applies the flags. A rule in
the setter would have to refuse a panel toggle or a keypress — both of which write as
`game` — over a row some flag set at startup, and a runtime write beating a startup one is
the whole reason the table binds live fields. `setDefault`'s test is the one predicate an
ordering cannot express, and it compares against `default` *specifically* rather than
"anything lower than `game`", so a tier inserted below it later is not silently overwritten
either.

That ordering is also where this was found to be false. `configure` used to run *after*
the command line, which made a game's `set` beat the flags — the exact inversion of what
this document, `Game.h` and the guide all claimed — and it was invisible because no game in
the tree wrote a setting from `configure`. Moving one call is the whole of the fix, and it
is why the ordering is now stated as the mechanism rather than as an outcome.

### A world unit is derived or it is a row, and never a literal

The rule the audit for game-specific code in `engine/` bought. That audit found no code
across the line — no include, no asset path, no node or material name, no key bound and no
panel drawn — and found instead that the *values* had leaked where the code had not.
`ssaoRadius = 0.5f` said so in its own comment: *"Sponza is roughly 20 units across, so
this is contact-scale."* Not a row, not derived, and not even the right measurement —
Sponza is 29.8 on its longest axis and 37.1 across the diagonal.

> **A world-unit constant in the engine is derived from the scene, or it is a settings row.
> A length written as a literal in a header is neither.**

Which of the two a value gets is **not** decided by whether it is a length. It is decided
by what the right answer is a function of:

- **A function of scene size, and of nothing else — derive it.** `particleSortRange`
  quantises the whole visible depth span; `fogBaseHeight` is the scene's floor;
  `Camera::moveSpeed` and `Camera::nearPlane` are what make crossing a room take about as
  long whatever the room is. There is no user answer to give, so none of them has a key.
- **A function of the content — make it a row.** An occlusion hemisphere is contact scale
  at every scene size: scaling it with the bounds would give a warehouse metre-deep creases
  and a doorknob none. So are a depth bias and a wall's thickness. `render.ssaoRadius`,
  `render.ssaoBias` and `render.ssrThickness` are rows for that reason, and they were
  literals nothing could reach until they were.

**The failure to avoid is a row over a value something recomputes.** A key bound to a field
`setScene` or `frameBounds` rewrites is a setting that appears to work and reverts on the
next scene load — the same class of failure as a flag nothing reads, and the reason a
`Scene` source tier was considered and declined: with nothing in the engine deriving a
*row*, it would have been a fifth precedence tier with no writer.

The corollary is about bounds rather than defaults, and rule 3 already states it: **a
row's ceiling is bounded by what the value is for, not by how big the test scene is.**
`render.ssrMaxDistance` stopped at 64, under twice Sponza's own diagonal, while
`render.shadowDistance` and `render.fogMaxDistance` — the same kind of number — stopped at
500. That is a property of the test scene wearing an engineering argument's clothes.

**Two things a config file must never do**, both of which it did before this rule was
applied:

- **Carry a key that parses and does nothing.** Every key that has ever left the file is in
  `settings::removedKeys` with the sentence saying where it went, and a file still carrying
  one gets that sentence. Silence is the failure; a message is the fix.
- **Hold two spellings of one value.** The name of a setting already existed as a string in
  the file, again in the parser, again in the flag that overrode it and again in the panel
  that drew it. The table makes that one place; [tooling.md](tooling.md#configuration) is
  the mechanism.

**Both prohibitions bind the command line too, and neither did until they were stated for
it.** The table removed the silent no-op from the file's door and left it standing in the
other: `--tonemap reinhardt` started the program in ACES and said nothing, which is *a key
that parses and does nothing* committed by the parser rather than by the file. So the rule
in full:

> **A value spelled by name has exactly one list of names, and a name that list does not
> hold is refused with the legal values printed — whichever door it arrived through.**

The value is refused and the run is not: the setting keeps what it held, and the refusal
says which one that is. A hard exit over a typo in a hand-edited config file is a different
kind of damage from the one being fixed, while a sweep that silently measured the wrong arm
produces a number nobody can tell is wrong. The mechanism, the enums it covers and the
build-time totality guard beside each list are in
[tooling.md](tooling.md#names-and-the-parse-that-refuses).

That table is **not** the property registry the section below still refuses, and the
distinction is worth reading rather than assuming, because the two look alike:
[limitations.md](limitations.md#the-settings-table-and-why-it-is-not-the-registry-that-was-refused)
sets them side by side. The short version is that a registry for the inspector would have
one consumer and would save writing `ui.slider`, where these names already existed in four
places and the failure without them was a config key that silently did nothing.

---

## 8. One vocabulary for the engine's surface

Five subsystems grew independently and spell the same ideas five ways. This section is the
decision about which spelling is right; applying it to code already written is a separate,
slower job. It is here rather than in a plan because **the next subsystem written has to be
written in it**, and a convention that arrives after the code it should have shaped has
already lost most of what it was for.

### Lifetime and bring-up

| Idea | Spelling | Not |
|---|---|---|
| Make a thing exist | `create(desc)` | `addBody`, `addSource`, `addCharacter`, `setEmitters` |
| Make it stop existing | `destroy(handle)` | `remove`, `release`, `free`, `erase` |
| Bring a subsystem up | `init(config)` | `initialize`, `setup`, `start` |
| Take it down | `shutdown()` | `clear`, `destroy`, `teardown`, nothing at all |

`init` returns `bool` **only where a caller must act on the failure** — `AudioEngine::init`
returns false and every call after it becomes a no-op, which is a game running without
sound rather than a game that will not run. Where there is no such choice to make, it
returns `void`, and a failure it cannot recover from goes through `Logger::critical`.
Returning `bool` that every caller ignores is worse than `void`: it reads as a decision
somebody made and did not.

### Predicates and accessors

- A predicate is the **bare adjective**: `active()`, `ready()`, `empty()`, `valid()`,
  `capturing()`. Not `isEnabled`, not `inTextMode`. The `is` prefix is C's answer to not
  having a type system and it buys nothing beside a `[[nodiscard]] bool`.
- **`[[nodiscard]]` on every accessor that returns a value and changes nothing.** It is a
  property of the engine's surface, not of a directory, and the current split — 117 in
  `scene/`, none in `core/Input.h` across some thirty getters — is the shape of a rule
  applied by whoever happened to be in the file.
- An index parameter is named for **what it indexes** wherever a class holds more than one
  kind of thing: `body`, `character`, `source`. A bare `i` is correct only where there is
  exactly one thing to index.

### Namespaces

**A namespace names a directory under `engine/`**, and the directories fall into three
tiers that `scripts/check_layers.sh` holds and the build enforces:

```
core                                 -> (nothing)
gfx scene ui sim root                -> core, and each other
ai nav particles physics audio anim  -> core, the engine cluster
```

**A module is exactly what `root` cannot reach.** That is the whole definition, and the
tiers follow from it: anything `Engine.h` reaches is bidirectionally coupled with it and
*is* the engine, so `gfx`, `scene`, `ui` and `sim` are one cluster with `root` rather than
four layers stacked on each other — which is what "there are four" used to say, and it
described an intent the includes never had. Two rules come out of it:

1. **Nothing in `core` or the engine cluster may name a module.** That is the point of the
   whole arrangement — see "Engine and game separation" for why an object file every game
   links is what decides what every game carries.
2. **No module may name another module.** Where two appear to need each other, what they
   share is *description*, and description belongs in `scene/` beside `Collider.h`,
   `AudioSource.h`, `ParticleEmitter.h`, `AnimationRig.h`, `CharacterMotion.h` and
   `Body.h`, which are all exactly that split.

The guard fails the build on an edge running the wrong way, and its `ACCEPTED` list —
the exceptions it was baselined with — has been empty since the second phase of the
migration that introduced it.

A finer namespace nested inside is allowed where one header owns a vocabulary that would
otherwise collide on common words — `core::settings`, `core::options`, `core::input`,
`core::json`. What is not allowed is the finer name *instead of* the module name, which is
what those four used to be.

The corollary decides a header that is in the wrong namespace by being in the wrong
directory: `GpuProfiler` takes a `VulkanContext`, writes into a `VkQueryPool` and hands
zones to `Renderer`, so it *is* graphics and it now lives in `gfx/`. **Where the namespace
is right and the directory is wrong, move the file.** A namespace changed to match a
misfiled header records the mistake instead of fixing it. Nothing else moved: `AudioTap`
and `Recorder` sound like they belong beside audio and video, and both are ring buffers
over bytes with no dependency on either subsystem, which is what `core` means.

Three things the rule deliberately leaves at global scope:

- **`engine/Engine.h`, `engine/Entry.h` and `engine/Game.h`.** A namespace names a module
  and a module is a directory; those three are in no module directory, because they *are*
  the outermost edge — the surface a game author writes against. `Game`, `GameSetup` and
  the `extern "C"` entry point read better unqualified for the same reason `main` does.
- **`engine/core/Format.h`**, which defines one macro. A macro has no scope for a namespace
  to change, so wrapping it would be a comment pretending to be code.
- **Anything a dependency owns.** `gfx::` is not stamped on Vulkan handles, `scene::` not
  on Jolt's or miniaudio's.

Inside a module, names from that module stay unqualified — `Renderer.cpp` writes `GpuImage`,
not `gfx::GpuImage`. Redundant self-qualification is noise, and it hides the one thing the
prefix is there to tell you, which is that a name came from somewhere else.

Test translation units may open a module with `using namespace scene;` rather than
qualifying several hundred call sites. A test is a consumer, not part of the engine's
surface, and the directive is confined to one `.cpp`. **No header does this, ever** — a
using-directive in a header is inherited by everything downstream of it, which is the
namespace deleted for everyone who included it.

### Sentinels

Every sentinel is named, and the name says which domain it belongs to — `kNoBody`,
`kNoSource`, `kNoNode`, `kNoCharacter`, `kAnyState`, `settings::Id::None`. Three rules
follow, and the third is the one that is currently broken:

1. **One declaration per sentinel**, at the narrowest scope its users reach. Not
   `0xFFFFFFFFu` written out beside a comment explaining it.
2. **A sentinel is a value a caller can hold**, so anything taking an index has to answer
   for it rather than index past the end. That is why the accessors on `AudioEngine` and
   `PhysicsWorld` are bounds-checked, and why every settings getter goes through one
   private accessor that answers the first row for a handle naming none.

   **A count is not a sentinel**, and the settings table is where that cost something.
   `Id::Count` was both the boundary of the row table and `find`'s answer for *no such key*,
   which was harmless only while nothing could take `Count` as a real id. The moment a row
   could be declared, the first one would have read as unknown everywhere `!= Count` was the
   test. `Id::None` is the sentinel now and `Count` is only the boundary.
3. **A function returns only a sentinel from its own domain.** `findClip` returns a clip
   sentinel, never `kNoCharacter`; `findParameter` returns a parameter sentinel, never
   `kAnyState`. Every sentinel in this engine is currently the same number, which makes a
   cross-domain return *work* and makes it wrong — the caller has no way to know which name
   to test against, and the compiler cannot help until handles carry types.

### Shaders

The same rule about one vocabulary, applied to GLSL. None of these changes the SPIR-V; all
of them change how long it takes to see that two shaders do the same thing.

- **A push-constant block is `pc`.** `layout(push_constant) uniform Push { ... } pc;` in all
  23 of them.
- **`local_size` spells the dimensions the shader indexes, and no others.** A 1D dispatch
  writes `local_size_x = 64`; a 2D one writes `local_size_x = 8, local_size_y = 8`.
  `local_size_z = 1` is the language's default and states nothing.
- **A storage image a pass only writes is `writeonly`.** The qualifier is documentation
  first — it says at the declaration that the pass is a producer — and it is a real
  constraint second. The one image in the engine that omits it, `bloom_up.comp`'s
  `dstImage`, says in a comment why: the upsample adds into what it overwrites.
- **A formula that two shaders need lives in a `.glsl` both include**, at the narrowest
  scope that reaches them — `worldFromDepth` and `FAR_DEPTH` are in `frame.glsl` because
  `frame.glsl` owns `invViewProj`. Four copies under three names is how that one arrived.
  `srgbToLinear` is the second application of the rule and was caught at *two*: P4's
  `sprite.frag` wants a tint converted for the same reason `overlay.frag` does, so it moved
  to `srgb.glsl` on the way in rather than waiting to become a third copy. Extracting it
  changed no image — the golden set is byte-identical across the move, which is the check
  worth having on a rule that claims not to change the SPIR-V.

---

## 9. The running process never writes a file a later run reads as an input

A build writes what a run reads. A run writes what a person reads. Nothing the engine does
while it is running produces a file that a subsequent launch will pick up and behave
differently because of.

The rule is narrow on purpose, and it is worth saying what it does **not** forbid. The
engine writes plenty: `substrate.json` and `bindings.json` are authored state, edited
through the panel and belonging to the person; a save file is the game's own data; a Chrome
trace, a log, a recording and a captured PNG are outputs nothing reads back. Every one of
those is either a person's file or a dead end. What the rule forbids is the third kind -- an
artifact the engine both produces and consumes -- because that is the shape where a build
directory stops describing the build.

**Why it is a rule and not a preference.** Two writers share one directory and the build
system cannot tell which files it made. Everything downstream of that is guesswork: a
golden suite that claims eleven byte-identical cases from a clean tree is claiming something
about what produced the inputs, and the claim quietly stops being true the moment a run can
edit them. The failure has C15's shape -- *a cache that is wrong looks exactly like a cache
that is fast* -- and nothing reports it.

The engine had exactly two of these and both are closed:

- **Hot-reloaded SPIR-V** (D10). `recompileShaders` used to rename a freshly compiled
  `.spv` into `build/<cfg>/shaders/`, which `readShaderBinary` reads on the next *cold*
  start -- so reload, quit, relaunch booted SPIR-V no build produced. It now compiles to a
  temp path outside the build tree, reads the bytes, unlinks it, and publishes through
  `gfx::overrideShaderBinary` for the life of the process.
- **The scene sidecar** (D9). `--bake-scene` used to call `writeSceneCache` from inside
  `GltfScene::loadCpu`, so a shipped game carried the writer and a flag that reached it, and
  a player who typed it wrote a `.scene` into the install directory. Baking is now
  `substrate-bake`, a host-only binary; `writeSceneCache` lives in a translation unit
  `libsubstrate.a` does not link.

**Both are enforced by construction rather than by review.** D10's is a spelling -- the
reload path no longer names a build directory, so it cannot write to one by accident. D9's
is the linker: a static library links by object file, so putting the writer in its own
translation unit is what makes "a game cannot produce a `.scene`" checkable with `nm`. That
is the same mechanism as the engine building with nothing under `game/` in the tree, applied
one level down.

**What the rule buys, stated as a property:** delete every generated artifact beside the
sources and every build directory, rebuild, and the engine behaves identically. A cache may
be absent -- and an absent cache is not an error, is not logged, and is not rebuilt at
runtime -- but nothing is *missing*, because nothing that a run needs was produced by a run.

The corollary is that offline artifacts must be reproducible, since that is the only way to
check the rule is holding. `scripts/ktx2.py` was the first: nothing under `engine/` writes a
`.ktx2`, and `manifest.py --require-cache` turns a cold image into a release-build failure.
`substrate-bake` is the second, and making it true cost two small fixes -- the durations a
bake measured are zeroed before they are written, and the padding inside a struct written as
a byte range is zeroed before it is written -- without which the same scene baked twice
produced different bytes and `cmp` could say nothing.

---

## What over-abstraction would look like here

Concrete anti-patterns, so the line is unambiguous:

- A `RenderPass` base class with `virtual void execute()`.
- A `ResourceManager` or `TextureCache` owning `GpuImage` lifetimes by handle.
- A render graph with declared reads/writes and automatic barrier insertion.
- A `Material` class hierarchy, or shader permutations behind an `IMaterial`.
- `RenderDevice`/`IRenderer` wrapping `VkDevice` "in case we add DX12".
- Templated `Buffer<T>` / `Pipeline<Vertex, Push>` machinery.
- A retained widget tree: a `Widget` base class with `draw`, `layout` and `onClick`, a
  parent pointer and a child vector.
- A property registry: a schema plus a type-erased setter plus a name-to-offset map, so an
  inspector can list fields.

The last two are worth naming because a UI and an editor are where a codebase most
reliably acquires the first six. The engine has both and neither of those.

---

## Engine and game separation

Running a game on Substrate used to mean editing `main.cpp`. **That is closed.** The
boundary was deferred until one of three conditions fired, and the third did: *"`main.cpp`
accumulates content-specific logic that neither config nor the scene file can absorb."*
It listed `placeLights`, the animation drive and the settings panel by name as the thing
rotting, and by the end there were 1806 lines of it.

The shape it took is the escape hatch that was recorded in advance, with one substitution:

| Recorded | Built |
|---|---|
| `add_library` plus a thin `main.cpp` | `add_library(substrate)` plus `engine/Entry.cpp`, eight lines, reached by the linker or not at all |
| An `extern "C"` struct of function pointers | `Game`, five empty-defaulted virtuals — the same indirection at the same edge, with better ergonomics |
| The POD draw-list view the instance table provides | Unchanged; `Engine` hands out references to concrete types and wraps none of them |
| `readShaderBinary` as the single path indirection | Still one path, and it turned out to be two sites rather than one — see below |

`Renderer` touching the scene through flat data rather than behaviour is what made this
cheap: there was no wrong abstraction to unwind, and concrete functions over flat vectors
are the easiest thing to put a boundary around.

### What the boundary is, and what checks it

`engine/` is the engine and builds to a static library. `game/<name>/` is a game and builds
to an executable. `./build.sh` builds only the first, and **produces no runnable binary**
— which is the whole check: the engine has to build, test and sanitize with nothing under
`game/` in the tree, so a dependency leaking from a game into `engine/` is a link error
rather than a code review.

**`Engine`'s constructor and destructor are out of line, and that is a boundary decision
rather than a style one.** Written inline in the header, `~Engine() { teardown(); }`
destroys twenty-odd by-value members in *the caller's* translation unit — so `Entry.cpp.o`,
whose eight lines mention no subsystem, carried undefined references to
`scene::PhysicsWorld`, `scene::AudioEngine`, `scene::SceneLoader`, `core::Recorder` and
`core::settings::Settings`, and so did any game translation unit that let an `Engine` go out
of scope. Both are defined in `Engine.cpp` now, and `Entry.cpp.o` names five `Engine`
members and `core::seedExecutablePath` and nothing else.

G10 is where that was found, and it also measured what it is worth: **nothing, in bytes.**
The binary is the same size to within one page of alignment, because `Engine.cpp.o` is
linked into every game regardless and pulls all of those object files in on its own account.
The change is kept because a translation unit should not reference types it does not
mention, and the *measurement* is kept because it is the load-bearing half — it is why the
much larger version of the same idea was declined. See
[limitations.md](limitations.md#what-stays-declined-and-its-trigger--the-game-arc).

Two rules keep `Engine` from becoming a facade over the engine:

> **`Engine` gets no method that merely forwards to one subsystem.** `dumpProfile()`
> earns its place because it spans two, and `startRecording()` because it spans three —
> the `Recorder`, the renderer's frame tee and the audio tap. `setExposure()` does not;
> that is `renderer().exposure`.

> **The engine acts on its own config. The engine binds no keys.** `--capture`,
> `--frames` and the resize drive are run *modes* that `scripts/golden.sh` and
> `scripts/baseline.py` depend on, so they stay engine-side. ~~Not one key is bound and not
> one panel is drawn in `engine/`~~; the engine exposes capabilities and a game decides how, or
> whether, to reach them. That is what keeps exactly one owner of the keyboard.

**The struck sentence was never true and `making-a-game.md` has carried the accurate version for
some time**: `BindingMenu` is a panel drawn in `engine/`, `Ui.Click` is a key bound there, and
`scene::FlyCamera` binds nine more. What is true is the rule underneath it — **nothing in
`engine/` binds a key unless a game asked for it.** G18 is what made that literal rather than
aspirational: the camera's nine actions used to be declared unconditionally from `Engine`, and are
now declared by `FlyCamera::activate` when a game constructs one and installs it. A game that
installs no camera lists no `Camera.*` rows at all.

Actions are declared by whatever consumes them, and that is unchanged by the split: the
binding menu and the UI's own click are the engine's and are declared there, and so is the
flycam's set — but only once a game has installed one;
everything else is `game/demo/DemoGame.cpp`. `Engine::run` applies the config's rebinds
*after* `Game::init`, because a config can only rebind an action that exists.

### What is still open

Of the three revisit conditions, one remains genuinely undone:

1. ~~A second real game or tool needs the engine.~~ Partly: `game/` holds one. A second
   is what turns the boundary from designed into exercised, and it is a row in
   the G arc rather than a claim here.
2. **Someone who cannot recompile the engine needs to build against it.** Still the only
   case that requires dynamic loading and a frozen C ABI, still a permanent tax, and
   still not true. In-tree games are the supported path and no install rules are written.
3. ~~`main.cpp` accumulates content-specific logic.~~ Paid.

Iteration speed alone is not a trigger. Reloading C++ requires all game state to live in
an engine-owned arena, which is a heavier constraint than it first appears; weigh a
scripting layer against a `.so` plugin separately if that is the real want.

**One recorded claim did not survive contact, and has since been paid.**
`SUBSTRATE_SHADER_DIR` was listed as having *"one choke point: `readShaderBinary` in
`engine/gfx/Pipeline.cpp`"*, and there were two — `Renderer::pollShaderReload` also
consumes it, as the *destination* it compiles `.spv` output into. A game with its own
shaders needs both a read path and a compile destination. That is recorded rather than
quietly amended, because it is exactly the failure mode a checked property is supposed to
prevent: the concern was answered on paper and never verified.

G5b built both. Resolution is again one function — `readShaderBinary` tries the game's
output directory and then the engine's — and compilation is one loop over a two-entry
table in `Renderer.cpp` that the CMake rules mirror. What the fix taught is a third thing
neither the claim nor its retraction had noticed: **a compiled shader outlives its
source.** Nothing removes an output whose `add_custom_command` stopped being generated, and
in the game's tree the mere presence of a file is what decides which of two shaders runs —
so deleting `game/<name>/shaders/tonemap.frag` left `tonemap.frag.spv` overriding the
engine's, with no source in the tree to explain the picture. `CMakeLists.txt` empties that
one directory on every configure; the engine's is left alone, because deleting an engine
shader also deletes the `readShaderBinary` call that named it.

---

## How a claim gets made

- **Zero validation errors** with layers on, in every capture.
- **A number, not an impression** — per-pass GPU cost before and after, from trace medians
  rather than single frames.
- **A screenshot compared against expectation**, plus G-buffer debug views for anything
  that writes to the G-buffer.
- **Sanitizers** for anything touching threads or lifetimes.
- **Both arms verified.** A check that always passes and one that does nothing look
  identical from the outside, so every check that matters is exercised against a planted
  mismatch as well as a correct input: the SPIR-V reflection check against three
  deliberately wrong layouts, the timestamp calibration against a build with the extension
  forced off, the golden comparison against a known-different render.
- **Establish neutrality, then measure.** A subsystem that should not move a pixel proves
  it by matching the golden set against a build of the previous commit, *before* any claim
  is made about its cost.

See [tooling.md](tooling.md) for the mechanics.

---

## The rules the three arcs established

The C, D, G and P arcs were planned in three roadmap documents. Those documents are gone —
each row's argument now lives on its card in [the board](../kanban/), and what follows is the
part that belonged to no single row: the rules the arcs held themselves to, and which the
engine is still held to.

## The consistency rules

The counterpart to the other roadmap's "Simplicity constraints". Those rules are about what
not to build; these are about making what *is* built look like one engine rather than five.

Rules 1 to 3 below are enforced by **C1**, which is a capability row and covers handles and
lifetimes only. Everything else the rules imply — naming, namespaces, sentinels, the
boilerplate that accumulated where nothing was extracted — is enforced by
Part 3, the D arc, which is where the rest of this section's thesis
actually gets paid for.

The problem, measured:

| Subsystem | How a thing is made | Handle | Can it be released? |
|---|---|---|---|
| `InstanceTable` | `create(desc)` | `InstanceId` — index + generation | **`destroy(id)`** |
| `PhysicsWorld` bodies | `addBody(desc)` | `uint32_t` | No |
| `PhysicsWorld` characters | `addCharacter(desc)` | `uint32_t` | No |
| `AudioEngine` sources | `addSource(desc)` | `uint32_t` | No |
| `SceneAnimator` characters | `addCharacter(skin)` | `uint32_t` | No |
| `ParticleSystem` emitters | `setEmitters(vector)` | vector index | No — whole-list replacement |
| `GltfScene` textures | `acquireTextureSlot()` | `uint32_t` + free list | **`releaseTextureSlot()`**, and see below |

Four spellings of "make a thing exist", two lifetime models out of seven, and one subsystem
that cannot express "add one" at all.

**The seventh row is the sharpest evidence in the table, and it reads as the solved one.**
`GltfScene` has the free list `InstanceTable` has, and no generation beside it — so a slot
released and reacquired hands the stale holder a *valid index pointing at a different
texture*. That is not a missing feature, it is the silent-alias bug Rule 1 exists to
prevent, already present in the one subsystem that looks like it took the lesson.
`acquireTextureSlot` also returns a bare `UINT32_MAX` on exhaustion with no named constant,
where `kNoBody`, `kNoSource`, `kNoCharacter` and `InstanceId::kInvalid` all exist next door
— which is D3's subject, and is how the two arcs meet.

### Rule 1 — one handle shape

Index plus generation, always, and **a distinct type per kind**. A bare `uint32_t` that
outlives its object is precisely the silent-alias bug `InstanceId` was given a generation to
prevent; five subsystems currently hand one out. Distinct types make a body handle passed
where a sound handle belongs a compile error, which is the same argument G2 makes for
`Setting<T>` being a `uint16_t` in a type wrapper.

**Five occurrences is the Rule of Threes met twice over**, so this is the one place the arc
introduces a shared type rather than repeating a pattern:

```cpp
// engine/core/Handle.h
template <typename Tag>
struct Handle {
    uint32_t index = 0;
    uint32_t generation = 0;
    [[nodiscard]] bool valid() const { return generation != 0; }
};
```

`InstanceId` becomes an alias rather than a rename, so no call site moves. The scope is a
global type because the callers span modules and it carries no state, which is the row the
scope table in [CLAUDE.md](../../CLAUDE.md) selects.

### Rule 2 — one verb set

`create` and `destroy`, on every subsystem. `addBody`, `addSource`, `addCharacter` and
`setEmitters` become `create`; nothing keeps a second spelling for compatibility, because
in-tree games are the supported path and there is one.

### Rule 3 — everything creatable is destroyable

No handle outlives its owner by contract. Where a subsystem cannot free the underlying
resource immediately — a Jolt body mid-step, a `ma_sound` mid-buffer — it retires the slot to
a free list and reclaims at the step boundary. **The generation counter is what makes the
delay safe**, because a stale handle reports staleness instead of aliasing whatever took
the slot.

One-shot things self-destroy. An effect whose last particle dies releases its own slot, and
the handle the caller still holds goes stale and says so. That is the whole lifetime story for
the common case, and it needs no explicit call.

### Rule 4 — the reference-versus-call rule holds, unchanged

Derived state is written through a call; plain data is handed out by reference.
`setLocalPosition` invalidates a world transform, a bounding box and a normal matrix, so it is
a call. A light's intensity invalidates nothing, so it is a reference. Restated here only so
the two documents agree; the other one argues it in full.

### What over-abstraction looks like in the runtime arc

- A `Resource` or `Handle` base class with `virtual void release()`. `Handle<Tag>` is a
  16-byte value type and has no methods beyond `valid()`.
- An asset manager with reference counting and a virtual `IAssetLoader`. Already refused in
  the other roadmap; C10 is where it would come back and must not.
- A `SaveGame` class hierarchy, or a reflection system to drive serialisation. C6 is two
  virtuals and a byte stream.
- A `Query` object with builder methods. C2 is three free functions returning plain structs.
- An `Effect` or `Emitter` component type. C3 spawns from the same `ParticleEmitter` data
  glTF already authors.

---

## Designing for scale — the runtime

Every capacity this arc introduces carries one of the three dispositions
[principles.md](principles.md#3-designing-for-scale) defines.

| Capacity | Disposition | Obligation |
|---|---|---|
| **Live handles per subsystem** | Generalized | Dense arrays with a free list, grown from data. No fixed capacity. The existing budgets (`bodyBudget`, `particleBudget`) keep their stated refuse-and-report policy and now bound *live* objects rather than lifetime totals. All four budgets are `GameSetup` fields since D14; the policy is unchanged and only who states the number moved |
| **Hits per query** | Generalized | The caller supplies a `std::span`; the call returns how many it *wanted*, not how many fit. A truncated result is detectable at the call site, which is the same contract `scopef`'s name pool and the overlay's quad cap already use |
| **Save file size** | Generalized | Streamed, versioned, no fixed record count. A version the build does not know is refused with a reason rather than partially applied |
| **Screen-space images** | Generalized | ~~Reuses the existing bindless texture array and its free list. There is no second descriptor array~~ **There is, and it is the right answer.** The overlay holds its images in a set of its own, with the font atlas at slot zero, because the scene's array lives in a set that does not exist until `setScene` and the overlay draws before one is loaded. The index rides on the vertex and one draw covers text and images together. ~~the count is a named constant~~ **`kMaxOverlayImages` is gone**: P1 made the set growable, and the ceiling is now the device's limit — see *Resident images* in the discharge table above |
| **Lights visible per frame** | Generalized in scene size, **stated cap** on the visible set | C8 makes `lightBudget` a cap on lights that survive the frustum test rather than on lights that exist. The refuse-and-report policy is unchanged; what changes is what it counts |
| **Navmesh polygons** | Generalized | Sized from the geometry it is baked over |
| **LOD levels per mesh** | Generalized | Data-driven, from the source asset. No fixed count |
| **Scene load time** | **Tiered, with no crossover** | Two paths — the glTF and the baked sidecar — and the selector is the sidecar's presence, not a measured threshold. Stated explicitly because the Tiered obligation is normally that the crossover *is* measured, and here there is none to measure: the baked path is never the slower one. What C13 measures is therefore not *whether* to bake but *what the bake is worth per axis*, which is what orders C14 against C15 and what catches a regression once the asset count is in the thousands |

---

### What D must not restate

The same discipline this document already applies to C3 and G7, and the reason the arc can be
worked in any order without collisions:

- **C1 owns the handle type and the `create`/`destroy` verb pair** across the five
  subsystems. D1 owns only what C1 does not touch — `[[nodiscard]]`, the `init` return shape,
  the teardown spelling, predicate naming. **D1 must not respell `addBody`**; landing both
  would collide in five files. Note that this is a split of *scope*, not of order — D1's
  decision half runs ahead of C1 precisely so that C1 has a convention to be written in, and
  what D1 is left holding afterwards is the retrofit. See
  Recommended order.
- **C14 owns the glTF document parse**, including the shared helper in `core/Json.h`.
  No D row restates it, and D8's shader-side deduplication is the same *shape* of finding in a
  different tree, not the same finding.
- ~~**`Config.cpp`'s three copies of the tri-state parse, and its four string-to-index
  ladders.**~~ **Paid, by the settings arc rather than by a D row**, because cleaning a file
  that arc was about to rewrite would have been work done twice. `triState` is one helper
  taking what `auto` means as an argument; the ladders are tables; `Config::render.debugView` and
  `Config::render.tonemap` are a `gfx::DebugView` and a `gfx::TonemapOperator` instead of the
  magic integers that would have silently mis-mapped every `--debug-view` flag if either enum
  were reordered. Both are parsed at the flag and stored as the value since D14, so the
  accessors that used to resolve them are gone as well. Left here rather than deleted because the audit that found it is what this
  section is a record of.

---

## Layout, and the build split

The arc introduces one new top-level directory and one new script.

```
engine/             the engine. Builds to a library. Knows nothing about any game.
  gfx/ scene/ core/ ui/
  shaders/          the engine's GLSL
  assets/           what the golden suite pins
game/               games. Not built by build.sh.
  demo/             the demo -- what main.cpp is today, moved
    shaders/        optional; searched before the engine's (G5b)
    assets/         the demo's content
  <name>/           what a developer creates
tests/              the unit suite. Links the hosted sources only; never touches game/.
```

```bash
./build.sh      [debug|release|asan|tsan|clean]   # the engine and the unit suite
./build_game.sh <name> [debug|release|asan|tsan]  # the engine, plus one game
./run.sh        [debug|release|asan|tsan] -- ...  # runs the configured game
./test.sh       [debug|release|asan|tsan] -- ...  # unchanged
```

**`build.sh` does not build a game, and therefore produces no runnable binary** — a
library and a test executable. That is the point of the split: the engine must be
buildable, testable and sanitizable without any game in the tree, which is the strongest
available check that the boundary is real. A dependency leaking from `game/` into `engine/`
becomes a link error rather than a code review.

**Which game a build directory holds is a property of the build directory.** `build_game.sh`
sets a `SUBSTRATE_GAME` cache variable; `build.sh` clears it. The engine's object files
survive the toggle, so alternating between the two costs a reconfigure rather than a
rebuild.

That choice is what keeps the tooling intact. `run.sh` hardcoded
`BIN="$BUILD_DIR/substrate"` on **one line**, and `scripts/baseline.py` mirrored it on one
more; both became a lookup of the configured game's name, and every script signature
stayed exactly as it was. `scripts/golden.sh` invokes `./run.sh` and needs no change at all. The
golden suite therefore keeps working with one added prerequisite — `./build_game.sh demo` —
rather than a rewrite.

**`test.sh` is untouched by any of this**, which is worth stating because it is the property
most easily broken by accident: the unit suite links the engine archive, names nothing that
draws, and links no volk and no glfw, so no game can affect whether it runs under
ThreadSanitizer.

---

## Simplicity constraints

The rules are in [principles.md](principles.md) and are not restated here.
This section says only what **this arc** does to them, because it is the arc most likely to
break them.

### The amendment: `Game` is a base class, and that is a change

The engine has exactly **one** derived type: a file-local `PhysicsDebugRenderer`
implementing two of `JPH::DebugRendererSimple`'s methods, because that is how Jolt hands
out the wireframe of a convex hull. The justification recorded for allowing it is *"a
dependency's API demanding one override is not an abstraction we wrote."*

**That argument does not cover `Game`.** A different one does, and it is already written
down: a module boundary spelled as an `extern "C"` struct of function pointers is *"not the
`virtual void execute()` rule 1 bans — it is one indirection at the outermost edge, not a
layer over Vulkan."* A base class with five virtual methods is the same indirection, in the
same place, with better ergonomics — five virtual calls per frame, none in any inner loop,
none between the engine and Vulkan. (It became six with the S arc: `configure` is where a
game names the scene and the other authored values that left `substrate.json`. It runs once
at startup, so the per-frame figure is unchanged.)

So the figure becomes **two**, and the line stays drawable:

> **`Game` is the only base class the engine defines, and it sits at the outermost edge.**

The ban is otherwise untouched. A `RenderPass` with `virtual void execute()`, an
`IMaterial`, a `RenderDevice` wrapping `VkDevice` — all still refused, and a second base
class appearing anywhere inside `engine/gfx/` is still the thing to stop.

**G7 would make it three, and that one needs no new argument.** A Jolt `ContactListener` is
a dependency's API demanding an override, which is precisely the justification the existing
one already stands on — `limitations.md` anticipates it in those words: *"That is a second
derived type, and it should arrive when something needs the event."*

Three claims elsewhere need updating the day G1 lands, and are listed here so none is
missed: `principles.md`'s figure, `docs/README.md`'s core-decisions row (*"One virtual
function in `engine/`"*), and `CLAUDE.md`'s **zero** virtual functions — which is already one
behind.

### What over-abstraction would look like in the game-API arc

The anti-patterns specific to what follows, so the line is unambiguous:

- A `Component` base class with `virtual void update()`, once nodes have attachments.
- A `SceneNode` class with a parent pointer and a `std::vector<SceneNode*>` of children.
  The tree here is indices into flat arrays; that is the whole design.
- An `Entity` / `GameObject` god type that owns a transform, a mesh, a body and a script.
- A `Material` hierarchy, or shader permutations behind an interface, once G5 makes a
  pipeline selectable per draw.
- An asset manager with reference counting, handles, and a virtual `IAssetLoader`.
- An event bus, a message queue, or an observer registry for settings changes. **G2 says
  explicitly why there is none.**

---

## Designing for scale — the game API

Every capacity, cost or fidelity limit carries one of three dispositions:

1. **Generalized** — one code path at every scale. No fixed capacity that can be exceeded
   silently; a limit is either sized from data, or it degrades by a stated policy and
   reports that it did.
2. **Tiered** — two paths with a measured crossover, selected by data rather than a build
   flag.
3. **Delegated** — the engine states the limit and a specific game implements past it. The
   architectural properties that keep it implementable must be **named and each verified to
   hold today**. A delegation with no checked properties is a handwave in a disposition's
   clothes.

> "The test scene doesn't need it" is not a disposition. It is a statement about the test
> scene.

### The dispositions the game-API arc assigns

| Capacity | Disposition | Obligation, and its status |
|---|---|---|
| **Nodes in a scene** | Generalized | Dense arrays that grow. No fixed capacity, no silent cap. |
| **Shader variants** | Generalized | Created lazily, on first use, never as a cross-product. A variant is a pipeline and pipelines are not free, so the count is **reported** rather than capped. |
| **Geometry buffer growth** | Generalized | Reallocate-and-recopy on overflow, and **log the reallocation**: a silent stall is worse than a loud one. |
| **Games per repository** | Generalized | `game/` holds N, one CMake target each, selected at configure time by `build_game.sh <name>`. There is no "the game". |
| **Attachments per node** | Generalized | One flat record with a slot per kind. Adding a kind widens a struct; it does not add a container. |
| **Out-of-tree games** (`find_package`) | **Delegated** | Three properties, checked below. |
| **Scripting, `.so` hot reload** | **Delegated** | Named so it is not relitigated. |

**Out-of-tree games — the three properties, and their status today:**

- (i) **One choke point for shader resolution.** *Fails today, and by more than the
  reference records.*
  [principles.md](principles.md#engine-and-game-separation) lists this concern
  as answered — *"One choke point: `readShaderBinary` in `engine/gfx/Pipeline.cpp`"* — and it
  is not one site but two: `SUBSTRATE_SHADER_DIR` is also consumed by
  `Renderer::pollShaderReload`, which writes `.spv` output back into that directory. A game
  with its own shaders needs both a read path and a compile destination. **G5b closed
  this**, and it was a row rather than a footnote precisely because the property had been
  asserted rather than checked — which is what the delegation discipline exists to catch.
  Building it turned up a third site the retraction had also missed: the stale-output
  case, recorded in the G5b notes below.
- (ii) **No absolute build-time path baked into a public header.** Holds today —
  `SUBSTRATE_SHADER_DIR` has a `"shaders"` fallback and appears only in `.cpp` files.
- (iii) **A static library with no plugin ABI to freeze.** Holds today, and G1 keeps it.

Until someone outside this repository needs to build against the engine, **in-tree games
are the supported path and no install rules are written.**

**Scripting and `.so` hot reload** are delegated for one reason, stated so it is not
re-argued every six months: the only thing that justifies a frozen ABI is a developer who
cannot recompile the engine, and that has not happened. Reloading C++ additionally requires
all game state to live in an engine-owned arena, which is a heavier constraint than it
first appears. If iteration speed alone is ever the want, weigh a scripting layer against a
`.so` plugin separately — they are not the same decision.

---

### What the 2D arc must not build

Restating `principles.md` where this arc is the thing that would break it, because a 2D path is
a classic place for a second renderer to appear:

- **A `Renderer2D`, a `SpriteBatch` class, or a `virtual void draw()` on anything.** P4 is a pass
  recorded inline as a method, like every other pass. The trigger that reopens this is unchanged
  and is a second graphics backend.
- **An asset manager.** P1 is a vector, a free list and a descriptor array. See below.
- **A 2D scene graph.** G3's tree is the tree.
- **A `Sprite` base class with `Tile` and `AnimatedSprite` deriving from it.** A sprite is a
  struct in a dense array.
- **A `Texture` wrapper type.** `ImageId` is a `Handle<Tag>`, which is two `uint32_t`s.

---

## The pixel-perfect contract

The arc's distinguishing constraint, written before the rows because four of them exist to
serve it.

### The four places a texel dies

None of these is a missing feature. Each is a thing the frame does on purpose, correctly, for
3D:

1. **Jitter.** [`Renderer.cpp:4334-4347`](../../engine/gfx/Renderer.cpp#L4334-L4347) applies a
   sub-pixel Halton offset as a clip-space translation on the view-projection. A sprite drawn
   twice lands on two different pixels.
2. **TAA.** The resolve blends against a reprojected history with a neighbourhood clamp. Even
   unjittered, a moving sprite's edge is a weighted average of what it was and what it is.
3. **The tonemap curve.** It is not identity. An authored sRGB value does not survive a round
   trip through an HDR target and a tonemapper, so the colour the artist picked is not the
   colour written.
4. **Extent mismatch.** The swapchain is whatever the window is. With no presentation step
   between the authored resolution and the window, a 320-wide image on a 1920-wide window is
   resampled by whatever the blit does.

The overlay already avoids the first three, and that is the shape the arc generalises: it draws
**last, onto the LDR swapchain, after tonemap**, in pixel coordinates with a NEAREST sampler.
[`Font.cpp`](../../engine/ui/Font.cpp) even documents the half-texel problem it had to solve —
*"NEAREST on a boundary is a driver coin-flip"* — which is the fifth way to lose a texel and the
one already answered.

### The contract

> **The guaranteed path is the default. A sprite leaves it by an explicit act, on its layer.**

Stated that way round deliberately. The alternative — sprites opting *in* — makes pixel-exactness
a property every author has to remember, which is a guarantee that quietly stops holding the
first time somebody forgets. Under this form, a scene that nobody has thought about is exact,
and a scene that is not exact contains a line somebody wrote.

The flag lives on the **layer** rather than the sprite, for the same reason: it is a decision
about a body of content, not about one quad, and a per-sprite flag would be sixteen thousand
opportunities to disagree. Sprites inherit their layer's.

**The stated risk**, recorded rather than left to be discovered: per-sprite flexibility means the
guarantee is checkable only per layer. The mitigations are that the default is the guaranteed
path, that the readback test covers the default, and that a debug view reports every layer which
has opted out — so "why is this blurry" has an answer on screen rather than in a grep.

### Where UI lands

P2 has one decision inside it that is not obvious and is easy to get wrong by omission: **the
overlay may draw inside the virtual-resolution target, or on top of it at native resolution.**
Both are wanted. A retro game wants its HUD at the same chunky scale as its world; a hi-res 2D or
2.5D game wants crisp text over soft art. It is a per-game decision, not a per-sprite one, so it
is a `GameSetup` field and not a layer flag.

---

## Designing for scale — 2D

Every capacity this arc introduces carries one of the three dispositions
[principles.md §3](principles.md#3-designing-for-scale) defines. Note the first
row: it **changes an existing disposition**, which no row in the C arc did.

| Capacity | Disposition | Obligation |
|---|---|---|
| **Resident images** | **Generalized** — *was Delegated*. **Landed** | P1. `kMaxOverlayImages` is **deleted, not raised** — a constant that is raised when someone complains is a silent limit with a changelog. What replaced it is `gfx::ImageTable` plus a descriptor set that doubles, bounded only by the device's own limit. **`kTextureSlotHeadroom` was not touched, and the prediction that it would be was wrong**: it belongs to `GltfScene`'s array, which the arc's own non-overlap rule keeps out of this row and which still has no caller outside its own file. The discharge table above now lists the two residencies separately for that reason |
| Sprites live | Generalized | Dense arrays grown from data, with a stated budget bounding *live* sprites and a refused spawn counted and reported — the policy C1 gave the voice and body budgets |
| Sprites per draw | Generalized | One instanced draw per layer, `vkCmdDraw(6, n)`. Recording is O(layers), not O(sprites), which is the property `principles.md` already claims for the indirect path |
| Sorting layers | Generalized | Sized from what the game declares. No fixed layer count |
| Virtual resolution | Generalized, with a **stated cap** | Any extent up to `maxImageDimension2D`. The integer scale is *derived* from it and the window, never authored, so the two cannot disagree |
| Atlas dimensions | **Stated cap** | `maxImageDimension2D`, refused at load with a reason rather than truncated |
| Tilemap extent | **No disposition — the capacity was declined** | ~~Chunked. Memory is proportional to occupied chunks, not to grid bounds~~ **P8 was reconsidered and refused**, so the engine holds no grid and this row has nothing to discharge. A tile is a sprite, so *Sprites live* above is the disposition that applies, and the extent a game may hold is bounded by its own grid — one or two bytes a cell, against a `GpuSprite`'s 64. See [limitations.md](limitations.md#what-stays-declined-and-its-trigger--the-2d-arc) for the argument and the two triggers |
| Tiles per tileset | Generalized | Derived from atlas dimensions and tile size — which is `SpriteSheetDesc`, already, and was never tilemap-specific |
| Sprite animation frames | Generalized | Data-driven from the sheet. No fixed frame count |

**One disposition this arc explicitly does not take.** Sprite *sorting* is not Tiered and has no
second path: it is a sort, at every scale. A GPU sort exists next door in the particle system,
and the note in P4 explains why P4 does not reach for it yet.

---
