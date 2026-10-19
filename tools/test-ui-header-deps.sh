#!/bin/sh
# Regression for incremental shell ABI corruption: header changes must recompile consumers,
# rather than merely relink stale objects. Run after installing the optional GUI dependencies.
# Usage: sh tools/test-ui-header-deps.sh [build/default]
set -eu
cd "$(dirname "$0")/.."
build_dir=${1:-build/default}
if ! pkg-config --exists sdl3 freetype2; then
    echo 'UI header dependencies: skipped (SDL3/FreeType unavailable)'
    exit 0
fi
# A cold tree would mask the defect by compiling everything anyway. Establish a current build
# before asking make what it would do after changing a header. -W never touches the header.
make BUILD="$build_dir" all >/dev/null
dry_run=$(mktemp)
trap 'rm -f "$dry_run"' EXIT HUP INT TERM
make -n -W src/ui/canvas.h BUILD="$build_dir" all >"$dry_run"
for source in src/ui/shell/app.c src/ui/shell/canvas_view.c src/ui/shell/control.c; do
    if ! grep -F -- "-c $source " "$dry_run" >/dev/null; then
        echo "UI header dependencies: $source would retain a stale canvas layout" >&2
        exit 1
    fi
done
make -n -W src/ui/theme.h BUILD="$build_dir" all >"$dry_run"
if ! grep -F -- '-c src/ui/snapshot/main.c ' "$dry_run" >/dev/null; then
    echo 'UI header dependencies: the snapshot would retain stale theme declarations' >&2
    exit 1
fi
make -n -W third_party/nuklear/nuklear.h BUILD="$build_dir" all >"$dry_run"
for source in third_party/nuklear/nuklear.c src/ui/spike/font.c src/ui/spike/render.c src/ui/shell/app.c; do
    if ! grep -F -- "-c $source " "$dry_run" >/dev/null; then
        echo "UI header dependencies: $source would retain a stale Nuklear ABI" >&2
        exit 1
    fi
done
echo 'UI header dependencies: shell and snapshot consumers rebuild'
