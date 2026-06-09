#!/usr/bin/env bash
# Overnight round-trip fuzzing for GLZA.
#
# Builds (if needed) and runs the libFuzzer harness in *fork mode* so it keeps
# going after a crash, saving every unique crash/leak/OOM/timeout as a minimal
# reproducer under fuzz/artifacts/. The corpus under fuzz/corpus/ grows with
# coverage-increasing inputs and is safe to commit (small, useful for
# regression).
#
# Run it inside the dev shell so clang++/cmake are on PATH:
#   nix develop --command bash glza/fuzz/run.sh
# or from inside `nix develop`:
#   bash fuzz/run.sh
#
# Stop with Ctrl-C; artifacts and corpus persist. Re-running resumes from the
# existing corpus.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # glza/ root
cd "$here"

BUILD_DIR="${BUILD_DIR:-build-fuzz}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
MAX_LEN="${MAX_LEN:-131072}"
RSS_LIMIT_MB="${RSS_LIMIT_MB:-4096}"
TIMEOUT="${TIMEOUT:-60}"          # per-input wall-clock seconds
TOTAL_TIME="${TOTAL_TIME:-0}"     # 0 = run until Ctrl-C; else seconds

mkdir -p fuzz/corpus fuzz/artifacts

# Seed the corpus on first run.
if [ -z "$(ls -A fuzz/corpus 2>/dev/null)" ]; then
  printf 'the quick brown fox the quick brown fox\n' > fuzz/corpus/seed_text
  printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'    > fuzz/corpus/seed_run
  head -c 8192 enwik1m > fuzz/corpus/seed_enwik 2>/dev/null || true
fi

# Configure + build. GLZA_FUZZ compiles the library sources straight into the
# fuzz target with -fsanitize=fuzzer,address, so the whole library is
# coverage-instrumented (real coverage-guided fuzzing) and ASan-checked. libc++
# hardening (on in this toolchain) additionally traps OOB std::vector indexing.
if [ ! -x "$BUILD_DIR/fuzz_roundtrip" ]; then
  cmake -S . -B "$BUILD_DIR" -DGLZA_FUZZ=ON -DGLZA_PRINTON=OFF
fi
cmake --build "$BUILD_DIR" --target fuzz_roundtrip -j"$JOBS"

echo "=== fuzzing: fork=$JOBS max_len=$MAX_LEN rss=${RSS_LIMIT_MB}MB timeout=${TIMEOUT}s total=${TOTAL_TIME:-inf}s ==="
exec "$BUILD_DIR/fuzz_roundtrip" \
  -fork="$JOBS" \
  -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1 \
  -max_len="$MAX_LEN" \
  -rss_limit_mb="$RSS_LIMIT_MB" \
  -timeout="$TIMEOUT" \
  -max_total_time="$TOTAL_TIME" \
  -print_final_stats=1 \
  -artifact_prefix=fuzz/artifacts/ \
  fuzz/corpus
