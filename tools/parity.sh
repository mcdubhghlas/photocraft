#!/bin/sh
# Regenerate docs/parity-checklist.md (every Photoshop menu item, live or missing) from the C menu
# catalog (src/ui/menu_catalog.c) and the live command registry (REWRITE_PLAN.md Z1; replaces
# `cargo xtask parity`). Deterministic: the catalogue and registry are static, so the output depends
# only on the built binary.
#
#   tools/parity.sh            regenerate docs/parity-checklist.md
#   tools/parity.sh --stdout   print the report instead of writing the file
#
# The report is produced by the headless photocraft-parity tool (src/ui/paritygen -> ui/parity.c).
# Set BUILD to use a build dir other than build/default.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-build/default}
BIN=$ROOT/$BUILD/bin/photocraft-parity
OUT=docs/parity-checklist.md

die() {
    printf 'parity: %s\n' "$*" >&2
    exit 1
}

# Build the tool if it is missing; a clear error if that is not possible.
if [ ! -x "$BIN" ] || [ -n "${PARITY_REBUILD:-}" ]; then
    command -v make >/dev/null 2>&1 || die "$BIN is missing and make is not on PATH; run 'make' first"
    ( cd "$ROOT" && make "$BUILD/bin/photocraft-parity" ) >&2 || die "building photocraft-parity failed"
fi
[ -x "$BIN" ] || die "$BIN not found; run 'make' first"

if [ "${1:-}" = "--stdout" ]; then
    exec "$BIN"
fi

"$BIN" --write "$ROOT/$OUT" || die "generating $OUT failed"
