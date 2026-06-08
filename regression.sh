#!/usr/bin/env bash
# GLZA roundtrip regression — aggregates harnesses used during max_rules debugging.
# See DEBUG_PLAN.md for techniques, lldb notes, and interpretation.
#
# Usage:
#   ./regression.sh              # quick gate (~minutes on enwik10m)
#   ./regression.sh --full       # more max_rules points + NULL-params test
#   ./regression.sh --asan       # ASAN decode-only (needs --save-blobs or existing blobs)
#   ./regression.sh --save-blobs # write /tmp/glza6325.bin and /tmp/glza6330.bin
#   ./regression.sh --build-only
#
# Environment:
#   GLZA_CLANG   compiler (default /usr/bin/clang)
#   GLZA_CORPUS  input file (default ../enwik10m)
#   GLZA_TMP     temp dir for blobs (default /tmp)
#   GLZA_RAM_MB  optional fixed RAM for rt_author (default: heuristic, not 16GB)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CLANG="${GLZA_CLANG:-/usr/bin/clang}"
TMPDIR="${GLZA_TMP:-/tmp}"
CFLAGS="-O3 -DPRINTON -D_FILE_OFFSET_BITS=64 -I."
ASAN_CFLAGS="-O1 -g -fsanitize=address -D_FILE_OFFSET_BITS=64 -I."

MODE_QUICK=1
MODE_FULL=0
MODE_ASAN=0
SAVE_BLOBS=0
BUILD_ONLY=0

for arg in "$@"; do
  case "$arg" in
    --full) MODE_QUICK=0; MODE_FULL=1 ;;
    --asan) MODE_ASAN=1 ;;
    --save-blobs) SAVE_BLOBS=1 ;;
    --build-only) BUILD_ONLY=1 ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

OBJS="GLZAcomp.o GLZAformat.o GLZAcompress.o GLZAencode.o GLZAdecode.o GLZAmodel.o GLZAparams.o GLZAfail.o"
SOURCES="GLZAcomp.c GLZAformat.c GLZAcompress.c GLZAencode.c GLZAdecode.c GLZAmodel.c GLZAparams.c GLZAfail.c"

log() { printf '==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

for CORPUS in enwik1m enwik10m enwik100m; do
  if [[ ! -f "$CORPUS" ]]; then
    die "Corpus not found: $CORPUS (set GLZA_CORPUS)"
  fi
done

build_release() {
  log "Release build ($CLANG -O3)"
  # shellcheck disable=SC2086
  $CLANG $CFLAGS -c $SOURCES
}

link_harness() {
  local out=$1
  shift
  # shellcheck disable=SC2086
  $CLANG $CFLAGS -o "$out" "./scripts/test/$@" $OBJS -lm -pthread
}

build_harnesses() {
  build_release
  link_harness rt_large rt_large.c
  link_harness rt_test rt_test.c
  link_harness rt_save rt_save.c
  link_harness dec_only6500 dec_only6500.c
  link_harness dec_only_mr dec_only_mr.c
  link_harness rt_same6500 rt_same6500.c
  link_harness rt_fast0 rt_fast0.c
  link_harness rt_slow rt_slow.c
  link_harness rt_author rt_author.c
  link_harness rt_author10m rt_author10m.c
  log "Built: rt_large rt_test rt_save dec_only_mr dec_only6500 rt_same6500 rt_slow rt_author"
}

# run_case name cmd expected_rc
# expected_rc: 0 = must pass; !0 = must be non-zero (known failure)
run_case() {
  local name=$1
  local cmd=$2
  local expect=$3
  local rc=0
  local logf
  logf=$(mktemp "${TMPDIR}/glza_regress_XXXX.log")
  printf '  %-40s ' "$name"
  if eval "$cmd" >"$logf" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  if [[ "$expect" == "0" ]]; then
    if [[ $rc -eq 0 ]]; then
      echo "PASS"
    else
      echo "FAIL (exit $rc, expected 0)"
      tail -8 "$logf" | sed 's/^/    /'
      FAILURES=$((FAILURES + 1))
    fi
  else
    if [[ $rc -ne 0 ]]; then
      echo "PASS (expected fail, exit $rc)"
    else
      echo "FAIL (expected non-zero, got 0)"
      tail -5 "$logf" | sed 's/^/    /'
      FAILURES=$((FAILURES + 1))
    fi
  fi
  rm -f "$logf"
}

# macOS 26.4+ changed dyld_shared_cache_iterate_text_swift to allocate during
# ASAN init, re-entering the runtime and deadlocking (StaticSpinMutex spin).
# See DEBUG_PLAN.md §3 and LLVM PR #182943 (_dyld_get_dyld_header).
asan_runtime_ok() {
  local probe probe_c pid waited rc
  probe=$(mktemp "${TMPDIR}/glza_asan_probe_XXXX")
  probe_c="${probe}.c"
  cat >"$probe_c" <<'EOF'
int main(void) { return 0; }
EOF
  if ! $CLANG -fsanitize=address -o "$probe" "$probe_c" 2>/dev/null; then
    rm -f "$probe" "$probe_c"
    return 1
  fi
  env ASAN_OPTIONS=detect_leaks=0 "$probe" >/dev/null 2>&1 &
  pid=$!
  waited=0
  while kill -0 "$pid" 2>/dev/null && [[ $waited -lt 3 ]]; do
    sleep 1
    waited=$((waited + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$probe" "$probe_c"
    return 1
  fi
  wait "$pid"
  rc=$?
  rm -f "$probe" "$probe_c"
  [[ $rc -eq 0 ]]
}

build_asan_decode() {
  if ! asan_runtime_ok; then
    log "SKIP ASAN: runtime hangs at init on this macOS (dyld/ASAN reentrancy; need LLVM with _dyld_get_dyld_header fix)"
    return 1
  fi
  log "ASAN decode build"
  # shellcheck disable=SC2086
  $CLANG $ASAN_CFLAGS -c GLZAdecode.c GLZAmodel.c GLZAparams.c
  # shellcheck disable=SC2086
  $CLANG $ASAN_CFLAGS -o dec_asan6400 ./scripts/test/dec_only6500.c GLZAdecode.o GLZAmodel.o GLZAparams.o -lm -pthread
}

FAILURES=0

log "GLZA regression (corpora: enwik1m enwik10m enwik100m)"
build_harnesses

if [[ $BUILD_ONLY -eq 1 ]]; then
  log "Build-only done."
  exit 0
fi

log "Quick roundtrip matrix (fast_mode=1 via rt_large)"
run_case "max_rules=5000 1m" "./rt_large 5000 enwik1m" 0
run_case "max_rules=6000 1m" "./rt_large 6000 enwik1m" 0
run_case "max_rules=6325 1m" "./rt_large 6325 enwik1m" 0

run_case "max_rules=5000 10m" "./rt_large 5000 enwik10m" 0
run_case "max_rules=6000 10m" "./rt_large 6000 enwik10m" 0
run_case "max_rules=6325 10m" "./rt_large 6325 enwik10m" 0

run_case "max_rules=6330 1m" "./rt_large 6330 enwik1m" 0
run_case "max_rules=6330 10m" "./rt_large 6330 enwik10m" 0

log "fast_mode=0 roundtrip on enwik1m (rt_slow)"
run_case "slow max_rules=500 1m" "./rt_slow 500 enwik1m" 0
run_case "slow max_rules=5000 1m" "./rt_slow 5000 enwik1m" 0
run_case "slow max_rules=6325 1m" "./rt_slow 6325 enwik1m" 0

log "Author-like fast_mode=0 on enwik1m (rt_author; default RAM heuristic, not 16GB)"
run_case "author-like 1m" "./rt_author enwik1m" 0

if [[ $MODE_FULL -eq 1 ]]; then
  log "Running 100m and other full test"

  run_case "max_rules=5000 100m" "./rt_large 5000 enwik100m" 0
  run_case "max_rules=6000 100m" "./rt_large 6000 enwik100m" 0
  run_case "max_rules=6325 100m" "./rt_large 6325 enwik100m" 0
  run_case "max_rules=6330 100m" "./rt_large 6330 enwik100m" 0

  run_case "max_rules=6300 1m" "./rt_large 6300 enwik1m" 0
  run_case "max_rules=6310 1m" "./rt_large 6310 enwik1m" 0
  run_case "max_rules=6320 1m" "./rt_large 6320 enwik1m" 0
  run_case "max_rules=6300 10m" "./rt_large 6300 enwik10m" 0
  run_case "max_rules=6310 10m" "./rt_large 6310 enwik10m" 0
  run_case "max_rules=6320 10m" "./rt_large 6320 enwik10m" 0
  run_case "max_rules=6300 100m" "./rt_large 6300 enwik100m" 0
  run_case "max_rules=6310 100m" "./rt_large 6310 enwik100m" 0
  run_case "max_rules=6320 100m" "./rt_large 6320 enwik100m" 0

  run_case "max_rules=6340 1m" "./rt_large 6340 enwik1m" 0
  run_case "max_rules=6400 1m" "./rt_large 6400 enwik1m" 0
  run_case "max_rules=6340 10m (known bad)" "./rt_large 6340 enwik10m" fail
  run_case "max_rules=6400 10m (known bad)" "./rt_large 6400 enwik10m" fail
  run_case "max_rules=6340 100m" "./rt_large 6340 enwik100m" 0
  run_case "max_rules=6400 100m" "./rt_large 6400 enwik100m" 0

  log "NULL vs explicit params (rt_test, 1 compress rep)"
  run_case "rt_test NULL+CLI 1m" "./rt_test enwik1m 1" 0
  run_case "rt_test NULL+CLI 10m" "./rt_test enwik10m 1" 0
  run_case "rt_test NULL+CLI 100m" "./rt_test enwik100m 1" 0

  log "fast_mode=0 roundtrip (rt_slow; 10m/100m are slow)"
  run_case "slow max_rules=500 10m" "./rt_slow 500 enwik10m" fail
  run_case "slow max_rules=5000 10m" "./rt_slow 5000 enwik10m" fail
  run_case "slow max_rules=5000 100m" "./rt_slow 5000 enwik100m" fail
fi

if [[ $SAVE_BLOBS -eq 1 ]] || [[ $MODE_ASAN -eq 1 ]]; then
  log "Saving compressed blobs for split decode tests"
  ./rt_save 6325 "${TMPDIR}/glza6325.bin"
  ./rt_save 6330 "${TMPDIR}/glza6330.bin"
  run_case "decode-only 6325" "./dec_only_mr '${TMPDIR}/glza6325.bin' 6325" 0
  run_case "decode-only 6330 (known bad)" "./dec_only_mr '${TMPDIR}/glza6330.bin' 6330" fail
fi

if [[ $MODE_ASAN -eq 1 ]]; then
  if [[ ! -f "${TMPDIR}/glza6330.bin" ]]; then
    log "ASAN: creating ${TMPDIR}/glza6330.bin"
    ./rt_save 6330 "${TMPDIR}/glza6330.bin"
  fi
  if build_asan_decode; then
    log "ASAN decode-only on 6330 blob (halt on first error)"
    ASAN_OPTIONS=halt_on_error=1:detect_leaks=0 \
      run_case "asan dec 6330" "./dec_asan6400 '${TMPDIR}/glza6330.bin'" fail
  fi
fi

if [[ $MODE_FULL -eq 1 ]] && [[ -x ./rt_fast0 ]]; then
  log "fast_mode=0 author profile on enwik10m (rt_fast0)"
  run_case "fast_mode=0 full max_rules 10m" "./rt_fast0" fail
fi
 
if [[ $MODE_FULL -eq 1 ]] && [[ -x ./rt_same6500 ]]; then
  log "Same-process 6500 (historical check)"
  run_case "rt_same6500" "./rt_same6500" fail
fi

echo ""
if [[ $FAILURES -eq 0 ]]; then
  log "All regression cases matched expectations."
  exit 0
else
  log "$FAILURES case(s) did not match expectations."
  exit 1
fi
