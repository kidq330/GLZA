#!/usr/bin/env bash
# GLZA C++ port — overnight AddressSanitizer stress matrix.
#
# Purpose: exercise the heap-fallback / heap-mode substitution path that
# HEAP_FALLBACK_ISSUE.md describes, under ASan, across many input sizes and
# parameter combinations, so a crash/overflow/round-trip regression in ANY
# cell is caught and logged without losing the rest of the sweep.
#
# What it catches (verified to fire on the current tree):
#   * the documented score_map_ heap-buffer-overflow in
#     rank_scores_thread_fast_impl (glza_compress.cpp:1112) — the canary that
#     the grammar GREW because heap-mode substitution replaced nothing
#     (triggered by large -D on >=~200 KB text);
#   * any other ASan WRITE/READ in the heap-fallback match tree;
#   * round-trip mismatches on the cells that DO complete.
#
# Each case is independent and bounded by `timeout`; a crash/hang in one cell
# does not stop the sweep. Full ASan reports go to per-case logs; a TSV +
# human summary are written at the end.
#
# USAGE (must be inside the dev shell for the toolchain at build time):
#   nix develop --command bash glza/asan_overnight.sh           # full matrix
#   nix develop --command bash glza/asan_overnight.sh --quick   # cheap cells only
#   nix develop --command bash glza/asan_overnight.sh --rebuild # force ASan rebuild
# Env knobs:
#   GLZA_ASAN_TIMEOUT=600   per-case wall-clock cap, seconds (default 600)
#   GLZA_ASAN_CORPUS=enwik1m   source text to slice inputs from (default enwik1m)
#   GLZA_ASAN_OUT=asan_logs    output dir (default asan_logs)
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build-dbg"
BIN="$BUILD_DIR/glza"
CORPUS="${GLZA_ASAN_CORPUS:-enwik1m}"
TIMEOUT="${GLZA_ASAN_TIMEOUT:-600}"
OUTROOT="${GLZA_ASAN_OUT:-asan_logs}"
QUICK=0
REBUILD=0

for arg in "$@"; do
  case "$arg" in
    --quick)   QUICK=1 ;;
    --rebuild) REBUILD=1 ;;
    -h|--help)
      sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

log()  { printf '==> %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v cmake  >/dev/null 2>&1 || die "cmake not found — run inside 'nix develop'."
command -v clang++ >/dev/null 2>&1 || die "clang++ not found — run inside 'nix develop'."
[ -f "$CORPUS" ] || die "corpus not found: $CORPUS (set GLZA_ASAN_CORPUS)"

# ---- build the ASan CLI if needed ------------------------------------------
if [ "$REBUILD" = 1 ] || [ ! -x "$BIN" ]; then
  log "Configuring + building ASan glza ($BUILD_DIR)"
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DGLZA_ASAN=ON -DGLZA_PRINTON=ON >/dev/null 2>&1 \
    || die "cmake configure failed"
  cmake --build "$BUILD_DIR" --target glza \
        -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null 2>&1 \
    || die "build failed"
fi
[ -x "$BIN" ] || die "ASan binary missing after build: $BIN"

# ---- output dir (timestamped so successive nights don't clobber) -----------
# new Date() is unavailable in workflow scripts but this is plain bash:
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="$OUTROOT/$STAMP"
mkdir -p "$OUT/inputs"
TSV="$OUT/results.tsv"
SUMMARY="$OUT/summary.txt"
printf 'status\tcase\tcsize\tbpb\tseconds\tlog\n' >"$TSV"

# ASan: abort on first error (exit 134) and don't spend time on leak reports —
# we want crash/overflow signal, not allocation accounting.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:handle_abort=1:print_stats=0"

# ---- input slices (sliced once from the corpus) ----------------------------
# Map a size token to a file under $OUT/inputs.
mkinput() {  # $1=token (e.g. 2k, 200k, 1m, rep64k)
  local tok="$1" f="$OUT/inputs/$1.bin" n
  [ -f "$f" ] && { echo "$f"; return; }
  case "$tok" in
    rep*)  # highly repetitive synthetic text (different match-tree shape)
      n="${tok#rep}"
      yes "the quick brown fox jumps over the lazy dog 0123456789" \
        | head -c "$(numfmt --from=iec "${n^^}" 2>/dev/null || echo "$n")" >"$f" ;;
    *)
      n="$(numfmt --from=iec "${tok^^}" 2>/dev/null || echo "$tok")"
      head -c "$n" "$CORPUS" >"$f" ;;
  esac
  echo "$f"
}

# ---- case matrix -----------------------------------------------------------
# Each CASE line: "<label>|<size-token>|<extra compress flags>"
# The harness always round-trips a successful compress.
CASES=()

# Group A — heap fallback via large RAM on small inputs (stable degraded path;
# hunts latent OOB in the heap fallback itself). -r forces the arena tiny
# relative to demand, so the heap fallback is taken many times per cycle.
for sz in 1k 2k 4k 8k 16k 64k; do
  CASES+=("A_ram2000_$sz|$sz|-r2000")
  CASES+=("A_ram8000_$sz|$sz|-r8000")
done

# Group B — grammar-growth / score_map_ overflow via large max_rules on text.
# This is the cell class that currently ABORTS under ASan (the documented bug).
for sz in 64k 200k 500k 1m; do
  CASES+=("B_rules50k_$sz|$sz|-D50000")
  CASES+=("B_rules200k_$sz|$sz|-D200000")
  CASES+=("B_rules1m_$sz|$sz|-D1000000")
done

# Group C — slow mode (-x), the original turn-1 path, default + large RAM.
for sz in 16k 64k 200k 1m; do
  CASES+=("C_slow_$sz|$sz|-x")
  CASES+=("C_slow_ram4000_$sz|$sz|-x -r4000")
done

# Group D — default stable config across sizes (regression guard; should all
# round-trip OK — any ASan/mismatch here is a real regression).
for sz in 1k 8k 64k 200k 500k 1m; do
  CASES+=("D_default_$sz|$sz|")
done

# Group E — repetitive synthetic inputs (stresses a different match-tree shape).
for sz in rep8k rep64k rep200k; do
  CASES+=("E_rep_$sz|$sz|-r2000")
  CASES+=("E_rep_slow_$sz|$sz|-x")
done

# In --quick mode keep only small/cheap cells for a fast smoke pass. Match on
# the size token (field 2 of the entry), not the whole "label|size|flags".
if [ "$QUICK" = 1 ]; then
  QC=()
  for c in "${CASES[@]}"; do
    IFS='|' read -r _ qsz _ <<<"$c"
    case "$qsz" in
      1m|500k|200k|rep200k) : ;;                 # drop the heavy cells
      *) QC+=("$c") ;;
    esac
  done
  CASES=("${QC[@]}")
fi

# ---- run -------------------------------------------------------------------
total=${#CASES[@]}
log "ASan overnight matrix: $total cases, ${TIMEOUT}s cap each, corpus=$CORPUS"
log "Logs: $OUT"
echo "start: $(date)"
printf '%-26s %-12s %s\n' CASE STATUS DETAIL

idx=0
declare -A COUNT=()
for entry in "${CASES[@]}"; do
  idx=$((idx + 1))
  IFS='|' read -r label sztok flags <<<"$entry"
  inp="$(mkinput "$sztok")"
  clog="$OUT/${label}.clog"
  dlog="$OUT/${label}.dlog"
  cout="$OUT/${label}.glza"
  dout="$OUT/${label}.rt"

  # compress
  t0=$SECONDS
  # The inner redirect captures the child's output; the outer 2>/dev/null
  # swallows the shell's own "Abort trap: 6" job message on ASan SIGABRT
  # (cosmetic — the full report is already in $clog).
  # shellcheck disable=SC2086
  { timeout "$TIMEOUT" "$BIN" c $flags "$inp" "$cout" >"$clog" 2>&1; } 2>/dev/null
  rc=$?
  dt=$((SECONDS - t0))

  status="" detail=""
  csize="" bpb=""
  if grep -q "AddressSanitizer" "$clog"; then
    status="ASAN"
    detail="$(grep -m1 -oE 'AddressSanitizer: [a-z-]+' "$clog" | head -1); $(grep -m1 -oE 'glza_[a-z_]+\.cpp:[0-9]+' "$clog" | head -1)"
  elif [ "$rc" = 124 ]; then
    status="TIMEOUT"; detail="${dt}s cap"
  elif [ "$rc" != 0 ]; then
    status="CRASH"; detail="exit $rc"
  else
    # compress ok — capture size + round-trip under ASan
    csize=$(stat -f%z "$cout" 2>/dev/null || stat -c%s "$cout")
    bpb=$(grep -oE '[0-9.]+ bpB' "$clog" | tail -1 | awk '{print $1}')
    { timeout "$TIMEOUT" "$BIN" d "$cout" "$dout" >"$dlog" 2>&1; } 2>/dev/null
    drc=$?
    if grep -q "AddressSanitizer" "$dlog"; then
      status="ASAN_DEC"
      detail="$(grep -m1 -oE 'AddressSanitizer: [a-z-]+' "$dlog"); $(grep -m1 -oE 'glza_[a-z_]+\.cpp:[0-9]+' "$dlog")"
    elif [ "$drc" = 124 ]; then
      status="TIMEOUT_DEC"; detail="decompress ${TIMEOUT}s cap"
    elif [ "$drc" != 0 ]; then
      status="CRASH_DEC"; detail="decompress exit $drc"
    elif cmp -s "$inp" "$dout"; then
      status="OK"; detail="rt ok, ${csize}B, ${bpb} bpB"
    else
      status="MISMATCH"; detail="round-trip differs ($(stat -f%z "$dout" 2>/dev/null || stat -c%s "$dout")B vs $(stat -f%z "$inp" 2>/dev/null || stat -c%s "$inp")B)"
    fi
    # keep disk bounded — drop the round-trip payload, keep the compressed blob small only on failure
    rm -f "$dout"
    [ "$status" = OK ] && rm -f "$cout"
  fi

  COUNT[$status]=$(( ${COUNT[$status]:-0} + 1 ))
  printf '%-26s %-12s %s\n' "$label" "$status" "$detail"
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$status" "$label" "${csize:-}" "${bpb:-}" "$dt" "${label}.clog" >>"$TSV"
done

echo "end: $(date)"

# ---- summary ---------------------------------------------------------------
{
  echo "GLZA ASan overnight — $STAMP — corpus=$CORPUS, cap=${TIMEOUT}s, cases=$total"
  echo
  echo "Status tally:"
  for k in "${!COUNT[@]}"; do printf '  %-12s %d\n' "$k" "${COUNT[$k]}"; done | sort
  echo
  echo "Non-OK cases (open these .clog/.dlog for the ASan stack):"
  awk -F'\t' 'NR>1 && $1!="OK" {printf "  %-12s %-26s %s\n", $1, $2, $6}' "$TSV"
  echo
  echo "Logs dir: $OUT"
  echo "Reproduce a cell directly:"
  echo "  ASAN_OPTIONS=$ASAN_OPTIONS \\"
  echo "    $BIN c <flags> <input> /tmp/out.glza"
} | tee "$SUMMARY"

# Non-zero exit if any cell failed, so a CI/`&&` chain notices.
fail=0
for k in "${!COUNT[@]}"; do
  case "$k" in OK) : ;; *) fail=$(( fail + COUNT[$k] )) ;; esac
done
[ "$fail" = 0 ] && log "All cells OK." || log "$fail non-OK cell(s) — see $SUMMARY"
exit $(( fail > 0 ? 1 : 0 ))
