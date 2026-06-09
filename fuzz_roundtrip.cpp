#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "glza_api.h"
#include "glza_params.h"

// Round-trip fuzzer: compress then decompress an arbitrary input and assert the
// output matches. Parameters are derived from the input so a single corpus
// exercises both fast and slow code paths, word/no-word passes, and a range of
// max_rules / threading / MTF settings.
//
// Invariants this harness must uphold so libFuzzer's leak detector stays clean:
//   * glza::compress() with fd==nullptr does NOT take ownership of inbuf
//     (it copies into its own temp buffer), so the harness frees inbuf itself.
//   * GLZAdecode() returns the caller-provided output buffer on success and
//     nullptr on failure; on success result==decbuf, so freeing result frees
//     decbuf exactly once.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > 1024 * 1024) return 0;

  // Derive parameters from the first byte. The whole input (including this
  // byte) is still round-tripped, so the test remains a faithful comparison.
  const uint8_t ctrl = data[0];
  uint8_t fast_mode = (ctrl & 0x01) ? 1 : 0;
  // Slow mode runs many passes; cap its input size so throughput stays usable.
  if (fast_mode == 0 && size > 128 * 1024) fast_mode = 1;

  const uint32_t max_rules_choices[] = {64, 1000, 5000, 60000};
  param_data params = {};
  params.max_rules = max_rules_choices[(ctrl >> 1) & 0x03];
  params.cap_encoded = (ctrl & 0x08) ? 1 : 0;
  params.cap_lock_disabled = (ctrl & 0x10) ? 1 : 0;
  params.delta_disabled = (ctrl & 0x20) ? 1 : 0;
  params.create_words = (ctrl & 0x40) ? 1 : 0;
  params.fast_mode = fast_mode;
  params.user_set_RAM_size = 0;
  params.user_set_profit_ratio_power = 0;
  params.print_dictionary = 0;
  params.use_mtf = static_cast<uint8_t>((ctrl >> 1) % 3);  // 0,1,2
  params.two_threads = (ctrl & 0x80) ? 1 : 0;
  params.RAM_usage = 0.0;
  params.order = 0.0;
  params.profit_ratio_power = 0.0;

  auto* inbuf = static_cast<uint8_t*>(malloc(size));
  if (inbuf == nullptr) return 0;
  memcpy(inbuf, data, size);

  const size_t out_capacity = size + (16 * 1024 * 1024);
  auto* outbuf = static_cast<uint8_t*>(malloc(out_capacity));
  if (outbuf == nullptr) {
    free(inbuf);
    return 0;
  }

  size_t outsize = 0;
  const uint8_t rc = GLZAcomp(size, inbuf, &outsize, outbuf, nullptr, &params);
  free(inbuf);
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
    free(compressed);
    return 0;
  }

  if (decsize != size || memcmp(data, result, size) != 0) {
    fprintf(stderr, "ROUNDTRIP MISMATCH: input=%zu compressed=%zu decoded=%zu\n",
            size, outsize, decsize);
    __builtin_trap();
  }

  free(result);  // result == decbuf on success
  free(compressed);
  return 0;
}
