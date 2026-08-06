---
id: X0
title: What the work is, as a noun phrase
arc: C
size: M
verification: golden, tests-4, validation
---

# X0 — What the work is

> Every path below is written as a card sees them — from inside a column directory, one level
> deeper than this file. They resolve once the card is copied into `backlog/`, and not before.

Two sentences on what gets built, in terms of what the tree will hold afterwards, then the
argument for why it is worth doing. This card is the only record of that argument — there is
no roadmap behind it — so what you leave out is not written down anywhere.

What does **not** belong here is anything true of the engine rather than of this work.
[architecture/](../../architecture/) owns that, and duplicating it is how the two start
disagreeing.

## Verification

What must be true before this may enter `done/`, spelled as commands rather than intentions:

- `scripts/test.sh debug`, then `scripts/test.sh asan`, each its own invocation.
- `scripts/golden.sh` — eleven cases, byte-identical.
- Zero validation errors with layers on.

## Reference update

Which of [architecture/](../../architecture/) this changes, from the table in
[closing a card](../../../.claude/skills/closing-a-card/SKILL.md). A card whose verification
passes but whose reference is unwritten
is not done; it is a row that will be re-derived from source in six months.

## Outcome

Filled in on close, in one paragraph: what landed, what the estimate did not predict, and any
defect the verification caught. Left empty until then.
