#!/bin/sh
# Count C source and test code per module (REWRITE_PLAN.md Z1; replaces `cargo xtask stats`).
# Prints a table to stdout; writes no file. Deterministic (modules in sorted order).
#
#   tools/stats.sh
#
# "files"/"lines" count src/<module>/**/*.{c,h}; "tests" counts RUN(test_*) registrations in
# tests/<module>/ (the test-harness run count). Third-party vendored code is not counted.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Lines and file count of the .c/.h files under a directory (0 0 when none).
count_dir() {
    # $1 = directory
    if [ ! -d "$1" ]; then
        echo "0 0"
        return
    fi
    files=$(find "$1" -type f \( -name '*.c' -o -name '*.h' \) | wc -l | tr -d ' ')
    if [ "$files" -eq 0 ]; then
        echo "0 0"
        return
    fi
    lines=$(find "$1" -type f \( -name '*.c' -o -name '*.h' \) -exec cat {} + | wc -l | tr -d ' ')
    echo "$files $lines"
}

# Test registrations (RUN(...)) under tests/<module>/, excluding the harness macro definition.
count_tests() {
    if [ ! -d "$1" ]; then
        echo 0
        return
    fi
    find "$1" -type f -name '*.c' -exec grep -hE '(^|[^A-Za-z_])RUN\(' {} + 2>/dev/null | wc -l | tr -d ' '
}

printf '%-14s %6s %8s %8s\n' module files lines tests
printf '%s\n' '-----------------------------------------'

tf=0
tl=0
tt=0
for d in src/*/; do
    m=${d#src/}
    m=${m%/}
    set -- $(count_dir "src/$m")
    files=$1
    lines=$2
    tests=$(count_tests "tests/$m")
    printf '%-14s %6s %8s %8s\n' "$m" "$files" "$lines" "$tests"
    tf=$((tf + files))
    tl=$((tl + lines))
    tt=$((tt + tests))
done

printf '%s\n' '-----------------------------------------'
printf '%-14s %6s %8s %8s\n' TOTAL "$tf" "$tl" "$tt"
