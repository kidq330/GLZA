#!/usr/bin/env bash
# GLZA C++23 roundtrip regression via CMake build.
# Mirrors regression.sh test matrix but uses CMake-built binaries.
#
# Usage:
#   ./regression_cmake.sh              # quick gate
#   ./regression_cmake.sh --full       # extended tests
#   ./regression_cmake.sh --build-only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build"
TMPDIR="${GLZA_TMP:-/tmp}"

MODE_FULL=0
BUILD_ONLY=0

for arg in "$@"; do
  case "$arg" in
    --full) MODE_FULL=1 ;;
    --build-only) BUILD_ONLY=1 ;;
    -h|--help)
      echo "Usage: $0 [--full] [--build-only]"
      exit 0
      ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

log() { printf '==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

for CORPUS in enwik1m enwik10m; do
  if [[ ! -f "$CORPUS" ]]; then
    die "Corpus not found: $CORPUS"
  fi
done

log "CMake configure + build"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DGLZA_PRINTON=ON 2>&1
cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1

if [[ $BUILD_ONLY -eq 1 ]]; then
  log "Build-only done."
  exit 0
fi

BIN="$BUILD_DIR"
FAILURES=0

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

log "Quick roundtrip matrix"
run_case "max_rules=5000 1m" "$BIN/rt_large 5000 enwik1m" 0
run_case "max_rules=6000 1m" "$BIN/rt_large 6000 enwik1m" 0
run_case "max_rules=6325 1m" "$BIN/rt_large 6325 enwik1m" 0
run_case "max_rules=5000 10m" "$BIN/rt_large 5000 enwik10m" 0
run_case "max_rules=6000 10m" "$BIN/rt_large 6000 enwik10m" 0
run_case "max_rules=6325 10m" "$BIN/rt_large 6325 enwik10m" 0
run_case "max_rules=6330 1m" "$BIN/rt_large 6330 enwik1m" 0
run_case "max_rules=6330 10m" "$BIN/rt_large 6330 enwik10m" 0

log "fast_mode=0 on enwik1m"
run_case "slow max_rules=500 1m" "$BIN/rt_slow 500 enwik1m" 0
run_case "slow max_rules=5000 1m" "$BIN/rt_slow 5000 enwik1m" 0
run_case "slow max_rules=6325 1m" "$BIN/rt_slow 6325 enwik1m" 0

log "Author-like on enwik1m"
run_case "author-like 1m" "$BIN/rt_author enwik1m" 0

if [[ $MODE_FULL -eq 1 ]] && [[ -f enwik100m ]]; then
  log "Extended 100m tests"
  run_case "max_rules=5000 100m" "$BIN/rt_large 5000 enwik100m" 0
  run_case "max_rules=6000 100m" "$BIN/rt_large 6000 enwik100m" 0
  run_case "max_rules=6325 100m" "$BIN/rt_large 6325 enwik100m" 0
  run_case "max_rules=6330 100m" "$BIN/rt_large 6330 enwik100m" 0
fi

echo ""
if [[ $FAILURES -eq 0 ]]; then
  log "All regression cases matched expectations."
  exit 0
else
  log "$FAILURES case(s) did not match expectations."
  exit 1
fi
