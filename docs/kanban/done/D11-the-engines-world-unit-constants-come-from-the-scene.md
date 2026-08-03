---
id: D11
title: The engine's world-unit constants come from the scene
arc: D
size: M
verification: golden-11, tests-4, readback, scripted-input, validation
---

# D11 — The engine's world-unit constants come from the scene

Afterwards no length in `engine/` is a literal: `ssaoRadius` is derived in
`Renderer::setScene` beside the two particle lengths already there, and `ssrMaxDistance`,
`fogMaxDistance` and `fogHeightFalloff` arrive through a new `Source::Scene` tier that a
config, game or command-line value still outranks. The sun a scene did not author stops
surviving into a scene that authored its lighting.

This came out of an audit for game-specific code in the engine, which found none — no include
across the line, no asset path, no node or material name, no key bound and no panel drawn.
What it found instead is that the *values* leaked where the code did not. `ssaoRadius = 0.5f`
says so in its own comment: *"Sponza is roughly 20 units across, so this is contact-scale."*
It is not a settings row and it is not derived, so a 2 m test scene gets a hemisphere meant
for a 30 m cathedral and there is no way to say otherwise short of recompiling. The mechanism
to fix it has been in `Renderer::setScene` since S3.3 — `particleSortRange` and
`particleCollisionThickness` are both scene-relative — and these were simply never moved onto
it.

`ssrMaxDistance` (8.0), `fogMaxDistance` (60.0) and `fogHeightFalloff` (6.0) are the softer
half: they are settings rows, so a user *can* correct them, but the Sponza-sized default is
duplicated between the table in `Settings.h` and the struct in `Renderer.h`, and a default
nobody knows is wrong is not much better than a literal. They cannot be derived in the table,
and rewriting a row's default breaks `saveJson`, which rebuilds a fresh defaults instance from
the macro list to decide what to write. A `Scene` tier ranked just above `Default` uses the
precedence machinery that already exists instead of fighting it: derive at scene load when
`source(id) <= Source::Scene`, so a second scene re-derives while anything explicit stands,
and skip `Scene` rows in `saveJson` so a derived value never lands in the user's
`substrate.json` as though they had chosen it.

**`fogDensity` is deliberately left alone**, and that is the one place this row stops short of
the audit. It is extinction *per* world unit, so it scales inversely rather than with the
three lengths — one helper covering both would be two intents behind one parameter. More to
the point the code already argues density is authored rather than derivable, and fog is off by
default. Its Sponza reference gets reworded; its value does not move.

The sun is a separate finding with the same shape. `Engine.cpp` states the rule — *"A file
that ships its own lights wins"* — and then applies it to the punctual list only. When a scene
ships lights but no directional, `render.lights` is replaced while the sun keeps whatever
`GameSetup` supplied; the log line says *"no directional light, sun from config"* out loud. So
an indoor scene lit by point lights silently gets the demo's sun at intensity 3.0, and the
only exit is an explicit `setup.sunIntensity = 0.0f` — a default you have to know to turn off.
Zeroing the sun in that branch makes the stated rule true of all of the file's lighting.
`GameSetup`'s defaults do not change: they are correct for the no-lights fallback, which is
what they were written for.

**What I expect to be wrong about.** Inserting a value mid-enum in `Source` is the risk — any
numeric cast or ordered comparison outside `setValue` breaks quietly, and the dump's name
table has to grow an entry. The table bounds are the other one: `ssrMaxDistance` stops at 64
and `fogHeightFalloff` at 100, which a large scene will exceed, so this row probably widens
three ranges it did not plan to. And the estimate assumes `setValue` clamps rather than
rejects out-of-range values, which has not been checked.

## Verification

- `scripts/kanban.py`, then `./build.sh`.
- `./test.sh debug`, `./test.sh asan`, `./test.sh tsan`, `./test.sh release` — each its own
  invocation, never chained. `Settings` gains a tier, so `ConfigTests` is the suite that
  matters: add cases that a `Scene`-sourced value loses to `Config` and to `Cli`, that a
  second scene re-derives one, and that `saveJson` does not write one.
- `scripts/golden.sh check`, then read every diff against the table below before re-snapping
  anything.
- `timeout -s TERM 60 ./run.sh demo` — unchanged. `showcase.gltf` ships no lights, so
  `placeLights` still fires and the demo's sun still stands.

**This row is not byte-identical across twelve, and that is intended rather than a re-snap of
convenience.** Four cases move:

| Cases | Expected | Why |
|---|---|---|
| `lit` `albedo` `normal` `depth` `ssao` `no-ibl` `msaa1` `no-rt` | byte-identical | All Sponza. Derive against an explicit reference diagonal so Sponza's scale factor is exactly `1.0f` and every product is bit-identical to today's literal. Sponza declares no lights at all, so the sun branch never runs. |
| `emissive` `particles` `physics` | change | Each declares punctual lights and no directional: loses the sun it never authored, *and* gets a scale-corrected SSAO radius. |
| `skin` | change | Declares no lights, so the SSAO radius only — corrected for a scene far smaller than Sponza. |

A non-zero mean delta on any Sponza case means the reference diagonal literal is wrong. Fix
the literal; do not re-snap.

Getting that literal needs a measurement first: there is no scene-bounds log today, so print
`diagonal` at `%.9g` from `Renderer::setScene`, run one frame against
`engine/assets/Sponza/glTF/Sponza.gltf`, bake the value, remove the log.

## Reference update

- [architecture/principles.md](../../architecture/principles.md) — the durable rule this row
  buys: *a world-unit constant in the engine is derived from the scene or it is a settings
  row, never a literal.* The `Source` precedence list gains its fifth tier here too.
- [architecture/rendering.md](../../architecture/rendering.md) — SSAO, SSR and fog stop
  documenting fixed distances.
- [guides/making-a-game.md](../../guides/making-a-game.md) — what a scene's own lights now
  override, sun included.

Four stale doc claims are cleared as part of this row, since each is a sentence describing
exactly what it moves: `engine/core/Resources.h` says the Sponza path is *"compiled into
`Config`"* and `tests/manifest_test.py` says *"Config.h names Sponza"*, both untrue since the
scene moved to `GameSetup`; `run.sh` says `scene.path` in `substrate.json` decides what a game
opens, from before the same move; and `Engine.cpp` cites `DemoGame::init` by name from engine
source, which should name the condition rather than the class.

The roughly forty other comments citing Sponza or the showcase stay. Each records the
measurement justifying a number, which is how this tree explains itself; deleting the
reference would orphan the rationale. This row removes only the ones that were load-bearing on
a *value*.

## Outcome

**The card's finding was right and its remedy was wrong, and the thing it got wrong is in
its title.** `ssaoRadius` was a world-unit length written as a literal in a header that no
config key, no flag and no panel could reach — that is real, and it is fixed. But it does
not *come from the scene*, and deriving it would have been a worse bug than the one being
fixed. What the row actually established is the test that separates the two cases, which
neither the card nor `principles.md` had:

> **A world-unit constant is derived from the scene, or it is a settings row, never a
> literal — and which of the two it gets is decided by what the right answer is a function
> of, not by whether the value is a length.**

**The split, audited value by value.**

| Value | Verdict | Why |
|---|---|---|
| `particleSortRange`, `particleCollisionThickness` | **derives** — unchanged | A sort key quantises the whole visible depth span. A function of scene size and of nothing else |
| `fogBaseHeight` | **derives** — unchanged | The scene's floor. Same |
| `Camera::moveSpeed`, `Camera::nearPlane` | **derives** — unchanged | Crossing a room should take about as long whatever the room is. `camera.moveSpeedScale` is already the row, and it is a dimensionless *multiplier* over the derived speed rather than a second spelling of it |
| `placeLights`' radius, height and fill distance | **derives** — unchanged, and it is game code | Fill lights are placed *relative* to the sun and to the bounds. Not the engine's, and not this row's |
| `render.shadowDistance` | already the model | 0 means "fit the box to the scene bounds", a positive value caps it. A derivation with an explicit override, in one row |
| `render.lodThreshold` | already correct | A coverage *fraction*. Dimensionless, so it never wanted a derivation. C17 measured it |
| `ColliderFreedom` | not in scope, and not a length | An enum of permitted degrees of freedom. P7's, and it names axes rather than distances |
| **`ssaoRadius`** | **constant hiding as a literal** → `render.ssaoRadius` | Contact occlusion is contact scale at *every* scene size. Scaling it with the bounds would give a warehouse metre-deep creases and a doorknob none |
| **`ssaoBias`** | **constant hiding as a literal** → `render.ssaoBias` | A depth offset in world units, doing exactly what `shadowDepthBias` does — which was already a row |
| **`ssrThickness`** | **constant hiding as a literal** → `render.ssrThickness` | How far behind a surface a march may still count as a hit. A wall's thickness is a property of the content, not of the box around it |
| `render.fogDensity` | left alone, as the card asked | Extinction *per* world unit, so it scales inversely rather than with the lengths beside it. Its Sponza sentence is reworded; its value did not move |

`ssrMaxDistance`, `fogMaxDistance` and `fogHeightFalloff` needed nothing done to them as
*values*: all three were already rows. What was wrong with them was the second half of the
card's complaint, and that is the part that turned out to be the larger finding.

**The duplicated default had already drifted, which is the defect nobody had looked for.**
The card says the Sponza-sized default is *"duplicated between the table in `Settings.h`
and the struct in `Renderer.h`"*. It is — 38 times — and one pair disagreed:
`render.debugOverlay` was `true` in the table and `false` on the field. Nothing caught it
because `bindRenderer` runs before the first frame reads the field, so the wrong copy was
never the one used. That is *"two spellings of one value"*, rule 7's second prohibition,
sitting inside the mechanism built to remove it. `core::defaults::<module>::<row>` is
generated from the same X-macro list as `core::options::<module>::<row>`, every field
`bindRenderer` binds now initialises from it, and `gfx::kDefaultLightBudget` derives from
`render.lightBudget` rather than restating 32. `SettingsTest` compares the two one row at
a time, generated from the list they are both generated from, so the pair cannot come apart
again.

**The `Scene` source tier was designed, then declined.** It is the right mechanism for what
the card wanted and there is nothing left for it to do: with no engine value deriving a
*row*, a fifth precedence tier would have had no writer. `Settings.h`'s `setDefault` comment
promised that tier by name and now records the refusal instead — the comparison against
`Source::Default` specifically is still right, and still for the reason that a tier added
below `Game` later must not be silently overwritten. **D15's card in `done/` is left as
written**; it recorded an expectation about D11 and the expectation is what a card is for.

**What was deliberately not done, and it is the card's own second half.** Zeroing the sun
when a scene ships lights but no directional is correct — `Engine.cpp` states the rule *"a
file that ships its own lights wins"* and applies it to the punctual list only, so an indoor
scene lit by point lights silently inherits the demo's sun at intensity 3.0. It also moves
`emissive`, `particles` and `physics`. This row was held to **byte-identical across eleven
with re-snapping unavailable**, which is a stronger standard than the card wrote for itself
and the right one for a D row: the card planned for four cases to move and called it
intended, and a row that changes an image cannot also claim that the 38 default initialisers
it rewrote changed nothing. The finding is recorded at the branch in `Engine.cpp` in the
words above, and it wants its own card and its own baselines.

**What the estimate did not predict.** Three of the four *"what I expect to be wrong
about"* guesses were about the `Source` enum and never came up, because the tier was not
built. The fourth was checked and was right in the useful direction: `setValue` clamps
rather than rejects, *and it logs the clamp* — so a bound a large scene exceeds is loud
rather than silent, which is a weaker argument for widening than the card assumed. Two were
widened anyway, on rule 3 rather than on silence: `render.ssrMaxDistance` stopped at 64,
under twice Sponza's own diagonal, while `render.shadowDistance` and `render.fogMaxDistance`
— the same kind of number — stopped at 500. A ceiling taken from the test scene is a
property of the test scene. Both are 500 now; `rtMaxDistance` keeps its 10000 because a ray
query costs the same at any range, which its own field already said.

**The measurement the card asked for, taken and then not needed.** Sponza is
`min (-15.37, -1.01, -9.46)`, `max (14.40, 11.44, 8.84)` — 29.8 x 12.5 x 18.3, a 37.1
diagonal. No reference literal was baked, because nothing derives; what the number did was
convict a comment. `ssaoRadius` justified itself with *"Sponza is roughly 20 units across"*
while `ssrMaxDistance` and `fogDensity` three screens away both said 30. The value was
right and the measurement behind it was not, which is the sharpest available argument for
this row existing at all.

**Verification.** `scripts/golden.sh check release` — **11 of 11 byte-identical**.
`scripts/readback.sh release` — **9 of 9 bit-identical plus the lit silhouette**, and the
resize soak clean. `scripts/locomotion.sh release` — **3 of 3 arms**, which matters here
because the walk-run-jump arm reads back 8.40 m travelled off a character move speed that
is exactly the class of value this row was auditing. `./test.sh` at debug, release, asan and
tsan — **837 tests, 88 suites, all passing** in each, four of them new: every generated
default against what the table stores, the three promoted lengths against the literals they
replaced (`0.5f`, `0.025f`, `0.5f`, exact equality rather than `FLOAT_EQ`), a promoted row
through the string door reporting `cli --set`, and the widened ceilings. A debug run under
the validation layers with all four rows set from the command line: **zero errors**, and
`--dump-settings` reports each as `cli --set`.

**Both arms, because a row that reverts is what this arc keeps finding.** The promoted rows
are bound live and read into push constants, and nothing recomputes them — proved rather
than asserted: `--set render.ssaoRadius=2.5` against the `ssao` golden mismatches **935,029
of 1,440,000 pixels at frame 60**, and `--set render.ssrThickness=8` against `no-rt`
mismatches 1,879. Absent the flag both cases are byte-identical, which is the other arm and
is the golden run above.
