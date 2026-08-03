#!/usr/bin/env bash
#
# Scaffold a new game under game/.
#
#   ./new_game.sh mygame           creates game/mygame/ and tells you how to build it
#   ./new_game.sh --list           names every game already in the tree
#
# The template is a Game subclass, a one-line CMakeLists.txt and a README, copied from
# scripts/template/game/ with the name substituted in. It loads no scene and draws one
# panel, which is the smallest thing that proves the loop: a game that names itself, takes
# a key, and draws.
#
# What makes this more than a convenience is the second consumer. The engine/game boundary
# was argued for on the grounds that a wish for modularity is not a second consumer -- so
# a scaffolded game beside game/demo/ is literally the thing that turns that boundary from
# speculative into exercised. A template that must not edit anything under engine/ is a
# continuously checked assertion that the public surface is complete.
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

TEMPLATE="scripts/template/game"
NAME="${1:-}"

case "$NAME" in
-h | --help | help)
    usage
    echo "games:"
    list_games
    exit 0
    ;;
--list | list)
    list_games
    exit 0
    ;;
"")
    echo "error: no name given. Usage: ./new_game.sh <name>" >&2
    exit 1
    ;;
esac

# A game's name reaches three places that constrain it: a directory, a CMake target, and
# -- through the class -- a C++ identifier. Checked here rather than discovered as a
# CMake error three commands later, which is the whole difference between a rejected name
# and a broken build directory.
if ! [[ "$NAME" =~ ^[a-z][a-z0-9_]*$ ]]; then
    echo "error: '$NAME' must be lowercase, start with a letter, and hold only letters," >&2
    echo "       digits and underscores -- it becomes a directory, a CMake target and" >&2
    echo "       part of a C++ class name." >&2
    exit 1
fi

if [ -e "game/$NAME" ]; then
    echo "error: game/$NAME already exists" >&2
    exit 1
fi

# mygame -> MygameGame. Ugly for one-word names and unambiguous for all of them, which is
# the trade a generated identifier should make: nothing here has to guess where a word
# boundary was.
CLASS="$(printf '%s' "${NAME:0:1}" | tr '[:lower:]' '[:upper:]')${NAME:1}Game"

mkdir -p "game/$NAME"

# Written to a temp and moved into place, so a failure part-way leaves no half-scaffolded
# directory for the next run to refuse.
render() {
    local src="$1" dst="$2"
    sed -e "s/@NAME@/$NAME/g" -e "s/@CLASS@/$CLASS/g" "$src" >"$dst.tmp"
    mv "$dst.tmp" "$dst"
}

render "$TEMPLATE/Game.h.in" "game/$NAME/$CLASS.h"
render "$TEMPLATE/Game.cpp.in" "game/$NAME/$CLASS.cpp"
render "$TEMPLATE/CMakeLists.txt.in" "game/$NAME/CMakeLists.txt"
render "$TEMPLATE/README.md.in" "game/$NAME/README.md"

echo "created game/$NAME/"
echo "  $CLASS.h  $CLASS.cpp  CMakeLists.txt  README.md"
echo
echo "next:"
echo "  ./build_game.sh $NAME"
echo "  ./run.sh"
