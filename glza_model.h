#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace glza {

inline constexpr uint32_t kTop = uint32_t{1} << 24;
inline constexpr uint32_t kBufSize = 0x40000;
inline constexpr uint16_t kUpFreqSymType = 1;
inline constexpr uint16_t kFreqSymTypeBot1 = 0x4000;
inline constexpr uint16_t kFreqSymTypeBot2 = 0x2000;
inline constexpr uint16_t kFreqSymTypeBot3 = 0x1000;
inline constexpr uint16_t kUpFreqMtfPos = 4;
inline constexpr uint16_t kFreqMtfPosBot = 0x2000;
inline constexpr uint16_t kUpFreqSid = 3;
inline constexpr uint16_t kFreqSidBot = 0x1000;
inline constexpr uint16_t kUpFreqInst = 8;
inline constexpr uint16_t kFreqInstBot = 0x8000;
inline constexpr uint16_t kFreqErgBot = 0x2000;
inline constexpr uint16_t kFreqGoMtfBot = 0x2000;
inline constexpr uint16_t kFreqWordTagBot = 0x1000;
inline constexpr uint16_t kUpFreqFirstChar = 8;
inline constexpr uint16_t kFreqFirstCharBot = 0x4000;
inline constexpr uint8_t kNotCap = 0;
inline constexpr uint8_t kCap = 1;
inline constexpr uint8_t kLevel0 = 0;
inline constexpr uint8_t kLevel0Cap = 1;
inline constexpr uint8_t kLevel1 = 2;
inline constexpr uint8_t kLevel1Cap = 3;

inline constexpr uint32_t kStartUtf8_2Byte = 0x80;
inline constexpr uint32_t kStartUtf8_3Byte = 0x800;
inline constexpr uint32_t kStartUtf8_4Byte = 0x10000;
inline constexpr uint32_t kMaxInstancesForRemove = 15;

struct FirstCharData {
  union {
    uint32_t all_data;
    struct {
      uint16_t freq;
      uint8_t symbol;
    } data;
  };
};

class ArithmeticModel {
 protected:
  uint32_t low_{};
  uint32_t range_{0xFFFFFFFF};
  uint32_t count_{};
  uint32_t range_low_{};
  uint32_t range_high_{};
  uint32_t in_char_num_{};
  uint32_t out_char_num_{};

  uint16_t last_queue_size_az_{};
  uint16_t last_queue_size_space_{};
  uint16_t last_queue_size_other_{};
  uint16_t rescale_queue_size_az_{};
  uint16_t rescale_queue_size_space_{};
  uint16_t rescale_queue_size_other_{};
  uint16_t unused_queue_freq_az_{};
  uint16_t unused_queue_freq_space_{};
  uint16_t unused_queue_freq_other_{};

  std::array<uint16_t, 2> range_scale_sid_{};
  std::array<std::array<uint16_t, 16>, 2> freq_sid_{};
  std::array<std::array<uint16_t, 16>, 2> range_scale_inst_{};
  std::array<std::array<std::array<uint16_t, 38>, 16>, 2> freq_inst_{};
  std::array<uint16_t, 0x100> freq_word_tag_{};
  std::array<uint16_t, 341> freq_erg_{};
  std::array<uint16_t, 0x5A0> freq_go_mtf_{};
  std::array<uint16_t, 3> range_scale_mtf_pos_{};
  std::array<std::array<uint16_t, 0x100>, 3> freq_mtf_pos_{};
  std::array<std::array<uint16_t, 2>, 0x34> freq_sym_type_prior_type_{};
  std::array<std::array<uint16_t, 2>, 0x100> freq_sym_type_prior_end_{};
  std::array<std::array<uint16_t, 3>, 2> freq_mtf_first_{};
  std::array<std::array<uint16_t, 7>, 0x100> range_scale_first_char_section_{};
  std::array<std::array<uint16_t, 0x100>, 4> range_scale_first_char_{};
  std::array<std::array<std::array<FirstCharData, 0x100>, 0x100>, 4>
      first_char_data_{};

  uint8_t cap_encoded_{};
  uint8_t utf8_compliant_{};
  uint8_t max_base_code_{};
  uint8_t max_inst_code_{};
  uint8_t cap_initialized_{};
  uint8_t cap_lock_initialized_{};
  uint8_t num_base_symbols_{};

  void start_model_sym_type(uint8_t use_mtf, uint8_t cap_encoded);
  void start_model_mtf_first();
  void start_model_mtf_pos();
  void start_model_sid();
  void start_model_inst(uint8_t num_inst_codes);
  void start_model_erg();
  void start_model_go_mtf();
  void start_model_word_tag();
  void start_model_first_char();
  void start_model_first_char_binary();

  void rescale_mtf_queue_pos(uint8_t context);
  void rescale_sid(uint8_t context);
  void rescale_inst(uint8_t context, uint8_t sid_symbol);
  void rescale_first_char(uint8_t sym_type, uint8_t prior_end);
  void rescale_first_char_binary(uint8_t prior_end);

 public:
  virtual ~ArithmeticModel() = default;

  void init_first_char(uint8_t first_char, uint8_t code_length);
  void init_first_char_binary(uint8_t first_char, uint8_t code_length);
  void init_prior_end(uint8_t prior_end, uint8_t* symbol_lengths);
  void init_prior_end_binary(uint8_t prior_end, uint8_t* code_length);
  void init_base_symbol_cap(uint8_t base_symbol, uint8_t* symbol_lengths);

  [[nodiscard]] uint32_t read_low() const { return low_; }
  [[nodiscard]] uint32_t read_range() const { return range_; }
};

class EncoderModel : public ArithmeticModel {
  uint8_t* out_buffer_{};
  size_t out_buffer_size_{};
  uint8_t encoder_failed_{};

  void encoder_fail(const char* reason);
  void normalize_encoder(uint32_t bot);

 public:
  void init_encoder(uint8_t max_base_code, uint8_t num_inst_codes,
                    uint8_t cap_encoded, uint8_t utf8_compliant,
                    uint8_t use_mtf, uint8_t* bufptr);
  void finish_encoder();

  void increase_range(uint32_t low_ranges, uint32_t ranges);
  void double_range(uint8_t low_ranges);
  void write_out_buffer(uint8_t value);
  void set_out_buffer_capacity(size_t size);
  void write_in_char_num(uint32_t value) { in_char_num_ = value; }
  [[nodiscard]] uint32_t read_out_char_num() const { return out_char_num_; }
  [[nodiscard]] uint8_t read_encoder_failed() const { return encoder_failed_; }
  void set_encoder_failed(const char* reason) { encoder_fail(reason); }
  void reset_codec_globals();

  void encode_dict_type_binary(uint8_t ctx1, uint8_t ctx2, uint16_t queue_size);
  void encode_dict_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3,
                        uint16_t queue_size);
  void encode_new_type_binary(uint8_t ctx1, uint8_t ctx2, uint16_t queue_size);
  void encode_new_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3,
                       uint16_t queue_size);
  void encode_mtf_type_binary(uint8_t ctx1, uint8_t ctx2);
  void encode_mtf_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3);
  void encode_mtf_first(uint8_t context, uint8_t first, uint16_t qs_other,
                        uint16_t qs_space, uint16_t qs_az);
  void encode_mtf_pos(uint8_t position, uint16_t queue_size);
  void encode_mtf_pos_az(uint8_t position, uint16_t queue_size);
  void encode_mtf_pos_space(uint8_t position, uint16_t queue_size);
  void encode_mtf_pos_other(uint8_t position, uint16_t queue_size);
  void encode_sid(uint8_t context, uint8_t sid_symbol);
  void encode_extra_sid(uint32_t extra_symbols);
  void encode_inst(uint8_t context, uint8_t sid_symbol, uint8_t symbol);
  void encode_erg(uint16_t ctx1, uint16_t ctx2, uint8_t symbol);
  void encode_go_mtf(uint16_t ctx1, uint8_t ctx2, uint8_t symbol);
  void encode_word_tag(uint8_t symbol, uint8_t context);
  void encode_short_dictionary_symbol(uint16_t bin_num, uint16_t dict_bins,
                                      uint16_t code_bins);
  void encode_long_dictionary_symbol(uint32_t bin_code, uint16_t bin_num,
                                     uint16_t dict_bins, uint8_t code_length,
                                     uint16_t code_bins);
  void encode_base_symbol(uint32_t base_symbol, uint32_t num_base_symbols,
                          uint32_t norm_base_symbols);
  void encode_first_char(uint8_t first_char, uint8_t sym_type,
                         uint8_t last_char);
  void encode_first_char_binary(uint8_t first_char, uint8_t last_char);

 private:
  bool check_mtf_pos(const char* where, uint8_t position, uint16_t queue_size);
};

class DecoderModel : public ArithmeticModel {
  uint8_t* in_buffer_{};
  uint32_t code_{};
  uint8_t decoder_failed_{};
  uint32_t in_size_dbg_{};

  void decoder_fail(const char* reason);
  void normalize_decoder(uint32_t bot);

 public:
  void set_in_size_dbg(uint32_t n) { in_size_dbg_ = n; }
  void init_decoder(uint8_t max_base_code, uint8_t num_inst_codes,
                    uint8_t cap_encoded, uint8_t utf8_compliant,
                    uint8_t use_mtf, uint8_t* inbuf);

  [[nodiscard]] uint8_t read_decoder_failed() const { return decoder_failed_; }
  void set_decoder_failed(const char* reason) { decoder_fail(reason); }
  void reset_codec_globals();

  uint8_t decode_sym_type_binary(uint8_t ctx1, uint8_t ctx2,
                                 uint16_t queue_size);
  uint8_t decode_sym_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3,
                          uint16_t queue_size);
  uint8_t decode_mtf_first(uint8_t context, uint16_t qs_other,
                           uint16_t qs_space, uint16_t qs_az);
  uint8_t decode_mtf_pos(uint16_t queue_size);
  uint8_t decode_mtf_pos_az(uint16_t queue_size);
  uint8_t decode_mtf_pos_space(uint16_t queue_size);
  uint8_t decode_mtf_pos_other(uint16_t queue_size);
  uint8_t decode_sid(uint8_t context);
  uint32_t decode_extra_sid();
  uint8_t decode_inst(uint8_t context, uint8_t sid_symbol);
  uint8_t decode_erg(uint16_t ctx1, uint16_t ctx2);
  uint8_t decode_go_mtf(uint16_t ctx1, uint8_t ctx2);
  uint8_t decode_word_tag(uint8_t context);
  uint16_t decode_bin(uint16_t bins);
  uint32_t decode_bin_code(uint8_t bits);
  uint32_t decode_base_symbol(uint32_t num_base_symbols);
  uint32_t decode_base_symbol_cap(uint32_t num_base_symbols);
  uint8_t decode_first_char(uint8_t sym_type, uint8_t last_char);
  uint8_t decode_first_char_binary(uint8_t last_char);

  void increase_range(uint32_t low_ranges, uint32_t ranges) {
    if (decoder_failed_ != 0) return;
    low_ -= range_ * low_ranges;
    range_ *= ranges;
  }

  void double_range(uint8_t low_ranges) {
    if (decoder_failed_ != 0) return;
    low_ -= range_ * static_cast<uint32_t>(low_ranges);
    range_ *= 2;
  }

  void write_in_char_num(uint32_t value) { in_char_num_ = value; }
  [[nodiscard]] uint32_t read_in_char_num() const { return in_char_num_; }
};

}  // namespace glza
