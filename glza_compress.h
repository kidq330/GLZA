#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "glza_error.h"
#include "glza_params.h"

namespace glza {

inline constexpr uint32_t kMaxPriorMatches = 20;
inline constexpr uint32_t kMaxMatchLength = 8000;
inline constexpr uint32_t kBaseNodesChildArraySize = 16;
inline constexpr uint32_t kNumPrecalcLog2X = 0x4000;
inline constexpr uint32_t kNumPrecalcXLog2X = 0x1000000;
inline constexpr uint32_t kNumPrecalcNfsmrLogs = 0x400;
inline constexpr uint32_t kNumPrecalcSymbolCosts = 2000;
inline constexpr uint32_t kMaxScores = 30000;
inline constexpr uint32_t kMaxScoresFast = 0x7FFF;
inline constexpr uint32_t kNodeDataStackDepth = kMaxMatchLength + 32;
inline constexpr float kBigFloat = 1000000000.0f;

enum class ScanMode : uint8_t {
  kWords = 0,
  kRunDedup = 1,
  kGeneral = 2,
  kRetry = 3,
};

class BumpArena {
  std::vector<std::byte> buf_;

 public:
  BumpArena() = default;
  explicit BumpArena(size_t bytes) : buf_(bytes, std::byte{0}) {}

  void resize(size_t bytes) { buf_.assign(bytes, std::byte{0}); }
  [[nodiscard]] size_t capacity() const { return buf_.size(); }

  uint8_t* base() { return reinterpret_cast<uint8_t*>(buf_.data()); }
  uint8_t* end() { return base() + buf_.size(); }

  template <typename T>
  T* as(size_t byte_offset = 0) {
    return reinterpret_cast<T*>(base() + byte_offset);
  }
};

struct SuffixNode {
  uint32_t symbol;
  uint32_t last_match_index;
  int32_t sibling_node_num[2];
  int32_t child_node_num;
  uint32_t num_extra_symbols;
  uint32_t instances;
};

struct MatchNode {
  uint32_t symbol;
  uint32_t num_symbols;
  uint32_t score_number;
  MatchNode* child_ptr;
  uint32_t sibling_node_num[16];
  MatchNode* miss_ptr;
  MatchNode* hit_ptr;
};

class Compressor {
 public:
  bool compress(size_t in_size, size_t* outsize_ptr, uint8_t** iobuf,
                const Params& params);

 private:
  struct SymbolEndsData {
    uint8_t start;
    uint8_t end;
  };

  struct NodeScoreData {
    float score;
    uint32_t last_match_index;
    uint32_t last_match_index2;
    uint16_t num_symbols;
  };

  struct ScoreData {
    SuffixNode* node_ptr;
    double string_entropy;
    double string_profit;
    float string_entropy_f;
    uint16_t num_symbols;
    uint8_t next_sibling;
  };

  struct TreeThreadData {
    uint32_t* start_cycle_symbol_ptr;
    uint32_t min_symbol;
    uint32_t max_symbol;
    uint32_t nodes_limit;
    uint32_t first_node_num;
    int32_t* base_nodes_child_node_num;
  };

  struct WordTreeThreadData {
    uint32_t first_node_num;
    uint32_t nodes_limit;
    int32_t start_positions[256];
    std::atomic<uint16_t> write_index;
    std::atomic<uint16_t> read_index;
  };

  struct RankScoresThreadData {
    uint16_t* candidates_index;
    uint16_t max_scores;
    uint16_t num_candidates;
    uint32_t num_file_symbols;
    NodeScoreData rank_scores_buffer[0x10000];
    NodeScoreData candidates[0x8000];
    uint16_t* candidates_position;
  };

  struct SubstituteThreadData {
    uint32_t* in_symbol_ptr;
    uint32_t* out_symbol_ptr;
    uint32_t* symbol_counts;
    uint32_t* substitute_data;
    uint32_t max_rule_symbol;
  };

  struct OverlapCheck {
    uint32_t* start_symbol_ptr;
    uint32_t* stop_matches_symbol_ptr;
    uint32_t* stop_symbol_ptr;
    uint32_t** next_match_ptr_ptr;
    uint32_t* match_stop_ptr;
    uint32_t num_overlaps;
    uint8_t* candidate_bad;
    MatchNode* match_nodes;
    uint32_t second[150000];
    int32_t next[150000];
  };

  struct FindSubstitutionsThreadData {
    uint32_t* start_symbol_ptr;
    uint32_t* stop_symbol_ptr;
    uint32_t extra_match_symbols;
    uint32_t data[0x800000];
    MatchNode* match_nodes;
    std::atomic<uint8_t> done;
    std::atomic<uint32_t> write_index;
    std::atomic<uint32_t> read_index;
  };

  struct MatchLimitCtx {
    uint32_t match_nodes_limit;
    uint32_t num_match_nodes;
    uint16_t num_candidates;
    uint16_t candidate_index;
    uint32_t max_match_length;
    size_t arena_match_bytes;
  };

  // --- member variables (ex file-scope statics) ---
  BumpArena arena_;
  uint32_t num_file_symbols_{};
  uint32_t num_terminals_{};
  uint32_t* start_symbol_ptr_{};
  uint32_t* end_symbol_ptr_{};
  uint8_t* end_ram_ptr_{};

  std::vector<uint32_t> symbol_counts_;
  std::vector<SymbolEndsData> symbol_ends_;
  std::vector<int16_t> score_map_;
  std::vector<double> x_log2_x_;
  std::vector<MatchNode> match_nodes_heap_;
  std::vector<uint32_t> match_strings_heap_;
  std::vector<MatchNode*> child_ptr_array_;

  uint32_t* next_match_ptr_[8]{};
  uint32_t num_starts_[0x100]{};
  uint32_t num_ends_[0x100]{};
  uint32_t o1c_[0x100][0x100]{};
  double log2_x_[kNumPrecalcLog2X]{};
  double nfs_profit_[kNumPrecalcNfsmrLogs]{};

  int32_t* base_nodes_child_node_num_{};
  SuffixNode* nodes_{};
  uint32_t nodes_num_limit_{};
  uint32_t child_ptr_array_size_{};
  NodeScoreData* candidates_{};

  uint8_t cap_encoded_{};
  uint8_t fast_mode_{};
  uint8_t warned_suffix_nodes_{};
  uint8_t warned_match_nodes_{};
  uint8_t warned_main_nodes_{};
  uint8_t warned_rank_buffer_{};
  size_t compress_ram_bytes_{};

  MatchLimitCtx match_limit_ctx_{};

  std::atomic<uint16_t> rank_scores_write_index_{};
  std::atomic<uint16_t> rank_scores_read_index_{};
  std::atomic<uint32_t> substitute_data_write_index_{};
  std::atomic<uint32_t> substitute_data_read_index_{};
  std::atomic<uintptr_t> max_symbol_ptr_{};
  std::atomic<uintptr_t> scan_symbol_ptr_{};

  double log_file_symbols_{};
  double nfs_p1_x_log_p1_{};
  double new_rule_cost_{};
  double order_ratio_{};
  float min_score_{};

  std::mutex suffix_tree_mutex_;
  std::unique_ptr<RankScoresThreadData> rank_scores_data_;

  // --- private helpers ---
  void warn_once(uint8_t& flag, const char* message);
  void free_match_heap_bufs();
  bool ensure_match_heap(uint32_t min_node_slots, uint32_t match_string_words);
  void note_match_limit(uint32_t match_nodes_limit, uint32_t num_match_nodes,
                        uint16_t num_candidates, uint16_t candidate_index,
                        uint32_t max_match_length, size_t arena_match_bytes);
  void bind_match_storage(MatchNode** out_match_nodes,
                          uint32_t* out_match_nodes_limit,
                          uint32_t** out_match_strings, uint8_t* free_RAM_ptr,
                          uintptr_t match_region_end_limit,
                          uint32_t child_ptr_count, uint16_t num_candidates,
                          uint32_t max_match_length, uint32_t est_match_nodes);
  void warn_suffix_nodes_limit(uint32_t limit, uint32_t next_num);
  void warn_match_nodes_limit();
  void warn_main_nodes_limit(uint32_t limit, uint32_t next_num);
  void warn_rank_buffer_limit(uint16_t max_scores);

  SuffixNode* create_suffix_node(uint32_t suffix_symbol, uint32_t symbol_index,
                                 uint32_t* next_node_num_ptr);
  SuffixNode* split_node_for_overlap(SuffixNode* node_ptr, uint32_t split_index,
                                     uint32_t in_symbol_index,
                                     uint32_t* next_node_num_ptr);
  void add_word_suffix(uint32_t* in_symbol_ptr, uint32_t* next_node_num_ptr);
  void add_suffix(uint32_t first_symbol, const uint32_t* in_symbol_ptr,
                  uint32_t* next_node_num_ptr);

  void build_tree_thread_impl(TreeThreadData& data);
  void word_build_tree_thread_impl(WordTreeThreadData& data);
  void rank_scores_thread_impl(RankScoresThreadData& data);
  void rank_scores_thread_fast_impl(RankScoresThreadData& data);
  void rank_word_scores_thread_impl(RankScoresThreadData& data);
  void overlap_check_thread_impl(OverlapCheck& data);
  void overlap_check_no_defs_thread_impl(OverlapCheck& data);
  void find_substitutions_thread_impl(FindSubstitutionsThreadData& data);
  void substitute_thread_impl(SubstituteThreadData& data);

  void overlap_check_invalidate_overlap(OverlapCheck* td, uint8_t* candidate_bad,
                                        uint32_t* num_overlaps,
                                        uint32_t prior_score,
                                        uint32_t node_score_number);
  void overlap_check_handle_leaf_match(OverlapCheck* td, MatchNode* mn,
                                       uint32_t* in_symbol_ptr,
                                       uint8_t* candidate_bad,
                                       uint32_t* num_overlaps,
                                       uint32_t* prior_match_score_number,
                                       uint32_t** prior_match_end_ptr,
                                       uint32_t* num_prior_matches);

  void score_base_node_tree(SuffixNode* node_ptr, ScoreData* node_data,
                            double profit_ratio_power,
                            const double* symbol_entropy,
                            NodeScoreData* rank_scores_buffer,
                            uint16_t* node_ptrs_num_ptr,
                            uint32_t prior_symbol);
  void score_base_node_tree_fast(SuffixNode* node_ptr, ScoreData* node_data,
                                 float string_entropy, float production_cost,
                                 float profit_ratio_power,
                                 float log2_nspsc, const float* new_symbol_cost,
                                 const float* symbol_entropy,
                                 NodeScoreData* rank_scores_buffer,
                                 uint16_t* node_ptrs_num_ptr);
  void score_base_node_tree_cap(SuffixNode* node_ptr, ScoreData* node_data,
                                double profit_ratio_power,
                                const double* symbol_entropy,
                                NodeScoreData* rank_scores_buffer,
                                uint16_t* node_ptrs_num_ptr,
                                uint32_t prior_symbol);
  void score_base_node_tree_cap_fast(SuffixNode* node_ptr, ScoreData* node_data,
                                     float string_entropy,
                                     float production_cost,
                                     float profit_ratio_power,
                                     float log2_nspsc,
                                     const float* new_symbol_cost,
                                     const float* symbol_entropy,
                                     NodeScoreData* rank_scores_buffer,
                                     uint16_t* node_ptrs_num_ptr);
  void score_base_node_tree_words(SuffixNode* node_ptr, ScoreData* node_data,
                                  float production_cost, float log2_nspsc,
                                  const float* new_symbol_cost,
                                  const float* symbol_entropy,
                                  NodeScoreData* rank_scores_buffer,
                                  uint16_t* node_ptrs_num_ptr);

  void score_symbol_tree(uint32_t min_symbol, uint32_t max_symbol,
                         NodeScoreData* rank_scores_buffer,
                         ScoreData* node_data, uint16_t* node_ptrs_num_ptr,
                         double profit_ratio_power, double* symbol_entropy,
                         const uint32_t* symbol_counts);
  void score_symbol_tree_fast(uint32_t min_symbol, uint32_t max_symbol,
                              NodeScoreData* rank_scores_buffer,
                              ScoreData* node_data,
                              uint16_t* node_ptrs_num_ptr,
                              float production_cost, double profit_ratio_power,
                              float log2_nspsc, float* new_symbol_cost,
                              float* symbol_entropy,
                              const uint32_t* symbol_counts);
  void score_symbol_tree_words(NodeScoreData* rank_scores_buffer,
                               ScoreData* node_data,
                               uint16_t* node_ptrs_num_ptr,
                               float production_cost, float log2_nspsc,
                               float* new_symbol_cost,
                               float* symbol_entropy);

  uint32_t stca_setup(size_t thread_count,
                      const uint8_t thread_symbol_limit[],
                      const uint32_t thread_first_node_num[],
                      const uint32_t thread_nodes_limit[], uint32_t num_rules,
                      uint32_t next_new_symbol_number,
                      TreeThreadData tree_thread_data[13],
                      uint32_t* start_cycle_symbol_ptr);
  float update_cycle_start_ratio(float cycle_start_ratio,
                                 float cycle_end_ratio, uint8_t fast_section,
                                 uint8_t fast_sections,
                                 uint32_t prior_cycle_symbols);

  void main_loop_init(uint32_t* out_next_new_sym, uint32_t num_rules,
                      double* out_d_nfs, uint8_t** out_free_ram,
                      double** out_symbol_entropy,
                      float** out_symbol_entropy_f, ScanMode scan_mode,
                      float* ptr_log2_nspsc,
                      float new_symbol_cost[kNumPrecalcSymbolCosts],
                      float* ptr_production_cost, uint32_t num_terminals_used,
                      uint16_t* ptr_scan_cycle, const uint8_t* end_ram,
                      uint32_t* out_node_num_limit);

  void scan_mode0(int32_t** out_base_node_child_num_ptr,
                  uint32_t* out_next_node_num, uint32_t** out_in_symbol_ptr,
                  uint8_t UTF8_compliant, uint32_t node_num_limit,
                  float* symbol_entropy_f, uint32_t* p_next_new_symbol_number,
                  uint16_t* out_node_ptrs_num,
                  RankScoresThreadData* rank_scores_data_ptr,
                  uint16_t max_scores, uint32_t* out_prior_cycle_symbols,
                  double order_0_entropy, double d_num_file_symbols,
                  uint16_t* out_num_candidates, size_t* ptr_sub_heap_size,
                  uint32_t max_rules, uint16_t** ptr_candidates_index,
                  uint8_t** ptr_sub_heap_buf, uint8_t** out_free_ram,
                  uint32_t** out_sub_data, SubstituteThreadData* ptr_sub_td,
                  uintptr_t end_ram, uint32_t* out_num_match_nodes,
                  uint32_t* out_max_match_length, uint32_t** out_match_strings,
                  OverlapCheck** ptr_oc_heap, uint8_t** ptr_candidate_bad,
                  uint32_t* ptr_num_rules,
                  FindSubstitutionsThreadData** ptr_fst_buf,
                  FindSubstitutionsThreadData** ptr_fst,
                  std::jthread* p_rank_thread, ScoreData* node_data,
                  float production_cost, float log2_nspsc,
                  float new_symbol_cost[kNumPrecalcSymbolCosts],
                  uint32_t* ptr_first_define_index,
                  OverlapCheck** out_oc_data);

  uint8_t scan_mode1(uint32_t** out_in_symbol_ptr, uint32_t next_new_sym,
                     uint32_t max_rules, uint32_t* out_max_scores,
                     uint32_t initial_max_scores, uint8_t max_terminal,
                     uint32_t* new_symbol_number, uint32_t* p_num_rules,
                     uint32_t* p_first_define_index, float* p_prior_min_score);

  void build_and_score_suffix_tree(
      uint32_t** out_in_symbol_ptr, uint32_t* out_next_node_num,
      uint16_t* out_node_ptrs_num, float* out_cycle_end_ratio,
      uint32_t* start_cycle_symbol_ptr, uint32_t node_num_limit,
      uint32_t next_new_sym, uint32_t num_rules, uint32_t max_scores,
      float cycle_start_ratio, uint8_t fast_section, uint8_t fast_sections,
      double* symbol_entropy, float* symbol_entropy_f,
      double profit_ratio_power, float production_cost, float log2_nspsc,
      float new_symbol_cost[kNumPrecalcSymbolCosts],
      RankScoresThreadData* rank_scores_data_ptr, ScoreData* node_data,
      TreeThreadData tree_thread_data[13], std::jthread build_tree_threads[7],
      std::jthread* rank_scores_thread1);

  void handle_zero_candidates(ScanMode* scan_mode, uint16_t* num_candidates,
                              float* prior_min_score, uint8_t* fast_section,
                              uint8_t* fast_sections, float* fast_min_score);

  void process_ranked_candidates(
      uint16_t* p_num_candidates, ScanMode* p_scan_mode,
      uint32_t next_new_sym, uint32_t max_rules, uint32_t max_scores,
      uint32_t* p_num_rules, uint32_t* p_first_define_index,
      uint32_t** p_in_symbol_ptr, const uint16_t* candidates_index,
      uint8_t* candidate_bad, uint8_t* end_ram,
      RankScoresThreadData* rank_scores_data_ptr,
      OverlapCheck** p_oc_data, OverlapCheck** p_oc_heap,
      uint32_t* p_num_match_nodes, uint32_t* p_max_match_length,
      uint32_t** p_match_strings, uint8_t* p_fast_section,
      uint8_t* p_fast_sections, uint8_t* p_section_repeats,
      float section_scores[23], float* p_prior_min_score,
      float* p_fast_min_score, float* p_new_min_score, uint16_t scan_cycle);

  uint8_t update_min_max_scores(uint32_t* ptr_max_scores, float* ptr_min_score,
                                float prior_min_score, uint16_t num_candidates,
                                uint32_t num_rules, uint32_t next_new_sym,
                                uint32_t initial_max_scores,
                                float fast_min_score, uint8_t fast_sections);

  void main_loop(uint32_t* p_num_rules, ScanMode scan_mode,
                 uint32_t num_terminals_used, uint8_t* end_ram,
                 uint32_t** p_in_symbol_ptr, uint8_t UTF8_compliant,
                 RankScoresThreadData* rank_scores_data_ptr,
                 uint32_t max_scores, ScoreData* node_data,
                 uint32_t max_rules, uint16_t* candidates_index,
                 uint8_t* candidate_bad, uint32_t first_define_index,
                 uint32_t initial_max_scores, uint8_t max_terminal,
                 uint32_t* new_symbol_number, uint8_t fast_section,
                 uint8_t fast_sections, double profit_ratio_power,
                 float fast_min_score, float section_scores[23],
                 uint8_t section_repeats, uint16_t* p_scan_cycle);
};

}  // namespace glza
