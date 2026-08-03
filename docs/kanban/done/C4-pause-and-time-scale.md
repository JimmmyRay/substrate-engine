---
id: C4
title: Pause and time scale
arc: C
size: S
verification: golden-12, tests-hosted
---

# C4 — Pause and time scale

`FixedClock::setTimeScale`/`timeScale`/`paused`, forwarded by `Engine`. The scale multiplies what the accumulator receives, so everything downstream of the step inherits the pause without knowing there is one. Five test cases, including the one the golden set rests on: at the default scale the locked clock is bit-identical to the clock before C4, because `dt * 1.0f` is exact

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- `./test.sh debug`, then `./test.sh asan`, each its own invocation.

## Reference

[architecture/systems.md](../../architecture/systems.md).

## Outcome

**Not recorded.** This row landed before the board existed, and no outcome was
written down at the time — what it cost and what it found out are recoverable only
from git history. The summary above is everything that was kept.
