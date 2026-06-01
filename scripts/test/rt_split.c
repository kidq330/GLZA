#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  uint32_t max_rules = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 7000;
  const char *path = argc > 2 ? argv[2] : "../enwik10m";
  struct param_data p = { max_rules, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };

  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  size_t n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 16*1024*1024), *dec = malloc(n);
  fread(in, 1, n, f); fclose(f);

  size_t sz = 0;
  int cr = GLZAcomp(n, in, &sz, out, 0, &p);
  fprintf(stderr, "compress ret=%d outsize=%zu\n", cr, sz);
  if (!cr) return 1;

  FILE *o = fopen("/tmp/glza_large.bin", "wb");
  fwrite(out, 1, sz, o);
  fclose(o);

  size_t ds = n;
  int dr = GLZAdecode(sz, out, &ds, dec, 0, &p) != 0;
  fprintf(stderr, "decode ret=%d decsize=%zu\n", dr, ds);
  return dr ? 0 : 2;
}
