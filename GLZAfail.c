#include <stdio.h>
#include <stdarg.h>
#include "GLZAfail.h"

static char glza_fail_stage[24];
static char glza_fail_detail[768];

void GLZAfail_clear(void) {
  glza_fail_stage[0] = '\0';
  glza_fail_detail[0] = '\0';
}

void GLZAfail_set(const char *stage, const char *fmt, ...) {
  va_list ap;
  size_t i;

  if (stage != 0) {
    for (i = 0; i < sizeof(glza_fail_stage) - 1 && stage[i] != '\0'; i++) {
      glza_fail_stage[i] = stage[i];
    }
    glza_fail_stage[i] = '\0';
  }
  if (fmt == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(glza_fail_detail, sizeof(glza_fail_detail), fmt, ap);
  va_end(ap);
}

const char *GLZAfail_stage(void) {
  return(glza_fail_stage);
}

const char *GLZAfail_detail(void) {
  return(glza_fail_detail);
}
