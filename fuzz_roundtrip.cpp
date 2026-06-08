#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "glza_api.h"
#include "glza_params.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > 1024 * 1024) return 0;

  param_data params = {5000, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0};

  auto* inbuf = static_cast<uint8_t*>(malloc(size));
  if (inbuf == nullptr) return 0;
  memcpy(inbuf, data, size);

  const size_t out_capacity = size + 16 * 1024 * 1024;
  auto* outbuf = static_cast<uint8_t*>(malloc(out_capacity));
  if (outbuf == nullptr) {
    free(inbuf);
    return 0;
  }

  size_t outsize = 0;
  const uint8_t rc = GLZAcomp(size, inbuf, &outsize, outbuf, nullptr, &params);
  if (rc == 0) {
    free(outbuf);
    return 0;
  }

  auto* compressed = static_cast<uint8_t*>(malloc(outsize));
  if (compressed == nullptr) {
    free(outbuf);
    return 0;
  }
  memcpy(compressed, outbuf, outsize);
  free(outbuf);

  size_t decsize = size;
  auto* decbuf = static_cast<uint8_t*>(malloc(size));
  if (decbuf == nullptr) {
    free(compressed);
    return 0;
  }

  uint8_t* result =
      GLZAdecode(outsize, compressed, &decsize, decbuf, nullptr, &params);
  if (result == nullptr) {
    free(decbuf);
    return 0;
  }

  if (decsize != size || memcmp(data, result, size) != 0) {
    fprintf(stderr, "ROUNDTRIP MISMATCH: input=%zu compressed=%zu decoded=%zu\n",
            size, outsize, decsize);
    __builtin_trap();
  }

  free(result);
  return 0;
}
