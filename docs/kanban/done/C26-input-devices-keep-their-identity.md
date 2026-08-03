---
id: C26
title: Input devices keep their identity
arc: C
size: L
verification: tests-hosted, scripted-input, golden-11
---

# C26 — Input devices keep their identity

Afterwards a press can be attributed to the device it came from, and two gamepads can drive
two different things. Today every connected pad is folded into one state before the input map
ever sees it:

```cpp
// engine/core/InputGlfw.cpp:53-77
GamepadState merged;
for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    ...
    merged.buttons[b] = merged.buttons[b] || raw.buttons[b] == GLFW_PRESS;
    if (std::fabs(v) > std::fabs(merged.axes[a])) merged.axes[a] = v;
}
map.setGamepad(merged);
```

Any button on any pad reads as pressed, and each axis takes the largest magnitude across every
pad. The loop *enumerates* devices and then deliberately discards which one acted.
`InputMap` holds one `GamepadState` ([`Input.h:451`](../../../engine/core/Input.h#L451)),
`Binding` is `{Source, code, scale}` and `Source::PadButton` names no pad, and
`input::saveBindings` writes one profile into one config file.

Local co-op is not merely hard, it is unreachable — there is no expression for "player two".
So is any asymmetric setup: a HOTAS on one axis set beside a pad on another. There is a
second-order defect in the same lines, worth fixing whatever shape this takes: the max is
taken *before* the deadzone the map applies later
([`Input.h:414`](../../../engine/core/Input.h#L414)), so an idle second pad's stick drift
permanently biases the first pad's movement.

**`Input.h`'s own scale section claims this is already general.** Lines 36-43 say "Generalized.
There is no fixed action count, no fixed binding count per action and no capped text length",
and name the key-state array as "the one bound quantity". The merged pad is a second bound
quantity and the header does not admit it — which is part of why nobody has looked at it.

Expected to be wrong about: whether the right unit is a device or a *player*, since keyboard
halves are a real local-co-op input and a player may hold two devices. Guessing "device index
on the binding" first is probably the mistake; the player is likely what an action resolves
against, with devices assigned to players.

## Verification

- `./test.sh debug`, then `./test.sh asan`, each its own invocation. `core/Input.cpp` is in
  `SUBSTRATE_HOSTED_SOURCES`, so the resolution table is testable with no device: two
  synthetic pads, opposite deflections, two players reading different values.
- `--input-script` through `scripts/`, which today addresses a global action namespace and
  will need to say which player it is scripting.
- `scripts/golden.sh` — eleven cases, byte-identical. No golden case reads a pad.

## Reference update

[architecture/systems.md](../../architecture/systems.md) — the input section, and
[limitations.md](../../architecture/limitations.md), which mentions gamepads only as a Windows
verification row and states no scope for device plurality.

## Outcome

**The card's own guess was right and worth stating as the result: the unit is the player, not
the device index.** `PlayerDevices` is `{bool keyboard; uint32_t pads;}` — a keyboard flag and
a bitmask of pad indices — and `InputMap` resolves every action once per player into
`Action::state[player]`. `value`/`held`/`pressed`/`released` take a trailing
`uint32_t player = 0`, so every existing call site is unchanged, and the default map holds one
player owning the keyboard and `kAllPads`. Two players are `setPlayerCount(2)` plus two
`setPlayerDevices` calls and nothing else: the binding table stays shared, only the state
vector is per player.

Putting a device index on the `Binding` — the thing the card called "probably the mistake" —
would have failed on the two cases that motivated the row. A player holding a pad *and* the
keyboard halves cannot be one device, and handing a pad to a different player would have meant
rewriting bindings rather than a mask. It would also have made the binding file per device,
and `saveBindings` would have had to grow a device key it could never resolve on a machine
where the pads came up in a different order.

`pollGamepads` now pushes one `GamepadState` per joystick slot with no merging, and
`InputMap` holds `std::vector<GamepadState> pads/padsLast` sized by what arrives. The
second-order defect the card named is fixed on the way: `resolve` and `edgePressed` apply the
deadzone **per pad before combining**, where the old code took the max first. That one is
invisible from a single pad and permanent from two — a resting stick at 0.12 with a deadzone
of 0.15 used to win the max against a deliberately-centred pad and bias the player forever.

`Script::Step` gained a `uint32_t pad` and the grammar gained an optional `@<pad>` suffix, so
`--input-script` can drive two players from one run. `Script::apply` builds one `GamepadState`
per pad a script names rather than one for the whole feed. A produced binding still names no
pad, deliberately: the pad is a property of the feed, not of the binding, which keeps a script
a test *of* the binding table.

`Input.h`'s scale section, which the card correctly flagged as claiming a generality it did
not have, now admits two bound quantities rather than one — the key-state array and the pad
vector, the latter capped at `kMaxPads = 16` because the mask is 32 bits.

### Verification

- `./test.sh debug` — 1002 tests from 102 suites, all passed.
- `./test.sh asan` — 1002 tests from 102 suites, all passed.
- Seven new cases: five `InputPlayers.*` (two synthetic pads at opposite deflections read back
  as two different values by two players; a player holding no device; the keyboard held by
  both; per-pad deadzone; `setPlayerCount(0)` still leaving one player) and two
  `InputScriptTest.*` for the `@<pad>` selector.
- `timeout -s TERM 1800 scripts/locomotion.sh debug` — 9 of 9 arms pass.
- `scripts/golden.sh` — all 11 cases match, as the card predicted: no golden case reads a pad.

### Deferred

Nothing assigns pads to players — there is no join flow, and hot-plug does not renumber a
player's mask. That is recorded in [limitations.md](../../architecture/limitations.md) as a
scope statement rather than as a card, because the engine cannot tell a reconnected controller
from a different one in the same slot; only a game knows. G17 is what makes a second player
reach a character.
