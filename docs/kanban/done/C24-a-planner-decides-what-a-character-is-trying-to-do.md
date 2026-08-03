---
id: C24
title: A planner decides what a character is trying to do
arc: C
size: L
verification: tests-hosted, tests-4, golden-11, inspection
---

# C24 — A planner decides what a character is trying to do

`engine/ai/` will hold a goal-oriented planner: a world state of named boolean properties, a
set of actions carrying prerequisites, effects and a cost, and a search that returns the
cheapest ordered sequence reaching a goal. What the tree holds afterwards is that planner, an
intent a character carries, and a demo whose character can be given a goal instead of a key.

**The engine can animate a decision and cannot make one.** [C23](../done/C23-a-state-machine-per-character.md)
gave every character its own state machine and [G15](G15-the-game-hand-wires-the-solver-to-the-state-machine.md)
drives that machine from the solver, which together answer "what pose, given what the body is
doing". Neither answers "what should the body do", and every existing answer to that in this
tree is an input map — which is a fine answer for exactly one character.

A planner rather than another state machine, and the distinction is the reason for the card.
A state machine needs every route spelled out as a transition: to reach `attack` from
`unarmed` somebody has to author `unarmed -> draw -> attack` and author it again for every
state `draw` might be entered from. A planner is given `attack` as a goal and derives the
route from what each action requires and produces, so an action added later is reachable from
everything that satisfies it without a single transition being edited. That property is worth
having in proportion to how many actions exist, which is the argument for doing it in an
engine meant for a substantial game rather than for the demo.

**This does not replace `AnimationStateMachine` and must not try to.** The planner's state is
boolean and its output is a sequence; the blend tree's parameters are continuous and its
output is a pose, and `speed` at 0.42 — most of the way from walk to run — is a value a
planner has no way to express. The two meet at an intent: the planner decides `walk to the
pot`, the character controller pursues it, and the machine blends whatever gait that produces.
A planner asked instead to choose a clip per frame would be a search run sixty times a second
over a question with no prerequisites and no sequence, and it would cost the cross-fades to do
it.

The implementation starts from Tethered's `src/substrate/ai/`, which is about 600 lines of
header-only template across `GOAP.h`, `GOAPState.h`, `GOAPGoal.h`, `GOAPPlan.h` and
`GOAPSystem.h`, and depends on nothing but `<cstdint>`, `<limits>` and `glm/vec3.hpp`. That
makes it hosted, so it tests under every sanitiser — which is most of why it is worth taking
rather than writing. Two things about it should be expected to change on the way in and are
the parts of this estimate most likely to be wrong:

- **A `shared_ptr` per action and a `std::function` per action body.** Substrate does not
  allocate per frame anywhere else, and a planner that re-plans on event rather than per frame
  may well not care — but that is a measurement, not an assumption, and it is one this card
  has to take before defending the shape.
- **Registering properties through a `GOAPStateBuilder` with a getter and setter lambda per
  field**, which exists to work around the absence of reflection. Whether that survives
  contact with a codebase that prefers a flat table to a builder is an open question.

Tethered's tree is also itself named `src/substrate/`, and its namespace is `ai`. Both collide
with this one, so nothing comes across without being re-namespaced first.

## Verification

- `./test.sh debug`, then `./test.sh asan`, `./test.sh tsan` and `./test.sh release`, each its
  own invocation. The planner is pure CPU and belongs in `SUBSTRATE_HOSTED_SOURCES`, so all
  four apply and TSan is meaningful for once.
- The tests that matter are the ones about *derived* routes rather than authored ones: a goal
  reachable only through an action whose prerequisite another action produces, a goal with two
  routes where the cheaper wins, a goal with no route at all returning nothing rather than a
  partial plan, and a plan invalidated mid-execution by the world changing under it.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case has an agent in it, so a
  difference is this card having reached into the render path, which it has no business doing.
- `inspection` for the demo, and the honest version of it: a character given a goal reaches it
  without a key being pressed, and the plan it followed is in the log. Without that this is a
  library with a test suite and no evidence it drives anything.

## Reference update

A new section in [architecture/systems.md](../../architecture/systems.md) for the decision
layer: what a world state is, where a plan is re-evaluated, and — the part worth writing down
because it is the mistake the design exists to prevent — why the planner does not choose
clips.

## Outcome

`engine/ai/Planner.{h,cpp}` — 64-property `WorldState`, a flat `Action` table, an A\* over
states, and an `Agent` that carries a goal and a cursor. Seventeen hosted cases. The demo's
`G` hands the character a goal and nothing else is pressed:

```
Goal: carry the torch
Plan: carry the torch -- walk to the torch > pick up the torch
Plan: walk to the torch
Plan: pick up the torch
Plan: done, the torch is carried
```

**Nobody authored that order.** `walk to the torch` has no prerequisites at all and does not
know it is the first step of anything; it is in the plan because `pick up the torch` requires
what it produces. That is the property that separates this from a state machine, and it is
the one the demo demonstrates rather than the one the tests merely assert.

**Tethered's implementation was not taken, and both of the things the card expected to change
did.** The card described ~600 lines of header-only template with a `shared_ptr` per action, a
`std::function` per action body and a `GOAPStateBuilder` with a getter/setter lambda per
field. None of that survives contact with this codebase and none of it is here — the design
that does is smaller than the one it replaces:

- **A state is two `uint64_t`.** `known` and `value`, not one mask, because *"the door is
  shut"* and *"I have no opinion about the door"* are different claims and one bitmask cannot
  tell them apart. `satisfies` is four instructions, `after` is three, and the search never
  allocates a state.
- **An `Action` is a name, two states and a cost.** No function body: what the planner needs
  is the *contract*, and running the thing is the caller's job at the far end of a plan.
- **A flat name table indexed by bit** rather than a builder. The builder exists to work
  around the absence of reflection, and there is nothing here to reflect over.

**The measurement the card demanded before defending the shape**, taken on the demo in release
over one goal followed to completion: `advance` costs **0.00027 ms** on a frame that did not
re-plan and **0.00739 ms** on the frame that searched. The search does allocate — a node
vector, a visited map, an open list — and that is the one place this departs from "no
allocation per frame". It is defensible because it is not per frame, and 7.4 microseconds once
per event against a 2 ms frame is the number that says so rather than the argument.

**One design error, found by a test rather than by reasoning, and it is the subtle one.**
`Agent::advance` first judged whether the held plan was still valid and then walked the cursor
past finished steps. That re-plans on **every step of every successful plan**: an effect is
precisely a thing that makes its own action's prerequisites false, so after `draw` the cursor
still points at `draw` and `draw` requires `unarmed`. The order is reversed and the test that
caught it —
`FollowsThePlanOneStepAtATime` asserting `replanned()` is *false* on the second step — is the
only kind that could have. Every other assertion passed with the bug in.

**Three decisions worth having on the card**, each of which a caller can otherwise get wrong
in silence:

- A goal already met returns **true with an empty plan**, which is not the same answer as
  false with an empty plan. A caller cannot tell them apart from the size, so the return value
  carries it.
- A failed plan leaves `out` **empty** rather than partial. A caller handed the reachable half
  of a plan would execute it and leave the world somewhere nobody asked for.
- The 65th property is **refused and named** rather than folded onto an existing bit. A
  property that quietly aliases another is a plan that is wrong in a way nobody can read.

960 tests in each of debug, release, asan and tsan, up from 943 — the planner is pure CPU and
in `SUBSTRATE_HOSTED_SOURCES`, so all four apply and TSan is meaningful.
`scripts/golden.sh check release`, eleven of eleven byte-identical: no golden case has an
agent in it, so a difference would have been this card reaching into the render path.
`scripts/locomotion.sh debug`, 8 of 8 — run because the card touches `game/demo/`, which is
the rule the previous card put in the gate. It went red once on a
`VK_ERROR_DEVICE_LOST` out of `vkCreateDevice` and passed on the re-run, which is the machine
and is now also in that gate.
