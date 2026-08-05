---
id: D24
title: CMakeLists stops naming source files
arc: D
size: M
verification: tests-4, golden, scaffold
---

# D24 — CMakeLists stops naming source files

`CMakeLists.txt` held an inventory of the tree: 47 files in `SUBSTRATE_HOSTED_SOURCES`,
eighteen more in `add_library`, two single-file variables, and a block naming each module's
wiring translation unit. Four targets were built out of those lists, so every hosted file was
compiled four times, and the module set was written down in three places — `check_layers.sh`,
the hosted list, and the wiring block — which drift silently.

They drifted during [D23](D23-five-subsystems-become-five-modules.md). A rename landed, the
list did not, and the build failed with `Cannot find source file: engine/scene/Animation.cpp`
— an error that says nothing about the code and blocks everyone sharing the checkout.

**The build should not have an opinion about where a file lives, and it does not need one.**
A static library yields an archive member only to resolve an undefined symbol, which is the
same mechanism the modules rest on. So: glob every `.cpp` under `engine/` into one library,
name none of them, and let the linker decide what each binary carries.

## Verification

- `./test.sh debug`, `release`, `asan`, `tsan` — each its own invocation, and each is the
  check, because the driverless targets link no volk and no glfw. A member needing a device
  is an undefined symbol there.
- `scripts/golden.sh check release` — 13 of 13 byte-identical.
- `scaffold` — a game out of `./new_game.sh` still builds and runs.
- `nm -C` on all three games: no `.scene` writer symbol, D9's invariant unchanged.

## Reference update

- [architecture/README.md](../../architecture/README.md) — the hosted-set paragraph, which
  named seventeen files.
- [architecture/principles.md](../../architecture/principles.md) and
  [architecture/rendering.md](../../architecture/rendering.md) — every claim phrased as
  "is in `SUBSTRATE_HOSTED_SOURCES`".
- [guides/building.md](../../guides/building.md),
  [guides/making-a-game.md](../../guides/making-a-game.md) and the scaffold template — the
  deleted `SUBSTRATE_ENTRY_POINT` option.
- `CLAUDE.md` — the sentence about what the unit suite links.

## Outcome

Landed with `7b5106f`. 132 lines of `CMakeLists.txt` went, and the hosted sources are
compiled once instead of four times.

**Three invariants that were spelled as source lists became the archive's own behaviour, and
each is stronger for it:**

| Was | Is |
|---|---|
| `SUBSTRATE_HOSTED_SOURCES`, 47 files by hand | The driverless targets link no volk and no glfw, so anything needing a device fails to link — and it now covers files nobody remembered to list |
| `SUBSTRATE_SCENE_WRITE_SOURCES` kept out of `substrate`, so no game could write a `.scene` (D9) | No game calls the writer, so no game links it. Verified: 0 symbols in viewer, demo and battle_arena |
| `SUBSTRATE_ENTRY_POINT`, an option to compile out `main()` | `tests/main.cpp` and any game defining its own `main` resolve the symbol themselves, so `Entry.cpp` is never pulled. The option had nothing left to do and is deleted |

**What the estimate did not predict:** the first instinct was wrong and cost an hour. A plain
glob looked impossible because the module wiring translation units name `Engine` and must not
reach the driverless targets, and `CMakeLists.txt` is not allowed to know which files those
are — so the design started as a transitive include scanner deriving hosted-ness from
`#include` lines. It was unnecessary. The targets do not need a *source list* at all; they
need to link the archive, and the linker had already been doing the selection.

`scripts/rdoc.sh` is the one caller that needs the old behaviour and says so in one line.

## Deferred

- Nothing. The three exceptions the glob could not classify all dissolved rather than moving.
