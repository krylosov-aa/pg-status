/** General support functions for compiled tests. */

#include "common_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

[[noreturn]] void support_fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(EXIT_FAILURE);
}

void support_assert_true(const bool condition, const char *message) {
  if (!condition) {
    support_fail(message);
  }
}

void support_assert_string_equal(
  const char *actual, const char *expected, const char *message
) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "Expected: '%s'\nActual:   '%s'\n", expected, actual);
    support_fail(message);
  }
}

void support_assert_contains(
  const char *actual, const char *expected, const char *message
) {
  if (!strstr(actual, expected)) {
    fprintf(
      stderr, "Expected output to contain: '%s'\nActual: '%s'\n", expected,
      actual
    );
    support_fail(message);
  }
}

void support_assert_not_contains(
  const char *actual, const char *unexpected, const char *message
) {
  if (strstr(actual, unexpected)) {
    fprintf(
      stderr, "Expected output not to contain: '%s'\nActual: '%s'\n",
      unexpected, actual
    );
    support_fail(message);
  }
}

void support_set_environment(const char *name, const char *value) {
  if (setenv(name, value, 1) != 0) {
    support_fail("setenv failed");
  }
}

void support_clear_environment(const char *name) {
  if (unsetenv(name) != 0) {
    support_fail("unsetenv failed");
  }
}

char *support_capture_standard_error(const support_action_t action) {
  FILE *capture = tmpfile();
  if (!capture) {
    support_fail("tmpfile failed");
  }

  if (fflush(stderr) != 0) {
    fclose(capture);
    support_fail("fflush failed");
  }

  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    fclose(capture);
    support_fail("dup failed");
  }
  if (dup2(fileno(capture), STDERR_FILENO) < 0) {
    close(saved_stderr);
    fclose(capture);
    support_fail("dup2 failed");
  }

  action();

  if (fflush(stderr) != 0) {
    support_fail("captured stderr flush failed");
  }
  if (dup2(saved_stderr, STDERR_FILENO) < 0) {
    support_fail("stderr restore failed");
  }
  close(saved_stderr);

  if (fseek(capture, 0, SEEK_END) != 0) {
    fclose(capture);
    support_fail("fseek failed");
  }
  const long captured_length = ftell(capture);
  if (captured_length < 0) {
    fclose(capture);
    support_fail("ftell failed");
  }
  if (fseek(capture, 0, SEEK_SET) != 0) {
    fclose(capture);
    support_fail("fseek failed");
  }

  const size_t output_size = (size_t)captured_length + 1;
  char *output = calloc(output_size, 1);
  if (!output) {
    fclose(capture);
    support_fail("calloc failed");
  }
  if (
    fread(output, 1, (size_t)captured_length, capture) !=
    (size_t)captured_length
  ) {
    free(output);
    fclose(capture);
    support_fail("fread failed");
  }
  fclose(capture);
  return output;
}
