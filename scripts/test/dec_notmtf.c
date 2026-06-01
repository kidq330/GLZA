#include <stdio.h>
#include <stdlib.h>
#include "GLZA.h"
#include "GLZAdecode.h"

int main(void) {
  struct param_data p = { 0xA00000, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };
  FILE *f = fopen("/tmp/glza_null.bin", "rb");
  fseek(f, 0, SEEK_END);
  size_t sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(sz), *dec = malloc(20*1024*1024);
  fread(in, 1, sz, f);
  fclose(f);
  size_t decsize = 20*1024*1024;
  GLZAdecode(sz, in, &decsize, dec, 0, &p);
  printf("ok %zu\n", decsize);
  return 0;
}
