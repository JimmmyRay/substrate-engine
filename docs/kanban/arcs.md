# The four arcs, and where they meet

Every card carries a prefix — **C**, **D**, **G** or **P** — and the prefix is not
decoration. Each names an arc with its own premise, its own boundary against the others, and
its own idea of what "done" means. This is that context: why each arc exists, and the rules
that keep two of them from building the same thing twice.

The arcs were once three roadmap documents. They are gone; each row's argument is on its
card, and this is what belonged to all of them at once.

## The C and D arcs — the runtime, and consistency

The G arc is about how a game *reaches* the engine. This
document is about what the engine can *do* for it.

The two are not the same shortfall, and until now only the first was written down. A survey
against what a moderately sized game needs — genre-agnostic, so a checklist rather than one
target — put the engine at roughly 85% of a renderer, 75% of its simulation subsystems, and
about 10% of the plumbing between them and a game. The G arc closes the API shape and adds,
by design, almost no capability. What it leaves untouched is a list of about ten things that
appear in no document at all: persistence, a spatial query that returns a hit, screen-space
imagery, spawning an effect at a point, pause, unloading, animation events.

Rows here are prefixed **C** so they never collide with the tier and subsystem numbering used
elsewhere, or with the G or P rows. Size key, matching the other roadmaps: **S** < 300 lines -
**M** 300-800 - **L** 800-2000 - **XL** > 2000.

Every C and D row is a card in [the board](README.md), and the directory the card sits in is
its state — which is how a row can say it is being verified, or is stuck, neither of which
the roadmap this replaced could express.

A third arc — the P rows — was opened after C5 landed. It covers what a 2D
game needs, and it exists because C5's separate image array gave the engine a *second* bindless
texture array — so a sprite atlas is a third caller wanting what two subsystems now hold
privately and differently. See the split rule below.

### The finding that shapes the C arc

One structural fact unifies what would otherwise be three unrelated gaps:

> **`InstanceTable` is the only subsystem in the engine with a lifetime model.**

`InstanceTable` hands out an `InstanceId` of index plus generation and takes it back through
`destroy()`. Nothing else does. Physics bodies, physics characters, audio sources, animator
characters and particle emitters all return a bare `uint32_t`, and two of them say in their
own headers that this is permanent:

> "Bodies keep the index `addBody` returned for as long as the world lives"
> — [`Physics.h:172`](../../engine/scene/Physics.h#L172)

> "Sources keep the index `addSource` returned for as long as the engine lives"
> — [`Audio.h:156`](../../engine/scene/Audio.h#L156)

Both sentences continue "for the same reason `InstanceTable` does not [compact or reorder]",
and that half of the reasoning is right — slots must be stable. But `InstanceTable` pairs slot
stability with a free list and a generation counter, and the other five took the stability
without the pair.

The consequence is that **runtime spawning, level unloading and API inconsistency are one
problem, not three.** A game cannot despawn a sound, retire a body, or remove an emitter.
That is not a missing feature in five places; it is a missing convention in one.

### The six gaps the C arc covers

- **Nothing can be destroyed.** The above.
- **A query cannot report what it hit.** `PhysicsWorld::segmentBlocked`
  ([`Physics.h:413`](../../engine/scene/Physics.h#L413)) returns a `bool`, and it is the whole
  narrow-phase surface. Jolt's `NarrowPhaseQuery` is called one line deep inside it and the
  hit is discarded. Interaction, picking, weapon traces, ground checks and AI perception all
  want the same call, and none of them can have it.
- **The engine cannot draw an image.** `overlay.frag` samples exactly one `sampler2D
  fontAtlas`, R8 coverage; a rectangle is a quad whose texcoords land on the atlas's reserved
  solid block. That is an elegant debug UI and it means no icon, no HUD art, no menu
  background, no loading screen and no cursor.
- **Nothing persists.** `Config` saves. Input bindings save. No game state does — there is no
  serialization of an instance, a transform, a body or anything a game would call its own.
- **The engine cannot be paused.** `beginFrame()` advances the accumulator unconditionally.
- **A level cannot be loaded while the game runs**, because loading one means re-deriving it
  from JSON. `GltfScene::load` parses the document, de-interleaves every accessor, flattens
  the node tree and cooks every collision mesh on every launch, and the packaging pipeline
  that already knows which staged files are scenes does not transform any of them.

`limitations.md` records the fourth of these nowhere and the third only as a list of missing
widgets. This document is where they get argued rather than discovered.

---

## Where the C and G arcs touch

Live plans have to be kept from disagreeing, so the split is a rule rather than a habit:

> A row belongs **here** if it adds a capability the engine does not have. It belongs
> **there** if it changes how a game reaches a capability that already exists.
>
> A row is a **D** row if it changes neither: it makes what already exists look like one
> engine.

The P arc extends that rule with a fourth clause rather than bending one
of the three, because by the rule as stated every P row would be a C row:

> A row belongs in the **P** arc if its correctness is judged in **texels** — that a texel the
> artist authored is the texel presented, checked against the source file rather than against a
> snapped reference.

That is a verification boundary rather than a subject one, which is the same basis on which the
D rows below are grouped: they are collected because they share a check, not a topic.

(A third document, the **S** arc, used to sit outside this split entirely: it was about
which values belong in `substrate.json` at all. It is complete and retired into the
reference — [principles.md §7](../architecture/principles.md#7-a-setting-is-a-property-of-the-person-running-the-program)
for the rule, [tooling.md](../architecture/tooling.md#configuration) for the table.)

The D rows live in this document rather than a fourth, in
Part 3, because they are the enforcement arm of the consistency rules
below — which state the thesis today and are enforced by exactly one row, C1. They were
written from an audit of the tree rather than from a survey of what a game needs, which is
why they arrive as a block rather than one at a time.

**Every D row is verifiable the same way, and that is the point of grouping them.** A D row
that changes what anything produces has failed, so the golden set is byte-identical across
all of them — the same argument this document already makes for C1, C14 and C15.

By that rule G2 (settings by name), G3 (a scene tree) and G6 (node inspection) are reach.
C2 (queries), C5 (imagery) and C6 (persistence) are capability. G4 is the interesting case
and lands *there* correctly: growable buffers change how geometry arrives, and the engine can
already draw geometry.

### Where the two arcs touch

| This row | Needs | Why |
|---|---|---|
| ~~C6 Persistence~~ | ~~**G3**~~ | ~~A scene tree is the thing being serialised. Building persistence against the flat instance table means building it twice~~ **Wrong in the same way C9's row was wrong, and it should have been caught the same day.** What a save holds is per-instance transforms and flags plus whatever a game streams — none of which is a tree. The flat table is not a lesser thing to serialise against, it is the *right* one: an index in the file is a slot, so a reader needs no name-to-node mapping and no ordering promise. G3 would let a save also record parentage; it was never what made the row possible |
| ~~C9 Spatial index~~ | ~~**G3**~~ | ~~The index is over node bounds, and reparenting is what makes it need incremental update rather than a rebuild~~ **Half right, and the half that was wrong is the half that gated it.** The index is over *instance* bounds, which the table already maintains; reparenting is what G3 would add, and it is the one thing `SpatialIndex` does not do |
| ~~C10 Streaming~~ | ~~**G4**, C1~~ | ~~Unloading a model means releasing geometry ranges G4 makes growable~~ **The gate was never real.** N models needs one set of shared buffers and a sub-allocator over them, not N scenes and not G4 -- the renderer reads seven things off the scene and none of them cares how many files the geometry came from. C1 landed; `RangeAllocator` and `appendModel` did the rest. See the correction below |

**One explicit non-overlap.** G7 owns the physics `ContactListener` and `playAt(sound,
position)`. C3 owns spawning a *particle effect* and a *decal* at a point. They are the same
shape of gap in three subsystems, and they are split across two documents because G7's half is
already argued in the other one. **C3 must not restate `playAt`**, and G7 must not grow to
cover effects.

The reverse pointer is recorded in the G arc so the rule is stated from both
sides.

---

## The C arc's four phases

Grouped by what each phase makes possible rather than by subsystem, because fifteen rows in
dependency order answers "what next" without answering "when can I start". Each phase ends at
a milestone that is demonstrable in the demo, not an adjective.

Rows are numbered in the order they were written down, not the order they run — C13 to C15
were added after C12 and belong in Phase 3. The phase a row sits in is what orders the
work; see [order.md](order.md) for the sequence within one.

### Phase 1 — things can come and go

> **Milestone:** the demo spawns a crate with a body, a sound and an effect on a keypress,
> destroys it on another, and repeats a thousand times without growing.

**C2 is the cheapest capability in the arc and unblocks the most.** It depends on nothing, and
interaction, picking, ground checks, weapon traces and AI perception are all the same call.

**Rows:**

- **C1** — Uniform lifetimes and handles
- **C2** — Spatial queries
- **C3** — Runtime effects
- **C4** — Pause and time scale

Each is a card. `ls docs/kanban/*/C1-*` finds one; its directory is its state.

### Phase 2 — it can be played

> **Milestone:** the demo has a pause menu with an icon in it; saves; quits; reloads to the
> same state; and plays a footstep on the frame the foot lands.

**Rows:**

- **C5** — Screen-space imagery
- **C6** — Persistence
- **C7** — Animation events and root motion

Each is a card. `ls docs/kanban/*/C5-*` finds one; its directory is its state.

### Phase 3 — it can be large

> **Milestone:** two levels, loaded and unloaded in either order with flat memory across ten
> cycles and the switch inside a stated budget rather than a stall; a scene with several
> hundred lights holding frame time; click-to-select in the viewport.

C10 is listed last in this phase rather than by number, because it is the row the other
three exist to make possible.

**Rows:**

- **C8** — Light volume culling
- **C9** — Spatial index
- **C13** — A measured load-time baseline
- **C14** — One document scan, and a package that bakes
- **C15** — The baked scene sidecar
- **C10** — Streaming and unloading

Each is a card. `ls docs/kanban/*/C8-*` finds one; its directory is its state.

### Phase 4 — the frontier

> **Milestone:** the engine stops being the constraint. What remains is genre.

**Rows:**

- **C11** — Occlusion culling
- **C12** — Navigation
- **C16** — Scripted input
- **C17** — Mesh LOD chains and the simplifier that generates them

Each is a card. `ls docs/kanban/*/C11-*` finds one; its directory is its state.

**C17 is the one row here that was not in the original survey.** It is C11's LOD half, split
off when C11's blocker was rechecked: the occlusion half had shipped and been measured, the
LOD half had never started, and one card cannot be in two columns. The blocker went with the
half it actually described.

## The D arc

The C rows came from a survey of what a game needs. These came from an audit of the tree
against itself — consistency, duplication, reuse, and whether a thing is fixed at the depth
it broke at. They are grouped rather than spread through the phases because they share a
verification: the golden set, byte-identical.

**Grouping them is not scheduling them, and the two were conflated in the first version of
this document.** Every row below splits into a decision and a retrofit, the decision half of
several of them belongs *ahead* of the capability rows, and
Recommended order is where that is argued. Read this Part as the
findings; read that section for when each half of one lands.

**No D row carries a disposition**, and that is the check rather than an omission: a D row
that introduced a capacity would be adding a capability, which would make it a C row. The
table in Designing for scale is complete without them.

Size key as elsewhere: **S** < 300 lines - **M** 300-800 - **L** 800-2000.

**Rows:**

- **D1** — One vocabulary for the engine's surface
- **D2** — A namespace that means something
- **D3** — Names for the values that have none
- **D4** — Vulkan boilerplate, at the Rule of Threes
- **D5** — Reuse what the engine already has
- **D6** — Bounds, and defaults that agree
- **D7** — The scripts and the build agree with themselves
- **D8** — Shader conventions, and the formula in four copies

Each is a card. `ls docs/kanban/*/D1-*` finds one; its directory is its state.

## The G arc — the API a game is written against

Substrate renders. It is not yet something a game is *written against*: running one means
forking the repository and editing `main.cpp`. This document is the arc that closes that,
and it is designed backwards — Part 1 is the call site a developer writes, and everything
after it is what each line costs.

Rows are prefixed **G** so they never collide with the tier and subsystem numbering used
elsewhere. Size key: **S** < 300 lines - **M** 300-800 - **L** 800-2000 - **XL** > 2000.

Every G row is a card in [the board](README.md), which is how G2 can say it is in progress
rather than needing a paragraph to name which four of its five parts are done.

### The four gaps

Three of these were already deferred with stated triggers, and all three triggers have now
fired. The fourth was never written down.

- **There is no scene tree.** `InstanceTable` is flat world transforms; glTF's node
  hierarchy is flattened at load, and only `AnimationRig` retains a parent-linked node
  array. Nothing can be reparented, and a light or a sound cannot ride a moving object.
- **There is one G-buffer pipeline.** Materials are a device-local buffer uploaded once at
  load and indexed by `GpuInstance::meta.y`. A material/pipeline registry was deferred with
  the trigger *"a registry earns its place when pipelines are created from data"*, sharpened
  later to *"a variant selected per draw rather than per keypress"*. Attaching a shader to
  geometry is that trigger, and **G5** is where the deferral ends.
- **The game is `main.cpp`.** 1806 lines holding `placeLights`, `spawnExtraCharacters`,
  `locomotionMachine`, `driveCharacters` and `drawSettingsPanel`. The engine/game separation
  argument in [principles.md](../architecture/principles.md#engine-and-game-separation) names
  three conditions for revisiting, of which the third is *"`main.cpp` accumulates
  content-specific logic that neither config nor the scene file can absorb"* — and it lists
  those functions by name as the thing rotting. `limitations.md` records the boundary itself
  as deferred for want of *"a second consumer"*. **G1** pays the third condition and
  **G1b** supplies the second consumer.
- **There is no way to start a project.** Onboarding for an engine *contributor* is good.
  Onboarding for someone making a *game* does not exist, because there is nothing to
  onboard onto. See Getting started.

Scene authoring adds a fifth, milder complaint: it runs through glTF
`extras.substrate_collider` / `substrate_emitter` / `substrate_audio`, an engine schema
smuggled through a standard's escape hatch. It works. It should not be the only door.

And `limitations.md` names one thing outright rather than leaving it to be inferred:

> **"This is the first thing a game layer will want."** — of physics contact events, which
> have no `ContactListener`, and which in turn block a one-shot audio API because *"nothing
> in the engine yet fires an event for it to serve."*

That is **G7**, and it is in this document because its own limitation says it belongs to
whoever builds the game layer.

### The G arc and the reference

[architecture/](../architecture/) is the reference: what the engine *is*, and what it
deliberately does not do. This is a roadmap: what it is not yet. The two are complementary
and the split is deliberate — a roadmap is a plan and goes stale, a reference is answers
and does not.

This document links into the reference for decisions that are settled and restates only
what its own argument turns on. Three of its rows exist to **change** something the
reference currently records — G1 the module boundary, G5 the material/pipeline registry,
G5b a claim about shader paths that is inaccurate — and each says so explicitly rather than
letting the two documents quietly disagree.

### The G arc against the C arc

The C arc is the second live plan, and the split between them is a rule
rather than a habit:

> A row belongs **there** if it adds a capability the engine does not have. It belongs
> **here** if it changes how a game reaches a capability that already exists.
>
> A row is a **D** row — also there — if it changes neither: it makes what already exists
> look like one engine.

So G2, G3, G6 and G8 are reach and stay here; queries, persistence and screen-space imagery
are capability and live there. G4 is the case worth naming, and it belongs here: growable
buffers change how geometry arrives, and drawing geometry is something the engine already
does.

**G9 satisfies neither half of the rule, and it is not a D row either.** It adds no
capability and changes nobody's reach — it is content, and by the letter of the rule above it
should not be in either document. It is here because the rule sorts *engine* rows and G9 is
not one: it is this document's acceptance test, and it belongs beside the claim it tests. The
same demo is what the C arc's phase milestones are written in terms of ("the demo spawns a
crate with a body, a sound and an effect on a keypress"), so the two documents share one
consumer rather than growing two.

The **D rows** came out of an audit of the tree against itself rather than a survey of what a
game needs, and none of them reaches this document — a row that changed how a game reaches
something would be a G row by the rule above. One is worth knowing about from here anyway:
**D1 unifies the engine's naming and `[[nodiscard]]` conventions and must land after that
document's C1**, so a G row landing in between should expect the surface it is written
against to be respelled once.

Three of that document's rows depend on this one — its persistence and spatial-index rows
need **G3**'s tree, and its streaming row needs **G4**'s growable buffers. One boundary runs
the other way and is worth stating so neither document grows across it: **G7 owns the
`ContactListener` and `playAt(sound, position)`; spawning a particle effect or a decal at a
point is that document's row, not this one's.**

A third arc — the P rows — touches this one at two points and neither is a
dependency:

- **G4 owns geometry residency; that document's P1 owns image residency.** Growable vertex and
  index buffers are this row; a growable bindless image array is that one. **G4 must not grow a
  texture table, and P1 must not grow a vertex buffer.** They are separable because they are
  already separate — images and geometry have never shared an allocator here.
- **G7 owns contact events; P7 owns making a body move.** `PhysicsWorld` today has no
  `setLinearVelocity`, `addForce` or `setTransform` at all, which is a gap a 3D game has as
  much as a 2D one; it is written down there because that is where it was found. G7 must not
  grow to cover motion.

---

## The G arc's stages

Ordered so that the stages which *remove* code come first, which is what makes the golden
image set a real check on them: G1 and G2 add no capability, so any moved pixel is a defect
and a re-snap is not an available answer.

**G1b and G5b are pinned to their neighbours rather than deferred to an end-of-arc
tidy-up**, and each for a specific reason: G1b is what gives G1's boundary a second
consumer, and G5b is what G5's game-supplied GLSL needs in order to resolve at all.

**Rows:**

- **G1** — Engine, Game, entry point
- **G1b** — Project scaffolding
- **G2** — Settings / property table
- **G3** — Scene tree
- **G4** — Assets
- **G5** — Shader variants
- **G5b** — Shader search path
- **G6** — Node inspection
- **G7** — Collision events, and the one-shot audio they unblock
- **G8** — What a game cannot reach yet
- **G9** — The demo, as everything the engine can do
- **G11** — A mesh made in code can carry morph targets
- **G12** — Locomotion through a six-state machine

Each is a card. `ls docs/kanban/*/G1-*` finds one; its directory is its state.

**G11 and G12 are G9's split**, and they take new ids for the reason C11's LOD half took
C17: splitting a row is the one thing that adds an id rather than reusing one. G9 kept its
own and closed on the half that could be built entirely out of public calls; the two halves
that could not — a morph target made in code, and a control scheme that needs
`--input-script` to be checkable — went to the end of the arc's numbering with their own
verification.

## The P arc — the second dimension

The G arc is about how a game *reaches* the engine.
The C arc is about what the engine can *do* for it. Both were written by
surveying what a moderately sized **3D** game needs, and neither contains the word sprite,
the word orthographic, or any row about presenting an image at the size it was drawn.

This document is the third arc. It covers what the engine would need in order to run a 2D
game — a genre-agnostic checklist, the same way the C arc is, rather than a plan shaped by
one target.

Rows here are prefixed **P** so they never collide with the C, D or G rows. Size key, matching
the other two: **S** < 300 lines - **M** 300-800 - **L** 800-2000 - **XL** > 2000.

Every P row is a card in [the board](README.md), and the directory the card sits in is its
state.

**A constraint, stated at the top because it changes almost every row below: the output has to
be pixel-exact.** A 2D engine that cannot guarantee that a texel the artist drew is the texel
that reaches the screen is not a 2D engine; it is a 3D engine that can draw flat things. That
guarantee is not a feature to be added at the end. It is a property the current frame destroys
in four separate places, and getting it back is what Part 1
is about.

### The finding that shapes the P arc

The C arc opened on the observation that `InstanceTable` was the only subsystem with a
lifetime model. This arc has an equivalent, and C5 landing is what created it:

> **The engine now has two bindless image arrays, and they agree about nothing.**

**The right-hand column is what P1 found; the parenthesised half is what it left.**

| | The scene's ([`GltfScene`](../../engine/scene/GltfScene.h)) | The overlay's — now [`gfx::ImageTable`](../../engine/gfx/ImageTable.h) |
|---|---|---|
| Sized | `loaded + 1 + 64` at load | ~~`kMaxOverlayImages = 16`, at compile time~~ (the device's sampled-image limit) |
| Can grow | No — a variable-count binding built once | ~~No~~ (yes — a set and its layout reallocated by doubling) |
| Handle | bare `uint32_t` slot ([`GltfScene.h:151`](../../engine/scene/GltfScene.h#L151)) | ~~bare `uint32_t` slot~~ (`ImageId = Handle<ImageTag>`) |
| Exhaustion | `UINT32_MAX`, unnamed | ~~`kNoOverlayImage`~~ (an invalid handle, resolving to `kFallbackSlot`) |
| Can a slot be released? | `releaseTextureSlot`, free list, **no generation** | ~~No unload at all, deliberately~~ (`destroy`, free list, **with** a generation) |
| Partially bound | Yes | No, deliberately |
| Callers | **none outside `GltfScene.cpp`** | `Engine::images()`, `ui::Context::image`, and the demo |

**Neither is wrong.** C5 argued its separate array carefully and the argument holds — the
overlay draws before `setScene`, so sharing the scene's set would mean no text path with which
to tell a user why the scene did not open. The 16-slot cap is right for "a logo, a few icon
sheets", which is what [`Renderer.h:726-730`](../../engine/gfx/Renderer.h#L726-L730) says it is
sized for. The absence of an unload is right for the same reason and says so.

What has changed is that **a sprite atlas is the third caller**, and three is the rule. It can
live in neither array: not the scene's, because a 2D game may load no scene and because the
array cannot grow past its 64 slots of headroom; not the overlay's, because sixteen images with
no unload is a UI's population, not a game's.

The consequence is that **runtime image loading, the 2D content path and a defect C1 left
behind are one problem rather than three.** That is P1, and every other row in this document
waits on it.

### The defect C1 left behind, and why it matters here

the C arc's consistency table called the `GltfScene` row "the sharpest evidence in the table,
and it reads as the solved one": a free list with no generation beside it, so a slot released
and reacquired hands a stale holder *a valid index pointing at a different texture*. C1 landed
and converted five subsystems. **It did not convert this one** — `acquireTextureSlot` still
returns a bare `uint32_t` ([`GltfScene.h:151`](../../engine/scene/GltfScene.h#L151)) — because C1's
scope was the five subsystems that hand out object handles, and this is a sixth.

So the one lifetime in the engine that C1 did not reach is the one the 2D arc needs most, and it
is the one already documented as carrying the exact silent-alias bug `Handle<Tag>` was
introduced to prevent. P1 does not merely add a capability; it closes the last row of a table
the C arc opened.

[`limitations.md`](../architecture/limitations.md) records a related gap from the other side:
*"The texture free list's release path is not unit-tested."* An untested release path on a
free list with no generation is two halves of one hazard.

### The six gaps the P arc covers

- **An image cannot outlive the thing that loaded it, or be loaded at all outside those two
  paths.** The above.
- **The engine has one projection.** [`Camera::projection`](../../engine/scene/Camera.cpp#L48) is
  an infinite reverse-Z perspective, hand-built, with no mode and no alternative. Three shaders
  hardcode the closed-form linearization only that matrix satisfies.
- **Nothing in the frame preserves a texel.** Jitter, TAA, the tonemap curve and the absence of
  an integer-scale presentation step each destroy pixel-exactness independently.
- **There is no world-space quad.** The nearest thing is the particle billboard, which has no
  rotation, no per-instance UV rect and no non-uniform scale.
- **A physics body cannot be pushed, and cannot be confined to a plane.**
  [`PhysicsWorld`](../../engine/scene/Physics.h) has `createBody`, `createCharacter`, `destroy` and
  `setCharacterInput`, and no `setLinearVelocity`, `addForce` or `setTransform` of any kind.
  There is no `EAllowedDOFs` anywhere in the tree.
- ~~**Nothing draws a grid.** Tiles are the one 2D primitive with a cost model of its own, and
  there is no row for them in any document.~~ **This was the one gap of the six that was not
  real, and P8 is where that was found.** Tiles have no cost model of their own: P4 draws every
  sprite in the arc in one call, so a grid costs what its cells cost as sprites and nothing
  extra. The row was declined; see [limitations.md](../architecture/limitations.md#what-stays-declined-and-its-trigger--the-2d-arc).

---

## The P arc against the other two

Two live plans already have a rule keeping them from disagreeing, and a third needs its own or
all three will drift. The C arc states the existing split:

> A row belongs **there** if it adds a capability the engine does not have. It belongs in
> the G arc if it changes how a game reaches a capability that already exists. It
> is a **D** row if it changes neither.

By that rule every row below is a C row, so this document owes a reason that is not filing
convenience. It is this:

> **A row belongs here if its correctness is judged in texels.**

That is a *verification* boundary rather than a subject one, which is the same basis on which
The C arc groups its D rows — they are collected because they share a check, not because they
share a topic. The C arc has two standards: byte-identical golden output for rows that change how
rather than what, and a trace median for rows that claim a number. This arc adds a third that no
C row can carry:

> **A texel authored is a texel presented**, checked by reading back the presented image and
> comparing it against the source file.

That is a stronger check than the golden set, because the expected answer is *computed from the
input* rather than snapped from a previous run. When a golden case fails you may, in principle,
re-snap it. When this fails there is nothing to re-snap against; the engine is simply wrong.

### Explicit non-overlaps, stated from both sides

The same discipline the C arc applies to C3 against G7:

| Boundary | Rule |
|---|---|
| **P1 vs C5** | C5 is **landed and closed**. P1 does not restate it, revisit its decision, or move the overlay's array into the scene's. P1 promotes the overlay's array to a type the engine owns and makes the overlay its first caller, with behaviour unchanged. [`Renderer.h:726-730`](../../engine/gfx/Renderer.h#L726-L730) predicted this row in its own words: *"The moment a game streams UI art is the moment to revisit it"* |
| **P1 vs G4** | P1 owns **image** residency. G4 keeps geometry buffers, `createMesh` and mutable materials. G4 must not grow a texture table; P1 must not grow a vertex buffer |
| **P1 vs C10** | C10 owns *streaming* — async load off the frame thread, and reclaiming geometry ranges. P1 owns residency and lifetime only. A load in P1 is synchronous, exactly as `loadImage` is today |
| **P1 vs `GltfScene`'s array** | **P1 does not touch it.** See P1's scope — promoting one rung reaches the overlay and sprites; reaching the scene's array is a second rung, and no caller needs it |
| **P3 vs D8** | D8 unified `worldFromDepth` and `FAR_DEPTH` into `frame.glsl`. P3 unifies `viewDistance` into the same header for the same reason. It is D8's argument applied to a second expression, not a restatement of it |
| **P7 vs G7** | G7 owns the Jolt `ContactListener` and `playAt`. P7 owns the DOF lock and the body force/velocity API. Same class, disjoint surfaces, and G7 must not grow to cover motion |
| **P6 vs C1** | Sprite handles are `Handle<Tag>` under C1's rules and introduce no sixth lifetime model |

## The P arc's call site

Designed backwards, the way both other roadmaps are.

## The P arc's three phases

Grouped by what each makes possible. Each phase ends at a milestone that is demonstrable, and
in this arc two of the three are demonstrable *as a bit-exact comparison* rather than as a
picture somebody looked at.

### Phase 1 — a texel lands where it was put

> **Milestone:** the demo loads a PNG, draws it through the overlay into a 320x180 target
> presented at 3x, and a readback of the swapchain is bit-identical to the source file scaled
> by three. No sprite exists yet, and the guarantee is already provable.

**P2 goes second rather than fourth on purpose.** The guarantee is a decision, and this document
inherits the C arc's finding that *a decision is cheap while it is still a decision and
expensive once code has been written without it*. Landing P2 before P4 means the sprite pass is
written against a frame that already preserves texels. Landing it after means writing the sprite
pass twice.

**Rows:**

- **P1** — The image table
- **P2** — Presentation: virtual resolution and integer scale

Each is a card. `ls docs/kanban/*/P1-*` finds one; its directory is its state.

### Phase 2 — the world can be flat

> **Milestone:** an orthographic scene of ten thousand unlit sprites across several layers,
> sorted correctly, holding frame time, with the readback still bit-exact.

**Rows:**

- **P3** — Orthographic camera, and one depth linearization
- **P4** — The sprite pass

Each is a card. `ls docs/kanban/*/P3-*` finds one; its directory is its state.

### Phase 3 — it can be a game

> **Milestone:** a scrolling tilemapped level with animated sprites, plane-locked bodies that
> can be pushed, and a lit sprite in the same scene as an unlit one.

**P5 reuses C7's event track and must not build a second one.** A flipbook frame crossing a
boundary is the same shape as a clip time crossing an event, and C7 already argued the two
decisions that make it right: the events are a *list read after the update* rather than a
callback, because a callback is the engine calling into a game mid-update; and every crossing in
the step fires, capped at one per event per update, so a game does not drop footsteps exactly
when it is already struggling. Frame timing runs off the fixed step, so it inherits C4's time
scale — a paused game has paused sprites, for free, because it is the same accumulator.

**P6 states what it does not get, in the header rather than in a bug report.** A lit sprite is
blended; [`InstanceTable.cpp:166`](../../engine/scene/InstanceTable.cpp#L166) excludes blended
instances from `dynamicCount()`, so a lit sprite writes no velocity and therefore gets no TAA
motion correction. It also, by construction, does not carry the pixel-perfect guarantee: it goes
through the G-buffer, the lighting pass and the tonemapper, which is the entire point of using
it. Both facts belong in the row, because both are the kind of thing that reads as a bug to
somebody who did not choose it.

P6 needs one unit quad in the geometry buffer and no more. That is cheaper than waiting for
G4's `createMesh`, and it is worth stating that the row was checked against that dependency
rather than assuming it: one quad is one quad, and G4 is about buffers that *grow*.

**P7's two halves are unequal and the row's name understates the second.** The DOF lock is one
enum field on `ColliderDesc` and one line in `createBody`; Jolt has `EAllowedDOFs` and the
header already rules out a second solver, so this is in-grain and small. The motion API is the
real work, and it is a gap this arc found rather than inherited: `PhysicsWorld` exposes
`createBody`, `createCharacter`, `destroy` and `setCharacterInput`
([`Physics.h:227`](../../engine/scene/Physics.h#L227),
[`:230`](../../engine/scene/Physics.h#L230), [`:305`](../../engine/scene/Physics.h#L305)) and nothing
that moves a body. **A 3D game has the same problem and neither other roadmap names it** — G7
covers contact events, C1 covered lifetimes. It surfaces here because a 2D game hits it on day
one, but the fix is not 2D-shaped and the row should say so.

~~**P8 is the row a genre could make wrong**, which is what the C arc says of C12, and it
deserves the same treatment: revisit it at the Phase 3 boundary rather than commit to it now.
Grid-based 2D is a real commitment. What justifies it as an engine primitive rather than game
code is a cost model nothing else in the arc has — roughly 8,000 tiles are visible at 16 px on a
1080p screen, and the difference between one draw per chunk and one per tile is the difference
between a tilemap and a slideshow. That is an engine's problem.~~ **Revisited at the Phase 3
boundary as instructed, and declined — and the sentence above is why.** The cost model was
written before P4 landed, and P4 did not land one draw per tile: it landed **one draw for all
layers**, measured at 0.053 ms for ten thousand sprites with no sort in it, which is more than
a 1080p screen of 16 px tiles. So the comparison the row rested on has no second arm. What was
left is chunking, which the game's own grid does better, and an authoring format, which this
arc had already refused at smaller scale for sprite sheets. The refusal and its two triggers
are in [limitations.md](../architecture/limitations.md#what-stays-declined-and-its-trigger--the-2d-arc);
the last clause above stands as the outcome — **tiles are sprites, and the sprite budget is the
ceiling**.

**The general lesson, because it is not about tilemaps.** A row deferred to the end of an arc
must have its justification re-read against what the arc *landed*, not against itself. The rows
in front of it are precisely the ones most likely to have answered it, and here one of them
had.

---

**Rows:**

- **P5** — Sprite sheets and animation
- **P6** — Lit sprites
- **P7** — 2D physics, and the motion API it exposes
- **P8** — Tilemaps

Each is a card. `ls docs/kanban/*/P5-*` finds one; its directory is its state.
