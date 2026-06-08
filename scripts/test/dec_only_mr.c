/* Decode a compressed blob; max_rules must match the compressor that wrote it.
 * Usage: dec_only_mr <blob> [max_rules]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <blob> [max_rules]\n", argv[0]);
    return 1;
  }
  const char *path = argv[1];
  uint32_t max_rules = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 5000;
  struct param_data p = { max_rules, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };

  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t sz = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = (uint8_t*)malloc(sz);
  uint8_t *dec = (uint8_t*)malloc(20 * 1024 * 1024);
  if (!in || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, sz, f) != sz) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  size_t ds = 20 * 1024 * 1024;
  if (GLZAdecode(sz, in, &ds, dec, 0, &p) == 0) {
    fprintf(stderr, "decode failed\n");
    return 2;
  }
  printf("ok %zu (max_rules=%u)\n", ds, (unsigned)max_rules);
  return 0;
}
