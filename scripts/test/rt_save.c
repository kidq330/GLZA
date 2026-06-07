#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"

int main(int argc, char **argv) {
  uint32_t max_rules = (uint32_t)strtoul(argv[1], 0, 0);
  const char *path = argv[2];
  struct param_data p = { max_rules, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0.0, 0.0, 0.0 };
  FILE *f = fopen("../enwik10m", "rb");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = malloc(n), *out = malloc(n + 16*1024*1024);
  fread(in, 1, n, f);
  fclose(f);
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, &p)) return 1;
  f = fopen(path, "wb");
  fwrite(out, 1, sz, f);
  fclose(f);
  printf("wrote %zu to %s\n", sz, path);
  return 0;
}
