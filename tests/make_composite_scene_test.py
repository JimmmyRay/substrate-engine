#!/usr/bin/env python3
"""
Tests for scripts/make_composite_scene.py.

    ./tests/make_composite_scene_test.py

Python rather than gtest because the thing under test is a Python script, and the same
arrangement as tests/manifest_test.py for the same reason: it is not part of `./test.sh`,
which builds and runs a C++ binary. Run it directly, or through the CI workflow.

Everything here is about `aim_quat`, because a light's orientation is the one thing this
generator emits that nothing downstream can sanity-check. A wrong vertex position is
visible the moment the scene loads. A sun rotated to the wrong quadrant loads clean,
validates clean, and renders a scene that merely looks overcast -- which is how a sign
error in the axis went unnoticed until someone asked why reflections had no shadows in
them. The answer was that nothing in the scene had shadows: the sun was 58 degrees below
the horizon, so every surface was self-shadowed and the whole image was sky ambient.

The test that catches it is the round trip. `aim_quat(d)` promises a rotation carrying
local -Z onto `d`, so rotating -Z by the result has to give `d` back. That is checkable
without knowing anything about quaternion conventions, and it fails loudly for an
inverted axis, a transposed rotation, or a wrong-handed cross product.
"""

import importlib.util
import math
import pathlib
import unittest

_spec = importlib.util.spec_from_file_location(
    "make_composite_scene",
    pathlib.Path(__file__).resolve().parent.parent / "scripts" / "make_composite_scene.py")
scene = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scene)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _rotate(q, v):
    """Apply an xyzw quaternion to a vector, the long way and with no dependencies."""
    u = (q[0], q[1], q[2])
    w = q[3]
    uv = sum(a * b for a, b in zip(u, v))
    uu = sum(a * a for a in u)
    cr = _cross(u, v)
    return tuple(2.0 * uv * u[i] + (w * w - uu) * v[i] + 2.0 * w * cr[i] for i in range(3))


def _normalise(v):
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


# Every direction the generator actually aims a light along, plus the axes and a pair of
# awkward ones. The named two are the regression: both are suns, and both pointed at the
# sky instead of at the ground.
#
# The demo's sun is kept here although the generator no longer writes it -- the composite
# that carried it is retired and `DemoGame::configure` states the same direction now. It is
# the vector the regression was found on, and the arithmetic it checks is `aim_quat`'s.
DEMO_SUN = (0.35, -0.85, -0.4)
STAGE_SUN = (0.62, -0.52, -0.58)

DIRECTIONS = [
    DEMO_SUN,
    STAGE_SUN,
    (0.0, 0.0, -1.0),   # already -Z: the identity case
    (0.0, 0.0, 1.0),    # antiparallel: the branch that picks an axis by hand
    (1.0, 0.0, 0.0),
    (-1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0),
    (0.0, -1.0, 0.0),
    (0.3, 0.2, -0.9),
    (-0.7, 0.1, 0.7),
    (2.0, -3.0, 6.0),   # unnormalised input
]


class AimQuatTest(unittest.TestCase):
    def test_carries_minus_z_onto_the_requested_direction(self):
        for direction in DIRECTIONS:
            with self.subTest(direction=direction):
                got = _rotate(scene.aim_quat(direction), (0.0, 0.0, -1.0))
                want = _normalise(direction)
                for axis, (g, w) in enumerate(zip(got, want)):
                    self.assertAlmostEqual(g, w, places=6, msg=f"axis {axis} of {direction}")

    def test_returns_a_unit_quaternion(self):
        for direction in DIRECTIONS:
            with self.subTest(direction=direction):
                q = scene.aim_quat(direction)
                self.assertAlmostEqual(math.sqrt(sum(c * c for c in q)), 1.0, places=6)

    def test_both_suns_end_up_above_the_horizon(self):
        """
        The regression, stated as the thing a reader would have checked by eye.

        glTF aims a light along its local -Z, so the vector handed to `aim_quat` is the
        direction light *travels* and the engine negates it to get the toward-the-light
        vector it shades with. A sun travelling downward therefore has to come back with
        a positive Y, and an inverted axis is exactly what turns that negative.
        """
        for name, travel in (("the demo's sun", DEMO_SUN), ("stage_sun", STAGE_SUN)):
            with self.subTest(sun=name):
                aimed = _rotate(scene.aim_quat(travel), (0.0, 0.0, -1.0))
                self.assertLess(aimed[1], 0.0, f"{name} should travel downward")
                self.assertGreater(-aimed[1], 0.0, f"{name} should be above the horizon")


if __name__ == "__main__":
    unittest.main()
