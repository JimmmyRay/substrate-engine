---
id: D1
title: One vocabulary for the engine's surface
arc: D
size: M
verification: golden-12
---

# D1 — One vocabulary for the engine's surface

`[[nodiscard]]` applied per project rather than per directory -- `core/Input.h` went from 0 to 30, `GpuProfiler.h` from 1 to 7, 38 in all. `Profiler::initialize` became `init`, so bring-up is `init` in all ten subsystems and teardown is `shutdown` in all of them. Predicates lost their prefixes: `isEnabled`/`inTextMode`/`inPointerMode`/`isRecording` are `enabled`/`textMode`/`pointerModeActive`/`recording`. Verified byte-identical across all twelve golden cases, which is the whole check a row that changes no behaviour can have

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.

## Reference

[architecture/principles.md](../../architecture/principles.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
