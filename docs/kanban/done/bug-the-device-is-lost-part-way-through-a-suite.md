---
id: bug-the-device-is-lost-part-way-through-a-suite
title: The device is lost part way through a suite
arc: bug
size: M
verification: scripts-fail
---

# bug-the-device-is-lost-part-way-through-a-suite — The device is lost part way through a suite

`scripts/locomotion.sh` runs eight arms as eight processes, each creating and destroying a
device. Twice in one session it reached the fourth or fifth arm and then hung for
twenty-three seconds inside `vkCreateDevice` before failing:

```
[Vulkan] [STATUS]   Device: NVIDIA GeForce RTX 3060 Ti (Vulkan 1.4.312, driver 2435301568)
[Vulkan] [STATUS]     max MSAA: 8x, timestampPeriod: 1.0 ns
                                             <- 23 seconds
[Vulkan] [CRITICAL] vkCreateDevice failed: VK_ERROR_DEVICE_LOST
```

Once on `jump-eaten`, once on `jump-buffered` — **a different arm each time**, which is what
says it is not the arm. The arms before it pass with zero assertions failed, and
`scripts/golden.sh` creates eleven devices back to back in the same session without a
stumble, so it is not device creation as such either.

The related sighting is a golden `particles` case that failed with `VK_ERROR_DEVICE_LOST` and
a byte-identical capture, cleared by re-running. That one was recorded as machine flakiness
and the `closing-a-card` skill grew a "re-run once first" line. Two more sightings in one
session is the point at which "flaky machine" stops being a finding and starts being a card:
a suite that cannot finish is a suite whose last three arms have never been run against this
row's changes, and nobody can tell that from a green checkout.

## What the kernel says, which is most of the answer

`journalctl -k` carries an `NVRM: Xid` at **both** failure timestamps to the second:

```
Aug 01 01:55:07  Xid 109  channel 0x00000036, errorString CTX SWITCH TIMEOUT, Info 0xfc040
Aug 01 03:13:44  Xid  32  channel 0x0100001c intr0 00040000     <- jump-eaten
Aug 01 03:17:24  Xid  32  channel 0x0100001c intr0 00040000     <- jump-buffered
```

**Xid 32 is an invalid or corrupted push buffer stream** and Xid 109 is a context-switch
timeout — both are the GPU's own fault reporting, not Vulkan's. So `VK_ERROR_DEVICE_LOST` is
the symptom of a fault the hardware had already raised, and the twenty-three seconds is the
driver waiting on a channel that was never coming back.

Two things follow, and the second is the interesting one:

- **It is not our teardown.** `Engine::teardown` calls `vkDeviceWaitIdle` before every GPU
  destroy, `ImageTable::shutdown` frees nothing on the device (it clears two vectors), and
  `Renderer::shutdown` waits before its first destroy. Inspection finds no path that frees an
  object while work is in flight, which was the first hypothesis and is the one now closed.
- **The fault lands in `vkCreateDevice`, before we have submitted anything.** A fresh process
  cannot corrupt a push buffer it has not written to. So the channel was already bad when the
  process opened it — left that way by the arm before, which is what the earlier CTX SWITCH
  TIMEOUT describes.

What is left to establish:

- **Which arm leaves it wedged, and with what.** The failure surfaces one process late, so the
  arm that fails is not the arm to look at. `scripts/rdoc.sh` over the arm *before* each
  observed failure is where this goes next.
- **Whether the engine can provoke it deliberately.** Eight processes in eighty seconds is a
  rate nothing else here reaches — the golden set is eleven captures inside one process, and it
  has never done this.
- **Whether a suite should survive it.** `closing-a-card` already says to re-run a golden case
  once before believing a `VK_ERROR_DEVICE_LOST`. A suite of eight arms that dies on the fifth
  and reports nothing about the last three is worse off than a suite that retries the arm and
  says it did.

## Verification

- `scripts/locomotion.sh` completes **all eight arms, three runs in a row**, with no `Xid` in
  `journalctl -k` for the window the runs cover. Three because two consecutive failures is what
  opened this card, so two clean runs is the same evidence pointing the other way and one more
  is the margin.
- Whatever the row turns out to change, an arm that names the *cause* rather than the symptom.
  A retry that hides a wedged channel passes the bullet above while making the engine worse,
  so a card that ends in a retry has to say what it is retrying around and log it.
- `scripts-fail`: if the answer is the suite surviving a lost device, then killing an arm on
  purpose has to leave the run **red and specific** — not green because the retry worked.

## Outcome

**Not reproduced, and not fixed — those are different sentences and both belong here.**

Three consecutive runs of `scripts/locomotion.sh`, eight arms each, twenty-four arms in ten
minutes: every one completed, two of the three with zero assertions failed, and
`journalctl -k` records **no `Xid` of any kind** across the whole window. That is the
verification this card named, met exactly as written.

What it does not do is explain the two failures that opened it. Nothing in this session
touched device creation, the shutdown path or anything the driver would see differently, so
the honest account is that a fault the hardware raised twice in four minutes then stopped
raising. The inspection above still stands and is the useful part of this card: the teardown
waits before every GPU destroy, `ImageTable::shutdown` frees nothing on the device, and an
`Xid 32` inside `vkCreateDevice` is a corrupted push buffer in a process that has not yet
submitted anything.

**If it recurs, reopen this rather than starting again.** What to do next is written above and
does not depend on catching it live: the arm that fails is one process too late to be the arm
to look at, so `scripts/rdoc.sh` goes over the arm *before* the failure. The three timestamps
and their Xid codes are recorded here precisely so a fourth sighting has something to sit
beside.

Deliberately not done: a retry around a lost device. It would have closed this card by making
the symptom invisible, which the verification above says in as many words.

## Verification results

- `scripts/locomotion.sh` — **three runs, eight arms each, all completing**, at 03:28:29,
  03:29:46 and 03:30:23. Runs two and three: exit 0, no assertions failed.
- `journalctl -k --since 03:26` — **zero** `NVRM`/`Xid` lines. The two failures this card was
  opened for each put one in the log to the second.
- Run one failed three assertions, and they were real: `faced 0.00 of the way it walked` on
  all three camera arms. Not this card's — a defect in
  `chore-the-demo-loads-a-scene-a-script-baked`, found by the suite that card had already said
  should be on its verification line. Fixed there and the note added to it.
