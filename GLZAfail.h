#if defined(__cplusplus)
extern "C" {
#endif

#include <stddef.h>

void GLZAfail_clear(void);
void GLZAfail_set(const char *stage, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
const char *GLZAfail_stage(void);
const char *GLZAfail_detail(void);

#if defined(__cplusplus)
}
#endif
