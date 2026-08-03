#!/usr/bin/env python3
#
#   scripts/kanban.py [--quiet]
#
# Check that docs/kanban/ holds together. This is a checker, not a generator: it writes
# nothing and renders nothing, because the board has no rendered form to fall out of date --
# a card's directory is its status, so the state and the record of the state are the same
# byte on disk.
#
# What it does check is everything that layout cannot enforce by itself: that an id is
# unique and well formed, that a filename matches the id and title inside it, that a
# `blocked-by` naming another card names one that exists, that a card in blocked/ says why,
# that in-progress/ is within its limit, and -- since the roadmaps are gone and a card is now
# the only record of its row -- that every card actually is one: a heading that matches its
# id, a non-empty `## Verification` section saying what would prove it done, and -- for a
# card in done/ -- an `## Outcome` recording what the row actually cost and found out.
#
# It exits non-zero and names the file. A check that cannot fail is not a check; that is the
# standard D7 held the other scripts to, and this one is held to it as well.

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BOARD = REPO / "docs" / "kanban"

COLUMNS = ["backlog", "ready", "in-progress", "verifying", "blocked", "done"]
WIP_LIMIT = 2

# The four planning arcs. Each was once a roadmap document; those are gone, and a card now
# carries its own argument, so there is no longer an external list of rows to check against.
# What survives of that check is the shape of an id and the agreement between id and arc.
ARCS = ["C", "D", "G", "P"]

# A card that belongs to no arc takes one of these instead of an arc letter.
KINDS = ["bug", "chore", "measure", "doc"]

ID_RE = re.compile(r"^(?:[CDGP]\d+b?|(?:bug|chore|measure|doc)-[a-z0-9-]+)$")
REQUIRED = ["id", "title", "arc", "size", "verification"]
# Anything else is a typo or a field somebody invented. `status:` and `roadmap:` are called
# out by name because both used to be legitimate and are now specifically wrong.
ALLOWED = set(REQUIRED) | {"blocked-by"}
SIZE_RE = re.compile(r"^(?:S|M|L|XL)(?:-(?:S|M|L|XL))*$")

# The count has come out of the golden token, at the third spelling this set predicted.
# `golden` is what a card written now names; `golden-<n>` stays accepted because cards in
# done/ ran against eleven or twelve cases and their record must keep saying which.
GOLDEN_RE = re.compile(r"^golden(?:-\d+)?$")
VERIFICATIONS = {
    "golden", "tests-4", "tests-hosted", "validation", "trace",
    "leak", "scaffold", "inspection", "scripts-fail", "readback", "scripted-input",
}

errors: list[str] = []


def fail(path: pathlib.Path, message: str) -> None:
    errors.append(f"{path.relative_to(REPO)}: {message}")


def slug(title: str) -> str:
    s = title.lower().replace("/", " ").replace("'", "")
    return re.sub(r"[^a-z0-9]+", "-", s).strip("-")


def raw_body(path: pathlib.Path) -> str:
    """Everything after the closing `---`."""
    text = path.read_text()
    m = re.search(r"^---\n.*?^---\n", text, re.S | re.M)
    return text[m.end():] if m else text


def section_body(body: str, name: str) -> str:
    """The text under `## <name>`, empty if the section is absent or a bare heading."""
    m = re.search(rf"^## {re.escape(name)}\s*$\n(.*?)(?=^## |\Z)", body, re.S | re.M)
    return m.group(1).strip() if m else ""


def parse_front_matter(path: pathlib.Path) -> dict[str, str] | None:
    """Deliberately not a YAML parser. The frontmatter is flat `key: value` by contract, and
    depending on PyYAML for that would put a package on the path of a check that has to run
    anywhere."""
    lines = path.read_text().splitlines()
    if not lines or lines[0].strip() != "---":
        fail(path, "no frontmatter -- the first line must be `---`")
        return None
    try:
        end = lines.index("---", 1)
    except ValueError:
        fail(path, "frontmatter is not closed by a second `---`")
        return None

    fields: dict[str, str] = {}
    for n, line in enumerate(lines[1:end], start=2):
        if not line.strip():
            continue
        if ":" not in line:
            fail(path, f"line {n} is not `key: value`: {line!r}")
            continue
        key, _, value = line.partition(":")
        key = key.strip()
        if key in fields:
            fail(path, f"line {n} repeats the key {key!r}")
        fields[key] = value.strip()
    return fields


def collect() -> dict[str, tuple[str, pathlib.Path, dict[str, str]]]:
    """id -> (column, path, fields). Reports duplicates rather than silently keeping one."""
    cards: dict[str, tuple[str, pathlib.Path, dict[str, str]]] = {}
    for column in COLUMNS:
        directory = BOARD / column
        # A missing column is an empty column, not an error. Git does not track empty
        # directories, and a placeholder file per column would be six files whose only job is
        # to stop this check complaining -- the check is the thing that should give way.
        # `mkdir -p` before a move is the other half of this; see the kanban skill.
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.md")):
            fields = parse_front_matter(path)
            if fields is None:
                continue

            for key in REQUIRED:
                if key not in fields:
                    fail(path, f"frontmatter has no {key!r}")
            if "status" in fields:
                fail(path, "frontmatter carries `status:` -- the directory is the status, and "
                           "a second copy of it is the one thing this layout exists to prevent")
            if "roadmap" in fields:
                fail(path, "frontmatter carries `roadmap:` -- the roadmaps are gone and the "
                           "card carries its own argument now")
            for key in sorted(set(fields) - ALLOWED - {"status", "roadmap"}):
                fail(path, f"frontmatter key {key!r} is not one of "
                           f"{', '.join(sorted(ALLOWED))}")

            cid = fields.get("id", "")
            if not ID_RE.match(cid):
                fail(path, f"id {cid!r} is not an arc row (C11, G5b) or a kinded id (bug-slug)")
                continue
            if cid in cards:
                other = cards[cid][1].relative_to(REPO)
                fail(path, f"id {cid!r} is already used by {other}")
                continue

            title = fields.get("title", "")
            # An arc row's id says nothing about its title, so the filename carries both.
            # A kinded id already *is* the title's slug -- `bug-empty-scene-bring-up` -- so
            # appending the slug again would name the file twice over. The check is the same
            # check either way: the filename, the id and the title may not drift apart.
            kinded = "-" in cid
            expected = f"{cid}.md" if kinded else f"{cid}-{slug(title)}.md"
            if title and path.name != expected:
                fail(path, f"filename does not match id and title -- expected {expected}")
            if title and kinded and cid != f"{cid.split('-', 1)[0]}-{slug(title)}":
                fail(path, f"id {cid!r} is not its kind and the slug of its title -- expected "
                           f"{cid.split('-', 1)[0]}-{slug(title)}")

            arc = fields.get("arc", "")
            if arc not in [*ARCS, *KINDS]:
                fail(path, f"arc {arc!r} is not one of {', '.join([*ARCS, *KINDS])}")
            elif arc in ARCS and not cid.startswith(arc):
                fail(path, f"id {cid!r} does not begin with its arc {arc!r}")

            # The card is the only record of this work now, so it has to actually be one.
            body = raw_body(path)
            if f"# {cid} " not in body:
                fail(path, f"body has no `# {cid} — <title>` heading")
            if "## Verification" not in body:
                fail(path, "body has no `## Verification` section -- with the roadmaps gone "
                           "the card is the only place the contract can live")
            elif not section_body(body, "Verification"):
                fail(path, "`## Verification` is empty -- a heading is not a contract")
            # A done/ card without an outcome has lost what the row found out, permanently.
            # "Not recorded" is an acceptable outcome; an absent section is not.
            if column == "done" and not section_body(body, "Outcome"):
                fail(path, "is in done/ with no `## Outcome` -- what a row cost and what it "
                           "found is the most valuable thing on a closed card")

            size = fields.get("size", "")
            if size and not SIZE_RE.match(size):
                fail(path, f"size {size!r} is not S, M, L, XL or a revision like M-L")

            for token in [v.strip() for v in fields.get("verification", "").split(",")]:
                if token and token not in VERIFICATIONS and not GOLDEN_RE.match(token):
                    fail(path, f"verification {token!r} is not in the vocabulary: "
                               f"{', '.join(sorted(VERIFICATIONS))}")

            cards[cid] = (column, path, fields)
    return cards


def check_columns(cards) -> None:
    in_progress = [c for c in cards.values() if c[0] == "in-progress"]
    if len(in_progress) > WIP_LIMIT:
        names = ", ".join(sorted(f[2].get("id", "?") for f in in_progress))
        errors.append(
            f"docs/kanban/in-progress/: {len(in_progress)} cards over a limit of {WIP_LIMIT} "
            f"({names}). Builds cannot be chained, so a third card in flight does not go "
            f"faster -- it makes the golden set unable to say which change moved a pixel")

    for cid, (column, path, fields) in sorted(cards.items()):
        blocker = fields.get("blocked-by", "").strip()
        if column == "blocked" and not blocker:
            fail(path, "is in blocked/ but names no `blocked-by`")
        if column != "blocked" and blocker:
            fail(path, f"names `blocked-by` but sits in {column}/, not blocked/")
        # A blocker that looks like a card id has to be one; free text is allowed, because
        # the thing standing in the way is often not a card at all.
        if blocker and ID_RE.match(blocker) and blocker not in cards:
            fail(path, f"blocked-by {blocker!r} names no card on the board")

        if column in ("ready", "in-progress", "verifying") and not fields.get("verification"):
            fail(path, f"is in {column}/ with no verification named -- a card may not leave "
                       f"backlog/ until it says what would prove it done")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check docs/kanban/ holds together.")
    parser.add_argument("--quiet", action="store_true", help="print nothing unless it fails")
    args = parser.parse_args()

    if not BOARD.is_dir():
        print(f"error: {BOARD.relative_to(REPO)} does not exist", file=sys.stderr)
        return 1

    cards = collect()
    check_columns(cards)

    if errors:
        print(f"error: the board does not hold together ({len(errors)} problems):",
              file=sys.stderr)
        for message in errors:
            print(f"  {message}", file=sys.stderr)
        return 1

    if not args.quiet:
        counts = {c: sum(1 for v in cards.values() if v[0] == c) for c in COLUMNS}
        summary = "  ".join(f"{c} {counts[c]}" for c in COLUMNS if counts[c])
        print(f"{len(cards)} cards -- {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
