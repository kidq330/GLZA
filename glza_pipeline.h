#pragma once

#include <cstdint>
#include <cstdio>

#include "glza_params.h"

namespace glza {

bool compress(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
              uint8_t* outbuf, FILE* fd, const Params& params);

uint8_t* decompress(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                     uint8_t* outbuf, FILE* fd_out, const Params& params);

}  // namespace glza
