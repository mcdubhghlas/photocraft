#!/bin/sh
# Enforce the module dependency graph (REWRITE_PLAN.md §3), run by `make check`.
#
# 1. A file in src/<module>/ may include "<other>/..." only if <other> is in its allowed list
#    below: the Rust crates' direct dependencies, plus base for everyone.
# 2. GUI headers (SDL, OpenGL, Nuklear) are allowed only in src/ui and src/app.
#
# Adding a module or a dependency is a design change: update this table and §3 together.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)

allowed() {
    case $1 in
    base) echo "" ;;
    geom | cms | psd | codecs | raw | affinity) echo "base" ;;
    color) echo "base cms" ;;
    raster) echo "base geom color" ;;
    doc) echo "base geom color raster" ;;
    ops) echo "base doc" ;;
    paint | algo) echo "base geom color raster" ;;
    vector) echo "base geom color raster doc" ;;
    text) echo "base geom color raster doc psd" ;;
    compose) echo "base geom color raster doc vector" ;;
    format) echo "base geom color raster doc codecs" ;;
    gpu) echo "base geom color raster doc compose" ;;
    io) echo "base geom color cms raster doc compose algo psd codecs raw affinity text paint vector format" ;;
    plugins) echo "base geom color raster" ;;
    engine) echo "base geom color cms raster doc ops compose paint algo text vector io psd format plugins codecs" ;;
    automation) echo "base geom doc compose codecs io format engine" ;;
    cli) echo "base automation engine io format codecs" ;;
    ui) echo "base geom color cms raster doc compose gpu engine io plugins algo text vector paint format codecs" ;;
    app) echo "base doc color raster geom compose engine codecs io format automation ui" ;;
    *) echo "UNKNOWN" ;;
    esac
}

is_module() {
    [ -d "$ROOT/src/$1" ]
}

status=0
files=$(cd "$ROOT" && find src -name '*.c' -o -name '*.h' | sort)
for f in $files; do
    mod=${f#src/}
    mod=${mod%%/*}
    allow=$(allowed "$mod")
    if [ "$allow" = UNKNOWN ]; then
        echo "$f: module '$mod' is not in tools/check-layers.sh" >&2
        status=1
        continue
    fi
    # Quoted includes of other modules.
    for inc in $(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/$f"); do
        case $inc in */*) ;; *) continue ;; esac
        dep=${inc%%/*}
        is_module "$dep" || continue
        [ "$dep" = "$mod" ] && continue
        case " $allow " in
        *" $dep "*) ;;
        *)
            echo "$f: includes \"$inc\" but $mod may not depend on $dep" >&2
            status=1
            ;;
        esac
    done
    # GUI headers only in the UI layer and the app.
    case $mod in ui | app) ;; *)
        if grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](SDL3?/|GL/|OpenGL/|photocraft_nuklear\.h|nuklear\.h)' "$ROOT/$f" >/dev/null; then
            echo "$f: GUI headers (SDL/GL/Nuklear) are only allowed in src/ui and src/app" >&2
            status=1
        fi
        ;;
    esac
done
[ $status -eq 0 ] && echo "layers: ok ($(echo "$files" | wc -l | tr -d ' ') files)"
exit $status
