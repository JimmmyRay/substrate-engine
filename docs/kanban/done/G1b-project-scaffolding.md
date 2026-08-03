---
id: G1b
title: Project scaffolding
arc: G
size: S
verification: scaffold
---

# G1b — Project scaffolding

S

## Getting started — two paths, one of which does not exist

The arc is judged by this, so it is stated rather than assumed.

**Path 1, contributing to the engine, exists and works.** `README.md` carries a five-line
quickstart, `docs/guides/building.md` carries the exact `apt install` line and a
verified-versions table, and `build.sh` / `run.sh` / `test.sh` / `scripts/fetch_assets.sh`
cover build, run, test and assets. **Nothing in this arc may regress it**, and G1 changes
it in exactly one way that must be paid for rather than absorbed: `./build.sh && ./run.sh`
stops working, because `build.sh` no longer produces a binary. The quickstart becomes

```bash
git submodule update --init --recursive
./build.sh                 # engine and unit suite
scripts/fetch_assets.sh
./build_game.sh demo       # the new line
./run.sh
./test.sh
```

and **the root `README.md`, `docs/guides/building.md` and `docs/architecture/README.md`'s
"Working on it" section are all edited in G1**, not left to drift. A quickstart that no
longer works is the most expensive documentation defect there is: it is the first thing a
new developer runs, and its failure is indistinguishable from a broken build.

**Path 2, making a game, does not exist.** There is no way to start a project that is not
"fork the repository and edit `main.cpp`", which is the sentence the engine/game separation
argument opens with. G1b closes it, and the target is:

```bash
git clone <substrate> && cd substrate
./setup.sh                    # submodules, dependency check, assets
./new_game.sh mygame          # scaffolds game/mygame/ from the template
./build_game.sh mygame
./run.sh
```

The template is a `Game` subclass that loads nothing and draws one procedural mesh, plus a
`CMakeLists.txt` and a `README.md`. `setup.sh` is a thin wrapper over the three commands the
root `README.md` already lists, existing so that the first instruction a new developer reads
is one line rather than four.

`run.sh` takes no game argument because it does not need one: `build_game.sh` recorded the
choice in the build directory, so `run.sh`, `golden.sh` and `baseline.py` all keep the
signatures they have today.

**Two reasons G1b is not decoration.**

First, the module boundary was refused partly on the grounds that *"a wish for modularity is
not a second consumer"* — the Rule of Threes applied to consumers. A scaffolded game beside
`game/demo/` is literally that second consumer, which is what turns G1's boundary from
speculative into exercised.

Second, a template that **must not reference anything under `engine/`** is a continuously
checked assertion that the public surface is complete. The day it needs an
`#include "gfx/Renderer.h"` to do something ordinary, that is a defect report about the API
rather than a note in the template.

**Documentation is not a final stage.** Each stage lands its own section of
`docs/guides/making-a-game.md`, because a guide written after six stages is written from
memory. The verification list below is where that obligation sits.

---

## Verification

Everything below must pass before this may enter `done/`:

Named before it may leave `backlog/`:

- A scaffolded game builds and runs without touching anything under `engine/`.

## Reference

[architecture/tooling.md](../../architecture/tooling.md), [guides/making-a-game.md](../../guides/making-a-game.md).

## Outcome

**Two scripts and a template.** `./setup.sh` is the wrapper this card asked for —
submodules, a named dependency check, assets — and it refuses with a list rather than
letting a missing `glslangValidator` surface as a shader that fails to compile four
minutes into a build. `./new_game.sh <name>` copies `scripts/template/game/` into
`game/<name>/` with the name substituted: a `Game` subclass, its header, a one-line
`CMakeLists.txt` and a README.

The template lives under `scripts/` rather than `game/` deliberately. A template *in*
`game/` would need a `CMakeLists.txt` to be a template of anything, and that is exactly
the file `list_games()` tests for — so it would appear in `./build_game.sh --list` as a
game nobody wrote.

**The name is validated rather than discovered.** It becomes a directory, a CMake target
and part of a C++ class name, so `new_game.sh` rejects anything that is not
`[a-z][a-z0-9_]*` with a sentence saying why. `mygame` becomes `MygameGame` — ugly for a
one-word name and unambiguous for every name, which is the trade a generated identifier
should make.

**What the row did not predict: the template found a live defect on its first run.**
`GameSetup::scene` has always documented "or empty to start with nothing loaded", and
nothing had ever taken it up — the demo names a scene and so does every golden case. An
empty path reached `core::Resources("")`, failed to load, and exited through
`Logger::critical`. So the first game the scaffold produced could not start. `loadScene`
now says so and carries on; everything downstream of the load was already shaped for a
scene with no meshes in it. **That is the second-consumer argument paying out on the day
it acquired the second consumer**, which is the reason this row was pinned next to G1
rather than deferred to a tidy-up.

**What the template draws is `ui::drawSettings`, which G2 landed an hour earlier.** That
was not planned and is worth recording: the smallest useful thing a game with no content
can put on screen is every render setting there is, and before G2 that would have been
forty hand-written widgets in a file whose whole point is to be small.

**The card's "one procedural mesh" is not what shipped.** Making a mesh from vertices is
G4 and does not exist, so the template loads nothing and draws a panel. Writing it against
an API that is not built would have been the guide-describing-the-unbuilt problem this
project retired the roadmaps over.

### Verification

- `./new_game.sh scaffoldcheck && ./build_game.sh scaffoldcheck debug` — builds, with
  **nothing under `engine/` edited to make it build**.
- `./run.sh debug -- --headless --frames 60 --validation on` — clean run, no validation
  errors, no scene, the panel drawn.
- `scripts/golden.sh check release` — all 12 cases match. `loadScene` changed, so this had
  to run again rather than be argued about.
- `./test.sh debug` — 626 tests green.
- `./setup.sh --no-assets`, `./new_game.sh --help`, and a rejected name — each does what it
  says.

### The one thing this does not do

The card calls the template "a continuously checked assertion that the public surface is
complete". It is checked when it is instantiated, and nothing instantiates it on its own —
there is no CI here to run `new_game.sh` on every commit. Making it continuous means either
a committed scaffolded game beside `game/demo/`, which is a second thing to maintain for a
check nobody asked for yet, or a CI step, which is its own row. Recorded rather than
claimed.
