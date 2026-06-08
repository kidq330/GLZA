#pragma once

#include "glza_model.h"
#include "glza_params.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace glza {

class Encoder {
 public:
  struct SymbolData {
    uint8_t starts, ends, code_length, type;
    uint32_t count, hits, array_index, symbol_start_index, previous, previous2;
    int32_t space_score;
    float score;
  };

  struct TransmitSymbol {
    uint32_t symbol;
    uint32_t distance;
  };

  bool encode(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
              uint8_t* outbuf, FILE* fd_out, size_t filesize,
              const Params& params);

 private:
  EncoderModel model_;

  uint8_t end_char_{};
  uint8_t found_first_symbol_{};
  uint8_t UTF8_compliant_{};
  uint8_t cap_encoded_{};
  uint8_t prior_is_cap_{};
  uint8_t prior_end_{};
  uint8_t use_mtf_{};
  uint8_t max_code_length_{};
  uint8_t queue_offset_{};
  uint8_t cap_symbol_defined_{};
  uint8_t cap_lock_symbol_defined_{};
  uint8_t max_regular_code_length_{};

  uint16_t queue_size_{};
  uint16_t queue_size_az_{};
  uint16_t queue_size_space_{};
  uint16_t queue_size_other_{};

  uint32_t prior_symbol_{};
  uint32_t num_grammar_rules_{};
  uint32_t num_transmits_{};
  uint32_t num_base_symbols_{};

  std::array<uint8_t, 0x100> symbol_lengths_{};
  std::array<uint8_t, 0x100> bin_code_length_{};
  std::array<uint8_t, 16> queue_miss_code_length_{};
  std::array<std::array<uint8_t, 26>, 0x100> sym_list_bits_{};
  std::array<std::array<uint16_t, 26>, 0x100> nbob_{};
  std::array<std::array<uint16_t, 26>, 0x100> fbob_{};
  std::array<uint16_t, 0x100> sum_nbob_{};
  std::array<std::array<uint32_t, 26>, 0x100> nsob_{};
  std::array<uint32_t, 0x100> queue_{};

  std::vector<SymbolData> sd_;
  std::vector<uint32_t> symbol_array_;
  std::vector<TransmitSymbol> transmits_;
  std::array<std::array<std::vector<uint32_t>, 26>, 0x100> sym_list_ptrs_;

  bool encode_queue_ok() const;
  void encode_queue_fail(const char* reason);
  void queue_subcount_inc(uint16_t& subcount, const char* which);
  void queue_subcount_dec(uint16_t& subcount, const char* which);
  void print_string(uint32_t symbol_number);
  uint32_t find_string_length(uint32_t symbol_number);
  uint32_t sum_dictionary_string_bytes(uint32_t num_codes_val,
                                       const uint32_t* first_define);
  void get_symbol_category(uint32_t symbol_number, uint8_t* sym_type_ptr);
  uint8_t find_first(uint32_t symbol_number);
  uint8_t find_first_UTF8(uint32_t symbol_number);
  uint8_t find_last(uint32_t symbol_number);
  uint8_t find_last_UTF8(uint32_t symbol_number);
  uint8_t add_dictionary_symbol(uint32_t symbol, uint8_t bits);
  void remove_dictionary_symbol(uint32_t symbol, uint8_t bits);
  void add_symbol_to_queue(uint32_t symbol_number);
  void update_queue(uint32_t symbol_number, uint8_t in_definition);
  void update_queue_prior_cap(uint32_t symbol_number, uint8_t in_definition);
  static uint16_t get_mtf_overflow_position(const SymbolData* sd,
                                            const uint32_t* queue,
                                            uint16_t queue_position,
                                            uint32_t num_transmits_over_sqrt2);
  static void add_mtf_hit_scores(SymbolData* sd, uint16_t queue_position,
                                 uint32_t num_transmits_over_sqrt2);
  void encode_dictionary_symbol(uint32_t symbol);
  uint32_t count_symbols(uint32_t symbol);
  void get_embedded_symbols(uint32_t define_symbol);
  void get_embedded_symbols2(uint32_t define_symbol);
  uint8_t embed_define(uint32_t define_symbol, uint8_t in_definition);
  void replace_symbol(uint32_t symbol, uint32_t** symbol2_ptr_ptr,
                      uint32_t* num_more_than_15_inst_definitions_ptr);
};

}  // namespace glza
