#!/bin/sh
# Performance runner (REWRITE_PLAN.md Z1 / §6 T1; replaces `cargo xtask perf`).
#
#   tools/perf.sh [--quick] [--update-baseline] [--only ID,ID,...] [--reps N]
#                 [--list] [--no-build] [--bin PATH]
#
# Builds the headless C perf-bench driver (tools/perfbench, `make perf-bench`) and runs it against
# the perf/budgets.toml scenarios. Each scenario builds a representative document and times the hot
# path through the public engine + CPU compositor APIs (no GPU, no window): p50/p95/max + peak RSS.
# Scenarios with no C counterpart yet (GPU-only, UI-tool strokes/previews, cancellation latency) are
# reported as "n/a" and never given a fabricated number.
#
#   --update-baseline   run the full set and write perf/baseline.json (what tools/scorecard.sh reads
#                       for the Performance table). Without it, nothing is written.
#   --quick             small synthetic documents (budgets don't apply); refused with --update-baseline.
#   --only ID,ID        run just these scenario ids.
#   --reps N            override the per-scenario repetition count.
#   --list              list the scenarios and exit (no build, no run).
#   --no-build          use an already-built driver (default: build/perf/bin/photocraft-perfbench).
#   --bin PATH          an explicit driver path.
#
# It never fails the build: a missing driver or a build failure is reported and exits 0.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUDGETS=$ROOT/perf/budgets.toml
BASELINE=$ROOT/perf/baseline.json
BUILD=${BUILD:-build/perf}
BIN=
quick=0
update=0
list=0
nobuild=0
fwd=""

while [ $# -gt 0 ]; do
    case $1 in
    --quick) quick=1; fwd="$fwd --quick" ;;
    --update-baseline) update=1 ;;
    --list) list=1 ;;
    --no-build) nobuild=1 ;;
    --bin) BIN=$2; shift ;;
    --only) fwd="$fwd --only $2"; shift ;;
    --reps) fwd="$fwd --reps $2"; shift ;;
    --threshold | --bench) shift ;; # accepted for argument-compatibility; the C driver ignores them
    --skip-build) nobuild=1 ;;
    -h | --help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) printf 'perf: ignoring unknown argument %s\n' "$1" >&2 ;;
    esac
    shift
done

[ -f "$BUDGETS" ] || { printf 'perf: %s missing\n' "$BUDGETS" >&2; exit 0; }

if [ "$list" -eq 1 ]; then
    scenarios=$(grep -c '^\[\[scenario\]\]' "$BUDGETS" 2>/dev/null || echo 0)
    printf 'perf: %s scenarios defined in perf/budgets.toml.\n\n' "$scenarios"
    if [ -n "$BIN" ] && [ -x "$BIN" ]; then
        "$BIN" --list
    elif [ -x "$ROOT/$BUILD/bin/photocraft-perfbench" ]; then
        "$ROOT/$BUILD/bin/photocraft-perfbench" --list
    else
        printf '(build the driver with `make perf-bench` to list its scenario mapping)\n'
    fi
    exit 0
fi

if [ "$update" -eq 1 ] && [ "$quick" -eq 1 ]; then
    printf 'perf: --update-baseline cannot be combined with --quick (budgets need the full run).\n' >&2
    exit 0
fi

# ---- locate / build the driver -----------------------------------------------------------------
if [ -z "$BIN" ]; then
    BIN=$ROOT/$BUILD/bin/photocraft-perfbench
fi
if [ "$nobuild" -eq 0 ]; then
    printf 'perf: building the C perf-bench driver (make perf-bench, BUILD=%s)...\n' "$BUILD"
    if ! ( cd "$ROOT" && make BUILD="$BUILD" -j"${PERF_JOBS:-4}" "$BUILD/bin/photocraft-perfbench" ) >/dev/null 2>&1; then
        printf 'perf: could not build the perf-bench driver; see `make perf-bench`. Nothing run.\n' >&2
        exit 0
    fi
fi
if [ ! -x "$BIN" ]; then
    printf 'perf: driver %s not found; run `make perf-bench`. Nothing run.\n' "$BIN" >&2
    exit 0
fi

# ---- run ---------------------------------------------------------------------------------------
if [ "$update" -eq 1 ]; then
    TMP=$(mktemp "${TMPDIR:-/tmp}/pc-baseline.XXXXXX.json")
    # shellcheck disable=SC2086
    "$BIN" $fwd --json "$TMP"
    mv "$TMP" "$BASELINE"
    printf '\nperf: wrote %s (read by tools/scorecard.sh). Regenerate docs/scorecard.md with `make scorecard`.\n' "$BASELINE"
else
    # shellcheck disable=SC2086
    "$BIN" $fwd
    printf '\nperf: baseline unchanged. Use --update-baseline to publish perf/baseline.json.\n'
fi

exit 0
