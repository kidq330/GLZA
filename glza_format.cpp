/***********************************************************************

Copyright 2014-2025 Kennon Conrad

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

#include "glza_format.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace glza {
namespace {

using Counts = std::array<uint32_t, 0x100>;
using Order1Counts = std::array<std::array<uint32_t, 0x100>, 0x100>;

void clear_counts(Counts& symbol_counts, Order1Counts& order_1_counts) {
  symbol_counts.fill(0);
  for (auto& row : order_1_counts) row.fill(0);
}

double calculate_order_1_entropy(const Counts& symbol_counts,
                                 const Order1Counts& order_1_counts) {
  uint16_t num_symbols = 0;
  double entropy = 0.0;
  for (uint16_t i = 0; i < 0x100; i++) {
    if (symbol_counts[i] != 0) {
      num_symbols++;
      entropy += static_cast<double>(symbol_counts[i]) *
                 std::log2(static_cast<double>(symbol_counts[i]));
      for (uint16_t j = 0; j < 0x100; j++) {
        if (order_1_counts[i][j]) {
          const double d_count = static_cast<double>(order_1_counts[i][j]);
          entropy -= d_count * std::log2(d_count);
        }
      }
    }
  }
  entropy += static_cast<double>(num_symbols) *
             (std::log2(static_cast<double>(num_symbols)) + 11.0);
  return entropy;
}

constexpr uint32_t kCharsToWrite = 0x40000;

void interleave_stride2(uint8_t* buf, size_t insize) {
  std::vector<uint8_t> tmp(kCharsToWrite);
  uint8_t* start_block = buf;
  uint8_t* end_char = buf + insize;
  uint8_t* end_block = start_block + kCharsToWrite;

  while (end_block < end_char) {
    uint8_t* tmp_ptr = tmp.data();
    uint8_t* in_ptr = start_block + 1;
    while (in_ptr < end_block) {
      *tmp_ptr++ = *in_ptr;
      in_ptr += 2;
    }
    uint8_t* out_ptr = start_block;
    in_ptr = start_block;
    while (in_ptr < end_block) {
      *out_ptr++ = *in_ptr;
      in_ptr += 2;
    }
    in_ptr = tmp.data();
    while (out_ptr < end_block)
      *out_ptr++ = *in_ptr++;
    start_block = end_block;
    end_block += kCharsToWrite;
  }

  uint8_t* tmp_ptr = tmp.data();
  uint8_t* in_ptr = start_block + 1;
  while (in_ptr < end_char) {
    *tmp_ptr++ = *in_ptr;
    in_ptr += 2;
  }
  uint8_t* out_ptr = start_block;
  in_ptr = start_block;
  while (in_ptr < end_char) {
    *out_ptr++ = *in_ptr;
    in_ptr += 2;
  }
  in_ptr = tmp.data();
  while (out_ptr < end_char)
    *out_ptr++ = *in_ptr++;
}

void interleave_stride4(uint8_t* buf, size_t insize) {
  std::vector<uint8_t> tmp(kCharsToWrite);
  uint8_t* start_block = buf;
  uint8_t* end_char = buf + insize;
  uint8_t* end_block = start_block + kCharsToWrite;

  while (end_block < end_char) {
    uint8_t* tmp_ptr = tmp.data();
    uint8_t* in_ptr = start_block + 1;
    while (in_ptr < end_block) {
      *tmp_ptr++ = *in_ptr;
      in_ptr += 4;
    }
    in_ptr = start_block + 2;
    while (in_ptr < end_block) {
      *tmp_ptr++ = *in_ptr;
      in_ptr += 4;
    }
    in_ptr = start_block + 3;
    while (in_ptr < end_block) {
      *tmp_ptr++ = *in_ptr;
      in_ptr += 4;
    }
    uint8_t* out_ptr = start_block;
    in_ptr = start_block;
    while (in_ptr < end_block) {
      *out_ptr++ = *in_ptr;
      in_ptr += 4;
    }
    in_ptr = tmp.data();
    while (out_ptr < end_block)
      *out_ptr++ = *in_ptr++;
    start_block = end_block;
    end_block += kCharsToWrite;
  }

  uint8_t* tmp_ptr = tmp.data();
  uint8_t* in_ptr = start_block + 1;
  while (in_ptr < end_char) {
    *tmp_ptr++ = *in_ptr;
    in_ptr += 4;
  }
  in_ptr = start_block + 2;
  while (in_ptr < end_char) {
    *tmp_ptr++ = *in_ptr;
    in_ptr += 4;
  }
  in_ptr = start_block + 3;
  while (in_ptr < end_char) {
    *tmp_ptr++ = *in_ptr;
    in_ptr += 4;
  }
  in_ptr = start_block;
  uint8_t* out_ptr = start_block;
  while (in_ptr < end_char) {
    *out_ptr++ = *in_ptr;
    in_ptr += 4;
  }
  in_ptr = tmp.data();
  while (out_ptr < end_char)
    *out_ptr++ = *in_ptr++;
}

}  // namespace

bool Formatter::format(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                       uint8_t** outbuf, const Params& params) {
  auto* inbuf2 = static_cast<uint8_t*>(std::malloc(insize));
  if (inbuf2 == nullptr)
    return false;
  std::memcpy(inbuf2, inbuf, insize);

  *outbuf = static_cast<uint8_t*>(std::malloc(2 * insize + 1));
  if (*outbuf == nullptr) {
    std::free(inbuf2);
    return false;
  }

  uint8_t* const end_char_ptr = inbuf2 + insize;

  const uint8_t cap_encoded = params.cap_encoded;
  const uint8_t cap_lock_disabled = params.cap_lock_disabled;
  const uint8_t delta_disabled = params.delta_disabled;

  uint32_t num_AZ = 0;
  uint32_t num_az_pre_AZ = 0;
  uint32_t num_az_post_AZ = 0;
  uint32_t num_spaces = 0;

  if (insize > 4) {
    uint8_t* in_char_ptr = inbuf2;
    uint8_t this_char = *in_char_ptr++;
    if (this_char == 0x20)
      num_spaces++;
    if ((this_char >= 'A') && (this_char <= 'Z')) {
      num_AZ++;
      if (in_char_ptr < end_char_ptr) {
        const uint8_t next_char = *in_char_ptr & 0xDF;
        if ((next_char >= 'A') && (next_char <= 'Z'))
          num_az_post_AZ++;
      }
    }

    while (in_char_ptr != end_char_ptr) {
      this_char = *in_char_ptr++;
      if (this_char == 0x20)
        num_spaces++;
      if ((this_char >= 'A') && (this_char <= 'Z')) {
        num_AZ++;
        const uint8_t prev_char = *(in_char_ptr - 2) & 0xDF;
        if (in_char_ptr < end_char_ptr) {
          const uint8_t next_char = *in_char_ptr & 0xDF;
          if ((next_char >= 'A') && (next_char <= 'Z'))
            num_az_post_AZ++;
        }
        if ((prev_char >= 'A') && (prev_char <= 'Z'))
          num_az_pre_AZ++;
      }
    }
  }

  uint8_t* out_char_ptr = *outbuf;

  if (((4 * num_az_post_AZ > num_AZ) && (num_az_post_AZ > num_az_pre_AZ) &&
       (num_spaces > 1 + (insize / 50)) && (cap_encoded != 2)) ||
      (cap_encoded == 1)) {
#ifdef PRINTON
    fprintf(stderr, "Converting textual data\n");
#endif
    *out_char_ptr++ = 1;
    uint8_t* in_char_ptr = inbuf2;
    while (in_char_ptr != end_char_ptr) {
      if ((*in_char_ptr >= 'A') && (*in_char_ptr <= 'Z')) {
        if ((in_char_ptr + 1 < end_char_ptr) &&
            ((*(in_char_ptr + 1) >= 'A') && (*(in_char_ptr + 1) <= 'Z') &&
             (cap_lock_disabled == 0)) &&
            ((in_char_ptr + 1 == end_char_ptr) ||
             (*(in_char_ptr + 2) < 'a') || (*(in_char_ptr + 2) > 'z'))) {
          *out_char_ptr++ = 'B';
          *out_char_ptr++ = *in_char_ptr++ + 0x20;
          *out_char_ptr++ = *in_char_ptr++ + 0x20;
          while ((in_char_ptr < end_char_ptr) && (*in_char_ptr >= 'A') &&
                 (*in_char_ptr <= 'Z'))
            *out_char_ptr++ = *in_char_ptr++ + 0x20;
          if ((in_char_ptr < end_char_ptr) && (*in_char_ptr >= 'a') &&
              (*in_char_ptr <= 'z'))
            *out_char_ptr++ = 'C';
        } else {
          *out_char_ptr++ = 'C';
          *out_char_ptr++ = *in_char_ptr++ + 0x20;
        }
      } else if (*in_char_ptr == 0xA) {
        in_char_ptr++;
        *out_char_ptr++ = 0xA;
        *out_char_ptr++ = ' ';
      } else {
        *out_char_ptr++ = *in_char_ptr++;
      }
    }
  } else if ((delta_disabled != 1) && (insize > 4)) {
    Counts symbol_counts;
    Order1Counts order_1_counts;

    clear_counts(symbol_counts, order_1_counts);
    for (uint32_t i = 0; i < insize - 1; i++) {
      symbol_counts[inbuf2[i]]++;
      order_1_counts[inbuf2[i]][inbuf2[i + 1]]++;
    }
    symbol_counts[inbuf2[insize - 1]]++;
    order_1_counts[inbuf2[insize - 1]][0x80]++;
    double order_1_entropy =
        calculate_order_1_entropy(symbol_counts, order_1_counts);
    double best_stride_entropy = order_1_entropy;
    uint8_t stride = 0;

    const uint32_t j = insize < 101 ? static_cast<uint32_t>(insize - 1) : 100;

    for (uint32_t k = 1; k <= j; k++) {
      clear_counts(symbol_counts, order_1_counts);
      if ((k == 2) | (k == 4)) {
        uint32_t i = 0;
        while (i < k) {
          symbol_counts[inbuf2[i]]++;
          order_1_counts[inbuf2[i]][0xFF & (inbuf2[i + k] - inbuf2[i])]++;
          i++;
        }
        while (i < static_cast<uint32_t>(insize) - k) {
          symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]++;
          order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]
                        [0xFF & (inbuf2[i + k] - inbuf2[i])]++;
          i++;
        }
        while (i < insize) {
          symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]++;
          order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - k])][0x80]++;
          i++;
        }
        order_1_entropy =
            calculate_order_1_entropy(symbol_counts, order_1_counts);
        if ((order_1_entropy < 0.95 * best_stride_entropy) ||
            ((stride != 0) && (order_1_entropy < best_stride_entropy))) {
          stride = k;
          best_stride_entropy = order_1_entropy;
        }
      } else {
        uint32_t i;
        for (i = 0; i < k - 1; i++) {
          symbol_counts[inbuf2[i]]++;
          order_1_counts[inbuf2[i]][inbuf2[i + 1]]++;
        }
        symbol_counts[inbuf2[k - 1]]++;
        order_1_counts[inbuf2[k - 1]][0xFF & (inbuf2[k] - inbuf2[0])]++;
        uint8_t failed_test = 0;
        i = k;
        if (insize > 100000) {
          uint32_t initial_test_size =
              100000 + ((static_cast<uint32_t>(insize) - 100000) >> 3);
          if (initial_test_size > insize)
            initial_test_size = static_cast<uint32_t>(insize) - 1;
          while (i < initial_test_size) {
            symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]++;
            order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]
                          [0xFF & (inbuf2[i + 1] - inbuf2[i + 1 - k])]++;
            i++;
          }
          order_1_entropy =
              calculate_order_1_entropy(symbol_counts, order_1_counts);
          if (order_1_entropy >= 1.05 * best_stride_entropy *
                                     static_cast<double>(initial_test_size) /
                                     static_cast<double>(insize))
            failed_test = 1;
        }
        if (failed_test == 0) {
          while (i < insize - 1) {
            symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]++;
            order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - k])]
                          [0xFF & (inbuf2[i + 1] - inbuf2[i + 1 - k])]++;
            i++;
          }
          symbol_counts[0xFF & (inbuf2[insize - 1] - inbuf2[insize - 1 - k])]++;
          order_1_counts[0xFF & (inbuf2[insize - 1] - inbuf2[insize - 1 - k])]
                        [0x80]++;
          order_1_entropy =
              calculate_order_1_entropy(symbol_counts, order_1_counts);
          if ((order_1_entropy < 0.9 * best_stride_entropy) ||
              ((stride != 0) && (order_1_entropy < best_stride_entropy))) {
            stride = k;
            best_stride_entropy = order_1_entropy;
          }
        }
      }
    }

    double min_entropy = best_stride_entropy;

#ifdef PRINTON
    if (stride != 0)
      fprintf(stderr, "Applying %u byte delta transformation\n",
              (unsigned int)stride);
    else
      fprintf(stderr, "Converting data\n");
#endif

    if (stride == 0) {
      *out_char_ptr++ = 0;
    } else if (stride == 1) {
      *out_char_ptr++ = 2;
      uint8_t* in_char_ptr = end_char_ptr - 1;
      while (--in_char_ptr >= inbuf2)
        *(in_char_ptr + 1) -= *in_char_ptr;
    } else if (stride == 2) {
      double saved_entropy[4];
      for (uint32_t ch = 0; ch < 2; ch++) {
        clear_counts(symbol_counts, order_1_counts);
        uint8_t prior_delta_symbol = inbuf2[ch];
        for (uint32_t i = ch; i < (insize & ~static_cast<size_t>(1)) - 2;
             i += 2) {
          const uint8_t delta_symbol = inbuf2[i + 2] - inbuf2[i];
          symbol_counts[prior_delta_symbol]++;
          order_1_counts[prior_delta_symbol][delta_symbol]++;
          prior_delta_symbol = delta_symbol;
        }
        symbol_counts[prior_delta_symbol]++;
        order_1_counts[prior_delta_symbol][0]++;
        saved_entropy[ch] =
            calculate_order_1_entropy(symbol_counts, order_1_counts);
      }

      clear_counts(symbol_counts, order_1_counts);
      if (saved_entropy[0] < saved_entropy[1]) {
        // big endian
        uint16_t prior_symbol =
            (static_cast<uint16_t>(inbuf2[0]) << 8) + inbuf2[1];
        uint16_t prior_delta_symbol = prior_symbol;
        uint32_t i;
        for (i = 0; i < insize - 3; i += 2) {
          const uint16_t symbol =
              (static_cast<uint16_t>(inbuf2[i + 2]) << 8) + inbuf2[i + 3];
          const uint16_t delta_symbol = symbol - prior_symbol + 0x8080;
          symbol_counts[prior_delta_symbol >> 8]++;
          order_1_counts[prior_delta_symbol >> 8][delta_symbol >> 8]++;
          symbol_counts[0xFF & prior_delta_symbol]++;
          order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
          prior_symbol = symbol;
          prior_delta_symbol = delta_symbol;
        }
        uint16_t delta_symbol;
        if (i == insize - 3) {
          delta_symbol = (static_cast<uint16_t>(inbuf2[i + 2]) << 8) -
                         prior_symbol + 0x8080;
          symbol_counts[delta_symbol >> 8]++;
          order_1_counts[delta_symbol >> 8][0]++;
        } else {
          delta_symbol = 0;
        }
        symbol_counts[prior_delta_symbol >> 8]++;
        order_1_counts[prior_delta_symbol >> 8][delta_symbol >> 8]++;
        symbol_counts[0xFF & prior_delta_symbol]++;
        order_1_counts[0xFF & prior_delta_symbol][0]++;
        order_1_entropy =
            calculate_order_1_entropy(symbol_counts, order_1_counts);
        if (order_1_entropy < best_stride_entropy) {
#ifdef PRINTON
          fprintf(stderr, "Big endian\n");
#endif
          *out_char_ptr++ = 0x14;
          uint8_t* in_char_ptr =
              inbuf2 + ((end_char_ptr - inbuf2 - 4) & ~static_cast<ptrdiff_t>(1));
          uint16_t value = (static_cast<uint16_t>(*(in_char_ptr + 2)) << 8) +
                           *(in_char_ptr + 3);
          while (in_char_ptr >= inbuf2) {
            const uint16_t prior_value =
                (static_cast<uint16_t>(*in_char_ptr) << 8) + *(in_char_ptr + 1);
            const uint16_t delta_value = value - prior_value + 0x80;
            *(in_char_ptr + 2) = delta_value >> 8;
            *(in_char_ptr + 3) = delta_value & 0xFF;
            value = prior_value;
            in_char_ptr -= 2;
          }
        } else {
#ifdef PRINTON
          fprintf(stderr, "No carry\n");
#endif
          *out_char_ptr++ = 4;
          uint8_t* in_char_ptr = end_char_ptr - 2;
          while (--in_char_ptr >= inbuf2)
            *(in_char_ptr + 2) -= *in_char_ptr;
        }
      } else {
        uint16_t prior_symbol =
            (static_cast<uint16_t>(inbuf2[1]) << 8) + inbuf2[0];
        uint16_t prior_delta_symbol = prior_symbol;
        uint32_t i;
        for (i = 0; i < insize - 3; i += 2) {
          const uint16_t symbol =
              (static_cast<uint16_t>(inbuf2[i + 3]) << 8) + inbuf2[i + 2];
          const uint16_t delta_symbol = symbol - prior_symbol + 0x8080;
          symbol_counts[0xFF & prior_delta_symbol]++;
          order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
          symbol_counts[prior_delta_symbol >> 8]++;
          order_1_counts[prior_delta_symbol >> 8][delta_symbol >> 8]++;
          prior_symbol = symbol;
          prior_delta_symbol = delta_symbol;
        }
        uint16_t delta_symbol;
        if (i == insize - 3) {
          delta_symbol = inbuf2[i + 2] - prior_symbol + 0x8080;
          symbol_counts[0xFF & delta_symbol]++;
          order_1_counts[0xFF & delta_symbol][0]++;
        } else {
          delta_symbol = 0;
        }
        symbol_counts[0xFF & prior_delta_symbol]++;
        order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
        symbol_counts[prior_delta_symbol >> 8]++;
        order_1_counts[prior_delta_symbol >> 8][0]++;
        order_1_entropy =
            calculate_order_1_entropy(symbol_counts, order_1_counts);
        if (order_1_entropy < best_stride_entropy) {
#ifdef PRINTON
          fprintf(stderr, "Little endian\n");
#endif
          *out_char_ptr++ = 0x34;
          uint8_t* in_char_ptr =
              inbuf2 + ((end_char_ptr - inbuf2 - 4) & ~static_cast<ptrdiff_t>(1));
          uint16_t value = (static_cast<uint16_t>(*(in_char_ptr + 3)) << 8) +
                           *(in_char_ptr + 2);
          while (in_char_ptr >= inbuf2) {
            const uint16_t prior_value =
                (static_cast<uint16_t>(*(in_char_ptr + 1)) << 8) + *in_char_ptr;
            const uint16_t delta_value = value - prior_value + 0x80;
            *(in_char_ptr + 2) = delta_value & 0xFF;
            *(in_char_ptr + 3) = (delta_value >> 8);
            value = prior_value;
            in_char_ptr -= 2;
          }
        } else {
#ifdef PRINTON
          fprintf(stderr, "No carry\n");
#endif
          *out_char_ptr++ = 4;
          uint8_t* in_char_ptr = end_char_ptr - 2;
          while (--in_char_ptr >= inbuf2)
            *(in_char_ptr + 2) -= *in_char_ptr;
        }
      }
    } else if (stride == 4) {
      double saved_entropy[4];
      for (uint32_t ch = 0; ch < 4; ch++) {
        clear_counts(symbol_counts, order_1_counts);
        symbol_counts[inbuf2[ch]]++;
        order_1_counts[inbuf2[ch]][0xFF & (inbuf2[ch + stride] - inbuf2[ch])]++;
        uint32_t i = ch + stride;
        while (i < insize - stride) {
          symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - stride])]++;
          order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - stride])]
                        [0xFF & (inbuf2[i + stride] - inbuf2[i])]++;
          i += stride;
        }
        symbol_counts[0xFF & (inbuf2[i] - inbuf2[i - stride])]++;
        order_1_counts[0xFF & (inbuf2[i] - inbuf2[i - stride])][0]++;
        saved_entropy[ch] =
            calculate_order_1_entropy(symbol_counts, order_1_counts);
      }

      double best_entropy[4];
      uint8_t best_entropy_position[4];
      for (uint32_t i = 0; i < 4; i++) {
        best_entropy[i] = saved_entropy[i];
        best_entropy_position[i] = i;
        for (int8_t jj = static_cast<int8_t>(i) - 1; jj >= 0; jj--) {
          if (saved_entropy[i] < best_entropy[jj]) {
            best_entropy[jj + 1] = best_entropy[jj];
            best_entropy_position[jj + 1] = best_entropy_position[jj];
            best_entropy[jj] = saved_entropy[i];
            best_entropy_position[jj] = i;
          }
        }
      }

      if (best_entropy[3] > 1.05 * best_entropy[0]) {
        if ((3.0 * best_entropy[1] <
             best_entropy[0] + best_entropy[2] + best_entropy[3]) &&
            (((best_entropy_position[0] - best_entropy_position[1]) & 3) ==
             2)) {
          clear_counts(symbol_counts, order_1_counts);
          if (best_entropy[0] + best_entropy[2] <
              best_entropy[1] + best_entropy[3]) {
            // big endian
            uint16_t prior_symbol1 =
                (static_cast<uint16_t>(inbuf2[0]) << 8) + inbuf2[1];
            uint16_t prior_symbol2 =
                (static_cast<uint16_t>(inbuf2[2]) << 8) + inbuf2[3];
            uint16_t prior_delta_symbol1 = prior_symbol1;
            uint16_t prior_delta_symbol2 = prior_symbol2;
            uint32_t i;
            for (i = 0; i < insize - 7; i += 4) {
              const uint16_t symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) + inbuf2[i + 5];
              const uint16_t symbol2 =
                  (static_cast<uint16_t>(inbuf2[i + 6]) << 8) + inbuf2[i + 7];
              const uint16_t delta_symbol1 =
                  symbol1 - prior_symbol1 + 0x8080;
              const uint16_t delta_symbol2 =
                  symbol2 - prior_symbol2 + 0x8080;
              symbol_counts[prior_delta_symbol1 >> 8]++;
              order_1_counts[prior_delta_symbol1 >> 8][delta_symbol1 >> 8]++;
              symbol_counts[0xFF & prior_delta_symbol1]++;
              order_1_counts[0xFF & prior_delta_symbol1]
                            [0xFF & delta_symbol1]++;
              symbol_counts[prior_delta_symbol2 >> 8]++;
              order_1_counts[prior_delta_symbol2 >> 8][delta_symbol2 >> 8]++;
              symbol_counts[0xFF & prior_delta_symbol2]++;
              order_1_counts[0xFF & prior_delta_symbol2]
                            [0xFF & delta_symbol2]++;
              prior_symbol1 = symbol1;
              prior_symbol2 = symbol2;
              prior_delta_symbol1 = delta_symbol1;
              prior_delta_symbol2 = delta_symbol2;
            }
            uint16_t delta_symbol1, delta_symbol2;
            if (i == insize - 7) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) + inbuf2[i + 5] -
                  prior_symbol1 + 0x8080;
              delta_symbol2 =
                  (static_cast<uint16_t>(inbuf2[i + 6]) << 8) - prior_symbol2 +
                  0x8080;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
              symbol_counts[0xFF & delta_symbol1]++;
              order_1_counts[0xFF & delta_symbol1][0x80]++;
              symbol_counts[delta_symbol2 >> 8]++;
              order_1_counts[delta_symbol2 >> 8][0x80]++;
            } else if (i == insize - 6) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) + inbuf2[i + 5] -
                  prior_symbol1 + 0x8080;
              delta_symbol2 = 0x8080;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
              symbol_counts[0xFF & delta_symbol1]++;
              order_1_counts[0xFF & delta_symbol1][0x80]++;
            } else if (i == insize - 5) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) - prior_symbol1 +
                  0x8080;
              delta_symbol2 = 0x8080;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
            } else {
              delta_symbol1 = 0x8080;
              delta_symbol2 = 0x8080;
            }
            symbol_counts[prior_delta_symbol1 >> 8]++;
            order_1_counts[prior_delta_symbol1 >> 8][delta_symbol1 >> 8]++;
            symbol_counts[0xFF & prior_delta_symbol1]++;
            order_1_counts[0xFF & prior_delta_symbol1]
                          [0xFF & delta_symbol1]++;
            symbol_counts[prior_delta_symbol2 >> 8]++;
            order_1_counts[prior_delta_symbol2 >> 8][delta_symbol2 >> 8]++;
            symbol_counts[0xFF & prior_delta_symbol2]++;
            order_1_counts[0xFF & prior_delta_symbol2][0x80]++;
            order_1_entropy =
                calculate_order_1_entropy(symbol_counts, order_1_counts);
            if (order_1_entropy < min_entropy) {
#ifdef PRINTON
              fprintf(stderr, "Two channel big endian\n");
#endif
              *out_char_ptr++ = 0x58;
              uint8_t* in_char_ptr =
                  inbuf2 +
                  ((end_char_ptr - inbuf2 - 6) & ~static_cast<ptrdiff_t>(1));
              while (in_char_ptr >= inbuf2) {
                const uint16_t delta_value =
                    (static_cast<uint16_t>(*(in_char_ptr + 4)) << 8) +
                    *(in_char_ptr + 5) -
                    ((static_cast<uint16_t>(*in_char_ptr) << 8) +
                     *(in_char_ptr + 1)) +
                    0x80;
                *(in_char_ptr + 4) = delta_value >> 8;
                *(in_char_ptr + 5) = delta_value & 0xFF;
                in_char_ptr -= 2;
              }
            } else {
#ifdef PRINTON
              fprintf(stderr, "No carry\n");
#endif
              *out_char_ptr++ = 8;
              uint8_t* in_char_ptr = end_char_ptr - 4;
              while (--in_char_ptr >= inbuf2)
                *(in_char_ptr + 4) -= *in_char_ptr;
            }
          } else {
            // little endian
            uint16_t prior_symbol1 =
                (static_cast<uint16_t>(inbuf2[1]) << 8) + inbuf2[0];
            uint16_t prior_symbol2 =
                (static_cast<uint16_t>(inbuf2[3]) << 8) + inbuf2[2];
            uint16_t prior_delta_symbol1 = prior_symbol1;
            uint16_t prior_delta_symbol2 = prior_symbol2;
            uint32_t i;
            for (i = 0; i < insize - 7; i += 4) {
              const uint16_t symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 5]) << 8) + inbuf2[i + 4];
              const uint16_t symbol2 =
                  (static_cast<uint16_t>(inbuf2[i + 7]) << 8) + inbuf2[i + 6];
              const uint16_t delta_symbol1 =
                  symbol1 - prior_symbol1 + 0x8080;
              const uint16_t delta_symbol2 =
                  symbol2 - prior_symbol2 + 0x8080;
              symbol_counts[0xFF & prior_delta_symbol1]++;
              order_1_counts[0xFF & prior_delta_symbol1]
                            [0xFF & delta_symbol1]++;
              symbol_counts[prior_delta_symbol1 >> 8]++;
              order_1_counts[prior_delta_symbol1 >> 8][delta_symbol1 >> 8]++;
              symbol_counts[0xFF & prior_delta_symbol2]++;
              order_1_counts[0xFF & prior_delta_symbol2]
                            [0xFF & delta_symbol2]++;
              symbol_counts[prior_delta_symbol2 >> 8]++;
              order_1_counts[prior_delta_symbol2 >> 8][delta_symbol2 >> 8]++;
              prior_symbol1 = symbol1;
              prior_symbol2 = symbol2;
              prior_delta_symbol1 = delta_symbol1;
              prior_delta_symbol2 = delta_symbol2;
            }
            uint16_t delta_symbol1, delta_symbol2;
            if (i == insize - 7) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 5]) << 8) + inbuf2[i + 4] -
                  prior_symbol1 + 0x8080;
              delta_symbol2 = inbuf2[i + 6] - prior_symbol2 + 0x8080;
              symbol_counts[0xFF & delta_symbol1]++;
              order_1_counts[0xFF & delta_symbol1][0x80]++;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
              symbol_counts[0xFF & delta_symbol2]++;
              order_1_counts[0xFF & delta_symbol2][0x80]++;
            } else if (i == insize - 6) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) + inbuf2[i + 5] -
                  prior_symbol1 + 0x8080;
              delta_symbol2 = 0x8080;
              symbol_counts[0xFF & delta_symbol1]++;
              order_1_counts[0xFF & delta_symbol1][0x80]++;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
            } else if (i == insize - 5) {
              delta_symbol1 =
                  (static_cast<uint16_t>(inbuf2[i + 4]) << 8) - prior_symbol1 +
                  0x8080;
              delta_symbol2 = 0x8080;
              symbol_counts[delta_symbol1 >> 8]++;
              order_1_counts[delta_symbol1 >> 8][0x80]++;
            } else {
              delta_symbol1 = 0x8080;
              delta_symbol2 = 0x8080;
            }
            symbol_counts[0xFF & prior_delta_symbol1]++;
            order_1_counts[0xFF & prior_delta_symbol1]
                          [0xFF & delta_symbol1]++;
            symbol_counts[prior_delta_symbol1 >> 8]++;
            order_1_counts[prior_delta_symbol1 >> 8][delta_symbol1 >> 8]++;
            symbol_counts[0xFF & prior_delta_symbol2]++;
            order_1_counts[0xFF & prior_delta_symbol2]
                          [0xFF & delta_symbol1]++;
            symbol_counts[prior_delta_symbol2 >> 8]++;
            order_1_counts[prior_delta_symbol2 >> 8][0x80]++;
            order_1_entropy =
                calculate_order_1_entropy(symbol_counts, order_1_counts);
            if (order_1_entropy < min_entropy) {
#ifdef PRINTON
              fprintf(stderr, "Two channel little endian\n");
#endif
              *out_char_ptr++ = 0x78;
              uint8_t* in_char_ptr =
                  inbuf2 +
                  ((end_char_ptr - inbuf2 - 6) & ~static_cast<ptrdiff_t>(1));
              while (in_char_ptr >= inbuf2) {
                const uint16_t delta_value =
                    (static_cast<uint16_t>(*(in_char_ptr + 5)) << 8) +
                    *(in_char_ptr + 4) -
                    ((static_cast<uint16_t>(*(in_char_ptr + 1)) << 8) +
                     *in_char_ptr) +
                    0x80;
                *(in_char_ptr + 4) = delta_value & 0xFF;
                *(in_char_ptr + 5) = (delta_value >> 8) & 0xFF;
                in_char_ptr -= 2;
              }
            } else {
#ifdef PRINTON
              fprintf(stderr, "No carry\n");
#endif
              *out_char_ptr++ = 8;
              uint8_t* in_char_ptr = end_char_ptr - 4;
              while (--in_char_ptr >= inbuf2)
                *(in_char_ptr + 4) -= *in_char_ptr;
            }
          }
        } else {
          // try big endian first
          clear_counts(symbol_counts, order_1_counts);
          uint32_t prior_symbol =
              (static_cast<uint32_t>(inbuf2[0]) << 24) +
              (static_cast<uint32_t>(inbuf2[1]) << 16) +
              (static_cast<uint32_t>(inbuf2[2]) << 8) + inbuf2[3];
          uint32_t prior_delta_symbol = prior_symbol;
          uint32_t i;
          for (i = 0; i < insize - 7; i += 4) {
            const uint32_t symbol =
                (static_cast<uint32_t>(inbuf2[i + 4]) << 24) +
                (static_cast<uint32_t>(inbuf2[i + 5]) << 16) +
                (static_cast<uint32_t>(inbuf2[i + 6]) << 8) + inbuf2[i + 7];
            const uint32_t delta_symbol = symbol - prior_symbol + 0x80808080;
            symbol_counts[prior_delta_symbol >> 24]++;
            order_1_counts[prior_delta_symbol >> 24][delta_symbol >> 24]++;
            symbol_counts[0xFF & (prior_delta_symbol >> 16)]++;
            order_1_counts[0xFF & (prior_delta_symbol >> 16)]
                          [0xFF & (delta_symbol >> 16)]++;
            symbol_counts[0xFF & (prior_delta_symbol >> 8)]++;
            order_1_counts[0xFF & (prior_delta_symbol >> 8)]
                          [0xFF & (delta_symbol >> 8)]++;
            symbol_counts[0xFF & prior_delta_symbol]++;
            order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
            prior_symbol = symbol;
            prior_delta_symbol = delta_symbol;
          }
          uint32_t delta_symbol;
          if (i == insize - 7) {
            delta_symbol =
                (static_cast<uint32_t>(inbuf2[i + 4]) << 24) +
                (static_cast<uint32_t>(inbuf2[i + 5]) << 16) +
                (static_cast<uint32_t>(inbuf2[i + 6]) << 8) - prior_symbol +
                0x80808080;
            symbol_counts[delta_symbol >> 24]++;
            order_1_counts[delta_symbol >> 24][0x80]++;
            symbol_counts[0xFF & (delta_symbol >> 16)]++;
            order_1_counts[0xFF & (delta_symbol >> 16)][0x80]++;
            symbol_counts[0xFF & (delta_symbol >> 8)]++;
            order_1_counts[0xFF & (delta_symbol >> 8)][0x80]++;
          } else if (i == insize - 6) {
            delta_symbol =
                (static_cast<uint32_t>(inbuf2[i + 4]) << 24) +
                (static_cast<uint32_t>(inbuf2[i + 5]) << 16) - prior_symbol +
                0x80808080;
            symbol_counts[delta_symbol >> 24]++;
            order_1_counts[delta_symbol >> 24][0x80]++;
            symbol_counts[0xFF & (delta_symbol >> 16)]++;
            order_1_counts[0xFF & (delta_symbol >> 16)][0x80]++;
          } else if (i == insize - 5) {
            delta_symbol = (static_cast<uint32_t>(inbuf2[i + 4]) << 24) -
                           prior_symbol + 0x80808080;
            symbol_counts[delta_symbol >> 24]++;
            order_1_counts[delta_symbol >> 24][0x80]++;
          } else {
            delta_symbol = 0x80808080;
          }
          symbol_counts[prior_delta_symbol >> 24]++;
          order_1_counts[prior_delta_symbol >> 24][delta_symbol >> 24]++;
          symbol_counts[0xFF & (prior_delta_symbol >> 16)]++;
          order_1_counts[0xFF & (prior_delta_symbol >> 16)]
                        [0xFF & (delta_symbol >> 16)]++;
          symbol_counts[0xFF & (prior_delta_symbol >> 8)]++;
          order_1_counts[0xFF & (prior_delta_symbol >> 8)]
                        [0xFF & (delta_symbol >> 8)]++;
          symbol_counts[0xFF & prior_delta_symbol]++;
          order_1_counts[0xFF & prior_delta_symbol][0x80]++;
          saved_entropy[0] =
              calculate_order_1_entropy(symbol_counts, order_1_counts);

          clear_counts(symbol_counts, order_1_counts);
          prior_symbol = (static_cast<uint32_t>(inbuf2[3]) << 24) +
                         (static_cast<uint32_t>(inbuf2[2]) << 16) +
                         (static_cast<uint32_t>(inbuf2[1]) << 8) + inbuf2[0];
          prior_delta_symbol = prior_symbol;
          for (i = 0; i < insize - 7; i += 4) {
            const uint32_t symbol =
                (static_cast<uint32_t>(inbuf2[i + 7]) << 24) +
                (static_cast<uint32_t>(inbuf2[i + 6]) << 16) +
                (static_cast<uint32_t>(inbuf2[i + 5]) << 8) + inbuf2[i + 4];
            const uint32_t delta_symbol = symbol - prior_symbol + 0x80808080;
            symbol_counts[0xFF & prior_delta_symbol]++;
            order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
            symbol_counts[0xFF & (prior_delta_symbol >> 8)]++;
            order_1_counts[0xFF & (prior_delta_symbol >> 8)]
                          [0xFF & (delta_symbol >> 8)]++;
            symbol_counts[0xFF & (prior_delta_symbol >> 16)]++;
            order_1_counts[0xFF & (prior_delta_symbol >> 16)]
                          [0xFF & (delta_symbol >> 16)]++;
            symbol_counts[prior_delta_symbol >> 24]++;
            order_1_counts[prior_delta_symbol >> 24][delta_symbol >> 24]++;
            prior_symbol = symbol;
            prior_delta_symbol = delta_symbol;
          }
          if (i == insize - 7) {
            delta_symbol =
                (static_cast<uint32_t>(inbuf2[i + 6]) << 16) +
                (static_cast<uint32_t>(inbuf2[i + 5]) << 8) + inbuf2[i + 4] -
                prior_symbol + 0x80808080;
            symbol_counts[0xFF & delta_symbol]++;
            order_1_counts[0xFF & delta_symbol][0]++;
            symbol_counts[0xFF & (delta_symbol >> 8)]++;
            order_1_counts[0xFF & (delta_symbol >> 8)][0]++;
            symbol_counts[0xFF & (delta_symbol >> 16)]++;
            order_1_counts[0xFF & (delta_symbol >> 16)][0]++;
          } else if (i == insize - 6) {
            delta_symbol =
                (static_cast<uint32_t>(inbuf2[i + 5]) << 8) + inbuf2[i + 4] -
                prior_symbol + 0x80808080;
            symbol_counts[0xFF & delta_symbol]++;
            order_1_counts[0xFF & delta_symbol][0]++;
            symbol_counts[0xFF & (delta_symbol >> 8)]++;
            order_1_counts[0xFF & (delta_symbol >> 8)][0]++;
          } else if (i == insize - 5) {
            delta_symbol = static_cast<uint32_t>(inbuf2[i + 4]) -
                           prior_symbol + 0x80808080;
            symbol_counts[0xFF & delta_symbol]++;
            order_1_counts[0xFF & delta_symbol][0]++;
          } else {
            delta_symbol = 0x80808080;
          }
          symbol_counts[0xFF & prior_delta_symbol]++;
          order_1_counts[0xFF & prior_delta_symbol][0xFF & delta_symbol]++;
          symbol_counts[0xFF & (prior_delta_symbol >> 8)]++;
          order_1_counts[0xFF & (prior_delta_symbol >> 8)]
                        [0xFF & (delta_symbol >> 8)]++;
          symbol_counts[0xFF & (prior_delta_symbol >> 16)]++;
          order_1_counts[0xFF & (prior_delta_symbol >> 16)]
                        [0xFF & (delta_symbol >> 16)]++;
          symbol_counts[prior_delta_symbol >> 24]++;
          order_1_counts[prior_delta_symbol >> 24][0]++;
          order_1_entropy =
              calculate_order_1_entropy(symbol_counts, order_1_counts);

          if ((saved_entropy[0] < min_entropy) &&
              (saved_entropy[0] < order_1_entropy)) {
#ifdef PRINTON
            fprintf(stderr, "Big endian\n");
#endif
            *out_char_ptr++ = 0x18;
            uint8_t* in_char_ptr =
                inbuf2 +
                ((end_char_ptr - inbuf2 - 8) & ~static_cast<ptrdiff_t>(3));
            uint32_t value =
                (static_cast<uint32_t>(*(in_char_ptr + 4)) << 24) +
                (static_cast<uint32_t>(*(in_char_ptr + 5)) << 16) +
                (static_cast<uint32_t>(*(in_char_ptr + 6)) << 8) +
                *(in_char_ptr + 7);
            while (in_char_ptr >= inbuf2) {
              const uint32_t prior_value =
                  (static_cast<uint32_t>(*in_char_ptr) << 24) +
                  (static_cast<uint32_t>(*(in_char_ptr + 1)) << 16) +
                  (static_cast<uint32_t>(*(in_char_ptr + 2)) << 8) +
                  *(in_char_ptr + 3);
              const uint32_t delta_value = value - prior_value + 0x808080;
              *(in_char_ptr + 4) = delta_value >> 24;
              *(in_char_ptr + 5) = (delta_value >> 16) & 0xFF;
              *(in_char_ptr + 6) = (delta_value >> 8) & 0xFF;
              *(in_char_ptr + 7) = delta_value & 0xFF;
              value = prior_value;
              in_char_ptr -= 4;
            }
          } else if (order_1_entropy < min_entropy) {
#ifdef PRINTON
            fprintf(stderr, "Little endian\n");
#endif
            *out_char_ptr++ = 0x38;
            uint8_t* in_char_ptr =
                inbuf2 +
                ((end_char_ptr - inbuf2 - 8) & ~static_cast<ptrdiff_t>(3));
            uint32_t value =
                (static_cast<uint32_t>(*(in_char_ptr + 7)) << 24) +
                (static_cast<uint32_t>(*(in_char_ptr + 6)) << 16) +
                (static_cast<uint32_t>(*(in_char_ptr + 5)) << 8) +
                *(in_char_ptr + 4);
            while (in_char_ptr >= inbuf2) {
              const uint32_t prior_value =
                  (static_cast<uint32_t>(*(in_char_ptr + 3)) << 24) +
                  (static_cast<uint32_t>(*(in_char_ptr + 2)) << 16) +
                  (static_cast<uint32_t>(*(in_char_ptr + 1)) << 8) +
                  *in_char_ptr;
              const uint32_t delta_value = value - prior_value + 0x808080;
              *(in_char_ptr + 7) = delta_value >> 24;
              *(in_char_ptr + 6) = (delta_value >> 16) & 0xFF;
              *(in_char_ptr + 5) = (delta_value >> 8) & 0xFF;
              *(in_char_ptr + 4) = delta_value & 0xFF;
              value = prior_value;
              in_char_ptr -= 4;
            }
          } else {
#ifdef PRINTON
            fprintf(stderr, "No carry\n");
#endif
            *out_char_ptr++ = 8;
            uint8_t* in_char_ptr = end_char_ptr - 4;
            while (--in_char_ptr >= inbuf2)
              *(in_char_ptr + 4) -= *in_char_ptr;
          }
        }
      } else {
#ifdef PRINTON
        fprintf(stderr, "No carry\n");
#endif
        *out_char_ptr++ = 8;
        uint8_t* in_char_ptr = end_char_ptr - 4;
        while (--in_char_ptr >= inbuf2)
          *(in_char_ptr + 4) -= *in_char_ptr;
      }
    } else if (stride == 3) {
      *out_char_ptr++ = 6;
      uint8_t* in_char_ptr = end_char_ptr - 3;
      while (--in_char_ptr >= inbuf2)
        *(in_char_ptr + 3) -= *in_char_ptr;
    } else {
      *out_char_ptr++ = 0x80 + stride;
      uint8_t* in_char_ptr = end_char_ptr - stride;
      while (--in_char_ptr >= inbuf2)
        *(in_char_ptr + stride) -= *in_char_ptr;
      in_char_ptr = inbuf2 + stride - 1;
      while (--in_char_ptr >= inbuf2)
        *(in_char_ptr + 1) -= *in_char_ptr;
    }

    if ((stride == 2) || (stride == 4)) {
      if (stride == 2)
        interleave_stride2(inbuf2, insize);
      else
        interleave_stride4(inbuf2, insize);
    }
    std::memcpy(out_char_ptr, inbuf2, insize);
    out_char_ptr += insize;
  } else {
#ifdef PRINTON
    fprintf(stderr, "Converting data\n");
#endif
    *out_char_ptr++ = 0;
    std::memcpy(out_char_ptr, inbuf2, insize);
    out_char_ptr += insize;
  }

  *outsize_ptr = static_cast<size_t>(out_char_ptr - *outbuf);
  auto* resized = static_cast<uint8_t*>(std::realloc(*outbuf, *outsize_ptr));
  if (resized == nullptr) {
    fprintf(stderr,
            "ERROR - Compressed output buffer memory reallocation failed\n");
    std::free(*outbuf);
    *outbuf = nullptr;
    std::free(inbuf2);
    return false;
  }
  *outbuf = resized;
  std::free(inbuf2);
  return true;
}

}  // namespace glza
