#!/usr/bin/env bash
#
# Fail the build if a directory under engine/ includes a header it is not allowed to.
#
#   ./scripts/check_layers.sh [--table]     --table prints the graph and exits
#
# A namespace names a module and a module is a directory under `engine/` -- the rule
# principles.md already stated. What it never had was a check, so `core/Config.h` grew an
# include of `gfx/` and another of `scene/`, two layers above it, and nothing said so for
# as long as it compiled.
#
# ## What a module is
#
# **What `root` can reach is not a module.** Anything Engine.h reaches is bidirectionally
# coupled with it -- it *is* the engine -- so `gfx`, `scene`, `ui` and `sim` are one cluster
# with `root` rather than four layers, and an include between any two of them is not a
# layering question. A module is exactly what root cannot reach.
#
# That leaves three tiers and two rules:
#
#   core                              depends on nothing. That is what makes it the bottom.
#   gfx scene ui sim root             the engine. Mutually coupled by construction.
#   ai nav particles physics audio    the modules. May name core and the engine.
#   anim
#
#   1. Nothing in core or the engine may name a module.
#   2. No module may name another module.
#
# Rule 1 is the point of the whole exercise: `engine/` is a static library and the linker
# pulls an archive member only to resolve an undefined symbol, so an object file every game
# links -- `Engine.cpp.o` -- that *names* a module drags that module into every game. A
# module is reached through an interface in `engine/Modules.h` whose default implementation
# does nothing, and swapped for the real one by a registrar in `<module>/<Name>Module.cpp`.
# Including that header is what links the module, and nothing else does.
#
# Rule 2 is what keeps the modules peers rather than a chain. Where two appear to need each
# other, what they share is *description* -- and description belongs in `scene/`, beside
# `Collider.h` and `AudioSource.h`, which are already exactly that split.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ---------------------------------------------------------------- the graph
#
# Two lists rather than a row per directory, because two lists is what the definition
# above actually is. A directory in neither, and not `core`, is an error rather than a
# pass -- so a new directory under engine/ has to be classified deliberately.
CLUSTER="gfx scene ui sim root"
MODULES="ai nav particles physics audio anim"

# Directories under engine/ that hold no translation unit and are not code.
NOT_A_MODULE="shaders assets"

# ---------------------------------------------------------------- what is not closed yet
#
# The edges that were here when the guard went into the build, each with the phase that
# closes it. **The guard fails on anything not in this list**, which is the property worth
# having on every build: the count can fall and cannot rise.
#
# A line is `<path>:<from>-><to>`, without the line number -- an accepted edge survives the
# file being reformatted, and pinning it to a line would turn every unrelated edit into a
# failure here. An entry that no longer matches anything is reported and fails: an exception
# outliving the edge it excused is how a list like this rots.
ACCEPTED=()

# ---------------------------------------------------------------- resolution
#
# Every member of the engine cluster answers to one name, so an edge inside it is a
# self-edge rather than a question.
tier() {
    case " $CLUSTER " in *" $1 "*) echo engine; return;; esac
    case " $MODULES " in *" $1 "*) echo module; return;; esac
    [ "$1" = core ] && echo core || echo unknown
}

# What a tier may include, as tiers.
allowed_tiers() {
    case "$1" in
        core)   echo "" ;;
        engine) echo "core engine" ;;
        module) echo "core engine" ;;
    esac
}

if [ "${1:-}" = "--table" ]; then
    printf '%-34s -> %s\n' core '(nothing)'
    printf '%-34s -> %s\n' "$CLUSTER" 'core, and each other'
    printf '%-34s -> %s\n' "$MODULES" 'core, the engine cluster'
    exit 0
fi

# ---------------------------------------------------------------- the tiers are a DAG
#
# Checked before a single file is read, because a cycle written into `allowed_tiers` would
# make every include legal and this script would pass by saying nothing. The engine cluster
# is one node here, which is the whole reason gfx <-> scene is not a cycle. White is
# unvisited, grey is on the current path, black is finished; grey reached twice is the cycle.
declare -A COLOUR=()
CYCLE=""

visit() {
    local node=$1 next
    COLOUR[$node]=grey
    for next in $(allowed_tiers "$node"); do
        [ "$next" = "$node" ] && continue
        case "${COLOUR[$next]:-white}" in
            grey)  CYCLE="$node -> $next"; return 1 ;;
            white) visit "$next" || { CYCLE="$node -> $CYCLE"; return 1; } ;;
        esac
    done
    COLOUR[$node]=black
    return 0
}

for t in core engine module; do
    if [ "${COLOUR[$t]:-white}" = white ]; then
        visit "$t" || { echo "error: the tier table has a cycle: $CYCLE" >&2; exit 2; }
    fi
done

# ---------------------------------------------------------------- the include walk
#
# Only quoted includes: <glm/glm.hpp> and every other angle-bracket form is a dependency
# rather than a directory of ours. An include with no slash names a header at the root of
# engine/, which is `root`.
declare -A SEEN=()
edges=0

while IFS= read -r hit; do
    file=${hit%%:*}
    rest=${hit#*:}
    line=${rest%%:*}
    text=${rest#*:}

    rel=${file#engine/}
    if [[ $rel == */* ]]; then from=${rel%%/*}; else from=root; fi
    [[ " $NOT_A_MODULE " == *" $from "* ]] && continue

    fromTier=$(tier "$from")
    if [ "$fromTier" = unknown ]; then
        echo "error: $file is in engine/$from/, which is in neither CLUSTER nor MODULES" >&2
        exit 2
    fi

    target=${text#*\"}
    target=${target%%\"*}
    if [[ $target == */* ]]; then to=${target%%/*}; else to=root; fi

    # A directory always reaches itself, and a vendored path reached through a quoted
    # include is not an edge in this graph at all.
    [ "$to" = "$from" ] && continue
    toTier=$(tier "$to")
    [ "$toTier" = unknown ] && continue

    # Two modules are peers, so one naming the other is a violation even though both sit
    # at the same tier. Checked before the tier test, which would otherwise pass it.
    if [ "$fromTier" = module ] && [ "$toTier" = module ]; then
        why="a module may not name another module"
    elif [[ " $(allowed_tiers "$fromTier") " == *" $toTier "* ]]; then
        continue
    else
        why="$fromTier may include: $(allowed_tiers "$fromTier")"
        [ -z "$(allowed_tiers "$fromTier")" ] && why="$fromTier may include nothing"
    fi

    key="$file:$from->$to"
    if [[ " ${ACCEPTED[*]} " == *" $key "* ]]; then
        SEEN[$key]=1
        continue
    fi
    printf '%s:%s: %s -> %s (%s)\n' "$file" "$line" "$from" "$to" "$why" >&2
    edges=$((edges + 1))
done < <(grep -rn -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' engine \
             --include='*.h' --include='*.cpp' --include='*.hpp' 2>/dev/null | sort)

# An accepted edge that matched nothing is an exception outliving what it excused. Failing
# on it is what keeps the list from becoming a place edges go to be forgotten.
stale=0
for key in "${ACCEPTED[@]}"; do
    if [ -z "${SEEN[$key]+set}" ]; then
        echo "error: accepted edge no longer exists, delete it from ACCEPTED: $key" >&2
        stale=$((stale + 1))
    fi
done

if [ "$edges" -gt 0 ] || [ "$stale" -gt 0 ]; then
    [ "$edges" -gt 0 ] && echo "layer guard: $edges upward include$([ "$edges" = 1 ] || echo s)" >&2
    exit 1
fi

if [ ${#ACCEPTED[@]} -gt 0 ]; then
    echo "layer guard: clean (${#ACCEPTED[@]} accepted edge$([ ${#ACCEPTED[@]} = 1 ] || echo s) remain; see ACCEPTED in $0)"
fi
