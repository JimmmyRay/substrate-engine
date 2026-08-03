---
id: bug-asan-cannot-run-the-renderer-and-only-tsan-says-so
title: ASan cannot run the renderer and only TSan says so
arc: bug
size: S
verification: inspection
---

# bug-asan-cannot-run-the-renderer-and-only-tsan-says-so — ASan cannot run the renderer and only TSan says so

`CLAUDE.md` tells a session that `./run.sh tsan` cannot run the renderer and stops there. The
reader's reasonable inference is that ASan can, and it cannot — not as launched. Under ASan
the same call fails a different way:

```
[Vulkan] [ERROR]    terminator_CreateDevice: Failed in ICD libGLX_nvidia.so.0 vkCreateDevice call
[Vulkan] [CRITICAL] vkCreateDevice failed: VK_ERROR_INITIALIZATION_FAILED
```

The fix is one flag, and the tree already knows it.
[tooling.md](../../architecture/tooling.md), "What each configuration can and cannot do", has
the whole answer in its table: `asan` renders **with `--no-ray-query`**, because requesting
the acceleration-structure extensions is what the driver refuses, and `Config.cpp:644` prints
`do not request the RT extensions (needed under ASan)` in `--help`. Three places say it and
the one file a session is *told* to read says none of it.

So this is not a missing capability. It is a paragraph in `CLAUDE.md` that names the TSan
exception, omits the ASan one, and thereby teaches the reverse of what is true.

What it has cost, twice:

- **G4** concluded "the thousand create/destroy cycles could not be run under ASan on this
  machine" and moved the arm to debug. Correct outcome, wrong reason on the card.
- **C22** wrote "a thousand import/remove cycles under ASan" into its verification, and
  closing it spent a full ASan build and a 4100-frame capture discovering the same failure a
  third time. Its Outcome says ASan cannot run the renderer, which is wrong and wants
  correcting when this card lands.

The second half is the `leak` token itself. Both card skills define it as **"a thousand
create/destroy cycles under ASan, high-water mark unchanged"** —
`.claude/skills/opening-a-card/SKILL.md:79` and `closing-a-card/SKILL.md:67` — so every card
that names `leak` inherits an arm that needs a flag neither skill mentions, at a cycle count
nobody has ever justified. C22 measured the alternative: ten import/remove cycles reach a
steady state of 518.0 MiB and sixty reach 518.4 MiB, which settles one-time growth against a
per-cycle leak in about thirty seconds. **Two short runs at different counts, compared on
`logMemoryUsage`**, is the shape that answers the question; a thousand of anything only
answers it more slowly.

## Verification

`inspection`, and deliberately: this is documentation, and the check is that the next session
to write a sanitizer arm gets it right from `CLAUDE.md` alone.

- `CLAUDE.md` names both sanitizer constraints and the flag that lifts the ASan one, without
  the reader needing `tooling.md` to find out.
- The `leak` token in both skills describes the two-run comparison, names `--no-ray-query` if
  the arm draws, and drops the unjustified thousand.
- C22's Outcome no longer claims ASan cannot run the renderer.
- `docs/architecture/tooling.md` is already correct and should not be re-argued — a link, not
  a second copy.

## Reference update

None. `tooling.md` is the reference here and it has been right all along; this card is about
the two files that disagree with it.

## Outcome

Done as scoped, in four edits and no code.

`CLAUDE.md` now opens the paragraph with **"Neither sanitizer opens a device the way you would
launch it, and they fail for different reasons"** and gives both: `--no-ray-query` for ASan,
nothing for TSan, with a link to `tooling.md`'s table rather than a second copy of it. The old
text named only TSan, and its shape was the problem — one exception stated on its own reads as
a complete list.

The `leak` token is rewritten in both card skills. It no longer says "a thousand cycles under
ASan"; it says **two runs at different cycle counts with the same steady state**, cites C22's
ten-against-sixty as what that looks like, and states where ASan and TSan can and cannot be
used for it. `CLAUDE.md` carries the same two-run rule beside the sanitizer paragraph, because
a session that never opens a skill still needs it.

C22's Outcome is corrected. It said ASan cannot run the renderer; it now says the arm was
launched without the flag and records that `tooling.md` had the answer the whole time.

**What this card is really about is the shape of a written exception, not a Vulkan flag.** The
fact was in `tooling.md`'s table, in `Config.cpp:644`'s `--help` string, and in G4's Outcome —
three places, none of them the file a session is instructed to read. Two cards then wrote an
unrunnable arm, and the second one spent a full ASan build and a 4100-frame capture proving it
unrunnable a second time before `tooling.md` was consulted. The cost was never in finding the
answer; it was in not knowing there was an answer to look for.

## Verification results

`inspection`, as named. No build and no run — every edit is markdown, and the claim is about
what a reader is told.

- `CLAUDE.md:56` names both constraints and the flag, in one paragraph, with the two-run leak
  rule beside it.
- `.claude/skills/opening-a-card/SKILL.md:79` and `.claude/skills/closing-a-card/SKILL.md:67`
  both define `leak` as the two-run comparison and both name `--no-ray-query`.
- `docs/kanban/done/C22-*.md` no longer asserts ASan cannot render; the only remaining match
  for that phrase in the file is this card's id.
- `docs/architecture/tooling.md` is untouched, deliberately. It was right.
