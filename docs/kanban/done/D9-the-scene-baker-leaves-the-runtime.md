---
id: D9
title: The scene baker leaves the runtime
arc: D
size: M-L
verification: golden-11, tests-4, validation, readback
---

# D9 — The scene baker leaves the runtime

A host-only `substrate-bake` binary writes the C15 sidecar, and `writeSceneCache`,
`--bake-scene` and `scene.bakeCache` leave the game binary entirely. Afterwards the tree holds
one writer of a `.scene`, it links no Vulkan, and a shipped game contains no path that can
produce one.

Mechanically that means `writeSceneCache` moves out of `SceneData.cpp` into its own
translation unit, linked by the tool and by the unit suite and by nothing else.
`readSceneCache` stays exactly where it is, because every game needs it and no game needs the
writer — which is the whole asymmetry this row is about. `SceneDataTests.cpp` already exercises
both halves against each other ([`:169`](../../../tests/SceneDataTests.cpp#L169) writes and
reads back, [`:274`](../../../tests/SceneDataTests.cpp#L274) checks a rejection), so the suite
gains the new TU and keeps its coverage.

## The finding

The engine has exactly two places where a running process writes a file that a later run reads
back as an *input*: this one, and D10's hot-reloaded SPIR-V. Everything else that writes —
[`Config.cpp:653`](../../../engine/core/Config.cpp#L653),
[`Input.cpp:721`](../../../engine/core/Input.cpp#L721),
[`SaveFile.cpp:106`](../../../engine/core/SaveFile.cpp#L106),
[`Profiler.cpp:439`](../../../engine/core/Profiler.cpp#L439),
[`Recorder.cpp:226`](../../../engine/core/Recorder.cpp#L226),
[`Logger.cpp:50`](../../../engine/core/Logger.cpp#L50) — is either authored state or an output
nothing reads. Textures were solved offline before the board existed: nothing under `engine/`
writes a `.ktx2`, [`scripts/ktx2.py`](../../../scripts/ktx2.py) does, and `manifest.py
--require-cache` turns a cold image into a release-build failure.

**The tree already has the mechanism this card wants, and applies it to the other site
instead.** `SUBSTRATE_SHADER_HOT_RELOAD` ([`CMakeLists.txt:406`](../../../CMakeLists.txt#L406))
is forced OFF by `SUBSTRATE_PORTABLE`
([`:423`](../../../CMakeLists.txt#L423)), which compiles out `SUBSTRATE_SHADER_SRC_DIR` and
`SUBSTRATE_GLSLANG` so `recompileShaders` and `kShaderTrees` vanish behind their `#if
defined`. The comment beside it calls that "the last of the three build-machine paths that
would otherwise reach a shipped binary." The scene baker has no gate at all:
`SceneData.cpp` is in `SUBSTRATE_HOSTED_SOURCES`
([`:344`](../../../CMakeLists.txt#L344)) unconditionally, and `--bake-scene` is registered
unconditionally at [`Config.cpp:133`](../../../engine/core/Config.cpp#L133). A packaged game
ships the writer and the flag that reaches it, and a player who types it writes a `.scene`
into the install directory.

**And the baker is the renderer.** [`build_release.sh:126`](../../../build_release.sh#L126)
runs `./run.sh release -- --headless --locked --frames 3 --bake-scene`, standing up a device,
a swapchain and every texture upload, to produce a CPU-side artifact that
[`GltfScene.cpp:929`](../../../engine/scene/GltfScene.cpp#L929) `loadCpu` computes while
touching neither. The bake therefore needs a GPU it does not use, and cannot run on a machine
or in a container that has none.

*When* the bake happens is already right — package time, wired to the same resolver that
decides what is staged, which is C14's rule and stays. This row changes what does the baking
and what ships.

## What does not change

- **The reader.** C15's contract — format version, layout digest and source stamp all checked
  before a payload byte is read, and a cache that does not apply is neither an error nor
  logged — is untouched. This is a writer-side row.
- **C13's rule that the writer is C++ sharing the runtime structs**, which is strengthened
  rather than weakened: the tool links `SceneData.cpp` itself, so a `Vertex` layout change
  stays a compile error rather than a corrupt scene. A Python writer is still refused, for the
  reason C13 gave — it would be a second encoding of the vertex layout with no compiler to
  make the two agree.
- **The embedded-image refusal.** `writeSceneCache` refuses a scene holding an embedded image
  with no `.ktx2` beside it, because the sidecar could not be read back without the document it
  exists to avoid opening. The tool keeps that verbatim, and `build_release.sh` keeps baking
  textures first so the case does not arise.

## The obstacle, and why this is M-L rather than S

`GltfScene.cpp` is **not** hosted. It sits with the device sources at
[`CMakeLists.txt:381`](../../../CMakeLists.txt#L381), beside `stb_impl.cpp`. `loadCpu` already
touches no device — C10 split it out for exactly that reason, so the parse could run on a
worker thread — but it shares a translation unit with `load`, which uploads. The substance of
this row is separating that CPU half into a hosted translation unit so the tool can link it
without pulling in Vulkan, and that is most of the line count.

The precedent for a second hosted binary exists already: the unit suite links
`SUBSTRATE_HOSTED_SOURCES` "and nothing else, so it needs no device, no window and no shaders"
([`CMakeLists.txt:649`](../../../CMakeLists.txt#L649)). The same property that lets the suite
run under TSan is the one that lets a baker run without a GPU, and the split this row performs
extends the boundary check the engine already gets — *a dependency leaking across the line is a
link error rather than a code review* — from "no game" to "no device".

The expected wrong estimate is the image half. `loadCpu` takes an `EmbeddedImages&`, and how
much of the stb decode path comes with it decides whether this lands at M or L.

## What this must not grow

- A second encoding of the sidecar format. One writer, one reader, one struct.
- A bake that rewrites the glTF. C13: "nothing rewrites the glTF, and deleting the cache
  restores the original behaviour exactly."
- `--bake-scene` kept as a deprecated alias. The flag leaving the settings table is half the
  point of the row.
- A bake run by `build.sh`. The development loop parsing the document is correct and is what
  makes a stale sidecar impossible to hide behind.

## Verification

- The tool's output **byte-identical** to what `./run.sh release -- --bake-scene` produces
  today, for every scene [`scripts/manifest.py`](../../../scripts/manifest.py) resolves. That
  is the whole correctness claim of the row, and it is checkable with `cmp`.
- `./test.sh debug`, then `./test.sh asan`, then `release` and `tsan` — each its own
  invocation. `SceneDataTests.cpp` already covers the round trip and must keep passing with
  the writer in its new home.
- `scripts/golden.sh` — **eleven** cases; the card was written while the `no-ibl` case still
  counted. Byte-identical: the scene a run loads must not change because its sidecar
  acquired a different producer.
- Zero validation errors with layers on.
- `./build_release.sh demo` completes, with `manifest.py --require-cache` still the thing that
  fails on a cold asset.

## Reference update

[`tooling.md`](../../architecture/tooling.md) gains the tool and loses the description of
baking as a renderer launch.

[`principles.md`](../../architecture/principles.md) gains the invariant this row exists to
make true: **the running process never writes a file a later run reads as an input.** That
sentence is the durable artifact — the code change is one application of it, and D10 is the
other.

## Outcome

Landed. Roughly 670 lines net across 25 files, of which about 1,300 are a move: `M-L` was
right, and it was `L`.

**The invariant, which is the point of the row and of D10 together.**
[principles.md §9](../../architecture/principles.md#9-the-running-process-never-writes-a-file-a-later-run-reads-as-an-input)
now says *the running process never writes a file a later run reads as an input*, and states
what it does not forbid — `substrate.json`, `bindings.json`, a save, a trace, a log, a
recording, a capture are all either a person's file or a dead end. It names both closures
(D10's SPIR-V, D9's sidecar), records that both are enforced by construction rather than by
review, and states the property it buys: *delete every generated artifact and every build
directory, rebuild, and the engine behaves identically.* Its corollary is new and was earned
here rather than assumed — an offline artifact has to be **reproducible**, because that is
the only way to check the rule is holding.

**What moved.**

- `scene/SceneParse.{h,cpp}` — the CPU half of a load, lifted out of `GltfScene.cpp` whole.
  `parseSceneData` and the four helpers only it uses (`nodeTransform`, `toGpuLight`,
  `textureIndexOf`, `embeddedBytes`) went with it; `ktx2CachePath`, `Decoded`, `decodeImage`
  and the buffer-usage flags stayed, because the decode belongs to the *upload* half.
  `GltfScene::loadCpu` became the free function `scene::loadSceneCpu` — it was never a
  property of the GPU scene class, and it could not stay a member of a class whose header
  owns `VkBuffer`s.
- `scene/SceneCacheFormat.h` — the wire format: magic, version, layout digest, stamp,
  `Writer`, `Reader` and every `put`/`get` pair. **A header rather than a third `.cpp`,
  and that was forced.** C15's file comment argues that each `put` must sit beside its `get`
  because that is where a field reaches one and not the other; splitting the writer into its
  own translation unit would have split all eleven pairs. A header keeps them adjacent and
  lets two `.cpp`s take one half each.
- `scene/SceneCacheWrite.cpp` — `writeSceneCache` and `sceneCacheSize`, and nothing else.
- `tools/bake.cpp` and `scripts/bake.sh` — the program and the script that builds and runs
  it. The script exists for `run.sh`'s reason (what you bake with is what the tree currently
  says) and it passes `SUBSTRATE_GAME` back into `build.sh` from the cache, so baking a scene
  cannot silently un-configure the game a build directory holds.
- Gone: `--bake-scene`, `Config::Scene::bakeCache`, and the `bakeCache` parameters on
  `GltfScene::load` and `loadCpu`. `Settings::removedKeys()` still names `scene.bakeCache`
  and now says where it went — a tool, not another flag.

**The mechanism is the linker, and the card understated how much that forced.** A static
library links by object file, so a `#if` would not have been enough and neither would a
private function: while `writeSceneCache` shared `SceneData.cpp` with `readSceneCache`, every
binary that could read a sidecar linked the object that writes one. Three checks on the
package `build_release.sh demo` produced:

- `nm -C` on the staged `demo`: **zero** `scene::writeSceneCache`.
- `ar t libsubstrate.a`: `SceneData.cpp.o` and `SceneParse.cpp.o`, **no** `SceneCacheWrite.cpp.o`.
- `strings` on the staged `demo`: **zero** occurrences of `--bake-scene`.

`substrate-bake` has all three symbols and links neither `volk` nor `glfw`. That absence is
itself the check: the day something device-side is pulled into a hosted translation unit,
the target stops linking.

**Byte-identical, and the two reasons it was not.** The baseline was taken first, from the
unmodified tree: `./run.sh release -- --headless --locked --frames 3 --bake-scene
engine/assets/Sponza/glTF/Sponza.gltf`, twice. **Those two runs already disagreed — 34
bytes.** So the row's central claim could not be checked at all until the bake was made
reproducible, which is the finding the estimate did not have:

1. **The timings.** `SceneStats` is written verbatim and carries eight `double`s of *this
   run's* wall clock. Five of them are non-zero at bake time, they occupy the two byte ranges
   the two baseline runs differed in, and no reader has ever used them — `loadSceneCpu`
   clears the parse timings on a cache hit anyway, because reporting a previous run's parse
   time for a run that did no parsing is a lie the log tells every time the cache works.
   `cache::bakedStats` zeroes them on the way out.
2. **The padding.** Three `put`s write a *byte range* of their struct rather than field by
   field, which includes whatever the compiler left between a `bool` and the `float` after
   it — indeterminate in an object nobody zero-filled. Measured: `particles.gltf` baked alone
   against the same scene baked second in one invocation of the tool differed in **16 bytes**,
   every one of them inside `ParticleEmitter`, from a document neither run had touched.
   `cache::zeroPadded` fixes it, and `SceneData::stats` is now brace-initialised for the same
   reason one level up.

With both in place:

| Comparison | Result |
|---|---|
| `substrate-bake` on Sponza, two runs | **byte-identical**, 15,013,138 bytes |
| `substrate-bake` on Sponza, solo vs. third in a batch of three | **byte-identical** |
| `substrate-bake` vs. the old in-process bake, Sponza | **40 bytes differ, all five run-timing doubles**; the other 15,013,098 identical |
| the old in-process bake vs. itself, two runs | 34 bytes differ, the same five doubles |
| all three, with those doubles masked | one SHA-256, `f75d739bcdd5f5f4...` |
| `particles.gltf` solo vs. batch, after the fix | **byte-identical** (16 before) |
| `emissive`, `physics`, `skin` vs. their old-path sidecars | identical outside the timings and the source stamp |

The one residual byte anywhere is offset 2472 of `particles.gltf.scene`, where the old
baseline held `0xFF` and the tool writes `0`. It is inside the emitter padding run above —
that is, a byte the old writer could not reproduce either.

**The format did not move, and that is checked rather than asserted.** The layout digest is
`0xfb1f5991` before and after, and the final verification runs load Sponza through the
sidecar the *old* in-process baker wrote, restored byte for byte. A game reading a sidecar
cannot tell what produced it, which is the same property C15 relies on one level down.

**Who runs it, and when.** `build_release.sh`, once, after `scripts/ktx2.py` and before
`manifest.py --require-cache` — one invocation for every scene the manifest resolves rather
than one renderer launch each. C14's rule is untouched: which scenes get baked is decided by
the resolver that decides what is staged, so the bake list cannot drift from the staged list.
`build.sh` still does not bake, deliberately, and `scripts/fetch_assets.sh` does not either —
a development build parses the document, which is what keeps a stale sidecar from hiding.
Demonstrated on Sponza, all four states, leaving the tree exactly as found:

| State | What the load did |
|---|---|
| sidecar present | `scene cache hit: Sponza.gltf.scene (20.9 ms)`, LOD chains 51 primitives / 650,946 indices |
| sidecar deleted | no hit, `parse=6.6ms geometry=20.0ms`, `LOD chains: 0 primitives (none; bake with substrate-bake)` |
| source mtime bumped 1 ms, sidecar untouched | no hit, `parse=4.7ms geometry=23.0ms`, no chains |
| mtime restored | hit again, 19.4 ms, chains back |

So C15's invalidation still catches the case a bake step nobody invokes would create, and it
catches it silently and correctly rather than by failing.

**Cold start, against C13's lineage.** C13 measured the 8000-node scaling scene at 79.9 ms
and C14 took it to ~44 ms; C15 took it to 9 ms and Sponza 123 → 111 ms, noting that Sponza's
load is mostly texture upload no scene format touches. That is still exactly what it is:
Sponza from the sidecar is `cache=20.9ms parse=0.0ms geometry=0.0ms textures=89.8ms
total=92.0ms`, and from the document `parse=6.6ms geometry=20.0ms textures=90.2ms
total=92.0ms`. The sidecar buys 26.6 ms of CPU on this scene and the whole difference between
LOD chains and none — and neither number moved because the baker changed hands, which is the
result the row wanted.

**`meshoptimizer` follows the baker.** C17 made it a submodule for `buildLodChains`, which is
now called by `tools/bake.cpp` and by nothing else. `substrate-bake` links it directly;
`libsubstrate.a` still links it too, because `scene/MeshLod.cpp` also holds the *selection*
half that runs every frame. The generator moved out of the runtime; the dependency did not
have to.

**Two things the verification caught that were not this row's.**

1. **`build_release.sh` did not compile.** `Renderer.cpp:2566` calls `Logger::warn` unqualified
   in the `#else` of `recompileShaders`, which is reached only when `SUBSTRATE_SHADER_HOT_RELOAD`
   is off — that is, only in a package. D2's namespace pass could not see a branch that never
   compiles, and it has been broken since. Fixed here because it blocked the card's own
   verification, with a comment saying why it hid.
2. **The ASCII guard did not cover `tools/`.** It defaulted to `engine game tests`, and a new
   top-level source directory would have gone unchecked. Now four.

**What the estimate got wrong, in both directions.** The card predicted the image half would
decide M or L: *"`loadCpu` takes an `EmbeddedImages&`, and how much of the stb decode path
comes with it"*. None of it did — `embeddedBytes` lifts payloads out of the document and
belongs to the parse, while `decodeImage` and `stb_impl.cpp` are the *upload* half and stayed
put, so the split fell exactly where C10 and C15 had already put the seam. What it did not
predict is that "byte-identical" was not a property the old bake had, and establishing it
first — before changing anything — is what turned the row's correctness claim from an
assertion into a `cmp`.

**Verification.** `scripts/golden.sh check release` **11 of 11**, run twice: once after the
split and once after the final change to `Renderer.cpp`. `scripts/readback.sh release` **9 of
9 bit-identical plus the lit silhouette**, also twice. `./test.sh debug`, `release`, `asan`,
`tsan`: **859 of 859** each, one invocation apiece — 858 before, and the new one is
`SceneCache.TwoWritesOfOneSceneAgreeToTheByte`, which poisons the padding of an emitter, a
collider and an audio source in place and requires the two files to agree; it was confirmed
to **fail** against the pre-fix writer before being kept. A 120-frame `--validation on` run:
**zero errors, zero VUIDs, zero criticals**, one known `VK_LAYER_PATH` warning.
`./build_release.sh demo` completes — 150 files resolved, `--require-cache` satisfied, a
177 MB AppImage, and `substrate-bake` is not in it.
