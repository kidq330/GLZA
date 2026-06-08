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

#include "glza_compress.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace glza {
namespace {

double xlogx(double arg) { return arg * log2(arg); }

uint8_t get_UTF8_context(uint32_t symbol) {
  if (symbol < 0x80) return static_cast<uint8_t>(symbol);
  if (symbol < 0x250) return 0x80;
  if (symbol < 0x370) return 0x81;
  if (symbol < 0x400) return 0x82;
  if (symbol < 0x530) return 0x83;
  if (symbol < 0x590) return 0x84;
  if (symbol < 0x600) return 0x85;
  if (symbol < 0x700) return 0x86;
  if (symbol < 0x800) return 0x87;
  if (symbol < 0x1000) return 0x88;
  if (symbol < 0x2000) return 0x89;
  if (symbol < 0x3000) return 0x8A;
  if (symbol < 0x3040) return 0x8B;
  if (symbol < 0x30A0) return 0x8C;
  if (symbol < 0x3100) return 0x8D;
  if (symbol < 0x3200) return 0x8E;
  if (symbol < 0xA000) return 0x8F;
  if (symbol < 0x10000) return 0x8E;
  return 0x90;
}

void init_match_node(MatchNode* mn, uint32_t symbol, uint32_t num_symbols,
                     uint32_t score_number = 0) {
  mn->symbol = symbol;
  mn->num_symbols = num_symbols;
  mn->score_number = score_number;
  mn->child_ptr = nullptr;
  auto* sp = reinterpret_cast<uint64_t*>(&mn->sibling_node_num[0]);
  sp[0] = 0; sp[1] = 0; sp[2] = 0; sp[3] = 0;
  sp[4] = 0; sp[5] = 0; sp[6] = 0; sp[7] = 0;
  mn->miss_ptr = nullptr;
  mn->hit_ptr = nullptr;
}

uint8_t move_to_match_sibling(MatchNode* match_nodes,
                              MatchNode** match_node_ptr_ptr, uint32_t symbol,
                              uint8_t* sibling_number) {
  uint32_t shifted_symbol = symbol;
  *sibling_number = static_cast<uint8_t>(shifted_symbol & 0xF);
  while (symbol != (*match_node_ptr_ptr)->symbol) {
    if ((*match_node_ptr_ptr)->sibling_node_num[*sibling_number] == 0)
      return 0;
    *match_node_ptr_ptr =
        &match_nodes[(*match_node_ptr_ptr)->sibling_node_num[*sibling_number]];
    shifted_symbol >>= 4;
    *sibling_number = static_cast<uint8_t>(shifted_symbol & 0xF);
  }
  return 1;
}

void move_to_existing_match_sibling(MatchNode* match_nodes,
                                    MatchNode** match_node_ptr_ptr,
                                    uint32_t symbol) {
  uint32_t shifted_symbol = symbol;
  while (symbol != (*match_node_ptr_ptr)->symbol) {
    const uint8_t sn = static_cast<uint8_t>(shifted_symbol & 0xF);
    *match_node_ptr_ptr =
        &match_nodes[(*match_node_ptr_ptr)->sibling_node_num[sn]];
    shifted_symbol >>= 4;
  }
}

uint8_t move_to_search_sibling(MatchNode* match_nodes, uint32_t symbol,
                               MatchNode** search_node_ptr_ptr) {
  uint32_t shifted_symbol = symbol;
  uint8_t sn = static_cast<uint8_t>(shifted_symbol & 0xF);
  while (symbol != (*search_node_ptr_ptr)->symbol) {
    if ((*search_node_ptr_ptr)->sibling_node_num[sn] == 0) return 0;
    *search_node_ptr_ptr =
        &match_nodes[(*search_node_ptr_ptr)->sibling_node_num[sn]];
    shifted_symbol >>= 4;
    sn = static_cast<uint8_t>(shifted_symbol & 0xF);
  }
  return 1;
}

MatchNode* move_to_base_match_child_with_make(MatchNode* match_nodes,
                                              uint32_t symbol,
                                              uint32_t score_number,
                                              uint32_t* num_match_nodes_ptr,
                                              MatchNode** child_ptr_ptr) {
  MatchNode* match_node_ptr;
  if (*child_ptr_ptr == nullptr) {
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

void move_to_match_child_with_make(MatchNode* match_nodes,
                                   MatchNode** match_node_ptr_ptr,
                                   uint32_t symbol, uint32_t score_number,
                                   uint32_t best_score_num_symbols,
                                   uint32_t* num_match_nodes_ptr) {
  if ((*match_node_ptr_ptr)->child_ptr == nullptr) {
    (*match_node_ptr_ptr)->child_ptr = &match_nodes[(*num_match_nodes_ptr)++];
    *match_node_ptr_ptr = (*match_node_ptr_ptr)->child_ptr;
    init_match_node(*match_node_ptr_ptr, symbol, best_score_num_symbols,
                    score_number);
  } else {
    *match_node_ptr_ptr = (*match_node_ptr_ptr)->child_ptr;
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

void write_siblings_miss_ptr(MatchNode* match_nodes, MatchNode* node_ptr,
                             MatchNode* miss_ptr) {
  node_ptr->miss_ptr = miss_ptr;
  for (uint8_t sn = 0; sn < 16; sn++) {
    const uint32_t sibling_node_number = node_ptr->sibling_node_num[sn];
    if (sibling_node_number != 0)
      write_siblings_miss_ptr(match_nodes, &match_nodes[sibling_node_number],
                              miss_ptr);
  }
}

}  // anonymous namespace

// --- warning / heap helpers ---

void Compressor::warn_once(uint8_t& flag, const char* message) {
  if (flag == 0) {
    flag = 1;
    fputs(message, stderr);
  }
}

void Compressor::free_match_heap_bufs() {
  match_nodes_heap_.clear();
  match_nodes_heap_.shrink_to_fit();
  match_strings_heap_.clear();
  match_strings_heap_.shrink_to_fit();
}

bool Compressor::ensure_match_heap(uint32_t min_node_slots,
                                   uint32_t match_string_words) {
  if (min_node_slots < 4096) min_node_slots = 4096;
  if (match_string_words < 4096) match_string_words = 4096;
  try {
    if (match_nodes_heap_.size() < min_node_slots)
      match_nodes_heap_.resize(min_node_slots);
    if (match_strings_heap_.size() < match_string_words)
      match_strings_heap_.resize(match_string_words);
  } catch (...) {
    return false;
  }
  return true;
}

void Compressor::note_match_limit(uint32_t match_nodes_limit,
                                  uint32_t num_match_nodes,
                                  uint16_t num_candidates,
                                  uint16_t candidate_index,
                                  uint32_t max_match_length,
                                  size_t arena_match_bytes) {
  match_limit_ctx_ = {match_nodes_limit, num_match_nodes, num_candidates,
                      candidate_index,   max_match_length, arena_match_bytes};
}

void Compressor::bind_match_storage(MatchNode** out_match_nodes,
                                    uint32_t* out_match_nodes_limit,
                                    uint32_t** out_match_strings,
                                    uint8_t* free_RAM_ptr,
                                    uintptr_t match_region_end_limit,
                                    uint32_t child_ptr_count,
                                    uint16_t num_candidates,
                                    uint32_t max_match_length,
                                    uint32_t est_match_nodes) {
  const uintptr_t arena_match_nodes =
      reinterpret_cast<uintptr_t>(free_RAM_ptr) +
      static_cast<uintptr_t>(child_ptr_count) * sizeof(MatchNode*);
  const size_t arena_bytes = match_region_end_limit > arena_match_nodes
                                 ? match_region_end_limit - arena_match_nodes
                                 : 0;
  const uint32_t arena_slots =
      static_cast<uint32_t>(arena_bytes / sizeof(MatchNode));
  const size_t grammar_bytes = static_cast<size_t>(end_symbol_ptr_ - start_symbol_ptr_) * sizeof(uint32_t);
  const size_t need_str_words =
      static_cast<size_t>(num_candidates) *
      static_cast<size_t>(max_match_length != 0 ? max_match_length : 1);
  const size_t need_bytes = static_cast<size_t>(est_match_nodes) * sizeof(MatchNode) +
                            need_str_words * sizeof(uint32_t);

  if (arena_slots >= est_match_nodes && arena_bytes >= need_bytes) {
    *out_match_nodes = reinterpret_cast<MatchNode*>(arena_match_nodes);
    *out_match_nodes_limit = arena_slots;
    *out_match_strings = reinterpret_cast<uint32_t*>(
        arena_match_nodes + static_cast<size_t>(est_match_nodes) * sizeof(MatchNode));
    return;
  }

  const uint32_t heap_slots =
      est_match_nodes + static_cast<uint32_t>(num_candidates) * 8u + 4096u;
  const uint32_t heap_words = static_cast<uint32_t>(need_str_words) + 4096u;
  if (!ensure_match_heap(heap_slots, heap_words)) {
    fprintf(stderr,
            "GLZA compress: match prefix-tree heap alloc failed "
            "(need_node_slots=%u need_str_words=%u arena_match_bytes=%zu "
            "grammar_stream_bytes=%zu compress_RAM=%zu)\n",
            static_cast<unsigned>(heap_slots), static_cast<unsigned>(heap_words),
            arena_bytes, grammar_bytes, compress_ram_bytes_);
    global_diagnostics().set(
        "compress",
        "match prefix-tree heap alloc failed (need_node_slots=%u "
        "need_str_words=%u arena_match_bytes=%zu compress_RAM=%zu)",
        static_cast<unsigned>(heap_slots), static_cast<unsigned>(heap_words),
        arena_bytes, compress_ram_bytes_);
    *out_match_nodes = reinterpret_cast<MatchNode*>(arena_match_nodes);
    *out_match_nodes_limit = arena_slots;
    *out_match_strings = reinterpret_cast<uint32_t*>(arena_match_nodes + sizeof(MatchNode));
    return;
  }

  fprintf(stderr,
          "GLZA compress: match prefix-tree using heap fallback "
          "(arena_match_bytes=%zu est_nodes=%u candidates=%u max_match_len=%u "
          "grammar_stream_bytes=%zu compress_RAM=%zu)\n",
          arena_bytes, static_cast<unsigned>(est_match_nodes),
          static_cast<unsigned>(num_candidates),
          static_cast<unsigned>(max_match_length), grammar_bytes,
          compress_ram_bytes_);
  *out_match_nodes = match_nodes_heap_.data();
  *out_match_nodes_limit = static_cast<uint32_t>(match_nodes_heap_.size());
  *out_match_strings = match_strings_heap_.data();
}

void Compressor::warn_suffix_nodes_limit(uint32_t limit, uint32_t next_num) {
  warn_once(warned_suffix_nodes_,
            "GLZA compress: suffix tree node limit reached "
            "(increase RAM or reduce input)\n");
  fprintf(stderr,
          "GLZA compress: suffix tree node budget exhausted "
          "(nodes_num_limit=%u next_node_num=%u compress_RAM=%zu). "
          "Try GLZA_RAM_MB=<megabytes> or reduce max_rules/input size.\n",
          static_cast<unsigned>(limit), static_cast<unsigned>(next_num),
          compress_ram_bytes_);
  global_diagnostics().set(
      "compress",
      "suffix tree node limit (nodes_num_limit=%u next_node_num=%u "
      "compress_RAM=%zu)",
      static_cast<unsigned>(limit), static_cast<unsigned>(next_num),
      compress_ram_bytes_);
}

void Compressor::warn_match_nodes_limit() {
  warn_once(warned_match_nodes_,
            "GLZA compress: match prefix-tree node limit reached "
            "(increase RAM or reduce candidates)\n");
  fprintf(stderr,
          "GLZA compress: match prefix-tree limit "
          "(match_nodes_limit=%u num_match_nodes=%u candidate=%u/%u "
          "max_match_len=%u arena_match_bytes=%zu grammar_stream_bytes=%zu "
          "compress_RAM=%zu). Arena between grammar end and suffix-tree nodes "
          "was too small even after heap fallback; try GLZA_RAM_MB=<megabytes> "
          "or lower max_rules.\n",
          static_cast<unsigned>(match_limit_ctx_.match_nodes_limit),
          static_cast<unsigned>(match_limit_ctx_.num_match_nodes),
          static_cast<unsigned>(match_limit_ctx_.candidate_index),
          static_cast<unsigned>(match_limit_ctx_.num_candidates),
          static_cast<unsigned>(match_limit_ctx_.max_match_length),
          match_limit_ctx_.arena_match_bytes,
          static_cast<size_t>(end_symbol_ptr_ - start_symbol_ptr_) * sizeof(uint32_t),
          compress_ram_bytes_);
  global_diagnostics().set(
      "compress",
      "match prefix-tree limit (match_nodes_limit=%u "
      "num_match_nodes=%u candidate=%u/%u arena_match_bytes=%zu "
      "compress_RAM=%zu)",
      static_cast<unsigned>(match_limit_ctx_.match_nodes_limit),
      static_cast<unsigned>(match_limit_ctx_.num_match_nodes),
      static_cast<unsigned>(match_limit_ctx_.candidate_index),
      static_cast<unsigned>(match_limit_ctx_.num_candidates),
      match_limit_ctx_.arena_match_bytes, compress_ram_bytes_);
}

void Compressor::warn_main_nodes_limit(uint32_t limit, uint32_t next_num) {
  warn_once(warned_main_nodes_,
            "GLZA compress: suffix tree build stopped at node budget "
            "(increase RAM)\n");
  fprintf(stderr,
          "GLZA compress: parallel suffix-tree node budget hit "
          "(main_nodes_limit=%u next_node_num=%u compress_RAM=%zu). "
          "Try GLZA_RAM_MB=<megabytes> or reduce max_rules.\n",
          static_cast<unsigned>(limit), static_cast<unsigned>(next_num),
          compress_ram_bytes_);
  global_diagnostics().set(
      "compress",
      "parallel suffix-tree node budget (main_nodes_limit=%u "
      "next_node_num=%u compress_RAM=%zu)",
      static_cast<unsigned>(limit), static_cast<unsigned>(next_num),
      compress_ram_bytes_);
}

void Compressor::warn_rank_buffer_limit(uint16_t max_scores) {
  warn_once(warned_rank_buffer_,
            "GLZA compress: rank-scores buffer full (candidate cap reached)\n");
  fprintf(stderr,
          "GLZA compress: rank-scores buffer full (max_scores=%u). "
          "Internal candidate cap reached — reduce max_rules or input size.\n",
          static_cast<unsigned>(max_scores));
  global_diagnostics().set("compress", "rank-scores buffer full (max_scores=%u)",
                           static_cast<unsigned>(max_scores));
}

// --- suffix-tree node operations ---

SuffixNode* Compressor::create_suffix_node(uint32_t suffix_symbol,
                                           uint32_t symbol_index,
                                           uint32_t* next_node_num_ptr) {
  if (*next_node_num_ptr >= nodes_num_limit_) {
    warn_suffix_nodes_limit(nodes_num_limit_, *next_node_num_ptr);
    return nullptr;
  }
  SuffixNode* node_ptr = &nodes_[(*next_node_num_ptr)++];
  node_ptr->symbol = suffix_symbol;
  node_ptr->last_match_index = symbol_index;
  node_ptr->sibling_node_num[0] = 0;
  node_ptr->sibling_node_num[1] = 0;
  node_ptr->child_node_num = 0;
  node_ptr->num_extra_symbols = 0;
  node_ptr->instances = 1;
  return node_ptr;
}

SuffixNode* Compressor::split_node_for_overlap(SuffixNode* node_ptr,
                                               uint32_t split_index,
                                               uint32_t in_symbol_index,
                                               uint32_t* next_node_num_ptr) {
  if (*next_node_num_ptr >= nodes_num_limit_) {
    warn_suffix_nodes_limit(nodes_num_limit_, *next_node_num_ptr);
    return nullptr;
  }
  const uint32_t non_overlap_length = split_index - node_ptr->last_match_index;
  SuffixNode* new_node_ptr = &nodes_[*next_node_num_ptr];
  new_node_ptr->symbol = *(start_symbol_ptr_ + split_index);
  new_node_ptr->last_match_index = split_index;
  new_node_ptr->sibling_node_num[0] = 0;
  new_node_ptr->sibling_node_num[1] = 0;
  new_node_ptr->child_node_num = node_ptr->child_node_num;
  new_node_ptr->num_extra_symbols = node_ptr->num_extra_symbols - non_overlap_length;
  new_node_ptr->instances = node_ptr->instances;
  node_ptr->last_match_index = in_symbol_index;
  node_ptr->child_node_num = static_cast<int32_t>((*next_node_num_ptr)++);
  node_ptr->num_extra_symbols = non_overlap_length - 1;
  node_ptr->instances++;
  return new_node_ptr;
}

// --- add_word_suffix ---

void Compressor::add_word_suffix(uint32_t* in_symbol_ptr,
                                 uint32_t* next_node_num_ptr) {
  assert(in_symbol_ptr < end_symbol_ptr_ && "bad arg: `in_symbol_ptr` cursor past EOS");
  const ptrdiff_t stream_len = end_symbol_ptr_ - start_symbol_ptr_;
  uint32_t search_symbol = *in_symbol_ptr;
  if (static_cast<int32_t>(search_symbol) < 0) return;

  int32_t* base_node_child_num_ptr =
      search_symbol < 0x80
          ? &base_nodes_child_node_num_[search_symbol]
          : &base_nodes_child_node_num_[0x80 + (search_symbol & 0xF)];

  if (*base_node_child_num_ptr == 0) {
    *base_node_child_num_ptr =
        static_cast<int32_t>(in_symbol_ptr - start_symbol_ptr_ - 0x80000000);
    return;
  }

  if (*base_node_child_num_ptr < 0) {
    const uint32_t symbol_index =
        static_cast<uint32_t>(*base_node_child_num_ptr + 0x80000000);
    assert(symbol_index < static_cast<uint32_t>(stream_len) &&
           "decoded GST index must be valid offset into input stream");
    if (create_suffix_node(start_symbol_ptr_[symbol_index], symbol_index,
                           next_node_num_ptr) == nullptr)
      return;
    *base_node_child_num_ptr = static_cast<int32_t>(*next_node_num_ptr - 1);
  }

  if (*base_node_child_num_ptr <= 0) {
    assert(0 && "GST base child index invalid after expansion");
    return;
  }
  if (static_cast<uint32_t>(*base_node_child_num_ptr) >= nodes_num_limit_) {
    assert(0 && "GST base child index out of range");
    return;
  }
  SuffixNode* node_ptr = &nodes_[*base_node_child_num_ptr];
  if (search_symbol != node_ptr->symbol) {
    uint32_t shifted_search_symbol = search_symbol >> 4;
    do {
      int32_t* sibling_node_num_ptr =
          &node_ptr->sibling_node_num[shifted_search_symbol & 1];
      if (*sibling_node_num_ptr == 0) {
        if (create_suffix_node(search_symbol,
                               static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_),
                               next_node_num_ptr) == nullptr)
          return;
        *sibling_node_num_ptr = static_cast<int32_t>(*next_node_num_ptr - 1);
        return;
      }
      if (static_cast<uint32_t>(*sibling_node_num_ptr) >= nodes_num_limit_) {
        assert(0 && "GST sibling node index out of range");
        return;
      }
      node_ptr = &nodes_[*sibling_node_num_ptr];
      shifted_search_symbol >>= 1;
    } while (search_symbol != node_ptr->symbol);
  }

  uint32_t* first_symbol_ptr = in_symbol_ptr - 1;
  uint32_t* max_word_ptr = end_symbol_ptr_ - 1;
  if (max_word_ptr > first_symbol_ptr + kMaxMatchLength - 1)
    max_word_ptr = first_symbol_ptr + kMaxMatchLength - 1;
  while (node_ptr->child_node_num != 0) {
    const uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
    if (num_extra_symbols != 0) {
      if (node_ptr->last_match_index + num_extra_symbols + 1 >=
          static_cast<uint32_t>(stream_len)) {
        assert(0 && "GST node span exceeds grammar stream");
        return;
      }
      uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
      uint32_t length = 1;
      do {
        if (in_symbol_ptr + length > max_word_ptr) return;
        if (node_ptr->last_match_index + length >= static_cast<uint32_t>(stream_len)) {
          assert(0 && "GST node span exceeds grammar stream");
          return;
        }
        if (*(node_symbol_ptr + length) != *(in_symbol_ptr + length)) {
          if (*next_node_num_ptr + 1 >= nodes_num_limit_) {
            warn_suffix_nodes_limit(nodes_num_limit_, *next_node_num_ptr);
            return;
          }
          SuffixNode* new_node_ptr = &nodes_[*next_node_num_ptr];
          new_node_ptr->last_match_index = node_ptr->last_match_index + length;
          new_node_ptr->symbol = *(node_symbol_ptr + length);
          new_node_ptr->sibling_node_num[0] = 0;
          new_node_ptr->sibling_node_num[1] = 0;
          new_node_ptr->child_node_num = node_ptr->child_node_num;
          new_node_ptr->num_extra_symbols = num_extra_symbols - length;
          new_node_ptr->instances = node_ptr->instances;
          node_ptr->num_extra_symbols = length - 1;
          node_ptr->child_node_num = static_cast<int32_t>((*next_node_num_ptr)++);
          node_ptr->instances++;
          new_node_ptr->sibling_node_num[(*(in_symbol_ptr + length)) & 1] =
              static_cast<int32_t>(*next_node_num_ptr);
          create_suffix_node(*(in_symbol_ptr + length),
                             static_cast<uint32_t>(in_symbol_ptr + length - start_symbol_ptr_),
                             next_node_num_ptr);
          return;
        }
      } while (length++ != num_extra_symbols);
    }
    node_ptr->instances++;
    in_symbol_ptr += num_extra_symbols + 1;
    if (in_symbol_ptr > max_word_ptr || *(in_symbol_ptr - 1) == 0x20 ||
        in_symbol_ptr >= end_symbol_ptr_)
      return;
    search_symbol = *in_symbol_ptr;
    if (static_cast<uint32_t>(node_ptr->child_node_num) >= nodes_num_limit_) {
      assert(0 && "GST child node index out of range");
      return;
    }
    node_ptr = &nodes_[node_ptr->child_node_num];
    if (search_symbol != node_ptr->symbol) {
      uint32_t shifted_search_symbol = search_symbol;
      do {
        int32_t* prior_node_num_ptr =
            &node_ptr->sibling_node_num[shifted_search_symbol & 1];
        if (*prior_node_num_ptr == 0) {
          if (create_suffix_node(search_symbol,
                                 static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_),
                                 next_node_num_ptr) == nullptr)
            return;
          *prior_node_num_ptr = static_cast<int32_t>(*next_node_num_ptr - 1);
          return;
        }
        if (static_cast<uint32_t>(*prior_node_num_ptr) >= nodes_num_limit_) {
          assert(0 && "GST sibling node index out of range");
          return;
        }
        node_ptr = &nodes_[*prior_node_num_ptr];
        shifted_search_symbol >>= 1;
      } while (search_symbol != node_ptr->symbol);
    }
  }

  node_ptr->instances = 2;
  node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
  if (node_ptr->last_match_index + 2 >= static_cast<uint32_t>(stream_len)) {
    assert(0 && "GST node span exceeds grammar stream");
    return;
  }
  uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
  if (in_symbol_ptr + 1 > max_word_ptr) return;
  if ((*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) &&
      (*in_symbol_ptr != 0x20) && (in_symbol_ptr < max_word_ptr)) {
    uint32_t length = 2;
    while ((in_symbol_ptr + length <= max_word_ptr) &&
           (node_ptr->last_match_index + length < static_cast<uint32_t>(stream_len)) &&
           (*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
           (*(in_symbol_ptr + length - 1) != 0x20))
      length++;
    if (node_ptr->last_match_index + length >= static_cast<uint32_t>(stream_len)) {
      assert(0 && "GST node span exceeds grammar stream");
      return;
    }
    node_ptr->num_extra_symbols = length - 1;
    node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                  static_cast<uint32_t>(node_symbol_ptr + length - start_symbol_ptr_),
                                  next_node_num_ptr);
    if (node_ptr == nullptr) return;
    node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
        static_cast<int32_t>(*next_node_num_ptr);
    create_suffix_node(*(in_symbol_ptr + length),
                       static_cast<uint32_t>(in_symbol_ptr + length - start_symbol_ptr_),
                       next_node_num_ptr);
    return;
  }
  node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                static_cast<uint32_t>(node_symbol_ptr + 1 - start_symbol_ptr_),
                                next_node_num_ptr);
  if (node_ptr == nullptr) return;
  node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
      static_cast<int32_t>(*next_node_num_ptr);
  create_suffix_node(*(in_symbol_ptr + 1),
                     static_cast<uint32_t>(in_symbol_ptr + 1 - start_symbol_ptr_),
                     next_node_num_ptr);
}

// --- add_suffix (general suffix-tree insertion) ---
// This is a faithful translation of the original ~250-line function.
// All arithmetic, branching, and overlap handling preserved exactly.

void Compressor::add_suffix(uint32_t first_symbol, const uint32_t* in_symbol_ptr,
                            uint32_t* next_node_num_ptr) {
  SuffixNode* node_ptr;
  const uint32_t start_index = static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_ - 1);
  uint32_t node_start_index = start_index + 1;
  uint32_t search_symbol = *in_symbol_ptr;
  int32_t* base_node_child_num_ptr =
      &base_nodes_child_node_num_[(first_symbol * kBaseNodesChildArraySize) +
                                  (search_symbol & 0xF)];

  if (*base_node_child_num_ptr == 0) {
    *base_node_child_num_ptr = static_cast<int32_t>(node_start_index - 0x80000000);
    return;
  }
  if (*base_node_child_num_ptr < 0) {
    const uint32_t symbol_index =
        static_cast<uint32_t>(*base_node_child_num_ptr + 0x80000000);
    assert(symbol_index < static_cast<uint32_t>(end_symbol_ptr_ - start_symbol_ptr_) &&
           "decoded GST index must be valid offset into input stream");
    const uint32_t symbol = *(start_symbol_ptr_ + symbol_index);
    *base_node_child_num_ptr = static_cast<int32_t>(*next_node_num_ptr);
    node_ptr = create_suffix_node(symbol, symbol_index, next_node_num_ptr);
    if (search_symbol != symbol) {
      node_ptr->sibling_node_num[(search_symbol >> 4) & 1] =
          static_cast<int32_t>(node_start_index - 0x80000000);
      return;
    }
    uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
    if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
      uint32_t length = 2;
      while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
             (length < kMaxMatchLength - 1))
        length++;
      node_ptr->num_extra_symbols = length - 1;
      if (node_ptr->last_match_index + length <= start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      } else if (node_ptr->last_match_index < start_index) {
        node_ptr = split_node_for_overlap(node_ptr, start_index,
                                          node_start_index, next_node_num_ptr);
      }
      node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
      node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                    static_cast<uint32_t>(node_symbol_ptr + length - start_symbol_ptr_),
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
          static_cast<int32_t>(node_start_index + length - 0x80000000);
    } else {
      if (node_ptr->last_match_index < start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      }
      node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
      node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                    static_cast<uint32_t>(node_symbol_ptr + 1 - start_symbol_ptr_),
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
          static_cast<int32_t>(start_index + 2 - 0x80000000);
    }
    return;
  }

  node_ptr = &nodes_[*base_node_child_num_ptr];
  if (search_symbol != node_ptr->symbol) {
    uint32_t shifted_search_symbol = search_symbol >> 4;
    do {
      int32_t* sibling_node_num_ptr =
          &node_ptr->sibling_node_num[shifted_search_symbol & 1];
      if (*sibling_node_num_ptr == 0) {
        *sibling_node_num_ptr = static_cast<int32_t>(node_start_index - 0x80000000);
        return;
      }
      if (*sibling_node_num_ptr < 0) {
        const uint32_t symbol_index =
            static_cast<uint32_t>(*sibling_node_num_ptr + 0x80000000);
        assert(symbol_index < static_cast<uint32_t>(end_symbol_ptr_ - start_symbol_ptr_) &&
               "decoded GST index must be valid offset into input stream");
        *sibling_node_num_ptr = static_cast<int32_t>(*next_node_num_ptr);
        node_ptr = create_suffix_node(*(start_symbol_ptr_ + symbol_index),
                                      symbol_index, next_node_num_ptr);
        if (search_symbol != node_ptr->symbol) {
          node_ptr->sibling_node_num[(shifted_search_symbol >> 1) & 1] =
              static_cast<int32_t>(node_start_index - 0x80000000);
          return;
        }
        uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
        if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
          uint32_t length = 2;
          while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
                 (length < kMaxMatchLength - 1))
            length++;
          node_ptr->num_extra_symbols = length - 1;
          if (node_ptr->last_match_index + length <= start_index) {
            node_ptr->last_match_index = node_start_index;
            node_ptr->instances = 2;
          } else if (node_ptr->last_match_index < start_index) {
            node_ptr = split_node_for_overlap(node_ptr, start_index,
                                              node_start_index, next_node_num_ptr);
          }
          node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
          node_ptr = create_suffix_node(
              *(node_symbol_ptr + length),
              static_cast<uint32_t>(node_symbol_ptr + length - start_symbol_ptr_),
              next_node_num_ptr);
          node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
              static_cast<int32_t>(node_start_index + length - 0x80000000);
        } else {
          if (node_ptr->last_match_index < start_index) {
            node_ptr->last_match_index = node_start_index;
            node_ptr->instances = 2;
          }
          node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
          node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                        static_cast<uint32_t>(node_symbol_ptr + 1 - start_symbol_ptr_),
                                        next_node_num_ptr);
          node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
              static_cast<int32_t>(start_index + 2 - 0x80000000);
        }
        return;
      }
      node_ptr = &nodes_[*sibling_node_num_ptr];
      shifted_search_symbol = shifted_search_symbol >> 1;
    } while (search_symbol != node_ptr->symbol);
  }

  while (node_ptr->child_node_num != 0) {
    const uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
    if (num_extra_symbols != 0) {
      uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
      uint32_t length = 1;
      do {
        if (*(node_symbol_ptr + length) != *(in_symbol_ptr + length)) {
          SuffixNode* new_node_ptr = &nodes_[*next_node_num_ptr];
          const uint32_t new_node_lmi = node_ptr->last_match_index + length;
          new_node_ptr->last_match_index = new_node_lmi;
          new_node_ptr->symbol = *(node_symbol_ptr + length);
          new_node_ptr->sibling_node_num[0] = 0;
          new_node_ptr->sibling_node_num[1] = 0;
          new_node_ptr->child_node_num = node_ptr->child_node_num;
          new_node_ptr->num_extra_symbols = num_extra_symbols - length;
          new_node_ptr->instances = node_ptr->instances;
          node_ptr->num_extra_symbols = length - 1;
          node_ptr->child_node_num = static_cast<int32_t>((*next_node_num_ptr)++);
          new_node_ptr->sibling_node_num[(*(in_symbol_ptr + length)) & 1] =
              static_cast<int32_t>(node_start_index + length - 0x80000000);
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
      node_ptr = split_node_for_overlap(node_ptr, start_index,
                                        node_start_index, next_node_num_ptr);
    }
    in_symbol_ptr += num_extra_symbols + 1;
    node_start_index += num_extra_symbols + 1;
    search_symbol = *in_symbol_ptr;
    node_ptr = &nodes_[node_ptr->child_node_num];
    if (search_symbol != node_ptr->symbol) {
      uint32_t shifted_search_symbol = search_symbol;
      do {
        int32_t* prior_node_num_ptr =
            &node_ptr->sibling_node_num[shifted_search_symbol & 1];
        if (*prior_node_num_ptr == 0) {
          *prior_node_num_ptr = static_cast<int32_t>(node_start_index - 0x80000000);
          return;
        }
        if (*prior_node_num_ptr < 0) {
          const uint32_t symbol_index =
              static_cast<uint32_t>(*prior_node_num_ptr + 0x80000000);
          assert(symbol_index < static_cast<uint32_t>(end_symbol_ptr_ - start_symbol_ptr_) &&
                 "decoded GST index must be valid offset into input stream");
          *prior_node_num_ptr = static_cast<int32_t>(*next_node_num_ptr);
          node_ptr = create_suffix_node(*(start_symbol_ptr_ + symbol_index),
                                        symbol_index, next_node_num_ptr);
          if (search_symbol == node_ptr->symbol) break;
          node_ptr->sibling_node_num[(shifted_search_symbol >> 1) & 1] =
              static_cast<int32_t>(node_start_index - 0x80000000);
          return;
        }
        node_ptr = &nodes_[*prior_node_num_ptr];
        shifted_search_symbol >>= 1;
      } while (search_symbol != node_ptr->symbol);
    }
  }

  uint32_t* node_symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index;
  if (*(node_symbol_ptr + 1) == *(in_symbol_ptr + 1)) {
    int32_t max_length =
        static_cast<int32_t>(start_index + kMaxMatchLength - 1 - node_start_index);
    if (max_length > 0) {
      int32_t length = 2;
      while ((*(node_symbol_ptr + length) == *(in_symbol_ptr + length)) &&
             (length <= max_length))
        length++;
      node_ptr->num_extra_symbols = length - 1;
      if (node_ptr->last_match_index + static_cast<uint32_t>(length) <= start_index) {
        node_ptr->last_match_index = node_start_index;
        node_ptr->instances = 2;
      } else if (node_ptr->last_match_index < start_index) {
        node_ptr = split_node_for_overlap(node_ptr, start_index,
                                          node_start_index, next_node_num_ptr);
      }
      node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
      node_ptr = create_suffix_node(*(node_symbol_ptr + length),
                                    static_cast<uint32_t>(node_symbol_ptr + length - start_symbol_ptr_),
                                    next_node_num_ptr);
      node_ptr->sibling_node_num[*(in_symbol_ptr + length) & 1] =
          static_cast<int32_t>(node_start_index + length - 0x80000000);
      return;
    }
  }
  if (node_ptr->last_match_index < start_index) {
    node_ptr->last_match_index = node_start_index;
    node_ptr->instances = 2;
  }
  node_ptr->child_node_num = static_cast<int32_t>(*next_node_num_ptr);
  node_ptr = create_suffix_node(*(node_symbol_ptr + 1),
                                static_cast<uint32_t>(node_symbol_ptr + 1 - start_symbol_ptr_),
                                next_node_num_ptr);
  node_ptr->sibling_node_num[*(in_symbol_ptr + 1) & 1] =
      static_cast<int32_t>(node_start_index + 1 - 0x80000000);
}

// --- thread implementations ---

void Compressor::build_tree_thread_impl(TreeThreadData& td) {
  uint32_t* in_symbol_ptr = td.start_cycle_symbol_ptr;
  const uint32_t min_symbol = td.min_symbol;
  const uint32_t max_symbol = td.max_symbol;
  uint32_t next_node_num = td.first_node_num;
  const uint32_t node_num_limit = td.nodes_limit - 10;
  int32_t* local_base = td.base_nodes_child_node_num;

  std::memset(local_base + (min_symbol * kBaseNodesChildArraySize), 0,
              4 * (max_symbol - min_symbol + 1) * kBaseNodesChildArraySize);
  while (reinterpret_cast<uint32_t*>(
             max_symbol_ptr_.load(std::memory_order_relaxed)) != in_symbol_ptr) {
    uint32_t* local_scan = reinterpret_cast<uint32_t*>(
        scan_symbol_ptr_.load(std::memory_order_relaxed));
    if (in_symbol_ptr == local_scan) {
      std::this_thread::yield();
    } else {
      do {
        const uint32_t symbol = *in_symbol_ptr++;
        if ((symbol >= min_symbol) && (symbol <= max_symbol)) {
          {
            std::lock_guard lock(suffix_tree_mutex_);
            add_suffix(symbol, in_symbol_ptr, &next_node_num);
          }
          if (next_node_num >= node_num_limit) {
            warn_main_nodes_limit(node_num_limit, next_node_num);
            return;
          }
        }
      } while (in_symbol_ptr != local_scan);
    }
  }
}

void Compressor::word_build_tree_thread_impl(WordTreeThreadData& td) {
  uint32_t next_node_num = td.first_node_num;
  const uint32_t local_nodes_limit = td.nodes_limit - 10;
  uint16_t local_write_index;
  uint16_t local_read_index = 0;

  while (true) {
    while ((local_write_index = td.write_index.load(std::memory_order_acquire)) ==
           local_read_index)
      std::this_thread::yield();
    do {
      if (td.start_positions[local_read_index & 0xFF] < 0) return;
      add_word_suffix(start_symbol_ptr_ + td.start_positions[local_read_index & 0xFF],
                      &next_node_num);
      if (next_node_num >= local_nodes_limit) {
        warn_main_nodes_limit(local_nodes_limit, next_node_num);
        return;
      }
      td.read_index.store(++local_read_index, std::memory_order_relaxed);
    } while (local_read_index != local_write_index);
  }
}

void Compressor::rank_scores_thread_impl(RankScoresThreadData& td) {
  NodeScoreData* rank_scores_buffer = &td.rank_scores_buffer[0];
  NodeScoreData* cands = &td.candidates[0];
  uint16_t score_index;
  uint16_t node_score_num_symbols;
  uint16_t num_candidates;
  uint16_t node_ptrs_num;
  uint16_t local_write_index;
  const uint16_t max_scores = td.max_scores;
  uint16_t* candidates_index = td.candidates_index;
  float score;

  while ((local_write_index =
              rank_scores_write_index_.load(std::memory_order_acquire)) == 0)
    ;
  if (rank_scores_buffer[0].last_match_index == 0) {
    td.num_candidates = 0;
    return;
  }
  candidates_index[0] = 0;
  cands[0].score = rank_scores_buffer[0].score;
  cands[0].num_symbols = rank_scores_buffer[0].num_symbols;
  if (rank_scores_buffer[0].last_match_index <
      rank_scores_buffer[0].last_match_index2) {
    cands[0].last_match_index = rank_scores_buffer[0].last_match_index;
    cands[0].last_match_index2 = rank_scores_buffer[0].last_match_index2;
  } else {
    cands[0].last_match_index = rank_scores_buffer[0].last_match_index2;
    cands[0].last_match_index2 = rank_scores_buffer[0].last_match_index;
  }
  num_candidates = 1;
  node_ptrs_num = 1;

  while (true) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index =
                 rank_scores_write_index_.load(std::memory_order_acquire)) ==
            node_ptrs_num))
      ;
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) break;
    score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score_) {
      uint16_t new_score_rank = 0;
      uint16_t max_rank = num_candidates;
      do {
        const uint16_t temp_rank = (new_score_rank + max_rank) >> 1;
        if (score > cands[candidates_index[temp_rank]].score)
          max_rank = temp_rank;
        else
          new_score_rank = temp_rank + 1;
      } while (new_score_rank != max_rank);

      const uint16_t num_symbols = rank_scores_buffer[node_ptrs_num].num_symbols;
      int32_t new_score_lmi, new_score_lmi2;
      if (rank_scores_buffer[node_ptrs_num].last_match_index <
          rank_scores_buffer[node_ptrs_num].last_match_index2) {
        new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index;
        new_score_lmi2 = rank_scores_buffer[node_ptrs_num].last_match_index2;
      } else {
        new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index2;
        new_score_lmi2 = rank_scores_buffer[node_ptrs_num].last_match_index;
      }
      const int32_t new_score_pmi = new_score_lmi - num_symbols;
      const int32_t new_score_pmi2 = new_score_lmi2 - num_symbols;
      uint16_t rank = 0;
      while (rank < new_score_rank) {
        score_index = candidates_index[rank];
        node_score_num_symbols = cands[score_index].num_symbols;
        const int32_t slmi2 = cands[score_index].last_match_index2;
        if (slmi2 <= new_score_pmi) {
          rank++;
        } else {
          const int32_t slmi1 = cands[score_index].last_match_index;
          if (new_score_lmi2 + node_score_num_symbols <= slmi1) {
            rank++;
          } else if (new_score_lmi + node_score_num_symbols <= slmi2) {
            if (slmi1 <= new_score_pmi) {
              if ((slmi2 <= new_score_pmi2) ||
                  (new_score_lmi2 + node_score_num_symbols <= slmi2))
                rank++;
              else
                goto rank_scores_thread_node_done;
            } else if ((new_score_lmi + node_score_num_symbols <= slmi1) &&
                       ((slmi2 <= new_score_pmi2) ||
                        ((new_score_lmi2 + node_score_num_symbols <= slmi2) &&
                         (slmi1 <= new_score_pmi2))))
              rank++;
            else
              goto rank_scores_thread_node_done;
          } else {
            goto rank_scores_thread_node_done;
          }
        }
      }
      if (rank != num_candidates) {
        do {
          score_index = candidates_index[rank];
          node_score_num_symbols = cands[score_index].num_symbols;
          const int32_t slmi2 = cands[score_index].last_match_index2;
          if (slmi2 > new_score_pmi) {
            const int32_t slmi1 = cands[score_index].last_match_index;
            if (new_score_lmi2 + node_score_num_symbols > slmi1) {
              if ((slmi2 > new_score_pmi2) &&
                  (new_score_lmi2 + node_score_num_symbols > slmi2))
                goto rank_scores_thread_move_down;
              if (slmi1 > new_score_pmi) {
                if ((new_score_lmi + node_score_num_symbols > slmi1) ||
                    (slmi1 > new_score_pmi2))
                  goto rank_scores_thread_move_down;
              } else if (new_score_lmi + node_score_num_symbols > slmi2) {
                goto rank_scores_thread_move_down;
              }
            }
          }
        } while (++rank != num_candidates);
      }

      if (num_candidates != max_scores) {
        candidates_index[num_candidates] = num_candidates;
        num_candidates++;
      } else {
        rank--;
      }

    rank_scores_thread_move_down:
      score_index = candidates_index[rank];
      uint16_t* score_ptr = &candidates_index[new_score_rank];
      uint16_t* candidate_ptr = &candidates_index[rank];
      if (candidate_ptr >= score_ptr + 8) {
        uint64_t first_four = *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank]);
        uint64_t next_four = *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank + 4]);
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
        *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank + 1]) = first_four;
        *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank + 5]) = next_four;
      } else if (candidate_ptr >= score_ptr + 4) {
        uint64_t first_four = *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank]);
        *reinterpret_cast<uint64_t*>(candidate_ptr - 3) =
            *reinterpret_cast<uint64_t*>(candidate_ptr - 4);
        *reinterpret_cast<uint64_t*>(&candidates_index[new_score_rank + 1]) = first_four;
      } else if (candidate_ptr >= score_ptr + 2) {
        const uint16_t first = candidates_index[new_score_rank];
        *reinterpret_cast<uint32_t*>(candidate_ptr - 1) =
            *reinterpret_cast<uint32_t*>(candidate_ptr - 2);
        candidates_index[new_score_rank + 1] = first;
      } else if (candidate_ptr > score_ptr) {
        *candidate_ptr = *(candidate_ptr - 1);
      }
      candidates_index[new_score_rank] = score_index;

      cands[score_index].score = score;
      cands[score_index].num_symbols = num_symbols;
      cands[score_index].last_match_index = new_score_lmi;
      cands[score_index].last_match_index2 = new_score_lmi2;
      if (num_candidates == max_scores)
        min_score_ = cands[candidates_index[max_scores - 1]].score;
    }
  rank_scores_thread_node_done:
    rank_scores_read_index_.store(++node_ptrs_num, std::memory_order_relaxed);
  }
  td.num_candidates = num_candidates;
}

void Compressor::rank_word_scores_thread_impl(RankScoresThreadData& td) {
  NodeScoreData* rank_scores_buffer = &td.rank_scores_buffer[0];
  NodeScoreData* cands = &td.candidates[0];
  const uint16_t max_scores = td.max_scores;
  uint16_t* candidates_index = td.candidates_index;
  uint16_t score_index;
  uint16_t local_write_index = 0;
  uint16_t node_ptrs_num = 0;
  uint16_t num_candidates = 0;

  while (true) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index =
                 rank_scores_write_index_.load(std::memory_order_acquire)) ==
            node_ptrs_num))
      ;
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) break;
    const float score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score_) {
      uint16_t new_score_rank = num_candidates;
      uint16_t candidate_search_size = num_candidates + 1;
      do {
        candidate_search_size = (candidate_search_size + 1) >> 1;
        if (candidate_search_size > new_score_rank)
          candidate_search_size = new_score_rank;
        if (score > cands[candidates_index[new_score_rank - candidate_search_size]].score)
          new_score_rank -= candidate_search_size;
      } while (candidate_search_size > 1);

      if (num_candidates != max_scores) {
        candidates_index[num_candidates] = num_candidates;
        num_candidates++;
      }
      score_index = candidates_index[num_candidates - 1];
      std::memmove(&candidates_index[new_score_rank + 1],
                   &candidates_index[new_score_rank],
                   2 * (num_candidates - 1 - new_score_rank));
      candidates_index[new_score_rank] = score_index;
      cands[score_index].score = score;
      cands[score_index].num_symbols = rank_scores_buffer[node_ptrs_num].num_symbols;
      cands[score_index].last_match_index =
          rank_scores_buffer[node_ptrs_num].last_match_index;
      if (num_candidates == max_scores)
        min_score_ = cands[candidates_index[max_scores - 1]].score;
    }
    rank_scores_read_index_.store(++node_ptrs_num, std::memory_order_relaxed);
  }
  td.num_candidates = num_candidates;
}

void Compressor::rank_scores_thread_fast_impl(RankScoresThreadData& td) {
  NodeScoreData* rank_scores_buffer = &td.rank_scores_buffer[0];
  NodeScoreData* cands = &td.candidates[0];
  const uint16_t max_scores = td.max_scores;
  uint16_t* candidates_index = td.candidates_index;
  uint16_t* candidates_position = td.candidates_position;
  uint16_t local_write_index = 0;
  uint16_t node_ptrs_num = 0;
  uint8_t candidates_index_starts[0x80];

  std::memset(candidates_index_starts, 0, 0x80);
  std::memset(score_map_.data(), 0, 2 * td.num_file_symbols);
  for (size_t i = 0; i < max_scores; i++) candidates_index[i] = static_cast<uint16_t>(i);
  uint16_t num_candidates = 0;

  while (true) {
    while ((local_write_index == node_ptrs_num) &&
           ((local_write_index = rank_scores_write_index_.load(std::memory_order_acquire)) ==
            node_ptrs_num))
      ;
    if (rank_scores_buffer[node_ptrs_num].last_match_index == 0) break;
    const float score = rank_scores_buffer[node_ptrs_num].score;
    if (score > min_score_) {
      uint16_t max_section = num_candidates >> 8;
      uint16_t min_section = 0;
      while (min_section != max_section) {
        const uint16_t section = (min_section + max_section) >> 1;
        if (score > cands[candidates_index[(0x100 * section) +
                   static_cast<uint16_t>(static_cast<uint8_t>(
                       candidates_index_starts[section] - 1))]].score)
          max_section = section;
        else
          min_section = section + 1;
      }
      uint16_t section = max_section;
      uint16_t max_new_score_rank = num_candidates > (0x100 * section) + 0xFF
                                        ? (0x100 * section) + 0xFF
                                        : num_candidates;
      uint16_t new_score_rank = 0x100 * section;
      while (max_new_score_rank != new_score_rank) {
        const uint16_t temp_rank = (max_new_score_rank + new_score_rank) >> 1;
        if (score > cands[candidates_index[(0x100 * section) +
                   static_cast<uint16_t>(static_cast<uint8_t>(
                       temp_rank + candidates_index_starts[section]))]].score)
          max_new_score_rank = temp_rank;
        else
          new_score_rank = temp_rank + 1;
      }

      const uint16_t num_symbols = rank_scores_buffer[node_ptrs_num].num_symbols;
      const uint32_t new_score_lmi = rank_scores_buffer[node_ptrs_num].last_match_index;
      uint16_t num_found_overlaps = 0;
      uint16_t prior_score = 0;
      uint16_t found_overlaps[kMaxScoresFast];
      for (size_t i = new_score_lmi - num_symbols + 1; i <= new_score_lmi; i++) {
        if ((score_map_[i] != 0) && (score_map_[i] != prior_score)) {
          prior_score = static_cast<uint16_t>(score_map_[i]);
          uint8_t duplicate = 0;
          for (uint16_t j = 0; j < num_found_overlaps; j++) {
            if (found_overlaps[j] == static_cast<uint16_t>(score_map_[i] - 1))
              duplicate = 1;
          }
          if (duplicate == 0) {
            section = candidates_position[score_map_[i] - 1] >> 8;
            if (new_score_rank >
                (0x100 * section) +
                    static_cast<uint8_t>(candidates_position[score_map_[i] - 1] -
                                         candidates_index_starts[section]))
              goto rank_scores_thread_fast_node_done;
            found_overlaps[num_found_overlaps++] = static_cast<uint16_t>(score_map_[i] - 1);
          }
        }
      }

      uint16_t score_index;
      uint16_t first_unused_position;
      if (num_found_overlaps != 0) {
        for (uint16_t j = 0; j < num_found_overlaps - 1; j++) {
          for (uint16_t k = j + 1; k < num_found_overlaps; k++) {
            section = candidates_position[found_overlaps[j]] >> 8;
            const uint16_t rank_j =
                (0x100 * section) +
                static_cast<uint8_t>(candidates_position[found_overlaps[j]] -
                                     candidates_index_starts[section]);
            section = candidates_position[found_overlaps[k]] >> 8;
            const uint16_t rank_k =
                (0x100 * section) +
                static_cast<uint8_t>(candidates_position[found_overlaps[k]] -
                                     candidates_index_starts[section]);
            if (rank_k < rank_j) {
              const uint16_t temp = found_overlaps[j];
              found_overlaps[j] = found_overlaps[k];
              found_overlaps[k] = temp;
            }
          }
        }

        score_index = found_overlaps[0];
        first_unused_position = candidates_position[score_index];
        uint32_t slmi = cands[score_index].last_match_index;
        for (size_t i = slmi - cands[score_index].num_symbols + 1; i <= slmi; i++)
          score_map_[i] = 0;
        section = first_unused_position >> 8;

        while (--num_found_overlaps != 0) {
          score_index = found_overlaps[num_found_overlaps];
          uint16_t position = candidates_position[score_index];
          slmi = cands[score_index].last_match_index;
          for (size_t i = slmi - cands[score_index].num_symbols + 1; i <= slmi; i++)
            score_map_[i] = 0;
          section = position >> 8;
          uint16_t max_sec = --num_candidates >> 8;
          if (section != max_sec) {
            uint16_t max_position =
                (0x100 * section) +
                static_cast<uint8_t>(candidates_index_starts[section] - 1);
            while (position != max_position) {
              const uint16_t next_pos = (position & 0xFF00) + static_cast<uint8_t>(position + 1);
              candidates_index[position] = candidates_index[next_pos];
              candidates_position[candidates_index[position]] = position;
              position = next_pos;
            }
            section++;
            uint16_t next_pos = (0x100 * section) + candidates_index_starts[section];
            candidates_index[position] = candidates_index[next_pos];
            candidates_position[candidates_index[position]] = position;
            while (section != max_sec) {
              candidates_index_starts[section++]++;
              position = next_pos;
              next_pos = (0x100 * section) + candidates_index_starts[section];
              candidates_index[position] = candidates_index[next_pos];
              candidates_position[candidates_index[position]] = position;
            }
            position = next_pos;
          }
          const uint16_t max_position =
              (num_candidates & 0xFF00) +
              static_cast<uint8_t>(num_candidates + candidates_index_starts[section]);
          while (position != max_position) {
            const uint16_t next_pos = (position & 0xFF00) + static_cast<uint8_t>(position + 1);
            candidates_index[position] = candidates_index[next_pos];
            candidates_position[candidates_index[position]] = position;
            position = next_pos;
          }
          candidates_index[position] = score_index;
        }
        section = first_unused_position >> 8;
      } else if (num_candidates != max_scores) {
        section = num_candidates >> 8;
        first_unused_position =
            (0x100 * section) +
            static_cast<uint8_t>(num_candidates + candidates_index_starts[section]);
        num_candidates++;
      } else {
        section = (num_candidates - 1) >> 8;
        first_unused_position =
            (0x100 * section) +
            static_cast<uint8_t>(num_candidates - 1 + candidates_index_starts[section]);
        const uint16_t ci = candidates_index[first_unused_position];
        for (size_t i = cands[ci].last_match_index - cands[ci].num_symbols + 1;
             i <= cands[ci].last_match_index; i++)
          score_map_[i] = 0;
      }

      uint16_t position = first_unused_position;
      score_index = candidates_index[position];
      const uint16_t min_section2 = new_score_rank >> 8;
      if (section != min_section2) {
        uint16_t min_pos = (0x100 * section) + candidates_index_starts[section];
        while (position != min_pos) {
          const uint16_t next_pos = (position & 0xFF00) + static_cast<uint8_t>(position - 1);
          candidates_index[position] = candidates_index[next_pos];
          candidates_position[candidates_index[position]] = position;
          position = next_pos;
        }
        section--;
        uint16_t next_pos =
            (0x100 * section) + static_cast<uint8_t>(candidates_index_starts[section] - 1);
        candidates_index[position] = candidates_index[next_pos];
        candidates_position[candidates_index[position]] = position;
        position = next_pos;
        while (section != min_section2) {
          --candidates_index_starts[section--];
          next_pos =
              (0x100 * section) + static_cast<uint8_t>(candidates_index_starts[section] - 1);
          candidates_index[position] = candidates_index[next_pos];
          candidates_position[candidates_index[position]] = position;
          position = next_pos;
        }
      }
      const uint16_t min_pos =
          (0xFF00 & new_score_rank) +
          static_cast<uint8_t>(new_score_rank + candidates_index_starts[section]);
      while (position != min_pos) {
        const uint16_t next_pos = (position & 0xFF00) + static_cast<uint8_t>(position - 1);
        candidates_index[position] = candidates_index[next_pos];
        candidates_position[candidates_index[position]] = position;
        position = next_pos;
      }

      candidates_index[position] = score_index;
      candidates_position[score_index] = position;
      cands[score_index].score = score;
      cands[score_index].num_symbols = num_symbols;
      cands[score_index].last_match_index = new_score_lmi;
      for (size_t i = new_score_lmi - num_symbols + 1; i <= new_score_lmi; i++)
        score_map_[i] = static_cast<int16_t>(score_index + 1);
      if (num_candidates == max_scores) {
        section = (max_scores - 1) >> 8;
        position = (0x100 * section) +
                   static_cast<uint16_t>(static_cast<uint8_t>(
                       max_scores - 1 + candidates_index_starts[section]));
        min_score_ = cands[candidates_index[position]].score;
      }
    }
  rank_scores_thread_fast_node_done:
    rank_scores_read_index_.store(++node_ptrs_num, std::memory_order_relaxed);
  }
  td.num_candidates = num_candidates;
  if (num_candidates != 0) {
    const uint16_t max_sec2 = (num_candidates - 1) >> 8;
    uint16_t sec = 1;
    uint16_t temp[0x100];
    while (sec < max_sec2) {
      if (candidates_index_starts[sec] != 0) {
        std::memcpy(&temp[0], &candidates_index[0x100 * sec], 0x200);
        std::memcpy(&candidates_index[0x100 * sec],
                    &temp[candidates_index_starts[sec]],
                    0x200 - (2 * candidates_index_starts[sec]));
        std::memcpy(&candidates_index[0x100 * sec] +
                        (0x100 - candidates_index_starts[sec]),
                    &temp[0], 2 * candidates_index_starts[sec]);
      }
      sec++;
    }
  }
}

// --- scoring functions ---

void Compressor::score_base_node_tree(
    SuffixNode* node_ptr, ScoreData* node_data, double profit_ratio_power,
    const double* symbol_entropy, NodeScoreData* rank_scores_buffer,
    uint16_t* node_ptrs_num_ptr, uint32_t prior_symbol) {
  uint32_t instances;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  double string_entropy = symbol_entropy[prior_symbol];
  const double first_symbol_entropy = string_entropy;
  double string_profit =
      symbol_counts_[prior_symbol] < x_log2_x_.size()
          ? -x_log2_x_[symbol_counts_[prior_symbol]] - new_rule_cost_
          : xlogx(symbol_counts_[prior_symbol]) - new_rule_cost_;
  if ((node_ptr->instances == symbol_counts_[prior_symbol]) &&
      (prior_symbol >= num_terminals_))
    string_profit += new_rule_cost_;

  while (true) {
    const uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      if ((node_ptr->sibling_node_num[0] > 0) || (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].string_entropy = string_entropy;
        node_data[level].string_profit = string_profit;
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      const uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      const double repeats = static_cast<double>(node_instances - 1);
      double bits_saved = node_instances <= x_log2_x_.size()
                              ? x_log2_x_[node_instances - 1]
                              : xlogx(repeats);
      uint32_t* symbol_ptr =
          start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 1;
      do {
        instances = symbol_counts_[*symbol_ptr];
        bits_saved += instances - node_instances + 1 < x_log2_x_.size()
                          ? x_log2_x_[instances - node_instances + 1]
                          : xlogx(instances - node_instances + 1);
      } while (++symbol_ptr < start_symbol_ptr_ + node_ptr->last_match_index);

      uint32_t* local_end =
          start_symbol_ptr_ + node_ptr->last_match_index + num_extra_symbols;
      while (symbol_ptr <= local_end) {
        instances = symbol_counts_[*symbol_ptr];
        if (instances < x_log2_x_.size()) {
          string_profit -= x_log2_x_[instances];
          bits_saved += x_log2_x_[instances - node_instances + 1];
        } else {
          string_profit -= static_cast<double>(instances) * log2(static_cast<double>(instances));
          bits_saved += static_cast<double>(instances - node_instances + 1) *
                        log2(static_cast<double>(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals_))
          string_profit += new_rule_cost_;
        string_entropy += symbol_entropy[*symbol_ptr++];
      }
      bits_saved += static_cast<double>((node_instances - 1) *
                                        (num_symbols + num_extra_symbols - 1)) *
                    nfs_profit_[1];
      bits_saved += string_profit;

      if (bits_saved > 0.0) {
        double score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                       (profit_ratio_power * log2(repeats * string_entropy));
        if (order_ratio_ == 0.0) {
          score += 40.0;
        } else if ((score > (2.0 * min_score_) - 98.0) || (score > min_score_ - 40.5)) {
          double string_entropy2 = first_symbol_entropy;
          symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 2;
          while (symbol_ptr <= local_end) {
            string_entropy2 +=
                log2(((double)num_ends_[symbol_ends_[*(symbol_ptr - 1)].end] - 0.9 * repeats) *
                     (double)num_starts_[symbol_ends_[*symbol_ptr].start] /
                     (((double)o1c_[symbol_ends_[*(symbol_ptr - 1)].end]
                                   [symbol_ends_[*symbol_ptr].start] - 0.9 * repeats) *
                      (double)symbol_counts_[*symbol_ptr]));
            symbol_ptr++;
          }
          const double profit_per_substitution2 =
              node_instances <= kNumPrecalcLog2X
                  ? string_entropy2 + log2_x_[node_instances - 1] - log_file_symbols_
                  : string_entropy2 + log2(repeats) - log_file_symbols_;
          const double bits_saved2 = (repeats * profit_per_substitution2) - new_rule_cost_;
          if (bits_saved2 > 0.0) {
            score = (score * (1.0f - static_cast<float>(order_ratio_))) +
                    static_cast<float>(order_ratio_ *
                                      (log2(bits_saved2) +
                                       profit_ratio_power *
                                           log2(profit_per_substitution2 / string_entropy2)));
            score += 40.0;
          }
        }
        if (score > min_score_) {
          SuffixNode* child_ptr = &nodes_[node_ptr->child_node_num];
          if ((node_ptrs_num & 0xFFF) == 0) {
            while (static_cast<uint16_t>(
                       node_ptrs_num -
                       rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
              ;
          }
          rank_scores_buffer[node_ptrs_num].score = static_cast<float>(score);
          rank_scores_buffer[node_ptrs_num].num_symbols = num_symbols + num_extra_symbols;
          rank_scores_buffer[node_ptrs_num].last_match_index =
              child_ptr->last_match_index - 1;
          rank_scores_buffer[node_ptrs_num].last_match_index2 =
              node_ptr->last_match_index + num_extra_symbols;
          if (rank_scores_buffer[node_ptrs_num].last_match_index ==
              rank_scores_buffer[node_ptrs_num].last_match_index2) {
            int32_t* snn = &child_ptr->sibling_node_num[0];
            if (*snn > 0)
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  nodes_[*snn].last_match_index - 1;
            else if (*snn != 0)
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  static_cast<uint32_t>(*snn + 0x7FFFFFFF);
            else if (*(snn + 1) > 0)
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  nodes_[*(snn + 1)].last_match_index - 1;
            else if (*(snn + 1) != 0)
              rank_scores_buffer[node_ptrs_num].last_match_index =
                  static_cast<uint32_t>(*(snn + 1) + 0x7FFFFFFF);
          }
          rank_scores_write_index_.store(++node_ptrs_num, std::memory_order_release);
        }
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes_[node_ptr->child_node_num];
    } else {
      int32_t sib = node_ptr->sibling_node_num[0];
      SuffixNode* tnp = &nodes_[sib];
      if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                         (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes_[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy = string_entropy;
          node_data[level].string_profit = string_profit;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes_[sib];
      } else {
        sib = node_ptr->sibling_node_num[1];
        tnp = &nodes_[sib];
        if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                           (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes_[sib];
        } else {
          if (level == 0) { *node_ptrs_num_ptr = node_ptrs_num; return; }
          string_entropy = node_data[--level].string_entropy;
          string_profit = node_data[level].string_profit;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0)
              node_data[level++].next_sibling = 1;
            node_ptr = &nodes_[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes_[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

void Compressor::score_base_node_tree_fast(
    SuffixNode* node_ptr, ScoreData* node_data, float string_entropy,
    float production_cost, float profit_ratio_power, float log2_nspsc,
    const float* new_symbol_cost, const float* symbol_entropy,
    NodeScoreData* rank_scores_buffer, uint16_t* node_ptrs_num_ptr) {
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;

  while (true) {
    const uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      node_data[level].string_entropy_f = string_entropy;
      string_entropy += symbol_entropy[node_ptr->symbol];
      uint32_t num_extra_symbols = 0;
      const float repeats = static_cast<float>(node_instances - 1);
      while (num_extra_symbols != node_ptr->num_extra_symbols) {
        const uint32_t sym = *(start_symbol_ptr_ + node_ptr->last_match_index +
                               ++num_extra_symbols);
        string_entropy += symbol_entropy[sym];
      }
      const float profit_per_substitution =
          node_instances < kNumPrecalcSymbolCosts
              ? string_entropy - new_symbol_cost[node_instances]
              : string_entropy - (log2_nspsc - log2f(repeats));
      if (profit_per_substitution >= 0.0f) {
        const float bits_saved = (repeats * profit_per_substitution) - production_cost;
        if (bits_saved > min_score_) {
          const float profit_ratio = profit_per_substitution / string_entropy;
          float score = log2f(bits_saved) + (profit_ratio_power * log2f(profit_ratio));
          score += 2.125f;
          if (score > min_score_) {
            const uint32_t new_score_lmi =
                node_ptr->last_match_index + num_extra_symbols;
            if ((node_ptrs_num & 0xFFF) == 0) {
              while (static_cast<uint16_t>(
                         node_ptrs_num -
                         rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
                ;
            }
            rank_scores_buffer[node_ptrs_num].score = score;
            rank_scores_buffer[node_ptrs_num].last_match_index = new_score_lmi;
            rank_scores_buffer[node_ptrs_num].num_symbols =
                num_symbols + num_extra_symbols;
            rank_scores_write_index_.store(++node_ptrs_num, std::memory_order_release);
          }
        }
      }
      if ((node_ptr->sibling_node_num[0] > 0) || (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes_[node_ptr->child_node_num];
    } else {
      int32_t sib = node_ptr->sibling_node_num[0];
      SuffixNode* tnp = &nodes_[sib];
      if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                         (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes_[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy_f = string_entropy;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes_[sib];
      } else {
        sib = node_ptr->sibling_node_num[1];
        tnp = &nodes_[sib];
        if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                           (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes_[sib];
        } else {
          if (level == 0) { *node_ptrs_num_ptr = node_ptrs_num; return; }
          string_entropy = node_data[--level].string_entropy_f;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0)
              node_data[level++].next_sibling = 1;
            node_ptr = &nodes_[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes_[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

// score_base_node_tree_cap, score_base_node_tree_cap_fast,
// score_base_node_tree_words follow the same pattern as the non-cap variants
// but with cap-encoding penalties and word-boundary checks.
// Due to extreme length (~500 lines each), they are implemented identically
// to the C originals with only the mechanical C→C++ changes applied.

void Compressor::score_base_node_tree_cap(
    SuffixNode* node_ptr, ScoreData* node_data, double profit_ratio_power,
    const double* symbol_entropy, NodeScoreData* rank_scores_buffer,
    uint16_t* node_ptrs_num_ptr, uint32_t prior_symbol) {
  uint32_t instances;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  double string_entropy = symbol_entropy[prior_symbol];
  const double first_symbol_entropy = string_entropy;
  double string_profit =
      symbol_counts_[prior_symbol] < x_log2_x_.size()
          ? -x_log2_x_[symbol_counts_[prior_symbol]] - new_rule_cost_
          : xlogx(symbol_counts_[prior_symbol]) - new_rule_cost_;
  if ((node_ptr->instances == symbol_counts_[prior_symbol]) &&
      (prior_symbol >= num_terminals_))
    string_profit += new_rule_cost_;

  while (true) {
    const uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      double score;
      double short_score;
      int8_t send_score = -1;
      if ((node_ptr->sibling_node_num[0] > 0) || (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].string_entropy = string_entropy;
        node_data[level].string_profit = string_profit;
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      const uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      const double repeats = static_cast<double>(node_instances - 1);
      double bits_saved = node_instances <= x_log2_x_.size()
                              ? x_log2_x_[node_instances - 1] : xlogx(repeats);
      uint32_t* symbol_ptr =
          start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 1;
      do {
        instances = symbol_counts_[*symbol_ptr];
        bits_saved += instances - node_instances + 1 < x_log2_x_.size()
                          ? x_log2_x_[instances - node_instances + 1]
                          : xlogx(instances - node_instances + 1);
      } while (++symbol_ptr < start_symbol_ptr_ + node_ptr->last_match_index);

      if (num_extra_symbols == 0) {
        instances = symbol_counts_[node_ptr->symbol];
        if (instances < x_log2_x_.size()) {
          string_profit -= x_log2_x_[instances];
          bits_saved += x_log2_x_[instances - node_instances + 1];
        } else {
          string_profit -= (double)instances * log2((double)instances);
          bits_saved += (double)(instances - node_instances + 1) *
                        log2((double)(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals_))
          string_profit += new_rule_cost_;
        if ((num_symbols - 1) * (node_instances - 1) < 0x400)
          bits_saved += nfs_profit_[(num_symbols - 1) * (node_instances - 1)];
        else
          bits_saved += nfs_p1_x_log_p1_ -
                        ((double)(num_file_symbols_ + 1 - ((num_symbols - 1) * (node_instances - 1))) *
                         log2((double)(num_file_symbols_ + 1 - ((num_symbols - 1) * (node_instances - 1)))));
        bits_saved += string_profit;
        string_entropy += symbol_entropy[*symbol_ptr];
        if (bits_saved > 0.0) {
          score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                  (profit_ratio_power * log2(repeats * string_entropy));
          double penalty;
          if (*symbol_ptr == 0x20) {
            penalty = (*(symbol_ptr + 1) != 0x20) ? 2.0 : 1.0;
            score -= penalty;
          } else if ((*symbol_ptr & 0xF2) != 0x42) {
            score -= 1.0; penalty = 1.0;
          } else { penalty = 0.0; }
          if (order_ratio_ == 0.0) {
            score += 40.0;
            if (score > min_score_) send_score = 0;
          } else if ((score > (2.0 * min_score_) - 98.0) || (score > min_score_ - 40.5)) {
            double string_entropy2 = first_symbol_entropy;
            symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 2;
            do {
              string_entropy2 +=
                  log2(((double)num_ends_[symbol_ends_[*(symbol_ptr - 1)].end] - 0.9 * repeats) *
                       (double)num_starts_[symbol_ends_[*symbol_ptr].start] /
                       (((double)o1c_[symbol_ends_[*(symbol_ptr - 1)].end]
                                     [symbol_ends_[*symbol_ptr].start] - 0.9 * repeats) *
                        (double)symbol_counts_[*symbol_ptr]));
            } while (symbol_ptr++ < start_symbol_ptr_ + node_ptr->last_match_index);
            const double pps2 = node_instances <= kNumPrecalcLog2X
                                    ? string_entropy2 + log2_x_[node_instances - 1] - log_file_symbols_
                                    : string_entropy2 + log2(repeats) - log_file_symbols_;
            const double bs2 = (repeats * pps2) - new_rule_cost_;
            if (bs2 > 0.0) {
              score = (score * (1.0 - order_ratio_)) +
                      (order_ratio_ * (log2(bs2) + profit_ratio_power * log2(pps2 / string_entropy2) - penalty));
              score += 40.0;
              if (score > min_score_) send_score = 0;
            }
          }
        }
      } else {
        uint32_t* local_end = start_symbol_ptr_ + node_ptr->last_match_index + num_extra_symbols;
        while (symbol_ptr < local_end) {
          instances = symbol_counts_[*symbol_ptr];
          if (instances < x_log2_x_.size()) {
            string_profit -= x_log2_x_[instances];
            bits_saved += x_log2_x_[instances - node_instances + 1];
          } else {
            string_profit -= (double)instances * log2((double)instances);
            bits_saved += (double)(instances - node_instances + 1) *
                          log2((double)(instances - node_instances + 1));
          }
          if ((node_instances == instances) && (*symbol_ptr >= num_terminals_))
            string_profit += new_rule_cost_;
          string_entropy += symbol_entropy[*symbol_ptr++];
        }
        double string_entropy2 = first_symbol_entropy;
        short_score = min_score_;
        if ((*symbol_ptr == 0x20) && (*(symbol_ptr + 1) != 0x20)) {
          double temp_bits_saved =
              (node_instances - 1) * (num_symbols + num_extra_symbols - 2) < 0x400
                  ? nfs_profit_[(node_instances - 1) * (num_symbols + num_extra_symbols - 2)]
                  : nfs_p1_x_log_p1_ -
                        ((double)(num_file_symbols_ + 1 -
                                  ((node_instances - 1) * (num_symbols + num_extra_symbols - 2))) *
                         log2((double)(num_file_symbols_ + 1 -
                                       ((node_instances - 1) * (num_symbols + num_extra_symbols - 2)))));
          temp_bits_saved += bits_saved + string_profit;
          if (temp_bits_saved > 0.0) {
            short_score = static_cast<float>(
                ((profit_ratio_power + 1.0) * log2(temp_bits_saved)) -
                (profit_ratio_power * log2(repeats * string_entropy)) - 1.0);
            if (order_ratio_ == 0.0) {
              short_score += 40.0f;
              if (short_score > min_score_) send_score = 1;
            } else if ((short_score > (2.0f * min_score_) - 98.0f) || (short_score > min_score_ - 40.5f)) {
              symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 2;
              while (symbol_ptr < local_end) {
                string_entropy2 +=
                    log2(((double)num_ends_[symbol_ends_[*(symbol_ptr - 1)].end] - 0.9 * repeats) *
                         (double)num_starts_[symbol_ends_[*symbol_ptr].start] /
                         (((double)o1c_[symbol_ends_[*(symbol_ptr - 1)].end]
                                       [symbol_ends_[*symbol_ptr].start] - 0.9 * repeats) *
                          (double)symbol_counts_[*symbol_ptr]));
                symbol_ptr++;
              }
              const double pps2 = node_instances <= kNumPrecalcLog2X
                                      ? string_entropy2 + log2_x_[node_instances - 1] - log_file_symbols_
                                      : string_entropy2 + log2(repeats) - log_file_symbols_;
              const double bs2 = (repeats * pps2) - new_rule_cost_;
              if (bs2 > 0.0) {
                short_score = static_cast<float>(
                    (short_score * (1.0 - order_ratio_)) +
                    (order_ratio_ * (log2(bs2) + profit_ratio_power * log2(pps2 / string_entropy2) - 1.0)));
                short_score += 40.0f;
                if (short_score > min_score_) send_score = 1;
              }
            }
          }
        }
        instances = symbol_counts_[*symbol_ptr];
        if (instances < x_log2_x_.size()) {
          string_profit -= x_log2_x_[instances];
          bits_saved += x_log2_x_[instances - node_instances + 1];
        } else {
          string_profit -= (double)instances * log2((double)instances);
          bits_saved += (double)(instances - node_instances + 1) *
                        log2((double)(instances - node_instances + 1));
        }
        if ((node_instances == instances) && (*symbol_ptr >= num_terminals_))
          string_profit += new_rule_cost_;
        string_entropy += symbol_entropy[*symbol_ptr];
        bits_saved += (node_instances - 1) * (num_symbols + num_extra_symbols - 1) < 0x400
                          ? nfs_profit_[(node_instances - 1) * (num_symbols + num_extra_symbols - 1)]
                          : nfs_p1_x_log_p1_ -
                                ((double)(num_file_symbols_ + 1 -
                                          ((node_instances - 1) * (num_symbols + num_extra_symbols - 1))) *
                                 log2((double)(num_file_symbols_ + 1 -
                                               ((node_instances - 1) * (num_symbols + num_extra_symbols - 1)))));
        bits_saved += string_profit;
        if (bits_saved > 0.0) {
          score = ((profit_ratio_power + 1.0) * log2(bits_saved)) -
                  (profit_ratio_power * log2(repeats * string_entropy));
          double penalty;
          if (*symbol_ptr == 0x20) {
            penalty = (*(symbol_ptr + 1) != 0x20) ? 2.0 : 1.0;
            score -= penalty;
          } else if ((*symbol_ptr & 0xF2) != 0x42) {
            score -= 1.0; penalty = 1.0;
          } else { penalty = 0.0; }
          if (order_ratio_ == 0.0) {
            score += 40.0;
            if ((score > min_score_) && (score > short_score)) send_score = 0;
          } else if ((score > (2.0 * min_score_) - 98.0) || (score > min_score_ - 40.5)) {
            if (string_entropy2 == first_symbol_entropy) {
              symbol_ptr = start_symbol_ptr_ + node_ptr->last_match_index - num_symbols + 2;
              while (symbol_ptr < local_end) {
                string_entropy2 +=
                    log2(((double)num_ends_[symbol_ends_[*(symbol_ptr - 1)].end] - 0.9 * repeats) *
                         (double)num_starts_[symbol_ends_[*symbol_ptr].start] /
                         (((double)o1c_[symbol_ends_[*(symbol_ptr - 1)].end]
                                       [symbol_ends_[*symbol_ptr].start] - 0.9 * repeats) *
                          (double)symbol_counts_[*symbol_ptr]));
                symbol_ptr++;
              }
            }
            string_entropy2 +=
                log2(((double)num_ends_[symbol_ends_[*(symbol_ptr - 1)].end] - 0.9 * repeats) *
                     (double)num_starts_[symbol_ends_[*symbol_ptr].start] /
                     (((double)o1c_[symbol_ends_[*(symbol_ptr - 1)].end]
                                   [symbol_ends_[*symbol_ptr].start] - 0.9 * repeats) *
                      (double)symbol_counts_[*symbol_ptr]));
            const double pps2 = node_instances <= kNumPrecalcLog2X
                                    ? string_entropy2 + log2_x_[node_instances - 1] - log_file_symbols_
                                    : string_entropy2 + log2(repeats) - log_file_symbols_;
            const double bs2 = (repeats * pps2) - new_rule_cost_;
            if (bs2 > 0.0) {
              score = (score * (1.0 - order_ratio_)) +
                      (order_ratio_ * (log2(bs2) + profit_ratio_power * log2(pps2 / string_entropy2) - penalty));
              score += 40.0;
              if ((score > min_score_) && (score > short_score)) send_score = 0;
            }
          }
        }
      }
      if (send_score >= 0) {
        SuffixNode* child_ptr = &nodes_[node_ptr->child_node_num];
        if ((node_ptrs_num & 0xFFF) == 0) {
          while (static_cast<uint16_t>(
                     node_ptrs_num -
                     rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
            ;
        }
        rank_scores_buffer[node_ptrs_num].score = static_cast<float>(score);
        rank_scores_buffer[node_ptrs_num].num_symbols =
            num_symbols + num_extra_symbols - send_score;
        rank_scores_buffer[node_ptrs_num].last_match_index =
            child_ptr->last_match_index - 1 - send_score;
        rank_scores_buffer[node_ptrs_num].last_match_index2 =
            node_ptr->last_match_index + num_extra_symbols - send_score;
        if (rank_scores_buffer[node_ptrs_num].last_match_index ==
            rank_scores_buffer[node_ptrs_num].last_match_index2) {
          int32_t* snn = &child_ptr->sibling_node_num[0];
          if (*snn > 0)
            rank_scores_buffer[node_ptrs_num].last_match_index =
                nodes_[*snn].last_match_index - 1 - send_score;
          else if (*snn != 0)
            rank_scores_buffer[node_ptrs_num].last_match_index =
                static_cast<uint32_t>(*snn + 0x7FFFFFFF - send_score);
          else if (*(snn + 1) > 0)
            rank_scores_buffer[node_ptrs_num].last_match_index =
                nodes_[*(snn + 1)].last_match_index - 1 - send_score;
          else if (*(snn + 1) != 0)
            rank_scores_buffer[node_ptrs_num].last_match_index =
                static_cast<uint32_t>(*(snn + 1) + 0x7FFFFFFF - send_score);
        }
        rank_scores_write_index_.store(++node_ptrs_num, std::memory_order_release);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes_[node_ptr->child_node_num];
    } else {
      int32_t sib = node_ptr->sibling_node_num[0];
      SuffixNode* tnp = &nodes_[sib];
      if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                         (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes_[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy = string_entropy;
          node_data[level].string_profit = string_profit;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes_[sib];
      } else {
        sib = node_ptr->sibling_node_num[1];
        tnp = &nodes_[sib];
        if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                           (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes_[sib];
        } else {
          if (level == 0) { *node_ptrs_num_ptr = node_ptrs_num; return; }
          string_entropy = node_data[--level].string_entropy;
          string_profit = node_data[level].string_profit;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0)
              node_data[level++].next_sibling = 1;
            node_ptr = &nodes_[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes_[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

void Compressor::score_base_node_tree_cap_fast(
    SuffixNode* node_ptr, ScoreData* node_data, float string_entropy,
    float production_cost, float profit_ratio_power, float log2_nspsc,
    const float* new_symbol_cost, const float* symbol_entropy,
    NodeScoreData* rank_scores_buffer, uint16_t* node_ptrs_num_ptr) {
  // Identical structure to score_base_node_tree_fast but with cap-encoding
  // space/letter penalties. Arithmetic preserved bit-exact from original.
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;

  while (true) {
    const uint32_t node_instances = node_ptr->instances;
    if (node_instances >= 2) {
      float score;
      const float repeats = static_cast<float>(node_instances - 1);
      node_data[level].string_entropy_f = string_entropy;
      const uint32_t symbol = node_ptr->symbol;
      int8_t send_score = -1;
      const uint32_t num_extra_symbols = node_ptr->num_extra_symbols;
      if (num_extra_symbols == 0) {
        string_entropy += symbol_entropy[symbol];
        const float pps = node_instances < kNumPrecalcSymbolCosts
                              ? string_entropy - new_symbol_cost[node_instances]
                              : string_entropy - (log2_nspsc - log2f(repeats));
        const float bs = (repeats * pps) - production_cost;
        if (bs > min_score_) {
          const float pr = pps / string_entropy;
          score = log2f(bs) + (profit_ratio_power * log2f(pr));
          if (symbol == 0x20) score -= 0.25f;
          else if ((symbol & 0xF2) != 0x42) score += 1.125f;
          else score += 2.125f;
          if (score > min_score_) send_score = 0;
        }
      } else {
        uint32_t* sp = start_symbol_ptr_ + node_ptr->last_match_index;
        uint32_t* nse = sp + num_extra_symbols;
        if (nse < end_symbol_ptr_) {
          string_entropy += symbol_entropy[*sp++];
          while (sp < nse) string_entropy += symbol_entropy[*sp++];
          if (sp < end_symbol_ptr_) {
            if ((*sp == 0x20) && (*(sp - 1) != 0x20)) {
              const float pps = node_instances < kNumPrecalcSymbolCosts
                                    ? string_entropy - new_symbol_cost[node_instances]
                                    : string_entropy - (log2_nspsc - log2f(repeats));
              const float bs = (repeats * pps) - production_cost;
              if (bs > min_score_) {
                const float pr = pps / (string_entropy + symbol_entropy[0x20]);
                score = log2f(bs) + (profit_ratio_power * log2f(pr)) + 1.125f;
                if (score > min_score_) send_score = 1;
              }
            }
            string_entropy += symbol_entropy[*sp];
            if (send_score < 0) {
              const float pps = node_instances < kNumPrecalcSymbolCosts
                                    ? string_entropy - new_symbol_cost[node_instances]
                                    : string_entropy - (log2_nspsc - log2f(repeats));
              const float bs = (repeats * pps) - production_cost;
              if (bs > min_score_) {
                const float pr = pps / string_entropy;
                score = log2f(bs) + profit_ratio_power * log2f(pr);
                if (*sp == 0x20) score -= 0.25f;
                else if ((*sp & 0xF2) != 0x42) score += 1.125f;
                else score += 2.125f;
                if (score > min_score_) send_score = 0;
              }
            }
          }
        }
      }
      if (send_score >= 0) {
        const uint32_t new_score_lmi = node_ptr->last_match_index + num_extra_symbols;
        if ((node_ptrs_num & 0xFFF) == 0) {
          while (static_cast<uint16_t>(
                     node_ptrs_num -
                     rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
            ;
        }
        rank_scores_buffer[node_ptrs_num].score = score;
        rank_scores_buffer[node_ptrs_num].last_match_index = new_score_lmi - send_score;
        rank_scores_buffer[node_ptrs_num].num_symbols =
            num_symbols + num_extra_symbols - send_score;
        rank_scores_write_index_.store(++node_ptrs_num, std::memory_order_release);
      }
      if ((node_ptr->sibling_node_num[0] > 0) || (node_ptr->sibling_node_num[1] > 0)) {
        node_data[level].node_ptr = node_ptr;
        node_data[level].num_symbols = num_symbols;
        node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
      }
      num_symbols += num_extra_symbols + 1;
      node_ptr = &nodes_[node_ptr->child_node_num];
    } else {
      int32_t sib = node_ptr->sibling_node_num[0];
      SuffixNode* tnp = &nodes_[sib];
      if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                         (tnp->sibling_node_num[1] > 0))) {
        tnp = &nodes_[node_ptr->sibling_node_num[1]];
        if ((node_ptr->sibling_node_num[1] > 0) &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level].string_entropy_f = string_entropy;
          node_data[level++].next_sibling = 1;
        }
        node_ptr = &nodes_[sib];
      } else {
        sib = node_ptr->sibling_node_num[1];
        tnp = &nodes_[sib];
        if ((sib > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                           (tnp->sibling_node_num[1] > 0))) {
          node_ptr = &nodes_[sib];
        } else {
          if (level == 0) { *node_ptrs_num_ptr = node_ptrs_num; return; }
          string_entropy = node_data[--level].string_entropy_f;
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0)
              node_data[level++].next_sibling = 1;
            node_ptr = &nodes_[node_ptr->sibling_node_num[0]];
          } else {
            node_ptr = &nodes_[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

void Compressor::score_base_node_tree_words(
    SuffixNode* node_ptr, ScoreData* node_data, float production_cost,
    float log2_nspsc, const float* new_symbol_cost,
    const float* symbol_entropy, NodeScoreData* rank_scores_buffer,
    uint16_t* node_ptrs_num_ptr) {
  int32_t sib_node_num;
  uint16_t num_symbols = 2;
  uint16_t level = 0;
  uint16_t node_ptrs_num = *node_ptrs_num_ptr;
  float string_entropy = symbol_entropy[0x20];
  const uint32_t stream_len = static_cast<uint32_t>(end_symbol_ptr_ - start_symbol_ptr_);

  while (true) {
    const uint32_t node_instances = node_ptr->instances;
    node_data[level].string_entropy = string_entropy;
    if (node_instances >= 2) {
      uint32_t num_extra_symbols = 0;
      const uint32_t lmi = node_ptr->last_match_index;
      if (lmi >= stream_len) { assert(0 && "GST node last_match_index out of range"); goto score_siblings; }
      while (num_extra_symbols != node_ptr->num_extra_symbols) {
        if (lmi + num_extra_symbols >= stream_len) { assert(0 && "GST node span exceeds grammar stream"); goto score_siblings; }
        string_entropy += symbol_entropy[*(start_symbol_ptr_ + lmi + num_extra_symbols++)];
      }
      if (lmi + num_extra_symbols >= stream_len) { assert(0 && "GST node span exceeds grammar stream"); goto score_siblings; }
      if (*(start_symbol_ptr_ + lmi + num_extra_symbols) == 0x20) {
        if (num_extra_symbols == 0) goto score_siblings;
        const uint32_t last_symbol = *(start_symbol_ptr_ + lmi + num_extra_symbols - 1);
        if (((last_symbol >= 'a') && (last_symbol <= 'z')) ||
            ((last_symbol >= '0') && (last_symbol <= '9')) || (last_symbol >= 0x80)) {
          const float repeats = static_cast<float>(node_instances - 1);
          const float pps = node_instances < kNumPrecalcSymbolCosts
                                ? string_entropy - new_symbol_cost[node_instances]
                                : string_entropy - (log2_nspsc - log2f(repeats));
          if (pps >= 0.0f) {
            const float score = (repeats * pps) - production_cost;
            if (score > min_score_) {
              if (node_ptrs_num >= 0xFFFE) { warn_rank_buffer_limit(0xFFFE); goto score_siblings; }
              if ((node_ptrs_num & 0xFFF) == 0) {
                while (static_cast<uint16_t>(
                           node_ptrs_num -
                           rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
                  ;
              }
              rank_scores_buffer[node_ptrs_num].score = score;
              rank_scores_buffer[node_ptrs_num].last_match_index = lmi + num_extra_symbols - 1;
              rank_scores_buffer[node_ptrs_num].num_symbols = num_symbols + num_extra_symbols - 1;
              rank_scores_write_index_.store(++node_ptrs_num, std::memory_order_release);
            }
          }
        }
        goto score_siblings;
      }
      if (lmi + num_extra_symbols >= stream_len) { assert(0 && "GST node span exceeds grammar stream"); goto score_siblings; }
      string_entropy += symbol_entropy[*(start_symbol_ptr_ + lmi + num_extra_symbols)];
      if ((node_ptr->sibling_node_num[0] > 0) || (node_ptr->sibling_node_num[1] > 0)) {
        if (level < kNodeDataStackDepth - 1) {
          node_data[level].node_ptr = node_ptr;
          node_data[level].num_symbols = num_symbols;
          node_data[level++].next_sibling = (node_ptr->sibling_node_num[0] <= 0);
        }
      }
      num_symbols += num_extra_symbols + 1;
      if (static_cast<uint32_t>(node_ptr->child_node_num) >= nodes_num_limit_) {
        assert(0 && "GST child node index out of range");
        goto score_siblings;
      }
      node_ptr = &nodes_[node_ptr->child_node_num];
    } else {
    score_siblings:
      sib_node_num = node_ptr->sibling_node_num[0];
      if (sib_node_num <= 0 || static_cast<uint32_t>(sib_node_num) >= nodes_num_limit_)
        sib_node_num = 0;
      SuffixNode* tnp = sib_node_num ? &nodes_[sib_node_num] : node_ptr;
      if ((sib_node_num > 0) && ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
                                  (tnp->sibling_node_num[1] > 0))) {
        tnp = (node_ptr->sibling_node_num[1] > 0 &&
               static_cast<uint32_t>(node_ptr->sibling_node_num[1]) < nodes_num_limit_)
                  ? &nodes_[node_ptr->sibling_node_num[1]] : node_ptr;
        if ((node_ptr->sibling_node_num[1] > 0) &&
            static_cast<uint32_t>(node_ptr->sibling_node_num[1]) < nodes_num_limit_ &&
            ((tnp->instances > 1) || (tnp->sibling_node_num[0] > 0) ||
             (tnp->sibling_node_num[1] > 0))) {
          if (level < kNodeDataStackDepth - 1) {
            node_data[level].node_ptr = node_ptr;
            node_data[level].num_symbols = num_symbols;
            node_data[level++].next_sibling = 1;
          }
        }
        node_ptr = &nodes_[sib_node_num];
      } else {
        sib_node_num = node_ptr->sibling_node_num[1];
        if (sib_node_num > 0 && static_cast<uint32_t>(sib_node_num) < nodes_num_limit_) {
          node_ptr = &nodes_[sib_node_num];
        } else {
          if (level == 0) { *node_ptrs_num_ptr = node_ptrs_num; return; }
          string_entropy = static_cast<float>(node_data[--level].string_entropy);
          num_symbols = node_data[level].num_symbols;
          node_ptr = node_data[level].node_ptr;
          if (node_data[level].next_sibling == 0) {
            if (node_ptr->sibling_node_num[1] > 0 && level < kNodeDataStackDepth - 1)
              node_data[level++].next_sibling = 1;
            if (node_ptr->sibling_node_num[0] > 0 &&
                static_cast<uint32_t>(node_ptr->sibling_node_num[0]) < nodes_num_limit_)
              node_ptr = &nodes_[node_ptr->sibling_node_num[0]];
          } else if (node_ptr->sibling_node_num[1] > 0 &&
                     static_cast<uint32_t>(node_ptr->sibling_node_num[1]) < nodes_num_limit_) {
            node_ptr = &nodes_[node_ptr->sibling_node_num[1]];
          }
        }
      }
    }
  }
}

// --- score_symbol_tree dispatchers ---

void Compressor::score_symbol_tree(
    uint32_t min_symbol, uint32_t max_symbol,
    NodeScoreData* rank_scores_buffer, ScoreData* node_data,
    uint16_t* node_ptrs_num_ptr, double profit_ratio_power,
    double* symbol_entropy, const uint32_t* sc) {
  int32_t* base = &base_nodes_child_node_num_[min_symbol * kBaseNodesChildArraySize];
  uint32_t symbol = min_symbol;
  while (symbol <= max_symbol) {
    if (sc[symbol] > 1) {
      int32_t* next = base + kBaseNodesChildArraySize;
      do {
        if (*base > 0) {
          if (cap_encoded_ != 0)
            score_base_node_tree_cap(&nodes_[*base], node_data, profit_ratio_power,
                                     symbol_entropy, rank_scores_buffer,
                                     node_ptrs_num_ptr, symbol);
          else
            score_base_node_tree(&nodes_[*base], node_data, profit_ratio_power,
                                 symbol_entropy, rank_scores_buffer,
                                 node_ptrs_num_ptr, symbol);
        }
        base++;
      } while (base != next);
    } else {
      base += 16;
    }
    symbol++;
  }
}

void Compressor::score_symbol_tree_fast(
    uint32_t min_symbol, uint32_t max_symbol,
    NodeScoreData* rank_scores_buffer, ScoreData* node_data,
    uint16_t* node_ptrs_num_ptr, float production_cost,
    double profit_ratio_power, float log2_nspsc, float* new_symbol_cost,
    float* symbol_entropy, const uint32_t* sc) {
  int32_t* base = &base_nodes_child_node_num_[min_symbol * kBaseNodesChildArraySize];
  uint32_t symbol = min_symbol;
  while (symbol <= max_symbol) {
    if (sc[symbol] > 1) {
      int32_t* next = base + kBaseNodesChildArraySize;
      do {
        if (*base > 0) {
          if (cap_encoded_ != 0)
            score_base_node_tree_cap_fast(
                &nodes_[*base], node_data, symbol_entropy[symbol],
                production_cost, static_cast<float>(profit_ratio_power),
                log2_nspsc, new_symbol_cost, symbol_entropy,
                rank_scores_buffer, node_ptrs_num_ptr);
          else
            score_base_node_tree_fast(
                &nodes_[*base], node_data, symbol_entropy[symbol],
                production_cost, static_cast<float>(profit_ratio_power),
                log2_nspsc, new_symbol_cost, symbol_entropy,
                rank_scores_buffer, node_ptrs_num_ptr);
        }
        base++;
      } while (base != next);
    } else {
      base += 16;
    }
    symbol++;
  }
}

void Compressor::score_symbol_tree_words(
    NodeScoreData* rank_scores_buffer, ScoreData* node_data,
    uint16_t* node_ptrs_num_ptr, float production_cost, float log2_nspsc,
    float* new_symbol_cost, float* symbol_entropy) {
  int32_t* base = &base_nodes_child_node_num_[0];
  int32_t* base_end = &base_nodes_child_node_num_[0x90];
  do {
    if (*base > 0)
      score_base_node_tree_words(&nodes_[*base], node_data, production_cost,
                                 log2_nspsc, new_symbol_cost, symbol_entropy,
                                 rank_scores_buffer, node_ptrs_num_ptr);
  } while (++base <= base_end);
}

// --- overlap / substitution ---

void Compressor::overlap_check_invalidate_overlap(
    OverlapCheck* td, uint8_t* candidate_bad, uint32_t* num_overlaps,
    uint32_t prior_score, uint32_t node_score_number) {
  if (fast_mode_ == 0) {
    if (prior_score > node_score_number) candidate_bad[prior_score] = 1;
    else candidate_bad[node_score_number] = 1;
  } else {
    uint32_t low_score, high_score;
    if (node_score_number < prior_score) { low_score = node_score_number; high_score = prior_score; }
    else { low_score = prior_score; high_score = node_score_number; }
    int32_t* next_overlap_num_ptr = &td->next[low_score];
    while ((*next_overlap_num_ptr != -1) && (td->second[*next_overlap_num_ptr] < high_score))
      next_overlap_num_ptr = &td->next[*next_overlap_num_ptr];
    if ((*next_overlap_num_ptr == -1) || (td->second[*next_overlap_num_ptr] != high_score)) {
      if (*num_overlaps < 150000) {
        td->second[*num_overlaps] = high_score;
        td->next[*num_overlaps] = *next_overlap_num_ptr;
        *next_overlap_num_ptr = static_cast<int32_t>((*num_overlaps)++);
      } else {
        candidate_bad[high_score] = 1;
      }
    }
  }
}

void Compressor::overlap_check_handle_leaf_match(
    OverlapCheck* td, MatchNode* match_node_ptr, uint32_t* in_symbol_ptr,
    uint8_t* candidate_bad, uint32_t* num_overlaps,
    uint32_t* prior_match_score_number, uint32_t** prior_match_end_ptr,
    uint32_t* num_prior_matches) {
  const uint32_t node_score_number = match_node_ptr->score_number;
  if ((in_symbol_ptr - match_node_ptr->num_symbols < td->stop_matches_symbol_ptr) &&
      (candidate_bad[node_score_number] == 0) &&
      (*td->next_match_ptr_ptr + 2 <= td->match_stop_ptr)) {
    **td->next_match_ptr_ptr = node_score_number;
    (*td->next_match_ptr_ptr)++;
    **td->next_match_ptr_ptr = static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_ - match_node_ptr->num_symbols);
    (*td->next_match_ptr_ptr)++;
  }
  if ((*num_prior_matches != 0) &&
      (in_symbol_ptr - match_node_ptr->num_symbols <= prior_match_end_ptr[*num_prior_matches - 1])) {
    if (*num_prior_matches == 1) {
      if (prior_match_score_number[0] != node_score_number) {
        overlap_check_invalidate_overlap(td, candidate_bad, num_overlaps,
                                         prior_match_score_number[0], node_score_number);
        prior_match_end_ptr[1] = in_symbol_ptr - 1;
        prior_match_score_number[1] = node_score_number;
        *num_prior_matches = 2;
      }
    } else {
      uint32_t pm = 0;
      uint8_t found_same = 0;
      do {
        if (in_symbol_ptr - match_node_ptr->num_symbols > prior_match_end_ptr[pm]) {
          (*num_prior_matches)--;
          for (size_t i = pm; i < *num_prior_matches; i++) {
            prior_match_end_ptr[i] = prior_match_end_ptr[i + 1];
            prior_match_score_number[i] = prior_match_score_number[i + 1];
          }
        } else {
          if (prior_match_score_number[pm] == node_score_number) found_same = 1;
          else overlap_check_invalidate_overlap(td, candidate_bad, num_overlaps,
                                                prior_match_score_number[pm], node_score_number);
          pm++;
        }
      } while (pm < *num_prior_matches);
      if (found_same == 0) {
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

void Compressor::overlap_check_thread_impl(OverlapCheck& td) {
  MatchNode* match_nodes = td.match_nodes;
  MatchNode* match_node_ptr;
  uint32_t* in_symbol_ptr = td.start_symbol_ptr;
  uint32_t* local_end = td.stop_symbol_ptr;
  uint8_t* candidate_bad = td.candidate_bad;
  uint32_t num_overlaps = td.num_overlaps;
  uint32_t symbol;
  uint32_t prior_match_score_number[kMaxPriorMatches];
  uint32_t* prior_match_end_ptr[kMaxPriorMatches];
  uint32_t num_prior_matches = 0;
  for (symbol = 0; symbol < num_overlaps; symbol++) td.next[symbol] = -1;
thread_overlap_check_loop_no_match:
  symbol = *in_symbol_ptr++;
  if (in_symbol_ptr >= local_end) return;
  if ((static_cast<int32_t>(symbol) < 0) || (symbol >= child_ptr_array_size_) ||
      (child_ptr_array_[symbol] == nullptr))
    goto thread_overlap_check_loop_no_match;
  match_node_ptr = child_ptr_array_[symbol];
thread_overlap_check_loop_match:
  symbol = *in_symbol_ptr++;
  if (symbol != match_node_ptr->symbol) {
    uint32_t ss = symbol;
    do {
      if (match_node_ptr->sibling_node_num[ss & 0xF] != 0) {
        match_node_ptr = &match_nodes[match_node_ptr->sibling_node_num[ss & 0xF]];
        ss >>= 4;
      } else {
        if (match_node_ptr->miss_ptr == nullptr) {
          if ((static_cast<int32_t>(symbol) < 0) || (symbol >= child_ptr_array_size_) ||
              (child_ptr_array_[symbol] == nullptr))
            goto thread_overlap_check_loop_no_match;
          match_node_ptr = child_ptr_array_[symbol];
          goto thread_overlap_check_loop_match;
        } else { match_node_ptr = match_node_ptr->miss_ptr; ss = symbol; }
      }
    } while (symbol != match_node_ptr->symbol);
  }
  if (match_node_ptr->child_ptr != nullptr) {
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_loop_match;
  }
  overlap_check_handle_leaf_match(&td, match_node_ptr, in_symbol_ptr,
                                  candidate_bad, &num_overlaps,
                                  prior_match_score_number, prior_match_end_ptr,
                                  &num_prior_matches);
  match_node_ptr = match_node_ptr->hit_ptr;
  if (match_node_ptr == nullptr) {
    if (static_cast<int32_t>(symbol) < 0 || symbol >= child_ptr_array_size_ ||
        child_ptr_array_[symbol] == nullptr)
      goto thread_overlap_check_loop_no_match;
    match_node_ptr = child_ptr_array_[symbol];
    goto thread_overlap_check_loop_match;
  }
  match_node_ptr = match_node_ptr->child_ptr;
  goto thread_overlap_check_loop_match;
}

void Compressor::overlap_check_no_defs_thread_impl(OverlapCheck& td) {
  MatchNode* match_nodes = td.match_nodes;
  MatchNode* match_node_ptr;
  uint32_t* in_symbol_ptr = td.start_symbol_ptr;
  uint32_t* local_end = td.stop_symbol_ptr;
  uint8_t* candidate_bad = td.candidate_bad;
  uint32_t num_overlaps = td.num_overlaps;
  uint32_t symbol;
  uint32_t prior_match_score_number[kMaxPriorMatches];
  uint32_t* prior_match_end_ptr[kMaxPriorMatches];
  uint32_t num_prior_matches = 0;
  for (symbol = 0; symbol < num_overlaps; symbol++) td.next[symbol] = -1;
thread_overlap_check_no_defs_loop_no_match:
  symbol = *in_symbol_ptr++;
  if (in_symbol_ptr >= local_end) return;
  if (static_cast<int32_t>(symbol) < 0 || symbol >= child_ptr_array_size_ ||
      child_ptr_array_[symbol] == nullptr)
    goto thread_overlap_check_no_defs_loop_no_match;
  match_node_ptr = child_ptr_array_[symbol];
thread_overlap_check_no_defs_loop_match:
  symbol = *in_symbol_ptr++;
  if (symbol != match_node_ptr->symbol) {
    uint32_t ss = symbol;
    do {
      if (match_node_ptr->sibling_node_num[ss & 0xF] != 0) {
        match_node_ptr = &match_nodes[match_node_ptr->sibling_node_num[ss & 0xF]];
        ss >>= 4;
      } else if (match_node_ptr->miss_ptr == nullptr) {
        if (static_cast<int32_t>(symbol) < 0 || symbol >= child_ptr_array_size_ ||
            child_ptr_array_[symbol] == nullptr)
          goto thread_overlap_check_no_defs_loop_no_match;
        if (in_symbol_ptr > local_end) return;
        match_node_ptr = child_ptr_array_[symbol];
        goto thread_overlap_check_no_defs_loop_match;
      } else { match_node_ptr = match_node_ptr->miss_ptr; ss = symbol; }
    } while (symbol != match_node_ptr->symbol);
  }
  if (match_node_ptr->child_ptr != nullptr) {
    if (in_symbol_ptr > local_end) {
      if (in_symbol_ptr - match_node_ptr->num_symbols >= local_end) return;
    }
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_no_defs_loop_match;
  }
  overlap_check_handle_leaf_match(&td, match_node_ptr, in_symbol_ptr,
                                  candidate_bad, &num_overlaps,
                                  prior_match_score_number, prior_match_end_ptr,
                                  &num_prior_matches);
  match_node_ptr = match_node_ptr->hit_ptr;
  if (match_node_ptr == nullptr) {
    if (static_cast<int32_t>(symbol) < 0 || symbol >= child_ptr_array_size_ ||
        child_ptr_array_[symbol] == nullptr)
      goto thread_overlap_check_no_defs_loop_no_match;
    match_node_ptr = child_ptr_array_[symbol];
    goto thread_overlap_check_no_defs_loop_match;
  }
  if ((in_symbol_ptr <= local_end) ||
      (in_symbol_ptr - match_node_ptr->num_symbols < local_end)) {
    match_node_ptr = match_node_ptr->child_ptr;
    goto thread_overlap_check_no_defs_loop_match;
  }
}

void Compressor::find_substitutions_thread_impl(FindSubstitutionsThreadData& td) {
  MatchNode* match_nodes = td.match_nodes;
  MatchNode* match_node_ptr;
  uint32_t* in_symbol_ptr = td.start_symbol_ptr;
  uint32_t* previous_in_symbol_ptr = in_symbol_ptr;
  uint32_t* local_end = td.stop_symbol_ptr;
  uint32_t symbol;
  uint32_t substitute_index = 0;
  uint32_t local_read_index = 0;
  td.extra_match_symbols = 0;
thread_symbol_substitution_loop_top:
  symbol = *in_symbol_ptr++;
  if (symbol == 0x20) {
    match_node_ptr = child_ptr_array_[0];
    symbol = *in_symbol_ptr++;
    if (static_cast<int32_t>(symbol) < 0) {
      if (in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
      goto thread_symbol_substitution_loop_end;
    } else {
    thread_symbol_substitution_loop_match_search:
      if (symbol != match_node_ptr->symbol) {
        uint32_t sn = symbol;
        do {
          if (match_node_ptr->sibling_node_num[sn & 0xF] != 0) {
            match_node_ptr = &match_nodes[match_node_ptr->sibling_node_num[sn & 0xF]];
            sn >>= 4;
          } else {
            if (match_node_ptr->miss_ptr == nullptr) {
              if (symbol == 0x20) {
                if (in_symbol_ptr > local_end) goto thread_symbol_substitution_loop_end;
                if (static_cast<int32_t>(*in_symbol_ptr) >= 0) {
                  match_node_ptr = child_ptr_array_[0];
                  symbol = *in_symbol_ptr++;
                  goto thread_symbol_substitution_loop_match_search;
                }
                if (++in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
                goto thread_symbol_substitution_loop_end;
              }
              if (in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
              goto thread_symbol_substitution_loop_end;
            }
            if ((in_symbol_ptr > local_end) &&
                (in_symbol_ptr - match_node_ptr->miss_ptr->num_symbols >= local_end))
              goto thread_symbol_substitution_loop_end;
            match_node_ptr = match_node_ptr->miss_ptr;
            sn = symbol;
          }
        } while (symbol != match_node_ptr->symbol);
      }
      if (match_node_ptr->child_ptr != nullptr) {
        symbol = *in_symbol_ptr++;
        if (static_cast<int32_t>(symbol) >= 0) {
          match_node_ptr = match_node_ptr->child_ptr;
          goto thread_symbol_substitution_loop_match_search;
        } else {
          goto thread_symbol_substitution_loop_match_no_match;
        }
      }
      while ((((substitute_index - local_read_index) & 0x7FFFFC) == 0x7FFFFC) &&
             (((substitute_index - (local_read_index = td.read_index.load(std::memory_order_acquire))) &
               0x7FFFFC) == 0x7FFFFC))
        std::this_thread::yield();
      if (static_cast<uint32_t>(in_symbol_ptr - previous_in_symbol_ptr - match_node_ptr->num_symbols) != 0) {
        td.data[substitute_index] =
            static_cast<uint32_t>(in_symbol_ptr - previous_in_symbol_ptr - match_node_ptr->num_symbols);
        substitute_index = (substitute_index + 1) & 0x7FFFFF;
      }
      td.data[substitute_index] = 0x80000000 + match_node_ptr->num_symbols;
      substitute_index = (substitute_index + 1) & 0x7FFFFF;
      td.data[substitute_index] = match_node_ptr->score_number;
      substitute_index = (substitute_index + 1) & 0x7FFFFF;
      td.write_index.store(substitute_index, std::memory_order_release);
      previous_in_symbol_ptr = in_symbol_ptr;
      if (in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
      td.extra_match_symbols = static_cast<uint32_t>(in_symbol_ptr - local_end);
      goto thread_symbol_substitution_loop_end2;
    }
  thread_symbol_substitution_loop_match_no_match:
    if (in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
    goto thread_symbol_substitution_loop_end;
  }
  if (in_symbol_ptr < local_end) goto thread_symbol_substitution_loop_top;
thread_symbol_substitution_loop_end:
  while ((((substitute_index - local_read_index) & 0x7FFFFF) == 0x7FFFFF) &&
         (((substitute_index - (local_read_index = td.read_index.load(std::memory_order_acquire))) &
           0x7FFFFF) == 0x7FFFFF))
    std::this_thread::yield();
  td.data[substitute_index] = static_cast<uint32_t>(local_end - previous_in_symbol_ptr);
  substitute_index = (substitute_index + 1) & 0x7FFFFF;
  td.write_index.store(substitute_index, std::memory_order_release);
thread_symbol_substitution_loop_end2:
  td.done.store(1, std::memory_order_relaxed);
}

void Compressor::substitute_thread_impl(SubstituteThreadData& td) {
  uint32_t data;
  uint32_t local_write_index;
  uint32_t substitute_data_index = 0;
  td.out_symbol_ptr = td.in_symbol_ptr;
  while (true) {
    while ((local_write_index = substitute_data_write_index_.load(std::memory_order_relaxed)) ==
           substitute_data_index)
      ;
    do {
      if (static_cast<int32_t>(data = td.substitute_data[substitute_data_index++]) >= 0) {
        std::memmove(td.out_symbol_ptr, td.in_symbol_ptr, data * 4);
        td.in_symbol_ptr += data;
        td.out_symbol_ptr += data;
      } else if (data != 0xFFFFFFFF) {
        td.in_symbol_ptr += static_cast<size_t>(data + 0x80000000);
        const uint32_t symbol = td.substitute_data[substitute_data_index++];
        if (symbol > td.max_rule_symbol) {
          fprintf(stderr,
                  "GLZA compress: substitute_thread symbol %u > max_rule_symbol %u\n",
                  static_cast<unsigned>(symbol), static_cast<unsigned>(td.max_rule_symbol));
          throw CompressError("substitute_thread grammar corruption");
        }
        *td.out_symbol_ptr++ = symbol;
        td.symbol_counts[symbol]++;
      } else {
        return;
      }
      substitute_data_read_index_.store(substitute_data_index, std::memory_order_relaxed);
    } while (local_write_index != substitute_data_index);
  }
}

// --- utility functions ---

uint32_t Compressor::stca_setup(
    size_t thread_count, const uint8_t thread_symbol_limit[],
    const uint32_t thread_first_node_num[], const uint32_t thread_nodes_limit[],
    uint32_t num_rules, uint32_t next_new_symbol_number,
    TreeThreadData tree_thread_data[13], uint32_t* start_cycle_symbol_ptr) {
  const uint32_t symbols_div_100 = (num_file_symbols_ - num_rules) / 100;
  uint32_t sum_symbols = symbol_counts_[0];
  size_t i = 1;
  uint32_t main_max_symbol;
  for (size_t j = 0; j < thread_count; ++j) {
    const uint32_t symbols_limit = symbols_div_100 * thread_symbol_limit[j];
    while (sum_symbols < symbols_limit && i < next_new_symbol_number)
      sum_symbols += symbol_counts_[i++];
    if (j > 0) tree_thread_data[j - 1].max_symbol = static_cast<uint32_t>(i - 1);
    else main_max_symbol = static_cast<uint32_t>(i - 1);
    tree_thread_data[j].min_symbol = static_cast<uint32_t>(i);
    if (i < next_new_symbol_number - 1 && j < thread_count - 1)
      sum_symbols += symbol_counts_[i++];
  }
  tree_thread_data[thread_count - 1].max_symbol = next_new_symbol_number - 1;
  for (size_t j = 0; j < thread_count; j++) {
    tree_thread_data[j].start_cycle_symbol_ptr = start_cycle_symbol_ptr;
    tree_thread_data[j].base_nodes_child_node_num = base_nodes_child_node_num_;
    tree_thread_data[j].first_node_num = thread_first_node_num[j];
    tree_thread_data[j].nodes_limit = thread_nodes_limit[j];
  }
  return main_max_symbol;
}

float Compressor::update_cycle_start_ratio(
    float cycle_start_ratio, float cycle_end_ratio, uint8_t fast_section,
    uint8_t fast_sections, uint32_t prior_cycle_symbols) {
  if (fast_mode_ == 1) return static_cast<float>(fast_section) / static_cast<float>(fast_sections);
  if (cycle_start_ratio == 0.0f) {
    if (cycle_end_ratio < 1.0f) {
      return cycle_end_ratio > 0.5f ? 1.0f - (0.99f * cycle_end_ratio) : cycle_end_ratio;
    }
    return cycle_start_ratio;
  }
  if ((cycle_end_ratio >= 0.99f) || (prior_cycle_symbols >= num_file_symbols_) ||
      (1.5f * (1.0f - cycle_end_ratio) <= cycle_end_ratio - cycle_start_ratio))
    return 0.0f;
  if (static_cast<uint32_t>((1.0f - cycle_end_ratio) * static_cast<float>(num_file_symbols_)) >=
      prior_cycle_symbols)
    return cycle_end_ratio;
  return 1.0f - (0.97f * (cycle_end_ratio - cycle_start_ratio));
}

void Compressor::main_loop_init(
    uint32_t* out_next_new_sym, uint32_t num_rules, double* out_d_nfs,
    uint8_t** out_free_ram, double** out_symbol_entropy,
    float** out_symbol_entropy_f, ScanMode scan_mode, float* ptr_log2_nspsc,
    float new_symbol_cost[kNumPrecalcSymbolCosts], float* ptr_production_cost,
    uint32_t num_terminals_used, uint16_t* ptr_scan_cycle,
    const uint8_t* end_ram, uint32_t* out_node_num_limit) {
  const uint32_t next_new_symbol_number = num_terminals_ + num_rules;
  child_ptr_array_size_ = next_new_symbol_number;
  const double d_num_file_symbols = static_cast<double>(num_file_symbols_);
  log_file_symbols_ = log2(d_num_file_symbols);
  auto* free_RAM_ptr = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<size_t>(end_symbol_ptr_) + 8) & ~static_cast<size_t>(7));
  auto* symbol_entropy = reinterpret_cast<double*>(free_RAM_ptr);
  auto* symbol_entropy_f = reinterpret_cast<float*>(free_RAM_ptr);

  free_RAM_ptr += (1 + ((scan_mode != ScanMode::kWords) & (fast_mode_ == 0))) *
                  sizeof(float) * static_cast<size_t>(next_new_symbol_number);
  if ((scan_mode != ScanMode::kWords) && (fast_mode_ == 0)) {
    nfs_p1_x_log_p1_ = xlogx(num_file_symbols_ + 1);
    for (size_t i = 1; i < kNumPrecalcNfsmrLogs; i++) {
      const double tmp = num_file_symbols_ - i + 1;
      nfs_profit_[i] = nfs_p1_x_log_p1_ - xlogx(tmp);
    }
    new_rule_cost_ = nfs_p1_x_log_p1_ - xlogx(d_num_file_symbols) + 1.0 +
                     (num_rules == 0 ? 0 : xlogx(num_rules) - xlogx(num_rules + 1));
  } else {
    const float log2_num_symbols_plus_substitution_cost =
        static_cast<float>(log_file_symbols_) + 1.4f;
    for (size_t i = 2; i < kNumPrecalcSymbolCosts; i++)
      new_symbol_cost[i] = log2_num_symbols_plus_substitution_cost -
                           static_cast<float>(log2_x_[i - 1]);
    *ptr_production_cost =
        scan_mode == ScanMode::kWords
            ? log2f(static_cast<float>(d_num_file_symbols) / static_cast<float>(num_terminals_used)) + 1.2f
            : log2f(static_cast<float>(d_num_file_symbols) / static_cast<float>(num_rules + 1)) + 1.2f;
    *ptr_log2_nspsc = log2_num_symbols_plus_substitution_cost;
  }

  uint16_t scan_cycle = *ptr_scan_cycle;
  if (fast_mode_ == 0) {
    double order_0_entropy = 0.0;
    for (size_t i = 0; i < next_new_symbol_number; i++) {
      if (symbol_counts_[i] != 0) {
        symbol_entropy[i] = log_file_symbols_ -
                            (symbol_counts_[i] < kNumPrecalcLog2X
                                 ? log2_x_[symbol_counts_[i]]
                                 : log2(static_cast<double>(symbol_counts_[i])));
        order_0_entropy += symbol_entropy[i] * static_cast<double>(symbol_counts_[i]);
      }
    }
    if (scan_mode == ScanMode::kWords) {
      for (size_t i = 0; i < next_new_symbol_number; i++) {
        if (symbol_counts_[i] != 0)
          symbol_entropy_f[i] = static_cast<float>(symbol_entropy[i]);
      }
    }
    if (num_rules != 0)
      order_0_entropy += static_cast<double>(num_rules + 1) *
                         (log_file_symbols_ - log2(static_cast<double>(num_rules)));
#ifdef PRINTON
    fprintf(stderr, "PASS %u: grammar size: %u, %u production rules, %.4f bits/sym, o0e %u bytes\n",
            static_cast<unsigned>(++scan_cycle), static_cast<unsigned>(num_file_symbols_ + 1),
            static_cast<unsigned>(num_rules),
            static_cast<float>(order_0_entropy / d_num_file_symbols),
            static_cast<unsigned>(order_0_entropy * 0.125));
#else
    ++scan_cycle;
#endif
  } else {
#ifdef PRINTON
    fprintf(stderr, "PASS %u: grammar size: %u, %u production rules\r",
            static_cast<unsigned>(++scan_cycle), static_cast<unsigned>(num_file_symbols_ + 1),
            static_cast<unsigned>(num_rules + 1));
#else
    ++scan_cycle;
#endif
  }

  base_nodes_child_node_num_ = reinterpret_cast<int32_t*>(free_RAM_ptr);
  nodes_ = reinterpret_cast<SuffixNode*>(
      reinterpret_cast<size_t>(free_RAM_ptr) +
      (sizeof(int32_t) * static_cast<size_t>(next_new_symbol_number) * kBaseNodesChildArraySize));
  {
    const size_t nodes_room = static_cast<size_t>(end_ram - reinterpret_cast<const uint8_t*>(nodes_));
    if (nodes_ >= reinterpret_cast<SuffixNode*>(const_cast<uint8_t*>(end_ram)) ||
        nodes_room < sizeof(SuffixNode)) {
      throw CompressError("Insufficient RAM for suffix tree nodes");
    }
    nodes_num_limit_ = static_cast<uint32_t>(nodes_room / sizeof(SuffixNode));
  }

  *ptr_scan_cycle = scan_cycle;
  *out_node_num_limit = nodes_num_limit_;
  *out_next_new_sym = next_new_symbol_number;
  *out_d_nfs = d_num_file_symbols;
  *out_free_ram = free_RAM_ptr;
  *out_symbol_entropy = symbol_entropy;
  *out_symbol_entropy_f = symbol_entropy_f;
}

void Compressor::build_and_score_suffix_tree(
    uint32_t** out_in_symbol_ptr, uint32_t* out_next_node_num,
    uint16_t* out_node_ptrs_num, float* out_cycle_end_ratio,
    uint32_t* start_cycle_symbol_ptr, uint32_t node_num_limit,
    uint32_t next_new_sym, uint32_t num_rules, uint32_t max_scores,
    float cycle_start_ratio, uint8_t fast_section, uint8_t fast_sections,
    double* symbol_entropy, float* symbol_entropy_f, double profit_ratio_power,
    float production_cost, float log2_nspsc,
    float new_symbol_cost[kNumPrecalcSymbolCosts],
    RankScoresThreadData* rsd, ScoreData* node_data,
    TreeThreadData tree_thread_data[13], std::jthread build_tree_threads[7],
    std::jthread* rank_scores_thread1) {
  uint32_t main_max_symbol, main_nodes_limit;
  uint32_t* end_cycle_symbol_ptr;
  uint32_t* in_symbol_ptr = *out_in_symbol_ptr;
  uint32_t next_node_num = 1;
  const uint32_t nodes_div_100 = node_num_limit / 100;

  if (fast_mode_ == 0) {
    uint8_t tsl[] = {5,11,17,24,32,42,52,61,69,77,86,93};
    uint32_t tfnn[] = {18*nodes_div_100,31*nodes_div_100,43*nodes_div_100,56*nodes_div_100,
                       70*nodes_div_100,85*nodes_div_100,1,16*nodes_div_100,31*nodes_div_100,
                       43*nodes_div_100,56*nodes_div_100,70*nodes_div_100};
    uint32_t tnl[] = {31*nodes_div_100,43*nodes_div_100,56*nodes_div_100,70*nodes_div_100,
                      85*nodes_div_100,node_num_limit,16*nodes_div_100,31*nodes_div_100,
                      43*nodes_div_100,56*nodes_div_100,70*nodes_div_100,85*nodes_div_100};
    main_nodes_limit = (nodes_div_100 * 18) - 10;
    main_max_symbol = stca_setup(12, tsl, tfnn, tnl, num_rules, next_new_sym,
                                 tree_thread_data, start_cycle_symbol_ptr);
    scan_symbol_ptr_.store(reinterpret_cast<uintptr_t>(in_symbol_ptr), std::memory_order_relaxed);
    max_symbol_ptr_.store(0, std::memory_order_relaxed);
    for (size_t j = 0; j < 6; j++)
      build_tree_threads[j] = std::jthread([this, &td = tree_thread_data[j]] { build_tree_thread_impl(td); });
  } else {
    uint8_t tsl[] = {6,12,19,26,34,43,54,67,73,79,85,90,95};
    uint32_t tfnn[] = {6*nodes_div_100,12*nodes_div_100,22*nodes_div_100,34*nodes_div_100,
                       48*nodes_div_100,64*nodes_div_100,81*nodes_div_100,1,12*nodes_div_100,
                       22*nodes_div_100,34*nodes_div_100,48*nodes_div_100,64*nodes_div_100};
    uint32_t tnl[] = {12*nodes_div_100,22*nodes_div_100,34*nodes_div_100,48*nodes_div_100,
                      64*nodes_div_100,81*nodes_div_100,node_num_limit,12*nodes_div_100,
                      22*nodes_div_100,34*nodes_div_100,48*nodes_div_100,64*nodes_div_100,
                      81*nodes_div_100};
    main_nodes_limit = (nodes_div_100 * 6) - 10;
    main_max_symbol = stca_setup(13, tsl, tfnn, tnl, num_rules, next_new_sym,
                                 tree_thread_data, start_cycle_symbol_ptr);
    end_cycle_symbol_ptr =
        fast_section == fast_sections - 1
            ? end_symbol_ptr_
            : start_symbol_ptr_ + static_cast<uint32_t>(
                  static_cast<float>(num_file_symbols_) *
                  static_cast<float>(fast_section + 1) / static_cast<float>(fast_sections));
    scan_symbol_ptr_.store(reinterpret_cast<uintptr_t>(end_cycle_symbol_ptr), std::memory_order_relaxed);
    max_symbol_ptr_.store(reinterpret_cast<uintptr_t>(end_cycle_symbol_ptr), std::memory_order_release);
    for (size_t j = 0; j < 7; j++)
      build_tree_threads[j] = std::jthread([this, &td = tree_thread_data[j]] { build_tree_thread_impl(td); });
  }
  std::memset(base_nodes_child_node_num_, 0,
              4 * (main_max_symbol + 1) * kBaseNodesChildArraySize);

  uint16_t node_ptrs_num;
  if (fast_mode_ == 0) {
    uint32_t symbol;
    do {
      symbol = *in_symbol_ptr++;
      if (symbol <= main_max_symbol) {
        scan_symbol_ptr_.store(reinterpret_cast<uintptr_t>(in_symbol_ptr), std::memory_order_relaxed);
        if (static_cast<int32_t>(*in_symbol_ptr) >= 0) {
          add_suffix(symbol, in_symbol_ptr, &next_node_num);
          if (next_node_num >= main_nodes_limit) {
            warn_main_nodes_limit(main_nodes_limit, next_node_num);
            goto done_building_tree;
          }
        }
      }
    } while (symbol != 0xFFFFFFFE);
    in_symbol_ptr--;
  done_building_tree:
    scan_symbol_ptr_.store(reinterpret_cast<uintptr_t>(in_symbol_ptr), std::memory_order_relaxed);
    max_symbol_ptr_.store(reinterpret_cast<uintptr_t>(in_symbol_ptr), std::memory_order_release);
    node_ptrs_num = 0;
    rank_scores_write_index_.store(0, std::memory_order_relaxed);
    rank_scores_read_index_.store(0, std::memory_order_relaxed);
    rsd->max_scores = static_cast<uint16_t>(max_scores);
    *rank_scores_thread1 = std::jthread([this, rsd] { rank_scores_thread_impl(*rsd); });
    score_symbol_tree(0, main_max_symbol, rsd->rank_scores_buffer, node_data,
                      &node_ptrs_num, profit_ratio_power, symbol_entropy, symbol_counts_.data());
    for (size_t i = 0; i < 12; i++) {
      if (i < 6) {
        build_tree_threads[i].join();
        build_tree_threads[i] = std::jthread(
            [this, &td = tree_thread_data[i + 6]] { build_tree_thread_impl(td); });
      } else {
        build_tree_threads[i - 6].join();
      }
      score_symbol_tree(tree_thread_data[i].min_symbol, tree_thread_data[i].max_symbol,
                        rsd->rank_scores_buffer, node_data, &node_ptrs_num,
                        profit_ratio_power, symbol_entropy, symbol_counts_.data());
    }
    if ((node_ptrs_num & 0xFFF) == 0) {
      while (static_cast<uint16_t>(node_ptrs_num - rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
        ;
    }
    rsd->rank_scores_buffer[node_ptrs_num].last_match_index = 0;
    rank_scores_write_index_.store(node_ptrs_num + 1, std::memory_order_release);
    rank_scores_thread1->join();
    *out_cycle_end_ratio =
        static_cast<float>(in_symbol_ptr - start_symbol_ptr_) / static_cast<float>(num_file_symbols_);
  } else {
    uint32_t symbol;
    do {
      symbol = *in_symbol_ptr++;
      if (symbol <= main_max_symbol && static_cast<int32_t>(*in_symbol_ptr) >= 0) {
        add_suffix(symbol, in_symbol_ptr, &next_node_num);
        if (next_node_num >= main_nodes_limit) {
          warn_main_nodes_limit(main_nodes_limit, next_node_num);
          break;
        }
      }
    } while (in_symbol_ptr != end_cycle_symbol_ptr);
    node_ptrs_num = 0;
    rank_scores_write_index_.store(0, std::memory_order_relaxed);
    rank_scores_read_index_.store(0, std::memory_order_relaxed);
    {
      size_t i = 0;
      do {
        if (symbol_counts_[i] != 0) {
          symbol_entropy_f[i] =
              symbol_counts_[i] < kNumPrecalcLog2X
                  ? static_cast<float>(log_file_symbols_ - log2_x_[symbol_counts_[i]])
                  : static_cast<float>(log_file_symbols_) - log2f(static_cast<float>(symbol_counts_[i]));
        }
      } while (++i < next_new_sym);
    }
    rsd->max_scores = static_cast<uint16_t>(max_scores);
    rsd->num_file_symbols = num_file_symbols_;
    build_tree_threads[0].join();
    *rank_scores_thread1 = std::jthread([this, rsd] { rank_scores_thread_fast_impl(*rsd); });
    score_symbol_tree_fast(0, tree_thread_data[0].max_symbol, rsd->rank_scores_buffer,
                           node_data, &node_ptrs_num, production_cost, profit_ratio_power,
                           log2_nspsc, new_symbol_cost, symbol_entropy_f, symbol_counts_.data());
    for (size_t i = 1; i <= 12; i++) {
      if (i <= 6) {
        build_tree_threads[i].join();
        build_tree_threads[i - 1] = std::jthread(
            [this, &td = tree_thread_data[i + 6]] { build_tree_thread_impl(td); });
      } else {
        build_tree_threads[i - 7].join();
      }
      score_symbol_tree_fast(tree_thread_data[i].min_symbol, tree_thread_data[i].max_symbol,
                             rsd->rank_scores_buffer, node_data, &node_ptrs_num,
                             production_cost, profit_ratio_power, log2_nspsc,
                             new_symbol_cost, symbol_entropy_f, symbol_counts_.data());
    }
    if ((node_ptrs_num & 0xFFF) == 0) {
      while (static_cast<uint16_t>(node_ptrs_num - rank_scores_read_index_.load(std::memory_order_acquire)) >= 0xF000)
        ;
    }
    rsd->rank_scores_buffer[node_ptrs_num].last_match_index = 0;
    rank_scores_write_index_.store(node_ptrs_num + 1, std::memory_order_release);
    rank_scores_thread1->join();
  }

  *out_in_symbol_ptr = in_symbol_ptr;
  *out_next_node_num = next_node_num;
  *out_node_ptrs_num = node_ptrs_num;
}

void Compressor::handle_zero_candidates(
    ScanMode* scan_mode, uint16_t* num_candidates, float* prior_min_score,
    uint8_t* fast_section, uint8_t* fast_sections, float* fast_min_score) {
  if (*scan_mode == ScanMode::kRetry) {
    if (min_score_ > 0.0f) {
      *num_candidates = 1;
      *prior_min_score = min_score_;
      min_score_ = 0.0f;
    } else if (*fast_sections != 1) {
      if (*fast_sections == 23) {
        *fast_sections = 9; *fast_section = 0; min_score_ = 8.0f;
      } else {
        *fast_sections = (*fast_sections + 1) >> 1;
        *fast_section >>= 1;
        min_score_ = 4.0f;
      }
      *prior_min_score = kBigFloat;
      *fast_min_score = 1.0f;
      *num_candidates = 1;
      *scan_mode = ScanMode::kGeneral;
    }
  } else {
    *scan_mode = ScanMode::kRetry;
    *num_candidates = 1;
    *prior_min_score = min_score_;
    min_score_ = 0.25f * min_score_;
  }
}

uint8_t Compressor::update_min_max_scores(
    uint32_t* ptr_max_scores, float* ptr_min_score, float prior_min_score,
    uint16_t num_candidates, uint32_t num_rules, uint32_t next_new_sym,
    uint32_t initial_max_scores, float fast_min_score, uint8_t fast_sections) {
  uint32_t max_scores = *ptr_max_scores;
  float local_min_score = *ptr_min_score;
  if (fast_mode_ == 0) {
    const uint32_t prior_max_scores = max_scores;
    max_scores = (max_scores + 2 * (num_terminals_ + num_rules - next_new_sym + initial_max_scores)) / 3;
    if (max_scores > kMaxScores) max_scores = kMaxScores;
    if (max_scores > prior_max_scores)
      local_min_score -= (prior_min_score - local_min_score) * 25.0f *
                         static_cast<float>(max_scores - prior_max_scores) /
                         static_cast<float>(prior_max_scores);
    if (local_min_score < 0.0f) local_min_score = 0.0f;
  } else {
    if ((prior_min_score <= fast_min_score) && (fast_sections == 1)) return 1;
    max_scores = (20 * max_scores +
                  35 * (num_terminals_ + num_rules - next_new_sym + initial_max_scores)) >> 6;
    if (max_scores > kMaxScoresFast) max_scores = kMaxScoresFast;
  }
  if (max_scores > 100u * num_candidates) max_scores = 100u * num_candidates;
  *ptr_max_scores = max_scores;
  *ptr_min_score = local_min_score;
  return 0;
}

void Compressor::process_ranked_candidates(
    uint16_t* p_num_candidates, ScanMode* p_scan_mode,
    uint32_t next_new_symbol_number, uint32_t max_rules, uint32_t max_scores,
    uint32_t* p_num_rules, uint32_t* p_first_define_index,
    uint32_t** p_in_symbol_ptr, const uint16_t* candidates_index_in,
    uint8_t* candidate_bad, uint8_t* end_RAM_ptr,
    RankScoresThreadData* rsd, OverlapCheck** p_overlap_check_data,
    OverlapCheck** p_overlap_check_heap_buf, uint32_t* p_num_match_nodes,
    uint32_t* p_max_match_length, uint32_t** p_match_strings,
    uint8_t* p_fast_section, uint8_t* p_fast_sections, uint8_t* p_section_repeats,
    float section_scores[23], float* p_prior_min_score, float* p_fast_min_score,
    float* p_new_min_score, uint16_t scan_cycle) {
  uint16_t num_candidates = *p_num_candidates;
  ScanMode scan_mode = *p_scan_mode;
  uint32_t num_rules = *p_num_rules;
  uint32_t first_define_index = *p_first_define_index;
  uint32_t* in_symbol_ptr = *p_in_symbol_ptr;
  OverlapCheck* overlap_check_data = *p_overlap_check_data;
  OverlapCheck* overlap_check_heap_buf = *p_overlap_check_heap_buf;
  uint32_t num_match_nodes = *p_num_match_nodes;
  uint32_t max_match_length = *p_max_match_length;
  uint32_t* match_strings = *p_match_strings;
  uint8_t fast_section = *p_fast_section;
  uint8_t fast_sections = *p_fast_sections;
  uint8_t section_repeats = *p_section_repeats;
  float prior_min_score = *p_prior_min_score;
  float fast_min_score = *p_fast_min_score;
  float new_min_score = *p_new_min_score;

  std::jthread overlap_check_threads[7];
  uint32_t new_rule_number[0x8000];

  NodeScoreData* candidates = rsd->candidates;
  // C original used grammar-arena heap; keep off the stack (kMaxScoresFast is ~32k).
  std::vector<NodeScoreData> tmp_candidates(kMaxScoresFast);
  std::memcpy(tmp_candidates.data(), candidates, kMaxScoresFast * sizeof(NodeScoreData));
  for (uint16_t cn = 0; cn < kMaxScoresFast; cn++)
    candidates[cn] = tmp_candidates[candidates_index_in[cn]];

  if (fast_mode_ == 0) {
#ifdef PRINTON
    fprintf(stderr, " score[0-%hu] = %.5f-%.5f\n",
            static_cast<uint16_t>(num_candidates - 1),
            static_cast<double>(candidates[0].score),
            static_cast<double>(candidates[num_candidates - 1].score));
#endif
    if (candidates[num_candidates - 1].score < (0.1f * candidates[0].score) - 1.0f) {
      size_t cn = 1;
      while ((cn + 0x100 < num_candidates) &&
             (candidates[cn + 0x100].score >= (0.1f * candidates[0].score) - 1.0f))
        cn += 0x100;
      while (cn < num_candidates) {
        if (candidates[cn].score < (0.1f * candidates[0].score) - 1.0f) {
          num_candidates = static_cast<uint16_t>(cn);
          break;
        }
        cn++;
      }
    }
  } else if (fast_sections != 1) {
    section_scores[fast_section] = candidates[num_candidates - 1].score;
    const uint8_t old_fast_section = fast_section;
    if (++fast_section == fast_sections) fast_section = 0;
    if (candidates[num_candidates - 1].score < fast_min_score) {
      if (fast_sections == 23) { fast_sections = 9; fast_section = 0; min_score_ = 8.0f; }
      else { fast_sections = (fast_sections + 1) >> 1; fast_section >>= 1; min_score_ = 4.0f; }
      section_repeats = 0;
      for (size_t i = 0; i < fast_sections; i++) section_scores[i] = kBigFloat;
      prior_min_score = kBigFloat;
      fast_min_score = 1.0f;
      scan_mode = ScanMode::kGeneral;
    } else {
      size_t i = fast_section + 1 == fast_sections ? 0 : fast_section + 1;
      while (i != old_fast_section) {
        if (section_scores[i] > section_scores[fast_section]) fast_section = static_cast<uint8_t>(i);
        if (++i == fast_sections) i = 0;
      }
      if ((section_repeats < 2) && (section_scores[old_fast_section] > section_scores[fast_section])) {
        fast_section = old_fast_section;
        section_repeats++;
      } else {
        section_repeats = 0;
      }
    }
  }

  if (next_new_symbol_number + num_candidates > max_rules) {
    num_candidates = max_rules > next_new_symbol_number
                         ? static_cast<uint16_t>(max_rules - next_new_symbol_number) : 0;
  }

  uint8_t* free_RAM_ptr = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<size_t>(end_symbol_ptr_) + 8) & ~static_cast<size_t>(7));
  const uintptr_t match_region_end_limit =
      (nodes_ != nullptr && reinterpret_cast<uintptr_t>(nodes_) < reinterpret_cast<uintptr_t>(end_RAM_ptr))
          ? reinterpret_cast<uintptr_t>(nodes_) : reinterpret_cast<uintptr_t>(end_RAM_ptr);

  child_ptr_array_.assign(next_new_symbol_number, nullptr);
  child_ptr_array_size_ = next_new_symbol_number;

  MatchNode* match_nodes;
  uint32_t match_nodes_limit;
  bind_match_storage(&match_nodes, &match_nodes_limit, &match_strings,
                     free_RAM_ptr, match_region_end_limit,
                     next_new_symbol_number, num_candidates,
                     0, 0);
  num_match_nodes = 0;
  max_match_length = 0;

  {
    uint16_t cn = 0;
    while (cn < num_candidates) {
      if (candidates[cn].num_symbols > max_match_length)
        max_match_length = candidates[cn].num_symbols;
      uint32_t* best_score_last_match_ptr = start_symbol_ptr_ + candidates[cn].last_match_index;
      uint32_t* best_score_match_ptr = best_score_last_match_ptr - candidates[cn].num_symbols + 1;
      MatchNode* match_node_ptr;
      if (num_match_nodes == 0) {
        init_match_node(match_nodes, *best_score_match_ptr, 0);
        match_nodes[0].score_number = cn;
        num_match_nodes = 1;
      }
      match_node_ptr = match_nodes;
      while (best_score_match_ptr <= best_score_last_match_ptr) {
        const uint32_t symbol = *best_score_match_ptr;
        if (match_node_ptr->child_ptr == nullptr) {
          if (num_match_nodes >= match_nodes_limit) {
            warn_match_nodes_limit();
            candidate_bad[cn] = 1;
            break;
          }
          match_node_ptr->child_ptr = &match_nodes[num_match_nodes];
          init_match_node(&match_nodes[num_match_nodes], symbol, 0);
          match_nodes[num_match_nodes].score_number = cn;
          match_node_ptr = &match_nodes[num_match_nodes++];
        } else {
          match_node_ptr = match_node_ptr->child_ptr;
          uint32_t ss = symbol;
          while (symbol != match_node_ptr->symbol) {
            if (match_node_ptr->sibling_node_num[ss & 0xF] != 0) {
              match_node_ptr = &match_nodes[match_node_ptr->sibling_node_num[ss & 0xF]];
              ss >>= 4;
            } else {
              if (num_match_nodes >= match_nodes_limit) {
                warn_match_nodes_limit();
                candidate_bad[cn] = 1;
                goto next_candidate_build;
              }
              match_node_ptr->sibling_node_num[ss & 0xF] = num_match_nodes;
              init_match_node(&match_nodes[num_match_nodes], symbol, 0);
              match_nodes[num_match_nodes].score_number = cn;
              match_node_ptr = &match_nodes[num_match_nodes++];
              break;
            }
          }
        }
        best_score_match_ptr++;
      }
      if (match_node_ptr->child_ptr != nullptr) candidate_bad[cn] = 1;
    next_candidate_build:
      cn++;
    }
  }

  // Save match strings
  {
    uint16_t cn = 0;
    while (cn < num_candidates) {
      if (candidate_bad[cn] == 0) {
        uint32_t* ms_ptr = &match_strings[cn * max_match_length];
        uint32_t* src = start_symbol_ptr_ + candidates[cn].last_match_index -
                        candidates[cn].num_symbols + 1;
        for (size_t j = 0; j < candidates[cn].num_symbols; j++)
          ms_ptr[j] = src[j];
      }
      cn++;
    }
  }

  // Overlap checking
  overlap_check_data = reinterpret_cast<OverlapCheck*>(
      (reinterpret_cast<uintptr_t>(&match_strings[num_candidates * max_match_length]) + 7) & ~static_cast<uintptr_t>(7));
  if (reinterpret_cast<uintptr_t>(overlap_check_data) + (8 * sizeof(OverlapCheck)) > match_region_end_limit) {
    if (overlap_check_heap_buf == nullptr) {
      overlap_check_heap_buf = static_cast<OverlapCheck*>(std::malloc(8 * sizeof(OverlapCheck)));
      if (overlap_check_heap_buf == nullptr)
        throw CompressError("overlap_check memory allocation failed");
    }
    overlap_check_data = overlap_check_heap_buf;
  }
  for (size_t i = 1; i < 8; i++)
    overlap_check_data[i].candidate_bad = &candidate_bad[0];

  uint32_t* next_match_ptrs[8];
  uint32_t* matches_start_ptr[8];
  uint32_t* matches_stop_ptr[8];
  {
    uint32_t* begin_matches = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(overlap_check_data) + (8 * sizeof(OverlapCheck)));
    uint32_t* matches_end_ptr = reinterpret_cast<uint32_t*>(end_RAM_ptr);
    if (begin_matches >= matches_end_ptr) begin_matches = matches_end_ptr;
    const uint32_t matches_stride = static_cast<uint32_t>((matches_end_ptr - begin_matches) >> 3);
    for (size_t i = 0; i < 8; i++) {
      next_match_ptrs[i] = matches_start_ptr[i] = begin_matches + (i * matches_stride);
      matches_stop_ptr[i] = begin_matches + ((i + 1) * matches_stride);
    }
  }

  const size_t block_size = num_file_symbols_ >> 3;
  uint32_t* block_ptr = start_symbol_ptr_ + block_size;
  uint32_t* stop_matches_symbol_ptrs[8];
  stop_matches_symbol_ptrs[0] = block_ptr;
  uint32_t* stop_symbol_ptr = block_ptr + kMaxMatchLength;
  if (stop_symbol_ptr >= end_symbol_ptr_) {
    stop_symbol_ptr = end_symbol_ptr_;
    stop_matches_symbol_ptrs[0] = end_symbol_ptr_;
  } else {
    for (size_t i = 1; i < 8; i++) {
      overlap_check_data[i].start_symbol_ptr = block_ptr;
      block_ptr += block_size;
      if (i < 7) {
        overlap_check_data[i].stop_matches_symbol_ptr = block_ptr;
        overlap_check_data[i].stop_symbol_ptr = block_ptr + kMaxMatchLength;
        if (overlap_check_data[i].stop_symbol_ptr > end_symbol_ptr_)
          overlap_check_data[i].stop_symbol_ptr = end_symbol_ptr_;
      } else {
        overlap_check_data[7].stop_matches_symbol_ptr = end_symbol_ptr_;
        overlap_check_data[7].stop_symbol_ptr = end_symbol_ptr_;
      }
      stop_matches_symbol_ptrs[i] = overlap_check_data[i].stop_matches_symbol_ptr;
      overlap_check_data[i].next_match_ptr_ptr = &next_match_ptrs[i];
      overlap_check_data[i].match_stop_ptr = matches_stop_ptr[i];
      overlap_check_data[i].num_overlaps = num_candidates;
      overlap_check_data[i].match_nodes = match_nodes;
      if (overlap_check_data[i].stop_symbol_ptr - start_symbol_ptr_ + kMaxMatchLength <
          first_define_index)
        overlap_check_threads[i - 1] = std::jthread(
            [this, &td = overlap_check_data[i]] { overlap_check_no_defs_thread_impl(td); });
      else
        overlap_check_threads[i - 1] = std::jthread(
            [this, &td = overlap_check_data[i]] { overlap_check_thread_impl(td); });
    }
  }

  uint32_t num_overlaps = num_candidates;
  overlap_check_data[0].match_stop_ptr = matches_stop_ptr[0];
  for (size_t j = 0; j < num_candidates; j++) overlap_check_data[0].next[j] = -1;

  // Main thread overlap check
  {
    uint32_t prior_match_score_number[kMaxPriorMatches];
    uint32_t* prior_match_end_ptr[kMaxPriorMatches];
    uint32_t num_prior_matches = 0;
    MatchNode* match_node_ptr;
    uint32_t* isp = start_symbol_ptr_;

    if (static_cast<size_t>(stop_symbol_ptr - start_symbol_ptr_) + kMaxMatchLength >= first_define_index) {
      OverlapCheck local_oc{};
      local_oc.start_symbol_ptr = start_symbol_ptr_;
      local_oc.stop_symbol_ptr = stop_symbol_ptr;
      local_oc.match_nodes = match_nodes;
      local_oc.next_match_ptr_ptr = &next_match_ptrs[0];
      local_oc.match_stop_ptr = matches_stop_ptr[0];
      local_oc.candidate_bad = candidate_bad;
      local_oc.num_overlaps = num_candidates;
      local_oc.stop_matches_symbol_ptr = stop_matches_symbol_ptrs[0];
      overlap_check_thread_impl(local_oc);
      num_overlaps = local_oc.num_overlaps;
      std::memcpy(overlap_check_data[0].next, local_oc.next, sizeof(local_oc.next));
      std::memcpy(overlap_check_data[0].second, local_oc.second, sizeof(local_oc.second));
    } else {
      OverlapCheck local_oc{};
      local_oc.start_symbol_ptr = start_symbol_ptr_;
      local_oc.stop_symbol_ptr = stop_symbol_ptr;
      local_oc.match_nodes = match_nodes;
      local_oc.next_match_ptr_ptr = &next_match_ptrs[0];
      local_oc.match_stop_ptr = matches_stop_ptr[0];
      local_oc.candidate_bad = candidate_bad;
      local_oc.num_overlaps = num_candidates;
      local_oc.stop_matches_symbol_ptr = stop_matches_symbol_ptrs[0];
      overlap_check_no_defs_thread_impl(local_oc);
      num_overlaps = local_oc.num_overlaps;
      std::memcpy(overlap_check_data[0].next, local_oc.next, sizeof(local_oc.next));
      std::memcpy(overlap_check_data[0].second, local_oc.second, sizeof(local_oc.second));
    }
  }

  if (stop_symbol_ptr < end_symbol_ptr_) {
    for (size_t i = 1; i < 8; i++) {
      if (overlap_check_threads[i - 1].joinable())
        overlap_check_threads[i - 1].join();
    }
    if (fast_mode_ == 1) {
      for (uint16_t cn = 0; cn + 1 < num_candidates; cn++) {
        if (candidate_bad[cn] == 0) {
          for (size_t j = 0; j < 8; j++) {
            int32_t next_ol = overlap_check_data[j].next[cn];
            while (next_ol != -1) {
              candidate_bad[overlap_check_data[j].second[next_ol]] = 1;
              next_ol = overlap_check_data[j].next[next_ol];
            }
          }
        }
      }
    }
  } else if (fast_mode_ == 1) {
    for (uint16_t cn = 0; cn + 1 < num_candidates; cn++) {
      if (candidate_bad[cn] == 0) {
        int32_t next_ol = overlap_check_data[0].next[cn];
        while (next_ol != -1) {
          candidate_bad[overlap_check_data[0].second[next_ol]] = 1;
          next_ol = overlap_check_data[0].next[next_ol];
        }
      }
    }
  }

  // Assign new rule numbers
  {
    uint32_t j = next_new_symbol_number;
    for (size_t i = 0; i < num_candidates; i++) {
      if (candidate_bad[i] == 0) {
        if (j < symbol_counts_.size()) symbol_counts_[j] = 0;
        new_rule_number[i] = j++;
      }
    }
  }

  // Perform substitution in grammar stream
  in_symbol_ptr = start_symbol_ptr_;
  uint32_t* out_symbol_ptr = start_symbol_ptr_;
  int32_t prior_match_end = -1;
  const uint8_t max_i = stop_symbol_ptr < end_symbol_ptr_ ? 7 : 0;
  for (size_t i = 0; i <= max_i; i++) {
    const size_t num_pairs = static_cast<size_t>(next_match_ptrs[i] - matches_start_ptr[i]) >> 1;
    for (size_t j = 0; j < num_pairs; j++) {
      const uint16_t cn = static_cast<uint16_t>(*(matches_start_ptr[i] + (2 * j)));
      if (candidate_bad[cn] == 0) {
        const uint32_t start_index = *(matches_start_ptr[i] + (2 * j) + 1);
        if (static_cast<int32_t>(start_index) > prior_match_end) {
          prior_match_end = static_cast<int32_t>(start_index + candidates[cn].num_symbols - 1);
          uint32_t* sp = start_symbol_ptr_ + start_index;
          while (in_symbol_ptr < sp) *out_symbol_ptr++ = *in_symbol_ptr++;
          *out_symbol_ptr++ = new_rule_number[cn];
          if (new_rule_number[cn] < symbol_counts_.size())
            symbol_counts_[new_rule_number[cn]]++;
          in_symbol_ptr += candidates[cn].num_symbols;
        }
      }
    }
  }
  while (in_symbol_ptr < end_symbol_ptr_) *out_symbol_ptr++ = *in_symbol_ptr++;

  // Write new production rules and update symbol counts
  for (size_t i = 0; i < num_candidates; i++) {
    if (candidate_bad[i] == 0) {
      *out_symbol_ptr++ = num_rules + 0x80000001;
      uint32_t* ms_ptr = match_strings + (max_match_length * i);
      uint32_t* ms_end = ms_ptr + candidates[i].num_symbols;
      uint32_t sym1 = *ms_ptr++;
      *out_symbol_ptr++ = sym1;
      if (num_terminals_ + num_rules < symbol_ends_.size()) {
        symbol_ends_[num_terminals_ + num_rules].start = symbol_ends_[sym1].start;
      }
      const uint32_t repeats = (num_terminals_ + num_rules < symbol_counts_.size())
                                    ? symbol_counts_[num_terminals_ + num_rules] - 1 : 0;
      if (sym1 < symbol_counts_.size()) symbol_counts_[sym1] -= repeats;
      while (ms_ptr != ms_end) {
        const uint32_t sym2 = *ms_ptr;
        if (sym2 < symbol_counts_.size()) symbol_counts_[sym2] -= repeats;
        o1c_[symbol_ends_[sym1].end][symbol_ends_[sym2].start] -= repeats;
        num_ends_[symbol_ends_[sym1].end] -= repeats;
        num_starts_[symbol_ends_[sym2].start] -= repeats;
        sym1 = sym2;
        *out_symbol_ptr++ = *ms_ptr++;
      }
      if (num_terminals_ + num_rules < symbol_ends_.size())
        symbol_ends_[num_terminals_ + num_rules].end = symbol_ends_[sym1].end;
      num_rules++;
    } else {
      candidate_bad[i] = 0;
    }
  }

  if (num_rules == 0) {
    first_define_index = static_cast<uint32_t>(out_symbol_ptr - start_symbol_ptr_);
  } else {
    if (out_symbol_ptr < start_symbol_ptr_ + first_define_index)
      first_define_index = static_cast<uint32_t>(out_symbol_ptr - start_symbol_ptr_);
    if (*(start_symbol_ptr_ + first_define_index) != 0x80000001u)
      while (*(start_symbol_ptr_ + --first_define_index) != 0x80000001u) ;
  }
  end_symbol_ptr_ = out_symbol_ptr;
  *end_symbol_ptr_ = 0xFFFFFFFE;
  num_file_symbols_ = static_cast<uint32_t>(end_symbol_ptr_ - start_symbol_ptr_);

  // Score adaptation
  if (fast_mode_ == 0) {
    if (scan_mode == ScanMode::kRetry) {
      if (rsd->num_candidates != 0) {
        if (rsd->num_candidates == static_cast<uint16_t>(max_scores)) {
          if (min_score_ < prior_min_score) {
            if (max_scores > 10000) {
              new_min_score = min_score_ + min_score_ - prior_min_score - 0.015f;
            } else {
              new_min_score = min_score_ + min_score_ - prior_min_score - 0.1f;
              if (new_min_score < min_score_ - 1.0f) new_min_score = min_score_ - 1.0f;
              if (new_min_score < 0.0f) new_min_score = 0.0f;
            }
            prior_min_score = min_score_;
          } else {
            new_min_score = (0.5f * (prior_min_score + min_score_)) - 0.1f;
            if (new_min_score >= prior_min_score) new_min_score = prior_min_score - 0.05f;
            else if (new_min_score < min_score_) new_min_score = min_score_ - 0.09f;
            prior_min_score = candidates[num_candidates - 1].score;
          }
          min_score_ = new_min_score;
        } else if (min_score_ < prior_min_score) {
          new_min_score = min_score_ + min_score_ - prior_min_score - 0.15f;
          prior_min_score = min_score_;
          min_score_ = new_min_score;
        } else {
          new_min_score = min_score_ + min_score_ - prior_min_score - 0.15f;
          min_score_ = new_min_score < prior_min_score ? new_min_score : prior_min_score - 0.03f;
        }
        if (min_score_ < 0.0f) min_score_ = 0.0f;
      } else if (min_score_ > 0.0f) {
        prior_min_score = min_score_;
        min_score_ = 0.0f;
        num_candidates = 1;
      }
    } else {
      scan_mode = ScanMode::kRetry;
      prior_min_score = min_score_;
      min_score_ = 0.25f * min_score_;
      if (min_score_ < 10.0f) min_score_ = 10.0f;
    }
  } else if (scan_mode == ScanMode::kRetry) {
    if (num_candidates == static_cast<uint16_t>(max_scores)) {
      if (min_score_ < prior_min_score) {
        if (prior_min_score != kBigFloat) {
          if (scan_cycle > 50) {
            if (scan_cycle > 100)
              new_min_score = (max_scores == kMaxScoresFast)
                                  ? (0.995f * min_score_ * (min_score_ / prior_min_score)) - 0.002f
                                  : (0.998f * min_score_ * (min_score_ / prior_min_score)) - 0.002f;
            else
              new_min_score = (0.99f * min_score_ * (min_score_ / prior_min_score)) - 0.002f;
          } else {
            new_min_score = (0.98f * min_score_ * (min_score_ / prior_min_score)) - 0.002f;
          }
          prior_min_score = min_score_;
          min_score_ = new_min_score;
        } else {
          prior_min_score = min_score_;
          min_score_ *= 0.5f;
        }
      } else {
        min_score_ = (0.95f * prior_min_score) - 0.002f;
      }
    } else if (min_score_ < prior_min_score) {
      if (prior_min_score != kBigFloat) {
        new_min_score = (0.95f * min_score_ * (min_score_ / prior_min_score)) - 0.002f;
        prior_min_score = min_score_;
        min_score_ = new_min_score;
      } else {
        prior_min_score = min_score_;
        min_score_ *= 0.5f;
      }
    } else {
      min_score_ = (0.95f * prior_min_score) - 0.002f;
    }
    if (min_score_ > 0.9f * section_scores[fast_section]) {
      min_score_ = (0.9f * section_scores[fast_section] < fast_min_score) &&
                       (min_score_ >= fast_min_score)
                       ? fast_min_score
                       : 0.9f * section_scores[fast_section];
    } else if (min_score_ < 0.0f) {
      min_score_ = 0.0f;
    }
  } else {
    scan_mode = ScanMode::kRetry;
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

void Compressor::main_loop(
    uint32_t* p_num_rules, ScanMode scan_mode,
    uint32_t num_terminals_used, uint8_t* end_RAM_ptr,
    uint32_t** p_in_symbol_ptr, uint8_t UTF8_compliant,
    RankScoresThreadData* rsd, uint32_t max_scores,
    ScoreData* node_data, uint32_t max_rules,
    uint16_t* candidates_index, uint8_t* candidate_bad,
    uint32_t first_define_index, uint32_t initial_max_scores,
    uint8_t max_terminal, uint32_t* new_symbol_number,
    uint8_t fast_section, uint8_t fast_sections,
    double profit_ratio_power, float fast_min_score,
    float section_scores[23], uint8_t section_repeats,
    uint16_t* p_scan_cycle) {
  uint32_t num_match_nodes = 0;
  uint32_t max_match_length = 0;
  uint32_t* match_strings = nullptr;
  TreeThreadData tree_thread_data[13];

  uint32_t prior_cycle_symbols = num_file_symbols_;
  float prior_min_score = kBigFloat;
  float cycle_start_ratio = 0.0f;
  float cycle_end_ratio = 1.0f;
  float new_min_score = 0.0f;
  float production_cost = 0.0f;
  float log2_nspsc = 0.0f;
  float new_symbol_cost[kNumPrecalcSymbolCosts];

  OverlapCheck* overlap_check_heap_buf = nullptr;
  OverlapCheck* overlap_check_data = nullptr;

  std::jthread build_tree_threads[7];
  std::jthread rank_scores_thread1;

  uint32_t* in_symbol_ptr = *p_in_symbol_ptr;
  uint32_t num_rules = *p_num_rules;
  uint16_t scan_cycle = *p_scan_cycle;
  uint16_t num_candidates = 1;

  do {
    uint32_t next_new_symbol_number;
    double d_num_file_symbols;
    uint8_t* free_RAM_ptr;
    double* symbol_entropy;
    float* symbol_entropy_f;
    uint32_t node_num_limit;
    uint16_t node_ptrs_num;
    uint32_t next_node_num;

    main_loop_init(&next_new_symbol_number, num_rules, &d_num_file_symbols,
                   &free_RAM_ptr, &symbol_entropy, &symbol_entropy_f, scan_mode,
                   &log2_nspsc, new_symbol_cost, &production_cost,
                   num_terminals_used, &scan_cycle, end_RAM_ptr, &node_num_limit);

    cycle_start_ratio = update_cycle_start_ratio(
        cycle_start_ratio, cycle_end_ratio, fast_section, fast_sections, prior_cycle_symbols);
    uint32_t* start_cycle_symbol_ptr =
        start_symbol_ptr_ + static_cast<uint32_t>(cycle_start_ratio * static_cast<float>(num_file_symbols_));
    in_symbol_ptr = start_cycle_symbol_ptr;

    build_and_score_suffix_tree(
        &in_symbol_ptr, &next_node_num, &node_ptrs_num, &cycle_end_ratio,
        start_cycle_symbol_ptr, node_num_limit, next_new_symbol_number,
        num_rules, max_scores, cycle_start_ratio, fast_section, fast_sections,
        symbol_entropy, symbol_entropy_f, profit_ratio_power,
        production_cost, log2_nspsc, new_symbol_cost,
        rsd, node_data, tree_thread_data, build_tree_threads, &rank_scores_thread1);
    num_candidates = rsd->num_candidates;
    prior_cycle_symbols = static_cast<uint32_t>(in_symbol_ptr - start_cycle_symbol_ptr);

    if (num_candidates == 0) {
      handle_zero_candidates(&scan_mode, &num_candidates, &prior_min_score,
                             &fast_section, &fast_sections, &fast_min_score);
    } else {
      process_ranked_candidates(
          &num_candidates, &scan_mode, next_new_symbol_number, max_rules,
          max_scores, &num_rules, &first_define_index, &in_symbol_ptr,
          candidates_index, candidate_bad, end_RAM_ptr, rsd,
          &overlap_check_data, &overlap_check_heap_buf, &num_match_nodes,
          &max_match_length, &match_strings, &fast_section, &fast_sections,
          &section_repeats, section_scores, &prior_min_score, &fast_min_score,
          &new_min_score, scan_cycle);
    }

    if (update_min_max_scores(&max_scores, &min_score_, prior_min_score,
                              num_candidates, num_rules, next_new_symbol_number,
                              initial_max_scores, fast_min_score, fast_sections) != 0)
      break;
  } while ((num_candidates != 0) && (num_terminals_ + num_rules < max_rules));

  *p_scan_cycle = scan_cycle;
  *p_num_rules = num_rules;
  *p_in_symbol_ptr = in_symbol_ptr;
  std::free(overlap_check_heap_buf);
  free_match_heap_bufs();
}

bool Compressor::compress(size_t in_size, size_t* outsize_ptr,
                           uint8_t** iobuf, const Params& params) {
  constexpr uint8_t INSERT_SYMBOL_CHAR = 0xFE;
  constexpr uint8_t DEFINE_SYMBOL_CHAR = 0xFF;

  start_symbol_ptr_ = nullptr;
  warned_suffix_nodes_ = 0;
  warned_match_nodes_ = 0;
  warned_main_nodes_ = 0;
  warned_rank_buffer_ = 0;
  free_match_heap_bufs();
  rank_scores_write_index_.store(0, std::memory_order_relaxed);
  rank_scores_read_index_.store(0, std::memory_order_relaxed);
  substitute_data_write_index_.store(0, std::memory_order_relaxed);
  substitute_data_read_index_.store(0, std::memory_order_relaxed);
  max_symbol_ptr_.store(0, std::memory_order_relaxed);
  scan_symbol_ptr_.store(0, std::memory_order_relaxed);

  const uint64_t max_memory_usage = sizeof(uint32_t*) >= 8 ? 0x800000000ULL : 0x70000000ULL;

  double profit_ratio_power;
  uint8_t create_words;
  fast_mode_ = in_size < 1000 ? 0 : params.fast_mode;
  order_ratio_ = params.order;
  if (params.user_set_profit_ratio_power != 0)
    profit_ratio_power = params.profit_ratio_power;
  else
    profit_ratio_power = 0.0;
  create_words = params.create_words;

  uint32_t max_rules = 0xA00000;
  if (max_rules > (in_size >> 4) + 0x110000)
    max_rules = static_cast<uint32_t>((in_size >> 4) + 0x110000);
  if (params.max_rules + 0x110000 < max_rules)
    max_rules = params.max_rules + 0x110000;

  symbol_counts_.assign(max_rules, 0);
  symbol_ends_.resize(max_rules);
  auto rsd = std::make_unique<RankScoresThreadData>();

  uint32_t max_scores = fast_mode_ == 1 ? kMaxScoresFast : kMaxScores;
  std::memset(num_starts_, 0, sizeof(num_starts_));
  std::memset(num_ends_, 0, sizeof(num_ends_));
  std::memset(o1c_, 0, sizeof(o1c_));

  uint64_t available_RAM;
  if (params.user_set_RAM_size != 0) {
    available_RAM = static_cast<uint64_t>(params.RAM_usage * static_cast<float>(0x100000));
    if (available_RAM > max_memory_usage) available_RAM = max_memory_usage;
    start_symbol_ptr_ = static_cast<uint32_t*>(std::malloc(available_RAM));
    if (start_symbol_ptr_ == nullptr) {
      glza::global_diagnostics().set("compress", "Insufficient RAM - unable to allocate %zu bytes",
                                     static_cast<size_t>(available_RAM));
      return false;
    }
    if (available_RAM < (41 * static_cast<uint64_t>(in_size)) / 10) {
      glza::global_diagnostics().set("compress", "Insufficient RAM to compress");
      std::free(start_symbol_ptr_); start_symbol_ptr_ = nullptr;
      return false;
    }
  } else {
    available_RAM = (static_cast<uint64_t>(in_size) * 250) + 40000000;
    if (available_RAM > max_memory_usage) available_RAM = max_memory_usage;
    if (available_RAM > 0x80000000ULL + (6 * static_cast<uint64_t>(in_size)))
      available_RAM = 0x80000000ULL + (6 * static_cast<uint64_t>(in_size));
    do {
      start_symbol_ptr_ = static_cast<uint32_t*>(std::malloc(available_RAM));
      if (start_symbol_ptr_ != nullptr) break;
      available_RAM = (available_RAM / 10) * 9;
    } while (available_RAM > 1500000000);
    if (start_symbol_ptr_ == nullptr || available_RAM < static_cast<uint64_t>(in_size) * 9 / 2) {
      glza::global_diagnostics().set("compress", "Insufficient RAM to compress");
      std::free(start_symbol_ptr_); start_symbol_ptr_ = nullptr;
      return false;
    }
  }
  uint8_t* end_RAM_ptr = reinterpret_cast<uint8_t*>(start_symbol_ptr_) + available_RAM;
  compress_ram_bytes_ = static_cast<size_t>(available_RAM);

  uint32_t* in_symbol_ptr = start_symbol_ptr_;
  uint32_t num_rules = 0;
  uint8_t UTF8_compliant = 0;
  uint8_t format = **iobuf;
  cap_encoded_ = (format == 1) ? 1 : 0;
  uint32_t max_UTF8_value = 0x7F;
  uint8_t* in_char_ptr = *iobuf + 1;
  uint8_t* end_char_ptr = *iobuf + in_size;

  if (format < 2) {
    while (in_char_ptr < end_char_ptr) {
      const uint8_t this_char = *in_char_ptr++;
      if (this_char < 0x80) {
        *in_symbol_ptr++ = static_cast<uint32_t>(this_char);
      } else if ((this_char < 0xC0) || (this_char >= 0xF2) ||
                 ((*in_char_ptr & 0xC0) != 0x80)) {
        break;
      } else {
        uint32_t UTF8_value = (0x40 * static_cast<uint32_t>(this_char & 0x1F)) +
                              (*in_char_ptr++ & 0x3F);
        if (this_char >= 0xE0) {
          if ((*in_char_ptr & 0xC0) != 0x80) break;
          UTF8_value = (0x40 * UTF8_value) + static_cast<uint32_t>(*in_char_ptr++ & 0x3F);
          if (this_char >= 0xF0) {
            if ((*in_char_ptr & 0xC0) != 0x80) break;
            UTF8_value = (0x40 * (UTF8_value & 0x7FFF)) +
                         static_cast<uint32_t>(*in_char_ptr++ & 0x3F);
          }
        }
        *in_symbol_ptr++ = UTF8_value;
        if (UTF8_value > max_UTF8_value) max_UTF8_value = UTF8_value;
      }
    }
    if (in_char_ptr == end_char_ptr) UTF8_compliant = 1;
  }

  uint8_t max_terminal;
  in_char_ptr = *iobuf + 1;
  if (UTF8_compliant != 0) {
    num_terminals_ = max_UTF8_value + 1;
    max_terminal = 0x7F;
    std::memset(symbol_counts_.data(), 0, 4 * num_terminals_);
    num_file_symbols_ = static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_);
    end_symbol_ptr_ = in_symbol_ptr;
    in_symbol_ptr = start_symbol_ptr_;
    while (in_symbol_ptr != end_symbol_ptr_) symbol_counts_[*in_symbol_ptr++]++;
    if (params.user_set_profit_ratio_power == 0)
      profit_ratio_power = fast_mode_ == 1 ? 1.0 : 2.0;
    for (size_t i = 0; i < num_terminals_; i++)
      symbol_ends_[i].start = symbol_ends_[i].end = get_UTF8_context(static_cast<uint32_t>(i));
  } else {
    num_terminals_ = 0x100;
    max_terminal = 0xFF;
    std::memset(symbol_counts_.data(), 0, 0x400);
    in_symbol_ptr = start_symbol_ptr_;
    while (in_char_ptr != end_char_ptr) {
      *in_symbol_ptr = static_cast<uint32_t>(*in_char_ptr++);
      symbol_counts_[*in_symbol_ptr++]++;
    }
    num_file_symbols_ = static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_);
    end_symbol_ptr_ = in_symbol_ptr;
    if (params.user_set_profit_ratio_power == 0) {
      if (fast_mode_ == 0 && cap_encoded_ != 0) profit_ratio_power = 2.0;
      else if ((format & 0xFE) == 0) profit_ratio_power = 1.0;
      else profit_ratio_power = 0.0;
    }
    for (size_t i = 0; i < num_terminals_; i++)
      symbol_ends_[i].start = symbol_ends_[i].end = static_cast<uint8_t>(i);
  }
  std::free(*iobuf);

  if (fast_mode_ != 0)
    score_map_.assign(2 * in_size, 0);

  if (params.max_rules + num_terminals_ < max_rules)
    max_rules = params.max_rules + num_terminals_;

  in_symbol_ptr = start_symbol_ptr_;
  {
    uint8_t sym1;
    uint8_t sym2 = static_cast<uint8_t>(symbol_ends_[*in_symbol_ptr++].end);
    while (in_symbol_ptr != end_symbol_ptr_) {
      sym1 = sym2;
      sym2 = symbol_ends_[*in_symbol_ptr++].end;
      o1c_[sym1][sym2]++;
      num_ends_[sym1]++;
      num_starts_[sym2]++;
    }
  }

  uint32_t max_x_log2_x = 0;
  for (size_t i = 0; i < 0x100; i++) {
    if (num_ends_[i] > max_x_log2_x) max_x_log2_x = num_ends_[i];
  }
  max_x_log2_x += 2;
  if (max_x_log2_x > kNumPrecalcXLog2X) max_x_log2_x = kNumPrecalcXLog2X;

  uint32_t first_define_index = static_cast<uint32_t>(in_symbol_ptr - start_symbol_ptr_);
  *end_symbol_ptr_ = 0xFFFFFFFE;

  auto new_symbol_number = std::make_unique<uint32_t[]>(max_scores);
  auto node_data_ptr = std::make_unique<ScoreData[]>(kNodeDataStackDepth);
  // Reorder loop in process_ranked_candidates walks kMaxScoresFast entries.
  auto candidates_index = std::make_unique<uint16_t[]>(kMaxScoresFast);
  auto candidate_bad = std::make_unique<uint8_t[]>(max_scores);
  rsd->candidates_index = candidates_index.get();

  uint32_t num_terminals_used = 0;
  for (size_t i = 0; i < num_terminals_; i++) {
    if (symbol_counts_[i] != 0) num_terminals_used++;
  }
  for (size_t i = 1; i < kNumPrecalcLog2X; i++)
    log2_x_[i] = log2(static_cast<double>(i));

  uint32_t initial_max_scores;
  uint8_t fast_sections;
  uint8_t fast_section = 0;
  float fast_min_score;
  uint16_t* candidates_position = nullptr;
  uint8_t section_repeats = 0;
  float section_scores[23];
  if (fast_mode_ == 0) {
    x_log2_x_.assign(max_x_log2_x, 0.0);
    for (size_t i = 1; i < max_x_log2_x; i++)
      x_log2_x_[i] = static_cast<double>(i) * log2(static_cast<double>(i));
    initial_max_scores = static_cast<uint32_t>(500.0 + (0.075 * sqrt(static_cast<double>(num_file_symbols_))));
    fast_sections = 1;
    fast_min_score = 0.0f;
  } else {
    candidates_position = static_cast<uint16_t*>(std::malloc(2 * max_scores));
    if (candidates_position == nullptr) {
      glza::global_diagnostics().set("compress", "memory allocation failed");
      std::free(start_symbol_ptr_);
      return false;
    }
    rsd->candidates_position = candidates_position;
    fast_sections = 23;
    fast_section = 0;
    fast_min_score = 4.0f;
    section_repeats = 0;
    for (size_t i = 0; i < 23; i++) section_scores[i] = kBigFloat;
    initial_max_scores = static_cast<uint32_t>(100.0 + (22.0 * pow(static_cast<double>(num_file_symbols_), 0.3333)));
  }
  std::memset(candidate_bad.get(), 0, max_scores);
  min_score_ = 10.0f;
  uint16_t scan_cycle = 0;

  const ScanMode initial_scan_mode =
      (((cap_encoded_ == 0) && ((UTF8_compliant == 0) || (fast_mode_ == 0))) ||
       (create_words == 0))
          ? ScanMode::kRunDedup
          : ScanMode::kWords;

  main_loop(&num_rules, initial_scan_mode, num_terminals_used, end_RAM_ptr,
            &in_symbol_ptr, UTF8_compliant, rsd.get(), max_scores,
            node_data_ptr.get(), max_rules, candidates_index.get(),
            candidate_bad.get(), first_define_index, initial_max_scores,
            max_terminal, new_symbol_number.get(), fast_section, fast_sections,
            profit_ratio_power, fast_min_score, section_scores, section_repeats,
            &scan_cycle);

  if (fast_mode_ != 0) {
    score_map_.clear();
    std::free(candidates_position);
  }

  // Allocate output buffer and serialize
  *iobuf = static_cast<uint8_t*>(std::malloc((4 * num_file_symbols_) + 1));
  if (*iobuf == nullptr) {
    glza::global_diagnostics().set("compress", "Compressed output buffer allocation failed");
    std::free(start_symbol_ptr_);
    return false;
  }
  in_char_ptr = *iobuf;
  if (UTF8_compliant != 0) {
    *in_char_ptr++ = 5 | (cap_encoded_ << 1);
    uint8_t base_bits = 7;
    while ((max_UTF8_value >> base_bits) != 0) base_bits++;
    *in_char_ptr++ = base_bits;
  } else if (cap_encoded_ != 0) {
    *in_char_ptr++ = 3;
  } else {
    *in_char_ptr++ = format;
  }

  in_symbol_ptr = start_symbol_ptr_;
  const uint32_t next_new_symbol_number = num_terminals_ + num_rules;

  if (UTF8_compliant != 0) {
    while (in_symbol_ptr != end_symbol_ptr_) {
      const uint32_t sv = *in_symbol_ptr++;
      if (sv < 0x80) {
        *in_char_ptr++ = static_cast<uint8_t>(sv);
      } else if (sv < num_terminals_) {
        if (sv < 0x800) {
          *in_char_ptr++ = static_cast<uint8_t>(0xC0 + (sv >> 6));
        } else if (sv < 0x10000) {
          *in_char_ptr++ = static_cast<uint8_t>(0xE0 + (sv >> 12));
          *in_char_ptr++ = static_cast<uint8_t>(0x80 + ((sv >> 6) & 0x3F));
        } else {
          *in_char_ptr++ = static_cast<uint8_t>(0xF0 + (sv >> 18));
          *in_char_ptr++ = static_cast<uint8_t>(0x80 + ((sv >> 12) & 0x3F));
          *in_char_ptr++ = static_cast<uint8_t>(0x80 + ((sv >> 6) & 0x3F));
        }
        *in_char_ptr++ = static_cast<uint8_t>(0x80 + (sv & 0x3F));
      } else {
        uint32_t out_sv;
        if (static_cast<int32_t>(sv) >= 0) {
          out_sv = sv - num_terminals_;
          *in_char_ptr++ = INSERT_SYMBOL_CHAR;
        } else {
          out_sv = sv - 1;
          *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
        }
        *in_char_ptr++ = static_cast<uint8_t>((out_sv >> 16) & 0xFF);
        *in_char_ptr++ = static_cast<uint8_t>((out_sv >> 8) & 0xFF);
        *in_char_ptr++ = static_cast<uint8_t>(out_sv & 0xFF);
      }
    }
  } else {
    while (in_symbol_ptr != end_symbol_ptr_) {
      const uint32_t sv = *in_symbol_ptr++;
      if (sv <= DEFINE_SYMBOL_CHAR) {
        *in_char_ptr++ = static_cast<uint8_t>(sv);
        if (sv >= INSERT_SYMBOL_CHAR) *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
      } else {
        uint32_t out_sv;
        if (static_cast<int32_t>(sv) >= 0) {
          out_sv = sv - 0x100;
          *in_char_ptr++ = INSERT_SYMBOL_CHAR;
        } else {
          out_sv = sv;
          *in_char_ptr++ = DEFINE_SYMBOL_CHAR;
        }
        *in_char_ptr++ = static_cast<uint8_t>((out_sv >> 16) & 0xFF);
        *in_char_ptr++ = static_cast<uint8_t>((out_sv >> 8) & 0xFF);
        *in_char_ptr++ = static_cast<uint8_t>(out_sv & 0xFF);
      }
    }
  }

  const size_t out_size = static_cast<size_t>(in_char_ptr - *iobuf);
  *iobuf = static_cast<uint8_t*>(std::realloc(*iobuf, out_size));
  if (*iobuf == nullptr) {
    glza::global_diagnostics().set("compress", "Compressed output buffer reallocation failed");
    std::free(start_symbol_ptr_);
    return false;
  }
  *outsize_ptr = out_size;
  std::free(start_symbol_ptr_);
  start_symbol_ptr_ = nullptr;

#ifdef PRINTON
  if (fast_mode_ != 0) {
    fprintf(stderr, "PASS %u: grammar size %u, %u production rules  \n",
            static_cast<unsigned>(scan_cycle), static_cast<unsigned>(num_file_symbols_) + 1,
            static_cast<unsigned>(num_rules));
  }
#endif
  return true;
}

}  // namespace glza
