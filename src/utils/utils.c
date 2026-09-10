#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logger.h"

char *copy_string(const char *str) {
  assert(str);
  char *result = strdup(str);
  if (!result) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal("utils", error_number, "failed to copy string");
  }
  return result;
}

bool is_equal_strings(const char *first, const char *second) {
  if (!first || !second) {
    return false;
  }
  return strcmp(first, second) == 0;
}

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

char *format_lsn(const uint64_t lsn) {
  return format_string(
    "%lX/%lX", (unsigned long)(lsn >> 32), (unsigned long)(lsn & 0xFFFFFFFFULL)
  );
}

int str_to_int(const char *value) {
  const long result = str_to_long(value);
  if (result < INT_MIN || result > INT_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to int", value);
  }
  return (int)result;
}

int str_to_int_greater_or_equal_zero(const char *value) {
  const int result = str_to_int(value);
  if (result < 0) {
    pg_status_log_fatal(
      "utils", "failed to convert '%s' to int greater or equal zero", value
    );
  }
  return result;
}

unsigned int str_to_uint(const char *value) {
  const unsigned long result = str_to_ulong(value);
  if (result > UINT_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to uint", value);
  }
  return (unsigned int)result;
}

uint16_t str_to_uint16(const char *value) {
  const unsigned long result = str_to_ulong(value);
  if (result > UINT16_MAX) {
    pg_status_log_fatal("utils", "failed to convert '%s' to uint16", value);
  }
  return (uint16_t)result;
}

void replace_from_env(const char *env_name, const char **result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = env_val;
  }
}

void replace_from_env_uint(const char *env_name, unsigned int *result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = str_to_uint(env_val);
  }
}

void replace_from_env_ull(const char *env_name, uint64_t *result) {
  assert(env_name);
  const char *env_val = getenv(env_name);
  if (env_val && *env_val) {
    *result = str_to_ull(env_val);
  }
}

cJSON *json_array(void) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr) {
    pg_status_log_fatal("utils", "failed to create JSON array");
  }
  return arr;
}

cJSON *json_object(void) {
  cJSON *arr = cJSON_CreateObject();
  if (!arr) {
    pg_status_log_fatal("utils", "failed to create JSON object");
  }
  return arr;
}

void add_str_to_json_object(cJSON *obj, const char *key, const char *val) {
  assert(obj);
  assert(key);
  assert(val);
  if (!cJSON_AddStringToObject(obj, key, val)) {
    pg_status_log_fatal("utils", "failed to add string to JSON object");
  }
}

void add_nullable_str_to_json_object(
  cJSON *json_obj, const char *name, const char *value
) {
  if (value) {
    add_str_to_json_object(json_obj, name, value);
  } else {
    add_null_to_json_object(json_obj, name);
  }
}

void add_null_to_json_object(cJSON *obj, const char *key) {
  assert(obj);
  assert(key);
  if (!cJSON_AddNullToObject(obj, key)) {
    pg_status_log_fatal("utils", "failed to add null to JSON object");
  }
}

void add_bool_to_json_object(cJSON *obj, const char *key, const bool val) {
  assert(obj);
  assert(key);
  if (!cJSON_AddBoolToObject(obj, key, val)) {
    pg_status_log_fatal("utils", "failed to add bool to JSON object");
  }
}

void add_uint64_to_json_object(
  cJSON *obj, const char *key, const uint64_t val
) {
  assert(obj);
  assert(key);
  if (!cJSON_AddNumberToObject(obj, key, (double)val)) {
    pg_status_log_fatal("utils", "failed to add number to JSON object");
  }
}

char *json_to_str(cJSON *json) {
  assert(json);
  char *result = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!result) {
    pg_status_log_fatal("utils", "failed to convert JSON to string");
  }
  return result;
}

uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}
