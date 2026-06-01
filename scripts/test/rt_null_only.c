#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "../enwik10m";
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t insize = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(insize);
  uint8_t *out = malloc(insize + 16 * 1024 * 1024);
  uint8_t *dec = malloc(insize);
  if (!in || !out || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, insize, f) != insize) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  size_t outsize = 0;
  if (!GLZAcomp(insize, in, &outsize, out, 0, 0)) {
    fprintf(stderr, "compress fail\n");
    return 1;
  }
  size_t decsize = insize;
  if (!GLZAdecode(outsize, out, &decsize, dec, 0, 0)) {
    fprintf(stderr, "decode fail decsize=%zu\n", decsize);
    return 2;
  }
  if (decsize != insize) {
    fprintf(stderr, "size mismatch decsize=%zu insize=%zu\n", decsize, insize);
    return 3;
  }
  for (size_t i = 0; i < insize; i++) {
    if (in[i] != dec[i]) {
      fprintf(stderr, "mismatch at %zu\n", i);
      return 4;
    }
  }
  printf("OK NULL params outsize=%zu\n", outsize);
  return 0;
}
