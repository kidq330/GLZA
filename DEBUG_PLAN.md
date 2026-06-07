# GLZA large-grammar debugging — independent continuation plan

This document captures techniques, tools, harnesses, and findings from debugging the
`max_rules` / cap-encoded dictionary roundtrip issue on branch `fix_arm_crashes`.
Use it with the aggregated runner: **`./regression.sh`**.

---

## 1. Problem summary

| Symptom | Meaning |
|---------|---------|
| Roundtrip OK at low `max_rules` | Compress + decode + byte compare succeed |
| Fail above ~6325–6330 on `enwik10m` | Not monotonic: different `max_rules` → different grammars |
| `empty dictionary bin` / `DecodeBin out of range` | Decode arithmetic still running but dictionary state wrong |
| `size mismatch` | Silent corruption (no guard hit) |
| `encode: normalization exceeded 64 steps` | Encoder range collapsed (separate failure mode) |
| Segfault (pre-hardening) | OOB in `decode_new_cap_encoded` string copy (~line 1291) |

**Working hypothesis:** encode/decode dictionary + MTF state desync during
`decode_new_cap_encoded` for some grammars — not same-process global pollution
(decode-only on saved blob fails the same way).

**TurboBench-safe today:** `GLZAparams.c` embedded defaults `max_rules=5000`, `fast_mode=1`.

---

## 2. Corpus and paths

| Path | Role |
|------|------|
| `../enwik10m` | 10 MiB regression corpus (default for harnesses) |
| `../enwik9` | Full bench corpus (slow; author bench) |
| `/tmp/glzaNNNN.bin` | Saved compressed blobs from `rt_save` |

Run all commands from **`glza/`** unless noted.

---

## 3. Build conventions

### Release (primary)

```bash
cd glza
CLANG=/usr/bin/clang   # avoid Nix clang if it breaks -static / linking
$CLANG -O3 -D_FILE_OFFSET_BITS=64 -c GLZAcomp.c GLZAformat.c GLZAcompress.c \
  GLZAencode.c GLZAdecode.c GLZAmodel.c GLZAparams.c
$CLANG -O3 -D_FILE_OFFSET_BITS=64 -o rt_large rt_large.c *.o -lm -pthread
```

`make` in `glza/` may fail on macOS/Nix (`-static`, SDK paths). Prefer explicit `clang` as above.

### AddressSanitizer (memory errors)

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
| 139 / 138 | Segfault / bus error (often pre-guard or encode collapse) |

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

Author-like `fast_mode=0` on enwik10m failed **earlier** (e.g. `InCharNum=73`).

---

## 7. Techniques (what to do next)

### A. Regression gate

```bash
./regression.sh              # quick (~few min)
./regression.sh --full       # more max_rules points + split tests
./regression.sh --asan       # ASAN decode-only on saved blob (needs blob)
```

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
| Compress loop | `GLZAcompress.c` | `max_rules` cap, `fast_mode` |
| Guards / cleanup | `GLZAdecode.c`, `GLZAmodel.c` | `DecoderFailed`, `ResetCodecGlobals` |

---

## 10. What was ruled out

- Same-process pollution only (decode-only blob fails).
- Dictionary output buffer floor alone (8× compressed size fix didn’t fix 6400+).
- Naive encode↔decode context formula swap (breaks 6000).
- LLDB campaigns (mostly aborted; partial frames above are enough for crash locale).

---

## 11. Suggested commit / PR discipline

1. Keep `max_rules=5000` embed until 6330+ roundtrip fixed on `enwik10m`.
2. One commit per concern: guards, globals reset, compress ARM, decode MTF, etc.
3. Update `GLZA_COMPRESS_GUIDE.md` when root cause is known.
4. Add passing `./regression.sh` to your pre-push habit.

---

## 12. Quick reference commands

```bash
cd glza
./regression.sh
./regression.sh --full --save-blobs
./rt_large 6325 ../enwik10m
./rt_large 6330 ../enwik10m 2>&1 | head -5
```
