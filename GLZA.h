#pragma once
// Compatibility header -- forwards to glza_params.h
#include "glza_params.h"

inline param_data* GLZA_params_or_default(param_data* p) {
  static param_data defaults = {
      5000, 0, 0, 0, 1, 1, 0, 0, 0, 2, 1, 0.0, 0.0, 0.0};
  return p ? p : &defaults;
}
