---
id: D6
title: Bounds, and defaults that agree
arc: D
size: S
verification: golden-12, tests-4
---

# D6 — Bounds, and defaults that agree

Nine `AudioEngine` accessors index `impl->voices[i]` unchecked while four in the same class bounds-check — and `kNoSource`, which `addSource` returns on failure, is a value a caller can hold. Three of eight `PhysicsWorld` accessors likewise. Separately, `ProfilerConfig` and `Config::Profiler` disagree in three of six fields (`maxFrames` 120/240, `autoFlushFrames` 300/120, `clearAfterFlush` true/false) where the Audio and Physics pairs agree in every field, so which value applies depends on whether you arrived through `Engine`. Every accessor in both classes is bounds-checked now, and `ProfilerConfig` matches the table in every field but `outputFile` -- a library default of "write nothing to disk" for a caller that did not ask for a trace, documented on the field. Three test cases pin it

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- The unit suite in four configurations, each its own invocation:
  `./test.sh debug`, `release`, `asan`, `tsan`.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
