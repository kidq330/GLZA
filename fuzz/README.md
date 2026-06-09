# GLZA round-trip fuzzing

`fuzz_roundtrip.cpp` compresses then decompresses an arbitrary input and asserts
the result matches. Compression parameters (fast/slow, words on/off, max_rules,
cap-encoding, MTF, threading) are derived from the input's first byte so a single
corpus exercises many code paths. All of those map to real CLI flags
(`-x`, `-w0`, `-D`, `-C`, `-m`, `-t`), so every input is a state the CLI can reach.

## Running

```sh
# turnkey overnight run (fork mode: survives crashes, collects minimal repros)
nix develop --command bash fuzz/run.sh
# tunables: JOBS=, MAX_LEN=, RSS_LIMIT_MB=, TIMEOUT=, TOTAL_TIME= (0 = until Ctrl-C)
```

Built with AddressSanitizer on the library + libFuzzer on the harness; the
toolchain's libc++ hardening additionally traps out-of-bounds `std::vector`
indexing. New crashes/leaks/OOMs/timeouts land in `fuzz/artifacts/`; the corpus
in `fuzz/corpus/` grows with coverage and is safe to keep.

Reproduce a single artifact:
```sh
nix develop --command ./build-fuzz/fuzz_roundtrip path/to/crash-xxxx
# for file:line in the report, use the -O0 debug build:
nix develop --command ./build-fuzz-dbg/fuzz_roundtrip path/to/crash-xxxx
```

## Findings

Fixed:
- `glza_compress.cpp` UTF-8 scan — read 1 byte past buffer on a lead byte at EOF.
- `glza_format.cpp:244` cap-lock lookahead — off-by-one guard read `*(p+2)` at EOF.
- `glza_format.cpp:290` delta-stride entropy — read `inbuf2[i+k]` without `insize>=2k`.
- `glza_compress.cpp` UTF-8 **compliance misdetection** — breaking on an invalid
  lead byte that happened to be the *last* byte still left `in_char_ptr ==
  end_char_ptr`, so the data was wrongly declared UTF-8-compliant and the
  unconsumed final byte was dropped (silent corruption). Now gated on a
  `utf8_invalid` flag. This single fix resolved both a class of round-trip
  mismatches *and* the `symbol_ends_` OOB crash (`:3727`) — same root cause: the
  bad compliance flag drove a symbol miscount that over-read `symbol_ends_`.

The first three are faithful ports of pre-existing OOB reads in the C; ASan +
libc++ hardening surfaced them.

Open (minimal repros under `known_crashes/`, found once real coverage-guided
fuzzing was enabled — see note below). **All three are in the forced
cap-encoding path (`cap_encoded=1`, i.e. CLI `-C1`) and reproduce against the
upstream C reference (`./GLZA`) too — they are pre-existing upstream GLZA bugs,
not C++ port regressions.** Because the port is validated for byte-parity
against the C reference, "fixing" these would diverge from upstream; they need
to be fixed in upstream GLZA (or the port + reference together), so they're
parked here rather than patched:
- `mismatch_roundtrip_4.bin` (35 B), `mismatch_roundtrip_5.bin` (36 B) —
  **round-trip mismatch**: the cap-encoded grammar codec drops the final level-0
  symbol (encoder parses N, decoder emits N-1). Minimal 24 B repro confirmed
  identical loss in `./GLZA c -C1`. Highest severity (silent corruption).
- `crash_model_1276.bin` (33 B) — `glza_model.cpp:1276` cap-decode; the C
  reference *hangs* on the same input (the hardened C++ vector traps instead).
- `crash_compress_3157.bin` (4096 B) — `glza_compress.cpp:3157`, slow-mode +
  `-C1` on text; same subsystem as the dropped words-pass divergence (see
  the port-status notes).

## Harness/infra fixes applied

- Leaked `inbuf` and `compressed` on every input (would trip libFuzzer's leak
  detector immediately) — fixed.
- Only ever tested `fast_mode=1` — now derives all params from input byte 0.
- **`glza_lib` was not coverage-instrumented** (only the harness was), so libFuzzer
  ran black-box. `CMakeLists.txt` now compiles the GLZA sources directly into the
  fuzz target under `-fsanitize=fuzzer,address`; coverage went from ~5 to ~1800
  edges and immediately reached the deeper bugs above.
