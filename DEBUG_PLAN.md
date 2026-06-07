# GLZA large-grammar debugging — independent continuation plan

This document captures techniques, tools, harnesses, and findings from debugging the
`max_rules` / cap-encoded dictionary roundtrip issue on branch `fix_arm_crashes`.
Use it with the aggregated runner: **`./regression.sh`**.

---

## 1. Problem summary

| Symptom | Meaning |
|---------|---------|
| Roundtrip OK at low `max_rules` | Compress + decode + byte compare succeed |
| Fail above ~6325–6330 on `enwik10m` (`fast_mode=1`) | Not monotonic: different `max_rules` → different grammars |
| `empty dictionary bin` / `DecodeBin out of range` | Decode arithmetic still running but dictionary state wrong |
| `size mismatch` | Silent corruption (no guard hit) |
| `encode: normalization exceeded 64 steps` | Encoder range collapsed; now exits **1** cleanly (no longer segfaults) |
| Segfault (139) | Mostly eliminated by encoder guards; rare if encode continues after `EncoderFailed` |
| Compress limit stderr (`suffix tree node budget`, `match prefix-tree limit`, …) | Arena/heuristic too small for input + `fast_mode=0`; see §10–12 |

**Working hypothesis:** encode/decode dictionary + MTF state desync during
`decode_new_cap_encoded` for some grammars — not same-process global pollution
(decode-only on saved blob fails the same way).

**TurboBench-safe today:** `GLZAparams.c` embedded defaults `max_rules=5000`, `fast_mode=1`.

---

## 2. Corpus and paths

| Path | Role |
|------|------|
| `enwik1m` | 1 MiB quick gate (in repo root; `./regression.sh` requires it) |
| `enwik10m` | 10 MiB primary regression corpus |
| `enwik100m` | 100 MiB scale test (`--full` only; slow) |
| `enwik9` | Full bench corpus (~364 MiB; author bench) |
| `/tmp/glzaNNNN.bin` | Saved compressed blobs from `rt_save` |

Run all commands from **`glza/`** unless noted. Harnesses accept a corpus path as the last argument
(e.g. `./rt_large 6325 enwik10m`).

---

## 3. Build conventions

### Release (primary)

```bash
cd glza
CLANG=/usr/bin/clang   # avoid Nix clang if it breaks -static / linking
$CLANG -O3 -D_FILE_OFFSET_BITS=64 -c GLZAcomp.c GLZAformat.c GLZAcompress.c \
  GLZAencode.c GLZAdecode.c GLZAmodel.c GLZAparams.c GLZAfail.c
$CLANG -O3 -D_FILE_OFFSET_BITS=64 -o rt_large scripts/test/rt_large.c *.o -lm -pthread
```

`make` in `glza/` may fail on macOS/Nix (`-static`, SDK paths). Prefer explicit `clang` as above.

### AddressSanitizer (memory errors)

On **macOS 26.4+**, Apple Clang’s ASAN runtime can **hang at process init**
(deadlock in `AsanInitInternal` → `get_dyld_hdr` → `dyld_shared_cache_iterate_text_swift`).
This affects any `-fsanitize=address` binary, not just GLZA. `./regression.sh --asan`
probes the runtime and skips with a message when init hangs. Use Linux or an LLVM build
with sanitizer_common PR #182943 (`_dyld_get_dyld_header`) until Apple/Xcode ship the fix.

```bash
$CLANG -O1 -g -fsanitize=address -D_FILE_OFFSET_BITS=64 -c GLZAdecode.c GLZAmodel.c GLZAparams.c
$CLANG -O1 -g -fsanitize=address -D_FILE_OFFSET_BITS=64 -o dec_asan dec_only6500.c \
  GLZAdecode.o GLZAmodel.o GLZAparams.o -lm -pthread
ASAN_OPTIONS=halt_on_error=1:detect_leaks=0 ./dec_asan /tmp/glza6400.bin
```

Full roundtrip ASAN needs all objects built with `-fsanitize=address` (see `regression.sh --asan`).

### Debug + LLDB (crash site)

```bash
$CLANG -O0 -g -D_FILE_OFFSET_BITS=64 -c GLZAdecode.c GLZAmodel.c GLZAparams.c
$CLANG -O0 -g -D_FILE_OFFSET_BITS=64 -o dec_dbg dec_only6500.c \
  GLZAdecode.o GLZAmodel.o GLZAparams.o -lm -pthread

lldb -b \
  -o 'run /tmp/glza6400.bin' \
  -o 'bt 10' \
  -o 'frame select 0' \
  -o 'source list' \
  -o 'quit' \
  -- ./dec_dbg
```

**Do not** leave lldb running in the background (Cursor tasks hung ~12h). Cleanup:

```bash
pkill -9 -f '/usr/bin/lldb' ; pkill -9 -f 'xcrun lldb'
pkill -9 -f '/TurboBench/glza/rt_' ; pkill -9 -f '/TurboBench/glza/dec_'
```

### Partial LLDB findings (already captured)

- **O1 `dec_only6500`:** `EXC_BAD_ACCESS` in `decode_new_cap_encoded` at string store (`strb` with bad index).
- **O0 `dec_dbg`:** `GLZAdecode.c` ~1291 — `symbol_strings[end_string_index++] = *symbol_string_ptr++` loop.

---

## 4. `struct param_data` (harness / API)

Field order in `GLZA.h`:

```c
uint32_t max_rules;
uint8_t cap_encoded, cap_lock_disabled, delta_disabled, create_words, fast_mode, user_set_RAM_size;
uint8_t user_set_profit_ratio_power, print_dictionary, use_mtf, two_threads;
double RAM_usage, order, profit_ratio_power;
```

| Preset | Fields | Use |
|--------|--------|-----|
| **TurboBench NULL** | `(defaults in GLZAparams.c)` | `params=NULL` → `max_rules=5000`, `fast_mode=1` |
| **rt_large CLI** | `max_rules=ARG`, rest like `{0xA00000,0,0,0,1,1,...}` | `fast_mode=1`, `two_threads=0` in harness |
| **Author bench** | `fast_mode=0`, `order=0.6`, `profit_ratio_power=4`, `RAM_usage=16000`, `user_set_RAM_size=1` | `rt_author10m.c`; matches `c -x -o0.6 -p4 -r16000` |
| **rt_slow** | `fast_mode=0`, `max_rules=ARG`, default RAM heuristic | `./rt_slow <max_rules> [corpus]`; prints `GLZA_last_fail_*` on compress fail |
| **rt_author** | Author scoring **without** forcing 16 GB RAM | `./rt_author [corpus]`; optional `GLZA_RAM_MB` override |
| **decode-only blob** | `max_rules` must match compress | `dec_only6500.c` hardcodes `6500` — edit or pass via new harness |

**CLI vs harness (`GLZA.c`):**

| Flag | Actual meaning |
|------|----------------|
| `-r16000` | **RAM MB**, not rule count |
| `-D#` | **max_rules** cap |
| `-x` | `fast_mode=0` (“extreme compression”) |
| `-o0.6` | `order=0.6`; also forces `fast_mode=0` |
| `-p4` | `profit_ratio_power=4` |
| `-C0` | disable cap transform |
| `-w0` | `create_words=0` |

Author command does **not** disable capitalization; enwik9 auto-enables `cap_encoded`.

---

## 5. Exit codes (`rt_large` roundtrip)

| Code | Meaning |
|------|---------|
| 0 | OK |
| 1 | `GLZAcomp` failed |
| 2 | `GLZAdecode` failed |
| 3 | Decoded size ≠ input size |
| 4 | Byte mismatch |
| 139 / 138 | Segfault / bus error (should be rare after hardening; report if seen) |

On compress failure, harnesses can print **`GLZA_last_fail_stage()`** and
**`GLZA_last_fail_detail()`** (set via `GLZAfail.c` in `GLZAcomp` / compress / encode paths).

---

## 6. Known `max_rules` ladder (`enwik10m`, `fast_mode=1`)

Use for bisect; **not monotonic** — do not assume failure above a single N.

| max_rules | Typical result |
|-----------|----------------|
| 5000–6325 | OK |
| 6330 | Decode error (empty dict bin) |
| 6335–6340 | Size mismatch |
| 6360 | Encode normalize failure |
| 6370–6500+ | Decode error or segfault |
| 6200 | Encode bus error (separate cliff) |

Occasional **exit 1** at `max_rules=6325` on enwik10m (~10% of runs) from encode normalization
or compress non-determinism — no longer exit 139 after atomic-store and encoder guard fixes.

### `fast_mode=0` ladder (updated)

| Corpus / harness | Typical result |
|------------------|----------------|
| `rt_slow 5000 enwik1m` | OK (~4 s) |
| `rt_author enwik1m` | OK (quick regression) |
| `rt_slow 5000 enwik10m` | Compress OK (~20 min); **decode fail** exit 2 at `InCharNum=920`: `empty dictionary bin at decode_new_cap dict` (100% reproducible) |
| `rt_slow 500 10m` / `5000 100m` | Expected fail (`--full`; compress limit or same decode bug) |
| Early `fast_mode=0` on 10m | Used to fail at compress (`InCharNum=73`) from match-prefix arena gap — **fixed** by heap fallback (§13) |

---

## 7. Techniques (what to do next)

### A. Regression gate

```bash
./regression.sh              # quick: rt_large 1m/10m, rt_slow 1m, rt_author 1m
./regression.sh --full       # + 100m matrix, rt_test, rt_slow 10m/100m (known fail), rt_fast0
./regression.sh --asan       # ASAN decode-only (skipped on macOS 26.4+ init hang)
./regression.sh --save-blobs # /tmp/glza6325.bin + glza6330.bin for split decode
```

**Quick gate** (~minutes): `rt_large` at 5000/6000/6325 on enwik1m and enwik10m; `6330` on 10m
expected fail; `rt_slow` and `rt_author` on enwik1m.

**Full gate** adds 100m points, NULL-params `rt_test`, slow 10m/100m cases (expected fail),
`rt_fast0` / `rt_same6500` historical checks. No `(flaky 139)` labels — failures are stable exit codes.

### B. Bisect `max_rules`

```bash
./rt_large 6320 ../enwik10m 2>&1 | tail -3
./rt_large 6330 ../enwik10m 2>&1 | head -5
```

Record **exit code** and **first stderr line** (encode vs decode vs mismatch).

### C. Split compress / decode (rule out same-process state)

```bash
# build rt_save + dec_only6500 once (see regression.sh)
./rt_save 6325 /tmp/glza6325.bin
./rt_save 6330 /tmp/glza6330.bin
./dec_only_mr /tmp/glza6325.bin 6325   # max_rules must match rt_save
./dec_only_mr /tmp/glza6330.bin 6330   # expect fail (match roundtrip)
```

If both fail/succeed consistently with roundtrip → **not** stale globals between comp/decode in one process.

### D. Compare encode vs decode type contexts (do not “fix” blindly)

Upstream pairs encode `(type & 0x18)` with decode `prior_type >> 4`. Naively aligning either side **breaks** `max_rules=6000`. Any fix must preserve small-grammar roundtrip.

Focus areas:

- `embed_define()` / `decode_new_cap_encoded()` dictionary insert/remove/`remaining`
- `decode_dict_fetch()` / `sum_nbob` / empty bin
- Type propagation: encode `+= (type & 0x18) - 8` vs decode `+= (prior_type & 0x30) + 4`

### E. Official CLI roundtrip

```bash
./GLZA c -D5000 -m1 ../enwik10m /tmp/out.glza
./GLZA d /tmp/out.glza /tmp/restored
cmp ../enwik10m /tmp/restored
```

Author profile (slow on 10m):

```bash
./GLZA c -x -o0.6 -p4 -r16000 ../enwik10m /tmp/out.glza
```

### F. TurboBench integration

From repo root:

```bash
./turbobench ../enwik10m -eglza
```

Uses NULL params → embedded `max_rules=5000`.

### G. Optional: PRINTON compress trace

```bash
$CLANG -O3 -DPRINTON -D_FILE_OFFSET_BITS=64 ...   # rebuild compress
# plot: scripts/glza_log_plot.py (if PRINTON stderr captured)
```

---

## 8. Harness inventory (C sources in `glza/`)

| File | Purpose |
|------|---------|
| `rt_large.c` | Main bisect: `./rt_large <max_rules> [corpus]` |
| `rt_test.c` | NULL vs explicit 5000 params; repeated compress reps |
| `rt_save.c` | Compress only → blob: `./rt_save <max_rules> <out.bin>` |
| `dec_only_mr.c` | Decode blob: `dec_only_mr <blob> [max_rules]` (match compress) |
| `dec_only6500.c` | Decode blob with hardcoded `max_rules=6500` |
| `dec_only.c` | Decode blob, `params=NULL` (embedded 5000) |
| `rt_same6500.c` | Same-process comp+decode @ 6500 |
| `rt_fast0.c` | Author-like `fast_mode=0`, full max_rules |
| `rt_slow.c` | `fast_mode=0` bisect: `./rt_slow <max_rules> [corpus]` |
| `rt_author.c` | Author-like without 16 GB RAM default |
| `rt_author10m.c` | Author-like + full max_rules on enwik10m |
| `rt_split.c` | Comp then decode separate in one binary |
| `rt_postcomp_saved.c` | Re-comp + decode saved blob |
| `rt_null_only.c` | NULL params roundtrip (lldb target) |
| `rt_d5000.c` | Quick check at 5000 |
| `save_comp.c` / `save_cli.c` | Save blob helpers |
| `dec_notmtf.c` | Decode with `use_mtf=0` override |

**Do not commit:** built binaries, `*.dSYM/`, ASAN artifacts, `/tmp/*.bin` unless you want LFS noise.

---

## 9. Key source locations

| Area | File | Notes |
|------|------|-------|
| Cap dictionary parse | `GLZAdecode.c` | `decode_new_cap_encoded`, `decode_dict_fetch` |
| Rule encode | `GLZAencode.c` | `embed_define`, `encode_dictionary_symbol` |
| Type byte layout | `GLZAencode.c` ~88–91 | bits 3–4 word class, bit 5 cap end |
| Embedded API defaults | `GLZAparams.c` | TurboBench NULL params |
| CLI | `GLZA.c` | Flag parsing |
| Compress loop | `GLZAcompress.c` | `max_rules` cap, `fast_mode`, arena layout |
| Match prefix heap fallback | `GLZAcompress.c` | `glza_bind_match_storage()`, `glza_free_match_heap_bufs()` |
| Compress RAM heuristic | `GLZAcompress.c` ~6177 | `(in_size * 250) + 40MB`, capped — **not** 16 GB by default |
| Failure diagnostics | `GLZAfail.c`, `GLZAcomp.c` | `GLZA_last_fail_stage/detail()` |
| Encoder guards | `GLZAmodel.c`, `GLZAencode.c` | `NORMALIZE_ENCODER`, `ReadEncoderFailed`, `ResetCodecGlobals` |
| Guards / cleanup | `GLZAdecode.c`, `GLZAmodel.c` | `DecoderFailed`, `ResetCodecGlobals` |
| Empty dict bin guard | `GLZAdecode.c` ~383 | `decode_dict_fetch` / `decode_new_cap_encoded` |

---

## 10. Hardening session findings (branch `fix_arm_crashes`)

### Fixed or mitigated

1. **Encoder segfault after normalization failure** — `Encode*` continued with `low=0, range=0`
   after `normalization exceeded 64 steps`. Fix: `NORMALIZE_ENCODER()` bails all encode paths;
   `ReadEncoderFailed()` checks in the main encode loop and before EOF flush; `ResetCodecGlobals()`
   at encode entry. Failures are exit **1**, not 139.

2. **Non-deterministic compress at high `max_rules`** — intermittent bad grammars from non-atomic
   writes to `rank_scores_*`, `substitute_data_*`, `scan_symbol_ptr`, `max_symbol_ptr`. Fixed with
   proper atomic stores; occasional encode normalize failure remains (~10% at 6325 on 10m) but clean exit.

3. **`fast_mode=0` compress abort on enwik10m** — match prefix-tree arena between grammar stream
   end and suffix-tree nodes was ~4 MB too small → trimmed match candidates → decode errors or crash.
   Fix: `glza_bind_match_storage()` heap fallback when arena gap insufficient; overlap check adjusted
   when `match_strings` lives on heap.

4. **ASAN init hang (macOS 26.4+)** — not GLZA-specific; `regression.sh` probes and skips. See §3.

5. **Opaque compress failures** — `GLZAfail_set()` records stage (`format` / `compress` / `encode`)
   and numeric context (arena bytes, `compress_RAM`, candidate index, etc.); `rt_slow` prints it.

### Still open (primary)

1. **Cap dictionary encode/decode desync** — `fast_mode=0` on enwik10m: compress completes,
   decode fails consistently at **`InCharNum=920`**: `empty dictionary bin at decode_new_cap dict`.
   Same failure on saved blob decode → not same-process pollution. Focus §7D (`embed_define` vs
   `decode_new_cap_encoded`, type propagation). Do **not** naively swap context formulas.

2. **Scale** — enwik100m `rt_slow 5000` hits suffix-tree node budget during compress before
   roundtrip is reachable; may need `GLZA_RAM_MB` or further arena work after decode fix.

3. **`fast_mode=1` high `max_rules`** — 6330+ decode/size/encode failures unchanged; separate from
   slow-mode cap bug but same dictionary family.

---

## 11. What was ruled out

- Same-process pollution only (decode-only blob fails).
- Dictionary output buffer floor alone (8× compressed size fix didn’t fix 6400+).
- Naive encode↔decode context formula swap (breaks 6000).
- **`fast_mode=0` enwik10m compress failure from match-prefix arena alone** (heap fallback fixes compress).
- **16 GB RAM required for 10m slow mode** — default heuristic allocates ~2.1–2.5 GB (`in_size*250+40MB`).
- LLDB campaigns (mostly aborted; partial frames above are enough for crash locale).

---

## 12. RAM and environment

| Variable | Effect |
|----------|--------|
| `GLZA_RAM_MB` | Optional; sets `user_set_RAM_size=1` and `RAM_usage` in `rt_slow` / `rt_author` |
| (default) | `(in_size * 250) + 40000000` bytes, capped by `max_memory_usage` and upper bounds in `GLZAcompress.c` |
| `GLZA_CLANG` | Compiler for `regression.sh` (default `/usr/bin/clang`) |

Author CLI `-r16000` is still 16 GB when passed explicitly; harnesses no longer assume that.

---

## 13. Suggested commit / PR discipline

1. Keep `max_rules=5000` embed until 6330+ roundtrip fixed on `enwik10m`.
2. One commit per concern: guards, globals reset, compress ARM, decode MTF, etc.
3. Update `GLZA_COMPRESS_GUIDE.md` when root cause is known.
4. Add passing `./regression.sh` to your pre-push habit.

---

## 14. Quick reference commands

```bash
cd glza
./regression.sh
./regression.sh --full --save-blobs
./rt_large 6325 enwik10m
./rt_large 6330 enwik10m 2>&1 | head -5
./rt_slow 5000 enwik1m          # pass
./rt_slow 5000 enwik10m         # compress OK, decode fail @920
GLZA_RAM_MB=8000 ./rt_slow 5000 enwik100m   # optional bump for scale tests
```
