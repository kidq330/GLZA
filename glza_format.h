#pragma once

#include <cstdint>
#include <cstdio>

#include "glza_params.h"

namespace glza {

class Formatter {
 public:
  bool format(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
              uint8_t** outbuf, const Params& params);
};

}  // namespace glza
