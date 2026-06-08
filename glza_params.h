#pragma once

#include <cstdint>

namespace glza {

struct Params {
  uint32_t max_rules{5000};
  uint8_t cap_encoded{};
  uint8_t cap_lock_disabled{};
  uint8_t delta_disabled{};
  uint8_t create_words{1};
  uint8_t fast_mode{1};
  uint8_t user_set_RAM_size{};
  uint8_t user_set_profit_ratio_power{};
  uint8_t print_dictionary{};
  uint8_t use_mtf{2};
  uint8_t two_threads{1};
  double RAM_usage{};
  double order{};
  double profit_ratio_power{};
};

inline constexpr Params kEmbeddedDefaults{};

}  // namespace glza

struct param_data {
  uint32_t max_rules;
  uint8_t cap_encoded, cap_lock_disabled, delta_disabled, create_words,
      fast_mode, user_set_RAM_size;
  uint8_t user_set_profit_ratio_power, print_dictionary, use_mtf, two_threads;
  double RAM_usage, order, profit_ratio_power;
};

inline glza::Params to_params(const param_data* p) {
  if (p == nullptr) return glza::kEmbeddedDefaults;
  return {
      .max_rules = p->max_rules,
      .cap_encoded = p->cap_encoded,
      .cap_lock_disabled = p->cap_lock_disabled,
      .delta_disabled = p->delta_disabled,
      .create_words = p->create_words,
      .fast_mode = p->fast_mode,
      .user_set_RAM_size = p->user_set_RAM_size,
      .user_set_profit_ratio_power = p->user_set_profit_ratio_power,
      .print_dictionary = p->print_dictionary,
      .use_mtf = p->use_mtf,
      .two_threads = p->two_threads,
      .RAM_usage = p->RAM_usage,
      .order = p->order,
      .profit_ratio_power = p->profit_ratio_power,
  };
}
