#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/glza6500.bin";
  struct param_data p = { 6500, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 1; }
  fseek(f, 0, SEEK_END);
  size_t sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(sz), *dec = malloc(20*1024*1024);
  fread(in, 1, sz, f);
  fclose(f);
  size_t ds = 20*1024*1024;
  uint8_t *r = GLZAdecode(sz, in, &ds, dec, 0, &p);
  if (!r) { fprintf(stderr, "decode failed\n"); return 2; }
  printf("ok %zu\n", ds);
  return 0;
}
