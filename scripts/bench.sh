#!/usr/bin/env bash
#
# Per-pass GPU cost for one configuration.
#
#   scripts/bench.sh [runs] [msaa] [-- extra flags]
#
#   scripts/bench.sh                  3 runs at 4x
#   scripts/bench.sh 5 8              5 runs at 8x
#   scripts/bench.sh 3 4 -- --no-ssr  3 runs at 4x with SSR off
#
# A front end for scripts/baseline.py, which does the running and the parsing. This
# used to awk the `GPU @` log line, and that was wrong in a way worth recording: that
# line prints `GpuProfiler::lastZoneMs`, the duration of *one* frame -- whichever the
# last collect happened to land on. Five runs of it is the median of five arbitrary
# frames, which is why `GBuffer` and `Bloom` read as wildly bimodal. Reading the trace
# instead puts a few hundred frames behind every number. See 5.5.
#
# The min/max columns are the point of this view. A zone whose max is twice its median
# is a zone whose median is not worth quoting -- and the whole *run* still lands in one
# of two states about 5% apart, so a before/after wants more than one run of each.
#
# The Frame zone is the one to quote for a whole-renderer number. The per-pass columns
# are for attribution -- toggle a feature, re-run, read the delta -- which is what the
# specialisation constants from 2.7 exist to make possible.
set -euo pipefail

cd "$(dirname "$0")/.."

RUNS="${1:-3}"
[[ "${1:-}" == "--" ]] && RUNS=3 || shift || true
MSAA=4
if [[ "${1:-}" != "--" && -n "${1:-}" ]]; then
    MSAA="$1"
    shift
fi
[[ "${1:-}" == "--" ]] && shift

exec scripts/baseline.py --zones --samples "$MSAA" --runs "$RUNS" -- "$@"
