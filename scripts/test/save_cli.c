#include <stdio.h>
#include <stdlib.h>
#include "GLZA.h"
#include "GLZAcomp.h"

int main(void) {
  struct param_data cli = { 5000, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0.0, 0.0, 0.0 };
  FILE *f = fopen("../enwik10m", "rb");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 16*1024*1024);
  fread(in, 1, n, f);
  fclose(f);
  size_t sz = 0;
  GLZAcomp(n, in, &sz, out, 0, &cli);
  FILE *o = fopen("/tmp/glza_cli.bin", "wb");
  fwrite(out, 1, sz, o);
  fclose(o);
  printf("wrote %zu\n", sz);
  return 0;
}
