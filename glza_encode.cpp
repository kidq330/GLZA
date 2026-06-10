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

#include "glza_encode.h"
#include "glza_error.h"
#include <cmath>
#include <cstring>

namespace glza {

namespace {
constexpr uint32_t kUniqueSymbol = 0xFFFFFFFF;
}  // namespace


bool Encoder::encode_queue_ok() const {
  return model_.read_encoder_failed() == 0;
}


void Encoder::encode_queue_fail(const char* reason) {
  model_.set_encoder_failed(reason);
}


void Encoder::queue_subcount_inc(uint16_t& subcount, const char* which) {
  if (subcount >= 0xFF) {
    fprintf(stderr, "GLZA encode: %s queue overflow (count=%u, total=%u az=%u space=%u other=%u)\n",
        which, static_cast<unsigned>(subcount), static_cast<unsigned>(queue_size_),
        static_cast<unsigned>(queue_size_az_), static_cast<unsigned>(queue_size_space_),
        static_cast<unsigned>(queue_size_other_));
    encode_queue_fail("MTF sub-queue overflow");
    return;
  }
  subcount++;
}


void Encoder::queue_subcount_dec(uint16_t& subcount, const char* which) {
  if (subcount == 0) {
    fprintf(stderr, "GLZA encode: %s queue underflow (total=%u az=%u space=%u other=%u)\n",
        which, static_cast<unsigned>(queue_size_), static_cast<unsigned>(queue_size_az_),
        static_cast<unsigned>(queue_size_space_), static_cast<unsigned>(queue_size_other_));
    encode_queue_fail("MTF sub-queue underflow");
    return;
  }
  subcount--;
}


// type:  bit 0: string starts a-z, bit 1: non-ergodic, bit 2: "word" ending determined,
//        bits 4-3: 0: not a word, 1: word, 2: word & >= 15 repeats (ending sub)symbol & likely followed by ' ',
//                  3: word & >= 15 repeats (ending sub)symbol
//        bit 5: string ends 'C' or 'B' (cap symbol/cap lock symbol), bit 6: in queue, bit 7: sent MTF


void Encoder::print_string(uint32_t symbol_number) {
  if (symbol_number < num_base_symbols_) {
    if (UTF8_compliant_ != 0) {
      if (symbol_number < kStartUtf8_2Byte)
        printf("%c", static_cast<unsigned char>(symbol_number));
      else if (symbol_number < kStartUtf8_3Byte) {
        printf("%c", static_cast<unsigned char>(symbol_number >> 6) + 0xC0);
        printf("%c", static_cast<unsigned char>(symbol_number & 0x3F) + 0x80);
      } else if (symbol_number < kStartUtf8_4Byte) {
        printf("%c", static_cast<unsigned char>(symbol_number >> 12) + 0xE0);
        printf("%c", static_cast<unsigned char>((symbol_number >> 6) & 0x3F) + 0x80);
        printf("%c", static_cast<unsigned char>(symbol_number & 0x3F) + 0x80);
      } else {
        printf("%c", static_cast<unsigned char>(symbol_number >> 18) + 0xF0);
        printf("%c", static_cast<unsigned char>((symbol_number >> 12) & 0x3F) + 0x80);
        printf("%c", static_cast<unsigned char>((symbol_number >> 6) & 0x3F) + 0x80);
        printf("%c", static_cast<unsigned char>(symbol_number & 0x3F) + 0x80);
      }
    } else
      printf("%c", static_cast<unsigned char>(symbol_number));
  } else {
    uint32_t* symbol_ptr = symbol_array_.data() + sd_[symbol_number].symbol_start_index;
    uint32_t* next_symbol_ptr = symbol_array_.data() + sd_[symbol_number + 1].symbol_start_index - 1;
    while (symbol_ptr != next_symbol_ptr)
      print_string(*symbol_ptr++);
  }
}


uint32_t Encoder::find_string_length(uint32_t symbol_number) {
  if (symbol_number < num_base_symbols_) {
    if (UTF8_compliant_ != 0) {
      if (symbol_number >= kStartUtf8_4Byte)
        return 4;
      else if (symbol_number >= kStartUtf8_3Byte)
        return 3;
      else if (symbol_number >= kStartUtf8_2Byte)
        return 2;
    }
    return 1;
  }
  uint32_t symbol_size = 0;
  uint32_t* symbol_ptr = symbol_array_.data() + sd_[symbol_number].symbol_start_index;
  uint32_t* next_symbol_ptr = symbol_array_.data() + sd_[symbol_number + 1].symbol_start_index - 1;
  while (symbol_ptr != next_symbol_ptr)
    symbol_size += find_string_length(*symbol_ptr++);
  return symbol_size;
}


uint32_t Encoder::sum_dictionary_string_bytes(uint32_t num_codes_val, const uint32_t* first_define) {
  uint32_t total = 0;
  std::vector<uint8_t> visited(num_codes_val, 0);

  const uint32_t* walk_ptr = symbol_array_.data();
  while (walk_ptr < first_define) {
    const uint32_t sym = *walk_ptr++;
    if (sym < num_codes_val && visited[sym] == 0) {
      visited[sym] = 1;
      total += find_string_length(sym);
    }
  }
  return total;
}


void Encoder::get_symbol_category(uint32_t symbol_number, uint8_t* sym_type_ptr) {
  if (symbol_number >= num_base_symbols_) {
    if ((sd_[symbol_number].type & 8) != 0) {
      *sym_type_ptr |= 0xC;
      return;
    }
    uint32_t* string_ptr = symbol_array_.data() + sd_[symbol_number + 1].symbol_start_index - 2;
    get_symbol_category(*string_ptr, sym_type_ptr);
    while (((*sym_type_ptr & 4) == 0) && (string_ptr != symbol_array_.data() + sd_[symbol_number].symbol_start_index))
      get_symbol_category(*--string_ptr, sym_type_ptr);
    if ((sd_[symbol_number].type & 4) == 0)
      sd_[symbol_number].type |= *sym_type_ptr & 0xC;
  } else if (symbol_number == static_cast<uint32_t>(' '))
    *sym_type_ptr |= 0xC;
}


uint8_t Encoder::find_first(uint32_t symbol_number) {
  const uint32_t first_symbol = symbol_array_[sd_[symbol_number].symbol_start_index];
  if ((first_symbol >= 0x100) && (sd_[first_symbol].starts == 0))
    sd_[first_symbol].starts = find_first(first_symbol);
  return sd_[first_symbol].starts;
}


uint8_t Encoder::find_first_UTF8(uint32_t symbol_number) {
  const uint32_t first_symbol = symbol_array_[sd_[symbol_number].symbol_start_index];
  if ((first_symbol >= num_base_symbols_) && (sd_[first_symbol].starts == 0))
    sd_[first_symbol].starts = find_first_UTF8(first_symbol);
  return sd_[first_symbol].starts;
}


uint8_t Encoder::find_last(uint32_t symbol_number) {
  const uint32_t last_symbol = symbol_array_[sd_[symbol_number + 1].symbol_start_index - 2];
  if ((last_symbol >= 0x100) && (sd_[last_symbol].ends == 0))
    sd_[last_symbol].ends = find_last(last_symbol);
  return sd_[last_symbol].ends;
}


uint8_t Encoder::find_last_UTF8(uint32_t symbol_number) {
  const uint32_t last_symbol = symbol_array_[sd_[symbol_number + 1].symbol_start_index - 2];
  if ((last_symbol >= num_base_symbols_) && (sd_[last_symbol].ends == 0))
    sd_[last_symbol].ends = find_last_UTF8(last_symbol);
  return sd_[last_symbol].ends;
}


uint8_t Encoder::add_dictionary_symbol(uint32_t symbol, uint8_t bits) {
  const uint8_t first_char = sd_[symbol].starts;
  if (nsob_[first_char][bits] == (uint32_t{1} << sym_list_bits_[first_char][bits])) {
    sym_list_bits_[first_char][bits]++;
    try {
      sym_list_ptrs_[first_char][bits].resize(1u << sym_list_bits_[first_char][bits]);
    } catch (...) {
      fprintf(stderr, "FATAL ERROR - symbol list realloc failure\n");
      return 0;
    }
  }
  sd_[symbol].array_index = nsob_[first_char][bits];
  sym_list_ptrs_[first_char][bits][nsob_[first_char][bits]] = symbol;
  if ((nsob_[first_char][bits]++ << (32 - bits)) == (static_cast<uint32_t>(nbob_[first_char][bits]) << (32 - bin_code_length_[first_char]))) {
    if (bits >= bin_code_length_[first_char]) {
      nbob_[first_char][bits]++;
      if (sum_nbob_[first_char] < 0x1000) {
        sum_nbob_[first_char]++;
        while (bits++ != max_code_length_)
          fbob_[first_char][bits]++;
      } else {
        bin_code_length_[first_char]--;
        sum_nbob_[first_char] = 0;
        for (bits = 1; bits <= max_code_length_; bits++) {
          fbob_[first_char][bits] = sum_nbob_[first_char];
          sum_nbob_[first_char] += (nbob_[first_char][bits] = (nbob_[first_char][bits] + 1) >> 1);
        }
      }
    } else {
      uint32_t new_bins = 1u << (bin_code_length_[first_char] - bits);
      if (sum_nbob_[first_char] + new_bins <= 0x1000) {
        sum_nbob_[first_char] += new_bins;
        nbob_[first_char][bits] += new_bins;
        while (bits++ != max_code_length_)
          fbob_[first_char][bits] += new_bins;
      } else {
        if (new_bins <= 0x1000) {
          nbob_[first_char][bits] += new_bins;
          do {
            bin_code_length_[first_char]--;
            sum_nbob_[first_char] = 0;
            for (bits = 1; bits <= max_code_length_; bits++)
              sum_nbob_[first_char] += (nbob_[first_char][bits] = (nbob_[first_char][bits] + 1) >> 1);
          } while (sum_nbob_[first_char] > 0x1000);
        } else {
          uint8_t bin_shift = bin_code_length_[first_char] - 12 - bits;
          if (sum_nbob_[first_char] != 0)
            bin_shift++;
          bin_code_length_[first_char] -= bin_shift;
          sum_nbob_[first_char] = 0;
          uint8_t code_length;
          for (code_length = 1; code_length <= max_code_length_; code_length++)
            sum_nbob_[first_char] += (nbob_[first_char][code_length]
                = ((nbob_[first_char][code_length] - 1) >> bin_shift) + 1);
          nbob_[first_char][bits] += new_bins >> bin_shift;
          sum_nbob_[first_char] += new_bins >> bin_shift;
        }
        uint16_t bin = nbob_[first_char][1];
        for (bits = 2; bits <= max_code_length_; bits++) {
          fbob_[first_char][bits] = bin;
          bin += nbob_[first_char][bits];
        }
      }
    }
  }
  return 1;
}


void Encoder::remove_dictionary_symbol(uint32_t symbol, uint8_t bits) {
  const uint8_t first_char = sd_[symbol].starts;
  const uint32_t last_symbol = sym_list_ptrs_[first_char][bits][--nsob_[first_char][bits]];
  sym_list_ptrs_[first_char][bits][sd_[symbol].array_index] = last_symbol;
  sd_[last_symbol].array_index = sd_[symbol].array_index;
}


void Encoder::add_symbol_to_queue(uint32_t symbol_number) {
  if (!encode_queue_ok())
    return;
  if (queue_size_ >= 0x100) {
    fprintf(stderr, "GLZA encode: unified MTF queue full (256 entries)\n");
    encode_queue_fail("MTF queue full");
    return;
  }
  queue_size_++;
  sd_[symbol_number].type |= 0xC0;
  queue_[static_cast<uint8_t>(--queue_offset_)] = symbol_number;
  if ((sd_[symbol_number].type & 1) != 0)
    queue_subcount_inc(queue_size_az_, "az");
  else if (sd_[symbol_number].starts == 0x20)
    queue_subcount_inc(queue_size_space_, "space");
  else
    queue_subcount_inc(queue_size_other_, "other");
}


void Encoder::update_queue(uint32_t symbol_number, uint8_t in_definition) {
  uint16_t queue_position = 0;
  uint32_t az_queue_position = 0;
  uint32_t space_queue_position = 0;
  uint32_t other_queue_position = 0;

  if (!encode_queue_ok())
    return;
  while (queue_position < queue_size_
      && symbol_number != queue_[static_cast<uint8_t>(queue_position + queue_offset_)]) {
    if ((sd_[queue_[static_cast<uint8_t>(queue_position + queue_offset_)]].type & 1) != 0)
      az_queue_position++;
    else if (sd_[queue_[static_cast<uint8_t>(queue_position + queue_offset_)]].starts == 0x20)
      space_queue_position++;
    else
      other_queue_position++;
    queue_position++;
  }
  if (queue_position >= queue_size_
      || symbol_number != queue_[static_cast<uint8_t>(queue_position + queue_offset_)]) {
    fprintf(stderr,
        "GLZA encode: symbol %u not in MTF queue (queue_size=%u in_definition=%u)\n",
        static_cast<unsigned>(symbol_number), static_cast<unsigned>(queue_size_),
        static_cast<unsigned>(in_definition));
    encode_queue_fail("MTF symbol not in queue");
    return;
  }

  if (cap_encoded_ != 0) {
    model_.encode_mtf_type(in_definition, 4 + in_definition + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7),
        prior_end_);
    if (sd_[symbol_number].starts == 0x20) {
      if (prior_end_ != 0xA)
        model_.encode_mtf_first(((sd_[prior_symbol_].type & 0x18) == 0x10), 1, queue_size_other_, queue_size_space_, queue_size_az_);
      model_.encode_mtf_pos_space(space_queue_position, queue_size_space_);
    } else if ((sd_[symbol_number].type & 1) != 0) {
      model_.encode_mtf_first(((sd_[prior_symbol_].type & 0x18) == 0x10), 2, queue_size_other_, queue_size_space_, queue_size_az_);
      model_.encode_mtf_pos_az(az_queue_position, queue_size_az_);
    } else {
      model_.encode_mtf_first(((sd_[prior_symbol_].type & 0x18) == 0x10), 0, queue_size_other_, queue_size_space_, queue_size_az_);
      model_.encode_mtf_pos_other(other_queue_position, queue_size_other_);
    }
  } else {
    model_.encode_mtf_type_binary(in_definition, prior_end_);
    model_.encode_mtf_pos(queue_position, queue_size_);
  }

  while (queue_position != 0) {
    queue_[static_cast<uint8_t>(queue_offset_ + queue_position)] = queue_[static_cast<uint8_t>(queue_offset_ + queue_position - 1)];
    queue_position--;
  }
  if (!encode_queue_ok())
    return;
  if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
    uint16_t context = 6 * (sd_[symbol_number].count - 1);
    if (cap_encoded_ != 0)
      context += (sd_[symbol_number].type & 1) + 3 * ((sd_[symbol_number].type & 0x18) == 0x10);
    model_.encode_go_mtf(context, 1, 1);
    queue_[queue_offset_] = symbol_number;
  } else {
    queue_size_--;
    if ((sd_[symbol_number].type & 1) != 0)
      queue_subcount_dec(queue_size_az_, "az");
    else if (sd_[symbol_number].starts == 0x20)
      queue_subcount_dec(queue_size_space_, "space");
    else
      queue_subcount_dec(queue_size_other_, "other");

    queue_offset_++;
    if ((sd_[symbol_number].count > kMaxInstancesForRemove)
        || (sd_[symbol_number].hits != sd_[symbol_number].count)) {
      uint16_t context = 6 * (sd_[symbol_number].count - 1);
      if (cap_encoded_ != 0)
        context += (sd_[symbol_number].type & 1) + 3 * ((sd_[symbol_number].type & 0x18) == 0x10);
      model_.encode_go_mtf(context, 1, 0);
      sd_[symbol_number].type &= 0xBF;
      add_dictionary_symbol(symbol_number, sd_[symbol_number].code_length);
    }
  }
}


void Encoder::update_queue_prior_cap(uint32_t symbol_number, uint8_t in_definition) {
  uint16_t queue_position = 0;
  uint8_t az_queue_position = 0;

  if (!encode_queue_ok())
    return;
  if ((sd_[symbol_number].type & 1) == 0) {
    fprintf(stderr,
        "GLZA encode: update_queue_prior_cap on non-az symbol %u (use unified queue path)\n",
        static_cast<unsigned>(symbol_number));
    encode_queue_fail("MTF cap-path on non-az symbol");
    return;
  }
  while (queue_position < queue_size_
      && symbol_number != queue_[static_cast<uint8_t>(queue_position + queue_offset_)]) {
    if ((sd_[queue_[static_cast<uint8_t>(queue_position + queue_offset_)]].type & 1) != 0)
      az_queue_position++;
    queue_position++;
  }
  if (queue_position >= queue_size_
      || symbol_number != queue_[static_cast<uint8_t>(queue_position + queue_offset_)]) {
    fprintf(stderr,
        "GLZA encode: az-cap symbol %u not in MTF queue (queue_size=%u az=%u)\n",
        static_cast<unsigned>(symbol_number), static_cast<unsigned>(queue_size_),
        static_cast<unsigned>(queue_size_az_));
    encode_queue_fail("MTF az-cap symbol not in queue");
    return;
  }
  while (queue_position != 0) {
    queue_[static_cast<uint8_t>(queue_offset_ + queue_position)] = queue_[static_cast<uint8_t>(queue_offset_ + queue_position - 1)];
    queue_position--;
  }
  if (!encode_queue_ok())
    return;
  model_.encode_mtf_type(2 + in_definition, 0x2C + 4 * in_definition + (sd_[prior_symbol_].type & 3), 'C');
  model_.encode_mtf_pos_az(az_queue_position, queue_size_az_);
  if (!encode_queue_ok())
    return;
  if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
    uint16_t context = 6 * (sd_[symbol_number].count - 1) + 2 + 3 * ((sd_[symbol_number].type & 0x18) == 0x10);
    model_.encode_go_mtf(context, 1, 1);
    queue_[queue_offset_] = symbol_number;
  } else {
    queue_size_--;
    queue_subcount_dec(queue_size_az_, "az");
    queue_offset_++;
    if ((sd_[symbol_number].count > kMaxInstancesForRemove) || (sd_[symbol_number].hits != sd_[symbol_number].count)) {
      uint16_t context = 6 * (sd_[symbol_number].count - 1) + 2 + 3 * ((sd_[symbol_number].type & 0x18) == 0x10);
      model_.encode_go_mtf(context, 1, 0);
      sd_[symbol_number].type &= 0xBF;
      add_dictionary_symbol(symbol_number, sd_[symbol_number].code_length);
    }
  }
}


uint16_t Encoder::get_mtf_overflow_position(const SymbolData* sd, const uint32_t* queue,
    uint16_t queue_position, uint32_t num_transmits_over_sqrt2) {
  uint16_t limit_position = 0;
  if ((queue_position > 0x1C) && ((sd[queue[0x1C]].count <= (num_transmits_over_sqrt2 >> 10))
      && (sd[queue[0x1C]].count > (num_transmits_over_sqrt2 >> 11))))
    limit_position = 0x1C;
  else if ((queue_position > 0x30) && ((sd[queue[0x30]].count <= (num_transmits_over_sqrt2 >> 11))
      && (sd[queue[0x30]].count > (num_transmits_over_sqrt2 >> 12))))
    limit_position = 0x30;
  else if ((queue_position > 0x44) && ((sd[queue[0x44]].count <= (num_transmits_over_sqrt2 >> 12))
      && (sd[queue[0x44]].count > (num_transmits_over_sqrt2 >> 13))))
    limit_position = 0x44;
  else if ((queue_position > 0x48) && ((sd[queue[0x48]].count <= (num_transmits_over_sqrt2 >> 13))
      && (sd[queue[0x48]].count > (num_transmits_over_sqrt2 >> 14))))
    limit_position = 0x48;
  else if ((queue_position > 0x88) && ((sd[queue[0x88]].count <= (num_transmits_over_sqrt2 >> 14))
      && (sd[queue[0x88]].count > (num_transmits_over_sqrt2 >> 15))))
    limit_position = 0x88;
  else if ((queue_position > 0xB0) && ((sd[queue[0xB0]].count <= (num_transmits_over_sqrt2 >> 15))
      && (sd[queue[0xB0]].count > (num_transmits_over_sqrt2 >> 16))))
    limit_position = 0xB0;
  else if ((queue_position > 0xD8) && ((sd[queue[0xD8]].count <= (num_transmits_over_sqrt2 >> 16))
      && (sd[queue[0xD8]].count > (num_transmits_over_sqrt2 >> 17))))
    limit_position = 0xD8;
  else if ((queue_position > 0xF0) && ((sd[queue[0xF0]].count <= (num_transmits_over_sqrt2 >> 17))
      && (sd[queue[0xF0]].count > (num_transmits_over_sqrt2 >> 18))))
    limit_position = 0xF0;
  else if ((queue_position > 0x108) && ((sd[queue[0x108]].count <= (num_transmits_over_sqrt2 >> 18))
      && (sd[queue[0x108]].count > (num_transmits_over_sqrt2 >> 19))))
    limit_position = 0x108;
  else if ((queue_position > 0x120) && ((sd[queue[0x120]].count <= (num_transmits_over_sqrt2 >> 19))
      && (sd[queue[0x120]].count > (num_transmits_over_sqrt2 >> 20))))
    limit_position = 0x120;
  else if ((queue_position > 0x138) && ((sd[queue[0x138]].count <= (num_transmits_over_sqrt2 >> 20))
      && (sd[queue[0x138]].count > (num_transmits_over_sqrt2 >> 21))))
    limit_position = 0x138;
  return limit_position;
}


void Encoder::add_mtf_hit_scores(SymbolData* sd, uint16_t queue_position,
    uint32_t num_transmits_over_sqrt2) {
  if (queue_position <= 1)
    sd->hits += 65;
  else if (queue_position <= 4)
    sd->hits += 67 - 3 * queue_position;
  else if (queue_position <= 8)
    sd->hits += 62 - 2 * queue_position;
  else if (queue_position <= 0xF)
    sd->hits += 53 - queue_position;
  else if (queue_position <= 0x1F) {
    if (sd->count <= (num_transmits_over_sqrt2 >> 12))
      sd->hits += 45 - (queue_position >> 1);
  } else if (queue_position <= 0x3F) {
    if (sd->count <= (num_transmits_over_sqrt2 >> 13))
      sd->hits += 37 - (queue_position >> 2);
  } else if (queue_position <= 0x7F) {
    if (sd->count <= (num_transmits_over_sqrt2 >> 14))
      sd->hits += 29 - (queue_position >> 3);
  } else {
    if (sd->count <= (num_transmits_over_sqrt2 >> 15))
      sd->hits += 21 - (queue_position >> 4);
  }
}


void Encoder::encode_dictionary_symbol(uint32_t symbol) {
  uint32_t symbol_index, bin_code;
  uint16_t bin_num;
  uint8_t first_char, code_length, bin_shift;

  first_char = sd_[symbol].starts;
  symbol_index = sd_[symbol].array_index;
  code_length = sd_[symbol].code_length;
  if (cap_encoded_ != 0) {
    if (prior_end_ != 0xA)
      model_.encode_first_char(first_char, (sd_[prior_symbol_].type & 0x18) >> 3, prior_end_);
  }
  else if (UTF8_compliant_ != 0)
    model_.encode_first_char(first_char, 0, prior_end_);
  else
    model_.encode_first_char_binary(first_char, prior_end_);

  if (code_length > bin_code_length_[first_char]) {
    uint32_t max_codes_in_bins, mcib;
    uint8_t reduce_bits = 0;
    bin_shift = code_length - bin_code_length_[first_char];
    max_codes_in_bins = static_cast<uint32_t>(nbob_[first_char][code_length]) << bin_shift;
    mcib = max_codes_in_bins >> 1;
    while (mcib >= nsob_[first_char][code_length]) {
      reduce_bits++;
      mcib = mcib >> 1;
    }
    if (bin_shift > reduce_bits) {
      bin_num = fbob_[first_char][code_length];
      const uint32_t min_extra_reduce_index = 2 * nsob_[first_char][code_length] - (max_codes_in_bins >> reduce_bits);
      uint16_t symbol_bins, code_bin;
      code_length = bin_shift - reduce_bits;
      if (symbol_index >= min_extra_reduce_index) {
        bin_code = 2 * symbol_index - min_extra_reduce_index;
        code_bin = static_cast<uint16_t>(bin_code >> code_length);
        bin_code -= static_cast<uint32_t>(code_bin) << code_length;
        symbol_bins = 2;
      } else {
        bin_code = symbol_index;
        code_bin = static_cast<uint16_t>(symbol_index >> code_length);
        bin_code -= static_cast<uint32_t>(code_bin) << code_length;
        symbol_bins = 1;
      }
      bin_num += code_bin;
      model_.encode_long_dictionary_symbol(bin_code, bin_num, sum_nbob_[first_char], code_length, symbol_bins);
      return;
    }
  }
  uint16_t bins_per_symbol = nbob_[first_char][code_length] / nsob_[first_char][code_length];
  uint16_t extra_bins = nbob_[first_char][code_length] - nsob_[first_char][code_length] * bins_per_symbol;
  bin_num = fbob_[first_char][code_length] + symbol_index * bins_per_symbol;
  if (symbol_index >= extra_bins)
    bin_num += extra_bins;
  else {
    bin_num += symbol_index;
    bins_per_symbol++;
  }
  model_.encode_short_dictionary_symbol(bin_num, sum_nbob_[first_char], bins_per_symbol);
}


uint32_t Encoder::count_symbols(uint32_t symbol) {
  if (symbol < num_base_symbols_)
    return 1;
  uint32_t* symbol_string_ptr = symbol_array_.data() + sd_[symbol].symbol_start_index;
  uint32_t* end_symbol_string_ptr = symbol_array_.data() + sd_[symbol + 1].symbol_start_index - 1;
  uint32_t string_symbols = 0;
  while (symbol_string_ptr != end_symbol_string_ptr) {
    if ((sd_[*symbol_string_ptr].count == 1) && (*symbol_string_ptr >= num_base_symbols_))
      string_symbols += count_symbols(*symbol_string_ptr);
    else
      string_symbols++;
    symbol_string_ptr++;
  }
  return string_symbols;
}


void Encoder::get_embedded_symbols(uint32_t define_symbol) {
  if (define_symbol >= num_base_symbols_) {
    uint32_t* define_string_ptr = symbol_array_.data() + sd_[define_symbol].symbol_start_index;
    uint32_t* define_string_end_ptr = symbol_array_.data() + sd_[define_symbol + 1].symbol_start_index - 1;
    do {
      const uint32_t symbol = *define_string_ptr++;
      sd_[symbol].hits++;
      if (sd_[symbol].previous == 0xFFFFFFFF)
        get_embedded_symbols(symbol);
      else {
        transmits_[sd_[symbol].previous].distance = num_transmits_ - sd_[symbol].previous;
        transmits_[num_transmits_].distance = 0xFFFFFFFF;
        sd_[symbol].previous2 = sd_[symbol].previous;
        sd_[symbol].previous = num_transmits_++;
      }
    } while (define_string_ptr != define_string_end_ptr);
  }
  if (sd_[define_symbol].count != 1)
    sd_[define_symbol].previous = num_transmits_++;
}


void Encoder::get_embedded_symbols2(uint32_t define_symbol) {
  if (define_symbol >= num_base_symbols_) {
    uint32_t* define_string_ptr = symbol_array_.data() + sd_[define_symbol].symbol_start_index;
    uint32_t* define_string_end_ptr = symbol_array_.data() + sd_[define_symbol + 1].symbol_start_index - 1;
    uint32_t symbol;
    do {
      symbol = *define_string_ptr++;
      if (sd_[symbol].previous == 0xFFFFFFFF)
        get_embedded_symbols2(symbol);
      else {
        transmits_[sd_[symbol].previous].distance = num_transmits_ - sd_[symbol].previous;
        transmits_[num_transmits_].symbol = symbol;
        transmits_[num_transmits_].distance = 0xFFFFFFFF;
        sd_[symbol].previous = num_transmits_++;
        if ((sd_[prior_symbol_].type & 8) != 0) {
          if (sd_[symbol].starts == 0x20)
            sd_[prior_symbol_].space_score++;
          else
            sd_[prior_symbol_].space_score -= 5;
        }
        prior_symbol_ = symbol;
      }
    } while (define_string_ptr != define_string_end_ptr);
    define_string_ptr--;
    sd_[define_symbol].type |= sd_[symbol].type & 0xC;
    while (((sd_[define_symbol].type & 4) == 0)
        && (define_string_ptr-- != symbol_array_.data() + sd_[define_symbol].symbol_start_index))
      get_symbol_category(*define_string_ptr, &sd_[define_symbol].type);
  }
  prior_symbol_ = define_symbol;
  if (sd_[define_symbol].count != 1) {
    sd_[define_symbol].previous = num_transmits_;
    transmits_[num_transmits_++].symbol = define_symbol;
  }
}


uint8_t Encoder::embed_define(uint32_t define_symbol, uint8_t in_definition) {
  uint32_t *define_string_ptr, *define_symbol_start_ptr, *define_string_end_ptr;
  uint32_t define_symbol_instances, symbols_in_definition, symbol, symbol_inst;
  uint8_t new_symbol_code_length, SID_symbol;
  uint8_t tag_type = 0;
  const uint8_t saved_prior_is_cap = prior_is_cap_;

  if (model_.read_encoder_failed() != 0)
    return 0;

  define_symbol_instances = sd_[define_symbol].count;
  if (define_symbol_instances != 1)
    new_symbol_code_length = sd_[define_symbol].code_length;
  else
    new_symbol_code_length = max_code_length_ + 1;

  if (define_symbol < num_base_symbols_) {
    model_.encode_sid(prior_is_cap_, 0);
    if (define_symbol_instances == 1)
      model_.encode_inst(prior_is_cap_, 0, kMaxInstancesForRemove - 1);
    else if (define_symbol_instances <= kMaxInstancesForRemove)
      model_.encode_inst(prior_is_cap_, 0, define_symbol_instances - 2);
    else
      model_.encode_inst(prior_is_cap_, 0, kMaxInstancesForRemove + max_regular_code_length_ - new_symbol_code_length);
    uint32_t new_symbol = define_symbol;
    if (cap_encoded_ != 0) {
      if (new_symbol > 'Z')
        new_symbol -= 24;
      else if (new_symbol > 'A')
        new_symbol -= 1;
      model_.encode_base_symbol(new_symbol, num_base_symbols_ - 24, num_base_symbols_);
    } else
      model_.encode_base_symbol(new_symbol, num_base_symbols_, num_base_symbols_);

    if ((define_symbol < kStartUtf8_2Byte) || (UTF8_compliant_ == 0)) {
      if (symbol_lengths_[define_symbol ^ 1] != 0)
        model_.double_range(define_symbol & 1);
      prior_end_ = static_cast<uint8_t>(define_symbol);
      symbol_lengths_[prior_end_] = new_symbol_code_length;
      if (cap_encoded_ == 0) {
        if (UTF8_compliant_ != 0) {
          model_.init_first_char(prior_end_, new_symbol_code_length);
          model_.init_prior_end(prior_end_, symbol_lengths_.data());
        } else {
          model_.init_first_char_binary(prior_end_, new_symbol_code_length);
          model_.init_prior_end_binary(prior_end_, symbol_lengths_.data());
        }
      } else {
        model_.init_base_symbol_cap(prior_end_, symbol_lengths_.data());
        if (prior_end_ == 'B')
          prior_end_ = 'C';
      }
    } else {
      prior_end_ = sd_[define_symbol].ends;
      if (symbol_lengths_[prior_end_] == 0) {
        symbol_lengths_[prior_end_] = new_symbol_code_length;
        model_.init_first_char(prior_end_, new_symbol_code_length);
        model_.init_prior_end(prior_end_, symbol_lengths_.data());
      }
    }
    prior_symbol_ = define_symbol;
    prior_is_cap_ = (sd_[define_symbol].type & 0x20) >> 5;
    if (found_first_symbol_ == 0) {
      found_first_symbol_ = 1;
      if ((cap_encoded_ != 0) && (prior_end_ == 'C'))
        end_char_ = define_symbol;
      else
        end_char_ = prior_end_;
      nbob_[end_char_][max_code_length_] = 1;
      sum_nbob_[end_char_] = 1;
    }
    if (define_symbol_instances == 1)
      return 1;
  } else {
    num_grammar_rules_++;
    define_symbol_start_ptr = symbol_array_.data() + sd_[define_symbol].symbol_start_index;
    define_string_ptr = define_symbol_start_ptr;
    define_string_end_ptr = symbol_array_.data() + sd_[define_symbol + 1].symbol_start_index - 1;

    symbols_in_definition = 0;
    while (define_string_ptr != define_string_end_ptr) {
      if ((sd_[*define_string_ptr].count != 1) || (*define_string_ptr < num_base_symbols_))
        symbols_in_definition++;
      else
        symbols_in_definition += count_symbols(*define_string_ptr);
      define_string_ptr++;
    }
    if (symbols_in_definition < 16) {
      SID_symbol = symbols_in_definition - 1;
      model_.encode_sid(prior_is_cap_, SID_symbol);
    } else {
      SID_symbol = 15;
      model_.encode_sid(prior_is_cap_, SID_symbol);
      model_.encode_extra_sid(symbols_in_definition - 16);
    }

    define_string_ptr = define_symbol_start_ptr;
    while (define_string_ptr != define_string_end_ptr) {
      symbol = *define_string_ptr++;
      symbol_inst = sd_[symbol].hits++;
      if (symbol_inst == 0) {
        if (cap_encoded_ != 0) {
          if (prior_is_cap_ == 0)
            model_.encode_new_type(1, 5 + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7), prior_end_, queue_size_);
          else
            model_.encode_new_type(3, 0x30 + (sd_[prior_symbol_].type & 3), 'C', queue_size_az_);
        } else
          model_.encode_new_type_binary(1, prior_end_, queue_size_);
        if (embed_define(symbol, 1) == 0)
          return 0;
      } else {
        if ((sd_[symbol].type & 0x40) != 0) {
          if (prior_is_cap_ != 0 && (sd_[symbol].type & 1) != 0)
            update_queue_prior_cap(symbol, 1);
          else
            update_queue(symbol, 1);
        } else {
          if (cap_encoded_ != 0) {
            if (prior_is_cap_ == 0)
              model_.encode_dict_type(1, 5 + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7), prior_end_, queue_size_);
            else
              model_.encode_dict_type(3, 0x30 + (sd_[prior_symbol_].type & 3), 'C', queue_size_az_);
          } else
            model_.encode_dict_type_binary(1, prior_end_, queue_size_);
          encode_dictionary_symbol(symbol);
          if (sd_[symbol].count <= kMaxInstancesForRemove) {
            if (sd_[symbol].count == sd_[symbol].hits)
              remove_dictionary_symbol(symbol, sd_[symbol].code_length);
            else if (use_mtf_ != 0) {
              if ((sd_[symbol].type & 2) != 0) {
                if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
                  if ((sd_[symbol].hits + 1 != sd_[symbol].count) || ((sd_[symbol].type & 0x80) != 0)) {
                    if (cap_encoded_ != 0)
                      model_.encode_go_mtf(6 * (sd_[symbol].count - 1) + prior_is_cap_ + (sd_[symbol].type & 1)
                          + 3 * (((sd_[symbol].type >> 3) & 3) == 2), 0, 1);
                    else
                      model_.encode_go_mtf(6 * (sd_[symbol].count - 1), 0, 1);
                  }
                  remove_dictionary_symbol(symbol, sd_[symbol].code_length);
                  add_symbol_to_queue(symbol);
                } else {
                  if (cap_encoded_ != 0)
                    model_.encode_go_mtf(6 * (sd_[symbol].count - 1) + prior_is_cap_
                        + (sd_[symbol].type & 1) + 3 * (((sd_[symbol].type >> 3) & 3) == 2), 0, 0);
                  else
                    model_.encode_go_mtf(6 * (sd_[symbol].count - 1), 0, 0);
                }
              }
            }
          } else if ((sd_[symbol].type & 2) != 0) {
            uint16_t context = 6 * (sd_[symbol].count - 1);
            if (cap_encoded_ != 0)
              context += prior_is_cap_ + (sd_[symbol].type & 1) + 3 * (((sd_[symbol].type >> 3) & 3) == 2);
            if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
              model_.encode_go_mtf(context, 0, 1);
              remove_dictionary_symbol(symbol, sd_[symbol].code_length);
              add_symbol_to_queue(symbol);
            } else
              model_.encode_go_mtf(context, 0, 0);
          }
        }
        prior_is_cap_ = (sd_[symbol].type & 0x20) >> 5;
        prior_symbol_ = symbol;
        num_transmits_++;
        prior_end_ = sd_[symbol].ends;
      }
    }
    prior_symbol_ = define_symbol;

    if (define_symbol_instances <= kMaxInstancesForRemove)
      model_.encode_inst(saved_prior_is_cap, SID_symbol, define_symbol_instances - 2);
    else
      model_.encode_inst(saved_prior_is_cap, SID_symbol,
          kMaxInstancesForRemove - 1 + max_regular_code_length_ - new_symbol_code_length);

    if (cap_encoded_ != 0) {
      if ((sd_[define_symbol].type & 0x10) != 0) {
        if ((sd_[define_symbol].type & 8) == 0)
          tag_type++;
        model_.encode_word_tag(tag_type++, prior_end_);
      } else if ((sd_[symbol_array_[sd_[define_symbol + 1].symbol_start_index - 2]].type & 0x18)
            > (sd_[define_symbol].type & 0x18))
        sd_[define_symbol].type
            += (sd_[symbol_array_[sd_[define_symbol + 1].symbol_start_index - 2]].type & 0x18) - 8;
    }
  }

  if ((use_mtf_ != 0) && ((new_symbol_code_length >= 11) || (define_symbol_instances <= kMaxInstancesForRemove))) {
    if (define_symbol_instances > kMaxInstancesForRemove) {
      uint16_t context, context2;
      if (cap_encoded_ != 0) {
        context = 6 * (new_symbol_code_length + kMaxInstancesForRemove - 1) + saved_prior_is_cap
            + (sd_[define_symbol].type & 1) + 3 * (((sd_[define_symbol].type >> 3) & 3) == 2);
        context2 = 240 + ((((sd_[define_symbol].type >> 3) & 3) == 2) << 1) + (sd_[define_symbol].type & 1);
      } else {
        context = new_symbol_code_length + kMaxInstancesForRemove - 1;
        context2 = 240;
      }
      if ((sd_[define_symbol].type & 2) != 0) {
        model_.encode_erg(context, context2, 1);
        if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
          model_.encode_go_mtf(context, 2, 1);
          add_symbol_to_queue(define_symbol);
        } else {
          model_.encode_go_mtf(context, 2, 0);
          if (add_dictionary_symbol(define_symbol, new_symbol_code_length) == 0)
            return 0;
        }
      } else {
        model_.encode_erg(context, context2, 0);
        if (add_dictionary_symbol(define_symbol, new_symbol_code_length) == 0)
          return 0;
      }
    } else {
      uint16_t context, context2;
      if (cap_encoded_ != 0) {
        context = 6 * (define_symbol_instances - 1) + saved_prior_is_cap + (sd_[define_symbol].type & 1)
            + 3 * (((sd_[define_symbol].type >> 3) & 3) == 2);
        context2 = 240 + (4 * new_symbol_code_length) + ((((sd_[define_symbol].type >> 3) & 3) == 2) << 1)
            + (sd_[define_symbol].type & 1);
      } else {
        context = define_symbol_instances - 1;
        context2 = 240 + new_symbol_code_length;
      }
      if ((sd_[define_symbol].type & 2) != 0) {
        if (define_symbol_instances > 2)
          model_.encode_erg(context, context2, 1);
        if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
          model_.encode_go_mtf(context, 2, 1);
          add_symbol_to_queue(define_symbol);
        } else {
          model_.encode_go_mtf(context, 2, 0);
          if (add_dictionary_symbol(define_symbol, new_symbol_code_length) == 0)
            return 0;
        }
      } else {
        if (define_symbol_instances > 2)
          model_.encode_erg(context, context2, 0);
        else
          model_.encode_go_mtf(context, 2, 0);
        if (add_dictionary_symbol(define_symbol, new_symbol_code_length) == 0)
          return 0;
      }
    }
  }
  else if (add_dictionary_symbol(define_symbol, new_symbol_code_length) == 0)
    return 0;
  num_transmits_++;
  return 1;
}


void Encoder::replace_symbol(uint32_t symbol, uint32_t** symbol2_ptr_ptr,
    uint32_t* num_more_than_15_inst_definitions_ptr) {
  uint32_t* define_string_ptr = symbol_array_.data() + sd_[symbol].symbol_start_index;
  uint32_t* define_string_end_ptr = symbol_array_.data() + sd_[symbol + 1].symbol_start_index - 1;
  while (define_string_ptr != define_string_end_ptr) {
    const uint32_t symbol2 = *define_string_ptr++;
    if ((symbol2 >= num_base_symbols_) && (sd_[symbol2].code_length != 0))
      replace_symbol(symbol2, symbol2_ptr_ptr, num_more_than_15_inst_definitions_ptr);
    else {
      *((*symbol2_ptr_ptr)++) = symbol2;
      if ((sd_[symbol2].code_length != 2) && (sd_[symbol2].count++ == kMaxInstancesForRemove))
        (*num_more_than_15_inst_definitions_ptr)++;
    }
  }
}


bool Encoder::encode(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                     uint8_t* outbuf, FILE* fd_out, size_t filesize,
                     const Params& params) {
  constexpr uint8_t kInsertSymbolChar = 0xFE;
  constexpr uint8_t kDefineSymbolChar = 0xFF;
  constexpr size_t kWriteSize = 0x40000;

  uint8_t temp_char, base_bits, format, verbose, code_length, increase_length, decrease_length;
  uint8_t* in_char_ptr;
  uint8_t* end_char_ptr;
  uint16_t queue_position;
  uint32_t i, j, k, num_symbols_defined, num_definitions_to_code, num_codes, grammar_size, dictionary_size;
  uint32_t num_more_than_15_inst_definitions, num_2_inst_definitions, num_greater_500, num_transmits_over_sqrt2, num_mtfs;
  uint32_t UTF8_value, max_UTF8_value, symbol, symbol_inst, prior_repeats, next_symbol;
  uint32_t min_ranked_symbols, ranked_symbols_save, num_new_symbols, num_symbols_to_code, num_rules_reversed;
  uint32_t code_length_limit, mtf_miss_code_space, sum_peaks, min_code_space, rules_reduced;
  uint32_t mtf_started[kMaxInstancesForRemove + 1], mtf_hits[kMaxInstancesForRemove + 1];
  uint32_t mtf_peak[kMaxInstancesForRemove + 1], mtf_peak_mtf[kMaxInstancesForRemove + 1];
  uint32_t mtf_in_dictionary[kMaxInstancesForRemove + 1], mtf_active[kMaxInstancesForRemove + 1];
  uint32_t *symbol_ptr, *symbol2_ptr, *end_symbol_ptr, *first_define_ptr, *max_ptr;
  uint32_t *ranked_symbols_ptr, *ranked_symbols2_ptr, *end_ranked_symbols_ptr;
  uint32_t *min_ranked_symbols_ptr, *max_ranked_symbols_ptr, *min_one_instance_ranked_symbols_ptr, *peak_array;
  int32_t remaining_symbols_to_code, remaining_code_space, max_len_adj_profit;
  int32_t index_last_length[26], index_first_length[26];
  double symbol_inst_factor, codes_per_code_space;

  verbose = cap_encoded_ = UTF8_compliant_ = num_symbols_defined = 0;
  first_define_ptr = nullptr;
  end_char_ = 0;
  prior_symbol_ = 0;
  num_transmits_ = 0;
  num_grammar_rules_ = 0;
  found_first_symbol_ = 0;
  use_mtf_ = params.use_mtf;
  verbose = params.print_dictionary;
  model_.reset_codec_globals();
  in_char_ptr = inbuf;
  end_char_ptr = inbuf + insize;
  format = *in_char_ptr++;
  if ((format & 0x81) == 1) {
    cap_encoded_ = (format >> 1) & 1;
    UTF8_compliant_ = (format >> 2) & 1;
    format = 1;
  }

  symbol_array_.resize(insize);
  symbol_ptr = symbol_array_.data();

  if (UTF8_compliant_ != 0) {
    base_bits = *in_char_ptr++;
    num_base_symbols_ = 1u << base_bits;
    max_UTF8_value = 0x7F;
    while (in_char_ptr < end_char_ptr) {
      temp_char = *in_char_ptr++;
      if (temp_char == kInsertSymbolChar) {
        symbol = num_base_symbols_ + 0x10000 * *in_char_ptr++;
        symbol += 0x100 * *in_char_ptr++;
        symbol += *in_char_ptr++;
        *symbol_ptr++ = symbol;
      } else if (temp_char == kDefineSymbolChar) {
        if (first_define_ptr == nullptr)
          first_define_ptr = symbol_ptr;
        in_char_ptr += 3;
        *symbol_ptr++ = 0x80000000 + num_symbols_defined++;
      } else if (temp_char < kStartUtf8_2Byte)
        *symbol_ptr++ = static_cast<uint32_t>(temp_char);
      else {
        if (temp_char >= 0xF0) {
          UTF8_value = 0x40000 * (temp_char & 7) + 0x1000 * (*in_char_ptr++ & 0x3F);
          UTF8_value += 0x40 * (*in_char_ptr++ & 0x3F);
        } else if (temp_char >= 0xE0) {
          UTF8_value = 0x1000 * (temp_char & 0xF) + 0x40 * (*in_char_ptr++ & 0x3F);
        } else
          UTF8_value = 0x40 * (temp_char & 0x1F);
        UTF8_value += *in_char_ptr++ & 0x3F;
        *symbol_ptr++ = UTF8_value;
        if (UTF8_value > max_UTF8_value)
          max_UTF8_value = UTF8_value;
      }
    }
  } else {
    base_bits = 8;
    num_base_symbols_ = 0x100;
    while (in_char_ptr < end_char_ptr) {
      temp_char = *in_char_ptr++;
      if (temp_char < kInsertSymbolChar)
        *symbol_ptr++ = static_cast<uint32_t>(temp_char);
      else if (*in_char_ptr == kDefineSymbolChar) {
        *symbol_ptr++ = static_cast<uint32_t>(temp_char);
        in_char_ptr++;
      } else if (temp_char == kInsertSymbolChar) {
        symbol = 0x100 + 0x10000 * *in_char_ptr++;
        symbol += 0x100 * *in_char_ptr++;
        symbol += *in_char_ptr++;
        *symbol_ptr++ = symbol;
      } else {
        if (first_define_ptr == nullptr)
          first_define_ptr = symbol_ptr;
        in_char_ptr += 3;
        *symbol_ptr++ = 0x80000000 + num_symbols_defined++;
      }
    }
  }

  end_symbol_ptr = symbol_ptr;
  *end_symbol_ptr = kUniqueSymbol;
  num_codes = num_base_symbols_ + num_symbols_defined;
  grammar_size = symbol_ptr - symbol_array_.data();
  if (first_define_ptr == nullptr)
    first_define_ptr = symbol_ptr;

  sd_.resize(num_codes + 1);
  std::vector<uint32_t> ranked_symbols(num_codes);
  std::vector<uint32_t> ranked_symbols2(num_codes);
  transmits_.resize(grammar_size);

  for (i = 0; i < num_codes; i++) {
    sd_[i].count = 0;
    sd_[i].symbol_start_index = 0;
  }
  symbol_ptr = symbol_array_.data();
  while (symbol_ptr != end_symbol_ptr) {
    symbol = *symbol_ptr++;
    if (static_cast<int32_t>(symbol) >= 0) {
      if (symbol >= num_codes) {
        fprintf(stderr,
            "GLZA encode: grammar terminal symbol %u >= num_codes %u (grammar_bytes=%u num_base_symbols=%u)\n",
            static_cast<unsigned>(symbol), static_cast<unsigned>(num_codes), static_cast<unsigned>(grammar_size),
            static_cast<unsigned>(num_base_symbols_));
        return false;
      }
      sd_[symbol].count++;
    } else {
      const uint32_t rule_index = symbol - 0x80000000 + num_base_symbols_;
      if (rule_index >= num_codes) {
        fprintf(stderr,
            "GLZA encode: grammar rule index %u >= num_codes %u (raw=0x%08x grammar_bytes=%u)\n",
            static_cast<unsigned>(rule_index), static_cast<unsigned>(num_codes), static_cast<unsigned>(symbol),
            static_cast<unsigned>(grammar_size));
        return false;
      }
      sd_[rule_index].symbol_start_index = symbol_ptr - symbol_array_.data();
    }
  }
  sd_[num_codes].symbol_start_index = symbol_ptr - symbol_array_.data() + 1;

  // ===== Dead-rule elimination (see glza/EOF_DESYNC_ISSUE.md) =====
  // A rule with count==0 has no live references, yet its definition still sits in
  // symbol_array_ where the count scan above tallied every symbol it references.
  // Those phantom references inflate other symbols' counts, so they never reach
  // count==hits, are never removed by the transmission loop, and remain dictionary
  // resident at EOF. When such a resident lands at (end_char_, max_code_length_) the
  // nsob==0 end-of-stream sentinel becomes ambiguous and the decoder over-reads.
  // Remove dead rules here -- BEFORE the count==1/2 reduction below, so that the
  // reduction's replace_symbol() inlining can never re-reference a rule whose
  // definition we dropped. First decrement the phantom references so each count equals
  // the number of references that will actually be transmitted (iterate: zeroing a
  // rule's count makes it dead in turn); then physically drop the dead definitions and
  // recompute symbol_start_index / first_define_ptr from the rebuilt grammar.
  {
    std::vector<uint8_t> dead_processed(num_codes, 0);
    uint32_t num_dead_rules = 0;
    bool changed = true;
    while (changed) {
      changed = false;
      for (uint32_t d = num_base_symbols_; d < num_codes; d++) {
        if ((dead_processed[d] != 0) || (sd_[d].count != 0))
          continue;
        dead_processed[d] = 1;
        const uint32_t body_begin = sd_[d].symbol_start_index;
        const uint32_t body_end = sd_[d + 1].symbol_start_index;  // body = [begin, end-1)
        if (body_end <= body_begin + 1)
          continue;
        num_dead_rules++;
        for (uint32_t p = body_begin; p + 1 < body_end; p++) {
          const uint32_t v = symbol_array_[p];
          if ((sd_[v].count != 0) && (--sd_[v].count == 0) && (v >= num_base_symbols_))
            changed = true;
        }
      }
    }
    if (num_dead_rules != 0) {
      uint32_t* read_ptr = symbol_array_.data();
      uint32_t* write_ptr = symbol_array_.data();
      while (read_ptr < end_symbol_ptr) {
        const uint32_t s = *read_ptr++;
        if (((s & 0x80000000) != 0)
            && (sd_[s - 0x80000000 + num_base_symbols_].count == 0)) {
          while (*read_ptr < 0x80000000)  // drop the dead rule's marker + body
            read_ptr++;
        } else {
          *write_ptr++ = s;
        }
      }
      end_symbol_ptr = write_ptr;
      *end_symbol_ptr = kUniqueSymbol;
      uint32_t prior_rule = num_base_symbols_ - 1;
      first_define_ptr = end_symbol_ptr;
      for (uint32_t* p = symbol_array_.data(); p < end_symbol_ptr; p++) {
        if ((*p & 0x80000000) != 0) {
          const uint32_t rule = *p - 0x80000000 + num_base_symbols_;
          const uint32_t pos = static_cast<uint32_t>((p + 1) - symbol_array_.data());
          if (first_define_ptr == end_symbol_ptr)
            first_define_ptr = p;
          while (++prior_rule < rule)  // fill every gap left by a dropped rule
            sd_[prior_rule].symbol_start_index = pos;
          sd_[rule].symbol_start_index = pos;
          prior_rule = rule;
        }
      }
      const uint32_t tail = static_cast<uint32_t>(end_symbol_ptr - symbol_array_.data()) + 1;
      while (++prior_rule <= num_codes)
        sd_[prior_rule].symbol_start_index = tail;
    }
  }

  if (cap_encoded_ != 0) {
    i = 0;
    do {
      sd_[i++].type = 0;
    } while (i != 0x61);
    do {
      sd_[i++].type = 1;
    } while (i != 0x7B);
    do {
      sd_[i++].type = 0;
    } while (i != num_base_symbols_);
    sd_[' '].type = 4;
    sd_['B'].type = 0x24;
    sd_['C'].type = 0x24;
    while (i < num_codes) {
      next_symbol = symbol_array_[sd_[i].symbol_start_index];
      while (next_symbol > i)
        next_symbol = symbol_array_[sd_[next_symbol].symbol_start_index];
      sd_[i].type = sd_[next_symbol].type & 1;
      next_symbol = symbol_array_[sd_[i + 1].symbol_start_index - 2];
      while (next_symbol > i)
        next_symbol = symbol_array_[sd_[next_symbol + 1].symbol_start_index - 2];
      sd_[i++].type |= sd_[next_symbol].type & 0x20;
    }
  } else {
    i = 0;
    while (i < num_codes)
      sd_[i++].type = 0;
  }

  ranked_symbols_ptr = ranked_symbols.data();
  for (i = 0; i < num_codes; i++)
    if (sd_[i].count != 0)
      *ranked_symbols_ptr++ = i;
  end_ranked_symbols_ptr = ranked_symbols_ptr;
  min_ranked_symbols_ptr = ranked_symbols_ptr;

  rules_reduced = 0;
  ranked_symbols_ptr = ranked_symbols.data();
  while (ranked_symbols_ptr < min_ranked_symbols_ptr) {
    if (sd_[*ranked_symbols_ptr].count == 1) {
      ranked_symbols_save = *ranked_symbols_ptr;
      if (ranked_symbols_save >= num_base_symbols_)
        rules_reduced++;
      *ranked_symbols_ptr = *--min_ranked_symbols_ptr;
      *min_ranked_symbols_ptr = ranked_symbols_save;
    } else
      ranked_symbols_ptr++;
  }
  min_one_instance_ranked_symbols_ptr = min_ranked_symbols_ptr;

  grammar_size -= num_symbols_defined + rules_reduced;
  num_new_symbols = end_ranked_symbols_ptr - ranked_symbols.data() - rules_reduced;

  for (i = 2; i <= kMaxInstancesForRemove; i++) {
    ranked_symbols_ptr = ranked_symbols.data();
    while (ranked_symbols_ptr < min_ranked_symbols_ptr) {
      if (sd_[*ranked_symbols_ptr].count == i) {
        ranked_symbols_save = *ranked_symbols_ptr;
        *ranked_symbols_ptr = *--min_ranked_symbols_ptr;
        *min_ranked_symbols_ptr = ranked_symbols_save;
      } else
        ranked_symbols_ptr++;
    }
  }
  num_more_than_15_inst_definitions = min_ranked_symbols_ptr - ranked_symbols.data();

  for (i = kMaxInstancesForRemove; i < 801; i++) {
    ranked_symbols_ptr = ranked_symbols.data();
    while (ranked_symbols_ptr < min_ranked_symbols_ptr) {
      if (sd_[*ranked_symbols_ptr].count == i) {
        ranked_symbols_save = *ranked_symbols_ptr;
        *ranked_symbols_ptr = *--min_ranked_symbols_ptr;
        *min_ranked_symbols_ptr = ranked_symbols_save;
      } else
        ranked_symbols_ptr++;
    }
  }

  min_ranked_symbols = min_ranked_symbols_ptr - ranked_symbols.data();
  for (i = 0; i < min_ranked_symbols; i++) {
    uint32_t max_symbol_count = 0;
    ranked_symbols_ptr = &ranked_symbols[i];
    while (ranked_symbols_ptr < min_ranked_symbols_ptr) {
      if (sd_[*ranked_symbols_ptr].count > max_symbol_count) {
        max_symbol_count = sd_[*ranked_symbols_ptr].count;
        max_ranked_symbols_ptr = ranked_symbols_ptr;
      }
      ranked_symbols_ptr++;
    }
    if (max_symbol_count > 0) {
      ranked_symbols_save = ranked_symbols[i];
      ranked_symbols[i] = *max_ranked_symbols_ptr;
      *max_ranked_symbols_ptr = ranked_symbols_save;
    }
  }

  num_definitions_to_code = min_one_instance_ranked_symbols_ptr - ranked_symbols.data();
  max_regular_code_length_ = 2;
  if (sd_[ranked_symbols[0]].count > kMaxInstancesForRemove)
    max_regular_code_length_ = static_cast<uint8_t>(log2(static_cast<double>(grammar_size - num_new_symbols) * 0.094821));
  if (max_regular_code_length_ > 24)
    max_regular_code_length_ = 24;

  for (i = num_base_symbols_; i < num_codes; i++)
    sd_[i].starts = sd_[i].ends = 0;

  if (UTF8_compliant_ != 0) {
    i = 0;
    while (i < 0x80) {
      sd_[i].starts = sd_[i].ends = static_cast<uint8_t>(i);
      i++;
    }
    uint32_t temp_UTF8_limit = 0x250;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x80;
      i++;
    }
    temp_UTF8_limit = 0x370;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x81;
      i++;
    }
    temp_UTF8_limit = 0x400;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x82;
      i++;
    }
    temp_UTF8_limit = 0x530;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x83;
      i++;
    }
    temp_UTF8_limit = 0x590;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x84;
      i++;
    }
    temp_UTF8_limit = 0x600;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x85;
      i++;
    }
    temp_UTF8_limit = 0x700;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x86;
      i++;
    }
    temp_UTF8_limit = kStartUtf8_3Byte;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x87;
      i++;
    }
    temp_UTF8_limit = 0x1000;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x88;
      i++;
    }
    temp_UTF8_limit = 0x2000;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x89;
      i++;
    }
    temp_UTF8_limit = 0x3000;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8A;
      i++;
    }
    temp_UTF8_limit = 0x3040;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8B;
      i++;
    }
    temp_UTF8_limit = 0x30A0;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8C;
      i++;
    }
    temp_UTF8_limit = 0x3100;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8D;
      i++;
    }
    temp_UTF8_limit = 0x3200;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8E;
      i++;
    }
    temp_UTF8_limit = 0xA000;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8F;
      i++;
    }
    temp_UTF8_limit = kStartUtf8_4Byte;
    if (max_UTF8_value < temp_UTF8_limit)
      temp_UTF8_limit = max_UTF8_value + 1;
    while (i < temp_UTF8_limit) {
      sd_[i].starts = sd_[i].ends = 0x8E;
      i++;
    }
    while (i <= max_UTF8_value) {
      sd_[i].starts = sd_[i].ends = 0x90;
      i++;
    }
    if (cap_encoded_ != 0)
      sd_['B'].ends = 'C';
    i = num_base_symbols_;
    while (i < num_codes) {
      if (sd_[i].starts == 0)
        sd_[i].starts = find_first_UTF8(i);
      if (sd_[i].ends == 0)
        sd_[i].ends = find_last_UTF8(i);
      i++;
    }
  } else {
    i = 0;
    while (i < 0x100) {
      sd_[i].starts = sd_[i].ends = static_cast<uint8_t>(i);
      i++;
    }
    if (cap_encoded_ != 0)
      sd_['B'].ends = 'C';
    i = num_base_symbols_;
    while (i < num_codes) {
      if (sd_[i].starts == 0)
        sd_[i].starts = find_first(i);
      if (sd_[i].ends == 0)
        sd_[i].ends = find_last(i);
      i++;
    }
  }

  num_transmits_ = 0;
  for (i = 0; i < num_codes; i++) {
    sd_[i].hits = 0;
    sd_[i].previous = 0xFFFFFFFF;
  }
  symbol_ptr = symbol_array_.data();

  while (symbol_ptr < first_define_ptr) {
    symbol = *symbol_ptr++;
    if (sd_[symbol].previous == 0xFFFFFFFF) {
      get_embedded_symbols(symbol);
    } else {
      transmits_[sd_[symbol].previous].distance = num_transmits_ - sd_[symbol].previous;
      transmits_[num_transmits_].distance = 0xFFFFFFFF;
      sd_[symbol].previous2 = sd_[symbol].previous;
      sd_[symbol].previous = num_transmits_++;
    }
  }

  for (i = 0; i < num_base_symbols_; i++) {
    if (sd_[i].count == 1)
      sd_[i].code_length = 2;
    else
      sd_[i].code_length = 0;
  }
  for (i = num_base_symbols_; i < num_codes; i++) {
    if (sd_[i].count == 1)
      sd_[i].code_length = 1;
    else
      sd_[i].code_length = 0;
  }

  i = num_more_than_15_inst_definitions;
  while ((i < num_definitions_to_code) && (sd_[ranked_symbols[i]].count > 2))
    i++;
  num_2_inst_definitions = num_definitions_to_code - i;
  num_rules_reversed = 0;

  {
    const uint32_t max_distance = static_cast<uint32_t>(10.0 * pow(static_cast<double>(num_transmits_), 0.4));
    while (i < num_definitions_to_code) {
      if (sd_[ranked_symbols[i] + 1].symbol_start_index - sd_[ranked_symbols[i]].symbol_start_index == 3) {
        const uint32_t distance = sd_[ranked_symbols[i]].previous - sd_[ranked_symbols[i]].previous2;
        uint32_t* define_string_ptr = symbol_array_.data() + sd_[ranked_symbols[i]].symbol_start_index;
        if (static_cast<uint64_t>((4 >> sd_[ranked_symbols[i]].hits) * (sd_[*define_string_ptr].count))
            * static_cast<uint64_t>(sd_[*(define_string_ptr + 1)].count) >= static_cast<uint64_t>(num_2_inst_definitions)) {
          if (distance > max_distance) {
            sd_[ranked_symbols[i]].code_length = 1;
            num_rules_reversed++;
          }
        }
      }
      i++;
    }
  }
  for (i = num_definitions_to_code - num_2_inst_definitions; i < num_definitions_to_code; i++) {
    if (sd_[ranked_symbols[i]].code_length != 0) {
      uint32_t* define_string_ptr = symbol_array_.data() + sd_[ranked_symbols[i]].symbol_start_index;
      if ((sd_[*define_string_ptr].code_length != 0) || (sd_[*(define_string_ptr + 1)].code_length != 0)) {
        sd_[ranked_symbols[i]].code_length = 0;
        num_rules_reversed--;
      }
    }
  }

  if ((num_rules_reversed != 0) || (rules_reduced != 0)) {
    std::vector<uint32_t> symbol_array2(end_symbol_ptr - symbol_array_.data());
    symbol2_ptr = symbol_array2.data();
    symbol_ptr = symbol_array_.data();
    while (symbol_ptr < end_symbol_ptr) {
      symbol = *symbol_ptr++;
      if ((symbol & 0x80000000) == 0) {
        if ((symbol >= num_base_symbols_) && (sd_[symbol].code_length != 0))
          replace_symbol(symbol, &symbol2_ptr, &num_more_than_15_inst_definitions);
        else
          *symbol2_ptr++ = symbol;
      } else if (sd_[symbol - 0x80000000 + num_base_symbols_].code_length != 0) {
        do {
          if ((sd_[*symbol_ptr].code_length != 2) && (--sd_[*symbol_ptr].count == kMaxInstancesForRemove))
            num_more_than_15_inst_definitions--;
        } while (*++symbol_ptr < 0x80000000);
      } else
        *symbol2_ptr++ = symbol;
    }
    std::memcpy(symbol_array_.data(), symbol_array2.data(), sizeof(uint32_t) * (symbol2_ptr - symbol_array2.data()));
    end_symbol_ptr = symbol_array_.data() + (symbol2_ptr - symbol_array2.data());
    *end_symbol_ptr = kUniqueSymbol;
  }

#ifdef PRINTON
  if (rules_reduced != 0)
    fprintf(stderr, "Eliminated %u single appearance production rules\n", rules_reduced);
  fprintf(stderr, "Parsed %u level 0 symbols\n", static_cast<unsigned>(first_define_ptr - symbol_array_.data()));
  fprintf(stderr, "Removed %u two instance length two production rules\n", num_rules_reversed);
  {
    double log_file_symbols = log2(static_cast<double>(grammar_size) + 1);
    double order_0_entropy = static_cast<double>(grammar_size + 1) * log_file_symbols;
    i = 0;
    do {
      if (sd_[i].count != 0) {
        double d_symbol_count = static_cast<double>(sd_[i].count);
        order_0_entropy -= d_symbol_count * log2(d_symbol_count);
        if (i < kStartUtf8_2Byte)
          order_0_entropy += 8.0;
        else if (i < kStartUtf8_3Byte)
          order_0_entropy += 16.0;
        else if (i < kStartUtf8_4Byte)
          order_0_entropy += 24.0;
        else
          order_0_entropy += 32.0;
      }
    } while (++i < num_base_symbols_);
    if (num_symbols_defined != 0) {
      while (i < num_codes) {
        if ((sd_[i].count > 1) && (sd_[i].code_length == 0)) {
          double d_symbol_count = static_cast<double>(sd_[i].count - 1);
          order_0_entropy -= d_symbol_count * log2(d_symbol_count);
        }
        i++;
      }
      double d_symbol_count = static_cast<double>(num_symbols_defined - rules_reduced - num_rules_reversed);
      order_0_entropy -= d_symbol_count * log2(d_symbol_count);
    }
    if (num_symbols_defined != 0) {
      uint32_t len_codes = num_symbols_defined - rules_reduced - num_rules_reversed;
      double log_len_codes = log2(static_cast<double>(len_codes));
      double code_entropy = 0.0;
      uint32_t len_counts[16];
      uint32_t extra_len_bits = 0;
      for (i = 0; i < 16; i++)
        len_counts[i] = 0;
      for (i = num_base_symbols_; i < num_codes; i++) {
        if (sd_[i].count > 1) {
          uint32_t dl = sd_[i + 1].symbol_start_index - sd_[i].symbol_start_index - 1;
          if (dl < 16)
            len_counts[dl - 1]++;
          else {
            len_counts[15]++;
            uint32_t extra_len = dl - 14;
            do {
              extra_len >>= 1;
              extra_len_bits += 2;
            } while (extra_len != 0);
          }
        }
      }
      len_counts[1] -= num_rules_reversed;
      for (i = 0; i < 16; i++) {
        if (len_counts[i] != 0) {
          double d_sc = static_cast<double>(len_counts[i]);
          code_entropy += d_sc * (log_len_codes - log2(d_sc));
        }
      }
      code_entropy += static_cast<double>(extra_len_bits);
      fprintf(stderr, "Final grammar size: %u (%u terminals + %u production rules + %u repeats)\n",
        grammar_size + 1, num_new_symbols + rules_reduced - num_symbols_defined + 1,
        num_symbols_defined - rules_reduced - num_rules_reversed, grammar_size - num_new_symbols + num_rules_reversed);
      fprintf(stderr, "%.4f bits/symbol plus %.4f bits/rule length, o0e %.2lf bytes\n",
          static_cast<float>(order_0_entropy / static_cast<double>(grammar_size)),
          static_cast<float>(code_entropy / static_cast<double>(len_codes)),
          0.125 * (order_0_entropy + code_entropy));
    }
  }
#endif

  prior_symbol_ = num_base_symbols_ - 1;
  symbol_ptr = first_define_ptr;
  while (symbol_ptr < end_symbol_ptr) {
    symbol = *symbol_ptr++;
    if ((symbol & 0x80000000) != 0) {
      symbol += num_base_symbols_ - 0x80000000;
      sd_[symbol].symbol_start_index = symbol_ptr - symbol_array_.data();
      if (++prior_symbol_ == num_base_symbols_)
        first_define_ptr = symbol_ptr - 1;
      if (prior_symbol_ != symbol) {
        sd_[prior_symbol_].symbol_start_index = symbol_ptr - symbol_array_.data();
        prior_symbol_ = symbol;
      }
    }
  }
  sd_[++prior_symbol_].symbol_start_index = end_symbol_ptr - symbol_array_.data() + 1;

  i = 0;
  while (i < num_definitions_to_code) {
    if (sd_[ranked_symbols[i]].code_length != 0) {
      num_definitions_to_code--;
      num_new_symbols--;
      const uint32_t bad_symbol = ranked_symbols[i];
      ranked_symbols[i] = ranked_symbols[num_definitions_to_code];
      ranked_symbols[num_definitions_to_code] = bad_symbol;
    } else {
      if ((i != 0) && (sd_[ranked_symbols[i]].count > sd_[ranked_symbols[i - 1]].count)) {
        uint32_t temp_symbol = ranked_symbols[i];
        const uint32_t temp_count = sd_[temp_symbol].count;
        uint32_t new_rank = i;
        while ((new_rank != 0) && (temp_count > sd_[ranked_symbols[new_rank - 1]].count)) {
          while ((new_rank >= 1001) && (sd_[ranked_symbols[new_rank - 1]].count == sd_[ranked_symbols[new_rank - 1001]].count)) {
            ranked_symbols[new_rank] = ranked_symbols[new_rank - 1000];
            new_rank -= 1000;
          }
          while ((new_rank >= 33) && (sd_[ranked_symbols[new_rank - 1]].count == sd_[ranked_symbols[new_rank - 33]].count)) {
            ranked_symbols[new_rank] = ranked_symbols[new_rank - 32];
            new_rank -= 32;
          }
          while ((new_rank >= 2) && (sd_[ranked_symbols[new_rank - 1]].count == sd_[ranked_symbols[new_rank - 2]].count)) {
            ranked_symbols[new_rank] = ranked_symbols[new_rank - 1];
            new_rank -= 1;
          }
          ranked_symbols[new_rank] = ranked_symbols[new_rank - 1];
          new_rank--;
        }
        ranked_symbols[new_rank] = temp_symbol;
      }
      i++;
    }
  }

  num_transmits_ = 0;
  for (i = 0; i < num_codes; i++) {
    sd_[i].space_score = 0;
    sd_[i].previous = 0xFFFFFFFF;
  }
  prior_symbol_ = static_cast<uint32_t>(-1);
  symbol_ptr = symbol_array_.data();

  while (symbol_ptr < first_define_ptr) {
    symbol = *symbol_ptr++;
    if (sd_[symbol].previous == 0xFFFFFFFF)
      get_embedded_symbols2(symbol);
    else {
      transmits_[sd_[symbol].previous].distance = num_transmits_ - sd_[symbol].previous;
      transmits_[num_transmits_].symbol = symbol;
      transmits_[num_transmits_].distance = 0xFFFFFFFF;
      sd_[symbol].previous = num_transmits_++;
      if ((sd_[prior_symbol_].type & 0x18) != 0) {
        if (sd_[symbol].starts == 0x20)
          sd_[prior_symbol_].space_score++;
        else
          sd_[prior_symbol_].space_score -= 5;
      }
      prior_symbol_ = symbol;
    }
  }

  num_symbols_to_code = grammar_size - num_new_symbols - rules_reduced;
  num_transmits_over_sqrt2 = static_cast<uint32_t>(static_cast<float>(num_transmits_) * 0.7071f);
  num_mtfs = 0;

  if (use_mtf_ != 0) {
    for (i = 0; i < num_codes; i++) {
      if ((sd_[i].count <= (num_transmits_over_sqrt2 >> 9)) || (sd_[i].count <= kMaxInstancesForRemove))
        sd_[i].type |= 2;
      sd_[i].hits = 0;
      sd_[i].array_index = 0;
    }
    queue_size_ = 0;
    for (i = 0; i < num_transmits_; i++) {
      symbol = transmits_[i].symbol;
      sd_[symbol].array_index++;
      if ((sd_[symbol].type & 0x40) != 0) {
        queue_position = 0;
        while (queue_[queue_position] != symbol)
          queue_position++;
        add_mtf_hit_scores(&sd_[symbol], queue_position, num_transmits_over_sqrt2);
        sd_[symbol].previous = i;
        if (transmits_[i].distance != 0xFFFFFFFF) {
          if (((sd_[symbol].type & 2) != 0)
              && (static_cast<uint64_t>(transmits_[i].distance) * 4 * static_cast<uint64_t>(sd_[symbol].count) <= num_transmits_)) {
            if ((sd_[symbol].array_index + 1 == sd_[symbol].count) && (sd_[symbol].count <= kMaxInstancesForRemove))
              sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                  * static_cast<double>(transmits_[i].distance))) - 1.5) / static_cast<double>(transmits_[i].distance)) - 0.001f;
            else if (transmits_[i].distance > transmits_[i + transmits_[i].distance].distance)
              sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                  * static_cast<double>(transmits_[i].distance))) - 0.0) / static_cast<double>(transmits_[i].distance)) - 0.001f;
            else
              sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                  * static_cast<double>(transmits_[i].distance))) - 2.5) / static_cast<double>(transmits_[i].distance)) - 0.001f;
            if (sd_[symbol].score >= 0.0f) {
              num_mtfs++;
              while (queue_position-- != 0)
                queue_[queue_position + 1] = queue_[queue_position];
              queue_[0] = symbol;
            } else
              transmits_[i].distance = 0xFFFFFFFF;
          } else
            transmits_[i].distance = 0xFFFFFFFF;
        }
        if (transmits_[i].distance == 0xFFFFFFFF) {
          sd_[symbol].type &= 0xBF;
          queue_size_--;
          while (queue_position != queue_size_) {
            queue_[queue_position] = queue_[queue_position + 1];
            queue_position++;
          }
        }
      } else if (transmits_[i].distance != 0xFFFFFFFF) {
        if (((sd_[symbol].type & 2) != 0) && (transmits_[i].distance <= num_transmits_ / (4 * sd_[symbol].count))) {
          if ((sd_[symbol].array_index + 1 == sd_[symbol].count) && (sd_[symbol].count <= kMaxInstancesForRemove))
            sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                * static_cast<double>(transmits_[i].distance))) - 4.0) / static_cast<double>(transmits_[i].distance)) - 0.001f;
          else if (transmits_[i].distance > transmits_[i + transmits_[i].distance].distance)
            sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                * static_cast<double>(transmits_[i].distance))) - 2.5) / static_cast<double>(transmits_[i].distance)) - 0.001f;
          else
            sd_[symbol].score = static_cast<float>((log2(static_cast<double>(num_transmits_) / (static_cast<double>(sd_[symbol].count)
                * static_cast<double>(transmits_[i].distance))) - 5.0) / static_cast<double>(transmits_[i].distance)) - 0.001f;
          if (sd_[symbol].score >= 0.0f) {
            num_mtfs++;
            sd_[symbol].previous = i;
            if (queue_size_ < 0x100) {
              sd_[symbol].type |= 0x40;
              queue_position = queue_size_++;
              while (queue_position-- != 0)
                queue_[queue_position + 1] = queue_[queue_position];
              queue_[0] = symbol;
            } else {
              float low_score = sd_[symbol].score;
              uint16_t low_score_pos = 0x100;
              for (k = 0; k <= 0xFF; k++) {
                j = sd_[queue_[k]].previous;
                if (sd_[queue_[k]].score * static_cast<float>(transmits_[j].distance) / static_cast<float>(transmits_[j].distance - (i - j))
                    < low_score) {
                  low_score
                      = sd_[queue_[k]].score * static_cast<float>(transmits_[j].distance) / static_cast<float>(transmits_[j].distance - (i - j));
                  low_score_pos = k;
                }
              }
              if (low_score_pos != 0x100) {
                sd_[symbol].type |= 0x40;
                sd_[queue_[low_score_pos]].type &= 0xBF;
                j = sd_[queue_[low_score_pos]].previous;
                queue_position = low_score_pos;
                while (queue_position-- != 0)
                 queue_[queue_position + 1] = queue_[queue_position];
                queue_[0] = symbol;
                transmits_[j].distance = 0xFFFFFFFF;
              } else
                transmits_[i].distance = 0xFFFFFFFF;
            }
          } else
            transmits_[i].distance = 0xFFFFFFFF;
        } else
          transmits_[i].distance = 0xFFFFFFFF;
      }
    }
#ifdef PRINTON
  fprintf(stderr, "Found %u MTF candidates, ", static_cast<unsigned>(num_mtfs));
#endif

    for (i = 0; i < num_codes; i++) {
      if ((sd_[i].type & 2) != 0) {
        if (sd_[i].count > kMaxInstancesForRemove) {
          if (static_cast<float>(sd_[i].hits) < static_cast<float>(sd_[i].count - 1)
              * (1.0f + 0.03f * static_cast<float>(pow(22.0 - log2(static_cast<float>(num_transmits_over_sqrt2) / static_cast<float>(sd_[i].count)), 2.5))))
            sd_[i].type &= 0xFD;
        } else if (sd_[i].hits == 0)
          sd_[i].type &= 0xFD;
      }
      sd_[i].hits = 0;
    }

    num_mtfs = 0;
    for (i = 0; i < num_transmits_; i++) {
      symbol = transmits_[i].symbol;
      if ((sd_[symbol].type & 0x40) != 0) {
        sd_[symbol].hits++;
        num_mtfs++;
        if (transmits_[i].distance == 0xFFFFFFFF)
          sd_[symbol].type &= 0xBF;
      } else if (transmits_[i].distance != 0xFFFFFFFF) {
        if ((sd_[symbol].type & 2) != 0)
          sd_[symbol].type |= 0x40;
        else
          transmits_[i].distance = 0xFFFFFFFF;
      }
    }
#ifdef PRINTON
  fprintf(stderr, "%u MTF symbols\n", static_cast<unsigned>(num_mtfs));
#endif
  }

  for (i = 2; i <= kMaxInstancesForRemove; i++) {
    mtf_started[i] = 0;
    mtf_peak[i] = 0;
    mtf_peak_mtf[i] = 0;
    mtf_active[i] = 0;
    mtf_hits[i] = 0;
    mtf_in_dictionary[i] = 0;
  }

  for (i = 0; i < num_codes; i++)
    sd_[i].array_index = 0;

  for (i = 0; i < num_transmits_; i++) {
    symbol = transmits_[i].symbol;
    sd_[symbol].array_index++;
    const uint32_t count = sd_[symbol].count;
    if (count <= kMaxInstancesForRemove) {
      if (mtf_in_dictionary[count] > mtf_peak_mtf[count])
        mtf_peak_mtf[count] = mtf_in_dictionary[count];
      if (mtf_active[count] > mtf_peak[count])
        mtf_peak[count] = mtf_active[count];
      if (sd_[symbol].array_index == 1) {
        mtf_started[count]++;
        mtf_active[count]++;
        if (transmits_[i].distance != 0xFFFFFFFF)
          sd_[symbol].type |= 0x40;
        else
          mtf_in_dictionary[count]++;
      } else if (sd_[symbol].array_index == count) {
        mtf_active[count]--;
        if ((sd_[symbol].type & 0x40) != 0) {
          sd_[symbol].type &= 0xBF;
          mtf_hits[count]++;
        } else
          mtf_in_dictionary[count]--;
      } else {
        if ((sd_[symbol].type & 0x40) != 0) {
          mtf_hits[count]++;
          if (transmits_[i].distance == 0xFFFFFFFF) {
            sd_[symbol].type &= 0xBF;
            mtf_in_dictionary[count]++;
          }
        } else if (transmits_[i].distance != 0xFFFFFFFF) {
          sd_[symbol].type |= 0x40;
          mtf_in_dictionary[count]--;
        }
      }
    } else if ((transmits_[i].distance != 0xFFFFFFFF) && ((sd_[symbol].type & 2) == 0))
      transmits_[i].distance = 0xFFFFFFFF;
  }

  if (use_mtf_ == 2) {
    use_mtf_ = 0;
    double sum_expected_peak = 0.0;
    double sum_actual_peak = 0.0;
    for (i = 2; i <= 15; i++) {
      sum_expected_peak += static_cast<double>(i - 1) * static_cast<double>(mtf_started[i]) * (1.0 - (1.0 / static_cast<double>(1 << (i - 1))));
      sum_actual_peak += static_cast<double>(i - 1) * static_cast<double>(mtf_peak_mtf[i]);
    }
    double score1, score2;
    score1 = 5.75 * static_cast<double>(mtf_started[2]) / (static_cast<double>(mtf_peak_mtf[2]) * (32.8 - log2(static_cast<double>(num_symbols_to_code))));
    score2 = sum_expected_peak / sum_actual_peak;
    if (score1 + score2 > 2.08)
      use_mtf_ = 1;
  }

  if (use_mtf_ != 0) {
    for (i = 0; i < num_codes; i++) {
      if (sd_[i].count > kMaxInstancesForRemove) {
        if ((sd_[i].type & 2) != 0) {
          if (sd_[i].count - sd_[i].hits <= kMaxInstancesForRemove) {
            num_symbols_to_code -= sd_[i].count - (kMaxInstancesForRemove + 1);
            sd_[i].count = kMaxInstancesForRemove + 1;
          } else {
            num_symbols_to_code -= sd_[i].hits;
            sd_[i].count -= sd_[i].hits;
          }
        }
      } else
        num_symbols_to_code -= sd_[i].hits;
    }

    max_ptr = ranked_symbols.data() + num_more_than_15_inst_definitions;
    ranked_symbols2_ptr = ranked_symbols2.data() + num_more_than_15_inst_definitions;
    for (i = kMaxInstancesForRemove + 1; i < 501; i++) {
      ranked_symbols_ptr = max_ptr;
      while ((--ranked_symbols_ptr >= ranked_symbols.data()) && (sd_[*ranked_symbols_ptr].count == i))
        *--ranked_symbols2_ptr = *ranked_symbols_ptr;
      max_ptr = ranked_symbols_ptr + 1;
      while (--ranked_symbols_ptr >= ranked_symbols.data()) {
        if (sd_[*ranked_symbols_ptr].count == i)
          *--ranked_symbols2_ptr = *ranked_symbols_ptr;
      }
    }
    num_greater_500 = ranked_symbols2_ptr - ranked_symbols2.data();
    ranked_symbols_ptr = max_ptr;
    while (--ranked_symbols_ptr >= ranked_symbols.data()) {
      if (sd_[*ranked_symbols_ptr].count >= 501)
        *--ranked_symbols2_ptr = *ranked_symbols_ptr;
    }
    for (i = 1; i < num_greater_500; i++) {
      const uint32_t temp_symbol = ranked_symbols2[i];
      const uint32_t temp_count = sd_[temp_symbol].count;
      if (temp_count > sd_[ranked_symbols2[i - 1]].count) {
        ranked_symbols2[i] = ranked_symbols2[i - 1];
        j = i - 1;
        while ((j != 0) && (temp_count > sd_[ranked_symbols2[j - 1]].count)) {
          ranked_symbols2[j] = ranked_symbols2[j - 1];
          j--;
        }
        ranked_symbols2[j] = temp_symbol;
      }
    }
    for (i = 0; i < num_more_than_15_inst_definitions; i++)
      ranked_symbols[i] = ranked_symbols2[i];
    peak_array = mtf_peak_mtf;
  } else {
    for (i = 0; i < num_codes; i++)
      sd_[i].type &= 0xFD;
    for (i = 2; i <= kMaxInstancesForRemove; i++)
      mtf_hits[i] = 0;
    peak_array = mtf_peak;
  }
  sum_peaks = 0;
  for (i = 2; i <= kMaxInstancesForRemove; i++)
    sum_peaks += peak_array[i];

  if (peak_array[2] != 0) {
    queue_miss_code_length_[2] = static_cast<uint8_t>(static_cast<uint32_t>(0.5 + log2(static_cast<double>(num_symbols_to_code)
        * static_cast<double>(peak_array[2]) / static_cast<double>(mtf_started[2] - mtf_hits[2]))));
    if (queue_miss_code_length_[2] > 25)
      queue_miss_code_length_[2] = 25;
    if (queue_miss_code_length_[2] <= max_regular_code_length_)
      queue_miss_code_length_[2] = max_regular_code_length_ + 1;
  } else
    queue_miss_code_length_[2] = 25;

  remaining_code_space = 1 << 30;
  while ((queue_miss_code_length_[2] < 25) && ((remaining_code_space >> (30 - queue_miss_code_length_[2])) <
      static_cast<int32_t>(sum_peaks + 2 * num_more_than_15_inst_definitions)))
    queue_miss_code_length_[2]++;
  sum_peaks -= peak_array[2];
  remaining_code_space -= (1 << (30 - queue_miss_code_length_[2]));
  remaining_code_space -= (1 << (30 - queue_miss_code_length_[2])) * peak_array[2];

  for (i = 3; i <= kMaxInstancesForRemove; i++) {
    if (peak_array[i] != 0) {
      queue_miss_code_length_[i] = static_cast<uint8_t>(static_cast<uint32_t>(0.5 + log2(static_cast<double>(num_symbols_to_code)
          * static_cast<double>(peak_array[i]) / static_cast<double>(mtf_started[i] * (i - 1) - mtf_hits[i]))));
      if (queue_miss_code_length_[i] > queue_miss_code_length_[i - 1])
        queue_miss_code_length_[i] = queue_miss_code_length_[i - 1];
      else if (queue_miss_code_length_[i] < queue_miss_code_length_[i - 1] - 1)
        queue_miss_code_length_[i] = queue_miss_code_length_[i - 1] - 1;
    } else
      queue_miss_code_length_[i] = queue_miss_code_length_[i - 1];
    while ((remaining_code_space >> (30 - queue_miss_code_length_[i])) <
        static_cast<int32_t>(sum_peaks + 2 * num_more_than_15_inst_definitions)) {
      queue_miss_code_length_[i]++;
      uint8_t jj = i;
      while ((jj > 2) && (queue_miss_code_length_[jj] > queue_miss_code_length_[jj - 1])) {
        queue_miss_code_length_[--jj]++;
        remaining_code_space += (1 << (30 - queue_miss_code_length_[jj])) * peak_array[jj];
      }
    }
    sum_peaks -= peak_array[i];
    remaining_code_space -= (1 << (30 - queue_miss_code_length_[i])) * peak_array[i];
  }
  if ((use_mtf_ != 0) && (queue_miss_code_length_[12] <= max_regular_code_length_))
    max_regular_code_length_--;

  remaining_symbols_to_code = 0;
  for (i = 0; i < num_more_than_15_inst_definitions; i++)
    remaining_symbols_to_code += sd_[ranked_symbols[i]].count - 1;
  mtf_miss_code_space = 0;
  for (i = 2; i <= kMaxInstancesForRemove; i++) {
    if (queue_miss_code_length_[i] <= max_regular_code_length_)
      queue_miss_code_length_[i] = max_regular_code_length_ + 1;
    mtf_miss_code_space += (1 << (30 - queue_miss_code_length_[i])) * peak_array[i];
  }

  max_code_length_ = queue_miss_code_length_[2];
  remaining_code_space = (1 << 30) - (1 << (30 - max_code_length_)) - mtf_miss_code_space;
  min_code_space = 1u << (30 - (queue_miss_code_length_[kMaxInstancesForRemove] - 1));
  codes_per_code_space = static_cast<double>(remaining_symbols_to_code) / static_cast<double>(remaining_code_space);
  max_regular_code_length_ = 1;
  prior_repeats = 0;
  for (i = 0; i <= 25; i++)
    index_last_length[i] = -1;

  for (i = 0; i < num_more_than_15_inst_definitions; i++) {
    symbol_inst = sd_[ranked_symbols[i]].count;
    if (--symbol_inst != prior_repeats) {
      prior_repeats = symbol_inst;
      symbol_inst_factor = static_cast<double>(0x5A827999) / static_cast<double>(symbol_inst);
      code_length_limit = static_cast<uint32_t>(log2(symbol_inst_factor * codes_per_code_space));
      if (code_length_limit < 2)
        code_length_limit = 2;
    }
    code_length = static_cast<uint8_t>(log2(symbol_inst_factor
        * (1.0 + static_cast<double>(remaining_symbols_to_code - 15 * static_cast<int32_t>(num_more_than_15_inst_definitions - i - 1)))
        / static_cast<double>(remaining_code_space - static_cast<int32_t>(min_code_space) * static_cast<int32_t>(num_more_than_15_inst_definitions - i - 1))));
    if (code_length < code_length_limit)
      code_length = code_length_limit;
    if (code_length >= queue_miss_code_length_[kMaxInstancesForRemove])
      code_length = queue_miss_code_length_[kMaxInstancesForRemove] - 1;
    else
      while (static_cast<int32_t>(num_more_than_15_inst_definitions - i - 1) * static_cast<int32_t>(min_code_space) > remaining_code_space - (1 << (30 - code_length)))
        code_length++;

    if (code_length > max_regular_code_length_)
      max_regular_code_length_ = code_length;

    if ((i != 0) && (sd_[ranked_symbols[i - 1]].code_length > code_length)) {
      code_length = sd_[ranked_symbols[i - 1]].code_length - 1;
      uint32_t temp_code_length = code_length - 1;
      if (index_last_length[code_length] == -1) {
        while ((index_last_length[temp_code_length] == -1) && (temp_code_length != 0))
          temp_code_length--;
        index_last_length[code_length] = index_last_length[temp_code_length] + 1;
      } else
        index_last_length[code_length]++;
      index_last_length[code_length + 1] = i;
      sd_[ranked_symbols[index_last_length[code_length]]].code_length = code_length;
      sd_[ranked_symbols[i]].code_length = code_length + 1;
    } else {
      index_last_length[code_length] = i;
      sd_[ranked_symbols[i]].code_length = code_length;
    }
    remaining_code_space -= 1 << (30 - code_length);
    remaining_symbols_to_code -= symbol_inst;
  }

  do {
    const uint8_t min_code_length_val = [&]() {
      uint8_t mcl = sd_[ranked_symbols[0]].code_length;
      for (uint32_t ii = mcl; ii < max_regular_code_length_; ii++)
        if (index_last_length[ii] == -1)
          mcl = ii + 1;
      return mcl;
    }();
    max_len_adj_profit = 0;
    for (i = min_code_length_val; i < max_regular_code_length_; i++) {
      int32_t len_adj_profit = -(sd_[ranked_symbols[index_last_length[i]]].count - 1);
      for (j = i + 2; j <= max_regular_code_length_; j++) {
        len_adj_profit += sd_[ranked_symbols[index_last_length[j - 1] + 1]].count - 1;
        if (index_last_length[j] != index_last_length[j - 1] + 1) {
          const int32_t next_symbol_profit = sd_[ranked_symbols[index_last_length[j - 1] + 2]].count - 1;
          if (len_adj_profit + next_symbol_profit > max_len_adj_profit) {
            max_len_adj_profit = len_adj_profit + next_symbol_profit;
            increase_length = i;
            decrease_length = j;
          }
        }
      }
    }
    if (max_len_adj_profit != 0) {
      sd_[ranked_symbols[index_last_length[increase_length]--]].code_length++;
      for (j = increase_length + 2; j <= decrease_length; j++)
        sd_[ranked_symbols[++index_last_length[j - 1]]].code_length--;
      sd_[ranked_symbols[++index_last_length[decrease_length - 1]]].code_length--;
      if (index_last_length[decrease_length - 1] == index_last_length[decrease_length])
        index_last_length[decrease_length] = -1;
    }
  } while (max_len_adj_profit != 0);

  j = 0;
  for (i = sd_[ranked_symbols[0]].code_length; i < max_regular_code_length_; i++) {
    if (index_last_length[i] >= 0) {
      index_first_length[i] = j;
      j = index_last_length[i] + 1;
    } else
      index_first_length[i] = -1;
  }

  do {
    max_len_adj_profit = 0;
    for (i = sd_[ranked_symbols[0]].code_length; i < max_regular_code_length_; i++) {
      if (index_last_length[i] - index_first_length[i] >= 2) {
        int32_t len_adj_profit = sd_[ranked_symbols[index_first_length[i]]].count - 1;
        len_adj_profit -= sd_[ranked_symbols[index_last_length[i]]].count - 1;
        len_adj_profit -= sd_[ranked_symbols[index_last_length[i] - 1]].count - 1;
        if (len_adj_profit > max_len_adj_profit) {
          max_len_adj_profit = len_adj_profit;
          decrease_length = i;
        }
      }
    }
    if (max_len_adj_profit != 0) {
      sd_[ranked_symbols[index_first_length[decrease_length]]].code_length--;
      sd_[ranked_symbols[index_last_length[decrease_length] - 1]].code_length++;
      sd_[ranked_symbols[index_last_length[decrease_length]]].code_length++;
      if (index_first_length[decrease_length - 1] < 0)
        index_first_length[decrease_length - 1] = index_first_length[decrease_length];
      index_last_length[decrease_length - 1] = index_first_length[decrease_length];
      if (index_first_length[decrease_length + 1] < 0)
        index_last_length[decrease_length + 1] = index_last_length[decrease_length];
      index_first_length[decrease_length + 1] = index_last_length[decrease_length] - 1;
      if (index_last_length[i] - index_first_length[i] == 2) {
        index_first_length[decrease_length] = -1;
        index_last_length[decrease_length] = -1;
      } else {
        index_first_length[decrease_length] += 1;
        index_last_length[decrease_length] -= 2;
      }
    }
  } while (max_len_adj_profit != 0);

  for (i = 0; i < num_more_than_15_inst_definitions; i++)
    if (sd_[ranked_symbols[i]].code_length < 11)
      sd_[ranked_symbols[i]].type &= 0xFD;

  for (i = num_more_than_15_inst_definitions; i < num_definitions_to_code; i++) {
    sd_[ranked_symbols[i]].type &= 0xBF;
    sd_[ranked_symbols[i]].code_length = queue_miss_code_length_[sd_[ranked_symbols[i]].count];
  }

  if (num_definitions_to_code == 0) {
    max_regular_code_length_ = 24;
    sd_[ranked_symbols[0]].code_length = 25;
  } else if (sd_[ranked_symbols[0]].count <= kMaxInstancesForRemove)
    max_regular_code_length_ = sd_[ranked_symbols[0]].code_length - 1;

  sd_[num_codes].type = 0;
  if (max_code_length_ >= 14) {
    i = 0;
    while (i < num_more_than_15_inst_definitions) {
     if ((sd_[ranked_symbols[i]].type & 8) != 0) {
        if (sd_[ranked_symbols[i]].space_score > 0)
          sd_[ranked_symbols[i]].type += 8;
        else
          sd_[ranked_symbols[i]].type += 0x10;
      }
      i++;
    }
    for (i = num_base_symbols_; i < num_codes; i++) {
      if ((sd_[i].type & 0x10) != 0) {
        uint32_t last_sym = symbol_array_[sd_[i + 1].symbol_start_index - 2];
        while (last_sym >= num_base_symbols_) {
          if ((sd_[last_sym].type & 0x18) == 0x10) {
            sd_[i].type = (sd_[i].type & 0x6F) | 8;
            break;
          }
          last_sym = symbol_array_[sd_[last_sym + 1].symbol_start_index - 2];
        }
      }
    }
  } else {
    for (i = 0; i < num_definitions_to_code; i++)
      sd_[ranked_symbols[i]].type &= 0x63;
  }

#ifdef PRINTON
  fprintf(stderr, "use_mtf %u, mcl %u mrcl %u\n",
      static_cast<unsigned>(use_mtf_), static_cast<unsigned>(max_code_length_), static_cast<unsigned>(max_regular_code_length_));
  if (verbose != 0) {
    if (verbose == 1) {
      for (i = 0; i < num_definitions_to_code; i++) {
        if (sd_[ranked_symbols[i]].code_length >= 11) {
          if (use_mtf_ != 0)
            printf("%u: #%u %u L%u D%02x: \"", i + 1, sd_[ranked_symbols[i]].array_index, sd_[ranked_symbols[i]].count,
                sd_[ranked_symbols[i]].code_length, sd_[ranked_symbols[i]].type & 0xCE);
          else
            printf("%u: #%u L%u D%02x: \"", i + 1, sd_[ranked_symbols[i]].count, sd_[ranked_symbols[i]].code_length,
                sd_[ranked_symbols[i]].type & 0xCE);
        } else
          printf("%u: #%u L%u: \"", i + 1, sd_[ranked_symbols[i]].count, sd_[ranked_symbols[i]].code_length);
        print_string(ranked_symbols[i]);
        printf("\"\n");
      }
      uint32_t temp_rank = num_definitions_to_code;
      for (i = 0; i < num_base_symbols_; i++) {
        if (sd_[i].count == 1) {
          printf("%u: #1: \"", ++temp_rank);
          print_string(i);
          printf("\"\n");
        }
      }
    } else {
      for (i = 0; i < num_codes; i++) {
        if ((sd_[i].array_index != 0) && ((sd_[i].array_index > 1) || (i < num_base_symbols_))) {
          if (sd_[i].code_length >= 11) {
            if (use_mtf_ != 0)
              printf("%u: #%u %u L%u D%02x: \"", i, sd_[i].array_index, sd_[i].count, sd_[i].code_length, sd_[i].type & 0xCE);
            else
              printf("%u: #%u L%u D%02x: \"", i, static_cast<unsigned>(sd_[i].count), sd_[i].code_length, sd_[i].type & 0xCE);
          } else
            printf("%u: #%u L%u: \"", i, sd_[i].count, sd_[i].code_length);
          print_string(i);
          printf("\"\n");
        } else if ((i < num_base_symbols_) && (sd_[i].count == 1)) {
          printf("%u: #%u D%02x: \"", i, sd_[i].count, sd_[i].type & 0xCE);
          printf("%c", static_cast<unsigned char>(i));
          printf("\"\n");
        }
      }
    }
  }
#endif

  symbol_ptr = symbol_array_.data();
  prior_is_cap_ = 0;
  temp_char = 0xFF;
  if (UTF8_compliant_ != 0)
    temp_char = 0x90;

  size_t enc_buf_size = insize * 4 + filesize + 100000;
  std::vector<uint8_t> enc_buf(enc_buf_size);
  model_.init_encoder(temp_char,
      static_cast<uint8_t>(kMaxInstancesForRemove + static_cast<uint32_t>(max_regular_code_length_ - sd_[ranked_symbols[0]].code_length) + 1),
      cap_encoded_, UTF8_compliant_, use_mtf_, enc_buf.data());
  model_.set_out_buffer_capacity(enc_buf_size);

  // HEADER:
  // BYTE 0:  4.0 * log2(file_size)
  // BYTE 1:  7=cap_encoded, 6=UTF8_compliant, 5=use_mtf, 4-0=max_code_length-1
  // BYTE 2:  7=M4D, 6=M3D, 5=use_delta, 4-0=min_code_length-1
  // BYTE 3:  7=M7D, 6=M6D, 5=M5D, 4-0=max_code_length-max_regular_code_length
  // BYTE 4:  7=M15D, 6=M14D, 5=M13D, 4=M12D, 3=M11D, 2=M10D, 1=M9D, 0=M8D
  // if UTF8_compliant
  // BYTE 5:  7-5=unused, 4-0=base_bits
  // else if use_delta
  //   if stride <= 4
  // BYTE 5:  7=0, 6-5=unused, 4=two channel, 3=little endian, 2=any endian, 1-0=stride-1
  //   else
  // BYTE 5:  7=1, 6-0=stride

  dictionary_size = sum_dictionary_string_bytes(num_codes, first_define_ptr);
  model_.write_out_buffer(static_cast<uint8_t>(12.5 * (log2(static_cast<double>(dictionary_size + 0x400)) - 10.0)) + 1);
  model_.write_out_buffer((cap_encoded_ << 7) | (UTF8_compliant_ << 6) | (use_mtf_ << 5) | (queue_miss_code_length_[2] - 1));
  temp_char = (((format & 0xFE) != 0) << 5) | (sd_[ranked_symbols[0]].code_length - 1);
  if (queue_miss_code_length_[3] != queue_miss_code_length_[2])
    temp_char |= 0x40;
  if (queue_miss_code_length_[4] != queue_miss_code_length_[3])
    temp_char |= 0x80;
  model_.write_out_buffer(temp_char);
  i = 7;
  do {
    temp_char = (temp_char << 1) | (queue_miss_code_length_[i] != queue_miss_code_length_[i - 1]);
  } while (--i != 4);
  model_.write_out_buffer((temp_char << 5) | (queue_miss_code_length_[2] - max_regular_code_length_));
  i = 15;
  do {
    temp_char = (temp_char << 1) | (queue_miss_code_length_[i] != queue_miss_code_length_[i - 1]);
  } while (--i != 7);
  model_.write_out_buffer(temp_char);
  i = 0xFF;
  if (UTF8_compliant_ != 0) {
    model_.write_out_buffer(base_bits);
    i = 0x90;
  } else if ((format & 0xFE) != 0) {
    if ((format & 0x80) == 0)
      model_.write_out_buffer(((format & 0xF0) >> 2) | (((format & 0xE) >> 1) - 1));
    else
      model_.write_out_buffer(format);
  }
  do {
    for (j = 1; j <= max_code_length_; j++) {
      sym_list_bits_[i][j] = 2;
      sym_list_ptrs_[i][j].assign(4, 0);
      nsob_[i][j] = 0;
      nbob_[i][j] = 0;
      fbob_[i][j] = 0;
    }
    sum_nbob_[i] = 0;
    bin_code_length_[i] = max_code_length_;
    symbol_lengths_[i] = 0;
  } while (i--);

  for (i = 0; i < num_more_than_15_inst_definitions; i++)
    sd_[ranked_symbols[i]].count = sd_[ranked_symbols[i]].code_length + kMaxInstancesForRemove;
  for (i = 0; i < num_codes; i++)
    sd_[i].hits = 0;
  num_transmits_ = queue_size_ = queue_size_az_ = queue_size_space_ = queue_size_other_ = queue_offset_ = 0;
  found_first_symbol_ = prior_end_ = 0;
  cap_symbol_defined_ = cap_lock_symbol_defined_ = 0;
  num_grammar_rules_ = 1;
  prior_symbol_ = num_codes;

  symbol = *symbol_ptr++;
  sd_[symbol].hits++;
  if (embed_define(symbol, 0) == 0) {
    goto encode_cleanup;
  }

  while (symbol_ptr < first_define_ptr) {
    if (model_.read_encoder_failed() != 0)
      break;
    symbol = *symbol_ptr++;
    symbol_inst = sd_[symbol].hits++;
    if (symbol_inst == 0) {
      if (cap_encoded_ != 0) {
        if (prior_is_cap_ == 0)
          model_.encode_new_type(0, 4 + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7), prior_end_, queue_size_);
        else
          model_.encode_new_type(2, 0x2C + (sd_[prior_symbol_].type & 3), 'C', queue_size_az_);
      } else
        model_.encode_new_type_binary(0, prior_end_, queue_size_);
      if (embed_define(symbol, 0) == 0) {
        goto encode_cleanup;
      }
    } else {
      if ((sd_[symbol].type & 0x40) != 0) {
        if (prior_is_cap_ != 0 && (sd_[symbol].type & 1) != 0)
          update_queue_prior_cap(symbol, 0);
        else
          update_queue(symbol, 0);
      } else {
        if (cap_encoded_ != 0) {
          if (prior_is_cap_ == 0)
            model_.encode_dict_type(0, 4 + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7), prior_end_, queue_size_);
          else
            model_.encode_dict_type(2, 0x2C + (sd_[prior_symbol_].type & 3), 'C', queue_size_az_);
        } else
          model_.encode_dict_type_binary(0, prior_end_, queue_size_);
        encode_dictionary_symbol(symbol);

        if (sd_[symbol].count <= kMaxInstancesForRemove) {
          if (sd_[symbol].count == sd_[symbol].hits)
            remove_dictionary_symbol(symbol, sd_[symbol].code_length);
          else if (use_mtf_ != 0) {
            if ((sd_[symbol].type & 2) != 0) {
              if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
                if ((sd_[symbol].hits + 1 != sd_[symbol].count) || ((sd_[symbol].type & 0x80) != 0)) {
                  if (cap_encoded_ != 0)
                    model_.encode_go_mtf(6 * (sd_[symbol].count - 1) + prior_is_cap_ + (sd_[symbol].type & 1)
                        + 3 * (((sd_[symbol].type >> 3) & 3) == 2), 0, 1);
                  else
                    model_.encode_go_mtf(6 * (sd_[symbol].count - 1), 0, 1);
                }
                remove_dictionary_symbol(symbol, sd_[symbol].code_length);
                add_symbol_to_queue(symbol);
              } else {
                if (cap_encoded_ != 0)
                  model_.encode_go_mtf(6 * (sd_[symbol].count - 1) + prior_is_cap_ + (sd_[symbol].type & 1)
                      + 3 * (((sd_[symbol].type >> 3) & 3) == 2), 0, 0);
                else
                  model_.encode_go_mtf(6 * (sd_[symbol].count - 1), 0, 0);
              }
            }
          }
        } else if ((sd_[symbol].type & 2) != 0) {
          uint16_t context = 6 * (sd_[symbol].count - 1);
          if (cap_encoded_ != 0)
            context += prior_is_cap_ + (sd_[symbol].type & 1) + 3 * (((sd_[symbol].type >> 3) & 3) == 2);
          if (transmits_[num_transmits_].distance != 0xFFFFFFFF) {
            model_.encode_go_mtf(context, 0, 1);
            remove_dictionary_symbol(symbol, sd_[symbol].code_length);
            add_symbol_to_queue(symbol);
          } else
            model_.encode_go_mtf(context, 0, 0);
        }
      }
      prior_is_cap_ = (sd_[symbol].type & 0x20) >> 5;
      prior_symbol_ = symbol;
      num_transmits_++;
      prior_end_ = sd_[symbol].ends;
    }
  }

  if (model_.read_encoder_failed() != 0) {
    fprintf(stderr,
        "GLZA encode: encoder failed mid-stream (grammar_bytes=%u enc_buf_size=%zu OutCharNum=%u)\n",
        static_cast<unsigned>(grammar_size), enc_buf_size, static_cast<unsigned>(model_.read_out_char_num()));
    global_diagnostics().set("encode",
                 "encoder failed mid-stream (grammar_bytes=%u enc_buf_size=%zu "
                 "OutCharNum=%u; enc_buf heuristic is grammar*4+file+%u)",
                 static_cast<unsigned>(grammar_size), enc_buf_size,
                 static_cast<unsigned>(model_.read_out_char_num()), 100000u);
    i = 0xFF;
    if (UTF8_compliant_ != 0)
      i = 0x90;
    goto encode_cleanup;
  }

  if (cap_encoded_ != 0) {
    model_.encode_dict_type(0, 4 + (sd_[prior_symbol_].type & 0x18) + 2 * (sd_[prior_symbol_].type & 7), prior_end_, queue_size_);
    model_.encode_first_char(end_char_, (sd_[prior_symbol_].type & 0x18) >> 3, prior_end_);
  } else {
    model_.encode_dict_type_binary(0, prior_end_, queue_size_);
    if (UTF8_compliant_ != 0)
      model_.encode_first_char(end_char_, 0, prior_end_);
    else
      model_.encode_first_char_binary(end_char_, prior_end_);
  }
  code_length = bin_code_length_[end_char_];
  model_.encode_short_dictionary_symbol(fbob_[end_char_][max_code_length_], sum_nbob_[end_char_], nbob_[end_char_][code_length]);
  model_.finish_encoder();
  i = 0xFF;
  if (UTF8_compliant_ != 0)
    i = 0x90;
encode_cleanup:
  do {
    for (j = 1; j <= max_code_length_; j++)
      sym_list_ptrs_[i][j].clear();
  } while (i--);
  symbol_array_.clear();
  sd_.clear();
  transmits_.clear();
  if (model_.read_encoder_failed() != 0) {
    return false;
  }
  *outsize_ptr = model_.read_out_char_num();
  if (fd_out != nullptr) {
    size_t writesize = 0;
    while (*outsize_ptr - writesize > kWriteSize) {
      fwrite(enc_buf.data() + writesize, 1, kWriteSize, fd_out);
      writesize += kWriteSize;
      fflush(fd_out);
    }
    fwrite(enc_buf.data() + writesize, 1, *outsize_ptr - writesize, fd_out);
    fflush(fd_out);
  } else if (*outsize_ptr > filesize) {
    fprintf(stderr,
        "GLZA encode: encoded size %zu exceeds caller buffer %zu (grammar_bytes=%u enc_buf_size=%zu)\n",
        *outsize_ptr, filesize, static_cast<unsigned>(grammar_size), enc_buf_size);
    return false;
  } else
    std::memcpy(outbuf, enc_buf.data(), *outsize_ptr);
  model_.reset_codec_globals();
  return true;
}

}  // namespace glza
