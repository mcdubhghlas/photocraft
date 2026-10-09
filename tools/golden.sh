#!/bin/sh
# Record reference outputs from the Rust implementation as committed goldens
# (REWRITE_PLAN.md B4). Needs the Rust tree and tools/rustref (Cargo); run it while they exist —
# after Z3 the goldens are all that remain of the reference.
#
#   tools/golden.sh psd-info PATH...    -> tests/golden/psd-info/<path>.json
#   tools/golden.sh decode PATH...      -> tests/golden/decode/<path>.json    (+ "raw_sha256")
#   tools/golden.sh composite PATH...   -> tests/golden/composite/<path>.json (+ "raw_sha256")
#
# PATH may be a file or a directory (searched recursively). Paths under corpus/ are stored
# without the corpus/ prefix. Errors are goldens too: {"error": "..."}.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUSTREF=$ROOT/tools/rustref/target/release/rustref

die() {
    printf 'golden: %s\n' "$*" >&2
    exit 1
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256 -q "$1"
    fi
}

[ "$#" -ge 2 ] || die "usage: tools/golden.sh psd-info|decode|composite PATH..."
cmd=$1
shift
case $cmd in psd-info | decode | composite) ;; *) die "unknown command: $cmd" ;; esac

(cd "$ROOT/tools/rustref" && cargo build --release -q) || die "building tools/rustref failed"
tmp=$(mktemp "${TMPDIR:-/tmp}/golden.XXXXXX")
trap 'rm -f "$tmp"' EXIT INT TERM

count=0
for p in "$@"; do
    [ -e "$p" ] || die "$p: no such file or directory"
    find "$p" -type f ! -name '*.md' ! -name 'LICENSE*' ! -name '.*' | sort | while IFS= read -r f; do
        abs=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
        rel=${abs#"$ROOT"/}
        rel=${rel#corpus/}
        out=$ROOT/tests/golden/$cmd/$rel.json
        mkdir -p "$(dirname "$out")"
        if [ "$cmd" = psd-info ]; then
            "$RUSTREF" psd-info "$f" >"$out" || true
        else
            rm -f "$tmp"
            json=$("$RUSTREF" "$cmd" "$f" "$tmp" || true)
            if [ -f "$tmp" ] && ! printf '%s' "$json" | grep -q '^{"error"'; then
                json=$(printf '%s' "$json" | sed "s/}\$/,\"raw_sha256\":\"$(sha256_of "$tmp")\"}/")
            fi
            printf '%s\n' "$json" >"$out"
        fi
    done
    count=$((count + $(find "$p" -type f ! -name '*.md' ! -name 'LICENSE*' ! -name '.*' | wc -l)))
done
echo "golden: $count $cmd outputs under tests/golden/$cmd/"
