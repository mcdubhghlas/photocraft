#!/bin/sh
# Performance runner (REWRITE_PLAN.md Z1; replaces `cargo xtask perf`).
#
#   tools/perf.sh [--quick] [--update-baseline] [--threshold PCT] [--bench NAME]... [--list]
#
# STATUS: there is no C benchmark harness yet. In the Rust reference the perf scenarios
# (perf/budgets.toml) were measured by benches that lived as `photocraft-ui-egui` examples
# (perf_scenarios, interactive_bench, fx_bench, bevel_bench, ...); those are not ported to C.
# Until a C bench harness exists, this script does not run anything: it validates the arguments,
# reports the state, and lists the scenarios/budgets so the interface is in place. It never fails
# the build.
#
# The committed baseline (perf/baseline.json, the 2026-10-05 macOS reference run) is what
# tools/scorecard.sh reads for the Performance table; --update-baseline is refused here because no
# measurements are produced.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUDGETS=$ROOT/perf/budgets.toml
BASELINE=$ROOT/perf/baseline.json

quick=0
update=0
list=0
for arg in "$@"; do
    case $arg in
    --quick) quick=1 ;;
    --update-baseline) update=1 ;;
    --list) list=1 ;;
    --threshold | --bench) ;; # value follows; accepted, ignored (no harness)
    --skip-build | --reuse) ;;
    -h | --help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) ;; # a value for --threshold/--bench, or unknown: tolerated (dev tool, never crash the build)
    esac
done

[ -f "$BUDGETS" ] || { printf 'perf: %s missing\n' "$BUDGETS" >&2; exit 0; }

scenarios=$(grep -c '^\[\[scenario\]\]' "$BUDGETS" 2>/dev/null || echo 0)
printf 'perf: no C benchmark harness yet (the Rust perf benches were photocraft-ui-egui examples;\n'
printf '      not ported to C). %s scenarios are defined in perf/budgets.toml.\n' "$scenarios"
if [ -f "$BASELINE" ]; then
    date=$(grep -m1 '"date"' "$BASELINE" | sed 's/.*"date": *"//; s/".*//')
    printf '      Committed baseline: %s (perf/baseline.json); tools/scorecard.sh renders it.\n' "${date:-unknown}"
fi
[ "$quick" -eq 1 ] && printf '      (--quick requested; nothing to run.)\n'

if [ "$update" -eq 1 ]; then
    printf 'perf: --update-baseline refused: no measurements are produced without a C bench harness.\n' >&2
    exit 0
fi

if [ "$list" -eq 1 ]; then
    printf '\nScenarios (id / budget) from perf/budgets.toml:\n'
    awk '
        /^\[\[scenario\]\]/ { id=""; budget=""; target=""; metric="p95"; next }
        /^id *=/            { id=$0; sub(/^id *= *"/,"",id); sub(/".*/,"",id) }
        /^metric *=/        { metric=$0; sub(/^metric *= *"/,"",metric); sub(/".*/,"",metric) }
        /^budget_ms *=/     { budget=$0; sub(/^budget_ms *= */,"",budget); sub(/ *#.*/,"",budget) }
        /^target *=/        { target=$0; sub(/^target *= *"/,"",target); sub(/".*/,"",target) }
        /^not_measurable *=/{ target="(not measurable yet)" }
        /^$/ {
            if (id != "") {
                if (target != "") t=target
                else if (budget != "") t="<= " budget " ms " metric
                else t="track only"
                printf "  %-5s %s\n", id, t
                id=""
            }
        }
        END {
            if (id != "") {
                if (target != "") t=target
                else if (budget != "") t="<= " budget " ms " metric
                else t="track only"
                printf "  %-5s %s\n", id, t
            }
        }
    ' "$BUDGETS"
fi

exit 0
