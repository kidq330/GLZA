// Minimal roundtrip test mimicking TurboBench: GLZAcomp/GLZAdecode with params=NULL
// and repeated compress like turbobench becomp timing loop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

static int roundtrip_once(uint8_t *in, size_t insize, uint8_t *out, size_t outcap, uint8_t *dec,
    struct param_data *params, int compress_reps) {
  size_t outsize = 0;
  int rep;

  for (rep = 0; rep < compress_reps; rep++) {
    outsize = 0;
    if (!GLZAcomp(insize, in, &outsize, out, (FILE *)0, params))
      return -1;
    if (outsize == 0 || outsize > outcap)
      return -2;
  }

  size_t decsize = insize;
  if (GLZAdecode(outsize, out, &decsize, dec, (FILE *)0, params) == 0)
    return -3;

  if (decsize != insize) {
    fprintf(stderr, "size mismatch: decsize=%zu insize=%zu\n", decsize, insize);
    return -4;
  }

  for (size_t i = 0; i < insize; i++) {
    if (in[i] != dec[i]) {
      fprintf(stderr, "mismatch at %zu: in=%02x dec=%02x\n", i, in[i], dec[i]);
      return (int)(i + 1);
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "../enwik10m";
  int reps = argc > 2 ? atoi(argv[2]) : 3;
  struct param_data cli_params = {
    .max_rules = 5000,
    .cap_encoded = 0,
    .cap_lock_disabled = 0,
    .delta_disabled = 0,
    .create_words = 1,
    .fast_mode = 1,
    .user_set_RAM_size = 0,
    .user_set_profit_ratio_power = 0,
    .print_dictionary = 0,
    .use_mtf = 1,
    .two_threads = 1,
    .RAM_usage = 0,
    .order = 0,
    .profit_ratio_power = 0,
  };

  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t insize = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = (uint8_t*)malloc(insize);
  uint8_t *out = (uint8_t*)malloc(insize + 10 * 1024 * 1024);
  uint8_t *dec = (uint8_t*)malloc(insize);
  if (!in || !out || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, insize, f) != insize) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  int r0 = roundtrip_once(in, insize, out, insize + 10 * 1024 * 1024, dec, 0, reps);
  printf("params=NULL reps=%d -> %d (outsize after last compress ok if 0)\n", reps, r0);

  int r1 = roundtrip_once(in, insize, out, insize + 10 * 1024 * 1024, dec, &cli_params, reps);
  printf("params=CLI  reps=%d -> %d\n", reps, r1);

  free(in);
  free(out);
  free(dec);
  return (r0 != 0 || r1 != 0) ? 1 : 0;
}
