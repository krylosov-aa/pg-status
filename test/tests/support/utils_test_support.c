/** Support callbacks specific to utility tests. */

#include "utils_test_support.h"

#include <errno.h>

#include "utils.h"

void support_emit_printf_error(void) {
  errno = ENOENT;
  printf_error("Cannot open %s", "file");
}
