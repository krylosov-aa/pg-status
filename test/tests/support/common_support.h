#ifndef COMMON_SUPPORT_H
#define COMMON_SUPPORT_H

#include <stdbool.h>

typedef void (*support_action_t)(void);

[[noreturn]] void support_fail(const char *message);
void support_assert_true(bool condition, const char *message);
void support_assert_string_equal(
  const char *actual, const char *expected, const char *message
);
void support_assert_contains(
  const char *actual, const char *expected, const char *message
);
void support_assert_not_contains(
  const char *actual, const char *unexpected, const char *message
);

void support_set_environment(const char *name, const char *value);
void support_clear_environment(const char *name);

char *support_capture_standard_error(support_action_t action);

#endif  // COMMON_SUPPORT_H
