#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "../enwik1m";
  /* Author-like scoring (fast_mode=0, order=0.6, profit_ratio_power=4) without forcing 16GB RAM.
   * Set GLZA_RAM_MB to override (e.g. 16000 for full author bench on large machines). */
  struct param_data p = { 0xA00000, 0, 0, 0, 1, 0, 0, 1, 0, 2, 0, 0.0, 0.6, 4.0 };
  p.two_threads = 0;
  const char *ram_mb = getenv("GLZA_RAM_MB");
  if (ram_mb != NULL && ram_mb[0] != '\0') {
    p.user_set_RAM_size = 1;
    p.RAM_usage = strtod(ram_mb, NULL);
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n);
  uint8_t *out = malloc(n + 32 * 1024 * 1024);
  uint8_t *dec = malloc(n);
  if (!in || !out || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, n, f) != n) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, &p)) {
    fprintf(stderr, "compress fail\n");
    return 1;
  }
  size_t ds = n;
  if (!GLZAdecode(sz, out, &ds, dec, 0, &p)) {
    fprintf(stderr, "decode fail\n");
    return 2;
  }
  if (ds != n || memcmp(in, dec, n)) {
    fprintf(stderr, "mismatch ds=%zu\n", ds);
    return 3;
  }
  printf("author-like fast_mode=0 OK outsize=%zu corpus=%s\n", sz, path);
  return 0;
}
