/**
 * General purpose utilities
 */

#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

static constexpr size_t LEGACY_ERROR_CAPACITY = 2048;

/**
 * Copies a string. The result must be freed by the caller.
 */
char *copy_string(const char *str) {
  assert(str);
  char *result = strdup(str);
  if (!result) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal("utils", error_number, "failed to copy string");
  }
  return result;
}

/**
 * Prints the message to stderr with \n and also adds the error text from errno
 */
void printf_error(const char *format, ...) {
  const int error_number = errno;
  char message[LEGACY_ERROR_CAPACITY];
  va_list args;
  va_start(args, format);
  // Clang 21 does not model C23's __builtin_c23_va_start correctly.
  // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
  const int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length < 0) {
    (void)snprintf(message, sizeof(message), "unable to format error");
  }
  pg_status_log_system_error(
    PG_STATUS_LOG_ERROR, "utils", error_number, "%s", message
  );
}

/**
 * Prints the message to stderr with \n and also adds the
 * error text from errno and abort
 */
void raise_error(const char *format, ...) {
  const int error_number = errno;
  char message[LEGACY_ERROR_CAPACITY];
  va_list args;
  va_start(args, format);
  // Clang 21 does not model C23's __builtin_c23_va_start correctly.
  // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
  const int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length < 0) {
    (void)snprintf(message, sizeof(message), "unable to format fatal error");
  }
  pg_status_log_system_fatal("utils", error_number, "%s", message);
}

/**
 * Concatenates strings and returns the new string.
 * The result must be freed by the caller.
 */
char *concatenate_strings(const char *first, const char *second) {
  assert(first);
  assert(second);
  const size_t len1 = strlen(first);
  const size_t len2 = strlen(second);
  char *new = malloc(len1 + len2 + 1);
  if (!new) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to concatenate strings '%s' and '%s'",
      first, second
    );
  }
  strlcpy(new, first, len1 + len2 + 1);
  strlcat(new, second, len1 + len2 + 1);
  return new;
}

/**
 * Checks if strings are the same
 */
bool is_equal_strings(const char *first, const char *second) {
  if (!first || !second) {
    return false;
  }
  return strcmp(first, second) == 0;
}

/**
 * Forms a new string and substitutes arguments in printf style.
 * The result must be freed by the caller.
 */
char *format_string(const char *format, ...) {
  va_list args;
  va_start(args, format);
  char *string = nullptr;
  const int len = vasprintf(&string, format, args);
  va_end(args);

  if (len < 0) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to format string format='%s'", format
    );
  }
  return string;
}

/**
 * Converts unsigned long to string. The result must be freed by the caller.
 */
char *ulong_to_str(const unsigned long value) {
  const int len = snprintf(nullptr, 0, "%lu", value);
  const size_t size = (size_t)len + 1;
  char *str = malloc(size);
  if (!str) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to format unsigned long value=%lu", value
    );
  }
  (void)snprintf(str, size, "%lu", value);
  return str;
}

/**
 * Converts long to string. The result must be freed by the caller.
 */
char *long_to_str(const long value) {
  const int len = snprintf(nullptr, 0, "%ld", value);
  const size_t size = (size_t)len + 1;
  char *str = malloc(size);
  if (!str) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to format long value=%ld", value
    );
  }
  (void)snprintf(str, size, "%ld", value);
  return str;
}

/**
 * Converts int to string. The result must be freed by the caller.
 */
char *int_to_str(const int value) {
  const int len = snprintf(nullptr, 0, "%d", value);
  const size_t size = (size_t)len + 1;
  char *str = malloc(size);
  if (!str) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to format int value=%d", value
    );
  }
  (void)snprintf(str, size, "%d", value);
  return str;
}

/**
 * Converts unsigned int to string. The result must be freed by the caller.
 */
char *uint_to_str(const unsigned int value) {
  const int len = snprintf(nullptr, 0, "%u", value);
  const size_t size = (size_t)len + 1;
  char *str = malloc(size);
  if (!str) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "utils", error_number, "failed to format unsigned int value=%u", value
    );
  }
  (void)snprintf(str, size, "%u", value);
  return str;
}

/**
 * Converts string to long
 */
long str_to_long(const char *value) {
  if (!value) {
    pg_status_log_fatal("utils", "failed to convert null to long");
  }

  char *end_ptr = nullptr;
  errno = 0;

  const long result = strtol(value, &end_ptr, 10);

  if (end_ptr == value || *end_ptr != '\0' || errno == ERANGE) {
    pg_status_log_fatal("utils", "failed to convert '%s' to long", value);
  }

  return result;
}

/**
 * Converts string to unsigned long
 */
unsigned long str_to_ulong(const char *value) {
  if (!value) {
    pg_status_log_fatal("utils", "failed to convert null to ulong");
  }
  if (*value == '\0' || *value == '-') {
    pg_status_log_fatal("utils", "failed to convert '%s' to ulong", value);
  }

  char *end_ptr = nullptr;
  errno = 0;

  const unsigned long result = strtoul(value, &end_ptr, 10);

  if (end_ptr == value || *end_ptr != '\0' || errno == ERANGE) {
    pg_status_log_fatal("utils", "failed to convert '%s' to ulong", value);
  }

  return result;
}

/**
 * Converts string to uint64_t
 */
uint64_t str_to_ull(const char *value) {
  if (!value) {
    pg_status_log_fatal("utils", "failed to convert null to ull");
  }
  if (*value == '\0' || *value == '-') {
    pg_status_log_fatal("utils", "failed to convert '%s' to ull", value);
  }

  char *end_ptr = nullptr;
  errno = 0;

  const uint64_t result = strtoull(value, &end_ptr, 10);

  if (end_ptr == value || *end_ptr != '\0' || errno == ERANGE) {
    pg_status_log_fatal("utils", "failed to convert '%s' to ull", value);
  }

  return result;
}

/**
 * Converts string to uint64_t without aborting on failure.
 * Returns true on success and writes the parsed value to *out.
 * Returns false on any malformed input: NULL/empty, leading '-',
 * non-numeric content, or overflow.
 */
bool try_str_to_ull(const char *value, uint64_t *out) {
  if (!value || *value == '\0' || *value == '-') {
    return false;
  }

  char *end_ptr = nullptr;
  errno = 0;

  const uint64_t result = strtoull(value, &end_ptr, 10);

  if (end_ptr == value || *end_ptr != '\0' || errno == ERANGE) {
    return false;
  }

  *out = result;
  return true;
}

/**
 * Parses LSN "HEX/HEX" form into a 64-bit
 * integer (high 32 bits | low 32 bits). Returns true on success and
 * writes the result to *out. Returns false on any malformed input:
 * NULL/empty, missing or leading '/', non-hex content, overflow of
 * either half above 0xFFFFFFFF, or trailing garbage.
 */
bool try_parse_lsn(const char *value, uint64_t *out) {
  if (
    !value || *value == '\0' || *value == '-' || *value == '+' || *value == '/'
  ) {
    return false;
  }

  const char *slash = strchr(value, '/');
  if (!slash) {
    return false;
  }

  char *end_ptr = nullptr;
  errno = 0;
  const unsigned long long hi = strtoull(value, &end_ptr, 16);
  if (end_ptr != slash || errno == ERANGE || hi > 0xFFFFFFFFULL) {
    return false;
  }

  const char *lo_start = slash + 1;
  if (*lo_start == '\0' || *lo_start == '-' || *lo_start == '+') {
    return false;
  }

  errno = 0;
  end_ptr = nullptr;
  const unsigned long long lo = strtoull(lo_start, &end_ptr, 16);
  if (
    end_ptr == lo_start || *end_ptr != '\0' || errno == ERANGE ||
    lo > 0xFFFFFFFFULL
  ) {
    return false;
  }

  *out = ((uint64_t)hi << 32) | (uint64_t)lo;
  return true;
}

/**
 * Formats a 64-bit LSN value back to "HEX/HEX" form
 * The result must be freed by the caller.
 */
char *format_lsn(const uint64_t lsn) {
  return format_string(
    "%lX/%lX", (unsigned long)(lsn >> 32), (unsigned long)(lsn & 0xFFFFFFFFULL)
  );
}

/**
 * Converts string to int
 */
int str_to_int(const char *value) {
  const long result = str_to_long(value);
  if (result < INT_MIN || result > INT_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to int", value);
  }
  return (int)result;
}

/**
 * Converts string to int greater than or equal to zero
 */
int str_to_int_greater_or_equal_zero(const char *value) {
  const int result = str_to_int(value);
  if (result < 0) {
    pg_status_log_fatal(
      "utils", "failed to convert '%s' to int greater or equal zero", value
    );
  }
  return result;
}

/**
 * Converts string to unsigned int
 */
unsigned int str_to_uint(const char *value) {
  const unsigned long result = str_to_ulong(value);
  if (result > UINT_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to uint", value);
  }
  return (unsigned int)result;
}

/**
 * Converts string to unsigned int 16
 */
uint16_t str_to_uint16(const char *value) {
  const unsigned long result = str_to_ulong(value);
  if (result > UINT16_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to uint16", value);
  }
  return (uint16_t)result;
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env(const char *env_name, const char **result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = env_val;
  }
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = str_to_uint(env_val);
  }
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_ull(const char *env_name, uint64_t *result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = str_to_ull(env_val);
  }
}

/**
 * Takes a value from the environment variables if it is set,
 * copies it and pastes it by the result pointer.
 * The string must be released with cJSON_free by the caller.
 */
void replace_from_env_copy(const char *env_name, char **result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val != nullptr && *env_val) {
    char *env_val_copy = copy_string(env_val);
    *result = env_val_copy;
  }
}

/**
 * Creates a new json array object
 */
cJSON *json_array(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) {
    pg_status_log_fatal("utils", "failed to create JSON array");
  }
  return arr;
}

/**
 * Creates a new json object
 */
cJSON *json_object(void) {
  cJSON *arr = cJSON_CreateObject();
  if (!arr) {
    pg_status_log_fatal("utils", "failed to create JSON object");
  }
  return arr;
}

/**
 * Adds a new key and string value to json
 */
void add_str_to_json_object(cJSON *obj, const char *key, const char *val) {
  assert(obj);
  assert(key);
  assert(val);
  if (!cJSON_AddStringToObject(obj, key, val)) {
    pg_status_log_fatal("utils", "failed to add string to JSON object");
  }
}

/**
 * Adds a new key with value null to json
 */
void add_null_to_json_object(cJSON *obj, const char *key) {
  assert(obj);
  assert(key);
  if (!cJSON_AddNullToObject(obj, key)) {
    pg_status_log_fatal("utils", "failed to add null to JSON object");
  }
}

/**
 * Adds a new key and bool value to json
 */
void add_bool_to_json_object(cJSON *obj, const char *key, const bool val) {
  assert(obj);
  assert(key);
  if (!cJSON_AddBoolToObject(obj, key, val)) {
    pg_status_log_fatal("utils", "failed to add bool to JSON object");
  }
}

/**
 * Adds a new key and uint64 value to json as a JSON number.
 */
void add_uint64_to_json_object(
  cJSON *obj, const char *key, const uint64_t val
) {
  assert(obj);
  assert(key);
  if (!cJSON_AddNumberToObject(obj, key, (double)val)) {
    pg_status_log_fatal("utils", "failed to add number to JSON object");
  }
}

/**
 * Converts json to string.
 * The string must be freed by the caller.
 */
char *json_to_str(cJSON *json) {
  assert(json);
  char *result = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!result) {
    pg_status_log_fatal("utils", "failed to convert JSON to string");
  }
  return result;
}

/**
 * Monotonic time in milliseconds.
 */
uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}
