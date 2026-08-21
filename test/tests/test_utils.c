/** Test scenarios for general-purpose utilities. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common_support.h"
#include "utils.h"
#include "utils_test_support.h"

static void test_printf_error(void) {
  // Arrange
  char expected_error[512];
  const int length = snprintf(
    expected_error, sizeof(expected_error), "%s (errno=%d)\n", strerror(ENOENT),
    ENOENT
  );
  support_assert_true(
    length > 0 && (size_t)length < sizeof(expected_error),
    "printf_error expected output is too large"
  );

  // Act
  char *actual = support_capture_standard_error(support_emit_printf_error);

  // Assert
  support_assert_contains(
    actual, " ERROR utils: Cannot open file:", "printf_error output"
  );
  support_assert_contains(actual, expected_error, "printf_error errno");

  // Cleanup
  free(actual);
}

static void test_raise_error(void) {
  // Arrange
  errno = ENOENT;

  // Act
  raise_error("Cannot open %s", "file");
}

static void test_copy_string(void) {
  // Arrange
  const char source[] = "copied value";

  // Act
  char *copy = copy_string(source);

  // Assert
  support_assert_string_equal(copy, source, "copy_string value");
  support_assert_true(
    copy != source, "copy_string returned the source pointer"
  );

  // Cleanup
  free(copy);
}

static void test_concatenate_strings(void) {
  // Arrange
  const char *first = "first";
  const char *second = " second";

  // Act
  char *result = concatenate_strings(first, second);

  // Assert
  support_assert_string_equal(
    result, "first second", "concatenate_strings value"
  );

  // Cleanup
  free(result);
}

static void test_is_equal_strings(void) {
  // Arrange
  const char first[] = "same";
  const char second[] = "same";

  // Act & Assert
  support_assert_true(is_equal_strings(first, second), "equal strings differ");
  support_assert_true(
    !is_equal_strings(first, "other"), "different strings match"
  );
  support_assert_true(
    !is_equal_strings(nullptr, second), "null first string matches"
  );
  support_assert_true(
    !is_equal_strings(first, nullptr), "null second string matches"
  );
}

static void test_format_string(void) {
  // Arrange
  const char *name = "replica";
  const int port = 5432;

  // Act
  char *result = format_string("%s:%d", name, port);

  // Assert
  support_assert_string_equal(result, "replica:5432", "format_string value");

  // Cleanup
  free(result);
}

static void test_ulong_to_str(void) {
  // Arrange & Act
  char *result = ulong_to_str(42UL);

  // Assert
  support_assert_string_equal(result, "42", "ulong_to_str value");

  // Cleanup
  free(result);
}

static void test_long_to_str(void) {
  // Arrange & Act
  char *result = long_to_str(-42L);

  // Assert
  support_assert_string_equal(result, "-42", "long_to_str value");

  // Cleanup
  free(result);
}

static void test_int_to_str(void) {
  // Arrange & Act
  char *result = int_to_str(INT_MIN);

  // Assert
  support_assert_string_equal(result, "-2147483648", "int_to_str value");

  // Cleanup
  free(result);
}

static void test_uint_to_str(void) {
  // Arrange & Act
  char *result = uint_to_str(UINT_MAX);

  // Assert
  support_assert_string_equal(result, "4294967295", "uint_to_str value");

  // Cleanup
  free(result);
}

static void test_str_to_long(void) {
  // Arrange & Act
  const long result = str_to_long("-42");

  // Assert
  support_assert_true(result == -42L, "str_to_long value");
}

static void test_str_to_long_null(void) {
  // Arrange & Act
  (void)str_to_long(nullptr);
}

static void test_str_to_long_invalid(void) {
  // Arrange & Act
  (void)str_to_long("42x");
}

static void test_str_to_ulong(void) {
  // Arrange & Act
  const unsigned long result = str_to_ulong("4294967295");

  // Assert
  support_assert_true(result == 4294967295UL, "str_to_ulong value");
}

static void test_str_to_ulong_null(void) {
  // Arrange & Act
  (void)str_to_ulong(nullptr);
}

static void test_str_to_ulong_negative(void) {
  // Arrange & Act
  (void)str_to_ulong("-1");
}

static void test_str_to_ull(void) {
  // Arrange & Act
  const uint64_t result = str_to_ull("18446744073709551615");

  // Assert
  support_assert_true(result == UINT64_MAX, "str_to_ull value");
}

static void test_str_to_ull_null(void) {
  // Arrange & Act
  (void)str_to_ull(nullptr);
}

static void test_str_to_ull_overflow(void) {
  // Arrange & Act
  (void)str_to_ull("18446744073709551616");
}

static void test_try_str_to_ull_valid(void) {
  // Arrange
  uint64_t result = 0;

  // Act
  const bool parsed = try_str_to_ull("18446744073709551615", &result);

  // Assert
  support_assert_true(parsed, "try_str_to_ull rejected a valid value");
  support_assert_true(result == UINT64_MAX, "try_str_to_ull value");
}

static void test_try_str_to_ull_invalid(void) {
  // Arrange
  const char *invalid_values[] = {
    nullptr, "", "-1", "text", "1x", "18446744073709551616",
  };

  // Act & Assert: all rows exercise the same malformed-input contract.
  for (size_t i = 0; i < sizeof(invalid_values) / sizeof(invalid_values[0]);
       i++) {
    uint64_t result = 123;
    support_assert_true(
      !try_str_to_ull(invalid_values[i], &result),
      "try_str_to_ull accepted an invalid value"
    );
    support_assert_true(
      result == 123, "try_str_to_ull changed output on failure"
    );
  }
}

static void test_try_parse_lsn_valid(void) {
  // Arrange
  uint64_t result = 0;

  // Act
  const bool parsed = try_parse_lsn("ABCDEF01/12345678", &result);

  // Assert
  support_assert_true(parsed, "try_parse_lsn rejected a valid LSN");
  support_assert_true(
    result == UINT64_C(0xABCDEF0112345678), "try_parse_lsn value"
  );
}

static void test_try_parse_lsn_invalid(void) {
  // Arrange
  const char *invalid_values[] = {
    nullptr,       "",
    "-1/0",        "+1/0",
    "/0",          "0/",
    "0",           "0/-1",
    "0/+1",        "xyz/0",
    "0/xyz",       "100000000/0",
    "0/100000000", "0/0/trailing",
  };

  // Act & Assert: all rows exercise the same malformed-LSN contract.
  for (size_t i = 0; i < sizeof(invalid_values) / sizeof(invalid_values[0]);
       i++) {
    uint64_t result = 123;
    support_assert_true(
      !try_parse_lsn(invalid_values[i], &result),
      "try_parse_lsn accepted an invalid LSN"
    );
    support_assert_true(
      result == 123, "try_parse_lsn changed output on failure"
    );
  }
}

static void test_format_lsn(void) {
  // Arrange & Act
  char *result = format_lsn(UINT64_C(0xABCDEF0112345678));

  // Assert
  support_assert_string_equal(result, "ABCDEF01/12345678", "format_lsn value");

  // Cleanup
  free(result);
}

static void test_str_to_int(void) {
  // Arrange & Act
  const int result = str_to_int("-2147483648");

  // Assert
  support_assert_true(result == INT_MIN, "str_to_int value");
}

static void test_str_to_int_overflow(void) {
  // Arrange & Act
  (void)str_to_int("2147483648");
}

static void test_str_to_int_greater_or_equal_zero(void) {
  // Arrange & Act
  const int result = str_to_int_greater_or_equal_zero("0");

  // Assert
  support_assert_true(result == 0, "str_to_int_greater_or_equal_zero value");
}

static void test_str_to_int_greater_or_equal_zero_negative(void) {
  // Arrange & Act
  (void)str_to_int_greater_or_equal_zero("-1");
}

static void test_str_to_uint(void) {
  // Arrange & Act
  const unsigned int result = str_to_uint("4294967295");

  // Assert
  support_assert_true(result == UINT_MAX, "str_to_uint value");
}

static void test_str_to_uint_overflow(void) {
  // Arrange & Act
  (void)str_to_uint("4294967296");
}

static void test_str_to_uint16(void) {
  // Arrange & Act
  const uint16_t result = str_to_uint16("65535");

  // Assert
  support_assert_true(result == UINT16_MAX, "str_to_uint16 value");
}

static void test_str_to_uint16_overflow(void) {
  // Arrange & Act
  (void)str_to_uint16("65536");
}

static constexpr char ENV_NAME[] = "PG_STATUS_UTILS_TEST_VALUE";

static void test_replace_from_env(void) {
  // Arrange
  const char *result = "default";
  support_set_environment(ENV_NAME, "configured");

  // Act
  replace_from_env(ENV_NAME, &result);

  // Assert
  support_assert_string_equal(result, "configured", "replace_from_env value");

  // Cleanup
  support_clear_environment(ENV_NAME);
}

static void test_replace_from_env_uint(void) {
  // Arrange
  unsigned int result = 1;
  support_set_environment(ENV_NAME, "4294967295");

  // Act
  replace_from_env_uint(ENV_NAME, &result);

  // Assert
  support_assert_true(result == UINT_MAX, "replace_from_env_uint value");

  // Cleanup
  support_clear_environment(ENV_NAME);
}

static void test_replace_from_env_ull(void) {
  // Arrange
  uint64_t result = 1;
  support_set_environment(ENV_NAME, "18446744073709551615");

  // Act
  replace_from_env_ull(ENV_NAME, &result);

  // Assert
  support_assert_true(result == UINT64_MAX, "replace_from_env_ull value");

  // Cleanup
  support_clear_environment(ENV_NAME);
}

static void test_replace_from_env_copy(void) {
  // Arrange
  char *result = nullptr;
  support_set_environment(ENV_NAME, "copied environment value");
  const char *environment_value = getenv(ENV_NAME);

  // Act
  replace_from_env_copy(ENV_NAME, &result);

  // Assert
  support_assert_string_equal(
    result, "copied environment value", "replace_from_env_copy value"
  );
  support_assert_true(
    result != environment_value,
    "replace_from_env_copy returned the environment pointer"
  );

  // Cleanup
  cJSON_free(result);
  support_clear_environment(ENV_NAME);
}

static void test_replace_from_empty_env(void) {
  // Arrange
  const char *string_result = "default";
  unsigned int uint_result = 12;
  uint64_t ull_result = 34;
  char *copy_result = nullptr;
  support_set_environment(ENV_NAME, "");

  // Act
  replace_from_env(ENV_NAME, &string_result);
  replace_from_env_uint(ENV_NAME, &uint_result);
  replace_from_env_ull(ENV_NAME, &ull_result);
  replace_from_env_copy(ENV_NAME, &copy_result);

  // Assert
  support_assert_string_equal(
    string_result, "default", "empty environment replaced string"
  );
  support_assert_true(uint_result == 12, "empty environment replaced uint");
  support_assert_true(ull_result == 34, "empty environment replaced uint64");
  support_assert_true(
    !copy_result, "empty environment allocated a string copy"
  );

  // Cleanup
  support_clear_environment(ENV_NAME);
}

static void test_json_array(void) {
  // Arrange & Act
  cJSON *array = json_array();

  // Assert
  support_assert_true(cJSON_IsArray(array), "json_array type");
  support_assert_true(
    cJSON_GetArraySize(array) == 0, "json_array is not empty"
  );

  // Cleanup
  cJSON_Delete(array);
}

static void test_json_object(void) {
  // Arrange & Act
  cJSON *object = json_object();

  // Assert
  support_assert_true(cJSON_IsObject(object), "json_object type");

  // Cleanup
  cJSON_Delete(object);
}

static void test_add_str_to_json_object(void) {
  // Arrange
  cJSON *object = cJSON_CreateObject();
  support_assert_true(object != nullptr, "cJSON_CreateObject failed");

  // Act
  add_str_to_json_object(object, "host", "replica");

  // Assert
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "host");
  support_assert_true(cJSON_IsString(item), "JSON string type");
  support_assert_string_equal(
    item->valuestring, "replica", "JSON string value"
  );

  // Cleanup
  cJSON_Delete(object);
}

static void test_add_null_to_json_object(void) {
  // Arrange
  cJSON *object = cJSON_CreateObject();
  support_assert_true(object != nullptr, "cJSON_CreateObject failed");

  // Act
  add_null_to_json_object(object, "lag_ms");

  // Assert
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "lag_ms");
  support_assert_true(cJSON_IsNull(item), "JSON null type");

  // Cleanup
  cJSON_Delete(object);
}

static void test_add_bool_to_json_object(void) {
  // Arrange
  cJSON *object = cJSON_CreateObject();
  support_assert_true(object != nullptr, "cJSON_CreateObject failed");

  // Act
  add_bool_to_json_object(object, "alive", true);

  // Assert
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "alive");
  support_assert_true(cJSON_IsTrue(item), "JSON bool value");

  // Cleanup
  cJSON_Delete(object);
}

static void test_add_uint64_to_json_object(void) {
  // Arrange
  cJSON *object = cJSON_CreateObject();
  support_assert_true(object != nullptr, "cJSON_CreateObject failed");

  // Act
  add_uint64_to_json_object(object, "lag_bytes", UINT32_MAX);

  // Assert
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "lag_bytes");
  support_assert_true(cJSON_IsNumber(item), "JSON number type");
  support_assert_true(
    cJSON_GetNumberValue(item) == (double)UINT32_MAX, "JSON number value"
  );

  // Cleanup
  cJSON_Delete(object);
}

static void test_json_to_str(void) {
  // Arrange
  cJSON *object = cJSON_CreateObject();
  support_assert_true(object != nullptr, "cJSON_CreateObject failed");
  support_assert_true(
    cJSON_AddStringToObject(object, "host", "master") != nullptr,
    "cJSON_AddStringToObject failed"
  );

  // Act
  char *result = json_to_str(object);

  // Assert
  support_assert_string_equal(
    result, "{\"host\":\"master\"}", "json_to_str value"
  );

  // Cleanup
  cJSON_free(result);
}

static void test_monotonic_ms(void) {
  // Arrange
  const uint64_t before = monotonic_ms();
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000L};

  // Act
  const int sleep_result = nanosleep(&delay, nullptr);
  const uint64_t after = monotonic_ms();

  // Assert
  support_assert_true(sleep_result == 0, "nanosleep failed");
  support_assert_true(after > before, "monotonic_ms did not advance");
}

static const struct {
  const char *name;
  support_action_t function;
} test_cases[] = {
  {"printf_error", test_printf_error},
  {"raise_error", test_raise_error},
  {"copy_string", test_copy_string},
  {"concatenate_strings", test_concatenate_strings},
  {"is_equal_strings", test_is_equal_strings},
  {"format_string", test_format_string},
  {"ulong_to_str", test_ulong_to_str},
  {"long_to_str", test_long_to_str},
  {"int_to_str", test_int_to_str},
  {"uint_to_str", test_uint_to_str},
  {"str_to_long", test_str_to_long},
  {"str_to_long_null", test_str_to_long_null},
  {"str_to_long_invalid", test_str_to_long_invalid},
  {"str_to_ulong", test_str_to_ulong},
  {"str_to_ulong_null", test_str_to_ulong_null},
  {"str_to_ulong_negative", test_str_to_ulong_negative},
  {"str_to_ull", test_str_to_ull},
  {"str_to_ull_null", test_str_to_ull_null},
  {"str_to_ull_overflow", test_str_to_ull_overflow},
  {"try_str_to_ull_valid", test_try_str_to_ull_valid},
  {"try_str_to_ull_invalid", test_try_str_to_ull_invalid},
  {"try_parse_lsn_valid", test_try_parse_lsn_valid},
  {"try_parse_lsn_invalid", test_try_parse_lsn_invalid},
  {"format_lsn", test_format_lsn},
  {"str_to_int", test_str_to_int},
  {"str_to_int_overflow", test_str_to_int_overflow},
  {"str_to_int_greater_or_equal_zero", test_str_to_int_greater_or_equal_zero},
  {"str_to_int_greater_or_equal_zero_negative",
   test_str_to_int_greater_or_equal_zero_negative},
  {"str_to_uint", test_str_to_uint},
  {"str_to_uint_overflow", test_str_to_uint_overflow},
  {"str_to_uint16", test_str_to_uint16},
  {"str_to_uint16_overflow", test_str_to_uint16_overflow},
  {"replace_from_env", test_replace_from_env},
  {"replace_from_env_uint", test_replace_from_env_uint},
  {"replace_from_env_ull", test_replace_from_env_ull},
  {"replace_from_env_copy", test_replace_from_env_copy},
  {"replace_from_empty_env", test_replace_from_empty_env},
  {"json_array", test_json_array},
  {"json_object", test_json_object},
  {"add_str_to_json_object", test_add_str_to_json_object},
  {"add_null_to_json_object", test_add_null_to_json_object},
  {"add_bool_to_json_object", test_add_bool_to_json_object},
  {"add_uint64_to_json_object", test_add_uint64_to_json_object},
  {"json_to_str", test_json_to_str},
  {"monotonic_ms", test_monotonic_ms},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }

  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("utils_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }

  support_fail("unknown test case name");
}
