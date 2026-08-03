#!/usr/bin/env bash
#
# Fail the build if a source file contains a byte outside the allowed set.
#
#   ./scripts/check_ascii.sh [dir ...]     defaults to engine/, game/, tests/ and tools/
#
# This exists because a stray non-English word once reached a source comment and
# was caught only by eye. Anything that is not plain ASCII is rejected, with one
# deliberate exception: U+2014 EM DASH, which is house style in comments across
# the whole codebase and would otherwise make this guard a rewrite rather than a
# check. Smart quotes, ellipses, non-Latin scripts and mojibake all still fail.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DIRS=("$@")
[ ${#DIRS[@]} -eq 0 ] && DIRS=(engine game tests tools)

# assets/ is excluded rather than listed around, because both trees that hold source now
# hold content beside it -- engine/assets/ and game/<name>/assets/. Reading a 143 MB
# JPEG with --binary-files=text is not a check, it is a stall that happens to pass.
# __pycache__ for the same reason and a worse one: a .pyc is NUL-dense, and a NUL
# reaching the second grep makes it call the whole stream binary.
#
# .git for the same reason again, and it is not hypothetical: every game but the demo is
# its own repository (see .gitignore), so game/<name>/.git is the normal arrangement and
# a zlib-deflated object store is the most NUL-dense thing in the tree.
# Strip the allowed sequences first, then anything left outside 0x00-0x7F is a
# violation. grep -n on the stripped stream keeps the reported line numbers right,
# because the substitution never removes a newline.
#
# **Both greps need --binary-files=text, not just the first.** Without it the second one
# answers a binary stream with "binary file matches" on stderr and nothing on stdout, so
# `found` comes back empty and this guard passes while it is holding a violation -- which
# is how a section sign sat in game/demo/DemoGame.h through every build that ran this.
found=$(
    LC_ALL=C.UTF-8 grep -rn --binary-files=text \
        --exclude-dir=assets --exclude-dir=__pycache__ --exclude-dir=.git \
        -P '[^\x00-\x7F]' "${DIRS[@]}" 2>/dev/null |
        LC_ALL=C.UTF-8 sed 's/\xe2\x80\x94//g' |
        LC_ALL=C.UTF-8 grep --binary-files=text -P '[^\x00-\x7F]' || true
)

if [ -n "$found" ]; then
    echo "error: non-ASCII bytes in source (only U+2014 EM DASH is allowed):" >&2
    echo "$found" >&2
    exit 1
fi
