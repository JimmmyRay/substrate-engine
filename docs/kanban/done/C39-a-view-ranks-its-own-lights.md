---
id: C39
title: A view ranks its own lights
arc: C
size: M
verification: golden, trace, tests-hosted, inspection
---

# C39 — A view ranks its own lights

A secondary view is lit by the **primary's** light ranking. `updateLights` ranks against one
camera and copies the result into every view's block; a view looking somewhere else is lit by the
lights the primary could see.

C35 and C38 each took half of what this needs and neither could take this half. C35 made the
**tile assignment** per view, so a view already culls per pixel against its own depth — it is
culling the wrong *list*. C38 gave every view its own target set and its own extent, so there is
somewhere to put a second ranking. What is left is the ranking itself.

**The blocker is the shadow atlas, and it is a real one.** `recordPunctualShadows` renders one
assignment for the whole frame, into one 24-layer atlas. A second ranking would put matrices in a
view's buffer describing layers the atlas does not hold, and the view would sample **the wrong
light's depth** — which is worse than the wrong ranking, because it is wrong silently and looks
like a shadow bug. `Renderer.cpp` says so at the top of `updateLights`, and that comment is the
argument this card has to answer.

So the work is the atlas, not the ranking:

- **A per-view atlas** is the obvious answer and the expensive one: up to 24 layer re-renders per
  view, which on C38's numbers is the cost of a second frame again. Measure it before assuming
  four views can afford it.
- **A shared atlas with per-view layer ranges** — each view owns a slice, so a light shadowed in
  two views occupies two layers. Cheaper than a full atlas each, and caps how many views can have
  shadowed lights at all.
- **Ranking per view but shadowing only what the primary shadows** — a view gets the right lights
  and the wrong shadows for the ones the primary dropped. Cheapest, and the only one that ships
  without touching the atlas, but it trades a silent wrongness for a quieter one and should be
  argued rather than defaulted to.

**Pick one and say why the other two lose**, because the second and third are the ones somebody
will otherwise re-derive.

**Provenance.** Named as the remaining half by both
[C35](../done/C35-clustered-light-assignment.md) and
[C38](../done/C38-more-than-two-views-each-at-its-own-size.md), whose outcome sections carry the
per-view tile and per-view target measurements this would build on.

## Verification

- `golden` — thirteen cases byte-identical. A one-view frame ranks against one camera either way,
  so this must move no pixel in the shipping path; the two mirror cases are what exercise a
  second view.
- `trace` — `Lighting` and `Frame` for one, two and four views, against C38's table (1 full 3.317,
  2 full 6.838, 4 full 13.985, 4 quarter 6.945). A per-view atlas that doubles the frame again is
  a result worth having, not a failure — but it has to be measured, not estimated.
- `tests-hosted` — `ViewTable` is hosted; whatever holds a view's ranking should be testable
  without a device.
- `inspection` — **a view whose camera looks away from the primary's, in a scene with lights the
  primary cannot see.** That is the check this card exists for and it cannot be inferred from a
  timing change; C38 left it unanswered for exactly this reason.

## Reference update

[rendering.md](../../architecture/rendering.md), "More than one view" — the bullet naming the
light ranking and the shadow atlas as the primary's is what this retires.

## Outcome

**Closed without code: the card's premise was already false when it was written, and that is my
error rather than a discovery.** I wrote C39 from C38's *card text* — which lists "a secondary
view copies the primary's light ranking" as an open row — instead of from what C38 had actually
delivered. C38 made the ranking per view. `updateLights` opens with **"Every view runs the whole
of this (C38); only the shadow *assignment* below is the primary's"**, and `updateUniforms` calls
it per view with that view's position and unjittered matrix. A view looking somewhere else already
shades the lights it can see.

**What remains is not a defect and does not want fixing.** A secondary view looks its lights up in
the primary's atlas assignment, keyed by **source index**: a light the primary also ranked keeps
its correct layer, and a light only this view ranked gets `params.w = -1` and casts no shadow. It
illuminates without occluding. Crucially it does *not* sample the wrong light's depth — the
failure this card was opened to prevent — because the lookup is by source rather than by rank, so
a layer is only ever read by the light it was assigned to.

That degradation is already accepted a few lines above it in the same function, for a light that
does not fit the atlas at all: *"Lights that do not fit keep params.w at -1 and simply do not
occlude: the alternative, dropping the light, changes the image far more than losing its shadow
does."* A light only a secondary view can see is the same case reached by a different route.

**So the three shapes this card asked me to choose between resolve to the third, which is what
already ships** — rank per view, shadow what the primary shadows. The other two are priced rather
than dismissed: a per-view atlas is up to 24 layer re-renders per view, which against C38's
numbers (one view 3.317 ms, four full-extent 13.985) is another frame again; a shared atlas with
per-view layer ranges is cheaper and caps how many views can have shadowed lights at all. Both are
recorded with the trigger, so neither has to be re-derived.

Established **by inspection**, which is what the verification line allows and what the evidence
actually is: the `primary` branch in `updateLights`, the `!primary` lookup that follows it, and
`updateUniforms`' per-view call. C38's probe — the only thing that could put a camera in a
secondary view — was deleted with that card, so rebuilding it to re-confirm a code path this
unambiguous would have been the expensive way to learn nothing. **The one arm genuinely still
unrun is a picture**: a view aimed at lights the primary cannot see, compared against the same
view rendered alone. That belongs to whoever next needs a second view to look right, not to a card
whose premise has dissolved.

No code changed, so `golden`, `trace` and `tests-hosted` have nothing to say about this card and
were not run for it; the tree is exactly what closed C38 and the naming chore, both of which ran
them.

Reference updated: `limitations.md` gains "A light only a secondary view can see illuminates but
does not occlude", with the per-view-atlas trigger and the two cheaper shapes; `rendering.md`'s
"More than one view" bullet now says the ranking is per view and only the assignment is shared,
which is what the tree does.
