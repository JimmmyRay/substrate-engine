# The board

Every piece of planned work in Substrate, and where it is. A card is the **whole** record of
its row — what it is, why it is worth doing, what was wrong about it the first time, what
would prove it done, and what it cost. [architecture/](../architecture/) remains the
reference for what the engine *is*; the board is what is being done to it.

Two companion documents hold what belongs to no single card:
[arcs.md](arcs.md), for why the C, D, G and P arcs exist and where they must not overlap, and
[order.md](order.md), for what to do next — which directories cannot express.

## Why this exists, and what it replaced

The work used to live in three roadmap documents. They were good at argument and bad at
state: their vocabulary had two values, a row was open or it had landed, and the evidence
that this was not enough is in the documents themselves.

- **C11** needed a sentence in its Notes cell to say *"occlusion half landed; LOD half
  deferred by decision"*, because no marker could say it. The board did not need that
  sentence either: a card whose halves are in different states is two cards, and C11 is now
  the occlusion half in `done/` with C17 carrying the LOD half. **A column cannot say "half"
  and should not have to** — which is a sharper answer than the Notes cell was.
- **G2** needed a paragraph in its Status column to say which four of its five parts were
  done.
- Status was spelled three different ways in three files — `~~struck~~ **Landed**` in the C
  and D tables, a `Status` column in the G table, and nothing at all in the P table, because
  nothing there has landed yet. Answering "what is done" meant reading three grammars.

State moved here first, and the argument followed. The roadmaps are gone: each row's
reasoning is on its card, the rules the arcs held themselves to are in
[architecture/principles.md](../architecture/principles.md), the refusals and their triggers
in [limitations.md](../architecture/limitations.md), the verification protocol in
[tooling.md](../architecture/tooling.md), and the call-site sketches in
[making-a-game.md](../guides/making-a-game.md). That is the same retirement the S arc got —
*the answers move into the reference and the plan is deleted rather than left to rot* — done
one document at a time rather than all at the end.

## Columns

A card is a file. The directory holding it is its status — there is no `status:` field to
disagree with the directory, and moving a card is `git mv`, so `git log --follow` on a card
*is* its flow history. Nothing is rendered and there is no board file, so there is nothing
to fall out of date.

| Column | A card is here when |
|---|---|
| `backlog/` | It is recorded and nothing more. No promise it will be built, no scope, no verification. |
| `ready/` | It is scoped and **its verification is named**. It could be started today with no further decisions. |
| `in-progress/` | It is being built. **At most two, and the limit is not aspirational** — see below. |
| `verifying/` | It is built and the protocol is running. This is a real column here because verification is heavyweight and serial: four build configurations, eleven golden cases, a trace, and each its own invocation. |
| `blocked/` | It cannot proceed and `blocked-by` names why. |
| `done/` | It passed its verification **and the reference was updated**. Not one without the other. |

**The WIP limit is two, and it is a physical limit rather than a discipline.** Builds cannot
be chained — each configuration is its own invocation — so two rows in flight already means
serialised builds. A third does not go faster; it goes wrong, because the golden set cannot
tell you which of three changes moved a pixel.

Cards accumulate in `done/`. They are not deleted, because the board is authoritative for
status and a deleted card would take its row's status with it.

## What a card is

`<ID>-<slug>.md` — `C11-occlusion-culling.md`. An arc row keeps its historic id, so
`C11` is `C11` forever and the numbering is never resequenced. Splitting a row is the one
thing that adds an id rather than reusing one: C11's LOD half became C17 at the end of the
arc's numbering, and C11 kept its own. Work belonging to no arc takes a kind prefix instead:
`bug-`, `chore-`, `measure-`, `doc-`.

The slug follows the title, and the checker enforces it — renaming a card means renaming the
file in the same move. A kinded id already *is* that slug, so its file is the id and nothing
more: `bug-empty-scene-bring-up.md`, not the slug twice over.

```yaml
---
id: C11
title: LOD and occlusion culling
arc: C
size: L
verification: golden, validation, trace
blocked-by: content -- no asset in the tree authors an LOD chain
---
```

**An empty column does not exist.** Git does not track empty directories, and a placeholder
file per column would be six files whose only job is to stop a check complaining — so the
check gives way instead: `scripts/kanban.py` reads a missing column as an empty one. The cost
is a `mkdir -p` before a `git mv` into a column that is currently empty.

`size` is the arcs' key, unchanged: **S** < 300 lines, **M** 300-800, **L** 800-2000,
**XL** > 2000, with a revised estimate written `M-L` so the original stays visible.
`verification` is a comma-separated list drawn from the vocabulary in
[closing a card](../../.claude/skills/closing-a-card/SKILL.md); `blocked-by` is omitted unless
the card is in `blocked/`. **There is no `status:` and no `roadmap:`** — the checker rejects
both, and any key it does not know.

### What belongs on a card

> A card is the whole record of its row. If a fact about this work is not on its card or in
> [architecture/](../architecture/), it is not written down anywhere.

That is a change of rule, not an accident. While the roadmaps existed a card was a pointer and
restating the argument was a defect; now the card *is* the argument, and cards run from a
dozen lines to a hundred and fifty depending on how much their row found out. What still does
not belong is anything true of the engine rather than of the work — that is
[architecture/](../architecture/)'s job, and duplicating it here is how the two start
disagreeing.

A `done/` card ends in an **Outcome**: what landed, what the estimate did not predict, and any
defect the verification caught. That paragraph is the most valuable thing on the board,
because it is the only place a wrong assumption gets written down as wrong.

## Working it

```bash
ls docs/kanban/*/                       # the board
ls docs/kanban/*/C11-*                  # where is C11
git mv docs/kanban/ready/C16-*.md docs/kanban/in-progress/    # pull it
scripts/kanban.py                       # does the board hold together
```

**`git mv`, never `mv`.** A plain `mv` is a delete and an add, and `git log --follow` cannot
cross it — which throws away the only record of the flow that this layout exists to keep.

`scripts/kanban.py` is a checker rather than a generator. It refuses a duplicate or
malformed ID, a `blocked-by` naming a card that does not exist, a card in `blocked/` with no
blocker, an over-limit `in-progress/`, an unknown frontmatter key, and a card whose body is
missing its heading or its `## Verification` section. It fails loudly and names the file,
which is the standard D7 held every other script to.

Three skills carry the operations: [`kanban`](../../.claude/skills/kanban/SKILL.md) for
everyday movement, [`opening-a-card`](../../.claude/skills/opening-a-card/SKILL.md) for
turning a row or a bug into one, and
[`closing-a-card`](../../.claude/skills/closing-a-card/SKILL.md) for the verification gate.
Two read-only agents keep it honest: `kanban-auditor` asks whether a `done/` card's claims
are true of the tree and whether a `blocked/` card's blocker is still real, and
`kanban-groomer` asks whether the backlog is still worth what it says.

## What is deliberately not here

- **Order and priority.** Directories cannot express them, so they live in
  [order.md](order.md), which carries the sequence *and argues for it*. Encoding a sequence
  into filenames as well would put the two in conflict the first time either changed.
- **Estimates, burndown, cycle time.** Every card is dated by git, and a number nobody has
  asked a question of is a number that will be wrong without anyone noticing.
- **A card per commit.** A card is a unit of work with a verification contract. Anything
  smaller is a commit message.
