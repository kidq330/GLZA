#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

int main(void) {
  /* Full author bench preset; set GLZA_RAM_MB=16000 on machines with enough RAM. */
  struct param_data p = { 0xA00000, 0, 0, 0, 1, 0, 0, 1, 0, 2, 0, 0.0, 0.6, 4.0 };
  const char *ram_mb = getenv("GLZA_RAM_MB");
  if (ram_mb != NULL && ram_mb[0] != '\0') {
    p.user_set_RAM_size = 1;
    p.RAM_usage = strtod(ram_mb, NULL);
  }
  FILE *f = fopen("../enwik10m", "rb");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 32*1024*1024), *dec = malloc(n);
  fread(in, 1, n, f);
  fclose(f);
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, &p)) { fprintf(stderr, "compress fail\n"); return 1; }
  size_t ds = n;
  if (!GLZAdecode(sz, out, &ds, dec, 0, &p)) { fprintf(stderr, "decode fail\n"); return 2; }
  if (ds != n || memcmp(in, dec, n)) { fprintf(stderr, "mismatch %zu\n", ds); return 3; }
  printf("author-like full max_rules OK outsize=%zu\n", sz);
  return 0;
}
