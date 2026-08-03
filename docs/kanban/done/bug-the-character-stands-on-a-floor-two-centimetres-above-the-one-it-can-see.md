---
id: bug-the-character-stands-on-a-floor-two-centimetres-above-the-one-it-can-see
title: The character stands on a floor two centimetres above the one it can see
arc: bug
size: S
verification: golden-11, inspection
---

# bug-the-character-stands-on-a-floor-two-centimetres-above-the-one-it-can-see — The character stands on a floor two centimetres above the one it can see

Sponza declares no collider anywhere, so `make_composite_scene.py` gives the showcase a
`showcase_ground` box for a character to stand on and put its top face at y = 0, with a
comment saying that is where Sponza's floor is. It is not. Sponza's ground floor is one flat
quad -- primitive 46, seven vertices, every one of them at **y = -0.02** about a root scaled
by 0.008.

Two centimetres, and four at `GameSetup::sceneScale` 2, because the box scales with the
scene while the gap between it and the floor scales with it too. Small enough to look like a
rendering artefact and large enough to see, which is why it survived being reported twice.

**The rig was never the problem, and two earlier readings of this were wrong.** Measured off
the animator rather than off a screenshot:

```
LeftToeBase  localY=+0.0025    RightToeBase localY=+0.0033    (charY = 0.0000)
```

The toes sit 3 mm above the character's own origin, so the mesh is planted on whatever the
controller is standing on. Screenshots could not settle it because the two feet are at
different depths in every idle pose, and a foot nearer the camera projects lower whether it
is lower or not -- which is what made one reading say "sunk 8.5 cm" and another "hovering",
from the same frame.

The other wrong turn is worth keeping: `boundsMin.y + 1.0` is the demo's *estimate* of the
floor for placing props, and taking it for the floor itself gave -0.023 and a story about a
2.3 cm gap that happened to be about the right size for the wrong reason. Primitive 45 is the
other trap -- its bounding box spans the whole courtyard, but it is 92 vertices in three
tiers, a trim strip rather than a surface.

## Verification

- `inspection`, and it is a number rather than a look: the toe joints against the floor
  plane, logged from `driveLocomotion` and read back.

  ```
  before   LeftToeBase worldY=+0.0025   RightToeBase worldY=+0.0033   floor -0.0400
  after    LeftToeBase worldY=-0.0403   RightToeBase worldY=-0.0400   floor -0.0400
  ```

- `scripts/golden.sh` -- eleven cases. None of them loads `showcase.gltf`, so none moves.

## Reference update

None. The fact lives in the comment beside the node that gets it wrong, which is where the
next person to write `y = 0` there will meet it.

## Outcome

Fixed in `scripts/make_composite_scene.py`, not in the asset: `game/demo/assets/` is
gitignored and everything in it is regenerated, so a hand-edited `showcase.gltf` would have
lasted until the next `fetch_assets.sh`.

**Moving the floor without moving what stands on it caused a second defect, and it is worth
recording because the mechanism is not obvious.** The character was authored at y = 0 as
well, so lowering only the box left it spawning two centimetres above its own floor -- four
at `sceneScale` 2. It fell, and the demo's animation state machine is driven by what the
solver reports rather than by what anybody pressed, so it correctly played a fall and then
two seconds of `hard landing` on startup with nobody touching a key. That reads exactly like
a character walking on its own:

```
before   Locomotion: idle -> fall at step 2, fall -> land at step 6, land -> idle at step 127
         3 changes over 200 steps, drift 0.50
after    0 changes over 200 steps, drift 0.02
```

The drift number is the second half of the evidence: `hard landing` travels its hips 0.4 m,
and half a metre of mesh-to-capsule drift is what that cost while it played.

Both heights now come from one `SPONZA_FLOOR_Y` constant in the generator, because they are
the same fact and a second literal is how they came apart the first time.

Related: [[bug-a-clip-held-at-its-end-reads-as-its-beginning]] came in with this one as a
single report about the character, and is a different defect in a different file.
