#!/bin/sh
# Fetch and verify the real-file test corpora into corpus/ (gitignored, never committed).
# Port of `cargo xtask corpus` (REWRITE_PLAN.md B4). Pins: tools/corpus/pins.txt; manifests:
# tools/corpus/<name>.sha256. Needs curl, tar and sha256sum / shasum / sha256.
#
#   tools/corpus.sh list                 what is pinned and what is present
#   tools/corpus.sh fetch NAME... | all  download what is missing or stale, verify every file
#   tools/corpus.sh verify NAME... | all check what is on disk against the manifests (exit 1 if not)
#
# Names: psd psd-tools photoshop heif exr affinity pngsuite.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PINS=$ROOT/tools/corpus/pins.txt
CORPUS=$ROOT/corpus
UA=Photocraft-dev
PNGSUITE_URL=http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz
PNGSUITE_SHA256=0294b244c95a8342c01b00010cf34abdcabc7c6a34fd0fe1bd963917537bfdc8
ALL="psd psd-tools photoshop heif exr affinity pngsuite"

die() {
    printf 'corpus: %s\n' "$*" >&2
    exit 1
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256 >/dev/null 2>&1; then
        sha256 -q "$1"
    else
        die "no sha256sum, shasum or sha256 found"
    fi
}

manifest_of() {
    printf '%s/tools/corpus/%s.sha256' "$ROOT" "$1"
}

# Print "missing" / "bad" lines for corpus $1 and return 1 if anything is wrong.
check() {
    c_name=$1
    c_dest=$CORPUS/$c_name
    c_bad=0
    [ -d "$c_dest" ] || return 1
    while IFS= read -r c_line || [ -n "$c_line" ]; do
        [ -n "$c_line" ] || continue
        c_hash=${c_line%%  *}
        c_file=${c_line#*  }
        if [ ! -f "$c_dest/$c_file" ]; then
            printf '  missing: %s\n' "$c_file"
            c_bad=1
        elif [ "$(sha256_of "$c_dest/$c_file")" != "$c_hash" ]; then
            printf '  sha256 mismatch: %s\n' "$c_file"
            c_bad=1
        fi
    done <"$(manifest_of "$c_name")"
    return $c_bad
}

pngsuite_present() {
    [ -f "$CORPUS/pngsuite/.sha256" ] && [ "$(cat "$CORPUS/pngsuite/.sha256")" = "$PNGSUITE_SHA256" ]
}

fetch_pngsuite() {
    if pngsuite_present; then
        echo "pngsuite: up to date"
        return 0
    fi
    p_tmp=$CORPUS/.pngsuite-download
    rm -rf "$p_tmp"
    mkdir -p "$p_tmp"
    curl -fsSL --retry 3 -A "$UA" -o "$p_tmp/ps.tgz" "$PNGSUITE_URL" || die "download failed: $PNGSUITE_URL"
    [ "$(sha256_of "$p_tmp/ps.tgz")" = "$PNGSUITE_SHA256" ] || die "pngsuite: archive sha256 mismatch"
    rm -rf "$CORPUS/pngsuite"
    mkdir -p "$CORPUS/pngsuite"
    tar -xzf "$p_tmp/ps.tgz" -C "$CORPUS/pngsuite"
    printf '%s' "$PNGSUITE_SHA256" >"$CORPUS/pngsuite/.sha256"
    rm -rf "$p_tmp"
    echo "pngsuite: $(ls "$CORPUS/pngsuite" | wc -l | tr -d ' ') files in corpus/pngsuite"
}

fetch() {
    f_name=$1
    [ -f "$(manifest_of "$f_name")" ] || die "unknown corpus: $f_name"
    if check "$f_name" >/dev/null 2>&1; then
        echo "$f_name: up to date"
        return 0
    fi
    f_stage=$CORPUS/.$f_name-download
    rm -rf "$f_stage"
    mkdir -p "$f_stage"
    f_i=0
    # Download each upstream at its pinned commit and unpack only what we need.
    grep -v '^#' "$PINS" | grep "^$f_name|" >"$f_stage/pins"
    while IFS='|' read -r _c f_prefix f_repo f_commit f_subdir f_extras; do
        f_i=$((f_i + 1))
        f_top="${f_repo##*/}-$f_commit"
        f_url="https://codeload.github.com/$f_repo/tar.gz/$f_commit"
        curl -fsSL --retry 3 -A "$UA" -o "$f_stage/a$f_i.tgz" "$f_url" || die "download failed: $f_url"
        mkdir -p "$f_stage/u$f_i"
        set -- "$f_top/$f_subdir"
        f_old_ifs=$IFS
        IFS=,
        for f_pair in $f_extras; do
            set -- "$@" "$f_top/${f_pair%%:*}"
        done
        IFS=$f_old_ifs
        tar -xzf "$f_stage/a$f_i.tgz" -C "$f_stage/u$f_i" "$@" || die "$f_name: unpacking $f_repo failed"
        rm -f "$f_stage/a$f_i.tgz"
        printf '%s|%s|%s\n' "$f_prefix" "$f_stage/u$f_i/$f_top/$f_subdir" "$f_stage/u$f_i/$f_top" >>"$f_stage/roots"
    done <"$f_stage/pins"

    rm -rf "$CORPUS/$f_name"
    mkdir -p "$CORPUS/$f_name"
    while IFS= read -r f_line || [ -n "$f_line" ]; do
        [ -n "$f_line" ] || continue
        f_file=${f_line#*  }
        case $f_file in /* | *..*) die "$f_name: unsafe manifest path: $f_file" ;; esac
        # The upstream whose prefix matches the longest.
        f_best=""
        f_bestlen=-1
        while IFS='|' read -r r_prefix r_dir _r_top; do
            case $f_file in
            "$r_prefix"*)
                if [ ${#r_prefix} -gt $f_bestlen ]; then
                    f_bestlen=${#r_prefix}
                    f_best="$r_dir/${f_file#"$r_prefix"}"
                fi
                ;;
            esac
        done <"$f_stage/roots"
        [ -n "$f_best" ] && [ -f "$f_best" ] || die "$f_name: $f_file not found upstream"
        mkdir -p "$(dirname "$CORPUS/$f_name/$f_file")"
        mv "$f_best" "$CORPUS/$f_name/$f_file"
    done <"$(manifest_of "$f_name")"

    # Licence and readme files next to the corpus.
    f_i=0
    while IFS='|' read -r _c _p _r _m _s f_extras; do
        f_i=$((f_i + 1))
        f_root=$(sed -n "${f_i}p" "$f_stage/roots" | cut -d'|' -f3)
        f_old_ifs=$IFS
        IFS=,
        for f_pair in $f_extras; do
            if [ -f "$f_root/${f_pair%%:*}" ]; then
                mkdir -p "$(dirname "$CORPUS/$f_name/${f_pair#*:}")"
                cp "$f_root/${f_pair%%:*}" "$CORPUS/$f_name/${f_pair#*:}"
            fi
        done
        IFS=$f_old_ifs
    done <"$f_stage/pins"
    {
        printf '# %s test corpus\n\nFetched by tools/corpus.sh, sha256-verified against tools/corpus/%s.sha256.\n' "$f_name" "$f_name"
        printf 'Unmodified files from (licences next to them):\n\n'
        while IFS='|' read -r _c f_prefix f_repo f_commit f_subdir _e; do
            printf -- '- `%s` https://github.com/%s/tree/%s/%s\n' "${f_prefix:-.}" "$f_repo" "$f_commit" "$f_subdir"
        done <"$f_stage/pins"
        printf '\nGitignored; never commit these files.\n'
    } >"$CORPUS/$f_name/SOURCES.md"
    rm -rf "$f_stage"
    check "$f_name" || die "$f_name: verification failed after download"
    echo "$f_name: $(wc -l <"$(manifest_of "$f_name")" | tr -d ' ') files in corpus/$f_name, verified"
}

names() {
    if [ "$#" -eq 0 ] || [ "$1" = all ]; then
        echo "$ALL"
    else
        echo "$*"
    fi
}

cmd=${1:-list}
[ "$#" -gt 0 ] && shift
case $cmd in
list)
    for n in $ALL; do
        if [ "$n" = pngsuite ]; then
            pngsuite_present && s=present || s=missing
            printf '%-10s %-8s %s\n' "$n" "$s" "$PNGSUITE_URL"
        else
            check "$n" >/dev/null 2>&1 && s=present || s=missing
            printf '%-10s %-8s %s files, %s\n' "$n" "$s" "$(wc -l <"$(manifest_of "$n")" | tr -d ' ')" \
                "$(grep "^$n|" "$PINS" | cut -d'|' -f3 | tr '\n' ' ')"
        fi
    done
    ;;
fetch)
    command -v curl >/dev/null 2>&1 || die "curl is required"
    mkdir -p "$CORPUS"
    for n in $(names "$@"); do
        if [ "$n" = pngsuite ]; then fetch_pngsuite; else fetch "$n"; fi
    done
    ;;
verify)
    rc=0
    for n in $(names "$@"); do
        if [ "$n" = pngsuite ]; then
            if pngsuite_present; then echo "pngsuite: ok"; else echo "pngsuite: missing (tools/corpus.sh fetch pngsuite)"; rc=1; fi
        elif check "$n"; then
            echo "$n: ok"
        else
            echo "$n: missing or stale (tools/corpus.sh fetch $n)"
            rc=1
        fi
    done
    exit $rc
    ;;
*)
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
