# GLZA C++ port — compressor crash / grammar-growth on text

Status as of 2026-06-10. Audience: whoever picks up the GLZA C++ port next.

## RESOLVED 2026-06-10 — substitution now works (regression-green, ~2.0 bpB)

The grammar-growth / `score_map_` crash is **fixed**. Root cause was **not**
buffer placement or "heap mode" (the hypothesis below) — it was that the C++
port **never built a *searchable* match tree**. The overlap search
(`overlap_check_thread_impl`) dispatches on `child_ptr_array_[symbol]` and
follows Aho-Corasick `miss_ptr`/`hit_ptr` failure links, but the port:
- never populated `child_ptr_array_` (only `assign(..., nullptr)`),
- never computed `miss_ptr`/`hit_ptr` (`write_siblings_miss_ptr` was dead code),
- omitted the C's valid-candidate **rebuild** and **suffix-span** passes,
- and its build #1 skipped the mid-walk "matched an existing leaf → bad" check
  (`GLZAcompress.c:4910-4913`), so candidate strings descended *off* leaves and
  corrupted the tree.

So the search found **zero** matches every pass (proven: `occurrences_replaced`
== 0 while `num_file_symbols` climbed past `in_size` → the `score_map_` memset
overflow at `:1112`). Only rule *definitions* were appended; nothing was
substituted; the grammar grew unboundedly. `bind(...,0,0)` and the buffer layout
were downstream red herrings — it failed identically in arena mode with a real
estimate.

**The fix** (`process_ranked_candidates`, mirroring the C faithfully):
1. build #1 mid-walk substring check via `move_to_match_sibling`
   (`GLZAcompress.c:4905-4934`);
2. overlap/miss-detection pass (`:4939-5001`);
3. valid-candidate **rebuild** rooted at `child_ptr_array_`
   (`:5003-5037`) — this is what populates the table the search reads;
4. suffix **span** pass computing `miss_ptr`/`hit_ptr` (`:5039-5107`);
5. re-enabled the real `est` (`1 + Σ(ns-1)`, `est_max_len`), the post-build
   `match_strings` reposition (`:5111`), and the 3-case `begin_matches`
   placement (`:5158-5190`).

**Validated:** `regression_cmake.sh` green (was fully red mid-fix); enwik 200 KB
and 1 MB round-trip at **2.01 bpB** (was crash / ~4.8 bpB degraded); the ASan
matrix's 12 `score_map_:1112` compress crashes are gone. A throwaway diagnostic
counts substitutions per pass — build `-DGLZA_SUBST_DIAG` and watch
`occurrences_replaced` stay > 0 and `num_file_symbols` shrink.

### Still open (separate from the heap fallback)
- **Decoder cap-codec on `max_code_length < 14`.** Inputs whose dictionary code
  length stays below 14 (enwik slices up to ~150 KB; mcl 11–13) now *reach* the
  decoder (they used to crash in compress) and **mismatch on round-trip** — the
  boundary is exactly mcl≥14 → OK. This is the documented cap-encoding decoder
  bug, **not** the compressor. Real/large text (mcl≥14) is unaffected.
- **Two compress ASan crashes remain on pathological synthetic-repetitive input
  only** (`glza_compress.cpp:1523`, `:1363`) — score-tree buffer overflows on
  degenerate inputs, unrelated to substitution.
- Slow-mode (`-x`) suffix-tree crashes (`:2783`/`:668`) from the earlier sweep
  still need their own look.

---

## Original investigation (below) — superseded; kept for history

## TL;DR

The C++ port of GLZA's compressor (`glza/glza_compress.cpp`) does **not**
deduplicate correctly when the match prefix-tree spills to the heap fallback,
which is the de-facto path for real inputs. With the "correct" estimate enabled
the grammar **grows** instead of shrinking, eventually overflowing `score_map_`
and crashing. The tree is currently pinned to a degraded mode (`bind(...,0,0)`)
that keeps it *stable and regression-green* but compresses text poorly (~4.8
bits/symbol on enwik vs the ~2 bits/symbol GLZA should reach). Fixing it for real
requires getting heap-mode match substitution working — the memory-layout port
alone is necessary but **not sufficient**.

The decoder-side cap-encoding corruption bug (separate) is **fixed** and
validated; see `git log` / the cap-codec notes.

## Where it lives

`glza/glza_compress.cpp`, `Compressor::process_ranked_candidates(...)` — the
per-cycle routine that:
1. estimates the match prefix-tree size,
2. `bind_match_storage(...)` — places `match_nodes` / `match_strings` either in
   the **arena** (the gap between the grammar stream end and the suffix-tree
   nodes) or, when that gap is too small, on the **heap** (the "heap fallback"),
3. builds the match tree, finds occurrences (multi-threaded "overlap search" /
   `find_substitutions`), substitutes them, and appends new rule definitions.

The C reference is `glza/GLZAcompress.c` (`process_ranked_candidates`, ~line
4708; the analogous layout lives around lines 4853, 5111, 5116, 5162).

## The chain of findings (how we got here)

1. **Original turn-1 bug (enwik200m).** `bind_match_storage` was called with
   `est_match_nodes = 0, max_match_length = 0`. With `est=0`, `match_strings`
   is placed **aliasing** `match_nodes` and the heap fallback is never taken.
   On a 200 MB slow-mode run this produced a grammar that *grew* every pass
   (4.9 bits/sym, never converging). The intended fix: pass the real estimate
   (mirroring the C: `est = 1 + Σ(ns−1)`).

2. **Enabling the real estimate broke ordinary text.** With a real `est`,
   `regression_cmake.sh` started segfaulting / hanging on enwik1m/10m — runs that
   previously passed. Bisect confirmed: works at `HEAD~1` (`est=0,0`), breaks at
   the commit that introduced the estimate.

3. **The C++ port never ported the heap-aware downstream layout.** The C, after
   building the tree, does three things the C++ omitted:
   - **Repositions** `match_strings = match_nodes + num_match_nodes*sizeof(MatchNode)`
     (after the *actual* node count, not the estimate) — `GLZAcompress.c:5111`.
   - Places `overlap_check_data` on a heap buffer when the match strings are on
     the heap — `:5116`.
   - Has **three** placement cases for the match buffer (`begin_matches`):
     heap-strings → arena slot after `child_ptr_array`; heap-overlap → after
     arena strings; all-arena → after `overlap_check_data` — `:5162`.
   All three were ported (see git history of this file) — and it **still grows**.

4. **The real estimate ⇒ heap fallback almost always.** The match arena (grammar
   end → suffix-tree nodes) is small *by design*, independent of total RAM: even
   a 2 KB input with `-r2000` (2 GB) takes the heap fallback 11×. So heap mode is
   the path that matters for real inputs (including enwik200m at 8 GB).

5. **Heap-mode substitution finds no matches.** ASan pinned the crash to a
   heap-buffer-overflow `WRITE` in `rank_scores_thread_fast_impl`
   (`glza_compress.cpp:1112`, `memset(score_map_, 0, 2*num_file_symbols)`):
   `num_file_symbols` had grown past `in_size`, overflowing `score_map_` (sized
   `2*in_size` at `:3757`). I.e. each pass appends new rule **definitions** but
   replaces **no occurrences** (the overlap search yields nothing in heap mode),
   so the grammar grows. The C sizes `score_map` identically and never overflows
   because its dedup actually shrinks the grammar.

**Conclusion:** the bug is in the multi-threaded heap-mode match-finding /
substitution (`overlap_check_data`, the per-thread `next_match_ptrs` match
buffers, traversal of the heap `match_nodes` tree), *not* in the buffer
placement, which now matches the C.

## Evidence: more rules can't route around it (raising max_rules in fast mode)

To check whether the poor ratio is just the low `max_rules=5000` default, a
`glza,r<N>` param (set `max_rules`, keep `fast_mode=1`) was tried on enwik1m:

| max_rules | rules built | csize   | ratio   | C MB/s | result   |
|----------:|------------:|--------:|--------:|-------:|----------|
| 5000      | 5000        | 501264  | 47.804% | 2.45   | ok       |
| 20000     | 20000       | 500897  | 47.769% | 1.13   | ok       |
| 100000    | ~59320      | —       | —       | —      | SEGFAULT |
| 1000000   | ~59320      | —       | —       | —      | SEGFAULT |

So **4× the rules buys 0.035%** (and halves speed), and past ~20–60k rules it
**crashes** — the same spiral as the enwik200m bug: more rules ⇒ bigger grammar
stream ⇒ smaller match arena (`est=0,0` keeps `match_strings` there) ⇒
`match_nodes_limit` hit ("match prefix-tree limit ... too small even after heap
fallback") ⇒ candidates marked bad ⇒ no substitution ⇒ grammar grows ⇒ arena
shrinks more ⇒ `score_map_` overflow / segfault. Conclusion: **the bottleneck is
substitution quality, not the rule cap** — rules being created aren't replacing
occurrences, so adding more does nothing. No parameter fixes this; only the
heap-mode substitution work below does. (The `glza,r` param was a throwaway
diagnostic and is not kept.)

## Current state of the code

`process_ranked_candidates` calls `bind_match_storage(..., 0, 0)` on purpose
(there is a comment there explaining this). Consequences:
- **Stable**: `regression_cmake.sh` is green; round-trips correct.
- **Weak**: text compresses poorly (grammar barely shrinks). This is the
  enwik200m grammar-growth issue from turn 1, now contained to "bad ratio"
  rather than "crash".

## How to tackle it (recommended next session)

1. **Reproduce small.** Build the CMake debug+ASan target (`glza/build-dbg`,
   `-DGLZA_ASAN=ON`) and run `./glza/build/glza c -r2000 <2KB-text> out` — it
   takes the heap fallback and grows. (ASan is slow here because the compressor
   is multi-threaded; be patient or add a single-thread debug path.)
2. **Instrument the per-pass "occurrences replaced" count** in heap mode and
   confirm it is ~0 while definitions are still appended. `score_map_` overflow
   at `:1112` is the canary that growth has happened.
3. **Trace one overlap-search thread** (`find_substitutions` / the code that
   fills `next_match_ptrs[i]` by walking the heap `match_nodes` tree against the
   symbol stream) and diff its behavior against the C
   (`GLZAcompress.c` overlap search, ~5193+). The divergence is there: either
   the heap `match_nodes`/`match_strings` are read wrong, the per-thread match
   buffer pointers/strides are wrong in heap mode, or a `child_ptr_array`
   arena-vs-`std::vector` assumption (the C keeps `child_ptr_array` in the arena
   at `free_RAM_ptr`; the C++ uses a `std::vector`, so the arena slot bind
   reserves for it is unused — the heap `begin_matches` math depends on this).
4. **Only after matches are found again**, re-enable the real estimate +
   re-compute + 3-case placement and re-run `regression_cmake.sh` and the
   round-trip fuzzer.

### Cheaper alternative worth a look

If heap-mode substitution proves too costly to fix, consider **enlarging the
match arena** so the real estimate fits in arena mode (which—with the
`match_strings` reposition—should be correct), avoiding the heap path entirely.
This trades suffix-tree headroom for match-tree headroom; needs care for very
large inputs.

## Related, still-open (separate)

- Small inputs (≤~500 B) hang and ~40 KB crashes even at `0,0` under the small
  default CLI RAM (suffix-tree node limit, `nodes_num_limit` tiny). Likely a
  RAM/node-limit edge case, possibly pre-existing.
- The legacy C build (`make` in `glza/`) is broken: `glza_params.h` now uses
  `#include <cstdint>` (C++), so the `.c` files won't compile; `./glza/GLZA` is a
  stale prebuilt binary. The C is only useful as a *reference* to read.
