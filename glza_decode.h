#pragma once

#include "glza_model.h"
#include "glza_params.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace glza {

class Decoder {
 public:
  struct SymData {
    uint32_t string_index;
    uint32_t string_length;
    union {
      uint32_t four_bytes;
      struct {
        uint8_t type;
        uint8_t repeats;
        uint8_t remaining;
        uint8_t ends;
      } bytes;
    };
  };

  struct BinData {
    uint32_t nsob{};
    uint16_t nbob{};
    uint16_t fbob{};
    uint32_t sym_list_size{};
    std::vector<SymData> symbol_data;
  };

  struct QueueData {
    uint32_t string_index;
    uint32_t string_length;
    union {
      uint32_t four_bytes;
      struct {
        uint8_t type;
        uint8_t repeats;
        uint8_t remaining;
        uint8_t ends;
      } bytes;
    };
    uint8_t starts;
    uint8_t code_length;
  };

  uint8_t* decode(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                  uint8_t* outbuf, FILE* fd_out, const Params& params);

 private:
  DecoderModel model_;

  static constexpr uint32_t kCharsToWrite = 0x40000;
  static constexpr uint32_t kMaxU32 = 0xFFFFFFFF;

  uint64_t* symbol_buffer_write_ptr_{};
  uint64_t* symbol_buffer_end_write_ptr_{};
  std::array<uint64_t, 0x800> symbol_buffer_{};

  uint32_t dictionary_size_{};
  uint32_t outbuf_index_{};
  uint32_t num_base_symbols_{};

  uint16_t out_buffers_sent_{};
  uint16_t queue_size_{};
  uint16_t queue_size_az_{};
  uint16_t queue_size_space_{};
  uint16_t queue_size_other_{};

  uint8_t min_code_length_{};
  uint8_t find_first_symbol_{};
  uint8_t two_threads_{};
  uint8_t prior_type_{};
  uint8_t write_cap_on_{};
  uint8_t write_cap_lock_on_{};
  uint8_t skip_space_on_{};
  uint8_t delta_format_{};
  uint8_t stride_{};
  uint8_t queue_offset_az_{};
  uint8_t queue_offset_space_{};
  uint8_t queue_offset_other_{};

  std::array<uint8_t, 0x100> queue_az_{};
  std::array<uint8_t, 0x100> queue_space_{};
  std::array<uint8_t, 0x100> queue_other_{};
  std::array<uint8_t, 0x100> queue_data_free_list_{};
  std::array<uint8_t, 0x40064> out_char0_{};
  std::array<uint8_t, 0x40064> out_char1_{};

  uint8_t* out_char_ptr_{};
  uint8_t* start_char_ptr_{};
  uint8_t* end_outbuf_{};
  uint8_t* outbuf_{};

  std::vector<uint8_t> symbol_strings_;

  uint8_t UTF8_compliant_{};
  uint8_t cap_encoded_{};
  uint8_t prior_is_cap_{};
  uint8_t prior_end_{};
  uint8_t use_mtf_{};
  uint8_t max_code_length_{};
  uint8_t cap_symbol_defined_{};
  uint8_t cap_lock_symbol_defined_{};
  uint8_t max_regular_code_length_{};
  uint8_t queue_offset_{};

  std::array<uint8_t, 0x100> symbol_lengths_{};
  std::array<uint8_t, 0x100> bin_code_length_{};
  std::array<uint8_t, 0x100> queue_{};
  std::array<uint8_t, 15> queue_miss_code_length_{};

  std::atomic<uint8_t> done_parsing_{0};
  std::array<std::atomic<uint8_t>, 2> symbol_buffer_owner_{};

  FILE* fd_{};

  std::array<uint16_t, 0x100> sum_nbob_{};
  std::array<std::array<uint8_t, 0x1000>, 0x100> lookup_bits_{};
  // Indexed by code length, up to bin_data_[c][max_code_length_ + 1] (an
  // overflow/sentinel bin). max_code_length_ = (inbuf[1] & 0x1F) + 1 can be as
  // large as 32, so the inner dimension must cover index 33. The original C used
  // a flat [0x100][26] array where the +1 overflow wrapped into the next row's
  // unused [0] slot; std::array bounds-checks that, so size it for the real max.
  std::array<std::array<BinData, 34>, 0x100> bin_data_{};
  std::array<QueueData, 0x100> queue_data_{};

  SymData* add_dictionary_symbol(uint8_t bits, uint8_t first_char);
  SymData* add_single_dictionary_symbol(uint8_t first_char);
  void remove_dictionary_symbol(BinData& bin_info, uint32_t index);

  void decode_queue_fail(const char* reason);
  void decode_queue_subcount_inc(uint16_t* subcount, const char* which);
  void decode_queue_subcount_dec(uint16_t* subcount, const char* which);
  int decode_dict_write_ok(uint32_t end_index, uint32_t add_bytes, const char* where);
  int decode_dict_ref_ok(uint32_t index, uint32_t length, const char* where);
  int decode_lookup_ok(uint8_t first_char, uint16_t bin_num, const char* where);
  int decode_lookup_store(uint8_t first_char, uint16_t bin, uint8_t bits, const char* where);
  uint8_t decode_lookup_load(uint8_t first_char, uint16_t bin_num, const char* where);
  int decode_append_byte(uint32_t* end_ptr, uint8_t byte, const char* where);
  int decode_append_ref(uint32_t* end_ptr, uint32_t src_index, uint32_t length, const char* where);
  int decode_append_sym(uint32_t* end_ptr, SymData* sym, uint8_t len1_byte, const char* where);
  int decode_dict_index_ok(uint8_t first_char, uint8_t code_length, uint32_t index, const char* where);
  int decode_dict_fetch(uint8_t first_char, uint16_t bin_num, uint8_t* code_length_out,
                        uint32_t* index_out, SymData** sym_out, const char* where);

  QueueData* add_symbol_to_queue(SymData* sym_data_ptr, uint8_t code_length, uint8_t first_char);
  QueueData* add_symbol_to_queue_cap_encoded(SymData* sym_data_ptr, uint8_t code_length, uint8_t first_char);
  SymData* update_queue(uint8_t queue_position);
  SymData* update_az_queue(uint8_t queue_position);
  SymData* update_space_queue(uint8_t queue_position);
  SymData* update_other_queue(uint8_t queue_position);

  uint32_t get_dictionary_index(uint16_t bin_num, uint8_t code_length, uint8_t first_char);

  void delta_transform(uint8_t* buffer, uint32_t len);
  uint8_t create_extended_UTF8_symbol(uint32_t base_symbol, uint32_t* string_index_ptr);
  uint8_t get_first_char(uint32_t index);

  SymData* decode_new(uint32_t* string_index_ptr);
  SymData* decode_new_cap_encoded(uint32_t* string_index_ptr);

  void transpose2(uint8_t* buffer, uint32_t len);
  void transpose4(uint8_t* buffer, uint32_t len);

  void write_output_buffer();
  void write_output_buffer_delta();

  void write_string(uint8_t*& ssp, uint32_t len);
  void write_string_cap_encoded(uint8_t*& ssp, uint32_t len,
                                uint8_t& wco, uint8_t& wclo, uint8_t& sso);
  void write_string_delta(uint8_t*& ssp, uint32_t len);

  void write_single_threaded_output();
  void write_symbol_buffer(uint8_t* buffer_number_ptr);
  void write_output_thread(uint8_t* outbuf_arg);
};

}  // namespace glza
