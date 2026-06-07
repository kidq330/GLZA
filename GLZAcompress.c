/***********************************************************************

Copyright 2014-2026 Kennon Conrad

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

***********************************************************************/

// GLZAcompress.c
//   Iteratively does the following until there are no rules worth generating:
//     1. Counts the symbol occurences and calculates the log base 2 of each
//     symbol's probability of occuring
//     2. Builds (in portions) the suffix tree and searches the nodes for the
//     "most compressible" symbol strings
//     3. Invalidates less desireable strings that overlap with better ones
//     4. Replaces each occurence of the best strings with a rule symbol and
//     adds the rule number followed by the rule
//        right hand side to the end of the data

#include "GLZA.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLZA_DIE(msg)                                                          \
  do {                                                                         \
    fputs(msg, stderr);                                                        \
    exit(1);                                                                   \
  } while (0)

static uint8_t glza_warned_suffix_nodes;
static uint8_t glza_warned_match_nodes;
static uint8_t glza_warned_main_nodes;
static uint8_t glza_warned_rank_buffer;

static void glza_warn_once(uint8_t *flag, const char *message) {
  if (*flag == 0) {
    *flag = 1;
    fputs(message, stderr);
  }
}

static void glza_warn_suffix_nodes_limit(void) {
  glza_warn_once(&glza_warned_suffix_nodes,
                 "GLZA compress: suffix tree node limit reached "
                 "(increase RAM or reduce input)\n");
}

static void glza_warn_match_nodes_limit(void) {
  glza_warn_once(&glza_warned_match_nodes,
                 "GLZA compress: match prefix-tree node limit reached "
                 "(increase RAM or reduce candidates)\n");
}

static void glza_warn_main_nodes_limit(void) {
  glza_warn_once(&glza_warned_main_nodes,
                 "GLZA compress: suffix tree build stopped at node budget "
                 "(increase RAM)\n");
}

static void glza_warn_rank_buffer_limit(void) {
  glza_warn_once(&glza_warned_rank_buffer,
                 "GLZA compress: rank-scores buffer full (candidate cap "
                 "reached)\n");
}

const uint32_t MAX_WRITE_SIZE = 0x200000;
const uint32_t MAX_PRIOR_MATCHES = 20;
const uint32_t MAX_MATCH_LENGTH = 8000;
const uint32_t BASE_NODES_CHILD_ARRAY_SIZE = 16;
const uint32_t NUM_PRECALCULATED_LOG2_X = 0x4000;
const uint32_t NUM_PRECALCULATED_X_LOG2_X = 0x1000000;
const uint32_t NUM_PRECALCULATED_NFSMR_LOGS = 0x400;
const uint32_t NUM_PRECALCULATED_SYMBOL_COSTS = 2000;
const uint32_t MAX_SCORES = 30000;
const uint32_t MAX_SCORES_FAST = 0x7FFF;
const uint32_t NODE_DATA_STACK_DEPTH = MAX_MATCH_LENGTH + 32;
const float BIG_FLOAT = 1000000000.0;

enum glza_scan_mode : uint8_t {
  GLZA_SCAN_WORDS = 0,     /* word suffix-tree pass */
  GLZA_SCAN_RUN_DEDUP = 1, /* one-shot run deduplication */
  GLZA_SCAN_GENERAL = 2,   /* full suffix-tree scan */
  GLZA_SCAN_RETRY = 3,     /* retry with lower min_score (fast-mode sections) */
};

static uint32_t num_file_symbols; /* grammar stream length in uint32_t symbols
                                     (includes rule markers) */
static uint32_t
    num_terminals; /* alphabet size: UTF-8 code points or 0x100 raw bytes */
static uint32_t *start_symbol_ptr; /* base of in-RAM grammar stream; also
                                      bump-allocator arena base */
static uint32_t *end_symbol_ptr;   /* one-past-last live symbol; sentinel
                                      0xFFFFFFFE written at *end_symbol_ptr */
static uint32_t *symbol_counts; /* malloc'd [next_new_symbol_number]; size tied
                                   to max_rules allocation */
static uint32_t *next_match_ptr[8]; /* per overlap-check thread: cursor into
                                       that thread's match-pair slice */
static uint32_t num_starts[0x100];  /* order-1: count of symbols starting with
                                       each UTF-8 context byte */
static uint32_t num_ends[0x100];   /* order-1: count of symbols ending with each
                                      context byte */
static uint32_t o1c[0x100][0x100]; /* order-1 co-occurrence counts
                                      [end_context][start_context] */

// suffix-tree child index table;
// rows sized child_ptr_array_size * BASE_NODES_CHILD_ARRAY_SIZE
// base_nodes_child_node_num[g] == 0 -> no symbols in group g encountered yet, no node from root
// base_nodes_child_node_num[g] <  0 -> exactly one symbol in group g encountered, offset stored as negative
// base_nodes_child_node_num[g] >  0 -> value should be < `nodes_num_limit` and index `nodes`
static int32_t *base_nodes_child_node_num; 
static int16_t *score_map; /* fast_mode==1 only: maps file offsets to candidate
                              ranks (2*in_size bytes) */
static uint8_t
    cap_encoded; /* input used capitalization transform (format byte) */
static uint8_t fast_mode; /* 0=slow/exhaustive scoring path; 1=fast sectioned
                             path (TurboBench default) */
static atomic_uint_least16_t rank_scores_write_index,
    rank_scores_read_index; /* producer/consumer for rank_scores_buffer */
static atomic_uint_least32_t substitute_data_write_index,
    substitute_data_read_index; /* word-substitution command queue */
static atomic_uintptr_t max_symbol_ptr,
    scan_symbol_ptr; /* fast_mode==0 parallel tree build: furthest scanned
                        symbol address */
static double log_file_symbols; /* log2(num_file_symbols); refreshed each
                                   main_loop pass */
static double
    num_file_symbols_p1_x_log_file_symbols_p1; /* fast_mode==0 && scan_mode!=0:
                                                  x*log2(x) helper for
                                                  nfs_profit[] */
static double new_rule_cost; /* fast_mode==0 && scan_mode!=0: entropy cost of
                                adding one more production */
static double *x_log2_x;     /* fast_mode==0 only: malloc(8*max_x_log2_x);
                                max_x_log2_x tracks logical length */
static double order_ratio;   /* from params->order; used in fast_mode==0
                                entropy/profit scoring */
static double
    log2_x[0x4000]; /* precalc log2(i) for i in [1, NUM_PRECALCULATED_LOG2_X);
                       tight bound on index */
static double
    nfs_profit[0x400];  /* fast_mode==0 && scan_mode!=0: precalc NFS marginal
                           profit; index < NUM_PRECALCULATED_NFSMR_LOGS */
static float min_score; /* current candidate score threshold; adaptive each pass
                           (paths differ by fast_mode) */

struct node {
  uint32_t symbol;
  uint32_t last_match_index;
  // indexes into `nodes` for the sibling nodes.
  // induces a binary tree over symbols that map to the same base_nodes_child_node_num position.
  // "sibling" nodes are lateral, meaning they are at the same level in the GST
  int32_t sibling_node_num[2];
  int32_t child_node_num;
  uint32_t num_extra_symbols;
  uint32_t instances;
};

struct match_node {
  uint32_t symbol;
  uint32_t num_symbols;
  uint32_t score_number;
  struct match_node *child_ptr;
  uint32_t sibling_node_num[16];
  struct match_node *miss_ptr;
  struct match_node *hit_ptr;
};

struct tree_thread_data {
  uint32_t *start_cycle_symbol_ptr;
  uint32_t min_symbol;
  uint32_t max_symbol;
  uint32_t nodes_limit;
  uint32_t first_node_num;
  int32_t *base_nodes_child_node_num;
};

struct word_tree_thread_data {
  uint32_t first_node_num;
  uint32_t nodes_limit;
  int32_t start_positions[256];
  atomic_uint_least16_t write_index;
  atomic_uint_least16_t read_index;
};

struct node_score_data {
  float score;
  uint32_t last_match_index;
  uint32_t last_match_index2;
  uint16_t num_symbols;
};

struct rank_scores_thread_data {
  uint16_t *candidates_index; /* heap: permutation sorting candidates[] by
                                 score; size max_scores */
  uint16_t max_scores;        /* capacity this pass; mirrors outer max_scores
                                 (MAX_SCORES or MAX_SCORES_FAST) */
  uint16_t num_candidates;    /* active entries in candidates[] after ranking */
  uint32_t num_file_symbols;  /* snapshot for rank_scores_thread_fast only */
  struct node_score_data
      rank_scores_buffer[0x10000]; /* SPSC queue to rank thread; index capped by
                                      max_scores in practice */
  struct node_score_data
      candidates[0x8000]; /* scored substring candidates; logical size
                             num_candidates <= max_scores */
  uint16_t *
      candidates_position; /* fast_mode==1 only: inverse map for partial sort */
};

struct substitute_thread_data {
  uint32_t *in_symbol_ptr;
  uint32_t *out_symbol_ptr;
  uint32_t *symbol_counts;
  uint32_t *substitute_data;
  uint32_t max_rule_symbol;
};

struct score_data {
  struct node *node_ptr;
  double string_entropy;
  double string_profit;
  float string_entropy_f;
  uint16_t num_symbols;
  uint8_t next_sibling;
};

struct overlap_check {
  uint32_t *start_symbol_ptr;
  uint32_t *stop_matches_symbol_ptr;
  uint32_t *stop_symbol_ptr;
  uint32_t **next_match_ptr_ptr;
  uint32_t *match_stop_ptr;
  uint32_t num_overlaps; /* next free slot in second[]/next[]; fast_mode==1
                            overlap-list path only */
  uint8_t *candidate_bad;
  struct match_node *match_nodes;
  uint32_t second[150000]; /* heuristic cap (not num_candidates); OOB guarded by
                              num_overlaps < 150000 check */
  int32_t next[150000]; /* parallel adjacency for overlap pairs; same heuristic
                           bound as second[] */
};

struct find_substitutions_thread_data {
  uint32_t *start_symbol_ptr;
  uint32_t *stop_symbol_ptr;
  uint32_t extra_match_symbols; /* trailing symbols past stop_symbol_ptr still
                                   part of last match */
  uint32_t data[0x800000]; /* ring buffer of substitution ops; indices masked
                              with 0x7FFFFF; size is heuristic */
  struct match_node *match_nodes;
  atomic_uchar done;
  atomic_uint_least32_t write_index; /* producer index into data[] */
  atomic_uint_least32_t read_index;  /* consumer index into data[] */
};

static struct symbol_ends_data {
  uint8_t start;
  uint8_t end;
} *symbol_ends; /* malloc(max_rules): UTF-8 context byte at start/end of each
                   symbol (rule RHS metadata) */

static struct node *nodes; /* suffix-tree node array; allocated in-RAM after
                              base_nodes_child_node_num */
static uint32_t nodes_num_limit; /* max struct node slots from remaining RAM
                                    (nodes_num_limit = room / sizeof(node)) */
static uint32_t
    child_ptr_array_size; /* equals next_new_symbol_number; sizes
                             child_ptr_array[] and base_nodes rows */
static struct node_score_data
    *candidates; /* alias of rank_scores_data_ptr->candidates[0] for legacy code
                  */
static struct match_node *
    *child_ptr_array; /* per-first-symbol roots into match_nodes prefix tree;
                         size child_ptr_array_size */
static pthread_mutex_t suffix_tree_mutex = PTHREAD_MUTEX_INITIALIZER;

static double xlogx(double arg) { return arg * log2(arg); }

static uint8_t get_UTF8_context(uint32_t symbol) {
  if (symbol < 0x80) {
    return ((uint8_t)symbol);
  }
  if (symbol < 0x250) {
    return (0x80);
  }
  if (symbol < 0x370) {
    return (0x81);
  }
  if (symbol < 0x400) {
    return (0x82);
  }
  if (symbol < 0x530) {
    return (0x83);
  }
  if (symbol < 0x590) {
    return (0x84);
  }
  if (symbol < 0x600) {
    return (0x85);
  }
  if (symbol < 0x700) {
    return (0x86);
  }
  if (symbol < 0x800) {
    return (0x87);
  }
  if (symbol < 0x1000) {
    return (0x88);
  }
  if (symbol < 0x2000) {
    return (0x89);
  }
  if (symbol < 0x3000) {
    return (0x8A);
  }
  if (symbol < 0x3040) {
    return (0x8B);
  }
  if (symbol < 0x30A0) {
    return (0x8C);
  }
  if (symbol < 0x3100) {
    return (0x8D);
  }
  if (symbol < 0x3200) {
    return (0x8E);
  }
  if (symbol < 0xA000) {
    return (0x8F);
  }
  if (symbol < 0x10000) {
    return (0x8E);
  }
  return (0x90);
}

static void init_match_node(struct match_node *match_node_ptr, uint32_t symbol,
                            uint32_t match_num_symbols,
                            uint32_t match_score_number) {
  match_node_ptr->symbol = symbol;
  match_node_ptr->num_symbols = match_num_symbols;
  match_node_ptr->score_number = match_score_number;
  match_node_ptr->child_ptr = 0;
  uint64_t *sibling_nodes_ptr =
      (uint64_t *)&match_node_ptr->sibling_node_num[0];
  *sibling_nodes_ptr = 0;
  *(sibling_nodes_ptr + 1) = 0;
  *(sibling_nodes_ptr + 2) = 0;
  *(sibling_nodes_ptr + 3) = 0;
  *(sibling_nodes_ptr + 4) = 0;
  *(sibling_nodes_ptr + 5) = 0;
  *(sibling_nodes_ptr + 6) = 0;
  *(sibling_nodes_ptr + 7) = 0;
  match_node_ptr->miss_ptr = 0;
  match_node_ptr->hit_ptr = 0;
}

static uint8_t move_to_match_sibling(struct match_node *match_nodes,
                                     struct match_node **match_node_ptr_ptr,
                                     uint32_t symbol, uint8_t *sibling_number) {
  uint32_t shifted_symbol = symbol;
  *sibling_number = (uint8_t)(shifted_symbol & 0xF);
  while (symbol != (*match_node_ptr_ptr)->symbol) {
    if ((*match_node_ptr_ptr)->sibling_node_num[*sibling_number] == 0) {
      return 0;
    }
    *match_node_ptr_ptr =
        &match_nodes[(*match_node_ptr_ptr)->sibling_node_num[*sibling_number]];
    shifted_symbol >>= 4;
    *sibling_number = (uint8_t)(shifted_symbol & 0xF);
  }
  return 1;
}

static void
move_to_existing_match_sibling(struct match_node *match_nodes,
                               struct match_node **match_node_ptr_ptr,
                               uint32_t symbol) {
  uint8_t sibling_number;
  uint32_t shifted_symbol = symbol;
  while (symbol != (*match_node_ptr_ptr)->symbol) {
    sibling_number = (uint8_t)(shifted_symbol & 0xF);
    *match_node_ptr_ptr =
        &match_nodes[(*match_node_ptr_ptr)->sibling_node_num[sibling_number]];
    shifted_symbol >>= 4;
  }
}

static uint8_t move_to_search_sibling(struct match_node *match_nodes,
                                      uint32_t symbol,
                                      struct match_node **search_node_ptr_ptr) {
  uint32_t shifted_symbol = symbol;
  uint8_t sibling_nibble = (uint8_t)(shifted_symbol & 0xF);
  while (symbol != (*search_node_ptr_ptr)->symbol) {
    if ((*search_node_ptr_ptr)->sibling_node_num[sibling_nibble] == 0) {
      return 0;
    }
    *search_node_ptr_ptr =
        &match_nodes[(*search_node_ptr_ptr)->sibling_node_num[sibling_nibble]];
    shifted_symbol >>= 4;
    sibling_nibble = (uint8_t)(shifted_symbol & 0xF);
  }
  return 1;
}

static struct match_node *move_to_base_match_child_with_make(
    struct match_node *match_nodes, uint32_t symbol, uint32_t score_number,
    uint32_t *num_match_nodes_ptr, struct match_node **child_ptr_ptr) {
  struct match_node *match_node_ptr;
  if (*child_ptr_ptr == 0) {
    *child_ptr_ptr = &match_nodes[(*num_match_nodes_ptr)++];
    match_node_ptr = *child_ptr_ptr;
    init_match_node(match_node_ptr, symbol, 2, score_number);
  } else {
    match_node_ptr = *child_ptr_ptr;
    uint8_t sibling_number;
    if (move_to_match_sibling(match_nodes, &match_node_ptr, symbol,
                              &sibling_number) == 0) {
      match_node_ptr->sibling_node_num[sibling_number] = *num_match_nodes_ptr;
      match_node_ptr = &match_nodes[(*num_match_nodes_ptr)++];
      init_match_node(match_node_ptr, symbol, 2, score_number);
    }
  }
  return match_node_ptr;
}

static void move_to_match_child_with_make(
    struct match_node *match_nodes, struct match_node **match_node_ptr_ptr,
    uint32_t symbol, uint32_t score_number, uint32_t best_score_num_symbols,
    uint32_t *num_match_nodes_ptr) {
  if ((*match_node_ptr_ptr)->child_ptr == NULL) {
    (*match_node_ptr_ptr)->child_ptr = &match_nodes[(*num_match_nodes_ptr)++];
    *match_node_ptr_ptr = (*match_node_ptr_ptr)->child_ptr;
    init_match_node(*match_node_ptr_ptr, symbol, best_score_num_symbols,
                    score_number);
  } else {
    (*match_node_ptr_ptr) = (*match_node_ptr_ptr)->child_ptr;
    uint8_t sibling_number;
    if (move_to_match_sibling(match_nodes, match_node_ptr_ptr, symbol,
                              &sibling_number) == 0) {
      (*match_node_ptr_ptr)->sibling_node_num[sibling_number] =
          *num_match_nodes_ptr;
      *match_node_ptr_ptr = &match_nodes[(*num_match_nodes_ptr)++];
      init_match_node(*match_node_ptr_ptr, symbol, best_score_num_symbols,
                      score_number);
    }
  }
}

static void write_siblings_miss_ptr(struct match_node *match_nodes,
                                    struct match_node *node_ptr,
                                    struct match_node *miss_ptr) {
  uint8_t sibling_nibble;
  node_ptr->miss_ptr = miss_ptr;
  for (sibling_nibble = 0; sibling_nibble < 16; sibling_nibble++) {
    uint32_t sibling_node_number = node_ptr->sibling_node_num[sibling_nibble];
    if (sibling_node_number != 0) {
      write_siblings_miss_ptr(match_nodes, &match_nodes[sibling_node_number],
                              miss_ptr);
    }
  }
}

static struct node *create_suffix_node(uint32_t suffix_symbol,
                                       uint32_t symbol_index,
                                       uint32_t *next_node_num_ptr) {
  // __jm__ does this create a node for `suffix_symbol` connected to the
  // "implicit" root of the GST?
  if (*next_node_num_ptr >= nodes_num_limit) {
    glza_warn_suffix_nodes_limit();
    return NULL;
  }
  struct node *node_ptr = &nodes[(*next_node_num_ptr)++];
  node_ptr->symbol = suffix_symbol;
  node_ptr->last_match_index = symbol_index;
  node_ptr->sibling_node_num[0] = 0;
  node_ptr->sibling_node_num[1] = 0;
  node_ptr->child_node_num = 0;
  node_ptr->num_extra_symbols = 0;
  node_ptr->instances = 1;
  return node_ptr;
}

static struct node *split_node_for_overlap(struct node *node_ptr,
                                           uint32_t split_index,
                                           uint32_t in_symbol_index,
                                           uint32_t *next_node_num_ptr) {
  if (*next_node_num_ptr >= nodes_num_limit) {
    glza_warn_suffix_nodes_limit();
    return NULL;
  }
  uint32_t non_overlap_length = split_index - node_ptr->last_match_index;
  struct node *new_node_ptr = &nodes[*next_node_num_ptr];
  new_node_ptr->symbol = *(start_symbol_ptr + split_index);
  new_node_ptr->last_match_index = split_index;
  new_node_ptr->sibling_node_num[0] = 0;
  new_node_ptr->sibling_node_num[1] = 0;
  new_node_ptr->child_node_num = node_ptr->child_node_num;
  new_node_ptr->num_extra_symbols =
      node_ptr->num_extra_symbols - non_overlap_length;
  new_node_ptr->instances = node_ptr->instances;
  node_ptr->last_match_index = in_symbol_index;
  node_ptr->child_node_num = (*next_node_num_ptr)++;
  node_ptr->num_extra_symbols = non_overlap_length - 1;
  node_ptr->instances++;
  return new_node_ptr;
}

static void add_word_suffix(uint32_t *in_symbol_ptr,
                            uint32_t *next_node_num_ptr) {
  assert(in_symbol_ptr < end_symbol_ptr &&
         "bad arg: `in_symbol_ptr` cursor past EOS");
  ptrdiff_t stream_len = end_symbol_ptr - start_symbol_ptr;
  uint32_t search_symbol = *in_symbol_ptr;
  if ((int32_t)search_symbol < 0) {
    return;
  }
  // 
  int32_t *base_node_child_num_ptr =
      search_symbol < 0x80
          ? &base_nodes_child_node_num[search_symbol]
          // higher bytes are aggregated into 16 buckets, the last 4 bits serving as the index
          // e.g. 0x81 and 0x91 will be mapped into the same bucket
          : &base_nodes_child_node_num[0x80 + (search_symbol & 0xF)];

  if (*base_node_child_num_ptr == 0) {
    // Record WHERE in the stream this word started, but don't allocate a node yet.
    // Negative value = -(index XOR 0x80000000) effectively stores stream offset.
    *base_node_child_num_ptr = in_symbol_ptr - start_symbol_ptr - 0x80000000;
    return;
  }

  if (*base_node_child_num_ptr < 0) {
    // decoding: in_symbol_ptr - start_symbol_ptr = index in the input stream
    uint32_t symbol_index = *base_node_child_num_ptr + 0x80000000;
    assert(symbol_index < stream_len &&
           "decoded GST index must be valid offset into input stream");
    if (create_suffix_node(start_symbol_ptr[symbol_index], symbol_index,
                           next_node_num_ptr) == NULL) {
      return;
    }
    *base_node_child_num_ptr = (int32_t)(*next_node_num_ptr - 1);
  }

  if (*base_node_child_num_ptr <= 0) {
    assert(0 && "GST base child index invalid after expansion");
    return;
  }
  if ((uint32_t)*base_node_child_num_ptr >= nodes_num_limit) {
    assert(0 && "GST base child index out of range");
    return;
  }
  struct node *node_ptr = &nodes[*base_node_child_num_ptr];
  if (search_symbol != node_ptr->symbol) { 
    // condition is true for "bucketed" high-byte `search_symbol` values.
    // we drop the low 4 bits since they are identical for `search_symbol` and `node_ptr->symbol`
    //
    // follow siblings until match found or end of siblings found 
    uint32_t shifted_search_symbol = search_symbol >> 4;
    // node_ptr->sibling_node_num induces a binary tree of _lateral_ nodes.
    // they can all be considered immediate children of the GST root.
    // _lateral_ nodes are created in order, so for two symbols sharing low bits,
    // you will go through the one inserted previously into the GST to create the node of the second one
    //
    // The routing scheme is safe as long as the nodes don't overflow the global GST size limit.
    // Though it could potentially start creating degenerate linear paths after the first 28 bits
    // are traversed and already occupied.
    // __jm__ TODO: check how many symbols per bucket there can be and whether that affects it.
    do {
      int32_t *sibling_node_num_ptr =
          &node_ptr->sibling_node_num[shifted_search_symbol & 1];
      if (*sibling_node_num_ptr == 0) { // no match so add sibling
        if (create_suffix_node(search_symbol, in_symbol_ptr - start_symbol_ptr,
                               next_node_num_ptr) == NULL) {
          return;
        }
        *sibling_node_num_ptr = (int32_t)(*next_node_num_ptr - 1);
        return;
      }
      if ((uint32_t)*sibling_node_num_ptr >= nodes_num_limit) {
        assert(0 && "GST sibling node index out of range");
        return;
      }
      node_ptr = &nodes[*sibling_node_num_ptr];
      shifted_search_symbol >>= 1;
    } while (search_symbol != node_ptr->symbol);
  }

  // __jm__ giving up here
  // found a matching sibling
  uint32_t *first_symbol_ptr = in_symbol_ptr - 1;
  uint32_t *max_word_ptr = end_symbol_ptr - 1;
  if (first_symbol_ptr + MAX_MATCH_LENGTH - 1 < max_word_ptr) {
    max_word_ptr = first_symbol_ptr + MAX_MATCH_LENGTH - 1;
  }
  while (node_ptr->child_node_num != 0) {
    // matching sibling with child so check length of match
    uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
    if (num_extra_symbols != 0) {
      if (node_ptr->last_match_index + num_extra_symbols + 1 >=
          (uint32_t)stream_len) {
        assert(0 && "GST node span exceeds grammar stream");
        return;
      }
      uint32_t *node_symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
      uint32_t length = 1;
      do {
        if (in_symbol_ptr + length > max_word_ptr) {
          return;
        }
        if (node_ptr->last_match_index + length >= (uint32_t)stream_len) {
          assert(0 && "GST node span exceeds grammar stream");
          return;
        }
        if (*(node_symbol_ptr + length) !=
            *(in_symbol_ptr + length)) { // insert node in branch
          if (*next_node_num_ptr + 1 >= nodes_num_limit) {
            glza_warn_suffix_nodes_limit();
            return;
          }
          struct node *new_node_ptr = &nodes[*next_node_num_ptr];
          new_node_ptr->last_match_index = node_ptr->last_match_index + length;
          new_node_ptr->symbol = *(node_symbol_ptr + length);
          new_node_ptr->sibling_node_num[0] = 0;
          new_node_ptr->sibling_node_num[1] = 0;
          new_node_ptr->child_node_num = node_ptr->child_node_num;
          new_node_ptr->num_extra_symbols = num_extra_symbols - length;
          new_node_ptr->instances = node_ptr->instances;
          node_ptr->num_extra_symbols = length - 1;
          node_ptr->child_node_num = (*next_node_num_ptr)++;
          node_ptr->instances++;
          new_node_ptr->sibling_node_num[(*(in_symbol_ptr + length)) & 1] =
              *next_node_num_ptr;
          create_suffix_node(*(in_symbol_ptr + length),
                             in_symbol_ptr + length - start_symbol_ptr,
                             next_node_num_ptr);
          return;
        }
      } while (length++ != num_extra_symbols);
    }
    node_ptr->instances++;
    in_symbol_ptr += num_extra_symbols + 1;
    if (in_symbol_ptr > max_word_ptr || *(in_symbol_ptr - 1) == 0x20 ||
        in_symbol_ptr >= end_symbol_ptr) {
      return;
    }
    search_symbol = *in_symbol_ptr;
    if ((uint32_t)node_ptr->child_node_num >= nodes_num_limit) {
      assert(0 && "GST child node index out of range");
      return;
    }
    node_ptr = &nodes[node_ptr->child_node_num];
    if (search_symbol != node_ptr->symbol) { // follow siblings until match
                                             // found or end of siblings found
      uint32_t shifted_search_symbol = search_symbol;
      do {
        int32_t *prior_node_num_ptr =
            &node_ptr->sibling_node_num[shifted_search_symbol & 1];
        if (*prior_node_num_ptr == 0) {
          if (create_suffix_node(search_symbol,
                                 in_symbol_ptr - start_symbol_ptr,
                                 next_node_num_ptr) == NULL) {
            return;
          }
          *prior_node_num_ptr = (int32_t)(*next_node_num_ptr - 1);
          return;
        }
        if ((uint32_t)*prior_node_num_ptr >= nodes_num_limit) {
          assert(0 && "GST sibling node index out of range");
          return;
        }
        node_ptr = &nodes[*prior_node_num_ptr];
        shifted_search_symbol >>= 1;
      } while (search_symbol != node_ptr->symbol);
    }
  }

  // Matching node without child - extend branch, add child for previous
  // instance, add child sibling
  node_ptr->instances = 2;
  node_ptr->child_node_num = *next_node_num_ptr;
  if (node_ptr->last_match_index + 2 >= (uint32_t)stream_len) {
    assert(0 && "GST node span exceeds grammar stream");
    return;
  }
  uint32_t *node_symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
  if (in_symbol_ptr + 1 > max_word_ptr) {
    return;
  }
  if ((*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) &&
      (*in_symbol_ptr != 0x20) && (in_symbol_ptr < max_word_ptr)) {
    uint32_t length = 2;
    while ((in_symbol_ptr + length <= max_word_ptr) &&
           (node_ptr->last_match_index + length < stream_len) &&
           (*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
           (*(in_symbol_ptr + length - 1) != 0x20)) {
      length++;
    }
    if (node_ptr->last_match_index + length >= (uint32_t)stream_len) {
      assert(0 && "GST node span exceeds grammar stream");
      return;
    }
    node_ptr->num_extra_symbols = length - 1;
    node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                  node_symbol_ptr + length - start_symbol_ptr,
                                  next_node_num_ptr);
    if (node_ptr == NULL) {
      return;
    }
    node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
        *next_node_num_ptr;
    create_suffix_node(*(in_symbol_ptr + length),
                       in_symbol_ptr + length - start_symbol_ptr,
                       next_node_num_ptr);
    return;
  }
  node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                node_symbol_ptr + 1 - start_symbol_ptr,
                                next_node_num_ptr);
  if (node_ptr == NULL) {
    return;
  }
  node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] = *next_node_num_ptr;
  create_suffix_node(*(in_symbol_ptr + 1), in_symbol_ptr + 1 - start_symbol_ptr,
                     next_node_num_ptr);
}

static void add_suffix(uint32_t first_symbol, const uint32_t *in_symbol_ptr,
                       uint32_t *next_node_num_ptr) {
  struct node *node_ptr;
  uint32_t start_index = in_symbol_ptr - start_symbol_ptr - 1;
  uint32_t node_start_index = start_index + 1;
  uint32_t search_symbol = *in_symbol_ptr;
  int32_t *base_node_child_num_ptr =
      &base_nodes_child_node_num[(first_symbol * BASE_NODES_CHILD_ARRAY_SIZE) +
                                 (search_symbol & 0xF)];

  if (*base_node_child_num_ptr ==
      0) { // first occurence of the symbol, so create a child
    *base_node_child_num_ptr = node_start_index - 0x80000000;
    return;
  }
  if (*base_node_child_num_ptr < 0) {
    uint32_t symbol_index = *base_node_child_num_ptr + 0x80000000;
    assert(symbol_index < (uint32_t)(end_symbol_ptr - start_symbol_ptr) &&
           "decoded GST index must be valid offset into input stream");
    uint32_t symbol = *(start_symbol_ptr + symbol_index);
    *base_node_child_num_ptr = *next_node_num_ptr;
    node_ptr = create_suffix_node(symbol, symbol_index, next_node_num_ptr);
    if (search_symbol != symbol) {
      node_ptr->sibling_node_num[(search_symbol >> 4) & 1] =
          node_start_index - 0x80000000;
      return;
    }
    // Matching node without child - extend branch, add child for previous
    // instance, add child sibling
    uint32_t *node_symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
    if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
      uint32_t length = 2;
      while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
             (length < MAX_MATCH_LENGTH - 1)) {
        length++;
      }
      node_ptr->num_extra_symbols = length - 1;
      if (node_ptr->last_match_index + length <= start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      } else if (node_ptr->last_match_index < start_index) {
        node_ptr = split_node_for_overlap(node_ptr, start_index,
                                          node_start_index, next_node_num_ptr);
      }
      node_ptr->child_node_num = *next_node_num_ptr;
      node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                    node_symbol_ptr + length - start_symbol_ptr,
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
          node_start_index + length - 0x80000000;
    } else {
      if (node_ptr->last_match_index < start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      }
      node_ptr->child_node_num = *next_node_num_ptr;
      node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                    node_symbol_ptr + 1 - start_symbol_ptr,
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
          start_index + 2 - 0x80000000;
    }
    return;
  }

  node_ptr = &nodes[*base_node_child_num_ptr];
  if (search_symbol != node_ptr->symbol) { 
    // follow siblings until match found or end of siblings found
    uint32_t shifted_search_symbol = search_symbol >> 4;
    do {
      int32_t *sibling_node_num_ptr =
          &node_ptr->sibling_node_num[shifted_search_symbol & 1];
      if (*sibling_node_num_ptr == 0) { // no sibling so add sibling
        *sibling_node_num_ptr = node_start_index - 0x80000000;
        return;
      }
      if (*sibling_node_num_ptr < 0) {
        // turn the sibling into a node
        uint32_t symbol_index = *sibling_node_num_ptr + 0x80000000;
        assert(symbol_index < (uint32_t)(end_symbol_ptr - start_symbol_ptr) &&
               "decoded GST index must be valid offset into input stream");
        *sibling_node_num_ptr = *next_node_num_ptr;
        node_ptr = create_suffix_node(*(start_symbol_ptr + symbol_index),
                                      symbol_index, next_node_num_ptr);
        if (search_symbol != node_ptr->symbol) {
          node_ptr->sibling_node_num[(shifted_search_symbol >> 1) & 1] =
              node_start_index - 0x80000000;
          return;
        }
        // Matching node without child - extend branch, add child for previous
        // instance, add child sibling
        uint32_t *node_symbol_ptr =
            start_symbol_ptr + node_ptr->last_match_index;
        if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
          uint32_t length = 2;
          while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
                 (length < MAX_MATCH_LENGTH - 1)) {
            length++;
          }
          node_ptr->num_extra_symbols = length - 1;
          if (node_ptr->last_match_index + length <= start_index) {
            node_ptr->last_match_index = node_start_index;
            node_ptr->instances = 2;
          } else if (node_ptr->last_match_index < start_index) {
            node_ptr = split_node_for_overlap(
                node_ptr, start_index, node_start_index, next_node_num_ptr);
          }
          node_ptr->child_node_num = *next_node_num_ptr;
          node_ptr = create_suffix_node(
              *(node_symbol_ptr + length),
              node_symbol_ptr + length - start_symbol_ptr, next_node_num_ptr);
          node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
              node_start_index + length - 0x80000000;
        } else {
          if (node_ptr->last_match_index < start_index) {
            node_ptr->last_match_index = node_start_index;
            node_ptr->instances = 2;
          }
          node_ptr->child_node_num = *next_node_num_ptr;
          node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                        node_symbol_ptr + 1 - start_symbol_ptr,
                                        next_node_num_ptr);
          node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
              start_index + 2 - 0x80000000;
        }
        return;
      }
      node_ptr = &nodes[*sibling_node_num_ptr];
      shifted_search_symbol = shifted_search_symbol >> 1;
    } while (search_symbol != node_ptr->symbol);
  }

  // found a matching sibling
  while (node_ptr->child_node_num != 0) {
    // matching sibling with child so check length of match
    uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
    if (num_extra_symbols != 0) {
      uint32_t *node_symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
      uint32_t length = 1;
      do {
        if (*(node_symbol_ptr + length) !=
            *(in_symbol_ptr + length)) { // insert node in branch
          struct node *new_node_ptr = &nodes[*next_node_num_ptr];
          uint32_t new_node_lmi = node_ptr->last_match_index + length;
          new_node_ptr->last_match_index = new_node_lmi;
          new_node_ptr->symbol = *(node_symbol_ptr + length);
          new_node_ptr->sibling_node_num[0] = 0;
          new_node_ptr->sibling_node_num[1] = 0;
          new_node_ptr->child_node_num = node_ptr->child_node_num;
          new_node_ptr->num_extra_symbols = num_extra_symbols - length;
          new_node_ptr->instances = node_ptr->instances;
          node_ptr->num_extra_symbols = length - 1;
          node_ptr->child_node_num = (*next_node_num_ptr)++;
          new_node_ptr->sibling_node_num[(*(in_symbol_ptr + length)) & 1] =
              node_start_index + length - 0x80000000;
          if (new_node_lmi <= start_index) {
            node_ptr->last_match_index = node_start_index;
            node_ptr->instances++;
          } else if (node_ptr->last_match_index < start_index) {
            (void)split_node_for_overlap(node_ptr, start_index,
                                         node_start_index, next_node_num_ptr);
          }
          return;
        }
      } while (length++ != num_extra_symbols);
    }
    if (node_ptr->last_match_index + num_extra_symbols < start_index) {
      node_ptr->last_match_index = node_start_index;
      node_ptr->instances++;
    } else if (node_ptr->last_match_index < start_index) {
      node_ptr = split_node_for_overlap(node_ptr, start_index, node_start_index,
                                        next_node_num_ptr);
    }

    in_symbol_ptr += num_extra_symbols + 1;
    node_start_index += num_extra_symbols + 1;
    search_symbol = *in_symbol_ptr;
    node_ptr = &nodes[node_ptr->child_node_num];
    if (search_symbol != node_ptr->symbol) { // follow siblings until match
                                             // found or end of siblings found
      uint32_t shifted_search_symbol = search_symbol;
      do {
        int32_t *prior_node_num_ptr =
            &node_ptr->sibling_node_num[shifted_search_symbol & 1];
        if (*prior_node_num_ptr == 0) {
          *prior_node_num_ptr = node_start_index - 0x80000000;
          return;
        }
        if (*prior_node_num_ptr < 0) { // turn the sibling into a node
          uint32_t symbol_index = *prior_node_num_ptr + 0x80000000;
          assert(symbol_index < (uint32_t)(end_symbol_ptr - start_symbol_ptr) &&
                 "decoded GST index must be valid offset into input stream");
          *prior_node_num_ptr = *next_node_num_ptr;
          node_ptr = create_suffix_node(*(start_symbol_ptr + symbol_index),
                                        symbol_index, next_node_num_ptr);
          if (search_symbol == node_ptr->symbol) {
            break;
          }
          node_ptr->sibling_node_num[(shifted_search_symbol >> 1) & 1] =
              node_start_index - 0x80000000;
          return;
        }
        node_ptr = &nodes[*prior_node_num_ptr];
        shifted_search_symbol >>= 1;
      } while (search_symbol != node_ptr->symbol);
    }
  }

  // Matching node without child - extend branch, add child for previous
  // instance, add child sibling
  uint32_t *node_symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
  if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
    int32_t max_length = start_index + MAX_MATCH_LENGTH - 1 - node_start_index;
    if (max_length > 0) {
      int32_t length = 2;
      while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
             (length <= max_length)) {
        length++;
      }
      node_ptr->num_extra_symbols = length - 1;
      if (node_ptr->last_match_index + length <= start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      } else if (node_ptr->last_match_index < start_index) {
        node_ptr = split_node_for_overlap(node_ptr, start_index,
                                          node_start_index, next_node_num_ptr);
      }
      node_ptr->child_node_num = *next_node_num_ptr;
      node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                    node_symbol_ptr + length - start_symbol_ptr,
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
          node_start_index + length - 0x80000000;
      return;
    }
  }
  if (node_ptr->last_match_index < start_index) {
    node_ptr->last_match_index = node_start_index;
    node_ptr->instances = 2;
  }
  node_ptr->child_node_num = *next_node_num_ptr;
  node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                node_symbol_ptr + 1 - start_symbol_ptr,
                                next_node_num_ptr);
  node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
      node_start_index + 1 - 0x80000000;
}

static void *build_tree_thread(void *arg) {
  struct tree_thread_data *thread_data_ptr = (struct tree_thread_data *)arg;
  uint32_t *in_symbol_ptr = thread_data_ptr->start_cycle_symbol_ptr;
  uint32_t min_symbol = thread_data_ptr->min_symbol;
  uint32_t max_symbol = thread_data_ptr->max_symbol;
  uint32_t next_node_num = thread_data_ptr->first_node_num;
  uint32_t node_num_limit = thread_data_ptr->nodes_limit - 10;
  int32_t *base_nodes_child_node_num =
      thread_data_ptr->base_nodes_child_node_num;

  memset(base_nodes_child_node_num + (min_symbol * BASE_NODES_CHILD_ARRAY_SIZE),
         0, 4 * (max_symbol - min_symbol + 1) * BASE_NODES_CHILD_ARRAY_SIZE);
  while ((uint32_t *)atomic_load_explicit(
             &max_symbol_ptr, memory_order_relaxed) != in_symbol_ptr) {
    uint32_t *local_scan_symbol_ptr = (uint32_t *)atomic_load_explicit(
        &scan_symbol_ptr, memory_order_relaxed);
    if (in_symbol_ptr == local_scan_symbol_ptr) {
      sched_yield();
    } else {
      do {
        uint32_t symbol = *in_symbol_ptr++;
        if ((symbol >= min_symbol) && (symbol <= max_symbol)) {
          pthread_mutex_lock(&suffix_tree_mutex);
          add_suffix(symbol, in_symbol_ptr, &next_node_num);
          pthread_mutex_unlock(&suffix_tree_mutex);
          if (next_node_num >= node_num_limit) {
            glza_warn_main_nodes_limit();
            return 0;
          }
        }
      } while (in_symbol_ptr != local_scan_symbol_ptr);
    }
  }
  return 0;
}

// __jm__ unused function!!
static void *word_build_tree_thread(void *arg) {
  struct word_tree_thread_data *thread_data_ptr =
      (struct word_tree_thread_data *)arg;
  uint32_t next_node_num = thread_data_ptr->first_node_num;
  uint32_t nodes_limit = thread_data_ptr->nodes_limit - 10;
  uint16_t local_write_index;
  uint16_t local_read_index = 0;

  while (1) {
    while ((local_write_index = (uint16_t)atomic_load_explicit(
                &thread_data_ptr->write_index, memory_order_acquire)) ==
           local_read_index) {
      sched_yield();
    }
    do {
      if (thread_data_ptr->start_positions[local_read_index & 0xFF] < 0) {
        return 0;
      }
      add_word_suffix(
          start_symbol_ptr +
              thread_data_ptr->start_positions[local_read_index & 0xFF],
          &next_node_num);
      if (next_node_num >= nodes_limit) {
        glza_warn_main_nodes_limit();
        return 0;
      }
      atomic_store_explicit(&thread_data_ptr->read_index, ++local_read_index,
                            memory_order_relaxed);
    } while (local_read_index != local_write_index);
  }
}

static void *rank_scores_thread(void *arg) {
  struct rank_scores_thread_data *thread_data_ptr =
      (struct rank_scores_thread_data *)arg;
  struct node_score_data *rank_scores_buffer =
      &thread_data_ptr->rank_scores_buffer[0];
  struct node_score_data *candidates = &thread_data_ptr->candidates[0];
  uint16_t score_index;
  uint16_t node_score_num_symbols;
  uint16_t num_candidates;
  uint16_t node_ptrs_num;
  uint16_t local_write_index;
  uint16_t max_scores = thread_data_ptr->max_scores;
  uint16_t *candidates_index = thread_data_ptr->candidates_index;
  float score;

  while ((local_write_index = atomic_load_explicit(&rank_scores_write_index,
                                                   memory_order_acquire)) == 0)
    ; // wait
  if (rank_scores_buffer[0].last_match_index == 0) {
    thread_data_ptr->num_candidates = 0;
    return 0;
  }
  candidates_index[0] = 0;
  candidates[0].score = rank_scores_buffer[0].score;
  candidates[0].num_symbols = rank_scores_buffer[0].num_symbols;
  if (rank_scores_buffer[0].last_match_index <
      rank_scores_buffer[0].last_match_index2) {
    candidates[0].last_match_index = rank_scores_buffer[0].last_match_index;
    candidates[0].last_match_index2 = rank_scores_buffer[0].last_match_index2;
  } else {
    candidates[0].last_match_index = rank_scores_buffer[0].last_match_index2;
    candidates[0].last_match_index2 = rank_scores_buffer[0].last_match_index;
  }
  num_candidates = 1;
  node_ptrs_num = 1;

  while (1) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index = atomic_load_explicit(&rank_scores_write_index,
                                                      memory_order_acquire)) ==
            node_ptrs_num))
      ; // wait
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) {
      break;
    }
    score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score) {
      // find the rank of the score
      uint16_t new_score_rank = 0;
      uint16_t max_rank = num_candidates;
      do {
        uint16_t temp_rank = (new_score_rank + max_rank) >> 1;
        if (score > candidates[candidates_index[temp_rank]].score) {
          max_rank = temp_rank;
        } else {
          new_score_rank = temp_rank + 1;
        }
      } while (new_score_rank != max_rank);

      // check for overlaps with candidates with better scores
      uint16_t num_symbols = rank_scores_buffer[node_ptrs_num].num_symbols;
      int32_t new_score_lmi;
      int32_t new_score_lmi2;
      if (rank_scores_buffer[node_ptrs_num].last_match_index <
          rank_scores_buffer[node_ptrs_num].last_match_index2) {
        new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index;
        new_score_lmi2 = rank_scores_buffer[node_ptrs_num].last_match_index2;
      } else {
        new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index2;
        new_score_lmi2 = rank_scores_buffer[node_ptrs_num].last_match_index;
      }
      int32_t new_score_pmi = new_score_lmi - num_symbols;
      int32_t new_score_pmi2 = new_score_lmi2 - num_symbols;
      uint16_t rank = 0;
      while (rank < new_score_rank) {
        score_index = candidates_index[rank];
        node_score_num_symbols = candidates[score_index].num_symbols;
        int32_t slmi2 = candidates[score_index].last_match_index2;
        if (slmi2 <= new_score_pmi) {
          rank++;
        } else {
          int32_t slmi1 = candidates[score_index].last_match_index;
          if (new_score_lmi2 + node_score_num_symbols <= slmi1) {
            rank++;
          } else if (new_score_lmi + node_score_num_symbols <=
                     slmi2) {             // score1 before newscore2
            if (slmi1 <= new_score_pmi) { // score1 after newscore1
              if ((slmi2 <= new_score_pmi2) ||
                  (new_score_lmi2 + node_score_num_symbols <= slmi2)) {
                rank++;
              } else {
                goto rank_scores_thread_node_done;
              }
            } else if ((new_score_lmi + node_score_num_symbols <=
                        slmi1) // score1 before newscore1
                       &&
                       ((slmi2 <= new_score_pmi2) // score2 after newscore2
                        ||
                        ((new_score_lmi2 + node_score_num_symbols <= slmi2) &&
                         (slmi1 <= new_score_pmi2)))) {
              rank++;
            } else {
              goto rank_scores_thread_node_done;
            }
          } else {
            goto rank_scores_thread_node_done;
          }
        }
      }
      // no better candidate overlaps so node will be put on the list
      // look for overlaps with lower scoring candidates that should be removed
      // (only looks for one)
      if (rank != num_candidates) {
        do {
          score_index = candidates_index[rank];
          node_score_num_symbols = candidates[score_index].num_symbols;
          int32_t slmi2 = candidates[score_index].last_match_index2;
          if (slmi2 > new_score_pmi) {
            int32_t slmi1 = candidates[score_index].last_match_index;
            if (new_score_lmi2 + node_score_num_symbols > slmi1) {
              if ((slmi2 > new_score_pmi2) &&
                  (new_score_lmi2 + node_score_num_symbols > slmi2)) {
                goto rank_scores_thread_move_down;
              }
              if (slmi1 > new_score_pmi) {
                if ((new_score_lmi + node_score_num_symbols > slmi1) ||
                    (slmi1 > new_score_pmi2)) {
                  goto rank_scores_thread_move_down;
                }
              } else if (new_score_lmi + node_score_num_symbols > slmi2) {
                goto rank_scores_thread_move_down;
              }
            }
          }

        } while (++rank != num_candidates);
      }

      if (num_candidates !=
          max_scores) { // increment the list length if not at limit
        candidates_index[num_candidates] = num_candidates;
        num_candidates++;
      } else { // otherwise throw away the lowest score instead of moving it
        rank--;
      }

    rank_scores_thread_move_down:
      // move the lower scoring nodes down one location
      score_index = candidates_index[rank];
      //    memmove(&candidates_index[new_score_rank + 1],
      //    &candidates_index[new_score_rank], 2 * (rank - new_score_rank));
      //    (can fail due to DF corruption during rep movsq)
      uint16_t *score_ptr = &candidates_index[new_score_rank];
      uint16_t *candidate_ptr = &candidates_index[rank];
      if (candidate_ptr >= score_ptr + 8) {
        uint64_t first_four = *(uint64_t *)&candidates_index[new_score_rank];
        uint64_t next_four = *(uint64_t *)&candidates_index[new_score_rank + 4];
        do {
          *candidate_ptr = *(candidate_ptr - 1);
          *(candidate_ptr - 1) = *(candidate_ptr - 2);
          *(candidate_ptr - 2) = *(candidate_ptr - 3);
          *(candidate_ptr - 3) = *(candidate_ptr - 4);
          *(candidate_ptr - 4) = *(candidate_ptr - 5);
          *(candidate_ptr - 5) = *(candidate_ptr - 6);
          *(candidate_ptr - 6) = *(candidate_ptr - 7);
          *(candidate_ptr - 7) = *(candidate_ptr - 8);
        } while ((candidate_ptr -= 8) >= score_ptr + 8);
        *(uint64_t *)&candidates_index[new_score_rank + 1] = first_four;
        *(uint64_t *)&candidates_index[new_score_rank + 5] = next_four;
      } else if (candidate_ptr >= score_ptr + 4) {
        uint64_t first_four = *(uint64_t *)&candidates_index[new_score_rank];
        *(uint64_t *)(candidate_ptr - 3) = *(uint64_t *)(candidate_ptr - 4);
        *(uint64_t *)&candidates_index[new_score_rank + 1] = first_four;
      } else if (candidate_ptr >= score_ptr + 2) {
        uint16_t first = candidates_index[new_score_rank];
        *(uint32_t *)(candidate_ptr - 1) = *(uint32_t *)(candidate_ptr - 2);
        candidates_index[new_score_rank + 1] = first;
      } else if (candidate_ptr > score_ptr) {
        *candidate_ptr = *(candidate_ptr - 1);
      }
      candidates_index[new_score_rank] = score_index;

      // save the new score
      candidates[score_index].score = score;
      candidates[score_index].num_symbols = num_symbols;
      candidates[score_index].last_match_index = new_score_lmi;
      candidates[score_index].last_match_index2 = new_score_lmi2;
      if (num_candidates == max_scores) {
        min_score = candidates[candidates_index[max_scores - 1]].score;
      }
    }
  rank_scores_thread_node_done:
    atomic_store_explicit(&rank_scores_read_index, ++node_ptrs_num,
                          memory_order_relaxed);
  }
  thread_data_ptr->num_candidates = num_candidates;
  return 0;
}

static void *rank_scores_thread_fast(void *arg) {
  struct rank_scores_thread_data *thread_data_ptr =
      (struct rank_scores_thread_data *)arg;
  struct node_score_data *rank_scores_buffer =
      &thread_data_ptr->rank_scores_buffer[0];
  struct node_score_data *candidates = &thread_data_ptr->candidates[0];
  uint32_t new_score_lmi;
  uint32_t slmi;
  uint16_t max_scores = thread_data_ptr->max_scores;
  uint16_t *candidates_index = thread_data_ptr->candidates_index;
  uint16_t *candidates_position = thread_data_ptr->candidates_position;
  uint16_t num_symbols;
  uint16_t score_index;
  uint16_t num_found_overlaps;
  uint16_t prior_score;
  uint16_t position;
  uint16_t next_position;
  uint16_t min_position;
  uint16_t max_position;
  uint16_t first_unused_position;
  uint16_t section;
  uint16_t min_section;
  uint16_t max_section;
  uint16_t new_score_rank;
  uint16_t max_new_score_rank;
  uint16_t num_candidates;
  uint16_t candidate_index;
  uint16_t found_overlaps[MAX_SCORES_FAST];
  uint16_t local_write_index = 0;
  uint16_t node_ptrs_num = 0;
  uint8_t candidates_index_starts[0x80];
  float score;

  memset(candidates_index_starts, 0, 0x80);
  memset(score_map, 0, 2 * thread_data_ptr->num_file_symbols);
  for (size_t i = 0; i < max_scores; i++) {
    candidates_index[i] = i;
  }
  num_candidates = 0;

  while (1) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index = atomic_load_explicit(&rank_scores_write_index,
                                                      memory_order_acquire)) ==
            node_ptrs_num))
      ; // wait
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) {
      break;
    }
    score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score) {
      // find the rank of the score
      max_section = num_candidates >> 8;
      min_section = 0;
      while (min_section != max_section) {
        section = (min_section + max_section) >> 1;
        if (score >
            candidates[candidates_index
                           [(0x100 * section) +
                            (uint16_t)((
                                uint8_t)(candidates_index_starts[section] -
                                         1))]]
                .score) {
          max_section = section;
        } else {
          min_section = section + 1;
        }
      }
      section = max_section;
      max_new_score_rank = num_candidates > (0x100 * section) + 0xFF
                               ? (0x100 * section) + 0xFF
                               : num_candidates;
      new_score_rank = 0x100 * section;
      while (max_new_score_rank != new_score_rank) {
        uint16_t temp_rank = (max_new_score_rank + new_score_rank) >> 1;
        if (score >
            candidates[candidates_index
                           [(0x100 * section) +
                            (uint16_t)((
                                uint8_t)(temp_rank +
                                         candidates_index_starts[section]))]]
                .score) {
          max_new_score_rank = temp_rank;
        } else {
          new_score_rank = temp_rank + 1;
        }
      }

      // make overlap list and check for overlaps with candidates with better
      // scores
      num_symbols = rank_scores_buffer[node_ptrs_num].num_symbols;
      new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index;
      num_found_overlaps = 0;
      prior_score = 0;
      for (size_t i = new_score_lmi - num_symbols + 1; i <= new_score_lmi;
           i++) {
        if ((score_map[i] != 0) && (score_map[i] != prior_score)) {
          prior_score = score_map[i];
          uint8_t duplicate = 0;
          for (uint16_t j = 0; j < num_found_overlaps; j++) {
            if (found_overlaps[j] == score_map[i] - 1) {
              duplicate = 1;
            }
          }
          if (duplicate == 0) {
            section = candidates_position[score_map[i] - 1] >> 8;
            if (new_score_rank >
                (0x100 * section) +
                    (uint8_t)(candidates_position[score_map[i] - 1] -
                              candidates_index_starts[section])) {
              goto rank_scores_thread_fast_node_done;
            }
            found_overlaps[num_found_overlaps++] = score_map[i] - 1;
          }
        }
      }

      if (num_found_overlaps != 0) { // sort overlap list
        for (uint16_t j = 0; j < num_found_overlaps - 1; j++) {
          for (uint16_t k = j + 1; k < num_found_overlaps; k++) {
            section = candidates_position[found_overlaps[j]] >> 8;
            uint16_t rank_j = (0x100 * section) +
                              (uint8_t)(candidates_position[found_overlaps[j]] -
                                        candidates_index_starts[section]);
            section = candidates_position[found_overlaps[k]] >> 8;
            uint16_t rank_k = (0x100 * section) +
                              (uint8_t)(candidates_position[found_overlaps[k]] -
                                        candidates_index_starts[section]);
            if (rank_k < rank_j) {
              uint16_t temp_found_overlap = found_overlaps[j];
              found_overlaps[j] = found_overlaps[k];
              found_overlaps[k] = temp_found_overlap;
            }
          }
        }

        score_index = found_overlaps[0];
        first_unused_position = candidates_position[score_index];
        slmi = candidates[score_index].last_match_index;
        for (size_t i = slmi - candidates[score_index].num_symbols + 1;
             i <= slmi; i++) {
          score_map[i] = 0;
        }
        section = first_unused_position >> 8;

        while (--num_found_overlaps != 0) { // remove lower scoring overlaps
          score_index = found_overlaps[num_found_overlaps];
          position = candidates_position[score_index];
          slmi = candidates[score_index].last_match_index;
          for (size_t i = slmi - candidates[score_index].num_symbols + 1;
               i <= slmi; i++) {
            score_map[i] = 0;
          }
          section = position >> 8;
          max_section = --num_candidates >> 8;
          if (section != max_section) {
            max_position = (0x100 * section) +
                           (uint8_t)(candidates_index_starts[section] - 1);
            while (position != max_position) {
              next_position = (position & 0xFF00) + (uint8_t)(position + 1);
              candidates_index[position] = candidates_index[next_position];
              candidates_position[candidates_index[position]] = position;
              position = next_position;
            }
            section++;
            next_position =
                (0x100 * section) + candidates_index_starts[section];
            candidates_index[position] = candidates_index[next_position];
            candidates_position[candidates_index[position]] = position;
            while (section != max_section) {
              candidates_index_starts[section++]++;
              position = next_position;
              next_position =
                  (0x100 * section) + candidates_index_starts[section];
              candidates_index[position] = candidates_index[next_position];
              candidates_position[candidates_index[position]] = position;
            }
            position = next_position;
          }
          max_position =
              (num_candidates & 0xFF00) +
              (uint8_t)(num_candidates + candidates_index_starts[section]);
          while (position != max_position) {
            next_position = (position & 0xFF00) + (uint8_t)(position + 1);
            candidates_index[position] = candidates_index[next_position];
            candidates_position[candidates_index[position]] = position;
            position = next_position;
          }
          candidates_index[position] = score_index;
        }
        section = first_unused_position >> 8;
      } else if (num_candidates !=
                 max_scores) { // increment the list length if not at limit
        section = num_candidates >> 8;
        first_unused_position =
            (0x100 * section) +
            ((uint8_t)(num_candidates + candidates_index_starts[section]));
        num_candidates++;
      } else { // otherwise remove the lowest score
        section = (num_candidates - 1) >> 8;
        first_unused_position =
            (0x100 * section) +
            ((uint8_t)(num_candidates - 1 + candidates_index_starts[section]));
        candidate_index = candidates_index[first_unused_position];
        for (size_t i = candidates[candidate_index].last_match_index -
                        candidates[candidate_index].num_symbols + 1;
             i <= candidates[candidate_index].last_match_index; i++) {
          score_map[i] = 0;
        }
      }

      // move the lower scoring nodes down one location
      position = first_unused_position;
      score_index = candidates_index[position]; // save the index - use later to
                                                // hold new score
      min_section = new_score_rank >> 8;
      if (section != min_section) {
        min_position = (0x100 * section) + candidates_index_starts[section];
        while (position != min_position) {
          next_position = (position & 0xFF00) + (uint8_t)(position - 1);
          candidates_index[position] = candidates_index[next_position];
          candidates_position[candidates_index[position]] = position;
          position = next_position;
        }
        section--;
        next_position =
            (0x100 * section) + (uint8_t)(candidates_index_starts[section] - 1);
        candidates_index[position] = candidates_index[next_position];
        candidates_position[candidates_index[position]] = position;
        position = next_position;
        while (section != min_section) {
          --candidates_index_starts[section--];
          next_position = (0x100 * section) +
                          (uint8_t)(candidates_index_starts[section] - 1);
          candidates_index[position] = candidates_index[next_position];
          candidates_position[candidates_index[position]] = position;
          position = next_position;
        }
      }
      min_position =
          (0xFF00 & new_score_rank) +
          (uint8_t)(new_score_rank + candidates_index_starts[section]);
      while (position != min_position) {
        next_position = (position & 0xFF00) + (uint8_t)(position - 1);
        candidates_index[position] = candidates_index[next_position];
        candidates_position[candidates_index[position]] = position;
        position = next_position;
      }

      // save the new score
      candidates_index[position] = score_index;
      candidates_position[score_index] = position;
      candidates[score_index].score = score;
      candidates[score_index].num_symbols = num_symbols;
      candidates[score_index].last_match_index = new_score_lmi;
      for (size_t i = new_score_lmi - num_symbols + 1; i <= new_score_lmi;
           i++) {
        score_map[i] = score_index + 1;
      }
      if (num_candidates == max_scores) {
        section = (max_scores - 1) >> 8;
        position = (0x100 * section) +
                   (uint16_t)((uint8_t)(max_scores - 1 +
                                        candidates_index_starts[section]));
        min_score = candidates[candidates_index[position]].score;
      }
    }
  rank_scores_thread_fast_node_done:
    atomic_store_explicit(&rank_scores_read_index, ++node_ptrs_num,
                          memory_order_relaxed);
  }
  thread_data_ptr->num_candidates = num_candidates;
  if (num_candidates != 0) {
    max_section = (num_candidates - 1) >> 8;
    section = 1;
    uint16_t temp[0x100];
    while (section < max_section) {
      if (candidates_index_starts[section] != 0) {
        memcpy(&temp[0], &candidates_index[0x100 * section], 0x200);
        memcpy(&candidates_index[0x100 * section],
               &temp[candidates_index_starts[section]],
               0x200 - (2 * candidates_index_starts[section]));
        memcpy(&candidates_index[0x100 * section] +
                   (0x100 - candidates_index_starts[section]),
               &temp[0], 2 * candidates_index_starts[section]);
      }
      section++;
    }
  }
  return 0;
}

static void *rank_word_scores_thread(void *arg) {
  struct rank_scores_thread_data *thread_data_ptr =
      (struct rank_scores_thread_data *)arg;
  struct node_score_data *rank_scores_buffer =
      &thread_data_ptr->rank_scores_buffer[0];
  struct node_score_data *candidates = &thread_data_ptr->candidates[0];
  uint16_t max_scores = thread_data_ptr->max_scores;
  uint16_t *candidates_index = thread_data_ptr->candidates_index;
  uint16_t score_index;
  uint16_t local_write_index = 0;
  uint16_t node_ptrs_num = 0;
  uint16_t num_candidates = 0;

  while (1) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index = atomic_load_explicit(&rank_scores_write_index,
                                                      memory_order_acquire)) ==
            node_ptrs_num))
      ; // wait
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) {
      break;
    }
    float score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score) {
      // find the position in the score list this node would go in
      uint16_t new_score_rank;
      uint16_t candidate_search_size;
      new_score_rank = num_candidates;
      candidate_search_size = num_candidates + 1;
      do {
        candidate_search_size = (candidate_search_size + 1) >> 1;
        if (candidate_search_size > new_score_rank) {
          candidate_search_size = new_score_rank;
        }
        if (score >
            candidates[candidates_index[new_score_rank - candidate_search_size]]
                .score) {
          new_score_rank -= candidate_search_size;
        }
      } while (candidate_search_size > 1);

      if (num_candidates !=
          max_scores) { // increment the list length if not at limit
        candidates_index[num_candidates] = num_candidates;
        num_candidates++;
      }
      score_index = candidates_index[num_candidates - 1];
      memmove(&candidates_index[new_score_rank + 1],
              &candidates_index[new_score_rank],
              2 * (num_candidates - 1 - new_score_rank));
      candidates_index[new_score_rank] = score_index;
      candidates[score_index].score = score;
      candidates[score_index].num_symbols =
          rank_scores_buffer[node_ptrs_num].num_symbols;
      candidates[score_index].last_match_index =
          rank_scores_buffer[node_ptrs_num].last_match_index;
      if (num_candidates == max_scores) {
        min_score = candidates[candidates_index[max_scores - 1]].score;
      }
    }
    atomic_store_explicit(&rank_scores_read_index, ++node_ptrs_num,
                          memory_order_relaxed);
  }
  thread_data_ptr->num_candidates = num_candidates;
  return 0;
}

static void
score_base_node_tree(struct node *node_ptr, struct score_data *node_data,
                     double profit_ratio_power, const double *symbol_entropy,
                     struct node_score_data *rank_scores_buffer,
                     uint16_t *node_ptrs_num_ptr, uint32_t prior_symbol) {
  uint32_t instances;
  uint32_t node_instances;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  double repeats;
  double profit_per_substitution2;
  double bits_saved;
  double bits_saved2;
  double string_profit;
  double string_entropy2;
  double string_entropy = symbol_entropy[prior_symbol];
  double first_symbol_entropy = string_entropy;

  string_profit = symbol_counts[prior_symbol] < NUM_PRECALCULATED_X_LOG2_X
                      ? -x_log2_x[symbol_counts[prior_symbol]] - new_rule_cost
                      : xlogx(symbol_counts[prior_symbol]) - new_rule_cost;
  if ((node_ptr->instances == symbol_counts[prior_symbol]) &&
      (prior_symbol >= num_terminals)) {
    string_profit += new_rule_cost;
  }

  while (1) {
    node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      if ((node_ptr->sibling_node_num[0] > 0) ||
          (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].string_entropy = string_entropy;
        node_data[level].string_profit = string_profit;
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      repeats = (double)(node_instances - 1);
      // __jm__ differing arguments between branches?
      bits_saved = node_instances <= NUM_PRECALCULATED_X_LOG2_X
                       ? x_log2_x[node_instances - 1]
                       : xlogx(repeats);
      uint32_t *symbol_ptr =
          start_symbol_ptr + node_ptr->last_match_index - num_symbols + 1;
      do {
        instances = symbol_counts[*symbol_ptr];
        bits_saved +=
            instances - node_instances + 1 < NUM_PRECALCULATED_X_LOG2_X
                ? x_log2_x[instances - node_instances + 1]
                : xlogx(instances - node_instances + 1);
      } while (++symbol_ptr < start_symbol_ptr + node_ptr->last_match_index);

      uint32_t *end_symbol_ptr =
          start_symbol_ptr + node_ptr->last_match_index + num_extra_symbols;
      while (symbol_ptr <= end_symbol_ptr) {
        instances = symbol_counts[*symbol_ptr];
        if (instances < NUM_PRECALCULATED_X_LOG2_X) {
          string_profit -= x_log2_x[instances];
          bits_saved += x_log2_x[instances - node_instances + 1];
        } else {
          string_profit -= (double)instances * log2((double)instances);
          bits_saved += (double)(instances - node_instances + 1) *
                        log2((double)(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals)) {
          string_profit += new_rule_cost;
        }
        string_entropy += symbol_entropy[*symbol_ptr++];
      }
      bits_saved += (double)((node_instances - 1) *
                             (num_symbols + num_extra_symbols - 1)) *
                    nfs_profit[1];
      bits_saved += string_profit;

      // calculate score
      if (bits_saved > 0.0) {
        double score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                       (profit_ratio_power * log2(repeats * string_entropy));
        if (order_ratio == 0.0) {
          score += 40.0;
        } else if ((score > (2.0 * min_score) - 98.0) ||
                   (score > min_score - 40.5)) {
          string_entropy2 = first_symbol_entropy;
          symbol_ptr =
              start_symbol_ptr + node_ptr->last_match_index - num_symbols + 2;
          while (symbol_ptr <= end_symbol_ptr) {
            string_entropy2 +=
                log2(((double)num_ends[symbol_ends[*(symbol_ptr - 1)].end] -
                      0.9 * repeats) *
                     (double)num_starts[symbol_ends[*symbol_ptr].start] /
                     (((double)o1c[symbol_ends[*(symbol_ptr - 1)].end]
                                  [symbol_ends[*symbol_ptr].start] -
                       0.9 * repeats) *
                      (double)symbol_counts[*symbol_ptr]));
            symbol_ptr++;
          }
          profit_per_substitution2 =
              node_instances <= NUM_PRECALCULATED_LOG2_X
                  ? string_entropy2 + log2_x[node_instances - 1] -
                        log_file_symbols
                  : string_entropy2 + log2(repeats) - log_file_symbols;
          bits_saved2 = (repeats * profit_per_substitution2) - new_rule_cost;
          if (bits_saved2 > 0.0) {
            score =
                (score * ((float)1.0 - (float)order_ratio)) +
                (float)(order_ratio *
                        (log2(bits_saved2) +
                         profit_ratio_power *
                             log2(profit_per_substitution2 / string_entropy2)));
            score += 40.0;
          }
        }
        if (score > min_score) {
          struct node *child_ptr = &nodes[node_ptr->child_node_num];
          if ((node_ptrs_num & 0xFFF) == 0) {
            while ((uint16_t)(node_ptrs_num -
                              atomic_load_explicit(&rank_scores_read_index,
                                                   memory_order_acquire)) >=
                   0xF000)
              ; // wait
          }
          rank_scores_buffer[node_ptrs_num].score = score;
          rank_scores_buffer[node_ptrs_num].num_symbols =
              num_symbols + num_extra_symbols;
          rank_scores_buffer[node_ptrs_num].last_match_index =
              child_ptr->last_match_index - 1;
          rank_scores_buffer[node_ptrs_num].last_match_index2 =
              node_ptr->last_match_index + num_extra_symbols;
          if (rank_scores_buffer[node_ptrs_num].last_match_index ==
              rank_scores_buffer[node_ptrs_num].last_match_index2) {
            int32_t *sibling_node_num_ptr = &child_ptr->sibling_node_num[0];
            if (*sibling_node_num_ptr > 0) {
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  nodes[*sibling_node_num_ptr].last_match_index - 1;
            } else if (*sibling_node_num_ptr != 0) {
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  *sibling_node_num_ptr + 0x7FFFFFFF;
            } else if (*(sibling_node_num_ptr + 1) > 0) {
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  nodes[*(sibling_node_num_ptr + 1)].last_match_index - 1;
            } else if (*(sibling_node_num_ptr + 1) != 0) {
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  *(sibling_node_num_ptr + 1) + 0x7FFFFFFF;
            }
          }
          atomic_store_explicit(&rank_scores_write_index, ++node_ptrs_num,
                                memory_order_release);
        }
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes[node_ptr->child_node_num]; // move to child
    } else {
      int32_t sib_node_num = node_ptr->sibling_node_num[0];
      struct node *tnp = &nodes[sib_node_num];
      if ((sib_node_num > 0) &&
          ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
           (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy = string_entropy;
          node_data[level].string_profit = string_profit;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes[sib_node_num]; // move to sibling 0
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        tnp = &nodes[sib_node_num];
        if ((sib_node_num > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes[sib_node_num]; // move to sibling 1 - prior symbol
                                           // unchanged (okay)
        } else {
          if (level == 0) {
            *node_ptrs_num_ptr = node_ptrs_num;
            return;
          }
          string_entropy = node_data[--level].string_entropy; // pop stack
          string_profit = node_data[level].string_profit;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0) {
              node_data[level++].next_sibling = 1; // put sibling 1 on stack
            }
            node_ptr =
                &nodes[node_ptr->sibling_node_num[0]]; // move to sibling 0
          } else {
            node_ptr =
                &nodes[node_ptr->sibling_node_num[1]]; // move to sibling 1
          }
        }
      }
    }
  }
}

static void score_base_node_tree_fast(
    struct node *node_ptr, struct score_data *node_data, float string_entropy,
    float production_cost, float profit_ratio_power,
    float log2_num_symbols_plus_substitution_cost, const float *new_symbol_cost,
    const float *symbol_entropy, struct node_score_data *rank_scores_buffer,
    uint16_t *node_ptrs_num_ptr) {
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  float profit_per_substitution;
  float bits_saved;

  while (1) {
    uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      node_data[level].string_entropy_f = string_entropy;
      uint32_t symbol = node_ptr->symbol;
      string_entropy += symbol_entropy[symbol];
      uint32_t num_extra_symbols = 0;
      float repeats = (float)(node_instances - 1);
      while (num_extra_symbols != node_ptr->num_extra_symbols) {
        symbol = *(start_symbol_ptr + node_ptr->last_match_index +
                   ++num_extra_symbols);
        string_entropy += symbol_entropy[symbol];
      }

      // calculate score
      profit_per_substitution =
          node_instances < NUM_PRECALCULATED_SYMBOL_COSTS
              ? string_entropy - new_symbol_cost[node_instances]
              : string_entropy -
                    (log2_num_symbols_plus_substitution_cost - log2f(repeats));
      if (profit_per_substitution >= 0.0) {
        bits_saved = (repeats * profit_per_substitution) - production_cost;
        if (bits_saved > min_score) {
          float profit_ratio = profit_per_substitution / string_entropy;
          float score =
              log2f(bits_saved) + (profit_ratio_power * log2f(profit_ratio));
          score += 2.125;
          if (score > min_score) {
            uint32_t new_score_lmi =
                node_ptr->last_match_index + num_extra_symbols;
            if ((node_ptrs_num & 0xFFF) == 0) {
              while ((uint16_t)(node_ptrs_num -
                                atomic_load_explicit(&rank_scores_read_index,
                                                     memory_order_acquire)) >=
                     0xF000)
                ; // wait
            }
            rank_scores_buffer[node_ptrs_num].score = score;
            rank_scores_buffer[node_ptrs_num].last_match_index = new_score_lmi;
            rank_scores_buffer[node_ptrs_num].num_symbols =
                num_symbols + num_extra_symbols;
            atomic_store_explicit(&rank_scores_write_index, ++node_ptrs_num,
                                  memory_order_release);
          }
        }
      }
      if ((node_ptr->sibling_node_num[0] > 0) ||
          (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes[node_ptr->child_node_num];
    } else {
      int32_t sib_node_num = node_ptr->sibling_node_num[0];
      struct node *tnp = &nodes[sib_node_num];
      if ((sib_node_num > 0) &&
          ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
           (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy_f = string_entropy;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes[sib_node_num];
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        tnp = &nodes[sib_node_num];
        if ((sib_node_num > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes[sib_node_num];
        } else {
          if (level == 0) {
            *node_ptrs_num_ptr = node_ptrs_num;
            return;
          }
          string_entropy = node_data[--level].string_entropy_f;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0) {
              node_data[level++].next_sibling = 1;
            }
            node_ptr = &nodes[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

static void score_base_node_tree_cap(struct node *node_ptr,
                                     struct score_data *node_data,
                                     double profit_ratio_power,
                                     const double *symbol_entropy,
                                     struct node_score_data *rank_scores_buffer,
                                     uint16_t *node_ptrs_num_ptr,
                                     uint32_t prior_symbol) {
  uint32_t instances;
  uint32_t node_instances;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  double repeats;
  double profit_per_substitution2;
  double bits_saved;
  double bits_saved2;
  double string_profit;
  double string_entropy2;
  double string_entropy = symbol_entropy[prior_symbol];
  double first_symbol_entropy = string_entropy;

  string_profit = symbol_counts[prior_symbol] < NUM_PRECALCULATED_X_LOG2_X
                      ? -x_log2_x[symbol_counts[prior_symbol]] - new_rule_cost
                      : xlogx(symbol_counts[prior_symbol]) - new_rule_cost;
  if ((node_ptr->instances == symbol_counts[prior_symbol]) &&
      (prior_symbol >= num_terminals)) {
    string_profit += new_rule_cost;
  }

  while (1) {
    node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      double score;
      double short_score;
      int8_t send_score = -1;
      if ((node_ptr->sibling_node_num[0] > 0) ||
          (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].string_entropy = string_entropy;
        node_data[level].string_profit = string_profit;
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      repeats = (double)(node_instances - 1);
      bits_saved = node_instances <= NUM_PRECALCULATED_X_LOG2_X
                       ? x_log2_x[node_instances - 1]
                       : xlogx(repeats);

      uint32_t *symbol_ptr =
          start_symbol_ptr + node_ptr->last_match_index - num_symbols + 1;
      do {
        instances = symbol_counts[*symbol_ptr];
        bits_saved +=
            instances - node_instances + 1 < NUM_PRECALCULATED_X_LOG2_X
                ? x_log2_x[instances - node_instances + 1]
                : xlogx(instances - node_instances + 1);
      } while (++symbol_ptr < start_symbol_ptr + node_ptr->last_match_index);

      if (num_extra_symbols == 0) {
        instances = symbol_counts[node_ptr->symbol];
        if (instances < NUM_PRECALCULATED_X_LOG2_X) {
          string_profit -= x_log2_x[instances];
          bits_saved += x_log2_x[instances - node_instances + 1];
        } else {
          string_profit -= (double)instances * log2((double)instances);
          bits_saved += (double)(instances - node_instances + 1) *
                        log2((double)(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals)) {
          string_profit += new_rule_cost;
        }
        if ((num_symbols - 1) * (node_instances - 1) < 0x400) {
          bits_saved += nfs_profit[(num_symbols - 1) * (node_instances - 1)];
        } else {
          bits_saved +=
              num_file_symbols_p1_x_log_file_symbols_p1 -
              ((double)(num_file_symbols + 1 -
                        ((num_symbols - 1) * (node_instances - 1))) *
               log2((double)(num_file_symbols + 1 -
                             ((num_symbols - 1) * (node_instances - 1)))));
        }
        bits_saved += string_profit;
        string_entropy += symbol_entropy[*symbol_ptr];

        // calculate score
        if (bits_saved > 0.0) {
          score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                  (profit_ratio_power * log2(repeats * string_entropy));
          double penalty;
          if (*symbol_ptr == 0x20) {
            if (*(symbol_ptr + 1) != 0x20) {
              score -= 2.0;
              penalty = 2.0;
            } else {
              score -= 1.0;
              penalty = 1.0;
            }
          } else if ((*symbol_ptr & 0xF2) != 0x42) {
            score -= 1.0;
            penalty = 1.0;
          } else {
            penalty = 0.0;
          }
          if (order_ratio == 0.0) {
            score += 40.0;
            if (score > min_score) {
              send_score = 0;
            }
          } else if ((score > (2.0 * min_score) - 98.0) ||
                     (score > min_score - 40.5)) {
            string_entropy2 = first_symbol_entropy;
            symbol_ptr =
                start_symbol_ptr + node_ptr->last_match_index - num_symbols + 2;
            do {
              string_entropy2 +=
                  log2(((double)num_ends[symbol_ends[*(symbol_ptr - 1)].end] -
                        0.9 * repeats) *
                       (double)num_starts[symbol_ends[*symbol_ptr].start] /
                       (((double)o1c[symbol_ends[*(symbol_ptr - 1)].end]
                                    [symbol_ends[*symbol_ptr].start] -
                         0.9 * repeats) *
                        (double)symbol_counts[*symbol_ptr]));
            } while (symbol_ptr++ <
                     start_symbol_ptr + node_ptr->last_match_index);
            profit_per_substitution2 =
                node_instances <= NUM_PRECALCULATED_LOG2_X
                    ? string_entropy2 + log2_x[node_instances - 1] -
                          log_file_symbols
                    : string_entropy2 + log2(repeats) - log_file_symbols;
            bits_saved2 = (repeats * profit_per_substitution2) - new_rule_cost;
            if (bits_saved2 > 0.0) {
              score = (score * (1.0 - order_ratio)) +
                      (order_ratio *
                       (log2(bits_saved2) +
                        profit_ratio_power *
                            log2(profit_per_substitution2 / string_entropy2) -
                        penalty));
              score += 40.0;
              if (score > min_score) {
                send_score = 0;
              }
            }
          }
        }
      } else {
        uint32_t *end_symbol_ptr =
            start_symbol_ptr + node_ptr->last_match_index + num_extra_symbols;
        while (symbol_ptr < end_symbol_ptr) {
          instances = symbol_counts[*symbol_ptr];
          if (instances < NUM_PRECALCULATED_X_LOG2_X) {
            string_profit -= x_log2_x[instances];
            bits_saved += x_log2_x[instances - node_instances + 1];
          } else {
            string_profit -= (double)instances * log2((double)instances);
            bits_saved += (double)(instances - node_instances + 1) *
                          log2((double)(instances - node_instances + 1));
          }
          if ((node_instances == instances) && (*symbol_ptr >= num_terminals)) {
            string_profit += new_rule_cost;
          }
          string_entropy += symbol_entropy[*symbol_ptr++];
        }
        string_entropy2 = first_symbol_entropy;
        short_score = min_score;
        if ((*symbol_ptr == 0x20) && (*(symbol_ptr + 1) != 0x20)) {
          double temp_bits_saved;
          temp_bits_saved =
              (node_instances - 1) * (num_symbols + num_extra_symbols - 2) <
                      0x400
                  ? nfs_profit[(node_instances - 1) *
                               (num_symbols + num_extra_symbols - 2)]
                  : num_file_symbols_p1_x_log_file_symbols_p1 -
                        ((double)(num_file_symbols + 1 -
                                  ((node_instances - 1) *
                                   (num_symbols + num_extra_symbols - 2))) *
                         log2((
                             double)(num_file_symbols + 1 -
                                     ((node_instances - 1) *
                                      (num_symbols + num_extra_symbols - 2)))));
          temp_bits_saved += bits_saved + string_profit;

          // calculate score
          if (temp_bits_saved > 0.0) {
            short_score =
                ((profit_ratio_power + 1.0) * log2(temp_bits_saved)) -
                (profit_ratio_power * log2(repeats * string_entropy)) - 1.0;
            if (order_ratio == 0.0) {
              short_score += 40.0;
              if (short_score > min_score) {
                send_score = 1;
              }
            } else if ((short_score > (2.0 * min_score) - 98.0) ||
                       (short_score > min_score - 40.5)) {
              symbol_ptr = start_symbol_ptr + node_ptr->last_match_index -
                           num_symbols + 2;
              while (symbol_ptr < end_symbol_ptr) {
                string_entropy2 +=
                    log2(((double)num_ends[symbol_ends[*(symbol_ptr - 1)].end] -
                          0.9 * repeats) *
                         (double)num_starts[symbol_ends[*symbol_ptr].start] /
                         (((double)o1c[symbol_ends[*(symbol_ptr - 1)].end]
                                      [symbol_ends[*symbol_ptr].start] -
                           0.9 * repeats) *
                          (double)symbol_counts[*symbol_ptr]));
                symbol_ptr++;
              }
              profit_per_substitution2 =
                  node_instances <= NUM_PRECALCULATED_LOG2_X
                      ? string_entropy2 + log2_x[node_instances - 1] -
                            log_file_symbols
                      : string_entropy2 + log2(repeats) - log_file_symbols;
              bits_saved2 =
                  (repeats * profit_per_substitution2) - new_rule_cost;
              if (bits_saved2 > 0.0) {
                short_score =
                    (short_score * (1.0 - order_ratio)) +
                    (order_ratio *
                     (log2(bits_saved2) +
                      profit_ratio_power *
                          log2(profit_per_substitution2 / string_entropy2) -
                      1.0));
                short_score += 40.0;
                if (short_score > min_score) {
                  send_score = 1;
                }
              }
            }
          }
        }

        instances = symbol_counts[*symbol_ptr];
        if (instances < NUM_PRECALCULATED_X_LOG2_X) {
          string_profit -= x_log2_x[instances];
          bits_saved += x_log2_x[instances - node_instances + 1];
        } else {
          string_profit -= (double)instances * log2((double)instances);
          bits_saved += (double)(instances - node_instances + 1) *
                        log2((double)(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals)) {
          string_profit += new_rule_cost;
        }
        string_entropy += symbol_entropy[*symbol_ptr];
        bits_saved +=
            (node_instances - 1) * (num_symbols + num_extra_symbols - 1) < 0x400
                ? nfs_profit[(node_instances - 1) *
                             (num_symbols + num_extra_symbols - 1)]
                : num_file_symbols_p1_x_log_file_symbols_p1 -
                      ((double)(num_file_symbols + 1 -
                                ((node_instances - 1) *
                                 (num_symbols + num_extra_symbols - 1))) *
                       log2((double)(num_file_symbols + 1 -
                                     ((node_instances - 1) *
                                      (num_symbols + num_extra_symbols - 1)))));
        bits_saved += string_profit;

        // calculate score
        if (bits_saved > 0.0) {
          score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                  (profit_ratio_power * log2(repeats * string_entropy));
          double penalty;
          if (*symbol_ptr == 0x20) {
            if (*(symbol_ptr + 1) != 0x20) {
              score -= 2.0;
              penalty = 2.0;
            } else {
              score -= 1.0;
              penalty = 1.0;
            }
          } else if ((*symbol_ptr & 0xF2) != 0x42) {
            score -= 1.0;
            penalty = 1.0;
          } else {
            penalty = 0.0;
          }
          if (order_ratio == 0.0) {
            score += 40.0;
            if ((score > min_score) && (score > short_score)) {
              send_score = 0;
            }
          } else if ((score > (2.0 * min_score) - 98.0) ||
                     (score > min_score - 40.5)) {
            if (string_entropy2 == first_symbol_entropy) {
              symbol_ptr = start_symbol_ptr + node_ptr->last_match_index -
                           num_symbols + 2;
              while (symbol_ptr < end_symbol_ptr) {
                string_entropy2 +=
                    log2(((double)num_ends[symbol_ends[*(symbol_ptr - 1)].end] -
                          0.9 * repeats) *
                         (double)num_starts[symbol_ends[*symbol_ptr].start] /
                         (((double)o1c[symbol_ends[*(symbol_ptr - 1)].end]
                                      [symbol_ends[*symbol_ptr].start] -
                           0.9 * repeats) *
                          (double)symbol_counts[*symbol_ptr]));
                symbol_ptr++;
              }
            }
            string_entropy2 +=
                log2(((double)num_ends[symbol_ends[*(symbol_ptr - 1)].end] -
                      0.9 * repeats) *
                     (double)num_starts[symbol_ends[*symbol_ptr].start] /
                     (((double)o1c[symbol_ends[*(symbol_ptr - 1)].end]
                                  [symbol_ends[*symbol_ptr].start] -
                       0.9 * repeats) *
                      (double)symbol_counts[*symbol_ptr]));
            profit_per_substitution2 =
                node_instances <= NUM_PRECALCULATED_LOG2_X
                    ? string_entropy2 + log2_x[node_instances - 1] -
                          log_file_symbols
                    : string_entropy2 + log2(repeats) - log_file_symbols;
            bits_saved2 = (repeats * profit_per_substitution2) - new_rule_cost;
            if (bits_saved2 > 0.0) {
              score = (score * (1.0 - order_ratio)) +
                      (order_ratio *
                       (log2(bits_saved2) +
                        profit_ratio_power *
                            log2(profit_per_substitution2 / string_entropy2) -
                        penalty));
              score += 40.0;
              if ((score > min_score) && (score > short_score)) {
                send_score = 0;
              }
            }
          }
        }
      }
      if (send_score >= 0) {
        struct node *child_ptr = &nodes[node_ptr->child_node_num];
        if ((node_ptrs_num & 0xFFF) == 0) {
          while ((uint16_t)(node_ptrs_num -
                            atomic_load_explicit(&rank_scores_read_index,
                                                 memory_order_acquire)) >=
                 0xF000)
            ; // wait
        }
        rank_scores_buffer[node_ptrs_num].score = score;
        rank_scores_buffer[node_ptrs_num].num_symbols =
            num_symbols + num_extra_symbols - send_score;
        rank_scores_buffer[node_ptrs_num].last_match_index =
            child_ptr->last_match_index - 1 - send_score;
        rank_scores_buffer[node_ptrs_num].last_match_index2 =
            node_ptr->last_match_index + num_extra_symbols - send_score;
        if (rank_scores_buffer[node_ptrs_num].last_match_index ==
            rank_scores_buffer[node_ptrs_num].last_match_index2) {
          int32_t *sibling_node_num_ptr = &child_ptr->sibling_node_num[0];
          if (*sibling_node_num_ptr > 0) {
            rank_scores_buffer[node_ptrs_num].last_match_index =
                nodes[*sibling_node_num_ptr].last_match_index - 1 - send_score;
          } else if (*sibling_node_num_ptr != 0) {
            rank_scores_buffer[node_ptrs_num].last_match_index =
                *sibling_node_num_ptr + 0x7FFFFFFF - send_score;
          } else if (*(sibling_node_num_ptr + 1) > 0) {
            rank_scores_buffer[node_ptrs_num].last_match_index =
                nodes[*(sibling_node_num_ptr + 1)].last_match_index - 1 -
                send_score;
          } else if (*(sibling_node_num_ptr + 1) != 0) {
            rank_scores_buffer[node_ptrs_num].last_match_index =
                *(sibling_node_num_ptr + 1) + 0x7FFFFFFF - send_score;
          }
        }
        atomic_store_explicit(&rank_scores_write_index, ++node_ptrs_num,
                              memory_order_release);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes[node_ptr->child_node_num]; // move to child
    } else {
      int32_t sib_node_num = node_ptr->sibling_node_num[0];
      struct node *tnp = &nodes[sib_node_num];
      if ((sib_node_num > 0) &&
          ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
           (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy = string_entropy;
          node_data[level].string_profit = string_profit;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes[sib_node_num]; // move to sibling 0
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        tnp = &nodes[sib_node_num];
        if ((sib_node_num > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes[sib_node_num]; // move to sibling 1 - prior symbol
                                           // unchanged (okay)
        } else {
          if (level == 0) {
            *node_ptrs_num_ptr = node_ptrs_num;
            return;
          }
          string_entropy = node_data[--level].string_entropy; // pop stack
          string_profit = node_data[level].string_profit;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0) {
              node_data[level++].next_sibling = 1; // put sibling 1 on stack
            }
            node_ptr =
                &nodes[node_ptr->sibling_node_num[0]]; // move to sibling 0
          } else {
            node_ptr =
                &nodes[node_ptr->sibling_node_num[1]]; // move to sibling 1
          }
        }
      }
    }
  }
}

static void score_base_node_tree_cap_fast(
    struct node *node_ptr, struct score_data *node_data, float string_entropy,
    float production_cost, float profit_ratio_power,
    float log2_num_symbols_plus_substitution_cost, const float *new_symbol_cost,
    const float *symbol_entropy, struct node_score_data *rank_scores_buffer,
    uint16_t *node_ptrs_num_ptr) {
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  float profit_per_substitution;
  float bits_saved;

  while (1) {
    uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      float score;
      float repeats = (float)(node_instances - 1);
      node_data[level].string_entropy_f = string_entropy;
      uint32_t symbol = node_ptr->symbol;
      int8_t send_score = -1;
      uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      if (num_extra_symbols == 0) {
        string_entropy += symbol_entropy[symbol];
        // calculate score
        profit_per_substitution =
            node_instances < NUM_PRECALCULATED_SYMBOL_COSTS
                ? string_entropy - new_symbol_cost[node_instances]
                : string_entropy - (log2_num_symbols_plus_substitution_cost -
                                    log2f(repeats));
        bits_saved = (repeats * profit_per_substitution) - production_cost;
        if (bits_saved > min_score) {
          float profit_ratio = profit_per_substitution / string_entropy;
          score =
              log2f(bits_saved) + (profit_ratio_power * log2f(profit_ratio));
          if (symbol == 0x20) {
            score -= 0.25;
          } else if ((symbol & 0xF2) != 0x42) {
            score += 1.125;
          } else {
            score += 2.125;
          }
          if (score > min_score) {
            send_score = 0;
          }
        }
      } else {
        uint32_t *symbol_ptr = start_symbol_ptr + node_ptr->last_match_index;
        uint32_t *node_string_end_ptr = symbol_ptr + num_extra_symbols;
        if (node_string_end_ptr < end_symbol_ptr) {
          string_entropy += symbol_entropy[*symbol_ptr++];
          while (symbol_ptr < node_string_end_ptr) {
            string_entropy += symbol_entropy[*symbol_ptr++];
          }

          if (symbol_ptr < end_symbol_ptr) {
            if ((*symbol_ptr == 0x20) && (*(symbol_ptr - 1) != 0x20)) {
              // calculate score
              profit_per_substitution =
                  node_instances < NUM_PRECALCULATED_SYMBOL_COSTS
                      ? string_entropy - new_symbol_cost[node_instances]
                      : string_entropy -
                            (log2_num_symbols_plus_substitution_cost -
                             log2f(repeats));
              bits_saved =
                  (repeats * profit_per_substitution) - production_cost;
              if (bits_saved > min_score) {
                float profit_ratio = profit_per_substitution /
                                     (string_entropy + symbol_entropy[0x20]);
                score = log2f(bits_saved) +
                        (profit_ratio_power * log2f(profit_ratio)) + 1.125;
                if (score > min_score) {
                  send_score = 1;
                }
              }
            }

            string_entropy += symbol_entropy[*symbol_ptr];
            // calculate score
            if (send_score < 0) {
              profit_per_substitution =
                  node_instances < NUM_PRECALCULATED_SYMBOL_COSTS
                      ? string_entropy - new_symbol_cost[node_instances]
                      : string_entropy -
                            (log2_num_symbols_plus_substitution_cost -
                             log2f(repeats));
              bits_saved =
                  (repeats * profit_per_substitution) - production_cost;
              if (bits_saved > min_score) {
                float profit_ratio = profit_per_substitution / string_entropy;
                score = log2f(bits_saved) +
                        profit_ratio_power * log2f(profit_ratio);
                if (*symbol_ptr == 0x20) {
                  score -= 0.25;
                } else if (((*symbol_ptr) & 0xF2) != 0x42) {
                  score += 1.125;
                } else {
                  score += 2.125;
                }
                if (score > min_score) {
                  send_score = 0;
                }
              }
            }
          }
        }
      }
      if (send_score >= 0) {
        uint32_t new_score_lmi = node_ptr->last_match_index + num_extra_symbols;
        if ((node_ptrs_num & 0xFFF) == 0) {
          while ((uint16_t)(node_ptrs_num -
                            atomic_load_explicit(&rank_scores_read_index,
                                                 memory_order_acquire)) >=
                 0xF000)
            ; // wait
        }
        rank_scores_buffer[node_ptrs_num].score = score;
        rank_scores_buffer[node_ptrs_num].last_match_index =
            new_score_lmi - send_score;
        rank_scores_buffer[node_ptrs_num].num_symbols =
            num_symbols + num_extra_symbols - send_score;
        atomic_store_explicit(&rank_scores_write_index, ++node_ptrs_num,
                              memory_order_release);
      }
      if ((node_ptr->sibling_node_num[0] > 0) ||
          (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes[node_ptr->child_node_num];
    } else {
      int32_t sib_node_num = node_ptr->sibling_node_num[0];
      struct node *tnp = &nodes[sib_node_num];
      if ((sib_node_num > 0) &&
          ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
           (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy_f = string_entropy;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes[sib_node_num];
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        tnp = &nodes[sib_node_num];
        if ((sib_node_num > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes[sib_node_num];
        } else {
          if (level == 0) {
            *node_ptrs_num_ptr = node_ptrs_num;
            return;
          }
          string_entropy = node_data[--level].string_entropy_f;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0) {
              node_data[level++].next_sibling = 1;
            }
            node_ptr = &nodes[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

static void score_base_node_tree_words(
    struct node *node_ptr, struct score_data *node_data, float production_cost,
    float log2_num_symbols_plus_substitution_cost, const float *new_symbol_cost,
    const float *symbol_entropy, struct node_score_data *rank_scores_buffer,
    uint16_t *node_ptrs_num_ptr) {
  int32_t sib_node_num;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  float string_entropy = symbol_entropy[0x20];
  uint32_t stream_len = (uint32_t)(end_symbol_ptr - start_symbol_ptr);

  while (1) {
    uint32_t node_instances = node_ptr->instances;
    node_data[level].string_entropy = string_entropy;
    if (node_instances >= 2) {
      uint32_t num_extra_symbols = 0;
      uint32_t lmi = node_ptr->last_match_index;
      if (lmi >= stream_len) {
        assert(0 && "GST node last_match_index out of range");
        goto score_siblings;
      }
      while (num_extra_symbols != node_ptr->num_extra_symbols) {
        if (lmi + num_extra_symbols >= stream_len) {
          assert(0 && "GST node span exceeds grammar stream");
          goto score_siblings;
        }
        string_entropy +=
            symbol_entropy[*(start_symbol_ptr + lmi + num_extra_symbols++)];
      }
      if (lmi + num_extra_symbols >= stream_len) {
        assert(0 && "GST node span exceeds grammar stream");
        goto score_siblings;
      }
      if (*(start_symbol_ptr + lmi + num_extra_symbols) == 0x20) {
        // calculate score
        if (num_extra_symbols == 0) {
          goto score_siblings;
        }
        uint32_t last_symbol =
            *(start_symbol_ptr + lmi + num_extra_symbols - 1);
        if (((last_symbol >= (uint32_t)'a') &&
             (last_symbol <= (uint32_t)'z')) ||
            ((last_symbol >= (uint32_t)'0') &&
             (last_symbol <= (uint32_t)'9')) ||
            (last_symbol >= 0x80)) {
          float repeats = (float)(node_instances - 1);
          float profit_per_substitution;
          profit_per_substitution =
              node_instances < NUM_PRECALCULATED_SYMBOL_COSTS
                  ? string_entropy - new_symbol_cost[node_instances]
                  : string_entropy - (log2_num_symbols_plus_substitution_cost -
                                      log2f(repeats));
          if (profit_per_substitution >= 0.0) {
            float score = (repeats * profit_per_substitution) - production_cost;
            if (score > min_score) {
              if (node_ptrs_num >= 0xFFFE) {
                glza_warn_rank_buffer_limit();
                goto score_siblings;
              }
              if ((node_ptrs_num & 0xFFF) == 0) {
                while ((uint16_t)(node_ptrs_num -
                                  atomic_load_explicit(&rank_scores_read_index,
                                                       memory_order_acquire)) >=
                       0xF000)
                  ; // wait
              }
              rank_scores_buffer[node_ptrs_num].score = score;
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  node_ptr->last_match_index + num_extra_symbols - 1;
              rank_scores_buffer[node_ptrs_num].num_symbols =
                  num_symbols + num_extra_symbols - 1;
              atomic_store_explicit(&rank_scores_write_index, ++node_ptrs_num,
                                    memory_order_release);
            }
          }
        }
        goto score_siblings;
      }
      if (lmi + num_extra_symbols >= stream_len) {
        assert(0 && "GST node span exceeds grammar stream");
        goto score_siblings;
      }
      string_entropy +=
          symbol_entropy[*(start_symbol_ptr + lmi + num_extra_symbols)];
      if ((node_ptr->sibling_node_num[0] > 0) ||
          (node_ptr->sibling_node_num[1] > 0)) {
        if (level < NODE_DATA_STACK_DEPTH - 1) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level++].next_sibling =
              (node_ptr->sibling_node_num[0] <= 0);
        }
      }
      num_symbols += num_extra_symbols + 1;
      if ((uint32_t)node_ptr->child_node_num >= nodes_num_limit) {
        assert(0 && "GST child node index out of range");
        goto score_siblings;
      }
      node_ptr = &nodes[node_ptr->child_node_num];
    } else {
    score_siblings:
      sib_node_num = node_ptr->sibling_node_num[0];
      if (sib_node_num <= 0 || (uint32_t)sib_node_num >= nodes_num_limit) {
        sib_node_num = 0;
      }
      struct node *tnp = sib_node_num ? &nodes[sib_node_num] : node_ptr;
      if ((sib_node_num > 0) &&
          ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
           (tnp->sibling_node_num[1] > 0))) {
        tnp = node_ptr->sibling_node_num[1] > 0 &&
                      (uint32_t)node_ptr->sibling_node_num[1] < nodes_num_limit
                  ? &nodes[node_ptr->sibling_node_num[1]]
                  : node_ptr;
        if ((node_ptr->sibling_node_num[1] > 0) &&
            (uint32_t)node_ptr->sibling_node_num[1] < nodes_num_limit &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          if (level < NODE_DATA_STACK_DEPTH - 1) {
            node_data[level].node_ptr = node_ptr;
            node_data[level].num_symbols = num_symbols;
            node_data[level++].next_sibling = 1;
          }
        }
        node_ptr = &nodes[sib_node_num];
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        if (sib_node_num > 0 && (uint32_t)sib_node_num < nodes_num_limit) {
          node_ptr = &nodes[sib_node_num];
        } else {
          if (level == 0) {
            *node_ptrs_num_ptr = node_ptrs_num;
            return;
          }
          string_entropy = node_data[--level].string_entropy;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0 &&
                level < NODE_DATA_STACK_DEPTH - 1) {
              node_data[level++].next_sibling = 1;
            }
            if (node_ptr->sibling_node_num[0] > 0 &&
                (uint32_t)node_ptr->sibling_node_num[0] < nodes_num_limit) {
              node_ptr = &nodes[node_ptr->sibling_node_num[0]];
            }
          } else if (node_ptr->sibling_node_num[1] > 0 &&
                     (uint32_t)node_ptr->sibling_node_num[1] <
                         nodes_num_limit) {
            node_ptr = &nodes[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

static void score_symbol_tree(uint32_t min_symbol, uint32_t max_symbol,
                              struct node_score_data *rank_scores_buffer,
                              struct score_data *node_data,
                              uint16_t *node_ptrs_num_ptr,
                              double profit_ratio_power, double *symbol_entropy,
                              const uint32_t *symbol_counts) {
  int32_t *next_base_node_child_num_ptr;
  int32_t *base_node_child_num_ptr =
      &base_nodes_child_node_num[min_symbol * BASE_NODES_CHILD_ARRAY_SIZE];
  uint32_t symbol = min_symbol;
  while (symbol <= max_symbol) {
    if (symbol_counts[symbol] > 1) {
      next_base_node_child_num_ptr =
          base_node_child_num_ptr + BASE_NODES_CHILD_ARRAY_SIZE;
      do {
        if (*base_node_child_num_ptr > 0) {
          if (cap_encoded != 0) {
            score_base_node_tree_cap(
                &nodes[*base_node_child_num_ptr], node_data, profit_ratio_power,
                symbol_entropy, rank_scores_buffer, node_ptrs_num_ptr, symbol);
          } else {
            score_base_node_tree(&nodes[*base_node_child_num_ptr], node_data,
                                 profit_ratio_power, symbol_entropy,
                                 rank_scores_buffer, node_ptrs_num_ptr, symbol);
          }
        }
        base_node_child_num_ptr++;
      } while (base_node_child_num_ptr != next_base_node_child_num_ptr);
    } else {
      base_node_child_num_ptr += 16;
    }
    symbol++;
  }
}

static void score_symbol_tree_fast(
    uint32_t min_symbol, uint32_t max_symbol,
    struct node_score_data *rank_scores_buffer, struct score_data *node_data,
    uint16_t *node_ptrs_num_ptr, float production_cost,
    double profit_ratio_power, float log2_num_symbols_plus_substitution_cost,
    float *new_symbol_cost, float *symbol_entropy,
    const uint32_t *symbol_counts) {
  int32_t *next_base_node_child_num_ptr;
  int32_t *base_node_child_num_ptr =
      &base_nodes_child_node_num[min_symbol * BASE_NODES_CHILD_ARRAY_SIZE];
  uint32_t symbol = min_symbol;
  while (symbol <= max_symbol) {
    if (symbol_counts[symbol] > 1) {
      next_base_node_child_num_ptr =
          base_node_child_num_ptr + BASE_NODES_CHILD_ARRAY_SIZE;
      do {
        if (*base_node_child_num_ptr > 0) {
          if (cap_encoded != 0) {
            score_base_node_tree_cap_fast(
                &nodes[*base_node_child_num_ptr], node_data,
                symbol_entropy[symbol], production_cost,
                (float)profit_ratio_power,
                log2_num_symbols_plus_substitution_cost, new_symbol_cost,
                symbol_entropy, rank_scores_buffer, node_ptrs_num_ptr);
          } else {
            score_base_node_tree_fast(
                &nodes[*base_node_child_num_ptr], node_data,
                symbol_entropy[symbol], production_cost,
                (float)profit_ratio_power,
                log2_num_symbols_plus_substitution_cost, new_symbol_cost,
                symbol_entropy, rank_scores_buffer, node_ptrs_num_ptr);
          }
        }
        base_node_child_num_ptr++;
      } while (base_node_child_num_ptr != next_base_node_child_num_ptr);
    } else {
      base_node_child_num_ptr += 16;
    }
    symbol++;
  }
}

static void
score_symbol_tree_words(struct node_score_data *rank_scores_buffer,
                        struct score_data *node_data,
                        uint16_t *node_ptrs_num_ptr, float production_cost,
                        float log2_num_symbols_plus_substitution_cost,
                        float *new_symbol_cost, float *symbol_entropy) {
  int32_t *base_node_child_num_ptr = &base_nodes_child_node_num[0];
  int32_t *base_node_child_num_end_ptr = &base_nodes_child_node_num[0x90];
  do {
    if (*base_node_child_num_ptr > 0) {
      score_base_node_tree_words(
          &nodes[*base_node_child_num_ptr], node_data, production_cost,
          log2_num_symbols_plus_substitution_cost, new_symbol_cost,
          symbol_entropy, rank_scores_buffer, node_ptrs_num_ptr);
    }
  } while (++base_node_child_num_ptr <= base_node_child_num_end_ptr);
}

static void overlap_check_invalidate_overlap(
    struct overlap_check *thread_data_ptr, uint8_t *candidate_bad,
    uint32_t *num_overlaps, uint32_t prior_score, uint32_t node_score_number) {
  if (fast_mode == 0) {
    // candidate_bad[std::max(prior_score, node_score_number)] = 1;
    if (prior_score > node_score_number) {
      candidate_bad[prior_score] = 1;
    } else {
      candidate_bad[node_score_number] = 1;
    }
  } else {
    uint32_t low_score;
    uint32_t high_score;
    // auto [low_score, high_score] = std::minmax(prior_score,
    // node_score_number);
    if (node_score_number < prior_score) {
      low_score = node_score_number;
      high_score = prior_score;
    } else {
      low_score = prior_score;
      high_score = node_score_number;
    }

    int32_t *next_overlap_num_ptr = &thread_data_ptr->next[low_score];
    while ((*next_overlap_num_ptr != -1) &&
           (thread_data_ptr->second[*next_overlap_num_ptr] < high_score)) {
      next_overlap_num_ptr = &thread_data_ptr->next[*next_overlap_num_ptr];
    }
    if ((*next_overlap_num_ptr == -1) ||
        (thread_data_ptr->second[*next_overlap_num_ptr] != high_score)) {
      if (*num_overlaps < 150000) {
        thread_data_ptr->second[*num_overlaps] = high_score;
        thread_data_ptr->next[*num_overlaps] = *next_overlap_num_ptr;
        *next_overlap_num_ptr = (*num_overlaps)++;
      } else {
        candidate_bad[high_score] = 1;
      }
    }
  }
}

static void overlap_check_handle_leaf_match(
    struct overlap_check *thread_data_ptr, struct match_node *match_node_ptr,
    uint32_t *in_symbol_ptr, uint8_t *candidate_bad, uint32_t *num_overlaps,
    uint32_t *prior_match_score_number, uint32_t **prior_match_end_ptr,
    uint32_t *num_prior_matches) {
  uint32_t node_score_number = match_node_ptr->score_number;
  if ((in_symbol_ptr - match_node_ptr->num_symbols <
       thread_data_ptr->stop_matches_symbol_ptr) &&
      (candidate_bad[node_score_number] == 0) &&
      (*thread_data_ptr->next_match_ptr_ptr + 2 <=
       thread_data_ptr->match_stop_ptr)) {
    **thread_data_ptr->next_match_ptr_ptr = node_score_number;
    (*thread_data_ptr->next_match_ptr_ptr)++;
    **thread_data_ptr->next_match_ptr_ptr =
        in_symbol_ptr - start_symbol_ptr - match_node_ptr->num_symbols;
    (*thread_data_ptr->next_match_ptr_ptr)++;
  }
  if ((*num_prior_matches != 0) &&
      (in_symbol_ptr - match_node_ptr->num_symbols <=
       prior_match_end_ptr[*num_prior_matches - 1])) {
    if (*num_prior_matches == 1) {
      if (prior_match_score_number[0] != node_score_number) {
        overlap_check_invalidate_overlap(
            thread_data_ptr, candidate_bad, num_overlaps,
            prior_match_score_number[0], node_score_number);
        prior_match_end_ptr[1] = in_symbol_ptr - 1;
        prior_match_score_number[1] = node_score_number;
        *num_prior_matches = 2;
      }
    } else {
      uint32_t prior_match_number = 0;
      uint8_t found_same_score_prior_match = 0;
      do {
        if (in_symbol_ptr - match_node_ptr->num_symbols >
            prior_match_end_ptr[prior_match_number]) {
          (*num_prior_matches)--;
          for (size_t i = prior_match_number; i < *num_prior_matches; i++) {
            prior_match_end_ptr[i] = prior_match_end_ptr[i + 1];
            prior_match_score_number[i] = prior_match_score_number[i + 1];
          }
        } else { // overlapping candidates - invalidate the lower score
          if (prior_match_score_number[prior_match_number] ==
              node_score_number) {
            found_same_score_prior_match = 1;
          } else {
            overlap_check_invalidate_overlap(
                thread_data_ptr, candidate_bad, num_overlaps,
                prior_match_score_number[prior_match_number],
                node_score_number);
          }
          prior_match_number++;
        }
      } while (prior_match_number < *num_prior_matches);
      if (found_same_score_prior_match == 0) {
        prior_match_end_ptr[*num_prior_matches] = in_symbol_ptr - 1;
        prior_match_score_number[(*num_prior_matches)++] = node_score_number;
      }
    }
  } else {
    *num_prior_matches = 1;
    prior_match_end_ptr[0] = in_symbol_ptr - 1;
    prior_match_score_number[0] = node_score_number;
  }
}

static void *overlap_check_thread(void *arg) {
  struct overlap_check *thread_data_ptr = (struct overlap_check *)arg;
  struct match_node *match_nodes = thread_data_ptr->match_nodes;
  struct match_node *match_node_ptr;
  uint32_t *in_symbol_ptr = thread_data_ptr->start_symbol_ptr;
  uint32_t *end_symbol_ptr = thread_data_ptr->stop_symbol_ptr;
  uint8_t *candidate_bad = thread_data_ptr->candidate_bad;
  uint32_t num_overlaps = thread_data_ptr->num_overlaps;
  uint32_t symbol;
  uint32_t prior_match_score_number[MAX_PRIOR_MATCHES];
  uint32_t *prior_match_end_ptr[MAX_PRIOR_MATCHES];
  uint32_t num_prior_matches = 0;

  for (symbol = 0; symbol < num_overlaps; symbol++) {
    thread_data_ptr->next[symbol] = -1;
  }

thread_overlap_check_loop_no_match:
  symbol = *in_symbol_ptr++;
  if (in_symbol_ptr >= end_symbol_ptr) {
    return 0;
  }
  if (((int32_t)symbol < 0) || (symbol >= child_ptr_array_size) ||
      (child_ptr_array[symbol] == 0)) {
    goto thread_overlap_check_loop_no_match;
  }
  match_node_ptr = child_ptr_array[symbol];
thread_overlap_check_loop_match:
  symbol = *in_symbol_ptr++;
  if (symbol != match_node_ptr->symbol) {
    uint32_t shifted_symbol = symbol;
    do {
      if (match_node_ptr->sibling_node_num[shifted_symbol & 0xF] != 0) {
        match_node_ptr =
            &match_nodes[match_node_ptr
                             ->sibling_node_num[shifted_symbol & 0xF]];
        shifted_symbol >>= 4;
      } else {
        if (match_node_ptr->miss_ptr == 0) {
          if (((int32_t)symbol < 0) || (symbol >= child_ptr_array_size) ||
              (child_ptr_array[symbol] == 0)) {
            goto thread_overlap_check_loop_no_match;
          }
          match_node_ptr = child_ptr_array[symbol];
          goto thread_overlap_check_loop_match;
        } else {
          match_node_ptr = match_node_ptr->miss_ptr;
          shifted_symbol = symbol;
        }
      }
    } while (symbol != match_node_ptr->symbol);
  }
  if (match_node_ptr->child_ptr != 0) {
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_loop_match;
  }

  overlap_check_handle_leaf_match(thread_data_ptr, match_node_ptr,
                                  in_symbol_ptr, candidate_bad, &num_overlaps,
                                  prior_match_score_number, prior_match_end_ptr,
                                  &num_prior_matches);
  match_node_ptr = match_node_ptr->hit_ptr;

  if (match_node_ptr == 0) {
    if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
        child_ptr_array[symbol] == 0) {
      goto thread_overlap_check_loop_no_match;
    }
    match_node_ptr = child_ptr_array[symbol];
    goto thread_overlap_check_loop_match;
  }
  match_node_ptr = match_node_ptr->child_ptr;
  goto thread_overlap_check_loop_match;
}

static void *overlap_check_no_defs_thread(void *arg) {
  struct overlap_check *thread_data_ptr = (struct overlap_check *)arg;
  struct match_node *match_nodes = thread_data_ptr->match_nodes;
  struct match_node *match_node_ptr;
  uint32_t *in_symbol_ptr = thread_data_ptr->start_symbol_ptr;
  uint32_t *end_symbol_ptr = thread_data_ptr->stop_symbol_ptr;
  uint8_t *candidate_bad = thread_data_ptr->candidate_bad;
  uint32_t num_overlaps = thread_data_ptr->num_overlaps;
  uint32_t symbol;
  uint32_t prior_match_score_number[MAX_PRIOR_MATCHES];
  uint32_t *prior_match_end_ptr[MAX_PRIOR_MATCHES];
  uint32_t num_prior_matches = 0;

  for (symbol = 0; symbol < num_overlaps; symbol++) {
    thread_data_ptr->next[symbol] = -1;
  }

thread_overlap_check_no_defs_loop_no_match:
  symbol = *in_symbol_ptr++;
  if (in_symbol_ptr >= end_symbol_ptr) {
    return 0;
  }
  if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
      child_ptr_array[symbol] == 0) {
    goto thread_overlap_check_no_defs_loop_no_match;
  }
  match_node_ptr = child_ptr_array[symbol];
thread_overlap_check_no_defs_loop_match:
  symbol = *in_symbol_ptr++;
  if (symbol != match_node_ptr->symbol) {
    uint32_t shifted_symbol = symbol;
    do {
      if (match_node_ptr->sibling_node_num[shifted_symbol & 0xF] != 0) {
        match_node_ptr =
            &match_nodes[match_node_ptr
                             ->sibling_node_num[shifted_symbol & 0xF]];
        shifted_symbol >>= 4;
      } else {
        if (match_node_ptr->miss_ptr == 0) {
          if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
              child_ptr_array[symbol] == 0) {
            goto thread_overlap_check_no_defs_loop_no_match;
          }
          // normal EOS while restarting from root:
          // one symbol was consumed above, so the cursor may sit one past
          // stop_symbol_ptr
          if (in_symbol_ptr > end_symbol_ptr) {
            return 0;
          }
          match_node_ptr = child_ptr_array[symbol];
          goto thread_overlap_check_no_defs_loop_match;
        } else {
          match_node_ptr = match_node_ptr->miss_ptr;
          shifted_symbol = symbol;
        }
      }
    } while (symbol != match_node_ptr->symbol);
  }
  if (match_node_ptr->child_ptr != 0) {
    /* Same stream-end tolerance as above: allow a trailing partial match. */
    if (in_symbol_ptr > end_symbol_ptr) {
      if (in_symbol_ptr - match_node_ptr->num_symbols >= end_symbol_ptr) {
        return 0;
      }
    }
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_no_defs_loop_match;
  }

  overlap_check_handle_leaf_match(thread_data_ptr, match_node_ptr,
                                  in_symbol_ptr, candidate_bad, &num_overlaps,
                                  prior_match_score_number, prior_match_end_ptr,
                                  &num_prior_matches);
  match_node_ptr = match_node_ptr->hit_ptr;
  if (match_node_ptr == 0) {
    if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
        child_ptr_array[symbol] == 0) {
      goto thread_overlap_check_no_defs_loop_no_match;
    }
    match_node_ptr = child_ptr_array[symbol];
    goto thread_overlap_check_no_defs_loop_match;
  }
  if ((in_symbol_ptr <= end_symbol_ptr) ||
      (in_symbol_ptr - match_node_ptr->num_symbols < end_symbol_ptr)) {
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_no_defs_loop_match;
  }
  return 0;
}

static void *find_substitutions_thread(void *arg) {
  struct find_substitutions_thread_data *thread_data_ptr =
      (struct find_substitutions_thread_data *)arg;
  struct match_node *match_nodes = thread_data_ptr->match_nodes;
  struct match_node *match_node_ptr;
  uint32_t *in_symbol_ptr = thread_data_ptr->start_symbol_ptr;
  uint32_t *previous_in_symbol_ptr = in_symbol_ptr;
  uint32_t *end_symbol_ptr = thread_data_ptr->stop_symbol_ptr;
  uint32_t symbol;
  uint32_t substitute_index = 0;
  uint32_t local_read_index = 0;

  thread_data_ptr->extra_match_symbols = 0;
thread_symbol_substitution_loop_top:
  symbol = *in_symbol_ptr++;
  if (symbol == 0x20) {
    match_node_ptr = child_ptr_array[0];
    symbol = *in_symbol_ptr++;
    if ((int32_t)symbol < 0) {
      if (in_symbol_ptr < end_symbol_ptr) {
        goto thread_symbol_substitution_loop_top;
      }
      goto thread_symbol_substitution_loop_end;
    } else {
    thread_symbol_substitution_loop_match_search:
      if (symbol != match_node_ptr->symbol) {
        uint32_t sibling_nibble = symbol;
        do {
          if (match_node_ptr->sibling_node_num[sibling_nibble & 0xF] != 0) {
            match_node_ptr =
                &match_nodes[match_node_ptr
                                 ->sibling_node_num[sibling_nibble & 0xF]];
            sibling_nibble = sibling_nibble >> 4;
          } else { // no match, so use miss node and output missed symbols
            if (match_node_ptr->miss_ptr == 0) {
              if (symbol == 0x20) {
                if (in_symbol_ptr > end_symbol_ptr) {
                  goto thread_symbol_substitution_loop_end;
                }
                if ((int32_t)*in_symbol_ptr >= 0) {
                  match_node_ptr = child_ptr_array[0];
                  symbol = *in_symbol_ptr++;
                  goto thread_symbol_substitution_loop_match_search;
                }
                if (++in_symbol_ptr < end_symbol_ptr) {
                  goto thread_symbol_substitution_loop_top;
                }
                goto thread_symbol_substitution_loop_end;
              }
              if (in_symbol_ptr < end_symbol_ptr) {
                goto thread_symbol_substitution_loop_top;
              }
              goto thread_symbol_substitution_loop_end;
            }
            if ((in_symbol_ptr > end_symbol_ptr) &&
                (in_symbol_ptr - match_node_ptr->miss_ptr->num_symbols >=
                 end_symbol_ptr)) {
              goto thread_symbol_substitution_loop_end;
            }
            match_node_ptr = match_node_ptr->miss_ptr;
            sibling_nibble = symbol;
          }
        } while (symbol != match_node_ptr->symbol);
      }
      if (match_node_ptr->child_ptr != 0) {
        symbol = *in_symbol_ptr++;
        if ((int32_t)symbol >= 0) {
          match_node_ptr = match_node_ptr->child_ptr;
          goto thread_symbol_substitution_loop_match_search;
        } else {
          goto thread_symbol_substitution_loop_match_no_match;
        }
      }
      // found a match
      while ((((substitute_index - local_read_index) & 0x7FFFFC) == 0x7FFFFC) &&
             (((substitute_index -
                (local_read_index = atomic_load_explicit(
                     &thread_data_ptr->read_index, memory_order_acquire))) &
               0x7FFFFC) == 0x7FFFFC)) {
        sched_yield();
      }
      if (in_symbol_ptr - previous_in_symbol_ptr -
              match_node_ptr->num_symbols !=
          0) {
        thread_data_ptr->data[substitute_index] = in_symbol_ptr -
                                                  previous_in_symbol_ptr -
                                                  match_node_ptr->num_symbols;
        substitute_index = (substitute_index + 1) & 0x7FFFFF;
      }
      thread_data_ptr->data[substitute_index] =
          0x80000000 + match_node_ptr->num_symbols;
      substitute_index = (substitute_index + 1) & 0x7FFFFF;
      thread_data_ptr->data[substitute_index] = match_node_ptr->score_number;
      substitute_index = (substitute_index + 1) & 0x7FFFFF;
      atomic_store_explicit(&thread_data_ptr->write_index, substitute_index,
                            memory_order_release);
      previous_in_symbol_ptr = in_symbol_ptr;
      if (in_symbol_ptr < end_symbol_ptr) {
        goto thread_symbol_substitution_loop_top;
      }
      thread_data_ptr->extra_match_symbols = in_symbol_ptr - end_symbol_ptr;
      goto thread_symbol_substitution_loop_end2;
    }
  thread_symbol_substitution_loop_match_no_match:
    if (in_symbol_ptr < end_symbol_ptr) {
      goto thread_symbol_substitution_loop_top;
    }
    goto thread_symbol_substitution_loop_end;
  }
  if (in_symbol_ptr < end_symbol_ptr) {
    goto thread_symbol_substitution_loop_top;
  }

thread_symbol_substitution_loop_end:
  while ((((substitute_index - local_read_index) & 0x7FFFFF) == 0x7FFFFF) &&
         (((substitute_index -
            (local_read_index = atomic_load_explicit(
                 &thread_data_ptr->read_index, memory_order_acquire))) &
           0x7FFFFF) == 0x7FFFFF)) {
    sched_yield();
  }
  thread_data_ptr->data[substitute_index] =
      end_symbol_ptr - previous_in_symbol_ptr;
  substitute_index = (substitute_index + 1) & 0x7FFFFF;
  atomic_store_explicit(&thread_data_ptr->write_index, substitute_index,
                        memory_order_release);
thread_symbol_substitution_loop_end2:
  atomic_store_explicit(&thread_data_ptr->done, 1, memory_order_relaxed);
  return 0;
}

static void *substitute_thread(void *arg) {
  struct substitute_thread_data *thread_data_ptr =
      (struct substitute_thread_data *)arg;
  uint32_t data;
  uint32_t local_write_index;
  uint32_t substitute_data_index = 0;

  thread_data_ptr->out_symbol_ptr = thread_data_ptr->in_symbol_ptr;
  while (1) {
    while ((local_write_index = atomic_load_explicit(
                &substitute_data_write_index, memory_order_relaxed)) ==
           substitute_data_index)
      ; // wait
    do {
      if ((int32_t)(data = thread_data_ptr
                               ->substitute_data[substitute_data_index++]) >=
          0) {
        memmove(thread_data_ptr->out_symbol_ptr, thread_data_ptr->in_symbol_ptr,
                data * 4);
        thread_data_ptr->in_symbol_ptr += data;
        thread_data_ptr->out_symbol_ptr += data;
      } else if (data != 0xFFFFFFFF) {
        thread_data_ptr->in_symbol_ptr += (size_t)(data + 0x80000000);
        uint32_t symbol =
            thread_data_ptr->substitute_data[substitute_data_index++];
        if (symbol > thread_data_ptr->max_rule_symbol) {
          fprintf(stderr,
                  "GLZA compress: substitute_thread symbol %u > "
                  "max_rule_symbol %u\n",
                  (unsigned int)symbol,
                  (unsigned int)thread_data_ptr->max_rule_symbol);
          GLZA_DIE("ERROR - substitute_thread grammar corruption\n");
        }
        *thread_data_ptr->out_symbol_ptr++ = symbol;
        thread_data_ptr->symbol_counts[symbol]++;
      } else {
        return 0;
      }
      atomic_store_explicit(&substitute_data_read_index, substitute_data_index,
                            memory_order_relaxed);
    } while (local_write_index != substitute_data_index);
  }
}

static uint32_t stca_setup(const size_t thread_count,
                           const uint8_t thread_symbol_limit[],
                           const uint32_t thread_first_node_num[],
                           const uint32_t thread_nodes_limit[],
                           uint32_t num_rules, uint32_t next_new_symbol_number,
                           struct tree_thread_data tree_thread_data[13],
                           uint32_t *start_cycle_symbol_ptr) {
  uint32_t symbols_div_100 = (num_file_symbols - num_rules) / 100;
  uint32_t sum_symbols = symbol_counts[0];
  size_t i = 1;

  uint32_t main_max_symbol;
  for (size_t j = 0; j < thread_count; ++j) {
    uint32_t symbols_limit = symbols_div_100 * thread_symbol_limit[j];
    while (sum_symbols < symbols_limit && i < next_new_symbol_number) {
      sum_symbols += symbol_counts[i++];
    }
    if (j > 0) {
      tree_thread_data[j - 1].max_symbol = i - 1;
    } else {
      main_max_symbol = i - 1;
    }
    tree_thread_data[j].min_symbol = i;
    if (i < next_new_symbol_number - 1 && j < thread_count - 1) {
      sum_symbols += symbol_counts[i++];
    }
  }

  tree_thread_data[thread_count - 1].max_symbol = next_new_symbol_number - 1;

  for (size_t j = 0; j < thread_count; j++) {
    tree_thread_data[j].start_cycle_symbol_ptr = start_cycle_symbol_ptr;
    tree_thread_data[j].base_nodes_child_node_num = base_nodes_child_node_num;
    tree_thread_data[j].first_node_num = thread_first_node_num[j];
    tree_thread_data[j].nodes_limit = thread_nodes_limit[j];
  }

  return main_max_symbol;
}

static float
update_cycle_start_ratio(uint8_t fast_mode, float cycle_start_ratio,
                         float cycle_end_ratio, uint8_t fast_section,
                         uint8_t fast_sections, uint32_t prior_cycle_symbols) {
  if (fast_mode == 1) {
    return (float)fast_section / (float)fast_sections;
  }

  if (cycle_start_ratio == 0.0) {
    if (cycle_end_ratio < 1.0) {
      if (cycle_end_ratio > 0.5) {
        return 1.0 - (0.99 * cycle_end_ratio);
      }
      return cycle_end_ratio;
    }
    // __jm__ author missed this branch
    return cycle_start_ratio;
  }

  if ((cycle_end_ratio >= 0.99) || (prior_cycle_symbols >= num_file_symbols) ||
      (1.5 * (1.0 - cycle_end_ratio) <= cycle_end_ratio - cycle_start_ratio)) {
    return 0.0;
  }
  if ((uint32_t)((1.0 - cycle_end_ratio) * (float)num_file_symbols) >=
      prior_cycle_symbols) {
    return cycle_end_ratio;
  }
  return 1.0 - (0.97 * (cycle_end_ratio - cycle_start_ratio));
}

static void
main_loop_init(uint32_t *out_next_new_symbol_number, uint32_t num_rules,
               double *out_d_num_file_symbols, uint8_t **out_free_RAM_ptr,
               double **out_symbol_entropy, float **out_symbol_entropy_f,
               enum glza_scan_mode scan_mode,
               float *ptr_log2_num_symbols_plus_substitution_cost,
               float new_symbol_cost[NUM_PRECALCULATED_SYMBOL_COSTS],
               float *ptr_production_cost, uint32_t num_terminals_used,
               uint16_t *ptr_scan_cycle, const uint8_t *end_RAM_ptr,
               uint32_t *out_node_num_limit) {
  uint32_t next_new_symbol_number = num_terminals + num_rules;
  child_ptr_array_size = next_new_symbol_number;
  double d_num_file_symbols = (double)num_file_symbols;
  log_file_symbols = log2(d_num_file_symbols);
  uint8_t *free_RAM_ptr = (char *)(((size_t)end_symbol_ptr + 8) & ~7);
  /* Both pointers start at the same address: double[num_symbols] then
   * float[num_symbols] when scan_mode==0 && fast_mode==0. fast_mode==0 uses
   * symbol_entropy (double) for score_symbol_tree; fast_mode==1 uses
   * symbol_entropy_f only. After init, free_RAM_ptr advances past the float
   * table — do not treat these as long-lived aliases. */
  double *symbol_entropy = (double *)free_RAM_ptr;
  float *symbol_entropy_f = (float *)free_RAM_ptr;

  /* GLZA_SCAN_WORDS: reserve one float table; later slow-path passes reserve
   * two. */
  free_RAM_ptr += (1 + ((scan_mode != GLZA_SCAN_WORDS) & (fast_mode == 0))) *
                  sizeof(float) * (size_t)next_new_symbol_number;
  if ((scan_mode != GLZA_SCAN_WORDS) && (fast_mode == 0)) {
    /* slow path once past word pass: NFS profit tables */
    num_file_symbols_p1_x_log_file_symbols_p1 = xlogx(num_file_symbols + 1);
    for (size_t i = 1; i < NUM_PRECALCULATED_NFSMR_LOGS; i++) {
      double tmp = num_file_symbols - i + 1;
      nfs_profit[i] = num_file_symbols_p1_x_log_file_symbols_p1 - xlogx(tmp);
    }
    new_rule_cost =
        num_file_symbols_p1_x_log_file_symbols_p1 - xlogx(d_num_file_symbols) +
        1.0 + (num_rules == 0 ? 0 : xlogx(num_rules) - xlogx(num_rules + 1));
  } else {
    /* scan_mode==0 (word pass) or fast_mode==1: repeat/substitution cost tables
     * for score_symbol_tree_words / _fast */
    float log2_num_symbols_plus_substitution_cost =
        (float)log_file_symbols + 1.4;
    for (size_t i = 2; i < NUM_PRECALCULATED_SYMBOL_COSTS; i++) {
      new_symbol_cost[i] = log2_num_symbols_plus_substitution_cost -
                           (float)log2_x[i - 1]; // -1 for repeats only
    }
    *ptr_production_cost =
        scan_mode == GLZA_SCAN_WORDS
            ? log2f((float)d_num_file_symbols / (float)num_terminals_used) + 1.2
            : log2f((float)d_num_file_symbols / (float)(num_rules + 1)) + 1.2;
    *ptr_log2_num_symbols_plus_substitution_cost =
        log2_num_symbols_plus_substitution_cost;
  }

  uint16_t scan_cycle = *ptr_scan_cycle;
  if (fast_mode == 0) {
    double order_0_entropy = 0.0;
    {
      size_t i = 0;
      do {
        if (symbol_counts[i] != 0) {
          symbol_entropy[i] =
              log_file_symbols - (symbol_counts[i] < NUM_PRECALCULATED_LOG2_X
                                      ? log2_x[symbol_counts[i]]
                                      : log2((double)symbol_counts[i]));
          order_0_entropy += symbol_entropy[i] * (double)symbol_counts[i];
        }
      } while (++i < next_new_symbol_number);
    }
    if (scan_mode == GLZA_SCAN_WORDS) {
      /* word pass only: mirror double entropies into symbol_entropy_f for
       * score_symbol_tree_words */
      size_t i = 0;
      do {
        if (symbol_counts[i] != 0) {
          symbol_entropy_f[i] = (float)symbol_entropy[i];
        }
      } while (++i < next_new_symbol_number);
    }
    if (num_rules != 0) {
      order_0_entropy += (double)(num_rules + 1) *
                         (log_file_symbols - log2((double)num_rules));
    }
    // __jm__ why does one printf log num_rules and the other num_rules+1? seems
    // like a bug
#ifdef PRINTON
    fprintf(stderr,
            "PASS %u: grammar size: %u, %u production rules, %.4f bits/sym, "
            "o0e %u bytes\n",
            (unsigned int)++scan_cycle, (unsigned int)num_file_symbols + 1,
            (unsigned int)num_rules,
            (float)(order_0_entropy / d_num_file_symbols),
            (unsigned int)(order_0_entropy * 0.125));
#endif
  } else {
#ifdef PRINTON
    fprintf(stderr, "PASS %u: grammar size: %u, %u production rules\r",
            (unsigned int)++scan_cycle, (unsigned int)num_file_symbols + 1,
            (unsigned int)num_rules + 1);
#endif
  }

  // Set the memory adddress for the suffix tree nodes
  base_nodes_child_node_num = (int32_t *)free_RAM_ptr;
  nodes = (struct node *)((size_t)free_RAM_ptr +
                          (sizeof(int32_t) * (size_t)next_new_symbol_number *
                           BASE_NODES_CHILD_ARRAY_SIZE));
  {
    size_t nodes_room = (size_t)end_RAM_ptr - (size_t)nodes;
    if (nodes >= (struct node *)end_RAM_ptr ||
        nodes_room < sizeof(struct node)) {
      fprintf(stderr, "ERROR - Insufficient RAM for suffix tree nodes\n");
      exit(1);
    }
    nodes_num_limit = (uint32_t)(nodes_room / sizeof(struct node));
  }

  *ptr_scan_cycle = scan_cycle;
  *out_node_num_limit = nodes_num_limit;
  *out_next_new_symbol_number = next_new_symbol_number;
  *out_d_num_file_symbols = d_num_file_symbols;
  *out_free_RAM_ptr = free_RAM_ptr;
  *out_symbol_entropy = symbol_entropy;
  *out_symbol_entropy_f = symbol_entropy_f;
}

static void scan_mode0(
    int32_t **out_base_node_child_num_ptr, uint32_t *out_next_node_num,
    uint32_t **out_in_symbol_ptr, uint8_t UTF8_compliant,
    uint32_t node_num_limit, // limit on the size (# of nodes) of the GST
    float *symbol_entropy_f, uint32_t *p_next_new_symbol_number,
    uint16_t *out_node_ptrs_num,
    struct rank_scores_thread_data *rank_scores_data_ptr, uint16_t max_scores,
    uint32_t *out_prior_cycle_symbols, double order_0_entropy,
    double d_num_file_symbols, uint16_t *out_num_candidates,
    size_t *ptr_substitute_heap_size, const uint32_t max_rules,
    uint16_t **ptr_candidates_index, uint8_t **ptr_substitute_heap_buf,
    uint8_t **out_free_RAM_ptr, uint32_t **out_substitute_data,
    struct substitute_thread_data *ptr_substitute_thread_data,
    const uintptr_t end_RAM_ptr, uint32_t *out_num_match_nodes,
    uint32_t *out_max_match_length, uint32_t **out_match_strings,
    struct overlap_check **ptr_overlap_check_heap_buf,
    uint8_t **ptr_candidate_bad, uint32_t *ptr_num_rules,
    struct find_substitutions_thread_data *
        *ptr_find_substitutions_thread_data_buf,
    struct find_substitutions_thread_data **ptr_find_substitutions_thread_data,
    pthread_t *p_rank_scores_thread1, struct score_data *node_data,
    float production_cost, float log2_num_symbols_plus_substitution_cost,
    float new_symbol_cost[NUM_PRECALCULATED_SYMBOL_COSTS],
    uint32_t *ptr_first_define_index,
    struct overlap_check **out_overlap_check_data) {
  // build the words suffix tree (single-threaded for correctness on ARM)
  int32_t *base_node_child_num_ptr = &base_nodes_child_node_num[0];
  while (base_node_child_num_ptr <= base_nodes_child_node_num + 0x90) {
    *base_node_child_num_ptr++ = 0;
  }

  uint32_t next_node_num = 1; // Supposed to track the size of the GST
  uint8_t word_start[0x80];   /* ASCII-ish set marking symbols that may start a
                                 word after space (0x20) */
  for (size_t i = 0; i < 0x80; i++) {
    word_start[i] = 0;
  }
  for (size_t i = 'a'; i <= 'z'; i++) {
    word_start[i] = 1;
  }
  for (size_t i = '0'; i <= '9'; i++) {
    word_start[i] = 1;
  }
  word_start['$'] = 1;

  uint32_t *in_symbol_ptr =
      start_symbol_ptr; // cursor over current compressed input
  {
    uint32_t symbol;
    while (in_symbol_ptr < end_symbol_ptr) {
      symbol = *in_symbol_ptr++;
      assert(symbol != 0xFFFFFFFE &&
             "compressed input must not contain EOF sentinel");
      if (symbol != ' ' || in_symbol_ptr == end_symbol_ptr) {
        continue;
      }
      uint32_t next_symbol = *in_symbol_ptr;
      if ((int32_t)next_symbol < 0) {
        // "negative" stream values are rule-define markers (0x80000001 + n) and
        // other reserved special values
        continue;
      }
      uint8_t does_next_symbol_start_word = (
          // for UTF-8, heuristically assume high bytes start word
          (next_symbol >= 0x80 && UTF8_compliant == 1)
          // otherwise use `word_start` lookup
          || (next_symbol < 0x80 && word_start[next_symbol] != 0));
      // we stop adding words once we're this many nodes away from
      // `node_num_limit`
      const uint8_t GST_RESERVED_NODES_THRESHOLD = 10;
      if (does_next_symbol_start_word &&
          next_node_num + GST_RESERVED_NODES_THRESHOLD < node_num_limit) {
        add_word_suffix(in_symbol_ptr, &next_node_num);
      }
    }
  }
  *out_next_node_num = next_node_num;

  uint32_t next_new_symbol_number = *p_next_new_symbol_number;
  if (fast_mode != 0) {
    size_t i = 0;
    do {
      if (symbol_counts[i] != 0) {
        if (symbol_counts[i] < NUM_PRECALCULATED_LOG2_X) {
          symbol_entropy_f[i] =
              (float)(log_file_symbols - log2_x[symbol_counts[i]]);
        } else {
          symbol_entropy_f[i] =
              (float)log_file_symbols - log2f((float)symbol_counts[i]);
        }
      }
    } while (++i < next_new_symbol_number);
  }

  uint16_t node_ptrs_num = 0;
  rank_scores_write_index = 0;
  rank_scores_read_index = 0;
  rank_scores_data_ptr->max_scores = max_scores;
  pthread_create(p_rank_scores_thread1, NULL, rank_word_scores_thread,
                 (void *)rank_scores_data_ptr);
  score_symbol_tree_words(rank_scores_data_ptr->rank_scores_buffer, node_data,
                          &node_ptrs_num, production_cost,
                          log2_num_symbols_plus_substitution_cost,
                          new_symbol_cost, symbol_entropy_f);
  while (node_ptrs_num !=
         atomic_load_explicit(&rank_scores_read_index, memory_order_acquire))
    ; // wait
  rank_scores_data_ptr->rank_scores_buffer[node_ptrs_num].last_match_index = 0;
  atomic_store_explicit(&rank_scores_write_index, node_ptrs_num + 1,
                        memory_order_release);
  pthread_join(*p_rank_scores_thread1, NULL);

  *out_prior_cycle_symbols = in_symbol_ptr - start_symbol_ptr;
  float min_score;
  min_score =
      fast_mode == 0
          ? (float)(1000.0 + (400.0 * (log2(order_0_entropy + 3000000.0) -
                                       log2(3000000.0))))
          : (float)(5.0 +
                    (log2(d_num_file_symbols + 5000000.0) - log2(5000000.0)));
  uint16_t num_candidates = rank_scores_data_ptr->num_candidates;
  if (next_new_symbol_number + num_candidates > max_rules) {
    if (max_rules > next_new_symbol_number) {
      num_candidates = max_rules - next_new_symbol_number;
    } else {
      fprintf(stderr,
              "GLZA compress: no room for word candidates "
              "(next_new_symbol_number=%u max_rules=%u)\n",
              (unsigned int)next_new_symbol_number, (unsigned int)max_rules);
      num_candidates = 0;
    }
  }
  size_t substitute_heap_size = *ptr_substitute_heap_size;
  uint16_t *candidates_index = *ptr_candidates_index;
  uint8_t *substitute_heap_buf = *ptr_substitute_heap_buf;
  if ((num_candidates != 0) &&
      (candidates[candidates_index[0]].score >= min_score)) {
    size_t substitute_heap_bytes =
        (num_file_symbols >= 1000000) ? 0x1000000 : 0x800000;
    uint8_t use_substitute_heap = 0;
    char *substitute_base;
    if (num_file_symbols >= 1000000) {
      if (substitute_heap_bytes > substitute_heap_size) {
        free(substitute_heap_buf);
        substitute_heap_buf = (uint8_t *)malloc(substitute_heap_bytes);
        substitute_heap_size = substitute_heap_bytes;
        if (substitute_heap_buf == 0) {
          fprintf(stderr, "ERROR - substitute memory allocation failed\n");
          exit(1);
        }
      }
      substitute_base = (char *)substitute_heap_buf;
      use_substitute_heap = 1;
    } else {
      substitute_base = (char *)(((size_t)end_symbol_ptr + 8) & ~7);
    }
    uint8_t *free_RAM_ptr = (uint8_t *)substitute_base;
    uint32_t *substitute_data = (uint32_t *)free_RAM_ptr;
    free_RAM_ptr +=
        0x40000 * sizeof(uint32_t); /* substitute_data_limit=0x40000 elsewhere;
                                       fixed heuristic size */
    struct substitute_thread_data substitute_thread_data =
        *ptr_substitute_thread_data;
    substitute_thread_data.symbol_counts = symbol_counts;
    substitute_thread_data.substitute_data = substitute_data;
    child_ptr_array = (struct match_node **)free_RAM_ptr;
    struct match_node *match_nodes =
        (struct match_node *)(free_RAM_ptr + sizeof(struct match_node *));

    // __jm__ simplify with lambda
    uintptr_t match_region_end_limit = end_RAM_ptr;
    if (use_substitute_heap != 0) {
      match_region_end_limit =
          (uintptr_t)substitute_base + substitute_heap_size;
    } else if (nodes != 0 && (uintptr_t)nodes < match_region_end_limit) {
      match_region_end_limit = (uintptr_t)nodes;
    }

    uint32_t match_nodes_limit =
        (uint32_t)((match_region_end_limit - (uintptr_t)match_nodes) /
                   sizeof(struct match_node)); /* derived bound; num_match_nodes
                                                  checked against this */
    uint32_t num_match_nodes = 1;
    uint32_t max_match_length = 0;
    {
      size_t candidate_num = 0;
      while (candidate_num < num_candidates) {
        if (candidates[candidates_index[candidate_num]].num_symbols >
            max_match_length) {
          max_match_length =
              candidates[candidates_index[candidate_num]].num_symbols;
        }
        if (candidates[candidates_index[candidate_num]].score < min_score) {
          num_candidates = candidate_num;
        }
        num_match_nodes +=
            candidates[candidates_index[candidate_num]].num_symbols - 1;
        if ((size_t)match_nodes +
                (num_match_nodes * sizeof(struct match_node)) +
                (4 * max_match_length) >=
            match_region_end_limit) {
          glza_warn_match_nodes_limit();
          num_candidates = candidate_num != 0 ? candidate_num - 1 : 0;
          break;
        }
        candidate_num++;
      }
    }
    uint32_t *match_strings =
        (uint32_t *)((size_t)match_nodes +
                     ((size_t)num_match_nodes * sizeof(struct match_node)));
    struct overlap_check *overlap_check_data =
        (struct overlap_check
             *)(((uintptr_t)&match_strings[num_candidates * max_match_length] +
                 7) &
                ~7);
    struct overlap_check *overlap_check_heap_buf = *ptr_overlap_check_heap_buf;
    uint8_t *candidate_bad = *ptr_candidate_bad;
    uint16_t num_candidates_processed = 0;
    uint32_t num_rules = *ptr_num_rules;
    struct find_substitutions_thread_data *find_substitutions_thread_data_buf =
        *ptr_find_substitutions_thread_data_buf;
    pthread_t substitute_thread1;
    pthread_t find_substitutions_threads[7];
    struct find_substitutions_thread_data *find_substitutions_thread_data =
        *ptr_find_substitutions_thread_data;
    uint32_t first_define_index = *ptr_first_define_index;
    if (num_candidates == 0) {
      goto skip_word_substitution;
    }

    {
      size_t candidate_num = 0;
      // save the match strings so they can be added to the end of the data
      // after symbol substitution is done
      while (candidate_num < num_candidates) {
        uint32_t *match_string_start_ptr =
            &match_strings[candidate_num * max_match_length];
        uint32_t *node_string_start_ptr =
            start_symbol_ptr +
            candidates[candidates_index[candidate_num]].last_match_index -
            candidates[candidates_index[candidate_num]].num_symbols + 1;
        for (size_t j = 0;
             j < candidates[candidates_index[candidate_num]].num_symbols; j++) {
          *(match_string_start_ptr + j) = *(node_string_start_ptr + j);
        }
        candidate_num++;
      }
    }
    if ((uintptr_t)overlap_check_data + (8 * sizeof(struct overlap_check)) >
        match_region_end_limit) {
      if (overlap_check_heap_buf == NULL) {
        overlap_check_heap_buf =
            (struct overlap_check *)malloc(8 * sizeof(struct overlap_check));
        if (overlap_check_heap_buf == NULL) {
          fprintf(stderr, "ERROR - overlap_check memory allocation failed\n");
          exit(1);
        }
      }
      overlap_check_data = overlap_check_heap_buf;
    }
    for (size_t i = 1; i < 8; i++) {
      overlap_check_data[i].candidate_bad = &candidate_bad[0];
    }

    do {
      next_new_symbol_number = num_terminals + num_rules;
      // build a prefix tree of the match strings, defer shorter overlapping
      // strings
      num_match_nodes = 0;
      {
        size_t candidate_num = 0;
        while (candidate_num < num_candidates) {
          if (candidate_bad[candidate_num] == 0) {
            uint32_t *best_score_last_match_ptr;
            uint32_t *best_score_match_ptr;
            best_score_match_ptr =
                match_strings + (candidate_num * max_match_length);
            best_score_last_match_ptr =
                best_score_match_ptr +
                candidates[candidates_index[candidate_num]].num_symbols - 1;
            best_score_match_ptr++;
            if (num_match_nodes == 0) {
              child_ptr_array[0] = match_nodes;
              init_match_node(match_nodes, *best_score_match_ptr, 2,
                              candidate_num);
              num_match_nodes = 1;
            }
            struct match_node *match_node_ptr = match_nodes;
            while (best_score_match_ptr <= best_score_last_match_ptr) {
              uint32_t symbol = *best_score_match_ptr;
              if (match_node_ptr->child_ptr == 0) {
                if (num_match_nodes >= match_nodes_limit) {
                  glza_warn_match_nodes_limit();
                  candidate_bad[candidate_num] = 1;
                  break;
                }
                match_node_ptr->child_ptr = &match_nodes[num_match_nodes++];
                match_node_ptr = match_node_ptr->child_ptr;
                init_match_node(match_node_ptr, symbol, 0, candidate_num);
              } else {
                match_node_ptr = match_node_ptr->child_ptr;
                uint8_t sibling_number;
                if (move_to_match_sibling(match_nodes, &match_node_ptr, symbol,
                                          &sibling_number) != 0) {
                  if (match_node_ptr->child_ptr == 0) {
                    candidate_bad[match_node_ptr->score_number] = 1;
                  }
                } else {
                  if (num_match_nodes >= match_nodes_limit) {
                    glza_warn_match_nodes_limit();
                    candidate_bad[candidate_num] = 1;
                    break;
                  }
                  match_node_ptr->sibling_node_num[sibling_number] =
                      num_match_nodes;
                  match_node_ptr = &match_nodes[num_match_nodes++];
                  init_match_node(match_node_ptr, symbol, 0, candidate_num);
                }
              }
              best_score_match_ptr++;
            }
            if (match_node_ptr->child_ptr != 0) {
              candidate_bad[candidate_num] = 1;
            }
          }
          candidate_num++;
        }
      }

      // Redo the tree build with just this subcycle's candidates
      child_ptr_array[0] = 0;
      num_match_nodes = 0;
      size_t j = next_new_symbol_number;
      {
        size_t candidate_num = 0;
        while (candidate_num < num_candidates) {
          if (candidate_bad[candidate_num] == 0) {
            uint32_t *best_score_last_match_ptr;
            uint32_t *best_score_match_ptr;
            best_score_match_ptr =
                match_strings + (candidate_num * max_match_length);
            best_score_last_match_ptr =
                best_score_match_ptr +
                candidates[candidates_index[candidate_num]].num_symbols - 1;
            best_score_match_ptr++;
            uint32_t symbol = *best_score_match_ptr++;
            uint32_t best_score_num_symbols = 2;
            struct match_node *match_node_ptr =
                move_to_base_match_child_with_make(match_nodes, symbol, j,
                                                   &num_match_nodes,
                                                   &child_ptr_array[0]);
            while (best_score_match_ptr <= best_score_last_match_ptr) {
              symbol = *best_score_match_ptr++;
              best_score_num_symbols++;
              move_to_match_child_with_make(match_nodes, &match_node_ptr,
                                            symbol, j, best_score_num_symbols,
                                            &num_match_nodes);
            }
            symbol_counts[j++] = 0;
          }
          candidate_num++;
        }
      }
      // scan the data following the prefix tree and substitute new symbols on
      // end matches (child is 0)
      uint32_t *stop_symbol_ptr;
      if (num_file_symbols >= 100000000) {
        if (find_substitutions_thread_data_buf == NULL) {
          find_substitutions_thread_data_buf =
              (struct find_substitutions_thread_data *)malloc(
                  6 * sizeof(struct find_substitutions_thread_data));
          if (find_substitutions_thread_data_buf == NULL) {
            fprintf(stderr,
                    "ERROR - find_substitutions memory allocation failed\n");
            exit(1);
          }
          memset(find_substitutions_thread_data_buf, 0,
                 6 * sizeof(struct find_substitutions_thread_data));
        }
        find_substitutions_thread_data = find_substitutions_thread_data_buf;
        stop_symbol_ptr = start_symbol_ptr + (64 * (num_file_symbols >> 9));
        find_substitutions_thread_data[0].start_symbol_ptr = stop_symbol_ptr;
        uint32_t *block_ptr = stop_symbol_ptr + (68 * (num_file_symbols >> 9));
        find_substitutions_thread_data[0].stop_symbol_ptr = block_ptr;
        find_substitutions_thread_data[1].start_symbol_ptr = block_ptr;
        block_ptr += 72 * (num_file_symbols >> 9);
        find_substitutions_thread_data[1].stop_symbol_ptr = block_ptr;
        find_substitutions_thread_data[2].start_symbol_ptr = block_ptr;
        block_ptr += 75 * (num_file_symbols >> 9);
        find_substitutions_thread_data[2].stop_symbol_ptr = block_ptr;
        find_substitutions_thread_data[3].start_symbol_ptr = block_ptr;
        block_ptr += 77 * (num_file_symbols >> 9);
        find_substitutions_thread_data[3].stop_symbol_ptr = block_ptr;
        find_substitutions_thread_data[4].start_symbol_ptr = block_ptr;
        block_ptr += 78 * (num_file_symbols >> 9);
        find_substitutions_thread_data[4].stop_symbol_ptr = block_ptr;
        find_substitutions_thread_data[5].start_symbol_ptr = block_ptr;
        find_substitutions_thread_data[5].stop_symbol_ptr = end_symbol_ptr;
        for (size_t i = 0; i < 6; i++) {
          find_substitutions_thread_data[i].match_nodes = match_nodes;
          find_substitutions_thread_data[i].done = 0;
          find_substitutions_thread_data[i].read_index = 0;
          find_substitutions_thread_data[i].write_index = 0;
          pthread_create(&find_substitutions_threads[i], NULL,
                         find_substitutions_thread,
                         (void *)&find_substitutions_thread_data[i]);
        }
      } else {
        stop_symbol_ptr = end_symbol_ptr;
      }

      uint32_t extra_match_symbols = 0;
      uint32_t substitute_index = 0;
      const uint32_t substitute_data_limit = 0x40000;
      in_symbol_ptr = start_symbol_ptr;
      uint32_t *previous_in_symbol_ptr = start_symbol_ptr;
      uint32_t *out_symbol_ptr = start_symbol_ptr;

      substitute_thread_data.in_symbol_ptr = start_symbol_ptr;
      substitute_thread_data.max_rule_symbol =
          (j > next_new_symbol_number) ? (j - 1) : (next_new_symbol_number - 1);
      substitute_data_write_index = 0;
      substitute_data_read_index = 0;
      pthread_create(&substitute_thread1, NULL, substitute_thread,
                     (void *)&substitute_thread_data);

    wmain_symbol_substitution_loop_top:
      if (*in_symbol_ptr++ == 0x20) {
        uint32_t symbol = *in_symbol_ptr++;
        if ((int32_t)symbol < 0) {
          if (in_symbol_ptr < stop_symbol_ptr) {
            goto wmain_symbol_substitution_loop_top;
          }
          goto wmain_symbol_substitution_loop_end;
        } else {
          struct match_node *match_node_ptr = child_ptr_array[0];
        wmain_symbol_substitution_loop_match_search:
          if (symbol != match_node_ptr->symbol) {
            uint32_t sibling_nibble = symbol;
            do {
              if (match_node_ptr->sibling_node_num[sibling_nibble & 0xF] != 0) {
                match_node_ptr =
                    &match_nodes[match_node_ptr
                                     ->sibling_node_num[sibling_nibble & 0xF]];
                sibling_nibble = sibling_nibble >> 4;
              } else { // no match, so output missed symbols
                if (symbol == 0x20) {
                  if (in_symbol_ptr > stop_symbol_ptr) {
                    goto wmain_symbol_substitution_loop_end;
                  }
                  symbol = *in_symbol_ptr++;
                  if ((int32_t)symbol >= 0) {
                    match_node_ptr = child_ptr_array[0];
                    goto wmain_symbol_substitution_loop_match_search;
                  }
                  if (in_symbol_ptr < stop_symbol_ptr) {
                    goto wmain_symbol_substitution_loop_top;
                  }
                  goto wmain_symbol_substitution_loop_end;
                }
                if (in_symbol_ptr < stop_symbol_ptr) {
                  goto wmain_symbol_substitution_loop_top;
                }
                goto wmain_symbol_substitution_loop_end;
              }
            } while (symbol != match_node_ptr->symbol);
          }
          if (match_node_ptr->child_ptr != 0) {
            symbol = *in_symbol_ptr++;
            if ((int32_t)symbol >= 0) {
              match_node_ptr = match_node_ptr->child_ptr;
              goto wmain_symbol_substitution_loop_match_search;
            }
            if (in_symbol_ptr < stop_symbol_ptr) {
              goto wmain_symbol_substitution_loop_top;
            }
            goto wmain_symbol_substitution_loop_end;
          }
          // found a match
          if ((substitute_index + 3) >= substitute_data_limit) {
            fprintf(stderr, "ERROR - substitute_data buffer overflow\n");
            exit(1);
          }
          if (((substitute_index + 2) & 0xFFFC) == 0) {
            while ((substitute_index -
                    atomic_load_explicit(&substitute_data_read_index,
                                         memory_order_acquire)) >= 0xFFF0)
              ; // wait
          }
          if (in_symbol_ptr - previous_in_symbol_ptr -
                  match_node_ptr->num_symbols !=
              0) {
            substitute_data[substitute_index++] = in_symbol_ptr -
                                                  previous_in_symbol_ptr -
                                                  match_node_ptr->num_symbols;
          }
          substitute_data[substitute_index++] =
              0x80000000 + match_node_ptr->num_symbols;
          substitute_data[substitute_index++] = match_node_ptr->score_number;
          atomic_store_explicit(&substitute_data_write_index, substitute_index,
                                memory_order_release);
          previous_in_symbol_ptr = in_symbol_ptr;
          if (in_symbol_ptr < stop_symbol_ptr) {
            goto wmain_symbol_substitution_loop_top;
          }
          extra_match_symbols = in_symbol_ptr - stop_symbol_ptr;
          goto wmain_symbol_substitution_loop_end2;
        }
      }
      if (in_symbol_ptr < stop_symbol_ptr) {
        goto wmain_symbol_substitution_loop_top;
      }

    wmain_symbol_substitution_loop_end:
      if ((substitute_index & 0xFFF) == 0) {
        while ((substitute_index -
                atomic_load_explicit(&substitute_data_read_index,
                                     memory_order_acquire)) >= 0xFFF0)
          ; // wait
      }
      substitute_data[substitute_index++] =
          stop_symbol_ptr - previous_in_symbol_ptr;
      atomic_store_explicit(&substitute_data_write_index, substitute_index,
                            memory_order_release);
    wmain_symbol_substitution_loop_end2:
      if ((substitute_index & 0xFFF) == 0) {
        while (substitute_index !=
               atomic_load_explicit(&substitute_data_read_index,
                                    memory_order_acquire))
          ; // wait
      }
      substitute_data[substitute_index++] = 0xFFFFFFFF;
      atomic_store_explicit(&substitute_data_write_index, substitute_index,
                            memory_order_release);
      pthread_join(substitute_thread1, NULL);
      in_symbol_ptr = substitute_thread_data.in_symbol_ptr;
      out_symbol_ptr = substitute_thread_data.out_symbol_ptr;

      if (num_file_symbols >= 100000000) {
        for (size_t i = 0; i < 6; i++) {
          uint32_t local_substitutions_write_index;
          uint32_t substitutions_index = 0;
          if (extra_match_symbols != 0) {
            while ((local_substitutions_write_index = atomic_load_explicit(
                        &find_substitutions_thread_data[i].write_index,
                        memory_order_acquire)) == 0)
              ; // wait
            if (find_substitutions_thread_data[i].data[0] >
                extra_match_symbols) {
              find_substitutions_thread_data[i].data[0] -= extra_match_symbols;
            } else {
              substitutions_index = 1;
            }
            extra_match_symbols = 0;
          }

          while ((atomic_load_explicit(&find_substitutions_thread_data[i].done,
                                       memory_order_acquire) == 0) ||
                 (substitutions_index !=
                  atomic_load_explicit(
                      &find_substitutions_thread_data[i].write_index,
                      memory_order_acquire))) {
            local_substitutions_write_index = atomic_load_explicit(
                &find_substitutions_thread_data[i].write_index,
                memory_order_acquire);
            if (substitutions_index != local_substitutions_write_index) {
              do {
                uint32_t data =
                    find_substitutions_thread_data[i].data[substitutions_index];
                if ((int32_t)data < 0) {
                  in_symbol_ptr += (size_t)(data + 0x80000000);
                  substitutions_index = (substitutions_index + 1) & 0x7FFFFF;
                  uint32_t symbol = find_substitutions_thread_data[i]
                                        .data[substitutions_index];
                  *out_symbol_ptr++ = symbol;
                  symbol_counts[symbol]++;
                  substitutions_index = (substitutions_index + 1) & 0x7FFFFF;
                  atomic_store_explicit(
                      &find_substitutions_thread_data[i].read_index,
                      substitutions_index, memory_order_release);
                } else {
                  memmove(out_symbol_ptr, in_symbol_ptr, data * 4);
                  in_symbol_ptr += data;
                  out_symbol_ptr += data;
                  substitutions_index = (substitutions_index + 1) & 0x7FFFFF;
                }
              } while (substitutions_index != local_substitutions_write_index);
            }
          }
          atomic_store_explicit(&find_substitutions_thread_data[i].read_index,
                                substitutions_index, memory_order_release);
          pthread_join(find_substitutions_threads[i], NULL);
          extra_match_symbols +=
              find_substitutions_thread_data[i].extra_match_symbols;
        }
      }

      if (num_rules == 0) {
        first_define_index = out_symbol_ptr - start_symbol_ptr;
      } else {
        if (out_symbol_ptr < start_symbol_ptr + first_define_index) {
          first_define_index = out_symbol_ptr - start_symbol_ptr;
        }
        if (*(start_symbol_ptr + first_define_index) != 0x80000001) {
          while (*(start_symbol_ptr + --first_define_index) != 0x80000001)
            ; // decrement index until found
        }
      }

      // Add new production rules and update symbol counts
      for (size_t i = 0; i < num_candidates; i++) {
        if (candidate_bad[i] == 0) {
          num_candidates_processed++;
          candidate_bad[i] = 2;
          uint32_t *match_string_ptr;
          uint32_t *match_string_end_ptr;
          *out_symbol_ptr++ = num_rules + 0x80000001;
          match_string_ptr = match_strings + (max_match_length * i);
          match_string_end_ptr =
              match_string_ptr + candidates[candidates_index[i]].num_symbols;
          uint32_t num_repeats = symbol_counts[num_terminals + num_rules] - 1;
          uint32_t sym1;
          uint32_t sym2;
          sym1 = *match_string_ptr;
          symbol_ends[num_terminals + num_rules].start =
              symbol_ends[sym1].start;
          symbol_counts[sym1] -= num_repeats;
          *out_symbol_ptr++ = *match_string_ptr++;
          while (match_string_ptr != match_string_end_ptr) {
            sym2 = *match_string_ptr;
            symbol_counts[sym2] -= num_repeats;
            o1c[symbol_ends[sym1].end][symbol_ends[sym2].start] -= num_repeats;
            num_ends[symbol_ends[sym1].end] -= num_repeats;
            num_starts[symbol_ends[sym2].start] -= num_repeats;
            sym1 = sym2;
            *out_symbol_ptr++ = *match_string_ptr++;
          }
          symbol_ends[num_terminals + num_rules++].end = symbol_ends[sym1].end;
        } else if (candidate_bad[i] == 1) {
          candidate_bad[i] = 0;
        }
      }
      end_symbol_ptr = out_symbol_ptr;
      *end_symbol_ptr = 0xFFFFFFFE;
      num_file_symbols = end_symbol_ptr - start_symbol_ptr;
      if (j > num_terminals + num_rules) {
        fprintf(stderr,
                "GLZA compress: reconciling num_rules %u -> %u after word "
                "substitution (j=%u num_terminals=%u)\n",
                (unsigned int)num_rules, (unsigned int)(j - num_terminals),
                (unsigned int)j, (unsigned int)num_terminals);
        num_rules = j - num_terminals;
      }
      if (fast_mode == 0) {
#ifdef PRINTON
        fprintf(stderr, "Replaced %u of %u words\n", num_candidates_processed,
                num_candidates);
#endif
      }
    } while (
        num_candidates_processed !=
        num_candidates); // should go to end here if hit maximum dictionary size
    *ptr_first_define_index = first_define_index;
    *ptr_find_substitutions_thread_data_buf =
        find_substitutions_thread_data_buf;
    *ptr_find_substitutions_thread_data = find_substitutions_thread_data;
    memset(candidate_bad, 0, num_candidates);
  skip_word_substitution:;
    *out_substitute_data = substitute_data;
    *out_free_RAM_ptr = free_RAM_ptr;
    *ptr_substitute_thread_data = substitute_thread_data;
    *out_num_match_nodes = num_match_nodes;
    *out_max_match_length = max_match_length;
    *out_match_strings = match_strings;
    *out_overlap_check_data = overlap_check_data;
    *ptr_overlap_check_heap_buf = overlap_check_heap_buf;
    *ptr_candidate_bad = candidate_bad;
    *ptr_num_rules = num_rules;
  }

  // mutate outer vars
  *out_base_node_child_num_ptr = base_node_child_num_ptr;
  *out_in_symbol_ptr = in_symbol_ptr;
  *p_next_new_symbol_number = next_new_symbol_number;
  *out_node_ptrs_num = node_ptrs_num;
  *out_num_candidates = num_candidates;
  *ptr_substitute_heap_size = substitute_heap_size;
  *ptr_candidates_index = candidates_index;
  *ptr_substitute_heap_buf = substitute_heap_buf;
}

static uint8_t scan_mode1(uint32_t **out_in_symbol_ptr,
                          uint32_t next_new_symbol_number,
                          const uint32_t max_rules, uint32_t *out_max_scores,
                          uint32_t initial_max_scores, uint8_t max_terminal,
                          uint32_t *new_symbol_number, uint32_t *p_num_rules,
                          uint32_t *p_first_define_index,
                          float *p_prior_min_score) {
  *out_max_scores = initial_max_scores;
  uint32_t max_run_length[0x100]; /* longest equal-symbol run per terminal;
                                     scan_mode==1 dedup pass only */
  uint32_t run_length = 0;
  uint32_t prior_symbol = 0xFFFFFFFE;
  memset(max_run_length, 0, 0x400);
  uint32_t *in_symbol_ptr = start_symbol_ptr;
  uint32_t symbol;
  do {
    symbol = *in_symbol_ptr++;
    if (symbol == prior_symbol) {
      run_length++;
    } else {
      if (run_length != 0 && run_length > max_run_length[prior_symbol]) {
        max_run_length[prior_symbol] = run_length;
      }
      run_length = 0;
      prior_symbol = symbol <= max_terminal ? symbol : 0xFFFFFFFE;
    }
  } while (in_symbol_ptr != end_symbol_ptr);
  in_symbol_ptr = start_symbol_ptr;
  if ((run_length != 0) && (run_length > max_run_length[prior_symbol])) {
    max_run_length[prior_symbol] = run_length;
  }

  uint8_t found_run = 0;
  for (size_t i = 0; i <= max_terminal; i++) {
    if ((max_run_length[i] >= 63) && (next_new_symbol_number < max_rules)) {
      max_run_length[i] =
          1 << (uint32_t)log2(sqrt((double)max_run_length[i] + 1.5));
      symbol_counts[next_new_symbol_number] = 0;
      new_symbol_number[i] = next_new_symbol_number++;
      found_run = 1;
    } else {
      max_run_length[i] = 0;
    }
  }

  if (found_run != 0) {
    if (fast_mode == 0) {
#ifdef PRINTON
      fprintf(stderr, "Deduplicating runs\n");
#endif
    }
    run_length = 0;
    uint32_t *out_symbol_ptr = start_symbol_ptr;
    prior_symbol = *in_symbol_ptr;
    while (in_symbol_ptr++ != end_symbol_ptr) {
      symbol = *in_symbol_ptr;
      if ((symbol == prior_symbol) && (symbol <= max_terminal)) {
        if (++run_length == max_run_length[symbol] - 1) {
          prior_symbol = new_symbol_number[symbol];
          run_length = 0;
          symbol_counts[new_symbol_number[symbol]]++;
        }
      } else {
        *out_symbol_ptr++ = prior_symbol;
        while (run_length != 0) {
          *out_symbol_ptr++ = prior_symbol;
          run_length--;
        }
        prior_symbol = symbol;
      }
    }

    uint32_t num_rules = *p_num_rules;
    uint32_t first_define_index = *p_first_define_index;
    if (num_rules == 0) {
      first_define_index = out_symbol_ptr - start_symbol_ptr;
    } else {
      if (out_symbol_ptr < start_symbol_ptr + first_define_index) {
        first_define_index = out_symbol_ptr - start_symbol_ptr;
      }
      if (*(start_symbol_ptr + first_define_index) !=
          0x80000001) // decrement index until found
        while (*(start_symbol_ptr + --first_define_index) != 0x80000001)
          ;
    }

    // Add the new symbol definitions to the end of the data
    for (size_t i = 0; i <= max_terminal; i++) {
      if (max_run_length[i] != 0) {
        *out_symbol_ptr++ = 0x80000001 + num_rules;
        uint32_t j = 0;
        while (j++ != max_run_length[i]) {
          *out_symbol_ptr++ = i;
        }
        symbol_counts[i] -=
            max_run_length[i] * (symbol_counts[new_symbol_number[i]] - 1);
        o1c[i][i] -=
            (max_run_length[i] - 1) * (symbol_counts[new_symbol_number[i]] - 1);
        num_ends[i] -=
            (max_run_length[i] - 1) * (symbol_counts[new_symbol_number[i]] - 1);
        num_starts[i] -=
            (max_run_length[i] - 1) * (symbol_counts[new_symbol_number[i]] - 1);
        symbol_ends[num_terminals + num_rules].start = i;
        symbol_ends[num_terminals + num_rules++].end = i;
      }
    }
    end_symbol_ptr = out_symbol_ptr;
    *end_symbol_ptr = 0xFFFFFFFE;
    num_file_symbols = end_symbol_ptr - start_symbol_ptr;
    min_score = fast_mode == 0 ? 10.0 : 40.0;
    *p_prior_min_score = BIG_FLOAT;
    *p_num_rules = num_rules;
    *p_first_define_index = first_define_index;
  }

  *out_in_symbol_ptr = in_symbol_ptr;
  return found_run;
}

static void build_and_score_suffix_tree(
    uint32_t **out_in_symbol_ptr, uint32_t *out_next_node_num,
    uint16_t *out_node_ptrs_num, float *out_cycle_end_ratio,
    uint32_t *start_cycle_symbol_ptr, uint32_t node_num_limit,
    uint32_t next_new_symbol_number, uint32_t num_rules, uint32_t max_scores,
    float cycle_start_ratio, uint8_t fast_section, uint8_t fast_sections,
    double *symbol_entropy, float *symbol_entropy_f, double profit_ratio_power,
    float production_cost, float log2_num_symbols_plus_substitution_cost,
    float new_symbol_cost[NUM_PRECALCULATED_SYMBOL_COSTS],
    struct rank_scores_thread_data *rank_scores_data_ptr,
    struct score_data *node_data, struct tree_thread_data tree_thread_data[13],
    pthread_t build_tree_threads[7], pthread_t *rank_scores_thread1) {
  uint32_t main_max_symbol;  /* highest symbol id handled by main (non-worker)
                                suffix-tree builder this pass */
  uint32_t main_nodes_limit; /* node budget for main thread; fast_mode==0: 18%
                                of nodes; fast_mode==1: 6% */
  size_t i = 1;
  uint32_t nodes_div_100 = node_num_limit / 100;
  uint32_t *end_cycle_symbol_ptr; /* fast_mode==1 only: end of current
                                     fast_section slice */
  uint32_t *in_symbol_ptr = *out_in_symbol_ptr;
  uint32_t next_node_num = 1;
  uint32_t symbol;
  if (fast_mode == 0) {
    // NOLINTBEGIN(readability-magic-numbers)
    uint8_t thread_symbol_limit[] = {5,  11, 17, 24, 32, 42,
                                     52, 61, 69, 77, 86, 93};
    uint32_t thread_first_node_num[] = {18 * nodes_div_100,
                                        31 * nodes_div_100,
                                        43 * nodes_div_100,
                                        56 * nodes_div_100,
                                        70 * nodes_div_100,
                                        85 * nodes_div_100,
                                        1,
                                        16 * nodes_div_100,
                                        31 * nodes_div_100,
                                        43 * nodes_div_100,
                                        56 * nodes_div_100,
                                        70 * nodes_div_100};
    uint32_t thread_nodes_limit[] = {
        31 * nodes_div_100, 43 * nodes_div_100, 56 * nodes_div_100,
        70 * nodes_div_100, 85 * nodes_div_100, node_num_limit,
        16 * nodes_div_100, 31 * nodes_div_100, 43 * nodes_div_100,
        56 * nodes_div_100, 70 * nodes_div_100, 85 * nodes_div_100};
    main_nodes_limit = (nodes_div_100 * 18) - 10;
    // NOLINTEND(readability-magic-numbers)
    main_max_symbol =
        stca_setup(12, thread_symbol_limit, thread_first_node_num,
                   thread_nodes_limit, num_rules, next_new_symbol_number,
                   tree_thread_data, start_cycle_symbol_ptr);

    scan_symbol_ptr = (uintptr_t)in_symbol_ptr;
    max_symbol_ptr = 0;
    for (size_t j = 0; j < 6; j++) {
      pthread_create(&build_tree_threads[j], NULL, build_tree_thread,
                     (void *)&tree_thread_data[j]);
    }
  } else {
    // NOLINTBEGIN(readability-magic-numbers)
    uint8_t thread_symbol_limit[] = {6,  12, 19, 26, 34, 43, 54,
                                     67, 73, 79, 85, 90, 95};
    uint32_t thread_first_node_num[] = {6 * nodes_div_100,  12 * nodes_div_100,
                                        22 * nodes_div_100, 34 * nodes_div_100,
                                        48 * nodes_div_100, 64 * nodes_div_100,
                                        81 * nodes_div_100, 1,
                                        12 * nodes_div_100, 22 * nodes_div_100,
                                        34 * nodes_div_100, 48 * nodes_div_100,
                                        64 * nodes_div_100};
    uint32_t thread_nodes_limit[] = {
        12 * nodes_div_100, 22 * nodes_div_100, 34 * nodes_div_100,
        48 * nodes_div_100, 64 * nodes_div_100, 81 * nodes_div_100,
        node_num_limit,     12 * nodes_div_100, 22 * nodes_div_100,
        34 * nodes_div_100, 48 * nodes_div_100, 64 * nodes_div_100,
        81 * nodes_div_100};
    main_nodes_limit = (nodes_div_100 * 6) - 10;
    // NOLINTEND(readability-magic-numbers)
    main_max_symbol =
        stca_setup(13, thread_symbol_limit, thread_first_node_num,
                   thread_nodes_limit, num_rules, next_new_symbol_number,
                   tree_thread_data, start_cycle_symbol_ptr);

    end_cycle_symbol_ptr =
        fast_section == fast_sections - 1 /* fast_mode==1 only */
            ? end_symbol_ptr
            : start_symbol_ptr +
                  (uint32_t)((float)num_file_symbols *
                             (float)(fast_section + 1) / (float)fast_sections);

    atomic_store_explicit(&scan_symbol_ptr, (uintptr_t)end_cycle_symbol_ptr,
                          memory_order_relaxed);
    atomic_store_explicit(&max_symbol_ptr, (uintptr_t)end_cycle_symbol_ptr,
                          memory_order_release);
    for (size_t j = 0; j < 7; j++) {
      pthread_create(&build_tree_threads[j], NULL, build_tree_thread,
                     (void *)&tree_thread_data[j]);
    }
  }
  memset(base_nodes_child_node_num, 0,
         4 * (main_max_symbol + 1) * BASE_NODES_CHILD_ARRAY_SIZE);

  uint16_t node_ptrs_num;
  if (fast_mode == 0) {
    do {
      symbol = *in_symbol_ptr++;
      if (symbol <= main_max_symbol) {
        atomic_store_explicit(&scan_symbol_ptr, (uintptr_t)in_symbol_ptr,
                              memory_order_relaxed);
        if ((int32_t)*in_symbol_ptr >= 0) {
          add_suffix(symbol, in_symbol_ptr, &next_node_num);
          if (next_node_num >= main_nodes_limit) {
            glza_warn_main_nodes_limit();
            goto done_building_tree_tree;
          }
        }
      }
    } while (symbol != 0xFFFFFFFE);
    in_symbol_ptr--;
  done_building_tree_tree:
    atomic_store_explicit(&scan_symbol_ptr, (uintptr_t)in_symbol_ptr,
                          memory_order_relaxed);
    atomic_store_explicit(&max_symbol_ptr, (uintptr_t)in_symbol_ptr,
                          memory_order_release);
    node_ptrs_num = 0;
    atomic_store_explicit(&rank_scores_write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&rank_scores_read_index, 0, memory_order_relaxed);
#ifdef PRINTON
    fprintf(stderr, ".");
#endif
    rank_scores_data_ptr->max_scores = (uint16_t)max_scores;

    pthread_create(rank_scores_thread1, NULL, rank_scores_thread,
                   (void *)rank_scores_data_ptr);
    score_symbol_tree(
        0, main_max_symbol, rank_scores_data_ptr->rank_scores_buffer, node_data,
        &node_ptrs_num, profit_ratio_power, symbol_entropy, symbol_counts);
    for (i = 0; i < 12; i++) {
#ifdef PRINTON
      fprintf(stderr, ".");
#endif
      if (i < 6) {
        pthread_join(build_tree_threads[i], NULL);
        pthread_create(&build_tree_threads[i], NULL, build_tree_thread,
                       (void *)&tree_thread_data[i + 6]);
      } else {
        pthread_join(build_tree_threads[i - 6], NULL);
      }
#ifdef PRINTON
      fprintf(stderr, ".");
#endif
      score_symbol_tree(
          tree_thread_data[i].min_symbol, tree_thread_data[i].max_symbol,
          rank_scores_data_ptr->rank_scores_buffer, node_data, &node_ptrs_num,
          profit_ratio_power, symbol_entropy, symbol_counts);
    }

    if ((node_ptrs_num & 0xFFF) == 0) {
      while ((uint16_t)(node_ptrs_num -
                        atomic_load_explicit(&rank_scores_read_index,
                                             memory_order_acquire)) >= 0xF000)
        ; // wait
    }
    rank_scores_data_ptr->rank_scores_buffer[node_ptrs_num].last_match_index =
        0;
    atomic_store_explicit(&rank_scores_write_index, node_ptrs_num + 1,
                          memory_order_release);
    pthread_join(*rank_scores_thread1, NULL);
#ifdef PRINTON
    fprintf(stderr, "\rStart %.4f", cycle_start_ratio);
#endif
    *out_cycle_end_ratio =
        (float)(in_symbol_ptr - start_symbol_ptr) / (float)num_file_symbols;
  } else {
    do {
      symbol = *in_symbol_ptr++;
      if (symbol <= main_max_symbol && (int32_t)*in_symbol_ptr >= 0) {
        add_suffix(symbol, in_symbol_ptr, &next_node_num);
        if (next_node_num >= main_nodes_limit) {
          glza_warn_main_nodes_limit();
          break;
        }
      }
    } while (in_symbol_ptr != end_cycle_symbol_ptr);
    node_ptrs_num = 0;
    atomic_store_explicit(&rank_scores_write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&rank_scores_read_index, 0, memory_order_relaxed);
    i = 0;
    do {
      if (symbol_counts[i] != 0) {
        symbol_entropy_f[i] =
            symbol_counts[i] < NUM_PRECALCULATED_LOG2_X
                ? (float)(log_file_symbols - log2_x[symbol_counts[i]])
                : (float)log_file_symbols - log2f((float)symbol_counts[i]);
      }
    } while (++i < next_new_symbol_number);
    rank_scores_data_ptr->max_scores = (uint16_t)max_scores;
    rank_scores_data_ptr->num_file_symbols = num_file_symbols;
    pthread_join(build_tree_threads[0], NULL);
    pthread_create(rank_scores_thread1, NULL, rank_scores_thread_fast,
                   (void *)rank_scores_data_ptr);
    score_symbol_tree_fast(0, tree_thread_data[0].max_symbol,
                           rank_scores_data_ptr->rank_scores_buffer, node_data,
                           &node_ptrs_num, production_cost, profit_ratio_power,
                           log2_num_symbols_plus_substitution_cost,
                           new_symbol_cost, symbol_entropy_f, symbol_counts);
    for (i = 1; i <= 12; i++) {
      if (i <= 6) {
        pthread_join(build_tree_threads[i], NULL);
        pthread_create(&build_tree_threads[i - 1], NULL, build_tree_thread,
                       (void *)&tree_thread_data[i + 6]);
      } else {
        pthread_join(build_tree_threads[i - 7], NULL);
      }
      score_symbol_tree_fast(
          tree_thread_data[i].min_symbol, tree_thread_data[i].max_symbol,
          rank_scores_data_ptr->rank_scores_buffer, node_data, &node_ptrs_num,
          production_cost, profit_ratio_power,
          log2_num_symbols_plus_substitution_cost, new_symbol_cost,
          symbol_entropy_f, symbol_counts);
    }
    if ((node_ptrs_num & 0xFFF) == 0) {
      while ((uint16_t)(node_ptrs_num -
                        atomic_load_explicit(&rank_scores_read_index,
                                             memory_order_acquire)) >= 0xF000)
        ; // wait
    }
    rank_scores_data_ptr->rank_scores_buffer[node_ptrs_num].last_match_index =
        0;
    atomic_store_explicit(&rank_scores_write_index, node_ptrs_num + 1,
                          memory_order_release);
    pthread_join(*rank_scores_thread1, NULL);
  }

  *out_in_symbol_ptr = in_symbol_ptr;
  *out_next_node_num = next_node_num;
  *out_node_ptrs_num = node_ptrs_num;
}

static void
handle_zero_candidates(enum glza_scan_mode *scan_mode, uint16_t *num_candidates,
                       float *prior_min_score, uint8_t *fast_section,
                       uint8_t *fast_sections, float *fast_min_score) {
  if (fast_mode == 0) {
#ifdef PRINTON
    fprintf(stderr, "\r");
#endif
  }
  if (*scan_mode == GLZA_SCAN_RETRY) {
    if (min_score > 0.0) {
      *num_candidates = 1;
      *prior_min_score = min_score;
      min_score = 0.0;
    } else if (*fast_sections != 1) {
      if (*fast_sections == 23) {
        *fast_sections = 9;
        *fast_section = 0;
        min_score = 8.0;
      } else {
        *fast_sections = (*fast_sections + 1) >> 1;
        *fast_section >>= 1;
        min_score = 4.0;
      }
      *prior_min_score = BIG_FLOAT;
      *fast_min_score = 1.0;

      *num_candidates = 1;
      *scan_mode = GLZA_SCAN_GENERAL;
    }
  } else {
    *scan_mode = GLZA_SCAN_RETRY;
    *num_candidates = 1;
    *prior_min_score = min_score;
    min_score = 0.25 * min_score;
  }
}

static void process_ranked_candidates(
    uint16_t *p_num_candidates, enum glza_scan_mode *p_scan_mode,
    uint32_t next_new_symbol_number, uint32_t max_rules, uint32_t max_scores,
    uint32_t *p_num_rules, uint32_t *p_first_define_index,
    uint32_t **p_in_symbol_ptr, const uint16_t *candidates_index,
    uint8_t *candidate_bad, uint8_t *end_RAM_ptr,
    struct rank_scores_thread_data *rank_scores_data_ptr,
    struct overlap_check **p_overlap_check_data,
    struct overlap_check **p_overlap_check_heap_buf,
    uint32_t *p_num_match_nodes, uint32_t *p_max_match_length,
    uint32_t **p_match_strings, uint8_t *p_fast_section,
    uint8_t *p_fast_sections, uint8_t *p_section_repeats,
    float section_scores[23], float *p_prior_min_score, float *p_fast_min_score,
    float *p_new_min_score, uint16_t scan_cycle) {
  uint16_t num_candidates = *p_num_candidates;
  enum glza_scan_mode scan_mode = *p_scan_mode;
  uint32_t num_rules = *p_num_rules;
  uint32_t first_define_index = *p_first_define_index;
  uint32_t *in_symbol_ptr = *p_in_symbol_ptr;
  struct overlap_check *overlap_check_data = *p_overlap_check_data;
  struct overlap_check *overlap_check_heap_buf = *p_overlap_check_heap_buf;
  uint32_t num_match_nodes = *p_num_match_nodes;
  uint32_t max_match_length = *p_max_match_length;
  uint32_t *match_strings = *p_match_strings;
  uint8_t fast_section = *p_fast_section;
  uint8_t fast_sections = *p_fast_sections;
  uint8_t section_repeats = *p_section_repeats;
  float prior_min_score = *p_prior_min_score;
  float fast_min_score = *p_fast_min_score;
  float new_min_score = *p_new_min_score;
  uint32_t symbol;

  pthread_t overlap_check_threads[7];
  uint32_t new_rule_number[0x8000];
  uint32_t *out_symbol_ptr;
  uint32_t *node_string_start_ptr;
  uint32_t *match_string_start_ptr;
  uint32_t *search_match_ptr;
  uint32_t *block_ptr;
  uint32_t *stop_symbol_ptr;
  size_t block_size;
  uint32_t num_overlaps;
  uint32_t num_prior_matches;
  uint32_t node_score_number;
  uint32_t best_score_num_symbols;
  uint32_t suffix_node_number;
  uint32_t prior_match_score_number[MAX_PRIOR_MATCHES];
  uint32_t *prior_match_end_ptr[MAX_PRIOR_MATCHES];
  struct match_node *match_node_ptr;
  uint8_t *free_RAM_ptr;
  uint32_t *stop_matches_symbol_ptr[8];

  free_RAM_ptr = (char *)(((size_t)end_symbol_ptr + 8) & ~7);
  uintptr_t match_region_end_limit = (uintptr_t)end_RAM_ptr;
  if (nodes != 0 && (uintptr_t)nodes < match_region_end_limit) {
    match_region_end_limit = (uintptr_t)nodes;
  }
  struct node_score_data *tmp_candidates =
      (struct node_score_data *)free_RAM_ptr;
  memcpy(&tmp_candidates[0], &candidates[0],
         MAX_SCORES_FAST * sizeof(struct node_score_data));
  for (uint16_t candidate_num = 0; candidate_num < MAX_SCORES_FAST;
       candidate_num++) {
    memcpy(&candidates[candidate_num],
           &tmp_candidates[candidates_index[candidate_num]],
           sizeof(struct node_score_data));
  }

  if (fast_mode == 0) {
#ifdef PRINTON
    fprintf(stderr, " score[0-%hu] = %.5f-%.5f\n",
            (unsigned short int)num_candidates - 1, candidates[0].score,
            candidates[num_candidates - 1].score);
#endif
    if (candidates[num_candidates - 1].score <
        (0.1 * candidates[0].score) - 1.0) {
      size_t candidate_num = 1;
      while ((candidate_num + 0x100 < num_candidates) &&
             (candidates[candidate_num + 0x100].score >=
              (0.1 * candidates[0].score) - 1.0)) {
        candidate_num += 0x100;
      }
      while (candidate_num < num_candidates) {
        if (candidates[candidate_num].score <
            (0.1 * candidates[0].score) - 1.0) {
          num_candidates = candidate_num;
          break;
        }
        candidate_num++;
      }
    }
  } else if (fast_sections != 1) {
    section_scores[fast_section] = candidates[num_candidates - 1].score;
    uint8_t old_fast_section = fast_section;
    if (++fast_section == fast_sections) {
      fast_section = 0;
    }
    if (candidates[num_candidates - 1].score < fast_min_score) {
      if (fast_sections == 23) {
        fast_sections = 9;
        fast_section = 0;
        min_score = 8.0;
      } else {
        fast_sections = (fast_sections + 1) >> 1;
        fast_section >>= 1;
        min_score = 4.0;
      }
      section_repeats = 0;
      for (size_t i = 0; i < fast_sections; i++) {
        section_scores[i] = BIG_FLOAT;
      }
      prior_min_score = BIG_FLOAT;
      fast_min_score = 1.0;
      scan_mode = GLZA_SCAN_GENERAL;
    } else {
      size_t i = fast_section + 1 == fast_sections ? 0 : fast_section + 1;
      while (i != old_fast_section) {
        if (section_scores[i] > section_scores[fast_section]) {
          fast_section = i;
        }
        if (++i == fast_sections) {
          i = 0;
        }
      }
      if ((section_repeats < 2) &&
          (section_scores[old_fast_section] > section_scores[fast_section])) {
        fast_section = old_fast_section;
        section_repeats++;
      } else {
        section_repeats = 0;
      }
    }
  }

  if (next_new_symbol_number + num_candidates > max_rules) {
    num_candidates = max_rules > next_new_symbol_number
                         ? max_rules - next_new_symbol_number
                         : 0;
  }

  // build a prefix tree of the match strings
  child_ptr_array = (struct match_node **)free_RAM_ptr;
  struct match_node *match_nodes =
      (struct match_node *)(free_RAM_ptr + (sizeof(struct match_node *) *
                                            next_new_symbol_number));
  uint32_t match_nodes_limit =
      (uint32_t)((match_region_end_limit - (uintptr_t)match_nodes) /
                 sizeof(struct match_node));
  num_match_nodes = 0;
  max_match_length = 0;
  {
    uint16_t candidate_num = 0;
    while (candidate_num < num_candidates) {
      if ((uintptr_t)match_nodes +
              ((num_match_nodes + 1) * sizeof(struct match_node)) +
              ((uintptr_t)(candidate_num + 1) * max_match_length *
               sizeof(uint32_t)) >=
          match_region_end_limit) {
        glza_warn_match_nodes_limit();
        num_candidates = candidate_num != 0 ? candidate_num - 1 : 0;
        break;
      }
      uint32_t *best_score_last_match_ptr;
      uint32_t *best_score_match_ptr;
      if (candidates[candidate_num].num_symbols > max_match_length) {
        max_match_length = candidates[candidate_num].num_symbols;
      }
      best_score_last_match_ptr =
          start_symbol_ptr + candidates[candidate_num].last_match_index;
      best_score_match_ptr =
          best_score_last_match_ptr - candidates[candidate_num].num_symbols + 1;
      if (num_match_nodes == 0) {
        init_match_node(match_nodes, *best_score_match_ptr, 0, candidate_num);
        num_match_nodes = 1;
      }
      match_node_ptr = match_nodes;
      while (best_score_match_ptr <= best_score_last_match_ptr) {
        symbol = *best_score_match_ptr;
        if (match_node_ptr->child_ptr == 0) {
          if (num_match_nodes >= match_nodes_limit) {
            glza_warn_match_nodes_limit();
            candidate_bad[candidate_num] = 1;
            break;
          }
          match_node_ptr->child_ptr = &match_nodes[num_match_nodes++];
          match_node_ptr = match_node_ptr->child_ptr;
          init_match_node(match_node_ptr, symbol, 0, candidate_num);
        } else {
          match_node_ptr = match_node_ptr->child_ptr;
          uint8_t sibling_number;
          if (move_to_match_sibling(match_nodes, &match_node_ptr, symbol,
                                    &sibling_number) != 0) {
            if (match_node_ptr->child_ptr == 0) {
              candidate_bad[candidate_num] = 1;
              break;
            }
          } else {
            if (num_match_nodes >= match_nodes_limit) {
              glza_warn_match_nodes_limit();
              candidate_bad[candidate_num] = 1;
              break;
            }
            match_node_ptr->sibling_node_num[sibling_number] = num_match_nodes;
            match_node_ptr = &match_nodes[num_match_nodes++];
            init_match_node(match_node_ptr, symbol, 0, candidate_num);
          }
        }
        best_score_match_ptr++;
      }
      if (match_node_ptr->child_ptr != 0) {
        candidate_bad[candidate_num] = 1;
      }
      candidate_num++;
    }
  }

  // for each candidate, search substrings for matches with other candidates, if
  // found invalidate lower score
  {
    uint16_t candidate_num = 0;
    while (candidate_num < num_candidates) {
      uint32_t *best_score_last_match_ptr;
      uint32_t *best_score_match_ptr;
      best_score_last_match_ptr =
          start_symbol_ptr + candidates[candidate_num].last_match_index;
      best_score_match_ptr =
          best_score_last_match_ptr - candidates[candidate_num].num_symbols + 1;
      // read the first symbol
      symbol = *best_score_match_ptr++;
      match_node_ptr = &match_nodes[1];
      move_to_existing_match_sibling(match_nodes, &match_node_ptr, symbol);
      while (best_score_match_ptr <= best_score_last_match_ptr) {
        // starting with the second symbol, look for suffixes that are in the
        // prefix tree
        search_match_ptr = best_score_match_ptr;
        struct match_node *search_node_ptr = match_nodes;
        while (1) { // follow the tree until find child = 0 or sibling = 0
          if (search_node_ptr->child_ptr ==
              0) { // found a scored string that is a substring of this string
            if (fast_mode == 0) {
              if (search_node_ptr->score_number > candidate_num) {
                candidate_bad[search_node_ptr->score_number] = 1;
              } else if (search_node_ptr->score_number != candidate_num) {
                candidate_bad[candidate_num] = 1;
              }
            } else {
              // uint16_t [l, r] = std::minmax(candidate_num,
              // search_node_ptr->score_number); if (l != r && candidate_bad[l]
              // == 0) {
              //   candidate_bad[r] = 1;
              // }
              uint16_t a = candidate_num;
              uint16_t b = search_node_ptr->score_number;
              if (b > a) {
                if (candidate_bad[a] == 0) {
                  candidate_bad[b] = 1;
                }
              } else if (b < a) {
                if (candidate_bad[b] == 0) {
                  candidate_bad[a] = 1;
                }
              }
            }
            break;
          }
          search_node_ptr = search_node_ptr->child_ptr;
          symbol = *search_match_ptr;
          if (move_to_search_sibling(match_nodes, symbol, &search_node_ptr) ==
              0) {
            break;
          }
          match_node_ptr->miss_ptr = search_node_ptr;
          search_match_ptr++;
        }
        symbol = *best_score_match_ptr++;
      }
      candidate_num++;
    }
  }

  // Redo the tree build and miss values with just the valid score symbols
  match_node_ptr = match_nodes + next_new_symbol_number;
  num_match_nodes = 0;
  {
    size_t j = next_new_symbol_number;
    while (j-- != 0) {
      child_ptr_array[j] = 0;
    }
  }
  {
    uint16_t candidate_num = 0;
    while (candidate_num < num_candidates) {
      if (candidate_bad[candidate_num] == 0) {
        uint32_t *best_score_last_match_ptr;
        uint32_t *best_score_match_ptr;
        best_score_last_match_ptr =
            start_symbol_ptr + candidates[candidate_num].last_match_index;
        best_score_match_ptr = best_score_last_match_ptr -
                               candidates[candidate_num].num_symbols + 1;
        struct match_node **child_ptr_ptr =
            &child_ptr_array[*best_score_match_ptr++];
        symbol = *best_score_match_ptr++;
        best_score_num_symbols = 2;
        match_node_ptr = move_to_base_match_child_with_make(
            match_nodes, symbol, candidate_num, &num_match_nodes,
            child_ptr_ptr);
        while (best_score_match_ptr <= best_score_last_match_ptr) {
          move_to_match_child_with_make(
              match_nodes, &match_node_ptr, *best_score_match_ptr++,
              candidate_num, ++best_score_num_symbols, &num_match_nodes);
        }
      }
      candidate_num++;
    }
  }

  // span nodes entering the longest (first) suffix match for each node
  {
    uint16_t candidate_num = 0;
    while (candidate_num < num_candidates) {
      if (candidate_bad[candidate_num] == 0) {
        uint32_t *best_score_last_match_ptr;
        uint32_t *best_score_suffix_ptr;
        best_score_last_match_ptr =
            start_symbol_ptr + candidates[candidate_num].last_match_index;
        best_score_suffix_ptr = best_score_last_match_ptr -
                                candidates[candidate_num].num_symbols + 1;
        suffix_node_number =
            child_ptr_array[*best_score_suffix_ptr++] - match_nodes;
        // starting at the node of the 2nd symbol in string, match strings with
        // prefix tree until no match found,
        //   for each match node found, if suffix miss symbol is zero, set it to
        //   the tree symbol node
        while (best_score_suffix_ptr <= best_score_last_match_ptr) {
          // follow the suffix until the end (or break on no tree matches)
          symbol = *best_score_suffix_ptr++;
          uint32_t shifted_symbol = symbol;
          while (symbol != match_nodes[suffix_node_number].symbol) {
            suffix_node_number = match_nodes[suffix_node_number]
                                     .sibling_node_num[shifted_symbol & 0xF];
            shifted_symbol >>= 4;
          }
          match_node_ptr = &match_nodes[suffix_node_number];
          uint32_t *best_score_match_ptr;
          best_score_match_ptr = best_score_suffix_ptr;
          if (symbol < child_ptr_array_size && child_ptr_array[symbol] != 0) {
            if ((match_node_ptr->child_ptr != 0) &&
                (match_node_ptr->child_ptr->miss_ptr == 0)) {
              write_siblings_miss_ptr(match_nodes, match_node_ptr->child_ptr,
                                      child_ptr_array[symbol]);
            }
            struct match_node *search_node_ptr = child_ptr_array[symbol];
            while (best_score_match_ptr <= best_score_last_match_ptr) {
              // follow the tree until end of match string or find child = 0 or
              // sibling = 0
              symbol = *best_score_match_ptr++;
              match_node_ptr = match_node_ptr->child_ptr;
              move_to_existing_match_sibling(match_nodes, &match_node_ptr,
                                             symbol);
              if (move_to_search_sibling(match_nodes, symbol,
                                         &search_node_ptr) == 0) {
                break;
              }
              if (match_node_ptr->child_ptr == 0) {
                if (match_node_ptr->hit_ptr == 0) {
                  match_node_ptr->hit_ptr = search_node_ptr;
                }
              } else if (match_node_ptr->child_ptr->miss_ptr == 0) {
                write_siblings_miss_ptr(match_nodes, match_node_ptr->child_ptr,
                                        search_node_ptr->child_ptr);
              }
              if (search_node_ptr->child_ptr == 0) {
                // no child, so done with this suffix
                break;
              }
              search_node_ptr = search_node_ptr->child_ptr;
            }
          }
          suffix_node_number =
              match_nodes[suffix_node_number].child_ptr - match_nodes;
        }
      }
      candidate_num++;
    }
  }

  // save the match strings so they can be added to the end of the data after
  // symbol substitution is done
  match_strings =
      (uint32_t *)((size_t)match_nodes +
                   ((size_t)num_match_nodes * sizeof(struct match_node)));
  overlap_check_data =
      (struct overlap_check
           *)(((uintptr_t)&match_strings[num_candidates * max_match_length] +
               7) &
              ~7);
  if ((uintptr_t)overlap_check_data + (8 * sizeof(struct overlap_check)) >
      match_region_end_limit) {
    if (overlap_check_heap_buf == 0) {
      overlap_check_heap_buf =
          (struct overlap_check *)malloc(8 * sizeof(struct overlap_check));
      if (overlap_check_heap_buf == 0) {
        fprintf(stderr, "ERROR - overlap_check memory allocation failed\n");
        exit(1);
      }
    }
    overlap_check_data = overlap_check_heap_buf;
  }
  for (size_t i = 1; i < 8; i++) {
    overlap_check_data[i].candidate_bad = &candidate_bad[0];
  }

  {
    uint16_t candidate_num = 0;
    while (candidate_num < num_candidates) {
      if (candidate_bad[candidate_num] == 0) {
        match_string_start_ptr =
            &match_strings[candidate_num * max_match_length];
        node_string_start_ptr = start_symbol_ptr +
                                candidates[candidate_num].last_match_index -
                                candidates[candidate_num].num_symbols + 1;
        for (size_t j = 0; j < candidates[candidate_num].num_symbols; j++) {
          *(match_string_start_ptr + j) = *(node_string_start_ptr + j);
        }
      }
      candidate_num++;
    }
  }

  uint32_t *matches_start_ptr[8];
  uint32_t *next_match_start_ptr[8];
  uint32_t *matches_stop_ptr[8];
  uintptr_t match_strings_end =
      ((uintptr_t)&match_strings[num_candidates * max_match_length] + 7) & ~7;
  uint32_t *begin_matches;
  uint32_t *matches_end_ptr;
  if (overlap_check_data == overlap_check_heap_buf) {
    begin_matches = (uint32_t *)match_strings_end;
    matches_end_ptr = (uint32_t *)match_region_end_limit;
  } else {
    begin_matches = (uint32_t *)((uintptr_t)overlap_check_data +
                                 (8 * sizeof(struct overlap_check)));
    matches_end_ptr = (uint32_t *)end_RAM_ptr;
  }
  if (begin_matches >= matches_end_ptr) {
    begin_matches = matches_end_ptr;
  }
  {
    uint32_t matches_stride =
        (uint32_t)((matches_end_ptr - begin_matches) >> 3);
    for (size_t i = 0; i < 8; i++) {
      next_match_start_ptr[i] = matches_start_ptr[i] =
          begin_matches + (i * matches_stride);
      matches_stop_ptr[i] = begin_matches + ((i + 1) * matches_stride);
      next_match_ptr[i] = next_match_start_ptr[i];
    }
  }

  if (fast_mode == 0) {
#ifdef PRINTON
    fprintf(stderr, "Overlap search\r");
#endif
  }
  block_size = num_file_symbols >> 3;
  block_ptr = start_symbol_ptr + block_size;
  stop_matches_symbol_ptr[0] = block_ptr;
  stop_symbol_ptr = block_ptr + MAX_MATCH_LENGTH;
  if (stop_symbol_ptr >= end_symbol_ptr) {
    stop_symbol_ptr = end_symbol_ptr;
    stop_matches_symbol_ptr[0] = end_symbol_ptr;
  } else {
    for (size_t i = 1; i < 8; i++) {
      overlap_check_data[i].start_symbol_ptr = block_ptr;
      block_ptr += block_size;
      if (i < 7) {
        overlap_check_data[i].stop_matches_symbol_ptr = block_ptr;
        overlap_check_data[i].stop_symbol_ptr = block_ptr + MAX_MATCH_LENGTH;
        if (overlap_check_data[i].stop_symbol_ptr > end_symbol_ptr) {
          overlap_check_data[i].stop_symbol_ptr = end_symbol_ptr;
        }
      } else {
        overlap_check_data[7].stop_matches_symbol_ptr = end_symbol_ptr;
        overlap_check_data[7].stop_symbol_ptr = end_symbol_ptr;
      }
      stop_matches_symbol_ptr[i] =
          overlap_check_data[i].stop_matches_symbol_ptr;
      overlap_check_data[i].next_match_ptr_ptr = &next_match_ptr[i];
      overlap_check_data[i].match_stop_ptr = matches_stop_ptr[i];
      overlap_check_data[i].num_overlaps = num_candidates;
      overlap_check_data[i].match_nodes = match_nodes;
      if (overlap_check_data[i].stop_symbol_ptr - start_symbol_ptr +
              MAX_MATCH_LENGTH <
          first_define_index) {
        pthread_create(&overlap_check_threads[i - 1], NULL,
                       overlap_check_no_defs_thread,
                       (void *)&overlap_check_data[i]);
      } else {
        pthread_create(&overlap_check_threads[i - 1], NULL,
                       overlap_check_thread, (void *)&overlap_check_data[i]);
      }
    }
  }

  num_overlaps = num_candidates;
  overlap_check_data[0].match_stop_ptr = matches_stop_ptr[0];
  for (size_t j = 0; j < num_candidates; j++) {
    overlap_check_data[0].next[j] = -1;
  }

  // scan the data, following prefix tree
  uint8_t found_same_score_prior_match;
  uint32_t prior_match_number;
  num_prior_matches = 0;
  in_symbol_ptr = start_symbol_ptr;

  if (stop_symbol_ptr - start_symbol_ptr + MAX_MATCH_LENGTH >=
      first_define_index) {
  main_overlap_check_loop_no_match:
    symbol = *in_symbol_ptr++;
    if (in_symbol_ptr >= stop_symbol_ptr) {
      goto main_overlap_check_loop_end;
    }
    if (((int32_t)symbol < 0) || (symbol >= child_ptr_array_size) ||
        (child_ptr_array[symbol] == 0)) {
      goto main_overlap_check_loop_no_match;
    }
    match_node_ptr = child_ptr_array[symbol];
  main_overlap_check_loop_match:
    symbol = *in_symbol_ptr++;
    if (symbol != match_node_ptr->symbol) {
      uint32_t shifted_symbol = symbol;
      do {
        if (match_node_ptr->sibling_node_num[shifted_symbol & 0xF] != 0) {
          match_node_ptr =
              &match_nodes[match_node_ptr
                               ->sibling_node_num[shifted_symbol & 0xF]];
          shifted_symbol >>= 4;
        } else {
          if (match_node_ptr->miss_ptr == 0) {
            if (((int32_t)symbol < 0) || (symbol >= child_ptr_array_size) ||
                (child_ptr_array[symbol] == 0)) {
              goto main_overlap_check_loop_no_match;
            }
            match_node_ptr = child_ptr_array[symbol];
            goto main_overlap_check_loop_match;
          } else {
            match_node_ptr = match_node_ptr->miss_ptr;
            shifted_symbol = symbol;
          }
        }
      } while (symbol != match_node_ptr->symbol);
    }
    if (match_node_ptr->child_ptr != 0) {
      match_node_ptr = match_node_ptr->child_ptr;
      goto main_overlap_check_loop_match;
    }

    // no child, so found a match - check for overlaps
    node_score_number = match_node_ptr->score_number;
    if ((in_symbol_ptr - match_node_ptr->num_symbols <
         stop_matches_symbol_ptr[0]) &&
        (candidate_bad[node_score_number] == 0) &&
        (next_match_ptr[0] + 2 <= matches_stop_ptr[0])) {
      *next_match_ptr[0] = node_score_number;
      next_match_ptr[0]++;
      *next_match_ptr[0] =
          in_symbol_ptr - start_symbol_ptr - match_node_ptr->num_symbols;
      next_match_ptr[0]++;
    }

    if ((num_prior_matches != 0) &&
        (in_symbol_ptr - match_node_ptr->num_symbols <=
         prior_match_end_ptr[num_prior_matches - 1])) {
      if (num_prior_matches == 1) {
        if (prior_match_score_number[0] != node_score_number) {
          if (fast_mode == 0) {
            if (prior_match_score_number[0] > node_score_number) {
              candidate_bad[prior_match_score_number[0]] = 1;
            } else {
              candidate_bad[node_score_number] = 1;
            }
          } else {
            uint32_t low_score;
            uint32_t high_score;
            if (node_score_number < prior_match_score_number[0]) {
              low_score = node_score_number;
              high_score = prior_match_score_number[0];
            } else {
              low_score = prior_match_score_number[0];
              high_score = node_score_number;
            }
            int32_t *next_overlap_num_ptr =
                &overlap_check_data[0].next[low_score];
            while ((*next_overlap_num_ptr != -1) &&
                   (overlap_check_data[0].second[*next_overlap_num_ptr] <
                    high_score)) {
              next_overlap_num_ptr =
                  &overlap_check_data[0].next[*next_overlap_num_ptr];
            }
            if ((*next_overlap_num_ptr == -1) ||
                (overlap_check_data[0].second[*next_overlap_num_ptr] !=
                 high_score)) {
              if (num_overlaps < 150000) {
                overlap_check_data[0].second[num_overlaps] = high_score;
                overlap_check_data[0].next[num_overlaps] =
                    *next_overlap_num_ptr;
                *next_overlap_num_ptr = num_overlaps++;
              } else {
                candidate_bad[high_score] = 1;
              }
            }
          }
          prior_match_end_ptr[1] = in_symbol_ptr - 1;
          prior_match_score_number[1] = node_score_number;
          num_prior_matches = 2;
        }
      } else {
        prior_match_number = 0;
        found_same_score_prior_match = 0;
        do {
          if (in_symbol_ptr - match_node_ptr->num_symbols >
              prior_match_end_ptr[prior_match_number]) {
            num_prior_matches--;
            for (size_t j = prior_match_number; j < num_prior_matches; j++) {
              prior_match_end_ptr[j] = prior_match_end_ptr[j + 1];
              prior_match_score_number[j] = prior_match_score_number[j + 1];
            }
          } else { // overlapping symbol substitution strings, so invalidate the
                   // lower score
            if (prior_match_score_number[prior_match_number] ==
                node_score_number) {
              found_same_score_prior_match = 1;
            } else if (fast_mode == 0) {
              if (prior_match_score_number[prior_match_number] >
                  node_score_number) {
                candidate_bad[prior_match_score_number[prior_match_number]] = 1;
              } else {
                candidate_bad[node_score_number] = 1;
              }
            } else {
              uint32_t low_score;
              uint32_t high_score;
              if (node_score_number <
                  prior_match_score_number[prior_match_number]) {
                low_score = node_score_number;
                high_score = prior_match_score_number[prior_match_number];
              } else {
                low_score = prior_match_score_number[prior_match_number];
                high_score = node_score_number;
              }
              int32_t *next_overlap_num_ptr =
                  &overlap_check_data[0].next[low_score];
              while ((*next_overlap_num_ptr != -1) &&
                     (overlap_check_data[0].second[*next_overlap_num_ptr] <
                      high_score)) {
                next_overlap_num_ptr =
                    &overlap_check_data[0].next[*next_overlap_num_ptr];
              }
              if ((*next_overlap_num_ptr == -1) ||
                  (overlap_check_data[0].second[*next_overlap_num_ptr] !=
                   high_score)) {
                if (num_overlaps < 150000) {
                  overlap_check_data[0].second[num_overlaps] = high_score;
                  overlap_check_data[0].next[num_overlaps] =
                      *next_overlap_num_ptr;
                  *next_overlap_num_ptr = num_overlaps++;
                } else {
                  candidate_bad[high_score] = 1;
                }
              }
            }
            prior_match_number++;
          }
        } while (prior_match_number < num_prior_matches);
        if (found_same_score_prior_match == 0) {
          prior_match_end_ptr[num_prior_matches] = in_symbol_ptr - 1;
          prior_match_score_number[num_prior_matches++] = node_score_number;
        }
      }
    } else {
      num_prior_matches = 1;
      prior_match_end_ptr[0] = in_symbol_ptr - 1;
      prior_match_score_number[0] = node_score_number;
    }
    match_node_ptr = match_node_ptr->hit_ptr;
    if (match_node_ptr == 0) {
      if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
          child_ptr_array[symbol] == 0) {
        goto main_overlap_check_loop_no_match;
      }
      match_node_ptr = child_ptr_array[symbol];
      goto main_overlap_check_loop_match;
    }
    match_node_ptr = match_node_ptr->child_ptr;
    goto main_overlap_check_loop_match;
  } else {
  main_overlap_check_no_defs_loop_no_match:
    symbol = *in_symbol_ptr++;
    if (in_symbol_ptr >= stop_symbol_ptr) {
      goto main_overlap_check_loop_end;
    }
    if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
        child_ptr_array[symbol] == 0) {
      goto main_overlap_check_no_defs_loop_no_match;
    }
    match_node_ptr = child_ptr_array[symbol];
  main_overlap_check_no_defs_loop_match:
    symbol = *in_symbol_ptr++;
    if (symbol != match_node_ptr->symbol) {
      uint32_t shifted_symbol = symbol;
      do {
        if (match_node_ptr->sibling_node_num[shifted_symbol & 0xF] != 0) {
          match_node_ptr =
              &match_nodes[match_node_ptr
                               ->sibling_node_num[shifted_symbol & 0xF]];
          shifted_symbol >>= 4;
        } else if (match_node_ptr->miss_ptr == 0) {
          if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
              child_ptr_array[symbol] == 0) {
            goto main_overlap_check_no_defs_loop_no_match;
          }
          if (in_symbol_ptr <= stop_symbol_ptr) {
            match_node_ptr = child_ptr_array[symbol];
            goto main_overlap_check_no_defs_loop_match;
          }
          goto main_overlap_check_loop_end;
        } else {
          match_node_ptr = match_node_ptr->miss_ptr;
          shifted_symbol = symbol;
        }
      } while (symbol != match_node_ptr->symbol);
    }
    if (match_node_ptr->child_ptr != 0) {
      if (in_symbol_ptr > stop_symbol_ptr &&
          in_symbol_ptr - match_node_ptr->num_symbols >= stop_symbol_ptr) {
        goto main_overlap_check_loop_end;
      }
      match_node_ptr = match_node_ptr->child_ptr;
      goto main_overlap_check_no_defs_loop_match;
    }

    // no child, so found a match - check for overlaps
    node_score_number = match_node_ptr->score_number;
    if ((in_symbol_ptr - match_node_ptr->num_symbols <
         stop_matches_symbol_ptr[0]) &&
        (candidate_bad[node_score_number] == 0) &&
        (next_match_ptr[0] + 2 <= matches_stop_ptr[0])) {
      *next_match_ptr[0] = node_score_number;
      next_match_ptr[0]++;
      *next_match_ptr[0] =
          in_symbol_ptr - start_symbol_ptr - match_node_ptr->num_symbols;
      next_match_ptr[0]++;
    }

    if ((num_prior_matches != 0) &&
        (in_symbol_ptr - match_node_ptr->num_symbols <=
         prior_match_end_ptr[num_prior_matches - 1])) {
      if (num_prior_matches == 1) {
        if (prior_match_score_number[0] != node_score_number) {
          if (fast_mode == 0) {
            if (prior_match_score_number[0] > node_score_number) {
              candidate_bad[prior_match_score_number[0]] = 1;
            } else {
              candidate_bad[node_score_number] = 1;
            }
          } else {
            uint32_t low_score;
            uint32_t high_score;
            if (node_score_number < prior_match_score_number[0]) {
              low_score = node_score_number;
              high_score = prior_match_score_number[0];
            } else {
              low_score = prior_match_score_number[0];
              high_score = node_score_number;
            }
            int32_t *next_overlap_num_ptr =
                &overlap_check_data[0].next[low_score];
            while ((*next_overlap_num_ptr != -1) &&
                   (overlap_check_data[0].second[*next_overlap_num_ptr] <
                    high_score)) {
              next_overlap_num_ptr =
                  &overlap_check_data[0].next[*next_overlap_num_ptr];
            }
            if ((*next_overlap_num_ptr == -1) ||
                (overlap_check_data[0].second[*next_overlap_num_ptr] !=
                 high_score)) {
              if (num_overlaps < 150000) {
                overlap_check_data[0].second[num_overlaps] = high_score;
                overlap_check_data[0].next[num_overlaps] =
                    *next_overlap_num_ptr;
                *next_overlap_num_ptr = num_overlaps++;
              } else {
                candidate_bad[high_score] = 1;
              }
            }
          }
          prior_match_end_ptr[1] = in_symbol_ptr - 1;
          prior_match_score_number[1] = node_score_number;
          num_prior_matches = 2;
        }
      } else {
        prior_match_number = 0;
        found_same_score_prior_match = 0;
        do {
          if (in_symbol_ptr - match_node_ptr->num_symbols >
              prior_match_end_ptr[prior_match_number]) {
            num_prior_matches--;
            for (size_t j = prior_match_number; j < num_prior_matches; j++) {
              prior_match_end_ptr[j] = prior_match_end_ptr[j + 1];
              prior_match_score_number[j] = prior_match_score_number[j + 1];
            }
          } else { // overlapping symbol substitution strings, so invalidate the
                   // lower score
            if (prior_match_score_number[prior_match_number] ==
                node_score_number) {
              found_same_score_prior_match = 1;
            } else if (fast_mode == 0) {
              if (prior_match_score_number[prior_match_number] >
                  node_score_number) {
                candidate_bad[prior_match_score_number[prior_match_number]] = 1;
              } else {
                candidate_bad[node_score_number] = 1;
              }
            } else {
              uint32_t low_score;
              uint32_t high_score;
              if (node_score_number <
                  prior_match_score_number[prior_match_number]) {
                low_score = node_score_number;
                high_score = prior_match_score_number[prior_match_number];
              } else {
                low_score = prior_match_score_number[prior_match_number];
                high_score = node_score_number;
              }
              int32_t *next_overlap_num_ptr =
                  &overlap_check_data[0].next[low_score];
              while ((*next_overlap_num_ptr != -1) &&
                     (overlap_check_data[0].second[*next_overlap_num_ptr] <
                      high_score)) {
                next_overlap_num_ptr =
                    &overlap_check_data[0].next[*next_overlap_num_ptr];
              }
              if ((*next_overlap_num_ptr == -1) ||
                  (overlap_check_data[0].second[*next_overlap_num_ptr] !=
                   high_score)) {
                if (num_overlaps < 150000) {
                  overlap_check_data[0].second[num_overlaps] = high_score;
                  overlap_check_data[0].next[num_overlaps] =
                      *next_overlap_num_ptr;
                  *next_overlap_num_ptr = num_overlaps++;
                } else {
                  candidate_bad[high_score] = 1;
                }
              }
            }
            prior_match_number++;
          }
        } while (prior_match_number < num_prior_matches);
        if (found_same_score_prior_match == 0) {
          prior_match_end_ptr[num_prior_matches] = in_symbol_ptr - 1;
          prior_match_score_number[num_prior_matches++] = node_score_number;
        }
      }
    } else {
      num_prior_matches = 1;
      prior_match_end_ptr[0] = in_symbol_ptr - 1;
      prior_match_score_number[0] = node_score_number;
    }
    match_node_ptr = match_node_ptr->hit_ptr;
    if (match_node_ptr == 0) {
      if ((int32_t)symbol < 0 || symbol >= child_ptr_array_size ||
          child_ptr_array[symbol] == 0) {
        goto main_overlap_check_no_defs_loop_no_match;
      }
      match_node_ptr = child_ptr_array[symbol];
      goto main_overlap_check_no_defs_loop_match;
    }
    if ((in_symbol_ptr <= stop_symbol_ptr) ||
        (in_symbol_ptr - match_node_ptr->num_symbols < stop_symbol_ptr)) {
      match_node_ptr = match_node_ptr->child_ptr;
      goto main_overlap_check_no_defs_loop_match;
    }
  }

main_overlap_check_loop_end:
  if (stop_symbol_ptr < end_symbol_ptr) {
    for (size_t i = 1; i < 8; i++) {
      pthread_join(overlap_check_threads[i - 1], NULL);
    }
    if (fast_mode == 1) {
      for (uint16_t candidate_num = 0; candidate_num < num_candidates - 1;
           candidate_num++) {
        if (candidate_bad[candidate_num] == 0) {
          for (size_t j = 0; j < 8; j++) {
            int32_t next_overlap_num =
                overlap_check_data[j].next[candidate_num];
            while (next_overlap_num != -1) {
              candidate_bad[overlap_check_data[j].second[next_overlap_num]] =
                  -1;
              next_overlap_num = overlap_check_data[j].next[next_overlap_num];
            }
          }
        }
      }
    }
  } else if (fast_mode == 1) {
    for (uint16_t candidate_num = 0; candidate_num < num_candidates - 1;
         candidate_num++) {
      if (candidate_bad[candidate_num] == 0) {
        int32_t next_overlap_num = overlap_check_data[0].next[candidate_num];
        while (next_overlap_num != -1) {
          candidate_bad[overlap_check_data[0].second[next_overlap_num]] = -1;
          next_overlap_num = overlap_check_data[0].next[next_overlap_num];
        }
      }
    }
  }

  for (size_t i = 0, j = next_new_symbol_number; i < num_candidates; i++) {
    if (candidate_bad[i] == 0) {
      symbol_counts[j] = 0;
      new_rule_number[i] = j++;
    }
  }

  in_symbol_ptr = out_symbol_ptr = start_symbol_ptr;
  int32_t prior_match_end = -1;
  uint8_t max_i = stop_symbol_ptr < end_symbol_ptr ? 7 : 0;
  for (size_t i = 0; i <= max_i; i++) {
    for (size_t j = 0; j < (next_match_ptr[i] - next_match_start_ptr[i]) >> 1;
         j++) {
      uint16_t candidate_num = *(next_match_start_ptr[i] + (2 * j));
      if (candidate_bad[candidate_num] == 0) {
        uint32_t start_index = *(next_match_start_ptr[i] + (2 * j) + 1);
        if ((int32_t)start_index > prior_match_end) {
          prior_match_end =
              start_index + candidates[candidate_num].num_symbols - 1;
          uint32_t *start_ptr = start_symbol_ptr + start_index;
          while (in_symbol_ptr < start_ptr) {
            *out_symbol_ptr++ = *in_symbol_ptr++;
          }
          *out_symbol_ptr++ = new_rule_number[candidate_num];
          symbol_counts[new_rule_number[candidate_num]]++;
          in_symbol_ptr += candidates[candidate_num].num_symbols;
        }
      }
    }
  }
  while (in_symbol_ptr < end_symbol_ptr) {
    *out_symbol_ptr++ = *in_symbol_ptr++;
  }

  // Add new production rules and update symbol counts
  for (size_t i = 0; i < num_candidates; i++) {
    if (candidate_bad[i] == 0) {
      *out_symbol_ptr++ = num_rules + 0x80000001;
      uint32_t *match_string_ptr;
      uint32_t *match_string_end_ptr;
      match_string_ptr = match_strings + (max_match_length * i);
      match_string_end_ptr = match_string_ptr + candidates[i].num_symbols;
      uint32_t sym1 = *match_string_ptr++;
      *out_symbol_ptr++ = sym1;
      symbol_ends[num_terminals + num_rules].start = symbol_ends[sym1].start;
      uint32_t repeats = symbol_counts[num_terminals + num_rules] - 1;
      symbol_counts[sym1] -= repeats;
      while (match_string_ptr != match_string_end_ptr) {
        uint32_t sym2 = *match_string_ptr;
        symbol_counts[sym2] -= repeats;
        o1c[symbol_ends[sym1].end][symbol_ends[sym2].start] -= repeats;
        num_ends[symbol_ends[sym1].end] -= repeats;
        num_starts[symbol_ends[sym2].start] -= repeats;
        sym1 = sym2;
        *out_symbol_ptr++ = *match_string_ptr++;
      }
      symbol_ends[num_terminals + num_rules++].end = symbol_ends[sym1].end;
    } else {
      candidate_bad[i] = 0;
    }
  }

  if (num_rules == 0) {
    first_define_index = out_symbol_ptr - start_symbol_ptr;
  } else {
    if (out_symbol_ptr < start_symbol_ptr + first_define_index) {
      first_define_index = out_symbol_ptr - start_symbol_ptr;
    }
    if (*(start_symbol_ptr + first_define_index) != 0x80000001) {
      while (*(start_symbol_ptr + --first_define_index) != 0x80000001)
        ; // decrement index until found
    }
  }
  end_symbol_ptr = out_symbol_ptr;
  *end_symbol_ptr = 0xFFFFFFFE;
  num_file_symbols = end_symbol_ptr - start_symbol_ptr;

  if (fast_mode == 0) {
    if (scan_mode == GLZA_SCAN_RETRY) {
      if (rank_scores_data_ptr->num_candidates != 0) {
        if (rank_scores_data_ptr->num_candidates == (uint16_t)max_scores) {
          if (min_score < prior_min_score) {
            if (max_scores > 10000) {
              new_min_score = min_score + min_score - prior_min_score - 0.015;
            } else {
              new_min_score = min_score + min_score - prior_min_score - 0.1;
              if (new_min_score < min_score - 1.0) {
                new_min_score = min_score - 1.0;
              }
              if (new_min_score < 0.0) {
                new_min_score = 0.0;
              }
            }
            prior_min_score = min_score;
          } else {
            new_min_score = (0.5 * (prior_min_score + min_score)) - 0.1;
            if (new_min_score >= prior_min_score) {
              new_min_score = prior_min_score - 0.05;
            } else if (new_min_score < min_score) {
              new_min_score = min_score - 0.09;
            }
            prior_min_score =
                candidates[candidates_index[num_candidates - 1]].score;
          }
          min_score = new_min_score;
        } else if (min_score < prior_min_score) {
          new_min_score = min_score + min_score - prior_min_score - 0.15;
          prior_min_score = min_score;
          min_score = new_min_score;
        } else {
          new_min_score = min_score + min_score - prior_min_score - 0.15;
          min_score = new_min_score < prior_min_score ? new_min_score
                                                      : prior_min_score - 0.03;
        }
        if (min_score < 0.0) {
          min_score = 0.0;
        }
      } else if (min_score > 0.0) {
        prior_min_score = min_score;
        min_score = 0.0;
        num_candidates = 1;
      }
    } else {
      scan_mode = GLZA_SCAN_RETRY;
      prior_min_score = min_score;
      min_score = 0.25 * min_score;
      if (min_score < 10.0) {
        min_score = 10.0;
      }
    }
  } else if (scan_mode == GLZA_SCAN_RETRY) {
    if (num_candidates == (uint16_t)max_scores) {
      if (min_score < prior_min_score) {
        if (prior_min_score != BIG_FLOAT) {
          if (scan_cycle > 50) {
            if (scan_cycle > 100) {
              new_min_score =
                  max_scores == MAX_SCORES_FAST
                      ? (0.995 * min_score * (min_score / prior_min_score)) -
                            0.002
                      : (0.998 * min_score * (min_score / prior_min_score)) -
                            0.002;
            } else {
              new_min_score =
                  (0.99 * min_score * (min_score / prior_min_score)) - 0.002;
            }
          } else {
            new_min_score =
                (0.98 * min_score * (min_score / prior_min_score)) - 0.002;
          }
          prior_min_score = min_score;
          min_score = new_min_score;
        } else {
          prior_min_score = min_score;
          min_score *= 0.5;
        }
      } else {
        min_score = (0.95 * prior_min_score) - 0.002;
      }
    } else if (min_score < prior_min_score) {
      if (prior_min_score != BIG_FLOAT) {
        new_min_score =
            (0.95 * min_score * (min_score / prior_min_score)) - 0.002;
        prior_min_score = min_score;
        min_score = new_min_score;
      } else {
        prior_min_score = min_score;
        min_score *= 0.5;
      }
    } else {
      min_score = (0.95 * prior_min_score) - 0.002;
    }
    if (min_score > 0.9 * section_scores[fast_section]) {
      min_score = (0.9 * section_scores[fast_section] < fast_min_score) &&
                          (min_score >= fast_min_score)
                      ? fast_min_score
                      : 0.9 * section_scores[fast_section];
    } else if (min_score < 0.0) {
      min_score = 0.0;
    }
  } else {
    scan_mode = GLZA_SCAN_RETRY;
  }

  *p_num_candidates = num_candidates;
  *p_scan_mode = scan_mode;
  *p_num_rules = num_rules;
  *p_first_define_index = first_define_index;
  *p_in_symbol_ptr = in_symbol_ptr;
  *p_overlap_check_data = overlap_check_data;
  *p_overlap_check_heap_buf = overlap_check_heap_buf;
  *p_num_match_nodes = num_match_nodes;
  *p_max_match_length = max_match_length;
  *p_match_strings = match_strings;
  *p_fast_section = fast_section;
  *p_fast_sections = fast_sections;
  *p_section_repeats = section_repeats;
  *p_prior_min_score = prior_min_score;
  *p_fast_min_score = fast_min_score;
  *p_new_min_score = new_min_score;
}

static uint8_t
update_min_max_scores(uint32_t *ptr_max_scores, float *ptr_min_score,
                      float prior_min_score, uint16_t num_candidates,
                      uint32_t num_rules, uint32_t next_new_symbol_number,
                      uint32_t initial_max_scores, float fast_min_score,
                      uint8_t fast_sections) {
  uint32_t max_scores = *ptr_max_scores;
  uint32_t min_score = *ptr_min_score;

  if (fast_mode == 0) {
    uint32_t prior_max_scores = max_scores;
    // __jm__ how the hell does this work? why divide by 3???
    max_scores =
        (max_scores + 2 * (num_terminals + num_rules - next_new_symbol_number +
                           initial_max_scores)) /
        3;
    if (max_scores > MAX_SCORES) {
      max_scores = MAX_SCORES;
    }
    if (max_scores > prior_max_scores) {
      // __jm__ wtf?
      min_score -= (prior_min_score - min_score) * 25.0 *
                   (double)(max_scores - prior_max_scores) /
                   (double)prior_max_scores;
    }
    if (min_score < 0.0) {
      min_score = 0.0;
    }
  } else {
    if ((prior_min_score <= fast_min_score) && (fast_sections == 1)) {
      return 1;
    }
    max_scores = (20 * max_scores +
                  35 * (num_terminals + num_rules - next_new_symbol_number +
                        initial_max_scores)) >>
                 6;
    if (max_scores > MAX_SCORES_FAST) {
      max_scores = MAX_SCORES_FAST;
    }
  }
  if (max_scores > 100 * num_candidates) {
    max_scores = 100 * num_candidates;
  }

  *ptr_max_scores = max_scores;
  *ptr_min_score = min_score;
  return 0;
}

static void main_loop(uint32_t *p_num_rules, enum glza_scan_mode scan_mode,
                      const uint32_t num_terminals_used, uint8_t *end_RAM_ptr,
                      uint32_t **p_in_symbol_ptr, const uint8_t UTF8_compliant,
                      struct rank_scores_thread_data *rank_scores_data_ptr,
                      uint32_t max_scores, struct score_data *node_data,
                      const uint32_t max_rules, uint16_t *candidates_index,
                      uint8_t *candidate_bad, uint32_t first_define_index,
                      const uint32_t initial_max_scores,
                      const uint8_t max_terminal, uint32_t *new_symbol_number,
                      uint8_t fast_section, uint8_t fast_sections,
                      const double profit_ratio_power, float fast_min_score,
                      float section_scores[23], uint8_t section_repeats,
                      uint16_t *p_scan_cycle) {
  uint32_t num_match_nodes; /* allocated prefix-tree nodes this sub-pass;
                               checked vs match_nodes_limit */
  uint32_t next_node_num;   /* next free suffix-tree node slot; checked vs
                               main_nodes_limit / node_num_limit */
  uint32_t
      max_match_length;             /* longest candidate string this pass; sizes
                                       match_strings[num_candidates * max_match_length] */
  uint32_t *start_cycle_symbol_ptr; /* start of this compression cycle slice in
                                       grammar stream */
  uint32_t *match_strings;   /* saved RHS copies; array size num_candidates *
                                max_match_length (heap layout) */
  uint32_t *substitute_data; /* word-pass command buffer; capacity
                                substitute_data_limit (0x40000) */
  int32_t *base_node_child_num_ptr; /* cursor into base_nodes_child_node_num
                                       during word-tree init */
  uint16_t num_candidates; /* candidates accepted this pass (<= max_scores, <=
                              max_rules headroom) */
  uint16_t node_ptrs_num;  /* rank_scores_buffer fill level / sync counter for
                              rank thread */
  float new_min_score;     /* scratch while adapting min_score (GLZA_SCAN_RETRY
                              path) */
  float log2_num_symbols_plus_substitution_cost; /* log2(n)+1.4; set in
                                                    main_loop_init for
                                                    GLZA_SCAN_WORDS or
                                                    fast_mode==1 */
  float production_cost;  /* per-production overhead; same init branch as
                             log2_num_symbols_plus_substitution_cost */
  double order_0_entropy; /* fast_mode==0 min_score input for scan_mode0() */
  float new_symbol_cost
      [NUM_PRECALCULATED_SYMBOL_COSTS]; /* repeat-cost table; filled for
                                           GLZA_SCAN_WORDS or fast_mode==1 */
  struct tree_thread_data
      tree_thread_data[13]; /* parallel suffix-tree builders; 12 threads if
                               fast_mode==0 else 13 */

  uint32_t prior_cycle_symbols = num_file_symbols;
  float prior_min_score = BIG_FLOAT;
  float cycle_start_ratio = 0.0;
  float cycle_end_ratio = 1.0;
  size_t substitute_heap_size = 0; /* bytes allocated for substitute_heap_buf;
                                      only when num_file_symbols >= 1M */

  uint8_t *substitute_heap_buf =
      NULL; /* optional 0x800000/0x1000000 heap when grammar >= 1M symbols */
  struct find_substitutions_thread_data *find_substitutions_thread_data_buf =
      NULL; /* lazy alloc; num_file_symbols >= 100M only */
  struct find_substitutions_thread_data *find_substitutions_thread_data =
      NULL; /* active thread array (stack or buf) */
  struct overlap_check *overlap_check_heap_buf =
      NULL; /* malloc fallback when in-RAM overlap_check[] won't fit */
  struct substitute_thread_data
      substitute_thread_data; /* threaded word substitution in/out pointers */
  struct overlap_check
      *overlap_check_data; /* 8 structs inline or overlap_check_heap_buf;
                              second/next sized heuristically */

  pthread_t build_tree_threads[7]; /* fast_mode==0: 6 workers; fast_mode==1: 7
                                      workers */
  pthread_t rank_scores_thread1;   /* rank_scores_thread (slow) or
                                      rank_scores_thread_fast */

  uint32_t *in_symbol_ptr = *p_in_symbol_ptr;
  uint32_t num_rules = *p_num_rules;
  uint16_t scan_cycle = *p_scan_cycle;

  num_candidates = 1;
  do {
    uint32_t next_new_symbol_number; /* num_terminals + num_rules; sizes entropy
                                        tables and child_ptr_array this pass */
    double d_num_file_symbols; /* (double)num_file_symbols for scoring ratios */
    uint8_t *free_RAM_ptr;     /* bump pointer into start_symbol_ptr arena after
                                  entropy + tree metadata */
    double *symbol_entropy;    /* fast_mode==0 scoring; aliases start of in-RAM
                                  block (see main_loop_init) */
    float *symbol_entropy_f; /* fast_mode==1 (+ word pass) per-symbol -log2(p);
                                may share base with symbol_entropy */
    uint32_t node_num_limit; /* max suffix-tree nodes (= remaining RAM /
                                sizeof(struct node)) */

    main_loop_init(&next_new_symbol_number, num_rules, &d_num_file_symbols,
                   &free_RAM_ptr, &symbol_entropy, &symbol_entropy_f, scan_mode,
                   &log2_num_symbols_plus_substitution_cost, new_symbol_cost,
                   &production_cost, num_terminals_used, &scan_cycle,
                   end_RAM_ptr, &node_num_limit);

    if (scan_mode == GLZA_SCAN_WORDS) {
      scan_mode = GLZA_SCAN_RUN_DEDUP;
      scan_mode0(
          &base_node_child_num_ptr, &next_node_num, &in_symbol_ptr,
          UTF8_compliant, node_num_limit, symbol_entropy_f,
          &next_new_symbol_number, &node_ptrs_num, rank_scores_data_ptr,
          max_scores, &prior_cycle_symbols, order_0_entropy, d_num_file_symbols,
          &num_candidates, &substitute_heap_size, max_rules, &candidates_index,
          &substitute_heap_buf, &free_RAM_ptr, &substitute_data,
          &substitute_thread_data, (uintptr_t)end_RAM_ptr, &num_match_nodes,
          &max_match_length, &match_strings, &overlap_check_heap_buf,
          &candidate_bad, &num_rules, &find_substitutions_thread_data_buf,
          &find_substitutions_thread_data, &rank_scores_thread1, node_data,
          production_cost, log2_num_symbols_plus_substitution_cost,
          new_symbol_cost, &first_define_index, &overlap_check_data);
      num_candidates = 1;
      continue;
    }

    if (scan_mode == GLZA_SCAN_RUN_DEDUP) {
      scan_mode = GLZA_SCAN_GENERAL;
      if (scan_mode1(&in_symbol_ptr, next_new_symbol_number, max_rules,
                     &max_scores, initial_max_scores, max_terminal,
                     new_symbol_number, &num_rules, &first_define_index,
                     &prior_min_score) != 0) {
        num_candidates = 1;
        continue;
      }
    }

    cycle_start_ratio = update_cycle_start_ratio(
        fast_mode, cycle_start_ratio, cycle_end_ratio, fast_section,
        fast_sections, prior_cycle_symbols);
    start_cycle_symbol_ptr =
        start_symbol_ptr +
        (uint32_t)(cycle_start_ratio * (float)num_file_symbols);
    in_symbol_ptr = start_cycle_symbol_ptr;

    build_and_score_suffix_tree(
        &in_symbol_ptr, &next_node_num, &node_ptrs_num, &cycle_end_ratio,
        start_cycle_symbol_ptr, node_num_limit, next_new_symbol_number,
        num_rules, max_scores, cycle_start_ratio, fast_section, fast_sections,
        symbol_entropy, symbol_entropy_f, profit_ratio_power, production_cost,
        log2_num_symbols_plus_substitution_cost, new_symbol_cost,
        rank_scores_data_ptr, node_data, tree_thread_data, build_tree_threads,
        &rank_scores_thread1);
    num_candidates = rank_scores_data_ptr->num_candidates;
    prior_cycle_symbols = in_symbol_ptr - start_cycle_symbol_ptr;

    if (num_candidates == 0) {
      handle_zero_candidates(&scan_mode, &num_candidates, &prior_min_score,
                             &fast_section, &fast_sections, &fast_min_score);
    } else {
      process_ranked_candidates(
          &num_candidates, &scan_mode, next_new_symbol_number, max_rules,
          max_scores, &num_rules, &first_define_index, &in_symbol_ptr,
          candidates_index, candidate_bad, end_RAM_ptr, rank_scores_data_ptr,
          &overlap_check_data, &overlap_check_heap_buf, &num_match_nodes,
          &max_match_length, &match_strings, &fast_section, &fast_sections,
          &section_repeats, section_scores, &prior_min_score, &fast_min_score,
          &new_min_score, scan_cycle);
    }

    if (update_min_max_scores(&max_scores, &min_score, prior_min_score,
                              num_candidates, num_rules, next_new_symbol_number,
                              initial_max_scores, fast_min_score,
                              fast_sections) != 0) {
      break;
    }
  } while ((num_candidates != 0) && (num_terminals + num_rules < max_rules));

  *p_scan_cycle = scan_cycle;
  *p_num_rules = num_rules;
  *p_in_symbol_ptr = in_symbol_ptr;
  free(substitute_heap_buf);
  free(find_substitutions_thread_data_buf);
  free(overlap_check_heap_buf);
}

uint8_t GLZAcompress(size_t in_size, size_t *outsize_ptr, uint8_t **iobuf,
                     struct param_data *params) {
  const uint8_t INSERT_SYMBOL_CHAR = 0xFE;
  const uint8_t DEFINE_SYMBOL_CHAR = 0xFF;
  // struct word_tree_thread_data word_tree_thread_data[4]; // __jm__ why the
  // hell is this one unused???? pthread_t word_build_tree_threads[4];

  start_symbol_ptr = 0;
  symbol_counts = 0;
  score_map = 0;
  atomic_store_explicit(&rank_scores_write_index, 0, memory_order_relaxed);
  atomic_store_explicit(&rank_scores_read_index, 0, memory_order_relaxed);
  atomic_store_explicit(&substitute_data_write_index, 0, memory_order_relaxed);
  atomic_store_explicit(&substitute_data_read_index, 0, memory_order_relaxed);
  atomic_store_explicit(&max_symbol_ptr, 0, memory_order_relaxed);
  atomic_store_explicit(&scan_symbol_ptr, 0, memory_order_relaxed);
  memset(next_match_ptr, 0, 8 * sizeof(next_match_ptr[0]));

  uint64_t max_memory_usage =
      sizeof(uint32_t *) >= 8 ? 0x800000000 : 0x70000000;

  // uint8_t fast_mode; — file-global; set below from params (forced 0 when
  // in_size < 1000) double order_ratio; — file-global; from params->order
  double profit_ratio_power; /* exponent on profit ratio; default 1.0 (fast)
                                or 2.0 (slow) unless user-set */
  uint8_t create_words;      /* if 0, word pass skipped (initial_scan_mode =
                                GLZA_SCAN_RUN_DEDUP) */
  {
    if (params != 0) {
      if (params->user_set_profit_ratio_power != 0) {
        profit_ratio_power = params->profit_ratio_power;
      }
      create_words = params->create_words;
      fast_mode = in_size < 1000 ? 0 : params->fast_mode;
      order_ratio = params->order;
    } else {
      create_words = 1;
      fast_mode = 1;
      order_ratio = 0.0;
    }
  }

  uint32_t max_rules; /* upper bound on num_terminals + num_rules; clamped by
                         in_size and params->max_rules */
  {
    // max_rules = min(0xA00000, (in_size >> 4) + 0x110000);
    max_rules = 0xA00000;
    if (max_rules > (in_size >> 4) + 0x110000) {
      max_rules = (in_size >> 4) + 0x110000;
    }
    if (params != 0 && params->max_rules + 0x110000 < max_rules) {
      max_rules = params->max_rules + 0x110000;
    }
  }

  struct rank_scores_thread_data *rank_scores_data_ptr;
  if ((0 == (symbol_counts = (uint32_t *)malloc(4 * max_rules))) ||
      (0 == (symbol_ends = (struct symbol_ends_data *)malloc(
                 max_rules * sizeof(struct symbol_ends_data)))) ||
      (0 == (rank_scores_data_ptr = (struct rank_scores_thread_data *)malloc(
                 sizeof(struct rank_scores_thread_data)))) ||
      ((fast_mode != 0) &&
       (0 == (score_map = (int16_t *)malloc(2 * in_size))))) {
    GLZA_DIE("ERROR - memory allocation failed\n");
  }

  uint32_t max_scores = fast_mode == 1
                            ? MAX_SCORES_FAST
                            : MAX_SCORES; /* per-pass candidate cap; grows
                                             adaptively in main_loop */
  candidates = &rank_scores_data_ptr->candidates[0];
  memset(num_starts, 0, 0x400);
  memset(num_ends, 0, 0x400);
  memset(o1c, 0, 0x40000);

  uint64_t available_RAM; /* bytes malloc'd for start_symbol_ptr arena; user-set
                             or heuristic from in_size */
  if (params != 0 && params->user_set_RAM_size != 0) {
    available_RAM = (uint64_t)(params->RAM_usage * (float)0x100000);
    if (available_RAM > max_memory_usage) {
      available_RAM = max_memory_usage;
    }
    if (0 == (start_symbol_ptr = (uint32_t *)malloc(available_RAM))) {
      fprintf(stderr,
              "ERROR - Insufficient RAM to compress - unable to allocate %zu "
              "bytes\n",
              (size_t)available_RAM);
      exit(1);
    }
    if (available_RAM < (41 * (uint64_t)in_size) / 10) {
      fprintf(stderr,
              "ERROR - Insufficient RAM to compress - program requires at "
              "least %.2lf MB\n",
              ((float)((41 * (uint64_t)in_size) / 10) / (float)0x100000) +
                  0.005);
      exit(1);
    }
  } else {
    available_RAM = ((uint64_t)in_size * 250) + 40000000;
    if (available_RAM > max_memory_usage) {
      available_RAM = max_memory_usage;
    }
    if (available_RAM > 0x80000000 + (6 * (uint64_t)in_size)) {
      available_RAM = 0x80000000 + (6 * (uint64_t)in_size);
    }
    do {
      start_symbol_ptr = (uint32_t *)malloc(available_RAM);
      if (start_symbol_ptr != 0) {
        break;
      }
      available_RAM = (available_RAM / 10) * 9;
    } while (available_RAM > 1500000000);
    if ((start_symbol_ptr == 0) ||
        (available_RAM < (uint64_t)in_size * 9 / 2)) {
      fprintf(stderr,
              "ERROR - Insufficient RAM to compress - unable to allocate %zu "
              "bytes\n",
              (size_t)((available_RAM * 10) / 9));
      exit(1);
    }
  }
  uint8_t *end_RAM_ptr = (uint8_t *)start_symbol_ptr +
                         available_RAM; /* one-past-end of grammar arena */

  // parse the file to determine UTF8_compliant
  uint32_t *in_symbol_ptr =
      start_symbol_ptr;   /* read/write cursor during ingest; later reused in
                             main_loop */
  uint32_t num_rules = 0; /* production rules generated so far */
  uint8_t UTF8_compliant = 0; /* set if entire input decoded as valid UTF-8 */
  uint8_t format = **iobuf;   /* GLZA format tag byte (not a boolean) */
  cap_encoded = (format == 1);
  uint32_t max_UTF8_value =
      0x7F; /* high water mark while parsing UTF-8 terminals */
  uint8_t *in_char_ptr =
      *iobuf + 1; /* raw input cursor during parse/serialize */
  uint8_t *end_char_ptr = *iobuf + in_size;

  uint32_t UTF8_value;
  if (format < 2) {
    do {
      uint8_t this_char = *in_char_ptr++;
      if (this_char < 0x80) {
        *in_symbol_ptr++ = (uint32_t)this_char;
      } else if ((this_char < 0xC0) || (this_char >= 0xF2) ||
                 ((*in_char_ptr & 0xC0) != 0x80)) {
        break;
      } else {
        UTF8_value =
            (0x40 * (uint32_t)(this_char & 0x1F)) + (*in_char_ptr++ & 0x3F);
        if (this_char >= 0xE0) {
          if ((*in_char_ptr & 0xC0) != 0x80) {
            break;
          }
          UTF8_value = (0x40 * UTF8_value) + (uint32_t)(*in_char_ptr++ & 0x3F);
          if (this_char >= 0xF0) {
            if ((*in_char_ptr & 0xC0) != 0x80) {
              break;
            }
            UTF8_value = (0x40 * (UTF8_value & 0x7FFF)) +
                         (uint32_t)(*in_char_ptr++ & 0x3F);
          }
        }
        *in_symbol_ptr++ = UTF8_value;
        if (UTF8_value > max_UTF8_value) {
          max_UTF8_value = UTF8_value;
        }
      }
    } while (in_char_ptr < end_char_ptr);
    if (in_char_ptr == end_char_ptr) {
      UTF8_compliant = 1;
    }
  }

#ifdef PRINTON
  fprintf(stderr, "cap encoded: %u, UTF8 compliant %u\n",
          (unsigned int)cap_encoded, (unsigned int)UTF8_compliant);
#endif
  // create the initial grammar and count symbols
  uint8_t max_terminal; /* 0x7F if UTF-8 path else 0xFF; bounds run-length dedup
                           and tree symbol tests */
  // create the initial grammar and count symbols
  in_char_ptr = *iobuf + 1;
  if (UTF8_compliant != 0) {
    num_terminals = max_UTF8_value + 1;
    max_terminal = 0x7F;
    memset(symbol_counts, 0, 4 * num_terminals);
    num_file_symbols = in_symbol_ptr - start_symbol_ptr;
    end_symbol_ptr = in_symbol_ptr;
    in_symbol_ptr = start_symbol_ptr;
    while (in_symbol_ptr != end_symbol_ptr) {
      symbol_counts[*in_symbol_ptr++]++;
    }
#ifdef PRINTON
    fprintf(stderr, "%u symbols, maximum UTF-8 value 0x%x\n",
            (unsigned int)num_file_symbols, (unsigned int)max_UTF8_value);
#endif
    if (params == 0 || params->user_set_profit_ratio_power == 0) {
      profit_ratio_power = fast_mode == 1 ? 1.0 : 2.0;
    }
    for (size_t i = 0; i < num_terminals; i++) {
      symbol_ends[i].start = symbol_ends[i].end = get_UTF8_context(i);
    }
  } else {
    num_terminals = 0x100;
    max_terminal = 0xFF;
    memset(symbol_counts, 0, 0x400);
    in_symbol_ptr = start_symbol_ptr;
    while (in_char_ptr != end_char_ptr) {
      *in_symbol_ptr = (uint32_t)*in_char_ptr++;
      symbol_counts[*in_symbol_ptr++]++;
    }
    num_file_symbols = in_symbol_ptr - start_symbol_ptr;
    end_symbol_ptr = in_symbol_ptr;
#ifdef PRINTON
    fprintf(stderr, "%u symbols\n", (unsigned int)num_file_symbols);
#endif
    if (params == 0 || params->user_set_profit_ratio_power == 0) {
      if (fast_mode == 0 && cap_encoded != 0) {
        profit_ratio_power = 2.0;
      } else if ((format & 0xFE) == 0) {
        profit_ratio_power = 1.0;
      } else {
        profit_ratio_power = 0.0;
      }
    }
    for (size_t i = 0; i < num_terminals; i++) {
      symbol_ends[i].start = symbol_ends[i].end = i;
    }
  }
  free(*iobuf);
  if (available_RAM < (4 * (uint64_t)in_size) +
                          (4 * BASE_NODES_CHILD_ARRAY_SIZE * num_terminals) +
                          (0x10 * MAX_SCORES_FAST)) {
    fprintf(
        stderr,
        "ERROR - Insufficient RAM to compress - unable to allocate %zu bytes\n",
        (size_t)((4 * (uint64_t)in_size) +
                 (4 * BASE_NODES_CHILD_ARRAY_SIZE * num_terminals) +
                 (0x10 * MAX_SCORES_FAST)));
    exit(1);
  }
  if (params != 0 && params->max_rules + num_terminals < max_rules) {
    max_rules = params->max_rules + num_terminals;
  }

  in_symbol_ptr = start_symbol_ptr;
  uint8_t sym1;
  uint8_t sym2 = *in_symbol_ptr++;
  while (in_symbol_ptr != end_symbol_ptr) {
    sym1 = sym2;
    sym2 = symbol_ends[*in_symbol_ptr++].end;
    o1c[sym1][sym2]++;
    num_ends[sym1]++;
    num_starts[sym2]++;
  }

  uint32_t max_x_log2_x =
      0; /* tracks x_log2_x[] allocation length; derived from num_ends[], capped
            at NUM_PRECALCULATED_X_LOG2_X */
  for (size_t i = 0; i < 0x100; i++) {
    if (num_ends[i] > max_x_log2_x) {
      max_x_log2_x = num_ends[i];
    }
  }
  max_x_log2_x += 2;
  if (max_x_log2_x > NUM_PRECALCULATED_X_LOG2_X) {
    max_x_log2_x = NUM_PRECALCULATED_X_LOG2_X;
  }

  uint32_t first_define_index =
      in_symbol_ptr -
      start_symbol_ptr; /* stream index of first 0x80000001 rule marker */
  *end_symbol_ptr = 0xFFFFFFFE;
  size_t min_RAM = end_symbol_ptr - start_symbol_ptr +
                   (2 * MAX_MATCH_LENGTH * sizeof(struct node));
  if (min_RAM > available_RAM) {
    fprintf(stderr,
            "ERROR - Insufficient RAM to compress - program requires at least "
            "%.2lf MB\n",
            ((float)min_RAM / (float)0x100000) + 0.005);
    exit(1);
  }

  uint32_t *new_symbol_number;  /* run-length dedup: terminal -> new repeat
                                   symbol; malloc(4*max_scores) */
  struct score_data *node_data; /* stack for tree scoring walk;
                                   NODE_DATA_STACK_DEPTH entries */
  uint16_t
      *candidates_index; /* permutation [0..max_scores); malloc(2*max_scores) */
  uint8_t *candidate_bad; /* 0=ok, 1=reject, 2=done; malloc(max_scores) */
  if ((0 == (new_symbol_number = (uint32_t *)malloc(4 * max_scores))) ||
      (0 == (node_data = (struct score_data *)malloc(
                 NODE_DATA_STACK_DEPTH * sizeof(struct score_data)))) ||
      (0 == (candidates_index = (uint16_t *)malloc(2 * max_scores))) ||
      (0 == (candidate_bad = (uint8_t *)malloc(max_scores))) ||
      ((fast_mode == 0) &&
       (0 == (x_log2_x = (double *)malloc(8 * max_x_log2_x))))) {
    GLZA_DIE("ERROR - memory allocation failed\n");
  }

  uint32_t num_terminals_used = 0;
  for (size_t i = 0; i < num_terminals; i++) {
    if (symbol_counts[i] != 0) {
      num_terminals_used++;
    }
  }
  for (size_t i = 1; i < NUM_PRECALCULATED_LOG2_X; i++) {
    log2_x[i] = log2((double)i);
  }
  rank_scores_data_ptr->candidates_index = candidates_index;

  uint32_t initial_max_scores; /* starting max_scores passed to main_loop;
                                  formula differs by fast_mode */
  uint8_t fast_sections; /* 1 if fast_mode==0; else 23 then adaptive down to 1
                            (fast_mode==1 only) */
  uint8_t fast_section;  /* fast_mode==1 only: which file slice
                            [0..fast_sections) is built this pass */
  float fast_min_score;  /* fast_mode==1 only: floor when shrinking sections on
                            weak scores */
  uint16_t *candidates_position; /* fast_mode==1 only: malloc(2*max_scores);
                                    __jm__ only initialized when fast_mode==1 */
  uint8_t
      section_repeats; /* fast_mode==1 only: stick with prior section up to 2
                          passes; __jm__ only initialized when fast_mode==1 */
  float section_scores[23]; /* fast_mode==1 only: best tail score seen per
                               section; size matches initial fast_sections */
  if (fast_mode == 0) {
    for (size_t i = 1; i < max_x_log2_x; i++) {
      x_log2_x[i] = (double)i * log2((double)i);
    }
    initial_max_scores =
        (uint32_t)(500.0 + (0.075 * sqrt((double)num_file_symbols)));
    fast_sections = 1;
  } else {
    if (0 == (candidates_position = (uint16_t *)malloc(2 * max_scores))) {
      GLZA_DIE("ERROR - memory allocation failed\n");
    }
    rank_scores_data_ptr->candidates_position = candidates_position;
    fast_sections = 23;
    fast_section = 0;
    fast_min_score = 4.0;
    section_repeats = 0;
    for (size_t i = 0; i < 23; i++) {
      section_scores[i] = BIG_FLOAT;
    }
    initial_max_scores =
        (uint32_t)(100.0 + (22.0 * pow((double)num_file_symbols, 0.3333)));
  }
  memset(candidate_bad, 0, max_scores);
  min_score = 10.0;
  uint16_t scan_cycle = 0; /* compression pass counter; surfaced in PRINTON and
                              output validation */

  enum glza_scan_mode initial_scan_mode =
      (((cap_encoded == 0) && ((UTF8_compliant == 0) || (fast_mode == 0))) ||
       (create_words == 0))
          ? GLZA_SCAN_RUN_DEDUP
          : GLZA_SCAN_WORDS;
  main_loop(&num_rules, initial_scan_mode, num_terminals_used, end_RAM_ptr,
            &in_symbol_ptr, UTF8_compliant, rank_scores_data_ptr, max_scores,
            node_data, max_rules, candidates_index, candidate_bad,
            first_define_index, initial_max_scores, max_terminal,
            new_symbol_number, fast_section, fast_sections, profit_ratio_power,
            fast_min_score, section_scores, section_repeats, &scan_cycle);

  if (fast_mode != 0) {
    free(score_map);
    free(candidates_position);
  } else {
    free(x_log2_x);
  }
  free(symbol_counts);
  free(symbol_ends);
  free(rank_scores_data_ptr);
  free(new_symbol_number);
  free(node_data);
  free(candidates_index);
  free(candidate_bad);

  if ((*iobuf = (uint8_t *)malloc((4 * num_file_symbols) + 1)) == 0) {
    GLZA_DIE("ERROR - Compressed output buffer memory allocation failed\n");
  }
  in_char_ptr = *iobuf;
  if (UTF8_compliant != 0) {
    *in_char_ptr++ = 5 | (cap_encoded << 1);
    uint8_t base_bits = 7;
    while ((max_UTF8_value >> base_bits) != 0) {
      base_bits++;
    }
    *in_char_ptr++ = base_bits;
  } else if (cap_encoded != 0) {
    *in_char_ptr++ = 3;
  } else {
    *in_char_ptr++ = format;
  }
  in_symbol_ptr = start_symbol_ptr;
  uint32_t next_new_symbol_number = num_terminals + num_rules;
  {
    uint32_t *validate_ptr = start_symbol_ptr;
    uint32_t invalid_count = 0;
    while (validate_ptr < end_symbol_ptr) {
      uint32_t symbol_value = *validate_ptr++;
      if (symbol_value == 0xFFFFFFFE) {
        continue;
      }
      if ((int32_t)symbol_value >= 0) {
        if (symbol_value >= next_new_symbol_number) {
          if (invalid_count < 8) {
            fprintf(stderr,
                    "GLZA compress: invalid terminal symbol %u >= "
                    "next_new_symbol_number %u at stream index %u (pass %u "
                    "rules %u)\n",
                    (unsigned int)symbol_value,
                    (unsigned int)next_new_symbol_number,
                    (unsigned int)(validate_ptr - 1 - start_symbol_ptr),
                    (unsigned int)scan_cycle, (unsigned int)num_rules);
          }
          invalid_count++;
        }
      } else {
        uint32_t rule_num = symbol_value - 0x80000001;
        if (rule_num >= num_rules) {
          if (invalid_count < 8) {
            fprintf(stderr,
                    "GLZA compress: invalid production marker 0x%08x (rule %u "
                    ">= num_rules %u) at stream index %u\n",
                    (unsigned int)symbol_value, (unsigned int)rule_num,
                    (unsigned int)num_rules,
                    (unsigned int)(validate_ptr - 1 - start_symbol_ptr));
          }
          invalid_count++;
        }
      }
    }
    if (invalid_count > 8) {
      fprintf(
          stderr,
          "GLZA compress: %u additional invalid symbols in grammar stream\n",
          (unsigned int)(invalid_count - 8));
    }
    if (invalid_count != 0) {
      fprintf(stderr,
              "GLZA compress: %u invalid symbols in final stream "
              "(num_terminals=%u num_rules=%u next_new_symbol_number=%u)\n",
              (unsigned int)invalid_count, (unsigned int)num_terminals,
              (unsigned int)num_rules, (unsigned int)next_new_symbol_number);
      GLZA_DIE("ERROR - invalid grammar stream\n");
    }
  }
  if (UTF8_compliant != 0) {
    while (in_symbol_ptr != end_symbol_ptr) {
      uint32_t symbol_value = *in_symbol_ptr++;
      if (symbol_value < 0x80) {
        *in_char_ptr++ = (uint8_t)symbol_value;
      } else if (symbol_value < num_terminals) {
        if (symbol_value < 0x800) {
          *in_char_ptr++ = 0xC0 + (symbol_value >> 6);
        } else if (symbol_value < 0x10000) {
          *in_char_ptr++ = 0xE0 + (symbol_value >> 12);
          *in_char_ptr++ = 0x80 + ((symbol_value >> 6) & 0x3F);
        } else {
          *in_char_ptr++ = 0xF0 + (symbol_value >> 18);
          *in_char_ptr++ = 0x80 + ((symbol_value >> 12) & 0x3F);
          *in_char_ptr++ = 0x80 + ((symbol_value >> 6) & 0x3F);
        }
        *in_char_ptr++ = 0x80 + (symbol_value & 0x3F);
      } else {
        if ((int32_t)symbol_value >= 0) {
          symbol_value -= num_terminals;
          *in_char_ptr++ = INSERT_SYMBOL_CHAR;
        } else {
          symbol_value--;
          *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
        }
        *in_char_ptr++ = (uint8_t)((symbol_value >> 16) & 0xFF);
        *in_char_ptr++ = (uint8_t)((symbol_value >> 8) & 0xFF);
        *in_char_ptr++ = (uint8_t)(symbol_value & 0xFF);
      }
    }
  } else {
    while (in_symbol_ptr != end_symbol_ptr) {
      uint32_t symbol_value = *in_symbol_ptr++;
      if (symbol_value <= DEFINE_SYMBOL_CHAR) {
        *in_char_ptr++ = (uint8_t)symbol_value;
        if (symbol_value >= INSERT_SYMBOL_CHAR) {
          *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
        }
      } else {
        if ((int32_t)symbol_value >= 0) {
          symbol_value -= 0x100;
          *in_char_ptr++ = INSERT_SYMBOL_CHAR;
        } else {
          *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
        }
        *in_char_ptr++ = (uint8_t)((symbol_value >> 16) & 0xFF);
        *in_char_ptr++ = (uint8_t)((symbol_value >> 8) & 0xFF);
        *in_char_ptr++ = (uint8_t)(symbol_value & 0xFF);
      }
    }
  }

  in_size = in_char_ptr - *iobuf;
  if ((*iobuf = (uint8_t *)realloc(*iobuf, in_size)) == 0) {
    GLZA_DIE("ERROR - Compressed output buffer memory reallocation failed\n");
  }
  *outsize_ptr = in_size;
  free(start_symbol_ptr);
  if (fast_mode != 0) {
#ifdef PRINTON
    fprintf(stderr, "PASS %u: grammar size %u, %u production rules  \n",
            (unsigned int)scan_cycle, (unsigned int)num_file_symbols + 1,
            (unsigned int)num_rules);
#endif
  }
  return 1;
}
