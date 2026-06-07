#include <stdio.h>
#include <stdlib.h>
#include "GLZA.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/glza_null.bin";
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t sz = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(sz);
  uint8_t *dec = malloc(20 * 1024 * 1024);
  if (!in || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, sz, f) != sz) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);
  size_t decsize = 20 * 1024 * 1024;
  if (GLZAdecode(sz, in, &decsize, dec, 0, 0) == 0) {
    fprintf(stderr, "decode fail\n");
    return 1;
  }
  printf("ok %zu\n", decsize);
  return 0;
}
