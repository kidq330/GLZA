#include <stdio.h>
#include <stdlib.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(void) {
  struct param_data p = { 5000, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0.0, 0.0, 0.0 };
  FILE *f = fopen("../enwik10m", "rb");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 16*1024*1024), *dec = malloc(n);
  fread(in, 1, n, f); fclose(f);
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, &p)) return 1;
  size_t ds = n;
  if (!GLZAdecode(sz, out, &ds, dec, 0, &p)) return 2;
  printf("OK max_rules=5000 outsize=%zu\n", sz);
  return ds == n ? 0 : 3;
}
