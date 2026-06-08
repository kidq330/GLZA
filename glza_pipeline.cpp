#include "glza_pipeline.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "glza_compress.h"
#include "glza_decode.h"
#include "glza_encode.h"
#include "glza_error.h"
#include "glza_format.h"

namespace glza {

bool compress(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
              uint8_t* outbuf, FILE* fd, const Params& params) {
  global_diagnostics().clear();

  if (insize == 0) {
    *outsize_ptr = 0;
    return true;
  }

  Formatter formatter;
  uint8_t* formatted_buf = nullptr;

  if (fd == nullptr) {
    auto* tempbuf = static_cast<uint8_t*>(malloc(insize));
    if (tempbuf == nullptr) {
      global_diagnostics().set("format",
                               "input temp buffer alloc failed (insize=%zu)",
                               insize);
      return false;
    }
    memcpy(tempbuf, inbuf, insize);
    if (!formatter.format(insize, tempbuf, outsize_ptr, &formatted_buf,
                          params)) {
      free(tempbuf);
      if (!global_diagnostics().has_error())
        global_diagnostics().set("format", "GLZAformat failed (insize=%zu)",
                                 insize);
      return false;
    }
    free(tempbuf);
  } else {
    if (!formatter.format(insize, inbuf, outsize_ptr, &formatted_buf,
                          params)) {
      if (!global_diagnostics().has_error())
        global_diagnostics().set("format", "GLZAformat failed (insize=%zu)",
                                 insize);
      return false;
    }
    free(inbuf);
  }

  const size_t formatted_size = *outsize_ptr;
  Compressor compressor;
  if (!compressor.compress(formatted_size, outsize_ptr, &formatted_buf,
                           params)) {
    if (!global_diagnostics().has_error())
      global_diagnostics().set(
          "compress",
          "GLZAcompress returned failure (formatted_size=%zu)",
          formatted_size);
    return false;
  }

  const size_t grammar_size = *outsize_ptr;
  Encoder encoder;
  const bool ok = encoder.encode(grammar_size, formatted_buf, outsize_ptr,
                                 outbuf, fd, insize, params);
  free(formatted_buf);
  if (!ok && !global_diagnostics().has_error())
    global_diagnostics().set(
        "encode",
        "GLZAencode returned failure (grammar_bytes=%zu file_size=%zu)",
        grammar_size, insize);
  return ok;
}

uint8_t* decompress(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                      uint8_t* outbuf, FILE* fd_out, const Params& params) {
  global_diagnostics().clear();
  if (insize == 0) {
    *outsize_ptr = 0;
    return outbuf;
  }
  Decoder decoder;
  return decoder.decode(insize, inbuf, outsize_ptr, outbuf, fd_out, params);
}

}  // namespace glza
