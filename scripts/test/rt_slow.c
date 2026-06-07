#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GLZA.h"
#include "GLZAcomp.h"
#include "GLZAdecode.h"

static struct param_data slow_defaults = {
  0xA00000, 0, 0, 0, 1, 0, 0, 0, 0, 2, 1, 0.0, 0.0, 0.0
};

static void print_comp_fail(const char *label) {
  const char *stage = GLZA_last_fail_stage();
  const char *detail = GLZA_last_fail_detail();
  fprintf(stderr, "%s: compress failed", label);
  if (stage != 0 && stage[0] != '\0') {
    fprintf(stderr, " [%s]", stage);
  }
  fprintf(stderr, "\n");
  if (detail != 0 && detail[0] != '\0') {
    fprintf(stderr, "  -> %s\n", detail);
  }
}

static int roundtrip(struct param_data *p, const char *label, uint8_t *in, size_t n,
    uint8_t *out, uint8_t *dec) {
  size_t sz = 0;
  if (!GLZAcomp(n, in, &sz, out, 0, p)) {
    print_comp_fail(label);
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
  uint32_t max_rules = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 5000;
  const char *path = argc > 2 ? argv[2] : "enwik1m";
  struct param_data p = slow_defaults;
  p.max_rules = max_rules;
  p.two_threads = 0;
  /* Optional: GLZA_RAM_MB=<megabytes> raises the grammar arena (default uses
   * in_size heuristic, ~2.5GB for 10MB input — no need for 16GB). */
  {
    const char *ram_mb = getenv("GLZA_RAM_MB");
    if (ram_mb != NULL && ram_mb[0] != '\0') {
      p.user_set_RAM_size = 1;
      p.RAM_usage = strtod(ram_mb, NULL);
    }
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
  size_t out_cap = n + 128 * 1024 * 1024;
  uint8_t *out = malloc(out_cap);
  uint8_t *dec = malloc(n);
  if (!in || !out || !dec) {
    fprintf(stderr, "malloc failed (input=%zu out_cap=%zu)\n", n, out_cap);
    return 1;
  }
  if (fread(in, 1, n, f) != n) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  fclose(f);

  char label[80];
  snprintf(label, sizeof(label), "fast_mode=0 max_rules=%u", (unsigned)max_rules);
  return roundtrip(&p, label, in, n, out, dec);
}
