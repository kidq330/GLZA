#pragma once

#include <cstdint>
#include <cstdio>

struct param_data;

#ifdef __cplusplus
extern "C" {
#endif

uint8_t GLZAcomp(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                 uint8_t* outbuf, FILE* fd, struct param_data* params);

uint8_t* GLZAdecode(size_t insize, uint8_t* inbuf, size_t* outsize_ptr,
                    uint8_t* outbuf, FILE* fd_out, struct param_data* params);

const char* GLZA_last_fail_stage(void);
const char* GLZA_last_fail_detail(void);

#ifdef __cplusplus
}
#endif
