#!/usr/bin/env bash
#
# RenderDoc capture and frame analysis (5.7).
#
#   scripts/rdoc.sh capture [config] [-- flags]   capture one frame, print the .rdc path
#   scripts/rdoc.sh passes    <rdc|xml>           pass tree: labels, pipelines, draw counts
#   scripts/rdoc.sh state     <rdc|xml> <chunk>   everything bound at one command
#   scripts/rdoc.sh barriers  <rdc|xml>           every image barrier, by pass
#   scripts/rdoc.sh resources <rdc|xml>           the ResourceId -> name map
#   scripts/rdoc.sh xml       <rdc>               convert only; print the .xml path
#   scripts/rdoc.sh thumb     <rdc>               the captured frame as a JPG
#
# The analysis commands take either a .rdc or an already-converted .xml. Handing them a
# .rdc converts it once next to the capture and reuses that file afterwards, because
# conversion is three seconds and analysing the same frame four ways is normal.
#
# What this reads is the *command stream* -- what the engine submitted and what was
# bound when it did. It cannot show what any of it produced: RenderDoc's replay API
# needs a Python module the official Linux build does not ship, and `qrenderdoc
# --python` is a silent no-op here. For pixels, `--capture-target` reads any named
# render target straight out of the engine; see scripts/rdoc/analyse.py for the detail.
#
# Captures are large -- a Sponza frame is about 1.5 GB, because an .rdc carries every
# resource the frame referenced. Delete them when you are done.
set -euo pipefail

usage() {
    # Print the leading comment block, minus the shebang, stopping at the first
    # line of actual code. Beats hard-coded line ranges, which drift.
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[0]}"
}

cd "$(dirname "$0")/.."

# RenderDoc is not packaged on most distributions and its official build is a tarball
# unpacked wherever the user put it, so there is no path worth defaulting to. RDOC_ROOT
# names that directory; failing that, an installed renderdoccmd on PATH is used and
# RDOC_ROOT is derived back from it, because the Python helpers below need the root and
# not just the binary.
if [[ -n "${RDOC_ROOT:-}" ]]; then
    RENDERDOCCMD="$RDOC_ROOT/bin/renderdoccmd"
elif RENDERDOCCMD="$(command -v renderdoccmd 2>/dev/null)"; then
    RDOC_ROOT="$(cd "$(dirname "$RENDERDOCCMD")/.." && pwd)"
else
    RDOC_ROOT=""
    RENDERDOCCMD=""
fi

require_renderdoc() {
    if [[ -z "$RENDERDOCCMD" || ! -x "$RENDERDOCCMD" ]]; then
        echo "error: renderdoccmd not found." >&2
        echo "       Set RDOC_ROOT to the RenderDoc install directory, or put" >&2
        echo "       renderdoccmd on PATH. RenderDoc: https://renderdoc.org/builds" >&2
        exit 1
    fi
}

# A .rdc is converted beside itself and the .xml reused. Returns the .xml path.
to_xml() {
    local input="$1"
    if [[ "$input" == *.xml ]]; then
        echo "$input"
        return
    fi
    require_renderdoc
    local out="${input%.rdc}.xml"
    if [[ ! -f "$out" || "$input" -nt "$out" ]]; then
        echo "==> converting $input" >&2
        "$RENDERDOCCMD" convert -f "$input" -o "$out" -c xml >&2
    fi
    echo "$out"
}

need_file() {
    [[ -f "$1" ]] || {
        echo "error: no such capture: $1" >&2
        exit 1
    }
}

MODE="${1:-}"
[[ $# -gt 0 ]] && shift

case "$MODE" in
capture)
    CONFIG="release"
    if [[ $# -gt 0 && "$1" != "--" ]]; then
        CONFIG="$1"
        shift
    fi
    [[ "${1:-}" == "--" ]] && shift

    # 60 and 90 match scripts/golden.sh: past the load hitch, inside the profiler
    # window, and a stated frame rather than whichever one the run ended on.
    FRAME=60
    FRAMES=90
    OUT="debug_frames/rdoc/frame"
    mkdir -p debug_frames/rdoc

    [[ -x "build/$CONFIG/substrate" ]] || {
        echo "error: build/$CONFIG/substrate not built. Run: ./build.sh $CONFIG" >&2
        exit 1
    }

    # ENABLE_VULKAN_RENDERDOC_CAPTURE is what pulls the implicit capture layer into the
    # process; RenderDoc's layer json is gated on it. Without it the run succeeds and
    # writes no .rdc, which is the single easiest way to waste a capture -- hence here
    # and not in a note somewhere.
    #
    # SIGTERM specifically, per CLAUDE.md: the profiler's handler flushes the trace.
    export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
    # shellcheck disable=SC2086
    # --windowed against the rule that a frame budget unmaps the window: RenderDoc hooks the
    # present it is asked to capture, and this is the one harness whose output is a human
    # opening the capture rather than a number.
    timeout -s TERM 300 ./run.sh "$CONFIG" -- --windowed \
        --frames "$FRAMES" --rdoc-capture-frame "$FRAME" --rdoc-capture-path "$OUT" "$@"

    LATEST=$(ls -t debug_frames/rdoc/*.rdc 2>/dev/null | head -1 || true)
    if [[ -z "$LATEST" ]]; then
        echo "error: the run finished but wrote no .rdc." >&2
        echo "       --frames must exceed --rdoc-capture-frame ($FRAMES vs $FRAME here)." >&2
        exit 1
    fi
    echo "$LATEST"
    ;;

passes | barriers | resources)
    need_file "${1:?usage: scripts/rdoc.sh $MODE <rdc|xml>}"
    XML=$(to_xml "$1")
    python3 scripts/rdoc/analyse.py "$MODE" "$XML"
    ;;

state)
    need_file "${1:?usage: scripts/rdoc.sh state <rdc|xml> <chunkIndex>}"
    XML=$(to_xml "$1")
    python3 scripts/rdoc/analyse.py state "$XML" "${2:?a chunkIndex -- find one with: scripts/rdoc.sh passes}"
    ;;

xml)
    need_file "${1:?usage: scripts/rdoc.sh xml <rdc>}"
    to_xml "$1"
    ;;

thumb)
    need_file "${1:?usage: scripts/rdoc.sh thumb <rdc>}"
    require_renderdoc
    OUT="${1%.rdc}.jpg"
    "$RENDERDOCCMD" thumb "$1" -o "$OUT" >&2
    echo "$OUT"
    ;;

-h | --help | help | "")
    usage
    ;;

*)
    echo "error: unknown command '$MODE'" >&2
    usage >&2
    exit 1
    ;;
esac
