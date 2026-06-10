#include "glza_model.h"

#include <cstdio>

namespace glza {

#ifdef GLZA_OVERREAD_DIAG
long glza_norm_ct = 0;
uint32_t glza_ring_low[1024], glza_ring_range[1024], glza_ring_code[1024];
#endif

// ---------------------------------------------------------------------------
// ArithmeticModel -- protected model-init helpers
// ---------------------------------------------------------------------------

void ArithmeticModel::start_model_sym_type(uint8_t use_mtf, uint8_t cap_encoded) {
  if (cap_encoded == 0) {
    uint8_t i = 1;
    do {
      if (use_mtf != 0)
        freq_sym_type_prior_type_[i][0] = 0x1C00;
      else
        freq_sym_type_prior_type_[i][0] = 0x2000;
      freq_sym_type_prior_type_[i][1] = 0x2000;
    } while (i-- != 0);
  } else {
    uint8_t i = 0x33;
    do {
      if (use_mtf != 0)
        freq_sym_type_prior_type_[i][0] = 0x16C0;
      else
        freq_sym_type_prior_type_[i][0] = 0x1A00;
      freq_sym_type_prior_type_[i][1] = 0x1A00;
    } while (i-- != 4);
    do {
      if (use_mtf != 0)
        freq_sym_type_prior_type_[i][0] = 0x1340;
      else
        freq_sym_type_prior_type_[i][0] = 0x1600;
      freq_sym_type_prior_type_[i][1] = 0x1600;
    } while (i-- != 0);
    i = 0xFF;
    do {
      if (use_mtf != 0)
        freq_sym_type_prior_end_[i][0] = 0xE00;
      else
        freq_sym_type_prior_end_[i][0] = 0x1000;
      freq_sym_type_prior_end_[i][1] = 0x1000;
    } while (i-- != 0);
  }
}

void ArithmeticModel::start_model_mtf_first() {
  freq_mtf_first_[0][0] = 0x900;
  freq_mtf_first_[0][1] = 0x500;
  freq_mtf_first_[0][2] = 0x200;
  freq_mtf_first_[1][0] = 0x100;
  freq_mtf_first_[1][1] = 0xE00;
  freq_mtf_first_[1][2] = 0x100;
}

void ArithmeticModel::start_model_mtf_pos() {
  range_scale_mtf_pos_[0] = 0;
  uint16_t j = 0;
  do {
    freq_mtf_pos_[0][j] = freq_mtf_pos_[1][j] = freq_mtf_pos_[2][j] = 0x200 / (j + 2);
    range_scale_mtf_pos_[0] += freq_mtf_pos_[0][j];
  } while (++j != 0x100);
  range_scale_mtf_pos_[1] = range_scale_mtf_pos_[2] = range_scale_mtf_pos_[0];
  unused_queue_freq_az_ = unused_queue_freq_space_ = unused_queue_freq_other_ = range_scale_mtf_pos_[0];
  last_queue_size_az_ = last_queue_size_space_ = last_queue_size_other_ = 0;
  rescale_queue_size_az_ = rescale_queue_size_space_ = rescale_queue_size_other_ = 0;
}

void ArithmeticModel::start_model_sid() {
  uint8_t i = 1;
  do {
    uint8_t j = 15;
    do {
      freq_sid_[i][j] = 1;
    } while (j-- != 8);
    do {
      freq_sid_[i][j] = 2;
    } while (j-- != 4);
    freq_sid_[i][3] = 4;
    freq_sid_[i][2] = 6;
    freq_sid_[i][1] = 8;
    freq_sid_[i][0] = 4;
    range_scale_sid_[i] = 0;
    j = 15;
    do {
      range_scale_sid_[i] += freq_sid_[i][j];
    } while (j-- != 0);
  } while (i-- != 0);
}

void ArithmeticModel::start_model_inst(uint8_t num_inst_codes) {
  uint8_t i = 1;
  do {
    uint8_t j = 15;
    do {
      uint8_t k = num_inst_codes;
      if (j != 0)
        k--;
      range_scale_inst_[i][j] = k--;
      do {
        freq_inst_[i][j][k] = 1;
      } while (k-- != 0);
    } while (j-- != 0);
  } while (i-- != 0);
}

void ArithmeticModel::start_model_erg() {
  uint16_t i = 340;
  do {
    freq_erg_[i] = 0x600;
  } while (i-- != 240);
  do {
    freq_erg_[i] = 0x800;
  } while (i-- != 1);
  freq_erg_[i] = 0x200;
}

void ArithmeticModel::start_model_go_mtf() {
  uint16_t i = 0x59F;
  do {
    freq_go_mtf_[i] = 0x555;
  } while (i-- != 0);
}

void ArithmeticModel::start_model_word_tag() {
  uint8_t i = 0xFF;
  do {
    freq_word_tag_[i] = 0x800;
  } while (i-- != 0);
}

void ArithmeticModel::start_model_first_char() {
  uint8_t i = 0xFF;
  do {
    uint8_t j = 0xFF;
    do {
      first_char_data_[0][i][j].data.freq = 0;
      first_char_data_[1][i][j].data.freq = 0;
      first_char_data_[2][i][j].data.freq = 0;
      first_char_data_[3][i][j].data.freq = 0;
    } while (j-- != 0);
    range_scale_first_char_[0][i] = 0;
    range_scale_first_char_[1][i] = 0;
    range_scale_first_char_[2][i] = 0;
    range_scale_first_char_[3][i] = 0;
  } while (i-- != 0);
}

void ArithmeticModel::start_model_first_char_binary() {
  uint8_t i = 0xFF;
  do {
    uint8_t j = 0xFF;
    do {
      first_char_data_[0][i][j].data.freq = 0;
    } while (j-- != 0);
    j = 6;
    do {
      range_scale_first_char_section_[i][j] = 0;
    } while (j-- != 0);
    range_scale_first_char_[0][i] = 0;
  } while (i-- != 0);
}

// ---------------------------------------------------------------------------
// ArithmeticModel -- protected rescale helpers
// ---------------------------------------------------------------------------

void ArithmeticModel::rescale_mtf_queue_pos(uint8_t context) {
  uint8_t i = 0xFF;
  if (context == 0) {
    range_scale_mtf_pos_[0] = 0;
    if (last_queue_size_other_ != 0x100)
      do {
        range_scale_mtf_pos_[0] += freq_mtf_pos_[0][i] = (freq_mtf_pos_[0][i] + 1) >> 1;
      } while (i-- != last_queue_size_other_);
    unused_queue_freq_other_ = range_scale_mtf_pos_[0];
    do {
      range_scale_mtf_pos_[0] += freq_mtf_pos_[0][i] = (freq_mtf_pos_[0][i] + 1) >> 1;
    } while (i-- != 0);
    rescale_queue_size_other_ = last_queue_size_other_;
  } else if (context == 1) {
    range_scale_mtf_pos_[1] = 0;
    if (last_queue_size_space_ != 0x100)
      do {
        range_scale_mtf_pos_[1] += freq_mtf_pos_[1][i] = (freq_mtf_pos_[1][i] + 1) >> 1;
      } while (i-- != last_queue_size_space_);
    unused_queue_freq_space_ = range_scale_mtf_pos_[1];
    do {
      range_scale_mtf_pos_[1] += freq_mtf_pos_[1][i] = (freq_mtf_pos_[1][i] + 1) >> 1;
    } while (i-- != 0);
    rescale_queue_size_space_ = last_queue_size_space_;
  } else if (context == 2) {
    range_scale_mtf_pos_[2] = 0;
    if (last_queue_size_az_ != 0x100)
      do {
        range_scale_mtf_pos_[2] += freq_mtf_pos_[2][i] = (freq_mtf_pos_[2][i] + 1) >> 1;
      } while (i-- != last_queue_size_az_);
    unused_queue_freq_az_ = range_scale_mtf_pos_[2];
    do {
      range_scale_mtf_pos_[2] += freq_mtf_pos_[2][i] = (freq_mtf_pos_[2][i] + 1) >> 1;
    } while (i-- != 0);
    rescale_queue_size_az_ = last_queue_size_az_;
  }
}

void ArithmeticModel::rescale_sid(uint8_t context) {
  uint8_t i = 14;
  range_scale_sid_[context] = freq_sid_[context][15] = (freq_sid_[context][15] + 1) >> 1;
  do {
    range_scale_sid_[context] += freq_sid_[context][i] = (freq_sid_[context][i] + 1) >> 1;
  } while (i-- != 0);
}

void ArithmeticModel::rescale_inst(uint8_t context, uint8_t sid_symbol) {
  range_scale_inst_[context][sid_symbol] = 0;
  uint8_t i = max_inst_code_;
  do {
    range_scale_inst_[context][sid_symbol] += freq_inst_[context][sid_symbol][i]
        = (freq_inst_[context][sid_symbol][i] + 1) >> 1;
  } while (i-- != 0);
}

void ArithmeticModel::rescale_first_char(uint8_t sym_type, uint8_t prior_end) {
  uint8_t i = max_base_code_;
  range_scale_first_char_[sym_type][prior_end] = 0;
  do {
    range_scale_first_char_[sym_type][prior_end] += first_char_data_[sym_type][prior_end][i].data.freq
        = (first_char_data_[sym_type][prior_end][i].data.freq + 1) >> 1;
  } while (i-- != 0);
}

void ArithmeticModel::rescale_first_char_binary(uint8_t prior_end) {
  range_scale_first_char_[0][prior_end] = first_char_data_[0][prior_end][0].data.freq
      = (first_char_data_[0][prior_end][0].data.freq + 1) >> 1;
  uint8_t i = 1;
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0x20);
  range_scale_first_char_section_[prior_end][0] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0x40);
  range_scale_first_char_section_[prior_end][1] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0x60);
  range_scale_first_char_section_[prior_end][2] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0x80);
  range_scale_first_char_section_[prior_end][3] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0xA0);
  range_scale_first_char_section_[prior_end][4] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0xC0);
  range_scale_first_char_section_[prior_end][5] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0xE0);
  range_scale_first_char_section_[prior_end][6] = range_scale_first_char_[0][prior_end];
  do {
    range_scale_first_char_[0][prior_end] += first_char_data_[0][prior_end][i].data.freq
        = (first_char_data_[0][prior_end][i].data.freq + 1) >> 1;
  } while (++i != 0);
  range_scale_first_char_section_[prior_end][6] -= range_scale_first_char_section_[prior_end][5];
  range_scale_first_char_section_[prior_end][5] -= range_scale_first_char_section_[prior_end][4];
  range_scale_first_char_section_[prior_end][4] -= range_scale_first_char_section_[prior_end][3];
  range_scale_first_char_section_[prior_end][3] -= range_scale_first_char_section_[prior_end][2];
  range_scale_first_char_section_[prior_end][2] -= range_scale_first_char_section_[prior_end][1];
  range_scale_first_char_section_[prior_end][1] -= range_scale_first_char_section_[prior_end][0];
}

// ---------------------------------------------------------------------------
// ArithmeticModel -- public init helpers
// ---------------------------------------------------------------------------

void ArithmeticModel::init_first_char(uint8_t first_char, uint8_t code_length) {
  uint8_t freq;
  uint8_t either_cap_initialized = cap_initialized_ || cap_lock_initialized_;
  uint8_t max_index;

  if (utf8_compliant_ != 0)
    max_index = 0x90;
  else
    max_index = 0xFF;

  uint8_t i = 3;
  do {
    uint8_t j = max_index;
    do {
      first_char_data_[i][j][num_base_symbols_].data.symbol = first_char;
    } while (j-- != 0);
  } while (i-- != 0);

  if (code_length < 8)
    freq = 1 << (8 - code_length);
  else
    freq = 1;

  i = max_index;
  do {
    if ((cap_encoded_ == 0) || ((i > 'Z') || (i < 'A') || ((i == 'C') && ((first_char >= 'a') && (first_char <= 'z'))))) {
      if ((range_scale_first_char_[0][i] != 0) || ((i == 'C') && (either_cap_initialized != 0))) {
        uint8_t k;
        for (k = 0; k < 4; k++) {
          first_char_data_[k][i][num_base_symbols_].data.freq = freq;
          range_scale_first_char_[k][i] += freq;
          if (range_scale_first_char_[k][i] > kFreqFirstCharBot)
            rescale_first_char(k, i);
        }
      }
    }
  } while (i-- != 0);
}

void ArithmeticModel::init_first_char_binary(uint8_t first_char, uint8_t code_length) {
  uint8_t freq;

  if (code_length < 8)
    freq = 1 << (8 - code_length);
  else
    freq = 1;
  uint8_t i = 0xFF;
  do {
    if (range_scale_first_char_[0][i] != 0) {
      first_char_data_[0][i][first_char].data.freq = freq;
      range_scale_first_char_[0][i] += freq;
      if (first_char < 0xE0)
        range_scale_first_char_section_[i][first_char >> 5] += freq;
      if (range_scale_first_char_[0][i] > kFreqFirstCharBot)
        rescale_first_char_binary(i);
    }
  } while (i-- != 0);
}

void ArithmeticModel::init_prior_end(uint8_t prior_end, uint8_t* symbol_lengths) {
  uint8_t freq, code_length;

  uint8_t k = 3;
  uint8_t prior_end_is_a_to_z = (prior_end >= 'a') && (prior_end <= 'z');
  (void)prior_end_is_a_to_z;
  do {
    uint8_t i = num_base_symbols_;
    do {
      uint8_t symbol_i = first_char_data_[k][prior_end][i].data.symbol;
      if (((cap_encoded_ != 0)
            && (((symbol_i == 'C') && (cap_initialized_ != 0))
              || ((symbol_i == 'B') && (cap_lock_initialized_ != 0))
              || ((symbol_i & 0xFE) != 0x42)))
          || (cap_encoded_ == 0)) {
        if (((cap_encoded_ != 0) && ((prior_end != 'C') || ((prior_end == 'C')
              && (symbol_i >= 'a') && (symbol_i <= 'z'))))
            || (cap_encoded_ == 0)) {
          code_length = symbol_lengths[symbol_i];
          if (code_length < 8)
            freq = 1 << (8 - code_length);
          else
            freq = 1;
          first_char_data_[k][prior_end][i].data.freq = freq;
          range_scale_first_char_[k][prior_end] += freq;
        }
      }
    } while (i-- != 0);
  } while (k-- != 0);
  num_base_symbols_++;
}

void ArithmeticModel::init_prior_end_binary(uint8_t prior_end, uint8_t* code_length) {
  uint8_t freq;
  uint8_t first_char = 0xFF;
  do {
    if (code_length[first_char] < 8)
      freq = 1 << (8 - code_length[first_char]);
    else
      freq = 1;
    if ((range_scale_first_char_[0][first_char] != 0) || (first_char == prior_end)) {
      first_char_data_[0][prior_end][first_char].data.freq = freq;
      range_scale_first_char_[0][prior_end] += freq;
      if (first_char < 0xE0)
        range_scale_first_char_section_[prior_end][first_char >> 5] += freq;
    }
  } while (first_char-- != 0);
}

void ArithmeticModel::init_base_symbol_cap(uint8_t base_symbol, uint8_t* symbol_lengths) {
  init_first_char(base_symbol, symbol_lengths[base_symbol]);
  if ((base_symbol & 0xFE) == 0x42) {
    if (base_symbol == 'C')
      cap_initialized_ = 1;
    else
      cap_lock_initialized_ = 1;
    if (cap_initialized_ + cap_lock_initialized_ == 1)
      init_prior_end('C', symbol_lengths);
    else
      num_base_symbols_++;
  } else
    init_prior_end(base_symbol, symbol_lengths);
}

// ---------------------------------------------------------------------------
// EncoderModel -- private helpers
// ---------------------------------------------------------------------------

void EncoderModel::encoder_fail(const char* reason) {
  if (encoder_failed_ == 0) {
    std::fprintf(stderr,
        "GLZA encode: %s (OutCharNum=%u OutBufferSize=%zu low=0x%08x range=0x%08x)\n",
        reason, static_cast<unsigned>(out_char_num_), out_buffer_size_, low_, range_);
    encoder_failed_ = 1;
  }
}

void EncoderModel::normalize_encoder(uint32_t bot) {
#ifdef GLZA_OVERREAD_DIAG
  {
    extern long glza_norm_ct;
    static long target = -2;
    if (target == -2) { const char* e = getenv("GLZA_TRACE_OP"); target = e ? atol(e) : -1; }
    const long ct = glza_norm_ct++;
    if (target >= 0 && ct >= target - 24 && ct <= target + 5)
      fprintf(stderr, "  ENC op=%ld low=%08x range=%08x bot=%x\n", ct, low_, range_, bot);
  }
#endif
  uint32_t normalize_steps = 0;
  while ((low_ ^ (low_ + range_)) < kTop || (range_ < bot && ((range_ = -low_ & (bot - 1)), 1))) {
    if (encoder_failed_ != 0)
      return;
    if (out_buffer_size_ != 0 && out_char_num_ + 1 > out_buffer_size_) {
      encoder_fail("output buffer overflow in NormalizeEncoder");
      return;
    }
    if (++normalize_steps > 64) {
      encoder_fail("arithmetic encoder normalization exceeded 64 steps (corrupt low/range?)");
      return;
    }
    out_buffer_[out_char_num_++] = static_cast<uint8_t>(low_ >> 24);
    range_ <<= 8;
    low_ <<= 8;
  }
}

bool EncoderModel::check_mtf_pos(const char* where, uint8_t position, uint16_t queue_size) {
  if (queue_size > 0xFF) {
    std::fprintf(stderr,
        "GLZA encode: MTF queue size %u exceeds FreqMtfPos table limit 255 in %s (position=%u)\n",
        static_cast<unsigned>(queue_size), where, static_cast<unsigned>(position));
    encoder_failed_ = 1;
    return false;
  }
  if (queue_size != 0 && position >= queue_size) {
    std::fprintf(stderr,
        "GLZA encode: MTF position %u >= queue size %u in %s\n",
        static_cast<unsigned>(position), static_cast<unsigned>(queue_size), where);
    encoder_failed_ = 1;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// EncoderModel -- public methods
// ---------------------------------------------------------------------------

void EncoderModel::reset_codec_globals() {
  in_char_num_ = 0;
  out_char_num_ = 0;
  out_buffer_ = nullptr;
  out_buffer_size_ = 0;
  encoder_failed_ = 0;
}

void EncoderModel::init_encoder(uint8_t max_base_code, uint8_t num_inst_codes,
    uint8_t cap_encoded, uint8_t utf8_compliant, uint8_t use_mtf, uint8_t* bufptr) {
  cap_initialized_ = 0;
  cap_lock_initialized_ = 0;
  cap_encoded_ = cap_encoded;
  utf8_compliant_ = utf8_compliant;
  max_base_code_ = max_base_code;
  max_inst_code_ = num_inst_codes - 1;
  num_base_symbols_ = 0;
  out_buffer_ = bufptr;
  out_char_num_ = 0;
  out_buffer_size_ = 0;
  encoder_failed_ = 0;
  low_ = 0;
  range_ = 0xFFFFFFFF;
  start_model_sym_type(use_mtf, cap_encoded);
  start_model_mtf_first();
  start_model_mtf_pos();
  start_model_sid();
  start_model_inst(num_inst_codes);
  start_model_erg();
  start_model_go_mtf();
  start_model_word_tag();
  if (cap_encoded || utf8_compliant)
    start_model_first_char();
  else
    start_model_first_char_binary();

  if (utf8_compliant != 0) {
    uint8_t i = 0x90;
    uint8_t j = 0x90;
    (void)i; (void)j;
  } else {
    uint8_t i = 0xFF;
    uint8_t j = 0xFF;
    (void)i; (void)j;
  }
}

void EncoderModel::finish_encoder() {
  if (encoder_failed_ != 0)
    return;
  if (out_buffer_size_ != 0 && out_char_num_ + 4 > out_buffer_size_) {
    encoder_fail("output buffer overflow in FinishEncoder");
    return;
  }
  out_buffer_[out_char_num_++] = static_cast<uint8_t>(low_ >> 24);
  out_buffer_[out_char_num_++] = static_cast<uint8_t>(low_ >> 16);
  out_buffer_[out_char_num_++] = static_cast<uint8_t>(low_ >> 8);
  out_buffer_[out_char_num_++] = static_cast<uint8_t>(low_);
}

void EncoderModel::increase_range(uint32_t low_ranges, uint32_t ranges) {
  if (encoder_failed_ != 0)
    return;
  low_ -= range_ * low_ranges;
  range_ *= ranges;
}

void EncoderModel::double_range(uint8_t low_ranges) {
  if (encoder_failed_ != 0)
    return;
  low_ -= range_ * low_ranges;
  range_ *= 2;
}

void EncoderModel::write_out_buffer(uint8_t value) {
  if (encoder_failed_ != 0)
    return;
  if (out_buffer_size_ != 0 && out_char_num_ + 1 > out_buffer_size_) {
    encoder_fail("output buffer overflow in WriteOutBuffer");
    return;
  }
  out_buffer_[out_char_num_++] = value;
}

void EncoderModel::set_out_buffer_capacity(size_t size) {
  out_buffer_size_ = size;
}

// ---------------------------------------------------------------------------
// EncoderModel -- symbol type encoding
// ---------------------------------------------------------------------------

void EncoderModel::encode_dict_type_binary(uint8_t ctx1, uint8_t ctx2, uint16_t queue_size) {
  normalize_encoder(kFreqSymTypeBot1);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (kFreqSymTypeBot1 - 1);
  if (queue_size != 0)
    range_ = freq_sym_type_prior_type_[ctx1][0] * (range_ >> 14) + extra_range;
  else
    range_ = (kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][1]) * (range_ >> 14) + extra_range;
  uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 6;
  freq_sym_type_prior_type_[ctx1][0] += delta + ((kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 6);
  freq_sym_type_prior_type_[ctx1][1] -= delta;
}

void EncoderModel::encode_dict_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3, uint16_t queue_size) {
  normalize_encoder(8 * kFreqSymTypeBot3);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (8 * kFreqSymTypeBot3 - 1);
  if (queue_size != 0)
    range_ = (freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx2][0] + freq_sym_type_prior_end_[ctx3][0]) * (range_ >> 15) + extra_range;
  else
    range_ = (0x8000 - freq_sym_type_prior_type_[ctx1][1] - freq_sym_type_prior_type_[ctx2][1] - freq_sym_type_prior_end_[ctx3][1]) * (range_ >> 15) + extra_range;
  uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 4;
  freq_sym_type_prior_type_[ctx1][0] += delta + ((0x2C00 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 4);
  freq_sym_type_prior_type_[ctx1][1] -= delta;
  delta = freq_sym_type_prior_type_[ctx2][1] >> 7;
  freq_sym_type_prior_type_[ctx2][0] += delta + ((0x3400 - freq_sym_type_prior_type_[ctx2][0] - freq_sym_type_prior_type_[ctx2][1]) >> 7);
  freq_sym_type_prior_type_[ctx2][1] -= delta;
  delta = freq_sym_type_prior_end_[ctx3][1] >> 4;
  freq_sym_type_prior_end_[ctx3][0] += delta + ((0x2000 - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1]) >> 4);
  freq_sym_type_prior_end_[ctx3][1] -= delta;
}

void EncoderModel::encode_new_type_binary(uint8_t ctx1, uint8_t ctx2, uint16_t queue_size) {
  normalize_encoder(kFreqSymTypeBot1);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (kFreqSymTypeBot1 - 1);
  const uint16_t freq_mtf = kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1];
  if (queue_size != 0)
    low_ += freq_sym_type_prior_type_[ctx1][0] * (range_ >>= 14) + extra_range;
  else
    low_ += (freq_sym_type_prior_type_[ctx1][0] + freq_mtf) * (range_ >>= 14) + extra_range;
  range_ *= freq_sym_type_prior_type_[ctx1][1];
  uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 6;
  freq_sym_type_prior_type_[ctx1][1] += delta + (freq_mtf >> 6);
  freq_sym_type_prior_type_[ctx1][0] -= delta;
}

void EncoderModel::encode_new_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3, uint16_t queue_size) {
  normalize_encoder(8 * kFreqSymTypeBot3);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (8 * kFreqSymTypeBot3 - 1);
  if (queue_size != 0)
    low_ += (freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx2][0] + freq_sym_type_prior_end_[ctx3][0]) * (range_ >>= 15) + extra_range;
  else
    low_ += (0x8000 - freq_sym_type_prior_type_[ctx1][1] - freq_sym_type_prior_type_[ctx2][1] - freq_sym_type_prior_end_[ctx3][1]) * (range_ >>= 15) + extra_range;
  range_ *= freq_sym_type_prior_type_[ctx1][1] + freq_sym_type_prior_type_[ctx2][1] + freq_sym_type_prior_end_[ctx3][1];
  uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 4;
  freq_sym_type_prior_type_[ctx1][1] += delta + ((0x2C00 - (freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx1][1])) >> 4);
  freq_sym_type_prior_type_[ctx1][0] -= delta;
  delta = freq_sym_type_prior_type_[ctx2][0] >> 7;
  freq_sym_type_prior_type_[ctx2][1] += delta + ((0x3400 - (freq_sym_type_prior_type_[ctx2][0] + freq_sym_type_prior_type_[ctx2][1])) >> 7);
  freq_sym_type_prior_type_[ctx2][0] -= delta;
  delta = freq_sym_type_prior_end_[ctx3][0] >> 4;
  freq_sym_type_prior_end_[ctx3][1] += delta + ((0x2000 - (freq_sym_type_prior_end_[ctx3][0] + freq_sym_type_prior_end_[ctx3][1])) >> 4);
  freq_sym_type_prior_end_[ctx3][0] -= delta;
}

void EncoderModel::encode_mtf_type_binary(uint8_t ctx1, uint8_t ctx2) {
  normalize_encoder(kFreqSymTypeBot1);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (kFreqSymTypeBot1 - 1);
  const uint16_t delta = freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx1][1];
  low_ += delta * (range_ >>= 14) + extra_range;
  range_ *= kFreqSymTypeBot1 - delta;
  freq_sym_type_prior_type_[ctx1][0] -= freq_sym_type_prior_type_[ctx1][0] >> 6;
  freq_sym_type_prior_type_[ctx1][1] -= freq_sym_type_prior_type_[ctx1][1] >> 6;
}

void EncoderModel::encode_mtf_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3) {
  normalize_encoder(8 * kFreqSymTypeBot3);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (8 * kFreqSymTypeBot3 - 1);
  const uint16_t delta = freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx1][1]
      + freq_sym_type_prior_type_[ctx2][0] + freq_sym_type_prior_type_[ctx2][1]
      + freq_sym_type_prior_end_[ctx3][0] + freq_sym_type_prior_end_[ctx3][1];
  low_ += delta * (range_ >>= 15) + extra_range;
  range_ *= 8 * kFreqSymTypeBot3 - delta;
  freq_sym_type_prior_type_[ctx1][0] -= freq_sym_type_prior_type_[ctx1][0] >> 4;
  freq_sym_type_prior_type_[ctx1][1] -= freq_sym_type_prior_type_[ctx1][1] >> 4;
  freq_sym_type_prior_type_[ctx2][0] -= freq_sym_type_prior_type_[ctx2][0] >> 7;
  freq_sym_type_prior_type_[ctx2][1] -= freq_sym_type_prior_type_[ctx2][1] >> 7;
  freq_sym_type_prior_end_[ctx3][0] -= freq_sym_type_prior_end_[ctx3][0] >> 4;
  freq_sym_type_prior_end_[ctx3][1] -= freq_sym_type_prior_end_[ctx3][1] >> 4;
}

// ---------------------------------------------------------------------------
// EncoderModel -- MTF first
// ---------------------------------------------------------------------------

void EncoderModel::encode_mtf_first(uint8_t context, uint8_t first, uint16_t qs_other,
    uint16_t qs_space, uint16_t qs_az) {
  uint16_t delta;
  normalize_encoder(0x1000);
  if (encoder_failed_) return;
  if (first == 0) {
    if (qs_space == 0) {
      if (qs_az != 0) {
        if (qs_other >= qs_az)
          range_ = (0x1000 - freq_mtf_first_[context][2]) * (range_ >> 12);
        else
          range_ = freq_mtf_first_[context][0] * (range_ >> 12);
      } else
        range_ = 0x1000 * (range_ >> 12);
    } else if ((qs_az == 0) && (qs_other >= qs_space))
      range_ = (0x1000 - freq_mtf_first_[context][1]) * (range_ >> 12);
    else
      range_ = freq_mtf_first_[context][0] * (range_ >> 12);
    delta = freq_mtf_first_[context][1] >> 7;
    freq_mtf_first_[context][1] -= delta;
    freq_mtf_first_[context][0] += delta;
    delta = freq_mtf_first_[context][2] >> 7;
    freq_mtf_first_[context][2] -= delta;
    freq_mtf_first_[context][0] += delta;
  } else if (first == 1) {
    if (qs_other == 0) {
      if (qs_az != 0) {
        if (qs_space >= qs_az)
          range_ = (0x1000 - freq_mtf_first_[context][2]) * (range_ >> 12);
        else
          range_ = freq_mtf_first_[context][1] * (range_ >> 12);
      }
      else
        range_ = 0x1000 * (range_ >> 12);
    } else if (qs_az == 0) {
      if (qs_space > qs_other) {
        low_ += freq_mtf_first_[context][0] * (range_ >>= 12);
        range_ *= 0x1000 - freq_mtf_first_[context][0];
      } else {
        low_ += (0x1000 - freq_mtf_first_[context][1]) * (range_ >>= 12);
        range_ *= freq_mtf_first_[context][1];
      }
    } else {
      low_ += freq_mtf_first_[context][0] * (range_ >>= 12);
      range_ *= freq_mtf_first_[context][1];
    }
    delta = freq_mtf_first_[context][0] >> 7;
    freq_mtf_first_[context][0] -= delta;
    freq_mtf_first_[context][1] += delta;
    delta = freq_mtf_first_[context][2] >> 7;
    freq_mtf_first_[context][2] -= delta;
    freq_mtf_first_[context][1] += delta;
  } else {
    if (qs_other == 0) {
      if (qs_space != 0) {
        if (qs_az > qs_space) {
          low_ += freq_mtf_first_[context][1] * (range_ >>= 12);
          range_ *= freq_mtf_first_[context][0] + freq_mtf_first_[context][2];
        } else {
          low_ += (0x1000 - freq_mtf_first_[context][2]) * (range_ >>= 12);
          range_ *= freq_mtf_first_[context][2];
        }
      }
      else
        range_ = 0x1000 * (range_ >> 12);
    } else if ((qs_space == 0) && (qs_az > qs_other)) {
      low_ += freq_mtf_first_[context][0] * (range_ >>= 12);
      range_ *= 0x1000 - freq_mtf_first_[context][0];
    } else {
      low_ += (0x1000 - freq_mtf_first_[context][2]) * (range_ >>= 12);
      range_ *= freq_mtf_first_[context][2];
    }
    delta = freq_mtf_first_[context][0] >> 7;
    freq_mtf_first_[context][0] -= delta;
    freq_mtf_first_[context][2] += delta;
    delta = freq_mtf_first_[context][1] >> 7;
    freq_mtf_first_[context][1] -= delta;
    freq_mtf_first_[context][2] += delta;
  }
}

// ---------------------------------------------------------------------------
// EncoderModel -- MTF position
// ---------------------------------------------------------------------------

void EncoderModel::encode_mtf_pos(uint8_t position, uint16_t queue_size) {
  if (!check_mtf_pos("EncodeMtfPos", position, queue_size))
    return;
  normalize_encoder(kFreqMtfPosBot);
  if (encoder_failed_) return;
  if (last_queue_size_other_ > queue_size)
    unused_queue_freq_other_ += freq_mtf_pos_[0][--last_queue_size_other_];
  else if (last_queue_size_other_ < queue_size) {
    do {
      if (last_queue_size_other_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA encode: MTF other queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_other_));
        encoder_failed_ = 1;
        return;
      }
      unused_queue_freq_other_ -= freq_mtf_pos_[0][last_queue_size_other_++];
      if (last_queue_size_other_ > rescale_queue_size_other_) {
        rescale_queue_size_other_++;
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 8;
        range_scale_mtf_pos_[0] += 8;
      } else {
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 2;
        range_scale_mtf_pos_[0] += 2;
      }
    } while (last_queue_size_other_ != queue_size);
  }
  if (range_scale_mtf_pos_[0] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(0);
  if (position == 0) {
    range_ = freq_mtf_pos_[0][0] * (range_ / (range_scale_mtf_pos_[0] - unused_queue_freq_other_));
    freq_mtf_pos_[0][0] += kUpFreqMtfPos;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[0][0];
    uint16_t* stop_freq_ptr = &freq_mtf_pos_[0][position];
    range_low_ = *freq_ptr++;

    while (freq_ptr != stop_freq_ptr)
      range_low_ += *freq_ptr++;
    low_ += range_low_ * (range_ /= (range_scale_mtf_pos_[0] - unused_queue_freq_other_));
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_other_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_other_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
  }
  range_scale_mtf_pos_[0] += kUpFreqMtfPos;
}

void EncoderModel::encode_mtf_pos_az(uint8_t position, uint16_t queue_size) {
  if (!check_mtf_pos("EncodeMtfPosAz", position, queue_size))
    return;
  normalize_encoder(kFreqMtfPosBot);
  if (encoder_failed_) return;
  if (last_queue_size_az_ > queue_size)
    unused_queue_freq_az_ += freq_mtf_pos_[2][--last_queue_size_az_];
  else if (last_queue_size_az_ < queue_size) {
    do {
      if (last_queue_size_az_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA encode: MTF az queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_az_));
        encoder_failed_ = 1;
        return;
      }
      unused_queue_freq_az_ -= freq_mtf_pos_[2][last_queue_size_az_++];
      if (last_queue_size_az_ > rescale_queue_size_az_) {
        rescale_queue_size_az_++;
        freq_mtf_pos_[2][last_queue_size_az_ - 1] += 16;
        range_scale_mtf_pos_[2] += 16;
      } else {
        freq_mtf_pos_[2][last_queue_size_az_ - 1] += 4;
        range_scale_mtf_pos_[2] += 4;
      }
    } while (last_queue_size_az_ != queue_size);
  }
  if (range_scale_mtf_pos_[2] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(2);
  if (position == 0) {
    range_ = freq_mtf_pos_[2][0] * (range_ / (range_scale_mtf_pos_[2] - unused_queue_freq_az_));
    freq_mtf_pos_[2][0] += kUpFreqMtfPos;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[2][0];
    uint16_t* stop_freq_ptr = &freq_mtf_pos_[2][position];
    range_low_ = *freq_ptr++;
    while (freq_ptr != stop_freq_ptr)
      range_low_ += *freq_ptr++;
    low_ += range_low_ * (range_ /= (range_scale_mtf_pos_[2] - unused_queue_freq_az_));
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_az_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_az_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
  }
  range_scale_mtf_pos_[2] += kUpFreqMtfPos;
}

void EncoderModel::encode_mtf_pos_space(uint8_t position, uint16_t queue_size) {
  if (!check_mtf_pos("EncodeMtfPosSpace", position, queue_size))
    return;
  normalize_encoder(kFreqMtfPosBot);
  if (encoder_failed_) return;
  if (last_queue_size_space_ > queue_size)
    unused_queue_freq_space_ += freq_mtf_pos_[1][--last_queue_size_space_];
  else if (last_queue_size_space_ < queue_size) {
    do {
      if (last_queue_size_space_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA encode: MTF space queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_space_));
        encoder_failed_ = 1;
        return;
      }
      unused_queue_freq_space_ -= freq_mtf_pos_[1][last_queue_size_space_++];
      if (last_queue_size_space_ > rescale_queue_size_space_) {
        rescale_queue_size_space_++;
        freq_mtf_pos_[1][last_queue_size_space_ - 1] += 16;
        range_scale_mtf_pos_[1] += 16;
      } else {
        freq_mtf_pos_[1][last_queue_size_space_ - 1] += 4;
        range_scale_mtf_pos_[1] += 4;
      }
    } while (last_queue_size_space_ != queue_size);
  }
  if (range_scale_mtf_pos_[1] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(1);
  if (position == 0) {
    range_ = freq_mtf_pos_[1][0] * (range_ / (range_scale_mtf_pos_[1] - unused_queue_freq_space_));
    freq_mtf_pos_[1][0] += kUpFreqMtfPos;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[1][0];
    uint16_t* stop_freq_ptr = &freq_mtf_pos_[1][position];
    range_low_ = *freq_ptr++;
    while (freq_ptr != stop_freq_ptr)
      range_low_ += *freq_ptr++;
    low_ += range_low_ * (range_ /= (range_scale_mtf_pos_[1] - unused_queue_freq_space_));
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_space_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_space_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
  }
  range_scale_mtf_pos_[1] += kUpFreqMtfPos;
}

void EncoderModel::encode_mtf_pos_other(uint8_t position, uint16_t queue_size) {
  if (!check_mtf_pos("EncodeMtfPosOther", position, queue_size))
    return;
  normalize_encoder(kFreqMtfPosBot);
  if (encoder_failed_) return;
  if (last_queue_size_other_ > queue_size)
    unused_queue_freq_other_ += freq_mtf_pos_[0][--last_queue_size_other_];
  else if (last_queue_size_other_ < queue_size) {
    do {
      if (last_queue_size_other_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA encode: MTF other queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_other_));
        encoder_failed_ = 1;
        return;
      }
      unused_queue_freq_other_ -= freq_mtf_pos_[0][last_queue_size_other_++];
      if (last_queue_size_other_ > rescale_queue_size_other_) {
        rescale_queue_size_other_++;
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 16;
        range_scale_mtf_pos_[0] += 16;
      } else {
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 4;
        range_scale_mtf_pos_[0] += 4;
      }
    } while (last_queue_size_other_ != queue_size);
  }
  if (range_scale_mtf_pos_[0] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(0);
  if (position == 0) {
    range_ = freq_mtf_pos_[0][0] * (range_ / (range_scale_mtf_pos_[0] - unused_queue_freq_other_));
    freq_mtf_pos_[0][0] += kUpFreqMtfPos;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[0][0];
    uint16_t* stop_freq_ptr = &freq_mtf_pos_[0][position];
    range_low_ = *freq_ptr++;
    while (freq_ptr != stop_freq_ptr)
      range_low_ += *freq_ptr++;
    low_ += range_low_ * (range_ /= (range_scale_mtf_pos_[0] - unused_queue_freq_other_));
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_other_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position + 1 == queue_size)
          unused_queue_freq_other_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
  }
  range_scale_mtf_pos_[0] += kUpFreqMtfPos;
}

// ---------------------------------------------------------------------------
// EncoderModel -- SID / INST / ERG / GoMtf / WordTag
// ---------------------------------------------------------------------------

void EncoderModel::encode_sid(uint8_t context, uint8_t sid_symbol) {
  normalize_encoder(kFreqSidBot);
  if (encoder_failed_) return;
  if (sid_symbol == 0) {
    range_ = freq_sid_[context][0] * (range_ / range_scale_sid_[context]);
    freq_sid_[context][0] += kUpFreqSid;
  } else {
    range_low_ = freq_sid_[context][0];
    uint8_t symbol = 1;
    while (symbol != sid_symbol)
      range_low_ += freq_sid_[context][symbol++];
    low_ += range_low_ * (range_ /= range_scale_sid_[context]);
    range_ *= freq_sid_[context][sid_symbol];
    freq_sid_[context][sid_symbol] += kUpFreqSid;
  }
  if ((range_scale_sid_[context] += kUpFreqSid) > kFreqSidBot)
    rescale_sid(context);
}

void EncoderModel::encode_extra_sid(uint32_t extra_symbols) {
  int64_t code;
  uint8_t range_multiplier;
  uint8_t bits = 9;
  if (extra_symbols <= 1) {
    code = extra_symbols << 6;
    range_multiplier = 0x40;
  } else if (extra_symbols <= 5) {
    code = (extra_symbols + 2) << 5;
    range_multiplier = 0x20;
  } else if (extra_symbols <= 0xD) {
    code = 0x80 + ((extra_symbols + 2) << 4);
    range_multiplier = 0x10;
  } else if (extra_symbols <= 0x1D) {
    code = 0x140 + ((extra_symbols + 2) << 2);
    range_multiplier = 4;
  } else {
    int64_t top, bottom;
    top = 0x200; bottom = 0x20;
    while (extra_symbols + 2 >= static_cast<uint32_t>(bottom * 2)) {
      top *= 4;
      bottom *= 2;
      bits += 2;
    }
    code = (top - 3 * bottom + extra_symbols + 2) << (6 - ((bits - 3) & 7));
    range_multiplier = 1 << (6 - ((bits - 3) & 7));
    bits += 6 - ((bits - 3) & 7);
  }
  normalize_encoder(uint32_t{1} << 9);
  if (encoder_failed_) return;
  uint16_t cnt = (code >> (bits - 9)) & 0x1FF;
  range_ >>= 9;
  low_ += range_ * cnt;
  bits -= 9;
  while (bits != 0) {
    normalize_encoder(uint32_t{1} << 8);
    if (encoder_failed_) return;
    cnt = (code >> (bits - 8)) & 0xFF;
    range_ >>= 8;
    low_ += range_ * cnt;
    bits -= 8;
  }
  low_ -= range_ * (cnt & (range_multiplier - 1));
  range_ *= range_multiplier;
}

void EncoderModel::encode_inst(uint8_t context, uint8_t sid_symbol, uint8_t symbol) {
  normalize_encoder(kFreqInstBot);
  if (encoder_failed_) return;
  uint32_t extra_range = range_;
  range_ /= range_scale_inst_[context][sid_symbol];
  extra_range -= range_ * range_scale_inst_[context][sid_symbol];
  if (symbol == 0) {
    range_ = range_ * freq_inst_[context][sid_symbol][0] + extra_range;
    if (range_scale_inst_[context][sid_symbol] >= (kFreqInstBot >> 1)) {
      freq_inst_[context][sid_symbol][0] += range_scale_inst_[context][sid_symbol] >> 11;
      if ((range_scale_inst_[context][sid_symbol] += (range_scale_inst_[context][sid_symbol]) >> 11) > kFreqInstBot)
        rescale_inst(context, sid_symbol);
    } else {
      freq_inst_[context][sid_symbol][0] += kUpFreqInst;
      range_scale_inst_[context][sid_symbol] += kUpFreqInst;
    }
  } else {
    range_low_ = freq_inst_[context][sid_symbol][0];
    uint8_t found_index = 1;
    while (found_index != symbol)
      range_low_ += freq_inst_[context][sid_symbol][found_index++];
    low_ += range_ * range_low_ + extra_range;
    range_ *= freq_inst_[context][sid_symbol][found_index];
    if (range_scale_inst_[context][sid_symbol] >= (kFreqInstBot >> 1)) {
      freq_inst_[context][sid_symbol][found_index] += range_scale_inst_[context][sid_symbol] >> 11;
      if ((range_scale_inst_[context][sid_symbol] += (range_scale_inst_[context][sid_symbol]) >> 11) > kFreqInstBot)
        rescale_inst(context, sid_symbol);
    } else {
      freq_inst_[context][sid_symbol][found_index] += kUpFreqInst;
      range_scale_inst_[context][sid_symbol] += kUpFreqInst;
    }
  }
}

void EncoderModel::encode_erg(uint16_t ctx1, uint16_t ctx2, uint8_t symbol) {
  normalize_encoder(kFreqErgBot);
  if (encoder_failed_) return;
  if (symbol == 0) {
    range_ = (freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]) * (range_ >> 13);
    freq_erg_[0] += (0x400 - freq_erg_[0]) >> 2;
    freq_erg_[ctx1] += (0x1000 - freq_erg_[ctx1]) >> 4;
    freq_erg_[ctx2] += (0xC00 - freq_erg_[ctx2]) >> 3;
  } else {
    low_ += (freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]) * (range_ >>= 13);
    range_ *= 0x2000 - (freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]);
    freq_erg_[0] -= freq_erg_[0] >> 2;
    freq_erg_[ctx1] -= freq_erg_[ctx1] >> 4;
    freq_erg_[ctx2] -= freq_erg_[ctx2] >> 3;
  }
}

void EncoderModel::encode_go_mtf(uint16_t ctx1, uint8_t ctx2, uint8_t symbol) {
  normalize_encoder(kFreqGoMtfBot);
  if (encoder_failed_) return;
  const uint32_t extra_range = range_ & (kFreqGoMtfBot - 1);
  uint16_t c1 = ctx1 + 0xF0 * ctx2;
  uint16_t c3 = c1 + 0x2D0;
  if (symbol == 0) {
    range_ = (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]) * (range_ >> 13) + extra_range;
    freq_go_mtf_[c1] += (0x800 - freq_go_mtf_[c1]) >> 2;
    freq_go_mtf_[ctx2] += (0x800 - freq_go_mtf_[ctx2]) >> 2;
    freq_go_mtf_[c3] += (0x800 - freq_go_mtf_[c3]) >> 6;
  } else {
    low_ += (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]) * (range_ >>= 13) + extra_range;
    range_ *= 0x2000 - (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]);
    freq_go_mtf_[c1] -= freq_go_mtf_[c1] >> 2;
    freq_go_mtf_[ctx2] -= freq_go_mtf_[ctx2] >> 2;
    freq_go_mtf_[c3] -= freq_go_mtf_[c3] >> 6;
  }
}

void EncoderModel::encode_word_tag(uint8_t symbol, uint8_t context) {
  normalize_encoder(kFreqWordTagBot);
  if (encoder_failed_) return;
  if (symbol == 0) {
    range_ = freq_word_tag_[context] * (range_ >> 12);
    freq_word_tag_[context] += (0x1000 - freq_word_tag_[context]) >> 4;
  } else {
    low_ += freq_word_tag_[context] * (range_ >>= 12);
    range_ *= 0x1000 - freq_word_tag_[context];
    freq_word_tag_[context] -= freq_word_tag_[context] >> 4;
  }
}

// ---------------------------------------------------------------------------
// EncoderModel -- dictionary / base symbol / first char encoding
// ---------------------------------------------------------------------------

void EncoderModel::encode_short_dictionary_symbol(uint16_t bin_num, uint16_t dict_bins, uint16_t code_bins) {
  if (dict_bins == 0)
    dict_bins = 1;
  if (code_bins == 0)
    code_bins = 1;
  normalize_encoder(uint32_t{1} << 12);
  if (encoder_failed_) return;
  low_ += bin_num * (range_ /= dict_bins);
  range_ *= static_cast<uint32_t>(code_bins);
}

void EncoderModel::encode_long_dictionary_symbol(uint32_t bin_code, uint16_t bin_num,
    uint16_t dict_bins, uint8_t code_length, uint16_t code_bins) {
  normalize_encoder(uint32_t{1} << 12);
  if (encoder_failed_) return;
  low_ += bin_num * (range_ /= dict_bins);
  normalize_encoder(uint32_t{1} << code_length);
  if (encoder_failed_) return;
  low_ += bin_code * (range_ >>= code_length);
  range_ *= static_cast<uint32_t>(code_bins);
}

void EncoderModel::encode_base_symbol(uint32_t base_symbol, uint32_t num_base_symbols, uint32_t norm_base_symbols) {
  normalize_encoder(norm_base_symbols);
  if (encoder_failed_) return;
  low_ += base_symbol * (range_ /= num_base_symbols);
}

void EncoderModel::encode_first_char(uint8_t first_char, uint8_t sym_type, uint8_t last_char) {
  uint16_t* range_scale_ptr = &range_scale_first_char_[sym_type][last_char];
  uint32_t extra_range;
  FirstCharData* fc_data_ptr = &first_char_data_[sym_type][last_char][0];

  normalize_encoder(static_cast<uint32_t>(kFreqFirstCharBot));
  if (encoder_failed_) return;
  extra_range = range_;
  range_low_ = 0;
  range_ /= *range_scale_ptr;
  extra_range -= range_ * *range_scale_ptr;
  while (fc_data_ptr->data.symbol != first_char) {
    range_low_ += fc_data_ptr++->data.freq;
  }
  low_ += range_low_ * range_;
  range_ *= fc_data_ptr->data.freq;
  if (fc_data_ptr == &first_char_data_[sym_type][last_char][0])
    range_ += extra_range;
  else
    low_ += extra_range;

  if (*range_scale_ptr >= (kFreqFirstCharBot >> 2)) {
    fc_data_ptr->data.freq += *range_scale_ptr >> 10;
    if ((*range_scale_ptr += (*range_scale_ptr >> 10)) >= kFreqFirstCharBot)
      rescale_first_char(sym_type, last_char);
  } else {
    fc_data_ptr->data.freq += kUpFreqFirstChar;
    *range_scale_ptr += kUpFreqFirstChar;
  }

  if (fc_data_ptr != &first_char_data_[sym_type][last_char][0]) {
    if (fc_data_ptr->data.freq > (fc_data_ptr - 1)->data.freq) {
      FirstCharData saved_data;
      saved_data.all_data = fc_data_ptr->all_data;
      do {
        fc_data_ptr->all_data = (fc_data_ptr - 1)->all_data;
        fc_data_ptr--;
      } while ((fc_data_ptr != &first_char_data_[sym_type][last_char][0]) && (saved_data.data.freq > (fc_data_ptr - 1)->data.freq));
      fc_data_ptr->all_data = saved_data.all_data;
    }
  }
}

void EncoderModel::encode_first_char_binary(uint8_t first_char, uint8_t last_char) {
  uint8_t section_index = 0;
  uint16_t* range_scale_ptr = &range_scale_first_char_[0][last_char];
  uint32_t extra_range;
  FirstCharData* fc_data_ptr;

  normalize_encoder(kFreqFirstCharBot);
  if (encoder_failed_) return;
  extra_range = range_;
  range_low_ = 0;
  while ((section_index != 7) && (first_char >= 0x20 * (section_index + 1))) {
    range_low_ += range_scale_first_char_section_[last_char][section_index];
    section_index++;
  }
  fc_data_ptr = &first_char_data_[0][last_char][first_char & 0xE0];
  while (fc_data_ptr != &first_char_data_[0][last_char][first_char]) {
    range_low_ += fc_data_ptr++->data.freq;
  }
  range_ /= static_cast<uint32_t>(*range_scale_ptr);
  extra_range -= range_ * static_cast<uint32_t>(*range_scale_ptr);
  low_ += static_cast<uint32_t>(range_low_) * range_;
  range_ *= static_cast<uint32_t>(fc_data_ptr->data.freq);
  if (fc_data_ptr == &first_char_data_[0][last_char][0])
    range_ += extra_range;
  else
    low_ += extra_range;

  if (*range_scale_ptr >= (kFreqFirstCharBot >> 2)) {
    fc_data_ptr->data.freq += *range_scale_ptr >> 10;
    if (section_index <= 6)
      range_scale_first_char_section_[last_char][section_index] += *range_scale_ptr >> 10;
    if ((*range_scale_ptr += *range_scale_ptr >> 10) >= kFreqFirstCharBot)
      rescale_first_char_binary(last_char);
  } else {
    fc_data_ptr->data.freq += kUpFreqFirstChar >> 1;
    if (section_index <= 6)
      range_scale_first_char_section_[last_char][section_index] += kUpFreqFirstChar >> 1;
    *range_scale_ptr += kUpFreqFirstChar >> 1;
  }
}

// ===========================================================================
// DecoderModel
// ===========================================================================

void DecoderModel::decoder_fail(const char* reason) {
  if (decoder_failed_ == 0) {
    std::fprintf(stderr,
        "GLZA decode: %s (InCharNum=%u code=0x%08x low=0x%08x range=0x%08x)\n",
        reason, static_cast<unsigned>(in_char_num_), code_, low_, range_);
    decoder_failed_ = 1;
  }
}

void DecoderModel::normalize_decoder(uint32_t bot) {
  if (decoder_failed_ != 0)
    return;
#ifdef GLZA_OVERREAD_DIAG
  extern long glza_norm_ct;
  extern uint32_t glza_ring_low[], glza_ring_range[], glza_ring_code[];
  const long ct = glza_norm_ct++;
  glza_ring_low[ct & 0x3FF] = low_;
  glza_ring_range[ct & 0x3FF] = range_;
  glza_ring_code[ct & 0x3FF] = code_;
#endif
  while ((low_ ^ (low_ + range_)) < kTop || (range_ < bot && ((range_ = -low_ & (bot - 1)), 1))) {
#ifdef GLZA_OVERREAD_DIAG
    if (in_size_dbg_ != 0 && in_char_num_ >= in_size_dbg_) {
      fprintf(stderr, "OVERREAD op=%ld low=%08x range=%08x code=%08x bot=%x\n", ct, low_, range_, code_, bot);
      for (long k = ct - 24; k <= ct; k++)
        fprintf(stderr, "  DEC op=%ld low=%08x range=%08x code=%08x\n",
                k, glza_ring_low[k & 0x3FF], glza_ring_range[k & 0x3FF], glza_ring_code[k & 0x3FF]);
      decoder_failed_ = 1;
      return;
    }
#endif
    code_ = (code_ << 8) | in_buffer_[in_char_num_++];
    low_ <<= 8;
    range_ <<= 8;
  }
}

void DecoderModel::reset_codec_globals() {
  in_char_num_ = 0;
  out_char_num_ = 0;
  in_buffer_ = nullptr;
  decoder_failed_ = 0;
}

void DecoderModel::init_decoder(uint8_t max_base_code, uint8_t num_inst_codes,
    uint8_t cap_encoded, uint8_t utf8_compliant, uint8_t use_mtf, uint8_t* inbuf) {
  cap_initialized_ = 0;
  cap_lock_initialized_ = 0;
  decoder_failed_ = 0;
  cap_encoded_ = cap_encoded;
  utf8_compliant_ = utf8_compliant;
  max_base_code_ = max_base_code;
  max_inst_code_ = num_inst_codes - 1;
  num_base_symbols_ = 0;
  in_buffer_ = inbuf;
  code_ = 0;
  range_ = 0xFFFFFFFF;
  for (low_ = 4; low_ != 0; low_--)
    code_ = (code_ << 8) | in_buffer_[in_char_num_++];
  low_ = 0;
  start_model_sym_type(use_mtf, cap_encoded);
  start_model_mtf_first();
  start_model_mtf_pos();
  start_model_sid();
  start_model_inst(num_inst_codes);
  start_model_erg();
  start_model_go_mtf();
  start_model_word_tag();
  if (cap_encoded || utf8_compliant)
    start_model_first_char();
  else
    start_model_first_char_binary();

  if (utf8_compliant != 0) {
    uint8_t i = 0x90;
    uint8_t j = 0x90;
    (void)i; (void)j;
  } else {
    uint8_t i = 0xFF;
    uint8_t j = 0xFF;
    (void)i; (void)j;
  }
}

// ---------------------------------------------------------------------------
// DecoderModel -- symbol type decoding
// ---------------------------------------------------------------------------

uint8_t DecoderModel::decode_sym_type_binary(uint8_t ctx1, uint8_t ctx2, uint16_t queue_size) {
  uint32_t dict_range;
  normalize_decoder(kFreqSymTypeBot1);
  const uint32_t extra_range = range_ & (kFreqSymTypeBot1 - 1);
  if (queue_size != 0) {
    if ((dict_range = (range_ >>= 14) * freq_sym_type_prior_type_[ctx1][0] + extra_range) > code_ - low_) {
      range_ = dict_range;
      uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 6;
      freq_sym_type_prior_type_[ctx1][0] += delta + ((kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 6);
      freq_sym_type_prior_type_[ctx1][1] -= delta;
      return 0;
    } else if (dict_range + range_ * freq_sym_type_prior_type_[ctx1][1] > code_ - low_) {
      low_ += dict_range;
      range_ *= freq_sym_type_prior_type_[ctx1][1];
      uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 6;
      freq_sym_type_prior_type_[ctx1][1] += delta + ((kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 6);
      freq_sym_type_prior_type_[ctx1][0] -= delta;
      return 1;
    } else {
      low_ += dict_range + range_ * freq_sym_type_prior_type_[ctx1][1];
      range_ *= kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1];
      freq_sym_type_prior_type_[ctx1][0] -= freq_sym_type_prior_type_[ctx1][0] >> 6;
      freq_sym_type_prior_type_[ctx1][1] -= freq_sym_type_prior_type_[ctx1][1] >> 6;
      return 2;
    }
  } else {
    dict_range = (range_ >>= 14) * (kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][1]) + extra_range;
    if (dict_range > code_ - low_) {
      range_ = dict_range;
      uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 6;
      freq_sym_type_prior_type_[ctx1][0] += delta + ((kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 6);
      freq_sym_type_prior_type_[ctx1][1] -= delta;
      return 0;
    } else {
      low_ += dict_range;
      range_ *= freq_sym_type_prior_type_[ctx1][1];
      uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 6;
      freq_sym_type_prior_type_[ctx1][1] += delta + ((kFreqSymTypeBot1 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 6);
      freq_sym_type_prior_type_[ctx1][0] -= delta;
      return 1;
    }
  }
}

uint8_t DecoderModel::decode_sym_type(uint8_t ctx1, uint8_t ctx2, uint8_t ctx3, uint16_t queue_size) {
  uint32_t dict_range;
  normalize_decoder(8 * kFreqSymTypeBot3);
  const uint32_t extra_range = range_ & (8 * kFreqSymTypeBot3 - 1);
  if (queue_size != 0) {
    if ((dict_range = (freq_sym_type_prior_type_[ctx1][0] + freq_sym_type_prior_type_[ctx2][0]
        + freq_sym_type_prior_end_[ctx3][0]) * (range_ >>= 15) + extra_range) > code_ - low_) {
      range_ = dict_range;
      uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 4;
      freq_sym_type_prior_type_[ctx1][0] += delta + ((0x2C00 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 4);
      freq_sym_type_prior_type_[ctx1][1] -= delta;
      delta = freq_sym_type_prior_type_[ctx2][1] >> 7;
      freq_sym_type_prior_type_[ctx2][0] += delta + ((0x3400 - freq_sym_type_prior_type_[ctx2][0] - freq_sym_type_prior_type_[ctx2][1]) >> 7);
      freq_sym_type_prior_type_[ctx2][1] -= delta;
      delta = freq_sym_type_prior_end_[ctx3][1] >> 4;
      freq_sym_type_prior_end_[ctx3][0] += delta + ((0x2000 - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1]) >> 4);
      freq_sym_type_prior_end_[ctx3][1] -= delta;
      return 0;
    } else if (dict_range + range_ * (freq_sym_type_prior_type_[ctx1][1] + freq_sym_type_prior_type_[ctx2][1]
        + freq_sym_type_prior_end_[ctx3][1]) > code_ - low_) {
      low_ += dict_range;
      range_ *= freq_sym_type_prior_type_[ctx1][1] + freq_sym_type_prior_type_[ctx2][1] + freq_sym_type_prior_end_[ctx3][1];
      uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 4;
      freq_sym_type_prior_type_[ctx1][1] += delta + ((0x2C00 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 4);
      freq_sym_type_prior_type_[ctx1][0] -= delta;
      delta = freq_sym_type_prior_type_[ctx2][0] >> 7;
      freq_sym_type_prior_type_[ctx2][1] += delta + ((0x3400 - freq_sym_type_prior_type_[ctx2][0] - freq_sym_type_prior_type_[ctx2][1]) >> 7);
      freq_sym_type_prior_type_[ctx2][0] -= delta;
      delta = freq_sym_type_prior_end_[ctx3][0] >> 4;
      freq_sym_type_prior_end_[ctx3][1] += delta + ((0x2000 - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1]) >> 4);
      freq_sym_type_prior_end_[ctx3][0] -= delta;
      return 1;
    } else {
      low_ += dict_range + range_ * (freq_sym_type_prior_type_[ctx1][1] + freq_sym_type_prior_type_[ctx2][1] + freq_sym_type_prior_end_[ctx3][1]);
      range_ *= 0x8000 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1] - freq_sym_type_prior_type_[ctx2][0]
          - freq_sym_type_prior_type_[ctx2][1] - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1];
      freq_sym_type_prior_type_[ctx1][0] -= freq_sym_type_prior_type_[ctx1][0] >> 4;
      freq_sym_type_prior_type_[ctx1][1] -= freq_sym_type_prior_type_[ctx1][1] >> 4;
      freq_sym_type_prior_type_[ctx2][0] -= freq_sym_type_prior_type_[ctx2][0] >> 7;
      freq_sym_type_prior_type_[ctx2][1] -= freq_sym_type_prior_type_[ctx2][1] >> 7;
      freq_sym_type_prior_end_[ctx3][0] -= freq_sym_type_prior_end_[ctx3][0] >> 4;
      freq_sym_type_prior_end_[ctx3][1] -= freq_sym_type_prior_end_[ctx3][1] >> 4;
      return 2;
    }
  } else {
    if ((dict_range = (0x8000 - freq_sym_type_prior_type_[ctx1][1] - freq_sym_type_prior_type_[ctx2][1] - freq_sym_type_prior_end_[ctx3][1])
        * (range_ >>= 15) + extra_range) > code_ - low_) {
      range_ = dict_range;
      uint16_t delta = freq_sym_type_prior_type_[ctx1][1] >> 4;
      freq_sym_type_prior_type_[ctx1][0] += delta + ((0x2C00 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 4);
      freq_sym_type_prior_type_[ctx1][1] -= delta;
      delta = freq_sym_type_prior_type_[ctx2][1] >> 7;
      freq_sym_type_prior_type_[ctx2][0] += delta + ((0x3400 - freq_sym_type_prior_type_[ctx2][0] - freq_sym_type_prior_type_[ctx2][1]) >> 7);
      freq_sym_type_prior_type_[ctx2][1] -= delta;
      delta = freq_sym_type_prior_end_[ctx3][1] >> 4;
      freq_sym_type_prior_end_[ctx3][0] += delta + ((0x2000 - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1]) >> 4);
      freq_sym_type_prior_end_[ctx3][1] -= delta;
      return 0;
    } else {
      low_ += dict_range;
      range_ *= freq_sym_type_prior_type_[ctx1][1] + freq_sym_type_prior_type_[ctx2][1] + freq_sym_type_prior_end_[ctx3][1];
      uint16_t delta = freq_sym_type_prior_type_[ctx1][0] >> 4;
      freq_sym_type_prior_type_[ctx1][1] += delta + ((0x2C00 - freq_sym_type_prior_type_[ctx1][0] - freq_sym_type_prior_type_[ctx1][1]) >> 4);
      freq_sym_type_prior_type_[ctx1][0] -= delta;
      delta = freq_sym_type_prior_type_[ctx2][0] >> 7;
      freq_sym_type_prior_type_[ctx2][1] += delta + ((0x3400 - freq_sym_type_prior_type_[ctx2][0] - freq_sym_type_prior_type_[ctx2][1]) >> 7);
      freq_sym_type_prior_type_[ctx2][0] -= delta;
      delta = freq_sym_type_prior_end_[ctx3][0] >> 4;
      freq_sym_type_prior_end_[ctx3][1] += delta + ((0x2000 - freq_sym_type_prior_end_[ctx3][0] - freq_sym_type_prior_end_[ctx3][1]) >> 4);
      freq_sym_type_prior_end_[ctx3][0] -= delta;
      return 1;
    }
  }
}

// ---------------------------------------------------------------------------
// DecoderModel -- MTF first
// ---------------------------------------------------------------------------

uint8_t DecoderModel::decode_mtf_first(uint8_t context, uint16_t qs_other, uint16_t qs_space, uint16_t qs_az) {
  uint16_t delta;
  uint16_t freq0, freq1;
  normalize_decoder(0x1000);
  if (qs_other == 0) {
    freq0 = 0;
    if (qs_space == 0)
      freq1 = 0;
    else if (qs_az == 0)
      freq1 = 0x1000;
    else if (qs_az > qs_space)
      freq1 = freq_mtf_first_[context][1];
    else
      freq1 = freq_mtf_first_[context][0] + freq_mtf_first_[context][1];
  } else if (qs_space == 0) {
    freq1 = 0;
    if (qs_az == 0)
      freq0 = 0x1000;
    else if (qs_az > qs_other)
      freq0 = freq_mtf_first_[context][0];
    else
      freq0 = freq_mtf_first_[context][0] + freq_mtf_first_[context][1];
  } else if (qs_az == 0) {
    if (qs_space > qs_other) {
      freq0 = freq_mtf_first_[context][0];
      freq1 = 0x1000 - freq_mtf_first_[context][0];
    } else {
      freq0 = 0x1000 - freq_mtf_first_[context][1];
      freq1 = freq_mtf_first_[context][1];
    }
  } else {
    freq0 = freq_mtf_first_[context][0];
    freq1 = freq_mtf_first_[context][1];
  }

  if (freq0 * (range_ >>= 12) > code_ - low_) {
    range_ *= freq0;
    delta = freq_mtf_first_[context][1] >> 7;
    freq_mtf_first_[context][1] -= delta;
    freq_mtf_first_[context][0] += delta;
    delta = freq_mtf_first_[context][2] >> 7;
    freq_mtf_first_[context][2] -= delta;
    freq_mtf_first_[context][0] += delta;
    return 0;
  } else if ((freq0 + freq1) * range_ > code_ - low_) {
    low_ += range_ * freq0;
    range_ *= freq1;
    delta = freq_mtf_first_[context][0] >> 7;
    freq_mtf_first_[context][0] -= delta;
    freq_mtf_first_[context][1] += delta;
    delta = freq_mtf_first_[context][2] >> 7;
    freq_mtf_first_[context][2] -= delta;
    freq_mtf_first_[context][1] += delta;
    return 1;
  } else {
    low_ += range_ * (freq0 + freq1);
    range_ *= 0x1000 - freq0 - freq1;
    delta = freq_mtf_first_[context][0] >> 7;
    freq_mtf_first_[context][0] -= delta;
    freq_mtf_first_[context][2] += delta;
    delta = freq_mtf_first_[context][1] >> 7;
    freq_mtf_first_[context][1] -= delta;
    freq_mtf_first_[context][2] += delta;
    return 2;
  }
}

// ---------------------------------------------------------------------------
// DecoderModel -- MTF position
// ---------------------------------------------------------------------------

uint8_t DecoderModel::decode_mtf_pos(uint16_t queue_size) {
  if (queue_size > 0xFF) {
    decoder_fail("MTF queue size exceeds FreqMtfPos table limit in DecodeMtfPos");
    return 0;
  }
  normalize_decoder(kFreqMtfPosBot);
  if (decoder_failed_ != 0)
    return 0;
  if (last_queue_size_other_ > queue_size)
    unused_queue_freq_other_ += freq_mtf_pos_[0][--last_queue_size_other_];
  else if (last_queue_size_other_ < queue_size) {
    do {
      if (last_queue_size_other_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA decode: MTF other queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_other_));
        decoder_fail("MTF pos table overflow in DecodeMtfPos");
        return 0;
      }
      unused_queue_freq_other_ -= freq_mtf_pos_[0][last_queue_size_other_++];
      if (last_queue_size_other_ > rescale_queue_size_other_) {
        rescale_queue_size_other_++;
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 8;
        range_scale_mtf_pos_[0] += 8;
      } else {
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 2;
        range_scale_mtf_pos_[0] += 2;
      }
    } while (last_queue_size_other_ != queue_size);
  }
  if (range_scale_mtf_pos_[0] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(0);
  count_ = (code_ - low_) / (range_ /= (range_scale_mtf_pos_[0] - unused_queue_freq_other_));
  if ((range_high_ = freq_mtf_pos_[0][0]) > count_) {
    range_ *= range_high_;
    freq_mtf_pos_[0][0] += kUpFreqMtfPos;
    range_scale_mtf_pos_[0] += kUpFreqMtfPos;
    return 0;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[0][1];
    while ((range_high_ += *freq_ptr) <= count_)
      freq_ptr++;
    const uint8_t position = freq_ptr - &freq_mtf_pos_[0][0];
    low_ += range_ * (range_high_ - *freq_ptr);
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_other_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_other_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
    range_scale_mtf_pos_[0] += kUpFreqMtfPos;
    return position;
  }
}

uint8_t DecoderModel::decode_mtf_pos_az(uint16_t queue_size) {
  if (queue_size > 0xFF) {
    decoder_fail("MTF queue size exceeds FreqMtfPos table limit in DecodeMtfPosAz");
    return 0;
  }
  normalize_decoder(kFreqMtfPosBot);
  if (decoder_failed_ != 0)
    return 0;
  if (last_queue_size_az_ > queue_size)
    unused_queue_freq_az_ += freq_mtf_pos_[2][--last_queue_size_az_];
  else if (last_queue_size_az_ < queue_size) {
    do {
      if (last_queue_size_az_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA decode: MTF az queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_az_));
        decoder_fail("MTF pos table overflow in DecodeMtfPosAz");
        return 0;
      }
      unused_queue_freq_az_ -= freq_mtf_pos_[2][last_queue_size_az_++];
      if (last_queue_size_az_ > rescale_queue_size_az_) {
        rescale_queue_size_az_++;
        freq_mtf_pos_[2][last_queue_size_az_ - 1] += 16;
        range_scale_mtf_pos_[2] += 16;
      } else {
        freq_mtf_pos_[2][last_queue_size_az_ - 1] += 4;
        range_scale_mtf_pos_[2] += 4;
      }
    } while (last_queue_size_az_ != queue_size);
  }
  if (range_scale_mtf_pos_[2] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(2);
  count_ = code_ - low_;
  range_ /= range_scale_mtf_pos_[2] - unused_queue_freq_az_;
  if ((range_high_ = range_ * freq_mtf_pos_[2][0]) > count_) {
    range_ *= freq_mtf_pos_[2][0];
    freq_mtf_pos_[2][0] += kUpFreqMtfPos;
    range_scale_mtf_pos_[2] += kUpFreqMtfPos;
    return 0;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[2][1];
    while ((range_high_ += range_ * *freq_ptr) <= count_)
      freq_ptr++;
    const uint8_t position = freq_ptr - &freq_mtf_pos_[2][0];
    low_ += range_high_ - range_ * *freq_ptr;
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_az_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_az_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
    range_scale_mtf_pos_[2] += kUpFreqMtfPos;
    return position;
  }
}

uint8_t DecoderModel::decode_mtf_pos_space(uint16_t queue_size) {
  if (queue_size > 0xFF) {
    decoder_fail("MTF queue size exceeds FreqMtfPos table limit in DecodeMtfPosSpace");
    return 0;
  }
  normalize_decoder(kFreqMtfPosBot);
  if (decoder_failed_ != 0)
    return 0;
  if (last_queue_size_space_ > queue_size)
    unused_queue_freq_space_ += freq_mtf_pos_[1][--last_queue_size_space_];
  else if (last_queue_size_space_ < queue_size) {
    do {
      if (last_queue_size_space_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA decode: MTF space queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_space_));
        decoder_fail("MTF pos table overflow in DecodeMtfPosSpace");
        return 0;
      }
      unused_queue_freq_space_ -= freq_mtf_pos_[1][last_queue_size_space_++];
      if (last_queue_size_space_ > rescale_queue_size_space_) {
        rescale_queue_size_space_++;
        freq_mtf_pos_[1][last_queue_size_space_ - 1] += 16;
        range_scale_mtf_pos_[1] += 16;
      } else {
        freq_mtf_pos_[1][last_queue_size_space_ - 1] += 4;
        range_scale_mtf_pos_[1] += 4;
      }
    } while (last_queue_size_space_ != queue_size);
  }
  if (range_scale_mtf_pos_[1] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(1);
  count_ = code_ - low_;
  range_ /= range_scale_mtf_pos_[1] - unused_queue_freq_space_;
  if ((range_high_ = range_ * freq_mtf_pos_[1][0]) > count_) {
    range_ *= freq_mtf_pos_[1][0];
    freq_mtf_pos_[1][0] += kUpFreqMtfPos;
    range_scale_mtf_pos_[1] += kUpFreqMtfPos;
    return 0;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[1][1];
    while ((range_high_ += range_ * *freq_ptr) <= count_)
      freq_ptr++;
    const uint8_t position = freq_ptr - &freq_mtf_pos_[1][0];
    low_ += range_high_ - range_ * *freq_ptr;
    range_ *= *freq_ptr;

    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_space_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_space_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
    range_scale_mtf_pos_[1] += kUpFreqMtfPos;
    return position;
  }
}

uint8_t DecoderModel::decode_mtf_pos_other(uint16_t queue_size) {
  if (queue_size > 0xFF) {
    decoder_fail("MTF queue size exceeds FreqMtfPos table limit in DecodeMtfPosOther");
    return 0;
  }
  normalize_decoder(kFreqMtfPosBot);
  if (decoder_failed_ != 0)
    return 0;
  if (last_queue_size_other_ > queue_size)
    unused_queue_freq_other_ += freq_mtf_pos_[0][--last_queue_size_other_];
  else if (last_queue_size_other_ < queue_size) {
    do {
      if (last_queue_size_other_ >= 0xFF) {
        std::fprintf(stderr,
            "GLZA decode: MTF other queue index overflow growing to QueueSize=%u (last=%u)\n",
            static_cast<unsigned>(queue_size), static_cast<unsigned>(last_queue_size_other_));
        decoder_fail("MTF pos table overflow in DecodeMtfPosOther");
        return 0;
      }
      unused_queue_freq_other_ -= freq_mtf_pos_[0][last_queue_size_other_++];
      if (last_queue_size_other_ > rescale_queue_size_other_) {
        rescale_queue_size_other_++;
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 16;
        range_scale_mtf_pos_[0] += 16;
      } else {
        freq_mtf_pos_[0][last_queue_size_other_ - 1] += 4;
        range_scale_mtf_pos_[0] += 4;
      }
    } while (last_queue_size_other_ != queue_size);
  }
  if (range_scale_mtf_pos_[0] > kFreqMtfPosBot)
    rescale_mtf_queue_pos(0);
  count_ = code_ - low_;
  range_ /= range_scale_mtf_pos_[0] - unused_queue_freq_other_;
  if ((range_high_ = range_ * freq_mtf_pos_[0][0]) > count_) {
    range_ *= freq_mtf_pos_[0][0];
    freq_mtf_pos_[0][0] += kUpFreqMtfPos;
    range_scale_mtf_pos_[0] += kUpFreqMtfPos;
    return 0;
  } else {
    uint16_t* freq_ptr = &freq_mtf_pos_[0][1];
    while ((range_high_ += range_ * *freq_ptr) <= count_)
      freq_ptr++;
    const uint8_t position = freq_ptr - &freq_mtf_pos_[0][0];
    low_ += range_high_ - range_ * *freq_ptr;
    range_ *= *freq_ptr;
    if (position >= 4) {
      if (position == 4) {
        *freq_ptr += kUpFreqMtfPos - 1;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_other_ += 1;
      } else if (position == 255) {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 1;
      } else {
        *(freq_ptr - 1) += 1;
        *freq_ptr += kUpFreqMtfPos - 2;
        *(freq_ptr + 1) += 1;
        if (position == queue_size - 1)
          unused_queue_freq_other_ += 1;
      }
    } else
      *freq_ptr += kUpFreqMtfPos;
    range_scale_mtf_pos_[0] += kUpFreqMtfPos;
    return position;
  }
}

// ---------------------------------------------------------------------------
// DecoderModel -- SID / ExtraSID / INST / ERG / GoMtf / WordTag
// ---------------------------------------------------------------------------

uint8_t DecoderModel::decode_sid(uint8_t context) {
  normalize_decoder(kFreqSidBot);
  range_ /= range_scale_sid_[context];
  count_ = code_ - low_;
  if ((range_high_ = range_ * freq_sid_[context][0]) > count_) {
    range_ = range_high_;
    freq_sid_[context][0] += kUpFreqSid;
    if ((range_scale_sid_[context] += kUpFreqSid) > kFreqSidBot)
      rescale_sid(context);
    return 0;
  } else {
    uint32_t temp;
    uint8_t sid_symbol = 0;
    while ((temp = range_high_ + range_ * freq_sid_[context][++sid_symbol]) <= count_)
      range_high_ = temp;
    low_ += range_high_;
    range_ *= freq_sid_[context][sid_symbol];
    freq_sid_[context][sid_symbol] += kUpFreqSid;
    if ((range_scale_sid_[context] += kUpFreqSid) > kFreqSidBot)
      rescale_sid(context);
    return sid_symbol;
  }
}

uint32_t DecoderModel::decode_extra_sid() {
  uint32_t extra_sid;
  normalize_decoder(uint32_t{1} << 9);
  uint16_t input = (code_ - low_) / (range_ >>= 9);
  if (input < 0x80) {
    extra_sid = input >> 6;
    low_ += range_ * (input & 0x40);
    range_ *= 0x40;
  } else if (input < 0x100) {
    extra_sid = (input >> 5) - 2;
    low_ += range_ * (input & 0xE0);
    range_ *= 0x20;
  } else if (input < 0x180) {
    extra_sid = (input >> 4) - 0xA;
    low_ += range_ * (input & 0x1F0);
    range_ *= 0x10;
  } else if (input < 0x1C0) {
    extra_sid = (input >> 2) - 0x52;
    low_ += range_ * (input & 0x1FC);
    range_ *= 4;
  } else if (input < 0x1E0) {
    extra_sid = input - 0x1A2;
    low_ += range_ * input;
  } else {
    low_ += range_ * input;
    uint64_t input_code, limit, mask;
    uint8_t count, j;
    j = 0;
    input_code = input;
    do {
      j += 4;
      normalize_decoder(uint32_t{1} << 8);
      count = (code_ - low_) / (range_ >>= 8);
      low_ += range_ * count;
      input_code = (input_code << 8) + count;
      limit = ((uint64_t{1} << (4 + 2 * j)) - (uint64_t{1} << j)) << 5;
    } while (input_code >= limit);
    mask = (uint64_t{1} << (5 + j)) - 1;
    input_code -= (uint64_t{1} << (4 + 2 * j)) << 5;
    if (input_code < 0 - (uint64_t{1} << (8 + j))) {
      extra_sid = ((input_code >> 6) & mask) - (0x14 << j) - 2;
      low_ -= range_ * (count & 0x3F);
      range_ *= 0x40;
    } else if (input_code < 0 - (uint64_t{1} << (7 + j))) {
      extra_sid = ((input_code >> 4) & mask) - (8 << j) - 2;
      low_ -= range_ * (count & 0xF);
      range_ *= 0x10;
    } else if (input_code < 0 - (uint64_t{1} << (6 + j))) {
      extra_sid = ((input_code >> 2) & mask) + (0x10 << j) - 2;
      low_ -= range_ * (count & 0x3);
      range_ *= 4;
    } else {
      extra_sid = (input_code & mask) + (0x20 << j) - 2;
    }
  }
  return extra_sid;
}

uint8_t DecoderModel::decode_inst(uint8_t context, uint8_t sid_symbol) {
  normalize_decoder(kFreqInstBot);
  uint32_t extra_range = range_;
  range_ /= range_scale_inst_[context][sid_symbol];
  extra_range -= range_ * range_scale_inst_[context][sid_symbol];
  range_high_ = range_ * freq_inst_[context][sid_symbol][0] + extra_range;
  if (range_high_ > code_ - low_) {
    range_ = range_high_;
    if (range_scale_inst_[context][sid_symbol] >= (kFreqInstBot >> 1)) {
      freq_inst_[context][sid_symbol][0] += range_scale_inst_[context][sid_symbol] >> 11;
      if ((range_scale_inst_[context][sid_symbol] += range_scale_inst_[context][sid_symbol] >> 11) > kFreqInstBot)
        rescale_inst(context, sid_symbol);
    } else {
      freq_inst_[context][sid_symbol][0] += kUpFreqInst;
      range_scale_inst_[context][sid_symbol] += kUpFreqInst;
    }
    return 0;
  } else {
    uint32_t temp;
    count_ = code_ - low_;
    uint8_t instances = 0;
    while ((temp = range_high_ + range_ * freq_inst_[context][sid_symbol][++instances]) <= count_)
      range_high_ = temp;
    low_ += range_high_;
    range_ *= freq_inst_[context][sid_symbol][instances];
    if (range_scale_inst_[context][sid_symbol] >= (kFreqInstBot >> 1)) {
      freq_inst_[context][sid_symbol][instances] += range_scale_inst_[context][sid_symbol] >> 11;
      if ((range_scale_inst_[context][sid_symbol] += (range_scale_inst_[context][sid_symbol] >> 11)) > kFreqInstBot)
        rescale_inst(context, sid_symbol);
    } else {
      freq_inst_[context][sid_symbol][instances] += kUpFreqInst;
      range_scale_inst_[context][sid_symbol] += kUpFreqInst;
    }
    return instances;
  }
}

uint8_t DecoderModel::decode_erg(uint16_t ctx1, uint16_t ctx2) {
  normalize_decoder(kFreqErgBot);
  if ((freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]) * (range_ >>= 13) > code_ - low_) {
    range_ *= freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2];
    freq_erg_[0] += (0x400 - freq_erg_[0]) >> 2;
    freq_erg_[ctx1] += (0x1000 - freq_erg_[ctx1]) >> 4;
    freq_erg_[ctx2] += (0xC00 - freq_erg_[ctx2]) >> 3;
    return 0;
  } else {
    low_ += range_ * (freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]);
    range_ *= 0x2000 - (freq_erg_[0] + freq_erg_[ctx1] + freq_erg_[ctx2]);
    freq_erg_[0] -= freq_erg_[0] >> 2;
    freq_erg_[ctx1] -= freq_erg_[ctx1] >> 4;
    freq_erg_[ctx2] -= freq_erg_[ctx2] >> 3;
    return 1;
  }
}

uint8_t DecoderModel::decode_go_mtf(uint16_t ctx1, uint8_t ctx2) {
  uint8_t go_mtf;
  normalize_decoder(kFreqGoMtfBot);
  const uint32_t extra_range = range_ & (kFreqGoMtfBot - 1);
  uint16_t c1 = ctx1 + 0xF0 * ctx2;
  uint16_t c3 = c1 + 0x2D0;
  if ((freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]) * (range_ >>= 13) + extra_range > code_ - low_) {
    range_ = range_ * (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]) + extra_range;
    freq_go_mtf_[c1] += (0x800 - freq_go_mtf_[c1]) >> 2;
    freq_go_mtf_[ctx2] += (0x800 - freq_go_mtf_[ctx2]) >> 2;
    freq_go_mtf_[c3] += (0x800 - freq_go_mtf_[c3]) >> 6;
    go_mtf = 0;
  } else {
    low_ += range_ * (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]) + extra_range;
    range_ *= 0x2000 - (freq_go_mtf_[c1] + freq_go_mtf_[ctx2] + 2 * freq_go_mtf_[c3]);
    freq_go_mtf_[c1] -= freq_go_mtf_[c1] >> 2;
    freq_go_mtf_[ctx2] -= freq_go_mtf_[ctx2] >> 2;
    freq_go_mtf_[c3] -= freq_go_mtf_[c3] >> 6;
    go_mtf = 1;
  }
  return go_mtf;
}

uint8_t DecoderModel::decode_word_tag(uint8_t context) {
  uint8_t tag;
  normalize_decoder(kFreqWordTagBot);
  if (freq_word_tag_[context] * (range_ >>= 12) > code_ - low_) {
    range_ = range_ * freq_word_tag_[context];
    freq_word_tag_[context] += (0x1000 - freq_word_tag_[context]) >> 4;
    tag = 0;
  } else {
    low_ += freq_word_tag_[context] * range_;
    range_ *= 0x1000 - freq_word_tag_[context];
    freq_word_tag_[context] -= freq_word_tag_[context] >> 4;
    tag = 1;
  }
  return tag;
}

// ---------------------------------------------------------------------------
// DecoderModel -- bin / base symbol / first char decoding
// ---------------------------------------------------------------------------

uint16_t DecoderModel::decode_bin(uint16_t bins) {
  normalize_decoder(uint32_t{1} << 12);
  const uint16_t bin_num = (code_ - low_) / (range_ /= bins);
  low_ += range_ * bin_num;
  return bin_num;
}

uint32_t DecoderModel::decode_bin_code(uint8_t bits) {
  normalize_decoder(uint32_t{1} << bits);
  const uint32_t bin_code = (code_ - low_) / (range_ >>= bits);
  low_ += bin_code * range_;
  return bin_code;
}

uint32_t DecoderModel::decode_base_symbol(uint32_t num_base_symbols) {
  normalize_decoder(num_base_symbols);
  range_ /= num_base_symbols;
  const uint32_t base_symbol = (code_ - low_) / range_;
  low_ += range_ * base_symbol;
  return base_symbol;
}

uint32_t DecoderModel::decode_base_symbol_cap(uint32_t num_base_symbols) {
  normalize_decoder(num_base_symbols);
  range_ /= num_base_symbols - 24;
  const uint32_t base_symbol = (code_ - low_) / range_;
  low_ += range_ * base_symbol;
  return base_symbol;
}

uint8_t DecoderModel::decode_first_char(uint8_t sym_type, uint8_t last_char) {
  uint16_t* range_scale_ptr = &range_scale_first_char_[sym_type][last_char];
  uint32_t extra_range;
  FirstCharData* fc_data_ptr = &first_char_data_[sym_type][last_char][0];

  normalize_decoder(static_cast<uint32_t>(kFreqFirstCharBot));
  extra_range = range_;
  range_ /= *range_scale_ptr;
  extra_range -= range_ * *range_scale_ptr;
  range_high_ = range_ * fc_data_ptr->data.freq + extra_range;
  count_ = code_ - low_;
  if (range_high_ > count_) {
    range_ = range_high_;
  } else {
    uint32_t temp;
    while ((temp = range_high_ + range_ * (++fc_data_ptr)->data.freq) <= count_)
      range_high_ = temp;
    low_ += range_high_;
    range_ *= fc_data_ptr->data.freq;
  }
  const uint8_t first_char = fc_data_ptr->data.symbol;

  if (*range_scale_ptr >= (kFreqFirstCharBot >> 2)) {
    fc_data_ptr->data.freq += *range_scale_ptr >> 10;
    if ((*range_scale_ptr += (*range_scale_ptr >> 10)) >= kFreqFirstCharBot)
      rescale_first_char(sym_type, last_char);
  } else {
    fc_data_ptr->data.freq += kUpFreqFirstChar;
    *range_scale_ptr += kUpFreqFirstChar;
  }

  if (fc_data_ptr != &first_char_data_[sym_type][last_char][0]) {
    if (fc_data_ptr->data.freq > (fc_data_ptr - 1)->data.freq) {
      FirstCharData saved_data;
      saved_data.all_data = fc_data_ptr->all_data;
      do {
        fc_data_ptr->all_data = (fc_data_ptr - 1)->all_data;
        fc_data_ptr--;
      } while ((fc_data_ptr != &first_char_data_[sym_type][last_char][0]) && (saved_data.data.freq > (fc_data_ptr - 1)->data.freq));
      fc_data_ptr->all_data = saved_data.all_data;
    }
  }
  return first_char;
}

uint8_t DecoderModel::decode_first_char_binary(uint8_t last_char) {
  uint16_t* range_scale_ptr = &range_scale_first_char_[0][last_char];
  uint32_t extra_range, temp;
  FirstCharData* fc_data_ptr = &first_char_data_[0][last_char][0];

  normalize_decoder(kFreqFirstCharBot);
  extra_range = range_;
  uint8_t section_index = 0;
  range_ /= static_cast<uint32_t>(*range_scale_ptr);
  extra_range -= range_ * static_cast<uint32_t>(*range_scale_ptr);
  count_ = code_ - low_;
  range_high_ = extra_range;
  while ((section_index != 7)
      && ((temp = range_high_ + range_ * range_scale_first_char_section_[last_char][section_index]) <= count_)) {
    range_high_ = temp;
    section_index++;
  }
  fc_data_ptr = &first_char_data_[0][last_char][0x20 * section_index];
  while ((temp = range_high_ + range_ * fc_data_ptr->data.freq) <= count_) {
    range_high_ = temp;
    fc_data_ptr++;
  }
  const uint8_t first_char = fc_data_ptr - &first_char_data_[0][last_char][0];
  if (first_char == 0)
    range_ = temp;
  else {
    low_ += range_high_;
    range_ *= static_cast<uint32_t>(fc_data_ptr->data.freq);
  }

  if (*range_scale_ptr >= (kFreqFirstCharBot >> 2)) {
    fc_data_ptr->data.freq += *range_scale_ptr >> 10;
    if (section_index <= 6)
      range_scale_first_char_section_[last_char][section_index] += *range_scale_ptr >> 10;
    if ((*range_scale_ptr += *range_scale_ptr >> 10) >= kFreqFirstCharBot)
      rescale_first_char_binary(last_char);
  } else {
    fc_data_ptr->data.freq += kUpFreqFirstChar >> 1;
    if (section_index <= 6)
      range_scale_first_char_section_[last_char][section_index] += kUpFreqFirstChar >> 1;
    *range_scale_ptr += kUpFreqFirstChar >> 1;
  }
  return first_char;
}

}  // namespace glza
