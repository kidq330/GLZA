#include "glza_api.h"
#include "glza_error.h"
#include "glza_params.h"
#include "glza_pipeline.h"

extern "C" {

uint8_t GLZAcomp(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                 uint8_t* outbuf, FILE* fd, struct param_data* params) {
  const auto p = to_params(params);
  return glza::compress(insize, inbuf, outsize_ptr, outbuf, fd, p) ? 1 : 0;
}

uint8_t* GLZAdecode(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                    uint8_t* outbuf, FILE* fd_out, struct param_data* params) {
  const auto p = to_params(params);
  return glza::decompress(insize, inbuf, outsize_ptr, outbuf, fd_out, p);
}

const char* GLZA_last_fail_stage(void) {
  return glza::global_diagnostics().last_stage();
}

const char* GLZA_last_fail_detail(void) {
  return glza::global_diagnostics().last_detail();
}

}  // extern "C"
