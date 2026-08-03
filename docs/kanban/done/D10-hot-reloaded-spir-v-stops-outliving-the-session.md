---
id: D10
title: Hot-reloaded SPIR-V stops outliving the session
arc: D
size: S
verification: golden-11, validation, inspection
---

# D10 — Hot-reloaded SPIR-V stops outliving the session

`recompileShaders` stops writing into `build/<cfg>/shaders/`. A recompiled shader reaches
`vkCreateShaderModule` without leaving an artifact behind, so afterwards the build directory
holds only what the build put there and a cold start can only run shaders CMake produced.

## The finding

D9's other half. The engine has exactly two places where a running process writes a file a
later run reads back as an input, and this is the second:
[`Renderer.cpp:2177`](../../../engine/gfx/Renderer.cpp#L2177) renames a freshly compiled
`<name>.spv` into the build's shader output directory, and
[`Pipeline.cpp:21`](../../../engine/gfx/Pipeline.cpp#L21) `readShaderBinary` reads that same
directory on the next **cold** start.

The package is already safe, and that is worth stating so the row is not oversold:
`SUBSTRATE_PORTABLE` forces `SUBSTRATE_SHADER_HOT_RELOAD` off
([`CMakeLists.txt:423`](../../../CMakeLists.txt#L423)) and the whole path compiles out behind
`#if defined`. This is a **development-build** defect, and it is one step past the class G5b
already paid for once — *"a compiled shader outlives its source, and in the game's tree that
is a defect."* G5b's version was a shader outliving its source across a *reconfigure*. This
one outlives the **process**: press reload, quit, relaunch, and the engine boots SPIR-V that no
build produced and that no source in the tree necessarily still matches.

Two writers share one directory and the build system cannot tell which files it made. Both
things `scripts/golden.sh` is trusted for — that eleven cases are byte-identical, and that the
result is reproducible from a clean tree — quietly depend on nobody having pressed reload since
the last build. Nothing reports the difference, which is the same failure shape C15 named for
caches: *a cache that is wrong looks exactly like a cache that is fast.*

## The shape

**In memory.** glslang writes to a temp path outside the build tree; the bytes are read, the
temp is unlinked, and the SPIR-V goes to a new `loadShader` overload taking a
`std::vector<uint32_t>` rather than a name. `readShaderBinary` and its two-directory
`executableDir()` resolution ([`Pipeline.cpp:21-56`](../../../engine/gfx/Pipeline.cpp#L21-L56))
are untouched, which matters because that resolution is G5b's row and re-opening it is not
this one's business.

The write-aside-then-rename dance at
[`Renderer.cpp:2150`](../../../engine/gfx/Renderer.cpp#L2150) exists so a shader that fails to
compile leaves the working SPIR-V untouched — glslang truncates its output before it knows
whether the parse succeeded. With nothing in the build tree to protect, that collapses to "on
failure, keep the module already bound", which is what the caller wanted in the first place.

The rejected alternative is a session-scoped overlay directory searched ahead of the build's.
It keeps the artifact, so it keeps the bug in a smaller box: a crash leaves the overlay behind,
and the next launch is back to running shaders no build produced.

## What this does not claim

It does **not** let [`CMakeLists.txt:223`](../../../CMakeLists.txt#L223)'s
`file(REMOVE_RECURSE ${GAME_SHADER_OUT_DIR})` go away. That wipe handles CMake's *own* stale
outputs when a shader source is deleted — nothing removes an output whose `add_custom_command`
stopped being generated — and it is independent of hot reload. Written down here because the
two problems look identical from the symptom and someone will try to remove it.

## Verification

- Hot reload by hand, which is the honest answer rather than a test: G5b records that the demo
  ships no shaders, so `pollShaderReload` has never had automated coverage. Edit an engine
  shader, reload, confirm the picture changes; then confirm no mtime under
  `build/<cfg>/shaders/` moved, and that a restart reverts to the built shader.
- `scripts/golden.sh` — eleven cases, byte-identical, from a clean build.
- Zero validation errors with layers on. A module created from an in-memory buffer is the one
  new way this row can produce an invalid `VkShaderModule`.
- Not `tests-4`: `Renderer.cpp` is not in `SUBSTRATE_HOSTED_SOURCES`, so the unit suite cannot
  reach this path in any configuration. That gap is part of the finding.

## Reference update

[`tooling.md`](../../architecture/tooling.md). Hot reload appears there once, as a property of
the `debug` configuration in the table at `:36`, and says nothing about where a reloaded shader
goes. After this row it should — the fact worth recording is that it goes nowhere, so a build
directory holds only what the build produced.

## Outcome

Landed. 111 lines added across four files, which is the S the card estimated.

**The mechanism, as found rather than as the card left it.** Hot reload is switched on by
`Config::render.shaderHotReload`, a `Tristate` carrying `--hot-reload auto|on|off` with
`auto` meaning Debug; `Engine.cpp` reads it once into `Renderer::shaderHotReload` and the
frame calls `pollShaderReload()` when it is set. It was the settings row
`render.shaderHotReload` until D14, an hour before this row started, decided a recompile
loop is a developer control rather than a preference — so there is no JSON key to reach it
by any more, and `removedKeys()` already names the retired one. None of that changed here;
this row touched only what a reload *does*.

**Which half of the sibling pair this is.** D9 and D10 split the engine's two places where a
running process writes a file a later run reads back as an input. **D9 is the scene baker**
— `writeSceneCache`, `--bake-scene`, and getting the writer out of the game binary. **D10 is
this one**: the hot-reload compiler's SPIR-V, which is a *development-build* defect only,
because `SUBSTRATE_PORTABLE` already forces `SUBSTRATE_SHADER_HOT_RELOAD` off and the whole
path compiles out behind its `#if defined`. Neither half touches the other's file, and the
invariant the pair exists to establish — *the running process never writes a file a later
run reads as an input* — is D9's to write into `principles.md`, because it is not true of
the engine until the baker moves too. Deliberately not written here.

**What landed.**

- `gfx::overrideShaderBinary(name, path)` in `Pipeline.{h,cpp}` reads a `.spv` and makes its
  bytes this process's answer for `name`. A TU-local `g_shaderOverrides` map holds them;
  `readShaderBinary` consults it before either directory and is otherwise untouched, so
  G5b's two-directory `executableDir()` resolution is exactly as it was.
- `recompileShaders` gives `glslangValidator` one temp path in the OS temp directory —
  reused by every shader in the pass, unlinked as soon as its bytes have been read — and
  publishes through that call. `ShaderTree` lost its `out` field, which is the structural
  half of the claim: `SUBSTRATE_SHADER_DIR` and `SUBSTRATE_GAME_SHADER_DIR` are no longer
  named anywhere in `Renderer.cpp`, so the reload path has no way to write into the build
  tree even by accident.
- The temp name carries the pid. Two engines reloading in the same instant would otherwise
  read each other's module, silently and with a picture neither source explains. Nothing
  ever reads that path back — it is on no search path — so even a file a crash leaves behind
  is unreachable rather than stale, which is the difference between this and the overlay
  directory the card rejected.

**Two deviations from the shape the card sketched, both deliberate.**

1. The card wanted a `loadShader` overload taking a `std::vector<uint32_t>`. That would have
   meant threading SPIR-V through `GraphicsPipelineDesc`, which names its shaders by string,
   at twenty-odd call sites — and it would have missed `verifyShaderBindings`, which reflects
   `readShaderBinary(name)` directly and would have gone on checking the *build's* module
   against layouts built from the reloaded one. Publishing by name behind `readShaderBinary`
   reaches every consumer of a module without any of them knowing, which is what the row
   actually needed.
2. The whole-file `.spv` read moved into a file-local `readSpirv` in `Pipeline.cpp`, shared
   by `readShaderBinary` and `overrideShaderBinary`. Two callers is normally a coincidence to
   be left alone, but they are two callers in one file of six lines carrying a `tellg`-sized
   allocation and a `reinterpret_cast` — and the narrowest scope reaching both is a local
   function, so the extraction costs nothing.

**What the inspection showed.** A debug run of the demo on Sponza, `--frames 6000 --locked
--hot-reload on`, with `engine/shaders/tonemap.frag` edited to `vec4(mapped.r, 0.0, 0.0,
1.0)` sixteen seconds in. The engine logged `Shaders reloaded in 3356 ms` and the capture
taken afterwards is the same frame in red — so the reload took effect, through 49 shaders
and a full pipeline rebuild. The snapshot bracketing it was taken *after* the engine was
already rendering, which matters: `run.sh` rebuilds, and the first attempt at this check
caught `file(REMOVE_RECURSE ${GAME_SHADER_OUT_DIR})` re-emitting `hologram.frag.spv` at
configure time and looked like a false positive. Bracketing only the run:

- 51 files under `build/debug/shaders/` — **not one byte, not one mtime, not one name
  changed** across a run that recompiled every shader in both trees.
- `find build/debug -newer` over the run: nothing.
- No `substrate-reload-*` left in `/tmp`.
- Relaunching after restoring the source produced a capture **byte-identical** to the one
  taken before the whole exercise, and `git diff` on the shader was empty.

**Verification.** `scripts/golden.sh check release` 11 of 11. `./test.sh debug` 858 of 858;
`./test.sh asan` 858 of 858. A validation run with `--validation on` *across a reload* — so
every `VkShaderModule` in its second half was created from the in-memory buffer, which is
the one new way this row could produce an invalid one — logged zero VUIDs, zero errors and
zero criticals. As the card predicted, the unit suite proves nothing about this path in any
configuration: `Renderer.cpp` is not in `SUBSTRATE_HOSTED_SOURCES`, and the demonstration
above is the honest substitute.

**Two things noticed and left alone.** `build/debug/shaders/` carries two orphaned `.spv`
(`rt_ambient.comp.spv`, `rt_ambient_blur.comp.spv`) from sources deleted long ago: the
engine's output directory has no equivalent of the game tree's configure-time wipe. That is
the CMake-side staleness the card's *"What this does not claim"* section warns is
indistinguishable from this row's symptom, and it is not this row's. And the reload now
costs ~3.3 s rather than the ~1.4 s `rendering.md` recorded — 49 `glslangValidator`
processes rather than 28, entirely the shader count. Both are written down in the reference
rather than left as folklore.
