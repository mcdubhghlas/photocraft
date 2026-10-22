#!/bin/sh
# Regenerate docs/scorecard.md (REWRITE_PLAN.md Z1; replaces `cargo xtask scorecard`).
#
#   tools/scorecard.sh            regenerate docs/scorecard.md
#   tools/scorecard.sh --check    exit non-zero if docs/scorecard.md is stale (no write)
#
# Sources, all committed, so the output is deterministic:
#   perf/budgets.toml + perf/baseline.json   the Performance table (committed baseline, full run)
#   scorecard/*.toml                         per-area checklists
#   tests/io/test_io_corpus.c                corpus floors (the C corpus test's enforced minimums)
#   docs/parity-checklist.md                 the menu-wiring line (run tools/parity.sh first)
#   src/engine/prefs.c + the src/ tree       the C prefs audit ("settings that do nothing")
#
# Deviations from the Rust xtask (the C tree differs from the Rust reference): the prefs audit is a
# C-native path-literal heuristic over src/engine/prefs.c; never-crash reports the C -Werror gate and
# panic_hunt instead of per-crate clippy lints; corpus floors come from the C test. No cargo/xtask.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=docs/scorecard.md
BUDGETS=perf/budgets.toml
BASELINE=perf/baseline.json
CORPUS_C=tests/io/test_io_corpus.c
PREFS_C=src/engine/prefs.c
PARITY_DOC=docs/parity-checklist.md

die() { printf 'scorecard: %s\n' "$*" >&2; exit 1; }

check=0
[ "${1:-}" = "--check" ] && check=1

for f in "$BUDGETS" "$BASELINE" "$PREFS_C" "$CORPUS_C"; do
    [ -f "$f" ] || die "$f missing"
done
[ -d scorecard ] || die "scorecard/ missing"

TMP=${TMPDIR:-/tmp}/scorecard.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

# ---- 1. baseline.json -> flat records -------------------------------------------------------
awk '
  function sp(s,  n){ n=0; while(substr(s,n+1,1)==" ") n++; return n }
  { ind=sp($0); line=$0; sub(/^ +/,"",line); sub(/,$/,"",line) }
  ind==2 && line ~ /^"machine": *\{/ { inm=1; next }
  ind==2 && line ~ /^"(machine_class|modes|schema)":/ { inm=0 }
  inm && ind==4 && line ~ /^"(cpu|gpu_adapter|os_version)":/ {
    k=line; sub(/^"/,"",k); sub(/".*/,"",k)
    v=line; sub(/^"[^"]*": *"/,"",v); sub(/"$/,"",v); print "M " k " " v; next }
  inm && ind==4 && line ~ /^"ram_bytes":/ { v=line; sub(/^"ram_bytes": */,"",v); print "M ram_bytes " v; next }
  ind==4 && line ~ /^"full": *\{/ { infull=1; next }
  infull && ind==6 && line ~ /^"commit":/ { v=line; sub(/^"commit": *"/,"",v); sub(/"$/,"",v); print "F commit " v; next }
  infull && ind==6 && line ~ /^"date":/   { v=line; sub(/^"date": *"/,"",v); sub(/"$/,"",v); print "F date " v; next }
  infull && ind==6 && line ~ /^"load_avg_start": *\[/ { inla=1; next }
  inla && line ~ /^\]/ { inla=0; next }
  inla { v=line; gsub(/[ ,]/,"",v); if(v!=""){ lan++; print "L " lan " " v } next }
  infull && ind==6 && line ~ /^"scenarios": *\{/ { insc=1; inf=0; next }
  infull && ind==6 && line ~ /^"failed": *\{/ { inf=1; insc=0; next }
  insc && ind==8 && line ~ /^"P[0-9]+": *\{/ { cur=line; sub(/^"/,"",cur); sub(/".*/,"",cur); next }
  insc && ind==10 && cur!="" { k=line; sub(/^"/,"",k); sub(/".*/,"",k); v=line; sub(/^"[^"]*": */,"",v); print "S " cur " " k " " v; next }
  inf && ind==8 && line ~ /^"P[0-9]+":/ { id=line; sub(/^"/,"",id); sub(/".*/,"",id); v=line; sub(/^"[^"]*": *"/,"",v); sub(/"$/,"",v); print "X " id " " v; next }
' "$BASELINE" > "$TMP/base.txt"

# ---- 2. Performance: records (ordered), NM records, counts ----------------------------------
REG_PCT=$(awk -F= '/^regression_pct/{v=$2; gsub(/[^0-9.]/,"",v); printf "%.0f", v; exit}' "$BUDGETS")
awk -v TMP="$TMP" '
  function fnum(v){ if(v=="")return "\342\200\223"; if(v+0>=100)return sprintf("%.0f",v); else if(v+0>=10)return sprintf("%.1f",v); else return sprintf("%.2f",v) }
  function fbytes(v){ if(v==""||v+0==0)return "\342\200\223"; if(v+0>=1073741824)return sprintf("%.1f GB",v/1073741824); else return sprintf("%d MB",int(v/1048576)) }
  function ilink(n){ if(n=="")return ""; return "[#" n "](https://github.com/storytold/photocraft/issues/" n ")" }
  function flush(   mv,tgt,status,why,lab){
    if(id==""){return}
    if(target!="") tgt=target; else if(budget!="") tgt="\342\211\244 " budget " ms " metric; else tgt="track only"
    why=""
    if(notm!=""){ nm++; printf "N\t%s\t%s\t%s\t%s\t%s\n", id,title,tgt,notm,ilink(issue) > NMF; id=""; return }
    if((id, "p50_ms") in V){
      if(budget!=""){
        mv = (metric=="p50") ? V[id,"p50_ms"] : (metric=="max") ? V[id,"max_ms"] : V[id,"p95_ms"]
        if(mv+0 > budget+0){ if(enforce+0==1){ status="BROKEN"; lab="**BUDGET BROKEN**" } else { status="OVER"; lab="over budget (target not met yet)" } overfail++ }
        else { status="PASS"; lab="pass"; within++ }
      } else { status="TRACKED"; lab="tracked"; tracked++ }
    } else {
      if(id in FAIL){ status="MISSING"; lab="**missing**: " FAIL[id]; overfail++ }
      else { status="NOTRUN"; lab="not run"; notrun++ }
    }
    printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", id,title,tgt, fnum(V[id,"p50_ms"]),fnum(V[id,"p95_ms"]),fnum(V[id,"max_ms"]), fbytes(V[id,"peak_rss_bytes"]),fbytes(V[id,"gpu_bytes"]), lab, ilink(issue) > ROWF
    id=""
  }
  BEGIN{ ROWF=TMP "/perf_rows.txt"; NMF=TMP "/perf_nm.txt"; printf "" > ROWF; printf "" > NMF }
  FNR==NR{
    if($1=="S"){ V[$2,$3]=$4 }
    else if($1=="X"){ v=$0; sub(/^X [^ ]+ /,"",v); FAIL[$2]=v }
    next
  }
  /^\[\[scenario\]\]/{ flush(); id=""; title=""; issue=""; metric="p95"; budget=""; target=""; enforce=1; notm=""; inscn=1; next }
  /^\[\[bench\]\]/ || /^\[settings\]/ || /^\[\[/ { flush(); inscn=0; next }
  !inscn { next }
  /^id *=/      { v=$0; sub(/^id *= *"/,"",v); sub(/".*/,"",v); id=v; next }
  /^title *=/   { v=$0; sub(/^title *= *"/,"",v); sub(/".*/,"",v); title=v; next }
  /^issue *=/   { v=$0; sub(/^issue *= */,"",v); sub(/[^0-9].*/,"",v); issue=v; next }
  /^metric *=/  { v=$0; sub(/^metric *= *"/,"",v); sub(/".*/,"",v); metric=v; next }
  /^budget_ms *=/ { v=$0; sub(/^budget_ms *= */,"",v); sub(/ *#.*/,"",v); gsub(/[ \t]/,"",v); budget=v; next }
  /^target *=/  { v=$0; sub(/^target *= *"/,"",v); sub(/".*/,"",v); target=v; next }
  /^enforce *=/ { enforce = ($0 ~ /true/) ? 1 : 0; next }
  /^not_measurable *=/ { v=$0; sub(/^not_measurable *= *"/,"",v); sub(/".*/,"",v); notm=v; next }
  END{ flush(); printf "%d %d %d %d %d\n", within+0, overfail+0, tracked+0, nm+0, notrun+0 > (TMP "/perf_counts.txt") }
' "$TMP/base.txt" "$BUDGETS"

read PERF_WITHIN PERF_OVER PERF_TRACKED PERF_NM PERF_NOTRUN < "$TMP/perf_counts.txt"
PERF_TOTAL=$(grep -c '^\[\[scenario\]\]' "$BUDGETS")
PERF_MISSING_COL=$((PERF_NM + PERF_NOTRUN))

# Baseline machine fields.
bval() { awk -v k="$1" '$1=="M"&&$2==k{v=$0; sub(/^M [^ ]+ /,"",v); print v; exit}' "$TMP/base.txt"; }
fval() { awk -v k="$1" '$1=="F"&&$2==k{print $3; exit}' "$TMP/base.txt"; }
BS_DATE=$(fval date); BS_DATE10=$(printf '%s' "$BS_DATE" | cut -c1-10)
BS_COMMIT=$(fval commit); BS_COMMIT10=$(printf '%s' "$BS_COMMIT" | cut -c1-10)
BS_CPU=$(bval cpu); BS_GPU=$(bval gpu_adapter); BS_OSVER=$(bval os_version)
BS_RAM=$(bval ram_bytes); BS_RAMGB=$(awk -v b="$BS_RAM" 'BEGIN{ printf "%d", (b!=""? int(b/1073741824):0) }')
BS_L1=$(awk '$1=="L"&&$2==1{printf "%.2f",$3}' "$TMP/base.txt")
BS_L2=$(awk '$1=="L"&&$2==2{printf "%.2f",$3}' "$TMP/base.txt")
BS_L3=$(awk '$1=="L"&&$2==3{printf "%.2f",$3}' "$TMP/base.txt")

# ---- 3. corpus floors (C test) --------------------------------------------------------------
awk '
  /[0-9]+ files/ { c=$0; sub(/ files.*/,"",c); sub(/.*[^0-9]/,"",c); files=c }
  /^static const source [A-Z_]+ *= *\{/ {
    s=$0; sub(/.*= *\{/,"",s); sub(/\}.*/,"",s)
    nf=split(s, a, ",")
    lab=a[1]; gsub(/^[ "]+|[ "]+$/,"",lab)
    dir=a[2]; gsub(/^[ "]+|[ "]+$/,"",dir)
    pass=a[4]; gsub(/[^0-9]/,"",pass)
    rt=a[5]; gsub(/[^0-9]/,"",rt)
    print lab "\t" dir "\t" files "\t" pass "\t" rt
    files=""
  }
' "$CORPUS_C" > "$TMP/corpus.txt"

# Summary key for File: "label: oracle >= pass/files, round trip >= rt/files; ..."
CORPUS_KEY=$(awk -F'\t' '{
  of=($3!="")?("/" $3):""
  s=$1 ": oracle \342\211\245 " $4 of ", round trip \342\211\245 " $5 of
  printf "%s%s", (NR>1?"; ":""), s
}' "$TMP/corpus.txt")

# ---- 4. prefs audit (C-native path-literal heuristic) ---------------------------------------
awk '
  FNR==NR {
    if ($0 ~ /\{"[a-zA-Z]+", section_[a-z_]+\}/) {
      id=$0; sub(/^[^"]*"/,"",id); sub(/".*/,"",id)
      fn=$0; sub(/.*section_/,"section_",fn); sub(/[^a-zA-Z_].*/,"",fn); map[fn]=id
    }
    next
  }
  /^static void section_[a-z_]+\(/ { cur=$0; sub(/^static void /,"",cur); sub(/\(.*/,"",cur); sec=map[cur]; next }
  /^static / { sec="" }
  sec!="" && /yyjson_mut_obj_add_(str|bool|uint|int|sint|real|double)\(d, o, "/ {
    f=$0; sub(/.*\(d, o, "/,"",f); sub(/".*/,"",f); print sec "." f
  }
' "$PREFS_C" "$PREFS_C" > "$TMP/prefpaths.txt"
PREFS_TOTAL=$(wc -l < "$TMP/prefpaths.txt" | tr -d ' ')
: > "$TMP/prefs_unread.txt"
while IFS= read -r p; do
    grep -rqF "\"$p\"" src/ 2>/dev/null || printf '%s\n' "$p" >> "$TMP/prefs_unread.txt"
done < "$TMP/prefpaths.txt"
PREFS_UNREAD=$(wc -l < "$TMP/prefs_unread.txt" | tr -d ' ')

# ---- 5. never-crash + parity line -----------------------------------------------------------
MODULES=$(find src -maxdepth 1 -mindepth 1 -type d | wc -l | tr -d ' ')
if [ -f tests/engine/test_panic_hunt.c ]; then PANIC_HUNT=present; else PANIC_HUNT=missing; fi
if [ -f "$PARITY_DOC" ]; then
    PARITY_LINE=$(grep -m1 'menu items live' "$PARITY_DOC" | sed 's/^\**//; s/\**$//; s/\.$//' | sed 's/^ *//; s/ *$//')
else
    PARITY_LINE=""
fi

# ---- 6. checklists in (order, area) order ---------------------------------------------------
: > "$TMP/order.txt"
for f in scorecard/*.toml; do
    awk -v F="$f" -F'"' '
      /^area/{a=$2} /^title/{t=$2} /^order/{o=$0; gsub(/[^0-9]/,"",o)}
      END{ printf "%s\t%s\t%s\t%s\n", (o==""?99:o), a, t, F }' "$f" >> "$TMP/order.txt"
done
sort -n "$TMP/order.txt" > "$TMP/order_sorted.txt"

# Emit per-checklist: counts (done partial missing) and a rendered table, into $TMP/<area>.*
render_checklist() {
    # $1 = toml file -> writes $TMP/cl_rows (table) and echoes "done partial missing"
    awk -v OUTF="$TMP/cl_rows" '
      function ilink(n){ if(n=="")return ""; return "[#" n "](https://github.com/storytold/photocraft/issues/" n ")" }
      function flush(){
        if(id==""){return}
        if(status=="done")d++; else if(status=="partial")p++; else if(status=="missing")m++
        printf "| %s | %s | %s | %s | %s |\n", id,target,status,ilink(issue),note > ROWF
        id=""
      }
      BEGIN{ ROWF=OUTF; printf "" > ROWF }
      /^\[\[item\]\]/{ flush(); id=""; target=""; status=""; issue=""; note=""; next }
      /^id *=/     { v=$0; sub(/^id *= *"/,"",v); sub(/".*/,"",v); id=v; next }
      /^target *=/ { v=$0; sub(/^target *= *"/,"",v); sub(/".*/,"",v); target=v; next }
      /^status *=/ { v=$0; sub(/^status *= *"/,"",v); sub(/".*/,"",v); status=v; next }
      /^issue *=/  { v=$0; sub(/^issue *= */,"",v); sub(/[^0-9].*/,"",v); issue=v; next }
      /^note *=/   { v=$0; sub(/^note *= *"/,"",v); sub(/"[ ]*$/,"",v); note=v; next }
      END{ flush(); printf "%d %d %d\n", d+0,p+0,m+0 }
    ' "$1"
}

# ---- 7. assemble -----------------------------------------------------------------------------
DOC="$TMP/scorecard.md"
{
printf '# PhotoCraft scorecard\n\n'
printf 'Generated by `tools/scorecard.sh` from `perf/budgets.toml`, `perf/baseline.json`, `tests/io/test_io_corpus.c`, `scorecard/*.toml` and the source tree. Do not edit by hand: change the sources and regenerate. Checklist statuses are verified against the code; numbers here supersede estimates elsewhere.\n\n'
printf '## Summary\n\n'
printf '| Area | Done | Partial | Missing | Key numbers |\n|---|---:|---:|---:|---|\n'
PERF_KEY="$PERF_TOTAL scenarios: $PERF_WITHIN within budget, $PERF_OVER over budget or failing, $PERF_TRACKED tracked only, $PERF_NOTRUN not run, $PERF_NM not measurable yet (baseline $BS_DATE10)"
printf '| [Performance](#performance) | %s | %s | %s | %s |\n' "$PERF_WITHIN" "$PERF_OVER" "$PERF_MISSING_COL" "$PERF_KEY"

# summary rows for each checklist in order
while IFS="$(printf '\t')" read -r order area title file; do
    counts=$(render_checklist "$file")
    set -- $counts; d=$1; p=$2; m=$3
    anchor=$(printf '%s' "$title" | tr 'A-Z' 'a-z' | sed 's/ /-/g; s/[^a-z0-9-]//g')
    case $area in
        file) key=$CORPUS_KEY ;;
        ui) key="settings that do nothing: $PREFS_UNREAD of $PREFS_TOTAL (target 0)" ;;
        automation) key=$PARITY_LINE ;;
        reliability) key="never-crash gate across $MODULES modules; panic_hunt $PANIC_HUNT" ;;
        *) key="" ;;
    esac
    printf '| [%s](#%s) | %s | %s | %s | %s |\n' "$title" "$anchor" "$d" "$p" "$m" "$key"
done < "$TMP/order_sorted.txt"

# Performance section
printf '\n## Performance\n\n'
printf 'Budgets: `perf/budgets.toml` (targets from #209, #210, #211). Numbers: the committed baseline (`perf/baseline.json`, full run), updated only by `tools/perf.sh --update-baseline`. The nightly perf job runs `tools/perf.sh` on a fixed runner and fails on a broken enforced budget or a p50 regression over %s %% against a baseline from the same machine class. "Over budget" rows are targets the code does not meet yet: they are reported, and only their regressions fail.\n\n' "$REG_PCT"
if [ -n "$BS_DATE10" ]; then
    printf 'Baseline: %s, commit `%s`, %s (%s GB), GPU %s, %s; load average %s / %s / %s (1/5/15 min) at the start of the run.\n\n' \
        "$BS_DATE10" "$BS_COMMIT10" "$BS_CPU" "$BS_RAMGB" "$BS_GPU" "$BS_OSVER" "$BS_L1" "$BS_L2" "$BS_L3"
else
    printf 'Baseline: **not run** (no `perf/baseline.json` full run yet).\n\n'
fi
printf '| Id | Scenario | Target | p50 ms | p95 ms | max ms | Peak RSS | GPU | Status | Issue |\n'
printf '|---|---|---|---:|---:|---:|---:|---:|---|---|\n'
cat "$TMP/perf_rows.txt"
if [ -s "$TMP/perf_nm.txt" ]; then
    printf '\n**Not measurable yet** (the code can'\''t do it yet; no number is recorded):\n\n'
    printf '| Id | Scenario | Target | Why | Issue |\n|---|---|---|---|---|\n'
    awk -F'\t' '{ printf "| %s | %s | %s | %s | %s |\n", $2,$3,$4,$5,$6 }' "$TMP/perf_nm.txt"
fi

# per-checklist sections
while IFS="$(printf '\t')" read -r order area title file; do
    counts=$(render_checklist "$file")
    printf '\n## %s\n\n' "$title"
    case $area in
    file)
        printf 'Corpus floors (`tests/io/test_io_corpus.c`): the enforced minimums; the corpus tests (`tools/corpus.sh` fetches the corpora, the io corpus tests run them in CI) fail below them.\n\n'
        printf '| Corpus | Directory | Files | Oracle pass floor | Round-trip floor |\n|---|---|---:|---:|---:|\n'
        awk -F'\t' '{ f=($3!=""?$3:"\342\200\223"); pf=($4!=""?$4:"\342\200\223"); rf=($5!=""?$5:"\342\200\223"); printf "| %s | `%s` | %s | %s | %s |\n", $1,$2,f,pf,rf }' "$TMP/corpus.txt"
        printf '\n'
        ;;
    ui)
        printf '**Settings that do nothing: %s of %s** (target 0, #204). Counted by the prefs audit (`tools/scorecard.sh`): a setting (a leaf of the default preferences tree in `src/engine/prefs.c`) counts as used when its `section.field` path appears as a string literal anywhere in `src/`. It is a heuristic lower bound.\n\n' "$PREFS_UNREAD" "$PREFS_TOTAL"
        if [ -s "$TMP/prefs_unread.txt" ]; then
            unread=$(awk '{ printf "%s`%s`", (NR>1?", ":""), $0 } END{print ""}' "$TMP/prefs_unread.txt")
            printf 'Unread: %s.\n\n' "$unread"
        fi
        ;;
    automation)
        [ -n "$PARITY_LINE" ] && printf 'Menu wiring (`docs/parity-checklist.md`): %s. This counts dispatch, not behaviour.\n\n' "$PARITY_LINE"
        ;;
    reliability)
        printf 'Never-crash gate: strict `-Werror` build with no `unwrap`/`panic` equivalents across %s modules; panic_hunt %s.\n\n' "$MODULES" "$PANIC_HUNT"
        ;;
    esac
    printf '| Id | Target | Status | Issue | Note |\n|---|---|---|---|---|\n'
    cat "$TMP/cl_rows"
done < "$TMP/order_sorted.txt"
} > "$DOC"

if [ "$check" -eq 1 ]; then
    if cmp -s "$DOC" "$OUT"; then
        printf 'scorecard: %s is up to date\n' "$OUT"
        exit 0
    fi
    printf 'scorecard: %s is stale; run tools/scorecard.sh\n' "$OUT" >&2
    exit 1
fi

cp "$DOC" "$OUT"
printf 'scorecard: wrote %s (settings that do nothing %s of %s; perf %s within / %s over / %s tracked)\n' \
    "$OUT" "$PREFS_UNREAD" "$PREFS_TOTAL" "$PERF_WITHIN" "$PERF_OVER" "$PERF_TRACKED"
