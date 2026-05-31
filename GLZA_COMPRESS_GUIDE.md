# GLZA compress: implementation guide

This document explains how `GLZAcompress.c` works, with pointers into the C source. It is meant as a companion to the paper, which leaves out many operational details that only appear in the code.

**Scope:** grammar induction in `GLZAcompress.c`. Entropy coding lives in `GLZAencode.c` / `GLZAmodel.c` and is only summarized here.

---

## 1. High-level picture

GLZA builds a **straight-line context-free grammar** iteratively:

1. Parse input into a **symbol stream** (`uint32_t` values in RAM).
2. Each **pass** counts symbols, builds a **suffix tree** of repeated substrings, scores candidates, resolves **overlaps**, and **substitutes** the best patterns with new rule symbols.
3. Append each new rule’s RHS (as `-symbol` define markers) to the tail of the stream.
4. Repeat until no candidate beats `min_score` or the rule budget is exhausted.
5. Serialize the final grammar to bytes for `GLZAencode`.

The file header states this loop explicitly:

```19:25:glza/GLZAcompress.c
//   Iteratively does the following until there are no rules worth generating:
//     1. Counts the symbol occurences and calculates the log base 2 of each symbol's probability of occuring
//     2. Builds (in portions) the suffix tree and searches the nodes for the "most compressible" symbol strings
//     3. Invalidates less desireable strings that overlap with better ones
//     4. Replaces each occurence of the best strings with a rule symbol and adds the rule number followed by the rule
//        right hand side to the end of the data
```

Think **Sequitur/Re-Pair-like** grammar growth, but with suffix-tree indexing, overlap arbitration, and a multi-phase `scan_mode` strategy.

---

## 2. Symbol stream and grammar encoding

### 2.1 RAM layout of the grammar

| Region | Meaning |
|--------|---------|
| `[start_symbol_ptr … end_symbol_ptr)` | **Main stream**: literals, rule uses, and define markers |
| Tail after substitutions | **Rule definitions**: `-N` marker (`0x80000001` style) followed by RHS symbols |
| `0xFFFFFFFE` | EOF sentinel appended after initial parse |

**Terminals** are byte values (0–255) or UTF-8 code points. **Rules** are numbered from `num_terminals` upward. A **use** of rule *R* is stored as a positive extended index; a **define** is a negative index.

`first_define_index` tracks where the definitional tail begins so overlap search can skip rule bodies when appropriate.

### 2.2 Output bytes

After induction, `GLZAcompress` writes a **format byte** and re-encodes the symbol stream (`GLZAcompress` ~4728+). That byte feeds `GLZAencode`, which stores `cap_encoded` and `UTF8_compliant` in its own header.

---

## 3. `cap_encoded`

**Meaning:** *capitalization-aware preprocessing/encoding* — the compressor and entropy coder treat uppercase and lowercase as distinct **symbol contexts** (separate statistics, separate decode paths).

Set at parse time from the format byte produced by `GLZAformat`:

```2585:2586:glza/GLZAcompress.c
  format = **iobuf;
  cap_encoded = (format == 1);
```

In `GLZAformat.c`, `cap_encoded` is chosen when the input looks like natural text with mixed case (capital letters followed by lowercase, etc.); format byte bit 0 marks it:

```77:86:glza/GLZAformat.c
  // format byte: B0: cap encoded, B3:B1 = stride (0 - 4), ...
  cap_encoded = 0;
  ...
  if (params != 0) {
    cap_encoded = params->cap_encoded;
```

**Effects downstream:**

| Location | Behavior |
|----------|----------|
| `GLZAcompress` | `scan_mode` initial value; word scoring checks last char of candidate (`score_base_node_tree_words`) |
| `GLZAencode` / `GLZAdecode` | Separate capital/lowercase symbol queues and MTF contexts |
| `GLZAmodel.c` | `StartModelSymType(use_mtf, cap_encoded)` builds different model tables |

When `cap_encoded == 0` and UTF-8 is off, pass 0 uses the **word** suffix tree (`scan_mode == 0`). When `cap_encoded != 0`, word pass is skipped and scanning starts in general mode:

```2808:2808:glza/GLZAcompress.c
  scan_mode = ((cap_encoded == 0) && ((UTF8_compliant == 0) || (fast_mode == 0))) || (create_words == 0);
```

CLI: `GLZA.c` sets `params.cap_encoded = 1` for `-c1` / `-c2`.

---

## 4. Main loop and `scan_mode`

Everything hangs off `top_main_loop` in `GLZAcompress()` (~2810):

```
top_main_loop
├── recount / entropy prep (symbol_entropy, production_cost, …)
├── allocate suffix-tree region (base_nodes_child_node_num, nodes)
├── scan_mode == 0  →  word pass (then scan_mode = 1)
├── scan_mode == 1  →  run deduplication (then scan_mode = 2)
└── scan_mode >= 2  →  full suffix-tree pass(es), overlap, substitute
     └── loop until no candidates; may set scan_mode = 3 for fast-mode sections
```

### 4.1 Pass 0 — words (`scan_mode == 0`)

**Goal:** Find profitable **space-delimited tokens** (ASCII alnum + UTF-8) before general substring mining.

1. **Build word suffix tree** — currently **single-threaded** (~2886–2916); see §6.
2. **Score** each tree node with ≥2 instances ending at a word boundary (`score_base_node_tree_words`, ~1878).
3. **Rank** candidates via `rank_scores_thread` (producer/consumer with atomics).
4. **Build prefix trees** (`match_node` forest) for top candidates.
5. **Overlap check** (8-way threaded on large inputs).
6. **Substitute** occurrences; append rule RHS to tail.
7. `goto top_main_loop`.

Word candidates must end with space (`0x20`) and have a “word-like” last character (~1894–1898).

### 4.2 Pass 1 — run dedup (`scan_mode == 1`)

One-shot transition (~3346–3440): detect long runs of identical terminals (≥63), introduce run-dedup rules, rewrite stream, then `scan_mode = 2`.

### 4.3 Pass 2+ — general scan (`scan_mode >= 2`)

**Goal:** Arbitrary repeated substrings over the full alphabet of current symbols.

1. **Build suffix tree** via `add_suffix` — threaded in fast mode, single-threaded in quality mode (~3716–3742).
2. **Score** with `score_symbol_tree` / `rank_scores_thread`.
3. Same **prefix tree → overlap → substitute** pipeline as words, but overlap logic also handles `-define` regions (`thread_overlap_check_no_defs_*`).
4. **`scan_mode == 3`**: fast-mode subsection scanning (23 sections, sliding `min_score`).

Loop exits when `num_candidates == 0` or `num_rules` hits `max_rules` (~4708).

---

## 5. Suffix tree data structures

### 5.1 `struct node` — grammar suffix tree

```62:69:glza/GLZAcompress.c
struct node {
  uint32_t symbol;
  uint32_t last_match_index;
  int32_t sibling_node_num[2];
  int32_t child_node_num;
  uint32_t num_extra_symbols;
  uint32_t instances;
};
```

- **`last_match_index`**: index into `start_symbol_ptr` where this node’s string starts (minus edge compression).
- **`num_extra_symbols`**: edge compression — up to several symbols stored implicitly by comparing input bytes.
- **`instances`**: number of suffixes reaching this node (repeat count for scoring).
- **Binary sibling trie**: `sibling_node_num[0/1]` chosen by successive bits of `symbol >> 4` (general) or low bits (words).

**Root indexing:** `base_nodes_child_node_num` is a flat array:

- **Words:** 0x91 buckets — ASCII direct, UTF-8 high bytes mapped to `0x80 + (symbol & 0xF)`.
- **General scan:** `first_symbol * BASE_NODES_CHILD_ARRAY_SIZE + (next_symbol & 0xF)` with `BASE_NODES_CHILD_ARRAY_SIZE = 16`.

Negative values in `base_nodes_child_node_num` mean “not yet materialized as a `node`”; the high bit trick (`± 0x80000000`) stores a **symbol index** until the branch is expanded.

### 5.2 `add_word_suffix` vs `add_suffix`

| | `add_word_suffix` (~360) | `add_suffix` (~488) |
|---|--------------------------|---------------------|
| Trigger | After space, at word start | Every symbol position (general pass) |
| Stops at | Next space | `MAX_MATCH_LENGTH`, define boundaries |
| Root map | 0x91 word buckets | 16 × alphabet per first symbol |

Both grow the same `nodes[]` array and share `create_suffix_node` / `split_node_for_overlap`.

### 5.3 `struct match_node` — substitution prefix tree

Separate from suffix-tree nodes. Built per candidate after scoring; used to **find occurrences** in the stream for replacement. 16-way sibling nibble trie + `miss_ptr`/`hit_ptr` for fast scan-path matching.

---

## 6. Threading

### 6.1 Original word-tree design (4 threads)

**Still present** in `word_build_tree_thread` (~717) but **disabled** in the main path (~2889 comment).

Original algorithm:

1. Main thread scans the stream for word starts (after `0x20`).
2. Enqueue `start_positions[i]` into one of **4 ring buffers** by `*in_symbol_ptr & 3`.
3. Each worker:
   - Own `first_node_num` region: `1 + thread_num * (node_num_limit >> 3)` (partitioned node index space).
   - Calls `add_word_suffix` for each queued position.
4. Main thread sends `-1` sentinel per queue and `pthread_join`s.

**Race that broke ARM:** all threads mutate **`base_nodes_child_node_num[0..0x90]`** concurrently while inserting into shared `nodes[]`. UTF-8 paths hit the same buckets; partitioned `next_node_num` does **not** isolate root-child pointers.

### 6.2 Re-threading safely (once correctness fixes land)

Options, in increasing complexity:

1. **Keep single-threaded** for pass 0 (simplest; word pass is a small fraction of total time on large files).
2. **Mutex around entire `add_word_suffix`** — correct, low contention if queues are balanced.
3. **Per-bucket mutexes** on `base_nodes_child_node_num[bucket]` — finer-grained; 0x91 locks.
4. **Partition by bucket range** — assign disjoint root buckets to threads (works for ASCII; UTF-8 high-byte buckets still collide).
5. **Two-phase build** — threads only collect `(position, bucket)` pairs; single thread inserts (no sharing).

The scan-path **`build_tree_thread`** (~685) uses a similar queue pattern with `scan_symbol_ptr` / `max_symbol_ptr` atomics; **`add_suffix` is now guarded by `suffix_tree_mutex`** because workers share `base_nodes_child_node_num` and `nodes`.

### 6.3 Other threads

| Thread | Role |
|--------|------|
| `rank_scores_thread` | Sort/rank scored candidates into `candidates[]` |
| `overlap_check_thread` ×8 | Find overlapping matches per candidate |
| `substitute_thread` | Apply queued substitutions on large inputs |
| `find_substitutions_thread` ×6 | Parallel match finder when `num_file_symbols >= 100M` |

Producer/consumer indices (`rank_scores_*`, `substitute_data_*`) are atomics; several had width/bounds bugs fixed on the ARM branch (see commits on `fix_arm_crashes`).

---

## 7. Memory layout (single RAM block)

All hot data lives in one **`malloc(available_RAM)`** block anchored at `start_symbol_ptr`. **`end_RAM_ptr = start_symbol_ptr + available_RAM`**.

```
LOW ADDRESS                                                          HIGH ADDRESS
┌──────────────────────────────────────────────────────────────────────────────┐
│ start_symbol_ptr … end_symbol_ptr          │  symbol stream (grows/shrinks)  │
│                                            │  + rule tail appended each pass │
├────────────────────────────────────────────┼─────────────────────────────────┤
│ free_RAM_ptr → (per pass, after stream)    │  bump-pointer arena:            │
│   symbol_entropy[]                         │  - per-symbol float/double costs│
│   base_nodes_child_node_num[]              │  - suffix-tree root table       │
│   nodes[] …                                │  - suffix tree nodes (grow ↑)   │
│   (optional substitute region)             │  - substitute_data[0x40000]     │
│                                            │  - child_ptr_array              │
│                                            │  - match_nodes[]                │
│                                            │  - match_strings                │
│                                            │  - overlap_check[8]             │
│                                            │  - match list buffers (×8)      │
├────────────────────────────────────────────┴─────────────────────────────────┤
│ end_RAM_ptr (hard limit)                                                     │
└──────────────────────────────────────────────────────────────────────────────┘

Separate heap (large inputs, ARM fixes):
  substitute_heap_buf   — 16 MiB when num_file_symbols >= 1M (word pass)
  overlap_check_heap_buf — 8 × struct overlap_check when inline space insufficient
  find_substitutions_thread_data_buf — 6 × thread struct when symbols >= 100M
```

### 7.1 Per-pass pointer setup (~2873–2884)

Each `top_main_loop` iteration:

1. `free_RAM_ptr = align8(end_symbol_ptr)` — arena starts **immediately after** the current stream.
2. Skip `symbol_entropy` tables.
3. `base_nodes_child_node_num = (int32_t *)free_RAM_ptr`
4. `nodes = free_RAM_ptr + next_new_symbol_number * BASE_NODES_CHILD_ARRAY_SIZE * sizeof(int32_t)`
5. **`node_num_limit = (end_RAM_ptr - nodes) / sizeof(node)`** — suffix tree may grow from `nodes[1]` upward until this cap.

**Critical invariant:** bump allocations for substitute/match structures must stay **below** `nodes` (or in the separate substitute heap). Previously, inline allocation from `end_symbol_ptr` could **overlap the suffix tree** on large grammars.

### 7.2 Suffix tree node index partitions (threaded builds)

When threading is enabled:

- Word workers: `first_node_num = 1 + t * (node_num_limit >> 3)` — four quarters of the node array.
- Scan workers (`tree_thread_data[13]`): partitions by **symbol range** (13 bands), each with local `base_nodes_child_node_num` slice but shared `nodes` (hence mutex).

### 7.3 Globals worth knowing

| Global | Purpose |
|--------|---------|
| `start_symbol_ptr` | Base of symbol stream |
| `nodes` / `nodes_num_limit` | Suffix tree array + bounds |
| `base_nodes_child_node_num` | Root / first-edge table |
| `child_ptr_array[symbol]` | Prefix-tree roots per symbol (substitution phase) |
| `free_RAM_ptr` | Bump pointer — easy to lose track in gotos |
| `end_RAM_ptr` | End of malloc block |

---

## 8. Scoring, overlap, substitution

### 8.1 Scoring

**Words:** `score_base_node_tree_words` (~1878) walks the suffix tree depth-first using `node_data[]` as an explicit stack (`NODE_DATA_STACK_DEPTH = MAX_MATCH_LENGTH + 32`).

Score ≈ `(instances - 1) × (string_entropy - cost_of_new_symbol) - production_cost`.

Candidates above `min_score` go to `rank_scores_buffer`; `rank_scores_thread` sorts into `candidates[]`.

**General:** `score_symbol_tree` — similar entropy accounting with `nfs_profit` / `new_rule_cost` in quality mode.

### 8.2 Overlap resolution

Multiple candidates can match at the same stream position. The pipeline:

1. Build `match_node` prefix tree per candidate.
2. Scan stream; at each position record `(candidate_id, start_offset)` into per-thread match lists.
3. **`overlap_check_thread`** marks `candidate_bad[]` when shorter/lower-score matches conflict.
4. Process candidates in score order; skip bad ones.

Match list pointers (`next_match_ptr[i]`) must stay within **`match_stop_ptr[i]`** — unbounded writes corrupted adjacent arena memory.

### 8.3 Substitution

For each accepted candidate:

1. Walk stream with prefix tree; on leaf match emit `substitute_data` records: `(gap_len, 0x80000000+match_len, rule_id)`.
2. `substitute_thread` (large inputs) or inline loop compacts the stream.
3. Append `-rule_id` define marker and RHS symbols at tail; update `symbol_counts`, `num_rules`.

---

## 9. Control flow and gotos

The implementation favors **`goto` state machines** over structured loops:

| Label family | Purpose |
|--------------|---------|
| `top_main_loop` | Next induction pass |
| `wmain_symbol_substitution_loop_*` | Word-pass substitution scanner |
| `main_overlap_check_loop_*` | Single-thread overlap detection |
| `thread_overlap_check_*` | Worker overlap detection |
| `score_siblings` | DFS retreat in word scoring |
| `done_building_tree_tree` | Exit suffix-tree build |

When reading, identify **which phase** you are in (`scan_mode`, word vs scan, build vs score vs substitute) before following jumps.

---

## 10. ARM fix commits (reference)

On branch `fix_arm_crashes`, eight sequential commits document the correctness work:

| Commit | Topic |
|--------|-------|
| 1 | `node_num_limit` from `end_RAM_ptr` |
| 2 | Suffix node allocation bounds (`create_suffix_node`, `add_word_suffix`) |
| 3 | `node_data` stack depth for scoring |
| 4 | `find_substitutions` heap (was overlaid on `nodes[]`) |
| 5 | Substitute/match region bounds + substitute heap |
| 6 | `substitute_index` widened to `uint32_t` |
| 7 | Overlap match list bounds; scan-path `child_ptr_array` guards |
| 8 | `suffix_tree_mutex`; single-thread word tree |

View with: `git log 1056b71..HEAD --oneline`

---

## 11. Suggested reading order in the source

1. File header + structs (~19–160)
2. `GLZAcompress()` setup: parse, RAM alloc (~2514–2808)
3. `top_main_loop` (~2810)
4. Word pass block (`scan_mode == 0`, ~2886–3343)
5. `add_word_suffix` / `add_suffix` (~360–700)
6. Run dedup (`scan_mode == 1`, ~3346)
7. General scan + tree build (~3600–3780)
8. Overlap + substitute (~3850–4600)
9. Output serialization (~4724+)

For entropy coding after induction, continue with `GLZAencode.c` and `GLZAmodel.c`.

---

## 12. ASCII diagram — one induction pass

```
  symbol stream:  [the quick the fox ...][-rule][t][h][e][...]
                         │
                         ▼
              build suffix tree (nodes[])
                         │
                         ▼
              score repeated strings ──► rank candidates
                         │
                         ▼
         build match_node prefix forest per candidate
                         │
                         ▼
           overlap scan ──► mark candidate_bad[]
                         │
                         ▼
      substitute in stream ◄── append rule RHS at tail
                         │
                         └──► top_main_loop (next pass)
```

---

*Generated for TurboBench `glza/` submodule. Line numbers refer to the post-ARM-fix tree on `fix_arm_crashes`.*
