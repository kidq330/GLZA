#include <stdio.h>
#include <stdlib.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  const char *blob = argc > 1 ? argv[1] : "/tmp/glza6500.bin";
  struct param_data p = { 6500, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };
  FILE *f = fopen("../enwik10m", "rb");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 16*1024*1024);
  fread(in, 1, n, f);
  fclose(f);
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, &p)) return 1;
  fprintf(stderr, "compressed %zu, now decode saved blob %s\n", sz, blob);
  f = fopen(blob, "rb");
  if (!f) { perror(blob); return 2; }
  fseek(f, 0, SEEK_END);
  size_t bsz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *bin = malloc(bsz), *dec = malloc(n);
  fread(bin, 1, bsz, f);
  fclose(f);
  size_t ds = n;
  if (!GLZAdecode(bsz, bin, &ds, dec, 0, &p)) return 3;
  printf("post-comp decode-saved OK %zu\n", ds);
  return ds == n ? 0 : 4;
}
