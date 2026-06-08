#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

static struct param_data cli_large = {
  0xA00000, 0, 0, 0, 1, 1, 0, 0, 0, 2, 1, 0.0, 0.0, 0.0
};

static int roundtrip(struct param_data *p, const char *label, uint8_t *in, size_t n,
    uint8_t *out, uint8_t *dec) {
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, p)) {
    fprintf(stderr, "%s: compress failed\n", label);
    return 1;
  }
  size_t ds = n;
  if (!GLZAdecode(sz, out, &ds, dec, 0, p)) {
    fprintf(stderr, "%s: decode failed (decsize=%zu)\n", label, ds);
    return 2;
  }
  if (ds != n) {
    fprintf(stderr, "%s: size mismatch %zu != %zu\n", label, ds, n);
    return 3;
  }
  for (size_t i = 0; i < n; i++) {
    if (in[i] != dec[i]) {
      fprintf(stderr, "%s: byte mismatch at %zu\n", label, i);
      return 4;
    }
  }
  printf("%s: OK outsize=%zu\n", label, sz);
  return 0;
}

int main(int argc, char **argv) {
  uint32_t max_rules = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 0xA00000;
  const char *path = argc > 2 ? argv[2] : "../enwik10m";
  struct param_data p = cli_large;
  p.max_rules = max_rules;
  p.two_threads = 0;

  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size_t n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *in = (uint8_t*)malloc(n);
  uint8_t *out = (uint8_t*)malloc(n + 16 * 1024 * 1024);
  uint8_t *dec = (uint8_t*)malloc(n);
  if (!in || !out || !dec) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  if (fread(in, 1, n, f) != n) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  char label[64];
  snprintf(label, sizeof(label), "max_rules=%u", (unsigned)max_rules);
  return roundtrip(&p, label, in, n, out, dec);
}
