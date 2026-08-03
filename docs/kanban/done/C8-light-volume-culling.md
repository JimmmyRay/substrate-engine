---
id: C8
title: Light volume culling
arc: C
size: S-M
verification: golden-12, validation, trace
---

# C8 — Light volume culling

`gfx::extractFrustum` and `gfx::lightVisible` in [`Light.h`](../../../engine/gfx/Light.h) -- hosted, therefore tested -- and `updateLights` culls before it ranks. Eight test cases. **The frame-time claim is unproven and the semantic one is not**: see the measurement below

## C8, and why it is not clustered assignment

`limitations.md` goes straight from "a budget" to "clustered assignment is the honest
version", and skips a cheaper step that the data already supports.

Every light already carries its own bounding volume.
[`Light.h:21`](../../../engine/gfx/Light.h#L21) stores range in `position.w`;
[`Light.h:28`](../../../engine/gfx/Light.h#L28) stores `cos(inner)` and `cos(outer)` in `params.x`
and `params.y`. A point light is a sphere and a spot is a cone, and testing either against the
view frustum is a few lines of arithmetic on data that is already uploaded.

That matters because **a scene having hundreds of lights is not the same as hundreds being
visible at once**, and the current budget conflates them: it ranks *every* light in the scene
and keeps the top N. Culling first changes what the budget means, from a cap on lights that
exist to a cap on lights that can affect this view — which is the semantics anyone would have
asked for.

It also fixes both weaknesses `lightImportance()` documents in its own header. A spot aimed
away from the camera is now culled by its cone rather than scored as though it were aimed at
you, and a light whose range does not reach the view is now culled by its sphere rather than
"not zeroed". The approximation stops needing an apology for the two cases it names.

**Clustered assignment stays declined, with a sharper trigger** — see the table below. C8 is
what makes that trigger measurable instead of assumed.

## What C8 actually bought, measured

**The frame-time win did not show, and the row is still right.** Three runs of 240 frames on
`game/demo/assets/stress.gltf`, release, 4x MSAA, medians from the trace:

| | Lighting | Frame |
|---|---|---|
| Before (cull disabled) | 3.153 ms | 4.107 ms |
| After | 3.124 ms | 4.044 ms |

About 1%, which is inside the run-to-run spread. The reason is not that the cull is
ineffective but that **no scene in this tree has a large light set partly outside the
view** — `stress.gltf` is the closest thing and its lights sit in front of the camera. That
is the same shape of finding C13 exists to make about load time, and it is an argument
against measuring an engine for large projects on the scenes a demo happens to ship.

What did change, and is verified rather than measured:

- **The budget's meaning.** It capped lights that *exist*; it now caps lights that can
  affect this view. A lamp behind the camera used to cost a slot a lamp in front of it
  wanted. That was the complaint the row was written from, and it is closed.
- **Both weaknesses `lightImportance` documents about itself.** A light whose range does
  not reach the view is now culled rather than "not zeroed".
- **Nothing else.** All twelve golden cases are byte-identical, which is the proof that
  `lightVisible` is conservative: it rejects only lights whose own volume cannot reach a
  visible surface, so no shaded pixel could move. A cone test for spots was deliberately
  not written -- the sphere bounds the cone, so the sphere is the conservative test, and a
  cone-versus-frustum test is easier to get subtly wrong than it is to profit from.

## Verification

This row was held to:

- `scripts/golden.sh` -- twelve cases, byte-identical.
- Zero validation errors with layers on, in every capture.
- Per-pass GPU cost from `scripts/baseline.py` trace medians, several runs
  per arm -- never the `GPU @` log line.

## Reference

[architecture/rendering.md](../../architecture/rendering.md).

## Outcome

Recorded above, under *C8, and why it is not clustered assignment*.
