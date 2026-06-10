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

#include "glza_decode.h"
#include <cmath>
#include <cstring>

namespace glza {


Decoder::SymData* Decoder::add_dictionary_symbol(uint8_t bits, uint8_t first_char) {
  auto& bin_info = bin_data_[first_char][bits];
  if (bin_info.nsob == bin_info.sym_list_size) {
    bin_info.sym_list_size <<= 1;
    bin_info.symbol_data.resize(bin_info.sym_list_size);
  }
  if ((bin_info.nsob << (32 - bits)) == (static_cast<uint32_t>(bin_info.nbob) << (32 - bin_code_length_[first_char]))) {
    if (bits >= bin_code_length_[first_char]) {
      bin_info.nbob++;
      if (sum_nbob_[first_char] < 0x1000) {
        sum_nbob_[first_char]++;
        if (bits != max_code_length_) {
          lookup_bits_[first_char][bin_data_[first_char][bits + 1].fbob] = bits;
          while (++bits != max_code_length_) {
            if (bin_data_[first_char][bits].nbob != 0)
              lookup_bits_[first_char][bin_data_[first_char][bits + 1].fbob] = bits;
            bin_data_[first_char][bits].fbob++;
          }
          bin_data_[first_char][max_code_length_].fbob++;
        }
      } else {
        bin_code_length_[first_char]--;
        uint16_t first_max_code_length = bin_data_[first_char][max_code_length_].fbob;
        sum_nbob_[first_char]
            = (bin_data_[first_char][min_code_length_].nbob = (bin_data_[first_char][min_code_length_].nbob + 1) >> 1);
        for (bits = min_code_length_ + 1; bits <= max_code_length_; bits++) {
          bin_data_[first_char][bits].fbob = sum_nbob_[first_char];
          sum_nbob_[first_char] += (bin_data_[first_char][bits].nbob = (bin_data_[first_char][bits].nbob + 1) >> 1);
        }
        uint16_t bin = 0;
        for (bits = min_code_length_; bits < max_code_length_; bits++)
          while (bin < bin_data_[first_char][bits + 1].fbob)
            lookup_bits_[first_char][bin++] = bits;
        while (bin < first_max_code_length)
          lookup_bits_[first_char][bin++] = max_code_length_;
      }
    } else {
      uint32_t new_bins = 1 << (bin_code_length_[first_char] - bits);
      if (sum_nbob_[first_char] + new_bins <= 0x1000) {
        bin_info.nbob += new_bins;
        sum_nbob_[first_char] += new_bins;
        if (bits != max_code_length_) {
          uint8_t code_length = max_code_length_;
          do {
            bin_data_[first_char][code_length--].fbob += new_bins;
            uint16_t bin;
            if (bin_data_[first_char][code_length].nbob >= new_bins)
              for (bin = bin_data_[first_char][code_length + 1].fbob - new_bins;
                  bin < bin_data_[first_char][code_length + 1].fbob; bin++)
                lookup_bits_[first_char][bin] = code_length;
            else
              for (bin = bin_data_[first_char][code_length].fbob + new_bins;
                  bin < bin_data_[first_char][code_length].fbob + new_bins + bin_data_[first_char][code_length].nbob; bin++)
                lookup_bits_[first_char][bin] = code_length;
          } while (code_length > bits);
        }
      } else if (new_bins <= 0x1000) {
        bin_info.nbob += new_bins;
        uint16_t first_max_code_length = bin_data_[first_char][max_code_length_].fbob;
        do {
          bin_code_length_[first_char]--;
          sum_nbob_[first_char]
              = (bin_data_[first_char][min_code_length_].nbob = (bin_data_[first_char][min_code_length_].nbob + 1) >> 1);
          for (bits = min_code_length_ + 1; bits <= max_code_length_; bits++)
            sum_nbob_[first_char] += (bin_data_[first_char][bits].nbob = (bin_data_[first_char][bits].nbob + 1) >> 1);
        } while (sum_nbob_[first_char] > 0x1000);
        uint16_t bin = bin_data_[first_char][min_code_length_].nbob;
        for (bits = min_code_length_ + 1; bits <= max_code_length_; bits++) {
          bin_data_[first_char][bits].fbob = bin;
          bin += bin_data_[first_char][bits].nbob;
        }
        bin = 0;
        for (bits = min_code_length_; bits < max_code_length_; bits++)
          while (bin < bin_data_[first_char][bits + 1].fbob)
            lookup_bits_[first_char][bin++] = bits;
        while (bin < first_max_code_length)
          lookup_bits_[first_char][bin++] = max_code_length_;
      } else if (sum_nbob_[first_char] == 0) {
        uint8_t bin_shift = bin_code_length_[first_char] - 12 - bits;
        bin_code_length_[first_char] -= bin_shift;
        bin_info.nbob = (new_bins >>= bin_shift);
        sum_nbob_[first_char] = new_bins;
        uint16_t bin = 0;
        while (bin < sum_nbob_[first_char])
          lookup_bits_[first_char][bin++] = bits;
        while (++bits <= max_code_length_)
          bin_data_[first_char][bits].fbob = sum_nbob_[first_char];
      } else {
        uint16_t first_max_code_length = bin_data_[first_char][max_code_length_].fbob;
        uint8_t bin_shift = bin_code_length_[first_char] - 11 - bits;
        bin_code_length_[first_char] -= bin_shift;
        bin_data_[first_char][min_code_length_].nbob = ((bin_data_[first_char][min_code_length_].nbob - 1) >> bin_shift) + 1;
        sum_nbob_[first_char] = bin_data_[first_char][min_code_length_].nbob;
        uint8_t code_length;
        for (code_length = min_code_length_ + 1; code_length <= max_code_length_; code_length++)
          sum_nbob_[first_char]
              += bin_data_[first_char][code_length].nbob = ((bin_data_[first_char][code_length].nbob - 1) >> bin_shift) + 1;
        bin_info.nbob += (new_bins >>= bin_shift);
        sum_nbob_[first_char] += new_bins;
        uint16_t bin = 0;
        for (bits = min_code_length_ + 1; bits <= max_code_length_; bits++)
          bin_data_[first_char][bits].fbob = (bin += bin_data_[first_char][bits - 1].nbob);
        bin = 0;
        for (bits = min_code_length_; bits < max_code_length_; bits++)
          while (bin < bin_data_[first_char][bits + 1].fbob)
            lookup_bits_[first_char][bin++] = bits;
        while (bin < first_max_code_length)
          lookup_bits_[first_char][bin++] = max_code_length_;
      }
    }
  }
  return &bin_info.symbol_data[bin_info.nsob++];
}


Decoder::SymData* Decoder::add_single_dictionary_symbol(uint8_t first_char) {
  auto& bin_info = bin_data_[first_char][max_code_length_ + 1];
  if (bin_info.nsob == bin_info.sym_list_size) {
    bin_info.sym_list_size <<= 1;
    bin_info.symbol_data.resize(bin_info.sym_list_size);
  }
  return &bin_info.symbol_data[bin_info.nsob++];
}


void Decoder::remove_dictionary_symbol(BinData& bin_info, uint32_t index) {
  bin_info.symbol_data[index].string_index = bin_info.symbol_data[--bin_info.nsob].string_index;
  bin_info.symbol_data[index].string_length = bin_info.symbol_data[bin_info.nsob].string_length;
  bin_info.symbol_data[index].four_bytes = bin_info.symbol_data[bin_info.nsob].four_bytes;
}


void Decoder::decode_queue_fail(const char* reason) {
  model_.set_decoder_failed(reason);
}


void Decoder::decode_queue_subcount_inc(uint16_t* subcount, const char* which) {
  if (*subcount >= 0xFF) {
    fprintf(stderr,
        "GLZA decode: %s queue overflow (count=%u total=%u az=%u space=%u other=%u)\n",
        which, static_cast<unsigned>(*subcount), static_cast<unsigned>(queue_size_),
        static_cast<unsigned>(queue_size_az_), static_cast<unsigned>(queue_size_space_),
        static_cast<unsigned>(queue_size_other_));
    decode_queue_fail("MTF sub-queue overflow");
    return;
  }
  (*subcount)++;
}


void Decoder::decode_queue_subcount_dec(uint16_t* subcount, const char* which) {
  if (*subcount == 0) {
    fprintf(stderr,
        "GLZA decode: %s queue underflow (total=%u az=%u space=%u other=%u)\n",
        which, static_cast<unsigned>(queue_size_), static_cast<unsigned>(queue_size_az_),
        static_cast<unsigned>(queue_size_space_), static_cast<unsigned>(queue_size_other_));
    decode_queue_fail("MTF sub-queue underflow");
    return;
  }
  (*subcount)--;
}


int Decoder::decode_dict_write_ok(uint32_t end_index, uint32_t add_bytes, const char* where) {
  if (model_.read_decoder_failed() != 0)
    return 0;
  if (end_index > dictionary_size_ || add_bytes > dictionary_size_ - end_index) {
    fprintf(stderr,
        "GLZA decode: dictionary write overflow at %s end=%u add=%u cap=%u\n",
        where, static_cast<unsigned>(end_index), static_cast<unsigned>(add_bytes),
        static_cast<unsigned>(dictionary_size_));
    decode_queue_fail("dictionary write overflow");
    return 0;
  }
  return 1;
}


int Decoder::decode_dict_ref_ok(uint32_t index, uint32_t length, const char* where) {
  if (model_.read_decoder_failed() != 0)
    return 0;
  if (length == 0 || index >= dictionary_size_ || length > dictionary_size_ - index) {
    fprintf(stderr,
        "GLZA decode: dictionary ref invalid at %s index=%u length=%u cap=%u\n",
        where, static_cast<unsigned>(index), static_cast<unsigned>(length),
        static_cast<unsigned>(dictionary_size_));
    decode_queue_fail("dictionary ref invalid");
    return 0;
  }
  return 1;
}


int Decoder::decode_lookup_ok(uint8_t first_char, uint16_t bin_num, const char* where) {
  if (model_.read_decoder_failed() != 0)
    return 0;
  if (bin_num >= 0x1000) {
    fprintf(stderr,
        "GLZA decode: lookup_bits OOB at %s first_char=%u bin=%u\n",
        where, static_cast<unsigned>(first_char), static_cast<unsigned>(bin_num));
    decode_queue_fail("lookup_bits index out of range");
    return 0;
  }
  return 1;
}


int Decoder::decode_lookup_store(uint8_t first_char, uint16_t bin, uint8_t bits, const char* where) {
  if (!decode_lookup_ok(first_char, bin, where))
    return 0;
  lookup_bits_[first_char][bin] = bits;
  return 1;
}


uint8_t Decoder::decode_lookup_load(uint8_t first_char, uint16_t bin_num, const char* where) {
  if (!decode_lookup_ok(first_char, bin_num, where))
    return max_code_length_;
  return lookup_bits_[first_char][bin_num];
}


int Decoder::decode_append_byte(uint32_t* end_ptr, uint8_t byte, const char* where) {
  if (!decode_dict_write_ok(*end_ptr, 1, where))
    return 0;
  symbol_strings_[(*end_ptr)++] = byte;
  return 1;
}


int Decoder::decode_append_ref(uint32_t* end_ptr, uint32_t src_index, uint32_t length, const char* where) {
  if (length == 1) {
    if (!decode_dict_write_ok(*end_ptr, 1, where) || !decode_dict_ref_ok(src_index, 1, where))
      return 0;
    symbol_strings_[(*end_ptr)++] = symbol_strings_[src_index];
    return 1;
  }
  if (!decode_dict_write_ok(*end_ptr, length, where) || !decode_dict_ref_ok(src_index, length, where))
    return 0;
  const uint8_t* src = &symbol_strings_[src_index];
  const uint8_t* end_src = src + length;
  while (src != end_src)
    symbol_strings_[(*end_ptr)++] = *src++;
  return 1;
}


int Decoder::decode_append_sym(uint32_t* end_ptr, SymData* sym, uint8_t len1_byte, const char* where) {
  if (sym == nullptr) {
    decode_queue_fail("null sym_data_ptr in dictionary copy");
    return 0;
  }
  if (sym->string_length == 1)
    return decode_append_byte(end_ptr, len1_byte, where);
  return decode_append_ref(end_ptr, sym->string_index, sym->string_length, where);
}


int Decoder::decode_dict_index_ok(uint8_t first_char, uint8_t code_length, uint32_t index, const char* where) {
  auto& bin_info = bin_data_[first_char][code_length];
  if (index >= bin_info.nsob || index >= bin_info.sym_list_size) {
    fprintf(stderr,
        "GLZA decode: dictionary index OOB at %s fc=%u cl=%u index=%u nsob=%u list=%u\n",
        where, static_cast<unsigned>(first_char), static_cast<unsigned>(code_length),
        static_cast<unsigned>(index), static_cast<unsigned>(bin_info.nsob),
        static_cast<unsigned>(bin_info.sym_list_size));
    decode_queue_fail("dictionary symbol index out of range");
    return 0;
  }
  return 1;
}


int Decoder::decode_dict_fetch(uint8_t first_char, uint16_t bin_num, uint8_t* code_length_out,
    uint32_t* index_out, SymData** sym_out, const char* where) {
  if (model_.read_decoder_failed() != 0)
    return 0;
  if (bin_num >= sum_nbob_[first_char]) {
    fprintf(stderr,
        "GLZA decode: DecodeBin out of range at %s fc=%u bin=%u sum_nbob=%u\n",
        where, static_cast<unsigned>(first_char), static_cast<unsigned>(bin_num),
        static_cast<unsigned>(sum_nbob_[first_char]));
    decode_queue_fail("DecodeBin out of range");
    return 0;
  }
  uint8_t code_length = decode_lookup_load(first_char, bin_num, where);
  auto& bin_info = bin_data_[first_char][code_length];
  if (bin_info.nsob == 0) {
    fprintf(stderr,
        "GLZA decode: empty dictionary bin at %s fc=%u cl=%u bin=%u\n",
        where, static_cast<unsigned>(first_char), static_cast<unsigned>(code_length),
        static_cast<unsigned>(bin_num));
    decode_queue_fail("dictionary bin empty");
    return 0;
  }
  uint32_t index = get_dictionary_index(bin_num, code_length, first_char);
  if (!decode_dict_index_ok(first_char, code_length, index, where))
    return 0;
  *code_length_out = code_length;
  *index_out = index;
  *sym_out = &bin_info.symbol_data[index];
  return 1;
}


Decoder::QueueData* Decoder::add_symbol_to_queue(SymData* sym_data_ptr, uint8_t code_length, uint8_t first_char) {
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  if (queue_size_ >= 0x100) {
    fprintf(stderr, "GLZA decode: unified MTF queue full (256 entries)\n");
    decode_queue_fail("MTF queue full");
    return nullptr;
  }
  uint8_t queue_data_index = queue_data_free_list_[queue_size_++];
  queue_[static_cast<uint8_t>(--queue_offset_)] = queue_data_index;
  sym_data_ptr->bytes.type |= 8;
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  queue_data_ptr->string_index = sym_data_ptr->string_index;
  queue_data_ptr->string_length = sym_data_ptr->string_length;
  queue_data_ptr->four_bytes = sym_data_ptr->four_bytes;
  queue_data_ptr->starts = first_char;
  queue_data_ptr->code_length = code_length;
  return queue_data_ptr;
}


Decoder::QueueData* Decoder::add_symbol_to_queue_cap_encoded(SymData* sym_data_ptr, uint8_t code_length,
    uint8_t first_char) {
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  if (queue_size_ >= 0x100) {
    fprintf(stderr, "GLZA decode: unified MTF queue full (256 entries)\n");
    decode_queue_fail("MTF queue full");
    return nullptr;
  }
  uint8_t queue_data_index = queue_data_free_list_[queue_size_];
  queue_size_++;
  if ((sym_data_ptr->bytes.type & 1) != 0) {
    queue_az_[static_cast<uint8_t>(--queue_offset_az_)] = queue_data_index;
    decode_queue_subcount_inc(&queue_size_az_, "az");
  } else if (first_char == 0x20) {
    queue_space_[static_cast<uint8_t>(--queue_offset_space_)] = queue_data_index;
    decode_queue_subcount_inc(&queue_size_space_, "space");
  } else {
    queue_other_[static_cast<uint8_t>(--queue_offset_other_)] = queue_data_index;
    decode_queue_subcount_inc(&queue_size_other_, "other");
  }
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  sym_data_ptr->bytes.type |= 8;
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  queue_data_ptr->string_index = sym_data_ptr->string_index;
  queue_data_ptr->string_length = sym_data_ptr->string_length;
  queue_data_ptr->four_bytes = sym_data_ptr->four_bytes;
  queue_data_ptr->starts = first_char;
  queue_data_ptr->code_length = code_length;
  return queue_data_ptr;
}


Decoder::SymData* Decoder::update_queue(uint8_t queue_position) {
  uint8_t queue_data_index = queue_[static_cast<uint8_t>(queue_position + queue_offset_)];
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  if ((queue_data_ptr->bytes.remaining < kMaxInstancesForRemove) && (--queue_data_ptr->bytes.remaining == 0)) {
    queue_size_--;
    queue_data_free_list_[queue_size_] = queue_data_index;
    if (queue_position <= (queue_size_ >> 1)) {
      while (queue_position != 0) {
        queue_[static_cast<uint8_t>(queue_offset_ + queue_position)]
            = queue_[static_cast<uint8_t>(queue_offset_ + queue_position - 1)];
        queue_position--;
      }
      queue_offset_++;
    } else {
      while (queue_position != queue_size_) {
        queue_[static_cast<uint8_t>(queue_offset_ + queue_position)]
            = queue_[static_cast<uint8_t>(queue_offset_ + queue_position + 1)];
        queue_position++;
      }
    }
  } else {
    uint16_t context = 6 * queue_data_ptr->bytes.repeats + prior_is_cap_ + (queue_data_ptr->bytes.type & 1)
        + 3 * ((queue_data_ptr->bytes.type >> 4) == 2);
    if (model_.decode_go_mtf(context, 1) == 0) {
      queue_size_--;
      queue_data_free_list_[queue_size_] = queue_data_index;
      auto* dict_data_ptr = add_dictionary_symbol(queue_data_ptr->code_length, queue_data_ptr->starts);
      dict_data_ptr->string_index = queue_data_ptr->string_index;
      dict_data_ptr->string_length = queue_data_ptr->string_length;
      dict_data_ptr->four_bytes = queue_data_ptr->four_bytes;
      if (queue_position <= (queue_size_ >> 1)) {
        queue_position += queue_offset_;
        while (queue_position != queue_offset_) {
          queue_[queue_position] = queue_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_offset_++;
      } else {
        queue_position += queue_offset_;
        while (queue_position != static_cast<uint8_t>(queue_offset_ + queue_size_)) {
          queue_[queue_position] = queue_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
      }
    } else {
      if (queue_position <= (queue_size_ >> 1)) {
        queue_position += queue_offset_;
        while (queue_position != queue_offset_) {
          queue_[queue_position] = queue_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_[queue_offset_] = queue_data_index;
      } else {
        queue_position += queue_offset_;
        while (queue_position != static_cast<uint8_t>(queue_offset_ + queue_size_)) {
          queue_[queue_position] = queue_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
        queue_[--queue_offset_] = queue_data_index;
      }
    }
  }
  return reinterpret_cast<SymData*>(queue_data_ptr);
}


Decoder::SymData* Decoder::update_az_queue(uint8_t queue_position) {
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  if (queue_position >= queue_size_az_) {
    fprintf(stderr,
        "GLZA decode: az queue position %u out of range (queue_size_az=%u)\n",
        static_cast<unsigned>(queue_position), static_cast<unsigned>(queue_size_az_));
    decode_queue_fail("az MTF queue position out of range");
    return nullptr;
  }
  uint8_t queue_data_index = queue_az_[static_cast<uint8_t>(queue_position + queue_offset_az_)];
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  if ((queue_data_ptr->bytes.remaining < kMaxInstancesForRemove) && (--queue_data_ptr->bytes.remaining == 0)) {
    queue_size_--;
    decode_queue_subcount_dec(&queue_size_az_, "az");
    queue_data_free_list_[queue_size_] = queue_data_index;
    if (queue_position <= (queue_size_az_ >> 1)) {
      while (queue_position != 0) {
        queue_az_[static_cast<uint8_t>(queue_offset_az_ + queue_position)]
            = queue_az_[static_cast<uint8_t>(queue_offset_az_ + queue_position - 1)];
        queue_position--;
      }
      queue_offset_az_++;
    } else {
      while (queue_position != queue_size_az_) {
        queue_az_[static_cast<uint8_t>(queue_offset_az_ + queue_position)]
            = queue_az_[static_cast<uint8_t>(queue_offset_az_ + queue_position + 1)];
        queue_position++;
      }
    }
  } else {
    uint16_t context = 6 * queue_data_ptr->bytes.repeats + prior_is_cap_ + (queue_data_ptr->bytes.type & 1)
        + 3 * ((queue_data_ptr->bytes.type >> 4) == 2);
    if (model_.decode_go_mtf(context, 1) == 0) {
      queue_size_--;
      decode_queue_subcount_dec(&queue_size_az_, "az");
      queue_data_free_list_[queue_size_] = queue_data_index;
      auto* dict_data_ptr = add_dictionary_symbol(queue_data_ptr->code_length, queue_data_ptr->starts);
      dict_data_ptr->string_index = queue_data_ptr->string_index;
      dict_data_ptr->string_length = queue_data_ptr->string_length;
      dict_data_ptr->four_bytes = queue_data_ptr->four_bytes;
      if (queue_position <= (queue_size_az_ >> 1)) {
        queue_position += queue_offset_az_;
        while (queue_position != queue_offset_az_) {
          queue_az_[queue_position] = queue_az_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_offset_az_++;
      } else {
        queue_position += queue_offset_az_;
        while (queue_position != static_cast<uint8_t>(queue_offset_az_ + queue_size_az_)) {
          queue_az_[queue_position] = queue_az_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
      }
    } else {
      if (queue_position <= (queue_size_az_ >> 1)) {
        queue_position += queue_offset_az_;
        while (queue_position != queue_offset_az_) {
          queue_az_[queue_position] = queue_az_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_az_[queue_offset_az_] = queue_data_index;
      } else {
        queue_position += queue_offset_az_;
        while (queue_position != static_cast<uint8_t>(queue_offset_az_ + queue_size_az_)) {
          queue_az_[queue_position] = queue_az_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
        queue_az_[--queue_offset_az_] = queue_data_index;
      }
    }
  }
  return reinterpret_cast<SymData*>(queue_data_ptr);
}


Decoder::SymData* Decoder::update_space_queue(uint8_t queue_position) {
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  if (queue_position >= queue_size_space_) {
    fprintf(stderr,
        "GLZA decode: space queue position %u out of range (queue_size_space=%u)\n",
        static_cast<unsigned>(queue_position), static_cast<unsigned>(queue_size_space_));
    decode_queue_fail("space MTF queue position out of range");
    return nullptr;
  }
  uint8_t queue_data_index = queue_space_[static_cast<uint8_t>(queue_position + queue_offset_space_)];
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  if ((queue_data_ptr->bytes.remaining < kMaxInstancesForRemove) && (--queue_data_ptr->bytes.remaining == 0)) {
    queue_size_--;
    decode_queue_subcount_dec(&queue_size_space_, "space");
    queue_data_free_list_[queue_size_] = queue_data_index;
    if (queue_position <= (queue_size_space_ >> 1)) {
      while (queue_position != 0) {
        queue_space_[static_cast<uint8_t>(queue_offset_space_ + queue_position)]
            = queue_space_[static_cast<uint8_t>(queue_offset_space_ + queue_position - 1)];
        queue_position--;
      }
      queue_offset_space_++;
    } else {
      while (queue_position != queue_size_space_) {
        queue_space_[static_cast<uint8_t>(queue_offset_space_ + queue_position)]
            = queue_space_[static_cast<uint8_t>(queue_offset_space_ + queue_position + 1)];
        queue_position++;
      }
    }
  } else {
    uint16_t context = 6 * queue_data_ptr->bytes.repeats + prior_is_cap_ + (queue_data_ptr->bytes.type & 1)
        + 3 * ((queue_data_ptr->bytes.type >> 4) == 2);
    if (model_.decode_go_mtf(context, 1) == 0) {
      queue_size_--;
      decode_queue_subcount_dec(&queue_size_space_, "space");
      queue_data_free_list_[queue_size_] = queue_data_index;
      auto* dict_data_ptr = add_dictionary_symbol(queue_data_ptr->code_length, queue_data_ptr->starts);
      dict_data_ptr->string_index = queue_data_ptr->string_index;
      dict_data_ptr->string_length = queue_data_ptr->string_length;
      dict_data_ptr->four_bytes = queue_data_ptr->four_bytes;
      if (queue_position <= (queue_size_space_ >> 1)) {
        queue_position += queue_offset_space_;
        while (queue_position != queue_offset_space_) {
          queue_space_[queue_position] = queue_space_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_offset_space_++;
      } else {
        queue_position += queue_offset_space_;
        while (queue_position != static_cast<uint8_t>(queue_offset_space_ + queue_size_space_)) {
          queue_space_[queue_position] = queue_space_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
      }
    } else {
      if (queue_position <= (queue_size_space_ >> 1)) {
        queue_position += queue_offset_space_;
        while (queue_position != queue_offset_space_) {
          queue_space_[queue_position] = queue_space_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_space_[queue_offset_space_] = queue_data_index;
      } else {
        queue_position += queue_offset_space_;
        while (queue_position != static_cast<uint8_t>(queue_offset_space_ + queue_size_space_)) {
          queue_space_[queue_position] = queue_space_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
        queue_space_[--queue_offset_space_] = queue_data_index;
      }
    }
  }
  return reinterpret_cast<SymData*>(queue_data_ptr);
}


Decoder::SymData* Decoder::update_other_queue(uint8_t queue_position) {
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  if (queue_position >= queue_size_other_) {
    fprintf(stderr,
        "GLZA decode: other queue position %u out of range (queue_size_other=%u)\n",
        static_cast<unsigned>(queue_position), static_cast<unsigned>(queue_size_other_));
    decode_queue_fail("other MTF queue position out of range");
    return nullptr;
  }
  uint8_t queue_data_index = queue_other_[static_cast<uint8_t>(queue_position + queue_offset_other_)];
  auto* queue_data_ptr = &queue_data_[queue_data_index];
  if ((queue_data_ptr->bytes.remaining < kMaxInstancesForRemove) && (--queue_data_ptr->bytes.remaining == 0)) {
    queue_size_--;
    decode_queue_subcount_dec(&queue_size_other_, "other");
    queue_data_free_list_[queue_size_] = queue_data_index;
    if (queue_position <= (queue_size_other_ >> 1)) {
      while (queue_position != 0) {
        queue_other_[static_cast<uint8_t>(queue_offset_other_ + queue_position)]
            = queue_other_[static_cast<uint8_t>(queue_offset_other_ + queue_position - 1)];
        queue_position--;
      }
      queue_offset_other_++;
    } else {
      while (queue_position != queue_size_other_) {
        queue_other_[static_cast<uint8_t>(queue_offset_other_ + queue_position)]
            = queue_other_[static_cast<uint8_t>(queue_offset_other_ + queue_position + 1)];
        queue_position++;
      }
    }
  } else {
    uint16_t context = 6 * queue_data_ptr->bytes.repeats + prior_is_cap_ + (queue_data_ptr->bytes.type & 1)
        + 3 * ((queue_data_ptr->bytes.type >> 4) == 2);
    if (model_.decode_go_mtf(context, 1) == 0) {
      queue_size_--;
      decode_queue_subcount_dec(&queue_size_other_, "other");
      queue_data_free_list_[queue_size_] = queue_data_index;
      auto* dict_data_ptr = add_dictionary_symbol(queue_data_ptr->code_length, queue_data_ptr->starts);
      dict_data_ptr->string_index = queue_data_ptr->string_index;
      dict_data_ptr->string_length = queue_data_ptr->string_length;
      dict_data_ptr->four_bytes = queue_data_ptr->four_bytes;
      if (queue_position <= (queue_size_other_ >> 1)) {
        queue_position += queue_offset_other_;
        while (queue_position != queue_offset_other_) {
          queue_other_[queue_position] = queue_other_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_offset_other_++;
      } else {
        queue_position += queue_offset_other_;
        while (queue_position != static_cast<uint8_t>(queue_offset_other_ + queue_size_other_)) {
          queue_other_[queue_position] = queue_other_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
      }
    } else {
      if (queue_position <= (queue_size_other_ >> 1)) {
        queue_position += queue_offset_other_;
        while (queue_position != queue_offset_other_) {
          queue_other_[queue_position] = queue_other_[static_cast<uint8_t>(queue_position - 1)];
          queue_position--;
        }
        queue_other_[queue_offset_other_] = queue_data_index;
      } else {
        queue_position += queue_offset_other_;
        while (queue_position != static_cast<uint8_t>(queue_offset_other_ + queue_size_other_)) {
          queue_other_[queue_position] = queue_other_[static_cast<uint8_t>(queue_position + 1)];
          queue_position++;
        }
        queue_other_[--queue_offset_other_] = queue_data_index;
      }
    }
  }
  return reinterpret_cast<SymData*>(queue_data_ptr);
}


uint32_t Decoder::get_dictionary_index(uint16_t bin_num, uint8_t code_length, uint8_t first_char) {
  uint32_t temp_index;
  uint16_t bins_per_symbol, extra_bins, end_extra_index;
  auto& bin_info = bin_data_[first_char][code_length];
  uint32_t num_symbols = bin_info.nsob;
  uint16_t num_bins = bin_info.nbob;
  uint32_t index = bin_num - bin_info.fbob;
  if (code_length > bin_code_length_[first_char]) {
    uint32_t min_extra_reduce_index;
    int8_t index_bits = code_length - bin_code_length_[first_char];
    uint32_t shifted_max_symbols = num_bins << index_bits;
    while ((shifted_max_symbols >>= 1) >= num_symbols)
      index_bits--;
    if (index_bits > 0) {
      min_extra_reduce_index = (num_symbols - shifted_max_symbols) << 1;
      index <<= index_bits;
      uint32_t bin_code = model_.decode_bin_code(index_bits);
      index += bin_code;
      if (index >= min_extra_reduce_index) {
        index = (index + min_extra_reduce_index) >> 1;
        model_.double_range(static_cast<uint8_t>(bin_code & 1));
      }
      return index;
    }
  }
  uint8_t bin_shift = bin_code_length_[first_char] - code_length;
  if ((num_symbols << bin_shift) == num_bins) {
    model_.increase_range(index & ((1 << bin_shift) - 1), 1 << bin_shift);
    return index >> bin_shift;
  }
  if (num_bins <= 2 * num_symbols) {
    extra_bins = num_bins - num_symbols;
    if (index >= 2 * extra_bins) {
      index -= extra_bins;
      return index;
    }
    model_.double_range(index & 1);
    return index >> 1;
  }
  if (num_bins < 3 * num_symbols) {
    bins_per_symbol = 2;
    extra_bins = num_bins - num_symbols * 2;
    end_extra_index = extra_bins * 3;
    if (index >= end_extra_index) {
      temp_index = index - end_extra_index;
      index = (temp_index >> 1) + extra_bins;
      model_.increase_range(temp_index & 1, 2);
    } else {
      temp_index = index;
      index /= 3;
      model_.increase_range(temp_index - index * 3, 3);
    }
  } else {
    bins_per_symbol = num_bins / num_symbols;
    extra_bins = num_bins - num_symbols * bins_per_symbol;
    end_extra_index = extra_bins * (bins_per_symbol + 1);
    if (index >= end_extra_index) {
      temp_index = index - end_extra_index;
      index = temp_index / bins_per_symbol;
      model_.increase_range(temp_index - index * bins_per_symbol, bins_per_symbol);
      index += extra_bins;
    } else {
      temp_index = index;
      index /= ++bins_per_symbol;
      model_.increase_range(temp_index - index * bins_per_symbol, bins_per_symbol);
    }
  }
  return index;
}


void Decoder::delta_transform(uint8_t* buffer, uint32_t len) {
  uint8_t* char_ptr = buffer;
  if (out_buffers_sent_ == 0) {
    if (stride_ > 4) {
      char_ptr = buffer + 1;
      do {
        *char_ptr += *(char_ptr - 1);
      } while (++char_ptr < buffer + stride_);
    }
    char_ptr = buffer + stride_;
    len -= stride_;
  }
  if (stride_ == 1) {
    while (len-- != 0) {
      *char_ptr += *(char_ptr - 1);
      char_ptr++;
    }
  } else if (stride_ == 2) {
    while (len-- != 0) {
      if ((delta_format_ & 4) == 0) {
        *char_ptr += *(char_ptr - 2);
        char_ptr++;
      } else {
        char_ptr++;
        if (((char_ptr - buffer) & 1) == 0) {
          if ((delta_format_ & 8) == 0) {
            uint32_t value = (*(char_ptr - 4) << 8) + *(char_ptr - 3) + (*(char_ptr - 2) << 8) + *(char_ptr - 1) - 0x80;
            *(char_ptr - 2) = (value >> 8) & 0xFF;
            *(char_ptr - 1) = value & 0xFF;
          } else {
            uint32_t value = (*(char_ptr - 3) << 8) + *(char_ptr - 4) + (*(char_ptr - 1) << 8) + *(char_ptr - 2) - 0x80;
            *(char_ptr - 1) = (value >> 8) & 0xFF;
            *(char_ptr - 2) = value & 0xFF;
          }
        }
      }
    }
  } else if (stride_ == 3) {
    while (len-- != 0) {
      *char_ptr += *(char_ptr - 3);
      char_ptr++;
    }
  } else if (stride_ == 4) {
    while (len-- != 0) {
      char_ptr++;
      if ((delta_format_ & 4) == 0)
        *(char_ptr - 1) += *(char_ptr - 5);
      else if ((delta_format_ & 0x10) != 0) {
        if (((char_ptr - buffer) & 1) == 0) {
          if ((delta_format_ & 8) == 0) {
            uint32_t value = (*(char_ptr - 6) << 8) + *(char_ptr - 5) + (*(char_ptr - 2) << 8) + *(char_ptr - 1) - 0x80;
            *(char_ptr - 2) = (value >> 8) & 0xFF;
            *(char_ptr - 1) = value & 0xFF;
          } else {
            uint32_t value = (*(char_ptr - 5) << 8) + *(char_ptr - 6) + (*(char_ptr - 1) << 8) + *(char_ptr - 2) - 0x80;
            *(char_ptr - 1) = (value >> 8) & 0xFF;
            *(char_ptr - 2) = value & 0xFF;
          }
        }
      } else {
        if (((char_ptr - buffer) & 3) == 0) {
          if ((delta_format_ & 8) == 0) {
            uint32_t value = (*(char_ptr - 8) << 24) + (*(char_ptr - 7) << 16) + (*(char_ptr - 6) << 8) + *(char_ptr - 5)
                + (*(char_ptr - 4) << 24) + (*(char_ptr - 3) << 16) + (*(char_ptr - 2) << 8) + *(char_ptr - 1) - 0x808080;
            *(char_ptr - 4) = value >> 24;
            *(char_ptr - 3) = (value >> 16) & 0xFF;
            *(char_ptr - 2) = (value >> 8) & 0xFF;
            *(char_ptr - 1) = value & 0xFF;
          } else {
            uint32_t value = (*(char_ptr - 5) << 24) + (*(char_ptr - 6) << 16) + (*(char_ptr - 7) << 8) + *(char_ptr - 8)
                + (*(char_ptr - 1) << 24) + (*(char_ptr - 2) << 16) + (*(char_ptr - 3) << 8) + *(char_ptr - 4) - 0x808080;
            *(char_ptr - 1) = value >> 24;
            *(char_ptr - 2) = (value >> 16) & 0xFF;
            *(char_ptr - 3) = (value >> 8) & 0xFF;
            *(char_ptr - 4) = value & 0xFF;
          }
        }
      }
    }
  } else {
    while (len-- != 0) {
      *char_ptr += *(char_ptr - stride_);
      char_ptr++;
    }
  }
}


uint8_t Decoder::create_extended_UTF8_symbol(uint32_t base_symbol, uint32_t* string_index_ptr) {
  if (base_symbol < kStartUtf8_3Byte) {
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol >> 6) + 0xC0;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol & 0x3F) + 0x80;
    if (base_symbol < 0x250)
      return 0x80;
    else if (base_symbol < 0x370)
      return 0x81;
    else if (base_symbol < 0x400)
      return 0x82;
    else if (base_symbol < 0x530)
      return 0x83;
    else if (base_symbol < 0x590)
      return 0x84;
    else if (base_symbol < 0x600)
      return 0x85;
    else if (base_symbol < 0x700)
      return 0x86;
    else
      return 0x87;
  } else if (base_symbol < kStartUtf8_4Byte) {
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol >> 12) + 0xE0;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>((base_symbol >> 6) & 0x3F) + 0x80;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol & 0x3F) + 0x80;
    if (base_symbol < 0x1000)
      return 0x88;
    else if (base_symbol < 0x2000)
      return 0x89;
    else if (base_symbol < 0x3000)
      return 0x8A;
    else if (base_symbol < 0x3040)
      return 0x8B;
    else if (base_symbol < 0x30A0)
      return 0x8C;
    else if (base_symbol < 0x3100)
      return 0x8D;
    else if (base_symbol < 0x3200)
      return 0x8E;
    else if (base_symbol < 0xA000)
      return 0x8F;
    else
      return 0x8E;
  } else {
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol >> 18) + 0xF0;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>((base_symbol >> 12) & 0x3F) + 0x80;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>((base_symbol >> 6) & 0x3F) + 0x80;
    symbol_strings_[(*string_index_ptr)++] = static_cast<uint8_t>(base_symbol & 0x3F) + 0x80;
    return 0x90;
  }
}


uint8_t Decoder::get_first_char(uint32_t index) {
  uint32_t UTF8_chars = (symbol_strings_[index] << 8) + symbol_strings_[index + 1];
  if (UTF8_chars < 0xE000) {
    if (UTF8_chars < 0xC990)
      return 0x80;
    else if (UTF8_chars < 0xCDB0)
      return 0x81;
    else if (UTF8_chars < 0xD000)
      return 0x82;
    else if (UTF8_chars < 0xD4B0)
      return 0x83;
    else if (UTF8_chars < 0xD690)
      return 0x84;
    else if (UTF8_chars < 0xD800)
      return 0x85;
    else if (UTF8_chars < 0xDC00)
      return 0x86;
    else
      return 0x87;
  } else if (UTF8_chars < 0xE100)
    return 0x88;
  else if (UTF8_chars < 0xE200)
    return 0x89;
  else if (UTF8_chars < 0xE300)
    return 0x8A;
  else if (UTF8_chars < 0xE381)
    return 0x8B;
  else {
    UTF8_chars = (UTF8_chars << 8) + symbol_strings_[index + 2];
    if (UTF8_chars < 0xE382A0)
      return 0x8C;
    else if (UTF8_chars < 0xE38400)
      return 0x8D;
    else if (UTF8_chars < 0xE38800)
      return 0x8E;
    else if (UTF8_chars < 0xEA0000)
      return 0x8F;
    else if (UTF8_chars < 0xF00000)
      return 0x8E;
    else
      return 0x90;
  }
}


Decoder::SymData* Decoder::decode_new(uint32_t* string_index_ptr) {
  uint8_t SID_symbol, sym_type, first_char;
  uint8_t put_in_mtf = 0;
  uint32_t symbols_in_definition, end_string_index;
  SymData* sym_data_ptr;
  QueueData temp_sym_data;

  temp_sym_data.string_index = *string_index_ptr;
  end_string_index = *string_index_ptr;
  SID_symbol = model_.decode_sid(kNotCap);
  if (SID_symbol == 0) {
    temp_sym_data.bytes.repeats = model_.decode_inst(kNotCap, SID_symbol);
    if (temp_sym_data.bytes.repeats < kMaxInstancesForRemove - 1)
      temp_sym_data.code_length = queue_miss_code_length_[++temp_sym_data.bytes.repeats];
    else if (temp_sym_data.bytes.repeats >= kMaxInstancesForRemove) {
      temp_sym_data.code_length = max_regular_code_length_ + kMaxInstancesForRemove - temp_sym_data.bytes.repeats;
      temp_sym_data.bytes.repeats = temp_sym_data.code_length + kMaxInstancesForRemove - 1;
    } else {
      temp_sym_data.bytes.repeats = 0;
      temp_sym_data.code_length = max_code_length_ + 1;
    }
    uint32_t base_symbol = model_.decode_base_symbol(num_base_symbols_);
    if ((UTF8_compliant_ == 0) || (base_symbol < kStartUtf8_2Byte)) {
      if (symbol_lengths_[base_symbol] != 0) {
        model_.double_range(static_cast<uint8_t>(base_symbol) & 1);
        base_symbol ^= 1;
      } else if (symbol_lengths_[base_symbol ^ 1] != 0) {
        model_.double_range(static_cast<uint8_t>(base_symbol) & 1);
      }
    }
    temp_sym_data.bytes.type = 0;

    if (UTF8_compliant_ != 0) {
      if (base_symbol < kStartUtf8_2Byte) {
        symbol_strings_[end_string_index++] = prior_end_ = static_cast<uint8_t>(base_symbol);
        temp_sym_data.string_length = 1;
      } else {
        prior_end_ = create_extended_UTF8_symbol(base_symbol, &end_string_index);
        temp_sym_data.string_length = end_string_index - temp_sym_data.string_index;
      }
      if (symbol_lengths_[prior_end_] == 0) {
        symbol_lengths_[prior_end_] = temp_sym_data.code_length;
        model_.init_first_char(prior_end_, temp_sym_data.code_length);
        model_.init_prior_end(prior_end_, symbol_lengths_.data());
      }
    } else {
      symbol_strings_[end_string_index++] = prior_end_ = static_cast<uint8_t>(base_symbol);
      temp_sym_data.string_length = 1;
      symbol_lengths_[prior_end_] = temp_sym_data.code_length;
      model_.init_first_char_binary(prior_end_, temp_sym_data.code_length);
      model_.init_prior_end_binary(prior_end_, symbol_lengths_.data());
    }
    temp_sym_data.starts = temp_sym_data.bytes.ends = prior_end_;

    if (find_first_symbol_ != 0) {
      find_first_symbol_ = 0;
      sum_nbob_[prior_end_] = bin_data_[prior_end_][max_code_length_].nbob = 1;
    }
    if (temp_sym_data.bytes.repeats == 0) {
      sym_data_ptr = add_single_dictionary_symbol(temp_sym_data.starts);
      sym_data_ptr->bytes.type = 0;
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      *string_index_ptr = end_string_index;
      return sym_data_ptr;
    }

    temp_sym_data.bytes.remaining = temp_sym_data.bytes.repeats;
    if (temp_sym_data.bytes.repeats < kMaxInstancesForRemove) {
      uint16_t context = temp_sym_data.bytes.repeats;
      uint16_t context2 = 240 + temp_sym_data.code_length;
      if (use_mtf_ != 0) {
        if (temp_sym_data.bytes.repeats == 1) {
          if (model_.decode_go_mtf(context, 2) != 0) {
            temp_sym_data.bytes.type = 2;
            put_in_mtf = 1;
          }
        } else {
          if (model_.decode_erg(context, context2) != 0) {
            temp_sym_data.bytes.type = 2;
            if (model_.decode_go_mtf(context, 2) != 0)
              put_in_mtf = 1;
          }
        }
      }
    } else {
      uint16_t context = temp_sym_data.bytes.repeats;
      uint16_t context2 = 240;
      if ((temp_sym_data.code_length >= 11) && (use_mtf_ != 0) && (model_.decode_erg(context, context2) != 0)) {
        temp_sym_data.bytes.type = 2;
        if (model_.decode_go_mtf(context, 2) != 0)
          put_in_mtf = 1;
      }
    }
    if (put_in_mtf == 0) {
      sym_data_ptr = add_dictionary_symbol(temp_sym_data.code_length, prior_end_);
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      sym_data_ptr->four_bytes = temp_sym_data.four_bytes;
    } else
      sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue(
          reinterpret_cast<SymData*>(&temp_sym_data), temp_sym_data.code_length, temp_sym_data.starts));
  } else {
    symbols_in_definition = SID_symbol + 1;
    if (symbols_in_definition == 16)
      symbols_in_definition += model_.decode_extra_sid();
    do {
      if ((sym_type = model_.decode_sym_type_binary(1, prior_end_, queue_size_)) == 0) {
        if (UTF8_compliant_ != 0)
          first_char = model_.decode_first_char(0, prior_end_);
        else
          first_char = model_.decode_first_char_binary(prior_end_);
        uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
        uint8_t code_length;
        uint32_t index;
        if (!decode_dict_fetch(first_char, bin_num, &code_length, &index, &sym_data_ptr, "decode_new dict"))
          return nullptr;
        prior_end_ = sym_data_ptr->bytes.ends;
        if (!decode_append_sym(&end_string_index, sym_data_ptr, first_char, "decode_new dict copy"))
          return nullptr;
        if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
          if (--sym_data_ptr->bytes.remaining == 0)
            remove_dictionary_symbol(bin_data_[first_char][code_length], index);
          else if ((sym_data_ptr->bytes.type & 2) != 0) {
            if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
              sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue(sym_data_ptr, code_length, first_char));
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            } else {
              uint16_t context = 6 * sym_data_ptr->bytes.repeats;
              if (model_.decode_go_mtf(context, 0) != 0) {
                sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue(sym_data_ptr, code_length, first_char));
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              }
            }
          }
        } else if ((sym_data_ptr->bytes.type & 2) != 0) {
          uint16_t context = 6 * sym_data_ptr->bytes.remaining;
          if (model_.decode_go_mtf(context, 0) != 0) {
            sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue(sym_data_ptr, code_length, first_char));
            remove_dictionary_symbol(bin_data_[first_char][code_length], index);
          }
        }
      } else if (sym_type == 1) {
        sym_data_ptr = decode_new(&end_string_index);
        if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
          return nullptr;
      } else {
        sym_data_ptr = update_queue(model_.decode_mtf_pos(queue_size_));
        if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
          return nullptr;
        prior_end_ = sym_data_ptr->bytes.ends;
        if (!decode_append_sym(&end_string_index, sym_data_ptr, prior_end_, "decode_new mtf copy"))
          return nullptr;
      }
    } while (--symbols_in_definition != 0);

    if ((symbol_strings_[*string_index_ptr] < 0x80) || (UTF8_compliant_ == 0))
      temp_sym_data.starts = symbol_strings_[*string_index_ptr];
    else
      temp_sym_data.starts = get_first_char(temp_sym_data.string_index);
    temp_sym_data.bytes.ends = prior_end_;
    temp_sym_data.string_length = end_string_index - temp_sym_data.string_index;

    temp_sym_data.bytes.repeats = model_.decode_inst(kNotCap, SID_symbol);
    if (temp_sym_data.bytes.repeats <= kMaxInstancesForRemove - 2) {
      temp_sym_data.code_length = queue_miss_code_length_[++temp_sym_data.bytes.repeats];
      uint16_t context = temp_sym_data.bytes.repeats;
      uint16_t context2 = 240 + temp_sym_data.code_length;
      if (use_mtf_ != 0) {
        if (temp_sym_data.bytes.repeats == 1) {
          if (model_.decode_go_mtf(context, 2) != 0) {
            temp_sym_data.bytes.type = 2;
            put_in_mtf = 1;
          } else
            temp_sym_data.bytes.type = 0;
        } else {
          if (model_.decode_erg(context, context2) != 0) {
            temp_sym_data.bytes.type = 2;
            if (model_.decode_go_mtf(context, 2) != 0)
              put_in_mtf = 1;
          } else
            temp_sym_data.bytes.type = 0;
        }
      } else
        temp_sym_data.bytes.type = 0;
    } else {
      temp_sym_data.code_length = max_regular_code_length_ + kMaxInstancesForRemove - 1 - temp_sym_data.bytes.repeats;
      temp_sym_data.bytes.repeats = temp_sym_data.code_length + kMaxInstancesForRemove - 1;
      uint16_t context = temp_sym_data.bytes.repeats;
      uint16_t context2 = 240;
      if ((temp_sym_data.code_length >= 11) && (use_mtf_ != 0) && (model_.decode_erg(context, context2) != 0)) {
        temp_sym_data.bytes.type = 2;
        if (model_.decode_go_mtf(context, 2) != 0)
          put_in_mtf = 1;
      } else
        temp_sym_data.bytes.type = 0;
    }
    temp_sym_data.bytes.remaining = temp_sym_data.bytes.repeats;
    if (put_in_mtf == 0) {
      sym_data_ptr = add_dictionary_symbol(temp_sym_data.code_length, temp_sym_data.starts);
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      sym_data_ptr->four_bytes = temp_sym_data.four_bytes;
    } else
      sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue(
          reinterpret_cast<SymData*>(&temp_sym_data), temp_sym_data.code_length, temp_sym_data.starts));
  }
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  *string_index_ptr = end_string_index;
  return sym_data_ptr;
}


Decoder::SymData* Decoder::decode_new_cap_encoded(uint32_t* string_index_ptr) {
  uint8_t SID_symbol, sym_type, first_char, saved_prior_is_cap;
  uint8_t put_in_mtf = 0;
  uint32_t symbols_in_definition, end_string_index;
  SymData* sym_data_ptr;
  QueueData temp_sym_data;

  temp_sym_data.string_index = *string_index_ptr;
  end_string_index = *string_index_ptr;
  SID_symbol = model_.decode_sid(prior_is_cap_);
  if (SID_symbol == 0) {
    temp_sym_data.bytes.repeats = model_.decode_inst(prior_is_cap_, SID_symbol);
    if (temp_sym_data.bytes.repeats < kMaxInstancesForRemove - 1) {
      temp_sym_data.code_length = queue_miss_code_length_[++temp_sym_data.bytes.repeats];
    } else if (temp_sym_data.bytes.repeats >= kMaxInstancesForRemove) {
      temp_sym_data.code_length = max_regular_code_length_ + kMaxInstancesForRemove - temp_sym_data.bytes.repeats;
      temp_sym_data.bytes.repeats = temp_sym_data.code_length + kMaxInstancesForRemove - 1;
    } else {
      temp_sym_data.bytes.repeats = 0;
      temp_sym_data.code_length = max_code_length_ + 1;
    }
    uint32_t base_symbol = model_.decode_base_symbol_cap(num_base_symbols_);
    if (base_symbol > 0x42)
      base_symbol += 24;
    else if (base_symbol > 0x40)
      base_symbol += 1;
    temp_sym_data.bytes.type = 0;
    saved_prior_is_cap = prior_is_cap_;
    prior_is_cap_ = 0;

    if ((UTF8_compliant_ == 0) || (base_symbol < kStartUtf8_2Byte)) {
      if (symbol_lengths_[base_symbol] != 0) {
        model_.double_range(static_cast<uint8_t>(base_symbol) & 1);
        base_symbol ^= 1;
      } else if (symbol_lengths_[base_symbol ^ 1] != 0)
        model_.double_range(static_cast<uint8_t>(base_symbol) & 1);
      symbol_lengths_[base_symbol] = temp_sym_data.code_length;
      model_.init_base_symbol_cap(static_cast<uint8_t>(base_symbol), symbol_lengths_.data());
      symbol_strings_[end_string_index++] = temp_sym_data.starts = temp_sym_data.bytes.ends = static_cast<uint8_t>(base_symbol);
      temp_sym_data.string_length = 1;

      // The encoder seeds sd_[' '/'B'/'C'].type with bit 2 (space=4, B/C=0x24),
      // but for short inputs (max_code_length_ < 14) it then clears bits 2-4,7
      // via `type &= 0x63` (glza_encode.cpp). The decoder must mirror that
      // masking on the same symbols, or the sym_type ctx2 for the following
      // symbol diverges and the stream desyncs. For mcl >= 14 the encoder keeps
      // bit 2 (and refines the cap-level bits), so keep type = 4 there.
      const bool short_input = max_code_length_ < 14;
      if (base_symbol == 'C') {
        prior_is_cap_ = 1;
        temp_sym_data.bytes.type = short_input ? 0x20 : 4;
      } else if (base_symbol == 'B') {
        prior_is_cap_ = 1;
        temp_sym_data.bytes.ends = 'C';
        temp_sym_data.bytes.type = short_input ? 0x20 : 4;
      } else {
        if (base_symbol == ' ') {
          temp_sym_data.bytes.type = short_input ? 0 : 4;
        } else if ((base_symbol >= 0x61) && (base_symbol <= 0x7A))
          temp_sym_data.bytes.type = 1;
      }
      prior_end_ = temp_sym_data.bytes.ends;
    } else {
      base_symbol = create_extended_UTF8_symbol(base_symbol, &end_string_index);
      prior_end_ = temp_sym_data.starts = temp_sym_data.bytes.ends = static_cast<uint8_t>(base_symbol);
      if (symbol_lengths_[prior_end_] == 0) {
        symbol_lengths_[prior_end_] = temp_sym_data.code_length;
        model_.init_first_char(prior_end_, temp_sym_data.code_length);
        model_.init_prior_end(prior_end_, symbol_lengths_.data());
      }
      temp_sym_data.string_length = end_string_index - temp_sym_data.string_index;
    }

    if (find_first_symbol_ != 0) {
      find_first_symbol_ = 0;
      sum_nbob_[base_symbol] = bin_data_[base_symbol][max_code_length_].nbob = 1;
    }
    if (temp_sym_data.bytes.repeats == 0) {
      sym_data_ptr = add_single_dictionary_symbol(temp_sym_data.starts);
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      prior_type_ = sym_data_ptr->bytes.type = temp_sym_data.bytes.type;
      *string_index_ptr = end_string_index;
      return sym_data_ptr;
    }

    temp_sym_data.bytes.remaining = temp_sym_data.bytes.repeats;
    if (temp_sym_data.bytes.repeats < kMaxInstancesForRemove) {
      uint16_t context = 6 * temp_sym_data.bytes.repeats + saved_prior_is_cap + (temp_sym_data.bytes.type & 1);
      uint16_t context2 = 240 + (4 * temp_sym_data.code_length) + (((temp_sym_data.bytes.type >> 4) == 2) << 1)
          + (temp_sym_data.bytes.type & 1);
      if (use_mtf_ != 0) {
        if (temp_sym_data.bytes.repeats == 1) {
          if (model_.decode_go_mtf(context, 2) != 0) {
            temp_sym_data.bytes.type |= 2;
            put_in_mtf = 1;
          }
        } else {
          if (model_.decode_erg(context, context2) != 0) {
            temp_sym_data.bytes.type |= 2;
            if (model_.decode_go_mtf(context, 2) != 0)
              put_in_mtf = 1;
          }
        }
      }
    } else {
      uint16_t context = 6 * temp_sym_data.bytes.repeats + saved_prior_is_cap + (temp_sym_data.bytes.type & 1);
      uint16_t context2 = 240 + (temp_sym_data.bytes.type & 1);
      if ((temp_sym_data.code_length >= 11) && (use_mtf_ != 0) && (model_.decode_erg(context, context2) != 0)) {
        temp_sym_data.bytes.type |= 2;
        if (model_.decode_go_mtf(context, 2) != 0)
          put_in_mtf = 1;
      }
    }
    if (put_in_mtf == 0) {
      sym_data_ptr = add_dictionary_symbol(temp_sym_data.code_length, static_cast<uint8_t>(base_symbol));
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      sym_data_ptr->four_bytes = temp_sym_data.four_bytes;
    } else
      sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue_cap_encoded(
          reinterpret_cast<SymData*>(&temp_sym_data), temp_sym_data.code_length, temp_sym_data.starts));
  } else {
    symbols_in_definition = SID_symbol + 1;
    if (symbols_in_definition == 16)
      symbols_in_definition += model_.decode_extra_sid();
    saved_prior_is_cap = prior_is_cap_;
    do {
      if (prior_is_cap_ == 0) {
        uint8_t context = 5 + 8 * (prior_type_ >> 4) + 2 * (prior_type_ & 7);
        if ((sym_type = model_.decode_sym_type(1, context, prior_end_, queue_size_)) == 0) {
          if (prior_end_ != 0xA)
            first_char = model_.decode_first_char(prior_type_ >> 4, prior_end_);
          else
            first_char = 0x20;
          uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
          uint8_t code_length;
          uint32_t index;
          if (!decode_dict_fetch(first_char, bin_num, &code_length, &index, &sym_data_ptr, "decode_new_cap dict"))
            return nullptr;
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (!decode_append_sym(&end_string_index, sym_data_ptr, first_char, "decode_new_cap dict copy"))
            return nullptr;
          if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
            if (--sym_data_ptr->bytes.remaining == 0)
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            else if ((sym_data_ptr->bytes.type & 2) != 0) {
              if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
                sym_data_ptr = reinterpret_cast<SymData*>(
                    add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              } else {
                uint16_t ctx = 6 * sym_data_ptr->bytes.repeats + (sym_data_ptr->bytes.type & 1)
                    + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
                if (model_.decode_go_mtf(ctx, 0) != 0) {
                  sym_data_ptr = reinterpret_cast<SymData*>(
                      add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
                  remove_dictionary_symbol(bin_data_[first_char][code_length], index);
                }
              }
            }
          } else if ((sym_data_ptr->bytes.type & 2) != 0) {
            uint16_t ctx = 6 * sym_data_ptr->bytes.remaining + (sym_data_ptr->bytes.type & 1)
                + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
            if (model_.decode_go_mtf(ctx, 0) != 0) {
              sym_data_ptr = reinterpret_cast<SymData*>(
                  add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            }
          }
        } else if (sym_type == 1) {
          sym_data_ptr = decode_new_cap_encoded(&end_string_index);
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            return nullptr;
        } else {
          uint8_t mtf_first;
          if (prior_end_ != 0xA)
            mtf_first = model_.decode_mtf_first((prior_type_ & 0x30) == 0x20, queue_size_other_, queue_size_space_, queue_size_az_);
          else
            mtf_first = 1;
          if (mtf_first == 0)
            sym_data_ptr = update_other_queue(model_.decode_mtf_pos_other(queue_size_other_));
          else if (mtf_first == 1)
            sym_data_ptr = update_space_queue(model_.decode_mtf_pos_space(queue_size_space_));
          else
            sym_data_ptr = update_az_queue(model_.decode_mtf_pos_az(queue_size_az_));
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            return nullptr;
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (!decode_append_ref(&end_string_index, sym_data_ptr->string_index, sym_data_ptr->string_length,
              "decode_new_cap mtf copy"))
            return nullptr;
        }
      } else {
        uint8_t context = 0x30 + (prior_type_ & 3);
        if ((sym_type = model_.decode_sym_type(3, context, 'C', queue_size_az_)) == 0) {
          first_char = model_.decode_first_char(0, 'C');
          uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
          uint8_t code_length;
          uint32_t index;
          if (!decode_dict_fetch(first_char, bin_num, &code_length, &index, &sym_data_ptr, "decode_new_cap cap dict"))
            return nullptr;
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (!decode_append_sym(&end_string_index, sym_data_ptr, first_char, "decode_new_cap cap dict copy"))
            return nullptr;
          if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
            if (--sym_data_ptr->bytes.remaining == 0)
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            else if ((sym_data_ptr->bytes.type & 2) != 0) {
              if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
                sym_data_ptr = reinterpret_cast<SymData*>(
                    add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              } else {
                uint16_t ctx = 6 * sym_data_ptr->bytes.repeats + 2 + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
                if (model_.decode_go_mtf(ctx, 0) != 0) {
                  sym_data_ptr = reinterpret_cast<SymData*>(
                      add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
                  remove_dictionary_symbol(bin_data_[first_char][code_length], index);
                }
              }
            }
          } else if ((sym_data_ptr->bytes.type & 2) != 0) {
            uint16_t ctx = 6 * sym_data_ptr->bytes.remaining + 2 + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
            if (model_.decode_go_mtf(ctx, 0) != 0) {
              sym_data_ptr = reinterpret_cast<SymData*>(
                  add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char));
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            }
          }
        } else if (sym_type == 1) {
          sym_data_ptr = decode_new_cap_encoded(&end_string_index);
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            return nullptr;
        } else {
          sym_data_ptr = update_az_queue(model_.decode_mtf_pos_az(queue_size_az_));
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            return nullptr;
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (!decode_append_sym(&end_string_index, sym_data_ptr, prior_end_, "decode_new_cap cap mtf copy"))
            return nullptr;
        }
      }
    } while (--symbols_in_definition != 0);

    temp_sym_data.bytes.ends = prior_end_;
    temp_sym_data.string_length = end_string_index - temp_sym_data.string_index;
    if ((symbol_strings_[temp_sym_data.string_index] < 0x80) || (UTF8_compliant_ == 0)) {
      temp_sym_data.starts = symbol_strings_[temp_sym_data.string_index];
      temp_sym_data.bytes.type = (temp_sym_data.starts >= 'a') && (temp_sym_data.starts <= 'z');
    } else {
      temp_sym_data.starts = get_first_char(temp_sym_data.string_index);
      temp_sym_data.bytes.type = 0;
    }

    temp_sym_data.bytes.repeats = model_.decode_inst(saved_prior_is_cap, SID_symbol);
    if (temp_sym_data.bytes.repeats <= kMaxInstancesForRemove - 2) {
      temp_sym_data.code_length = queue_miss_code_length_[++temp_sym_data.bytes.repeats];
      if ((prior_type_ & 4) != 0) {
        temp_sym_data.bytes.type += (prior_type_ & 0x30) + 4;
      } else if (max_code_length_ >= 14) {
        uint8_t* symbol_string_ptr = &symbol_strings_[end_string_index - 2];
        do {
          if (*symbol_string_ptr == ' ') {
            temp_sym_data.bytes.type += 0x14;
            break;
          }
        } while (symbol_string_ptr-- != &symbol_strings_[temp_sym_data.string_index]);
      }
      uint16_t context = 6 * temp_sym_data.bytes.repeats + saved_prior_is_cap + (temp_sym_data.bytes.type & 1)
          + 3 * ((temp_sym_data.bytes.type >> 4) == 2);
      uint16_t context2 = 240 + (4 * temp_sym_data.code_length) + (((temp_sym_data.bytes.type >> 4) == 2) << 1)
          + (temp_sym_data.bytes.type & 1);
      if (use_mtf_ != 0) {
        if (temp_sym_data.bytes.repeats == 1) {
          if (model_.decode_go_mtf(context, 2) != 0) {
            temp_sym_data.bytes.type |= 2;
            put_in_mtf = 1;
          }
        } else if (model_.decode_erg(context, context2) != 0) {
          temp_sym_data.bytes.type |= 2;
          if (model_.decode_go_mtf(context, 2) != 0)
            put_in_mtf = 1;
        }
      }
    } else {
      temp_sym_data.code_length = max_regular_code_length_ + kMaxInstancesForRemove - 1 - temp_sym_data.bytes.repeats;
      temp_sym_data.bytes.repeats = temp_sym_data.code_length + kMaxInstancesForRemove - 1;
      if ((prior_type_ & 4) != 0) {
        if ((prior_type_ & 0x10) != 0)
          temp_sym_data.bytes.type += ((3 - model_.decode_word_tag(prior_end_)) << 4) + 4;
        else
          temp_sym_data.bytes.type += (prior_type_ & 0x30) + 4;
      } else if (max_code_length_ >= 14) {
        uint8_t* symbol_string_ptr = &symbol_strings_[end_string_index - 2];
        do {
          if (*symbol_string_ptr == ' ') {
            if (temp_sym_data.bytes.repeats >= kMaxInstancesForRemove)
              temp_sym_data.bytes.type += ((3 - model_.decode_word_tag(prior_end_)) << 4) + 4;
            else
              temp_sym_data.bytes.type += 0x14;
            break;
          }
        } while (symbol_string_ptr-- != &symbol_strings_[temp_sym_data.string_index]);
      }
      uint16_t context = 6 * temp_sym_data.bytes.repeats + saved_prior_is_cap + (temp_sym_data.bytes.type & 1)
          + 3 * ((temp_sym_data.bytes.type >> 4) == 2);
      uint16_t context2 = 240 + (((temp_sym_data.bytes.type >> 4) == 2) << 1) + (temp_sym_data.bytes.type & 1);
      if ((temp_sym_data.code_length >= 11) && (use_mtf_ != 0) && (model_.decode_erg(context, context2) != 0)) {
        temp_sym_data.bytes.type |= 2;
        if (model_.decode_go_mtf(context, 2) != 0)
          put_in_mtf = 1;
      }
    }
    temp_sym_data.bytes.remaining = temp_sym_data.bytes.repeats;
    if (put_in_mtf == 0) {
      sym_data_ptr = add_dictionary_symbol(temp_sym_data.code_length, temp_sym_data.starts);
      sym_data_ptr->string_index = temp_sym_data.string_index;
      sym_data_ptr->string_length = temp_sym_data.string_length;
      sym_data_ptr->four_bytes = temp_sym_data.four_bytes;
    } else
      sym_data_ptr = reinterpret_cast<SymData*>(add_symbol_to_queue_cap_encoded(
          reinterpret_cast<SymData*>(&temp_sym_data), temp_sym_data.code_length, temp_sym_data.starts));
  }
  prior_type_ = temp_sym_data.bytes.type;
  if (model_.read_decoder_failed() != 0)
    return nullptr;
  *string_index_ptr = end_string_index;
  return sym_data_ptr;
}


void Decoder::transpose2(uint8_t* buffer, uint32_t len) {
  uint8_t temp_buf[0x30000];
  uint32_t block1_len = len - (len >> 1);
  std::memcpy(temp_buf, buffer + block1_len, len - block1_len);
  uint8_t* char2_ptr = buffer + 2 * block1_len;
  uint8_t* char_ptr = buffer + block1_len;
  while (char_ptr != buffer) {
    char2_ptr -= 2;
    *char2_ptr = *--char_ptr;
  }
  char2_ptr = buffer + 1;
  char_ptr = temp_buf;
  while (char2_ptr < buffer + len) {
    *char2_ptr = *char_ptr++;
    char2_ptr += 2;
  }
}


void Decoder::transpose4(uint8_t* buffer, uint32_t len) {
  uint8_t temp_buf[0x30000];
  uint32_t block1_len = (len + 3) >> 2;
  std::memcpy(temp_buf, buffer + block1_len, len - block1_len);
  uint8_t* char2_ptr = buffer + 4 * block1_len;
  uint8_t* char_ptr = buffer + block1_len;
  while (char_ptr != buffer) {
    char2_ptr -= 4;
    *char2_ptr = *--char_ptr;
  }
  char2_ptr = buffer + 1;
  char_ptr = temp_buf;
  while (char2_ptr < buffer + len) {
    *char2_ptr = *char_ptr++;
    char2_ptr += 4;
  }
  char2_ptr = buffer + 2;
  while (char2_ptr < buffer + len) {
    *char2_ptr = *char_ptr++;
    char2_ptr += 4;
  }
  char2_ptr = buffer + 3;
  while (char2_ptr < buffer + len) {
    *char2_ptr = *char_ptr++;
    char2_ptr += 4;
  }
}


void Decoder::write_output_buffer() {
  uint32_t chars_to_write = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  if (fd_ != nullptr) {
    fflush(fd_);
    fwrite(start_char_ptr_, 1, chars_to_write, fd_);
    if ((out_buffers_sent_ & 1) == 0)
      out_char_ptr_ = out_char1_.data();
    else
      out_char_ptr_ = out_char0_.data();
  }
  outbuf_index_ += chars_to_write;
  start_char_ptr_ = out_char_ptr_;
  end_outbuf_ = out_char_ptr_ + kCharsToWrite;
#ifdef PRINTON
  if ((out_buffers_sent_ & 0x7F) == 0)
    fprintf(stderr, "%u\r", static_cast<unsigned>(outbuf_index_));
#endif
  out_buffers_sent_++;
}


void Decoder::write_output_buffer_delta() {
  uint32_t chars_to_write = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  uint32_t len = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  if (stride_ == 4) {
    transpose4(start_char_ptr_, len);
    len = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  } else if (stride_ == 2) {
    transpose2(start_char_ptr_, len);
    len = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  }
  delta_transform(start_char_ptr_, len);
  if (fd_ != nullptr) {
    fflush(fd_);
    fwrite(start_char_ptr_, 1, chars_to_write, fd_);
    if ((out_buffers_sent_ & 1) == 0) {
      for (uint8_t k = 1; k <= stride_; k++)
        out_char1_[100 - k] = *(out_char_ptr_ - k);
      out_char_ptr_ = out_char1_.data() + 100;
    } else {
      for (uint8_t k = 1; k <= stride_; k++)
        out_char0_[100 - k] = *(out_char_ptr_ - k);
      out_char_ptr_ = out_char0_.data() + 100;
    }
  }
  outbuf_index_ += chars_to_write;
  start_char_ptr_ = out_char_ptr_;
  end_outbuf_ = out_char_ptr_ + kCharsToWrite;
#ifdef PRINTON
  if ((out_buffers_sent_ & 0x7F) == 0)
    fprintf(stderr, "%u\r", static_cast<unsigned>(outbuf_index_));
#endif
  out_buffers_sent_++;
}


void Decoder::write_string(uint8_t*& ssp, uint32_t len) {
  while (out_char_ptr_ + len >= end_outbuf_) {
    uint32_t temp_len = static_cast<uint32_t>(end_outbuf_ - out_char_ptr_);
    len -= temp_len;
    std::memcpy(out_char_ptr_, ssp, temp_len);
    out_char_ptr_ += temp_len;
    ssp += temp_len;
    write_output_buffer();
  }
  std::memcpy(out_char_ptr_, ssp, len);
  out_char_ptr_ += len;
}


void Decoder::write_string_cap_encoded(uint8_t*& ssp, uint32_t len,
    uint8_t& wco, uint8_t& wclo, uint8_t& sso) {
  while (out_char_ptr_ + len >= end_outbuf_) {
    uint32_t temp_len = static_cast<uint32_t>(end_outbuf_ - out_char_ptr_);
    len -= temp_len;
    while (temp_len-- != 0) {
      if (wco == 0) {
        if (sso == 0) {
          if ((*ssp & 0xFE) == 0x42) {
            wco = 1;
            if (*ssp++ == 'B')
              wclo = 1;
          } else {
            *out_char_ptr_++ = *ssp;
            if (*ssp++ == 0xA)
              sso = 1;
          }
        } else {
          ssp++;
          sso = 0;
        }
      } else {
        if (wclo != 0) {
          if ((*ssp >= 'a') && (*ssp <= 'z'))
            *out_char_ptr_++ = *ssp++ - 0x20;
          else {
            wclo = 0;
            wco = 0;
            if (*ssp == 'C')
              ssp++;
            else {
              *out_char_ptr_++ = *ssp;
              if (*ssp++ == 0xA)
                sso = 1;
            }
          }
        } else {
          wco = 0;
          *out_char_ptr_++ = *ssp++ - 0x20;
        }
      }
    }
    write_output_buffer();
  }
  while (len-- != 0) {
    if (wco == 0) {
      if (sso == 0) {
        if ((*ssp & 0xFE) == 0x42) {
          wco = 1;
          if (*ssp++ == 'B')
            wclo = 1;
        } else {
          *out_char_ptr_++ = *ssp;
          if (*ssp++ == 0xA)
            sso = 1;
        }
      } else {
        ssp++;
        sso = 0;
      }
    } else {
      if (wclo != 0) {
        if ((*ssp >= 'a') && (*ssp <= 'z'))
          *out_char_ptr_++ = *ssp++ - 0x20;
        else {
          wclo = 0;
          wco = 0;
          if (*ssp == 'C') {
            ssp++;
          } else {
            *out_char_ptr_++ = *ssp;
            if (*ssp++ == 0xA)
              sso = 1;
          }
        }
      } else {
        wco = 0;
        *out_char_ptr_++ = *ssp++ - 0x20;
      }
    }
  }
}


void Decoder::write_string_delta(uint8_t*& ssp, uint32_t len) {
  while (out_char_ptr_ + len >= end_outbuf_) {
    uint32_t temp_len = static_cast<uint32_t>(end_outbuf_ - out_char_ptr_);
    len -= temp_len;
    std::memcpy(out_char_ptr_, ssp, temp_len);
    out_char_ptr_ += temp_len;
    ssp += temp_len;
    write_output_buffer_delta();
  }
  std::memcpy(out_char_ptr_, ssp, len);
  out_char_ptr_ += len;
}


void Decoder::write_single_threaded_output() {
  uint8_t* symbol_string_ptr;
  auto* read_ptr = reinterpret_cast<uint32_t*>(symbol_buffer_.data());
  if (cap_encoded_ != 0) {
    while (read_ptr != reinterpret_cast<uint32_t*>(symbol_buffer_write_ptr_)) {
      symbol_string_ptr = &symbol_strings_[*read_ptr++];
      uint32_t length = *read_ptr++;
      write_string_cap_encoded(symbol_string_ptr, length, write_cap_on_, write_cap_lock_on_, skip_space_on_);
    }
  } else if (stride_ == 0) {
    while (read_ptr != reinterpret_cast<uint32_t*>(symbol_buffer_write_ptr_)) {
      symbol_string_ptr = &symbol_strings_[*read_ptr++];
      uint32_t length = *read_ptr++;
      write_string(symbol_string_ptr, length);
    }
  } else {
    while (read_ptr != reinterpret_cast<uint32_t*>(symbol_buffer_write_ptr_)) {
      symbol_string_ptr = &symbol_strings_[*read_ptr++];
      uint32_t length = *read_ptr++;
      write_string_delta(symbol_string_ptr, length);
    }
  }
}


void Decoder::write_symbol_buffer(uint8_t* buffer_number_ptr) {
  if (two_threads_ == 0) {
    write_single_threaded_output();
    symbol_buffer_write_ptr_ = symbol_buffer_.data();
  } else {
    if (*buffer_number_ptr != 0)
      symbol_buffer_write_ptr_ = symbol_buffer_.data();
    symbol_buffer_end_write_ptr_ = symbol_buffer_write_ptr_ + 0x400;
    symbol_buffer_owner_[*buffer_number_ptr].store(1, std::memory_order_release);
    *buffer_number_ptr ^= 1;
    while (symbol_buffer_owner_[*buffer_number_ptr].load(std::memory_order_acquire) != 0)
      ;
  }
}


void Decoder::write_output_thread(uint8_t* outbuf_arg) {
  uint8_t wco = 0, wclo = 0, sso = 0, next_buffer = 0;
  uint8_t* symbol_string_ptr;

  if (fd_ != nullptr)
    out_char_ptr_ = out_char0_.data() + 100;
  else
    out_char_ptr_ = outbuf_arg;
  start_char_ptr_ = out_char_ptr_;
  end_outbuf_ = out_char_ptr_ + kCharsToWrite;
  auto* buffer_ptr = reinterpret_cast<uint32_t*>(symbol_buffer_.data());
  auto* buffer_end_ptr = buffer_ptr + 0x800;

  if (cap_encoded_ != 0) {
    while ((done_parsing_.load(std::memory_order_acquire) == 0)
        || (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0)) {
      if (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0) {
        do {
          symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
          uint32_t length = *buffer_ptr++;
          write_string_cap_encoded(symbol_string_ptr, length, wco, wclo, sso);
        } while (buffer_ptr != buffer_end_ptr);
        symbol_buffer_owner_[next_buffer].store(0, std::memory_order_release);
        next_buffer ^= 1;
        buffer_ptr = reinterpret_cast<uint32_t*>(symbol_buffer_.data())
            + ((buffer_ptr - reinterpret_cast<uint32_t*>(symbol_buffer_.data())) & 0xFFF);
        buffer_end_ptr = buffer_ptr + 0x800;
      }
    }
    while (*buffer_ptr != kMaxU32) {
      symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
      uint32_t length = *buffer_ptr++;
      write_string_cap_encoded(symbol_string_ptr, length, wco, wclo, sso);
    }
  } else if (stride_ == 0) {
    while ((done_parsing_.load(std::memory_order_acquire) == 0)
        || (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0)) {
      if (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0) {
        do {
          symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
          uint32_t length = *buffer_ptr++;
          write_string(symbol_string_ptr, length);
        } while (buffer_ptr != buffer_end_ptr);
        symbol_buffer_owner_[next_buffer].store(0, std::memory_order_release);
        next_buffer ^= 1;
        buffer_ptr = reinterpret_cast<uint32_t*>(symbol_buffer_.data())
            + ((buffer_ptr - reinterpret_cast<uint32_t*>(symbol_buffer_.data())) & 0xFFF);
        buffer_end_ptr = buffer_ptr + 0x800;
      }
    }
    while (*buffer_ptr != kMaxU32) {
      symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
      uint32_t length = *buffer_ptr++;
      write_string(symbol_string_ptr, length);
    }
  } else {
    while ((done_parsing_.load(std::memory_order_acquire) == 0)
        || (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0)) {
      if (symbol_buffer_owner_[next_buffer].load(std::memory_order_acquire) != 0) {
        do {
          symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
          uint32_t length = *buffer_ptr++;
          write_string_delta(symbol_string_ptr, length);
        } while (buffer_ptr != buffer_end_ptr);
        symbol_buffer_owner_[next_buffer].store(0, std::memory_order_release);
        next_buffer ^= 1;
        buffer_ptr = reinterpret_cast<uint32_t*>(symbol_buffer_.data())
            + ((buffer_ptr - reinterpret_cast<uint32_t*>(symbol_buffer_.data())) & 0xFFF);
        buffer_end_ptr = buffer_ptr + 0x800;
      }
    }
    while (*buffer_ptr != kMaxU32) {
      symbol_string_ptr = &symbol_strings_[*buffer_ptr++];
      uint32_t length = *buffer_ptr++;
      write_string_delta(symbol_string_ptr, length);
    }
  }
  uint32_t chars_to_write = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
  if (stride_ != 0) {
    if (stride_ == 4)
      transpose4(start_char_ptr_, chars_to_write);
    else if (stride_ == 2)
      transpose2(start_char_ptr_, chars_to_write);
    delta_transform(start_char_ptr_, chars_to_write);
  }
  if (fd_ != nullptr)
    fwrite(start_char_ptr_, 1, chars_to_write, fd_);
  outbuf_index_ += chars_to_write;
}


uint8_t* Decoder::decode(size_t in_size, uint8_t* inbuf, size_t* outsize_ptr,
    uint8_t* outbuf, FILE* fd_out, const Params& params) {
  uint8_t sym_type, next_write_buffer;
  uint32_t new_string_index;
  SymData* sym_data_ptr;
  std::jthread output_thread;

  model_.reset_codec_globals();

  fd_ = fd_out;
  stride_ = outbuf_index_ = out_buffers_sent_ = next_write_buffer = two_threads_ = 0;
  dictionary_size_ = static_cast<uint32_t>(std::pow(2.0, 10.0 + 0.08 * static_cast<double>(inbuf[0])));
  {
    uint32_t min_dict = static_cast<uint32_t>(in_size * 4);
    if (min_dict < 0x200000)
      min_dict = 0x200000;
    if (dictionary_size_ < min_dict)
      dictionary_size_ = min_dict;
  }
  cap_encoded_ = inbuf[1] >> 7;
  UTF8_compliant_ = (inbuf[1] >> 6) & 1;
  use_mtf_ = (inbuf[1] >> 5) & 1;
  max_code_length_ = (inbuf[1] & 0x1F) + 1;
  queue_miss_code_length_[1] = max_code_length_;
  min_code_length_ = (inbuf[2] & 0x1F) + 1;
  max_regular_code_length_ = max_code_length_ - (inbuf[3] & 0x1F);
  {
    uint8_t i = 2;
    do {
      queue_miss_code_length_[i] = queue_miss_code_length_[i - 1] - ((inbuf[2] >> (i + 4)) & 1);
    } while (++i != 4);
    do {
      queue_miss_code_length_[i] = queue_miss_code_length_[i - 1] - ((inbuf[3] >> (i + 1)) & 1);
    } while (++i != 7);
    do {
      queue_miss_code_length_[i] = queue_miss_code_length_[i - 1] - ((inbuf[4] >> (i - 7)) & 1);
    } while (++i != 15);
  }

  uint16_t first_char_limit;
  if (UTF8_compliant_ != 0) {
    if (in_size < 6) {
      *outsize_ptr = 0;
      return outbuf;
    }
    model_.write_in_char_num(6);
    num_base_symbols_ = 1 << inbuf[5];
    first_char_limit = 0x90;
  } else {
    num_base_symbols_ = 0x100;
    delta_format_ = (inbuf[2] & 0x20) >> 5;
    if (delta_format_ != 0) {
      if (in_size < 6) {
        *outsize_ptr = 0;
        return outbuf;
      }
      model_.write_in_char_num(6);
      delta_format_ = inbuf[5];
      if ((delta_format_ & 0x80) == 0)
        stride_ = (delta_format_ & 0x3) + 1;
      else
        stride_ = delta_format_ & 0x7F;
    } else {
      if (in_size < 5) {
        *outsize_ptr = 0;
        return outbuf;
      }
      model_.write_in_char_num(5);
    }
    first_char_limit = 0xFF;
  }

  symbol_strings_.resize(dictionary_size_);

  {
    uint16_t i = first_char_limit;
    do {
      for (int j = max_code_length_ + 1; j >= min_code_length_; j--) {
        bin_data_[i][j].nsob = 0;
        bin_data_[i][j].nbob = 0;
        bin_data_[i][j].fbob = 0;
        bin_data_[i][j].sym_list_size = 4;
        bin_data_[i][j].symbol_data.assign(4, SymData{});
      }
      sum_nbob_[i] = 0;
      symbol_lengths_[i] = 0;
      bin_code_length_[i] = max_code_length_;
    } while (i-- != 0);
  }

  std::memset(&lookup_bits_[0][0], max_code_length_, sizeof(lookup_bits_));

  symbol_buffer_write_ptr_ = symbol_buffer_.data();
  symbol_buffer_end_write_ptr_ = symbol_buffer_write_ptr_ + 0x400;
  find_first_symbol_ = 1;
  queue_offset_ = 0;
  queue_size_ = queue_size_az_ = queue_size_space_ = queue_size_other_ = prior_is_cap_ = 0;
  new_string_index = 0;

  {
    uint8_t init_base = (UTF8_compliant_ != 0) ? 0x90 : 0xFF;
    model_.init_decoder(init_base,
        kMaxInstancesForRemove + 1 + max_regular_code_length_ - min_code_length_,
        cap_encoded_, UTF8_compliant_, use_mtf_, inbuf);
  }

  if (params.two_threads != 0) {
    two_threads_ = 1;
    done_parsing_.store(0, std::memory_order_relaxed);
    symbol_buffer_owner_[0].store(0, std::memory_order_relaxed);
    symbol_buffer_owner_[1].store(0, std::memory_order_relaxed);
    output_thread = std::jthread([this, outbuf]() { write_output_thread(outbuf); });
  } else {
    write_cap_on_ = write_cap_lock_on_ = skip_space_on_ = 0;
    if (fd_ != nullptr)
      out_char_ptr_ = out_char0_.data() + 100;
    else
      out_char_ptr_ = outbuf;
    start_char_ptr_ = out_char_ptr_;
    end_outbuf_ = out_char_ptr_ + kCharsToWrite;
  }

  sym_data_ptr = bin_data_[0][min_code_length_].symbol_data.data();
  sym_data_ptr->bytes.type = 0;
  prior_end_ = 0;
  prior_type_ = 0;
  cap_symbol_defined_ = cap_lock_symbol_defined_ = 0;

  if (use_mtf_ != 0) {
    for (uint16_t i = 0; i < 0x100; i++)
      queue_data_free_list_[i] = static_cast<uint8_t>(i);
  }

  if (cap_encoded_ != 0) {
    sym_data_ptr = decode_new_cap_encoded(&new_string_index);
    if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
      goto decode_failed_early;
    std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
    while (1) {
      if (symbol_buffer_write_ptr_ == symbol_buffer_end_write_ptr_)
        write_symbol_buffer(&next_write_buffer);
      if (prior_is_cap_ == 0) {
        if ((sym_type = model_.decode_sym_type(0, 4 + 8 * (prior_type_ >> 4)
            + 2 * (prior_type_ & 7), prior_end_, queue_size_)) == 0) {
          uint8_t first_char = ' ';
          if (prior_end_ != 0xA)
            first_char = model_.decode_first_char(prior_type_ >> 4, prior_end_);
          uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
          uint8_t code_length = decode_lookup_load(first_char, bin_num, "main dict");
          if (bin_data_[first_char][code_length].nsob == 0)
            break;
          uint32_t index = get_dictionary_index(bin_num, code_length, first_char);
          if (!decode_dict_index_ok(first_char, code_length, index, "main dict"))
            goto decode_failed_early;
          sym_data_ptr = &bin_data_[first_char][code_length].symbol_data[index];
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
            if (--sym_data_ptr->bytes.remaining == 0)
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            else if ((sym_data_ptr->bytes.type & 2) != 0) {
              if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
                (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              } else {
                uint16_t context = 6 * sym_data_ptr->bytes.repeats + (sym_data_ptr->bytes.type & 1)
                    + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
                if (model_.decode_go_mtf(context, 0) != 0) {
                  (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
                  remove_dictionary_symbol(bin_data_[first_char][code_length], index);
                }
              }
            }
          } else if ((sym_data_ptr->bytes.type & 2) != 0) {
            uint16_t context = 6 * sym_data_ptr->bytes.remaining + (sym_data_ptr->bytes.type & 1)
                + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
            if (model_.decode_go_mtf(context, 0) != 0) {
              (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            }
          }
        } else if (sym_type == 1) {
          sym_data_ptr = decode_new_cap_encoded(&new_string_index);
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            goto decode_failed_early;
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
        } else {
          uint8_t mtf_first;
          if (prior_end_ != 0xA)
            mtf_first = model_.decode_mtf_first((prior_type_ & 0x30) == 0x20,
                queue_size_other_, queue_size_space_, queue_size_az_);
          else
            mtf_first = 1;
          if (mtf_first == 0)
            sym_data_ptr = update_other_queue(model_.decode_mtf_pos_other(queue_size_other_));
          else if (mtf_first == 1)
            sym_data_ptr = update_space_queue(model_.decode_mtf_pos_space(queue_size_space_));
          else
            sym_data_ptr = update_az_queue(model_.decode_mtf_pos_az(queue_size_az_));
          if (sym_data_ptr == nullptr || model_.read_decoder_failed() != 0)
            goto decode_failed_early;
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
        }
      } else {
        if ((sym_type = model_.decode_sym_type(2, 0x2C + (prior_type_ & 3), 'C', queue_size_az_)) == 0) {
          uint8_t first_char = model_.decode_first_char(0, 'C');
          uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
          uint8_t code_length = lookup_bits_[first_char][bin_num];
          uint32_t index = get_dictionary_index(bin_num, code_length, first_char);
          sym_data_ptr = &bin_data_[first_char][code_length].symbol_data[index];
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
            if (--sym_data_ptr->bytes.remaining == 0)
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            else if ((sym_data_ptr->bytes.type & 2) != 0) {
              if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
                (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              } else {
                uint16_t context = 6 * sym_data_ptr->bytes.repeats + 1 + (sym_data_ptr->bytes.type & 1)
                     + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
                if (model_.decode_go_mtf(context, 0) != 0) {
                  (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
                  remove_dictionary_symbol(bin_data_[first_char][code_length], index);
                }
              }
            }
          } else if ((sym_data_ptr->bytes.type & 2) != 0) {
            uint16_t context = 6 * sym_data_ptr->bytes.remaining + 1 + (sym_data_ptr->bytes.type & 1)
                + 3 * ((sym_data_ptr->bytes.type >> 4) == 2);
            if (model_.decode_go_mtf(context, 0) != 0) {
              (void)add_symbol_to_queue_cap_encoded(sym_data_ptr, code_length, first_char);
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            }
          }
        } else if (sym_type == 1) {
          sym_data_ptr = decode_new_cap_encoded(&new_string_index);
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
        } else {
          sym_data_ptr = update_az_queue(model_.decode_mtf_pos_az(queue_size_az_));
          prior_is_cap_ = ((prior_end_ = sym_data_ptr->bytes.ends) == 'C');
          prior_type_ = sym_data_ptr->bytes.type;
          std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
        }
      }
    }
  } else {
    sym_data_ptr = decode_new(&new_string_index);
    std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
    while (1) {
      if (symbol_buffer_write_ptr_ == symbol_buffer_end_write_ptr_)
        write_symbol_buffer(&next_write_buffer);
      if ((sym_type = model_.decode_sym_type_binary(0, prior_end_, queue_size_)) == 0) {
        uint8_t first_char;
        if (UTF8_compliant_ != 0)
          first_char = model_.decode_first_char(0, prior_end_);
        else
          first_char = model_.decode_first_char_binary(prior_end_);
        uint16_t bin_num = model_.decode_bin(sum_nbob_[first_char]);
        uint8_t code_length = lookup_bits_[first_char][bin_num];
        if (bin_data_[first_char][code_length].nsob == 0)
          break;
        uint32_t index = get_dictionary_index(bin_num, code_length, first_char);
        sym_data_ptr = &bin_data_[first_char][code_length].symbol_data[index];
        std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
        prior_end_ = sym_data_ptr->bytes.ends;
        if (sym_data_ptr->bytes.remaining < kMaxInstancesForRemove) {
          if (--sym_data_ptr->bytes.remaining == 0)
            remove_dictionary_symbol(bin_data_[first_char][code_length], index);
          else if ((sym_data_ptr->bytes.type & 2) != 0) {
            if ((sym_data_ptr->bytes.remaining == 1) && ((sym_data_ptr->bytes.type & 8) == 0)) {
              (void)add_symbol_to_queue(sym_data_ptr, code_length, first_char);
              remove_dictionary_symbol(bin_data_[first_char][code_length], index);
            } else {
              uint16_t context = 6 * sym_data_ptr->bytes.repeats;
              if (model_.decode_go_mtf(context, 0) != 0) {
                (void)add_symbol_to_queue(sym_data_ptr, code_length, first_char);
                remove_dictionary_symbol(bin_data_[first_char][code_length], index);
              }
            }
          }
        } else if ((sym_data_ptr->bytes.type & 2) != 0) {
          uint16_t context = 6 * sym_data_ptr->bytes.remaining;
          if (model_.decode_go_mtf(context, 0) != 0) {
            (void)add_symbol_to_queue(sym_data_ptr, code_length, first_char);
            remove_dictionary_symbol(bin_data_[first_char][code_length], index);
          }
        }
      } else if (sym_type == 1) {
        sym_data_ptr = decode_new(&new_string_index);
        std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
      } else {
        sym_data_ptr = update_queue(model_.decode_mtf_pos(queue_size_));
        prior_end_ = sym_data_ptr->bytes.ends;
        std::memcpy(symbol_buffer_write_ptr_++, &sym_data_ptr->string_index, 8);
      }
    }
  }

  *reinterpret_cast<uint32_t*>(symbol_buffer_write_ptr_) = kMaxU32;

decode_failed_early:
  *reinterpret_cast<uint32_t*>(symbol_buffer_write_ptr_) = kMaxU32;
  if (two_threads_ != 0)
    done_parsing_.store(1, std::memory_order_release);
  {
    uint16_t i = first_char_limit;
    do {
      for (int j = max_code_length_ + 1; j >= min_code_length_; j--)
        bin_data_[i][j].symbol_data.clear();
    } while (i-- != 0);
  }
  if (two_threads_ != 0) {
    if (output_thread.joinable())
      output_thread.join();
  } else {
    write_single_threaded_output();
    uint32_t chars_to_write = static_cast<uint32_t>(out_char_ptr_ - start_char_ptr_);
    if (stride_ != 0) {
      if (stride_ == 4)
        transpose4(start_char_ptr_, chars_to_write);
      else if (stride_ == 2)
        transpose2(start_char_ptr_, chars_to_write);
      delta_transform(start_char_ptr_, chars_to_write);
    }
    if (fd_ != nullptr)
      fwrite(start_char_ptr_, 1, chars_to_write, fd_);
    outbuf_index_ += chars_to_write;
  }
  symbol_strings_.clear();
  symbol_strings_.shrink_to_fit();
  if (model_.read_decoder_failed() != 0) {
    *outsize_ptr = 0;
    return nullptr;
  }
  *outsize_ptr = outbuf_index_;
  return outbuf;
}


}  // namespace glza
