---
id: bug-the-pose-drift-check-has-been-red-since-it-was-written
title: The pose-drift check has been red since it was written
arc: bug
size: S-M
verification: scripted-input, tests-hosted, tests-4
---

# bug-the-pose-drift-check-has-been-red-since-it-was-written — The pose-drift check has been red since it was written

`scripts/locomotion.sh` asserts `drift <= 0.02` on all eight arms and six of them report
more. Afterwards either the hold covers what the assertion claims it does, or the
assertion says what the hold actually promises — and the suite is green either way.

Found by [G15](../done/G15-the-game-hand-wires-the-solver-to-the-state-machine.md), which
ran the suite because its own verification named it, and stopped to find out whether it had
caused the failure. It had not:

| Arm | `drift` |
|---|---|
| `still`, `modifier` | 0.02 |
| `camera-north`, `camera-south`, `camera-turning` | 0.05 |
| `walk-run-jump`, `jump-buffered`, `jump-eaten` | **0.50** |

[bug-the-clips-moved-the-character-a-second-time](../done/bug-the-clips-moved-the-character-a-second-time.md)
recorded **`drift 0.00` on every arm** when it landed, with the counterfactual at 3.17 m.
0.50 is nowhere near 3.17, so **the hold is working** — what has changed is not that the
clips are dragging the rig again.

## What the number is now measuring

`setRootNode` holds X and Z at the bind translation and **deliberately keeps Y**, and the
reason is on the call site: a rig binds in a T-pose and animates with bent knees, so holding
Y stood the showcase character eight centimetres off the floor. `LocomotionTrace::poseDrift`
is `max |here - poseRoot|` over the **whole** vector, so every centimetre of authored
vertical hip movement lands in it. 0.50 m is what `jumping up`, `falling` and `hard landing`
do to a pair of hips; 0.05 is a walk cycle's bob.

So the assertion and the hold disagree about what is being held, and the disagreement is
about one axis. Two honest fixes, and the card should measure before picking:

1. **Measure the axes the hold claims.** `poseDrift` becomes horizontal — the same `flat`
   projection every other number in that function already takes. It then reads 0.00 again
   and the 3.17 m counterfactual still fails it, which is the whole point of the number.
2. **Assert per state.** Keep the 3D drift and give the jump states their own bound. More
   faithful and more to maintain; worth it only if a vertical drift is a defect anybody can
   name, and the call site argues it is the opposite.

The first is almost certainly right, and the reason to say so on the card rather than in the
commit is that it is the *second* time this measurement has been subtly wrong about its own
subject — see the tautology note on the card that introduced it.

## When it broke, and why nobody saw

Not known, and the card should not guess: what is known is that the session that found it
built the tree at `36522a9` and got the same 0.50, so it predates that. `locomotion.sh` is
not part of the `closing-a-card` gate — that is `golden-11`, `tests-4`, `validation` and the
trace — so a card whose `verification:` line does not name `scripted-input` never runs it,
and none of the last several did.

**That is the more interesting half of this card.** A suite that is not in the gate is a
suite that goes red silently, and this one went red without a single card noticing. Whether
`scripted-input` joins the standing gate, or whether the skill's checklist grows a line
saying which suites exist outside it, is a decision this card should make rather than leave.

## Verification

- `scripts/locomotion.sh debug` and `release` — **8 of 8 arms, drift asserted and passing**,
  with every other figure on every arm unchanged to the digit. The other figures matter as
  much as the drift: they are what says the fix was to the measurement and not to the rig.
- The counterfactual the original card used, repeated: with the `setRootNode` call removed
  and nothing else changed, the arm must still fail. A drift measure that cannot fail is the
  defect this is fixing, one axis over.
- `./test.sh` in all four configurations.

## Outcome

Fix 1, and it is two lines: `poseDrift` projects the displacement onto the horizontal plane
before taking its length — the same `flat` projection every other number in
`driveLocomotion` already takes, and for the same reason.

| | before | after |
|---|---|---|
| `still`, `modifier` | 0.02 | **0.00** |
| `camera-north`, `camera-south`, `camera-turning` | 0.05 | **0.00** |
| `walk-run-jump`, `jump-buffered`, `jump-eaten` | 0.50 | **0.00** |
| **the counterfactual** (`setRootNode` removed) | 3.17 | **3.17** |

`scripts/locomotion.sh debug` and `release`: **8 of 8 arms**, with every other figure on
every arm identical to the digit — `8.21 m travelled, net 8.21, along 1.00, across 0.00,
facing 1.00`. That is the same table the row that introduced the measurement recorded, and
getting the *same* numbers back is what says the fix was to the measurement rather than to
the rig.

**The counterfactual is the assertion that mattered.** A drift measure that cannot fail is
the defect this is fixing one axis over, so the number had to keep failing without the hold —
and it does, at exactly the 3.17 m the original card recorded. The horizontal projection
keeps it because an unheld clip walks the rig *along the floor*, which is precisely what the
projection preserves and the vertical bob is not.

**Fix 2 was not taken and should not be.** Asserting per state would keep the 3D number and
give the jump states their own bound — more faithful only if a vertical drift is a defect
somebody can name, and the call site argues the opposite at length: holding Y stood the
character eight centimetres off the floor and flattened the bob out of every walk cycle. A
bound that has to be widened for the states that exercise the feature is not a bound.

**The second half of the card is the part worth keeping**, and it is a change to the gate
rather than to the code. `scripts/locomotion.sh` and `scripts/readback.sh` run only when a
card's `verification:` line names them, which is the right default and has this failure mode:
a suite outside the gate goes red in silence, and this one did, for an unknown number of
cards. `closing-a-card` now says to run `scripted-input` whenever a card touches
`game/demo/`, the character controller, the animator or the scene tree — named or not — and
`readback` for the presentation and sprite paths, and to record which commit an
already-red suite was established from so the next reader does not repeat the bisect.

Two smaller corrections travel with it, both found by reading the table: the `scripted-input`
row said **three arms** and there are eight, and the machine's flakiness has a line now —
`VK_ERROR_DEVICE_LOST` out of `vkCreateDevice` or `vkWaitForFences` appeared twice during this
session, once in a golden case whose captured image was byte-identical to its baseline
anyway, so a re-run comes before a diagnosis.

943 tests in each of debug, release, asan and tsan.
