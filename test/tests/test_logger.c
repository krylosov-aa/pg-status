/** Test scenarios for process logging. */

#include <cjson/cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common_support.h"
#include "log_formatter.h"
#include "logger.h"

typedef void (*logger_test_function_t)(void);

static const char *logger_test_program;
static void emit_long_message(void);

static int save_standard_error(void) {
  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    support_fail("failed to save stderr");
  }
  return saved_stderr;
}

static void restore_standard_error(const int saved_stderr) {
  if (dup2(saved_stderr, STDERR_FILENO) < 0) {
    close(saved_stderr);
    support_fail("failed to restore stderr");
  }
  close(saved_stderr);
}

static void redirect_standard_error(const int descriptor) {
  if (descriptor < 0) {
    support_fail("invalid stderr redirection descriptor");
  }
  if (dup2(descriptor, STDERR_FILENO) < 0) {
    support_fail("failed to redirect stderr");
  }
}

static void emit_info_message(void) {
  pg_status_log(PG_STATUS_LOG_INFO, "test", "value=%d", 42);
  pg_status_log_flush();
}

static bool has_timestamp_shape(const char *line) {
  static const size_t digit_positions[] = {
    0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 22,
  };
  if (
    strlen(line) < 25 || line[4] != '-' || line[7] != '-' || line[10] != 'T' ||
    line[13] != ':' || line[16] != ':' || line[19] != '.' || line[23] != 'Z' ||
    line[24] != ' '
  ) {
    return false;
  }
  for (size_t i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]);
       i++) {
    if (!isdigit((unsigned char)line[digit_positions[i]])) {
      return false;
    }
  }
  return true;
}

static void test_message_format(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_info_message);

  // Assert
  support_assert_true(has_timestamp_shape(output), "log timestamp format");
  support_assert_contains(
    output, " INFO test: value=42\n", "log level, component, or message"
  );

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static cJSON *parse_json_record(const char *line) {
  const char *newline = strchr(line, '\n');
  support_assert_true(
    newline && newline[1] == '\0', "expected one complete JSON log line"
  );
  support_assert_true(strlen(line) < 4096, "JSON log exceeded line capacity");
  cJSON *record = cJSON_ParseWithOpts(line, nullptr, true);
  support_assert_true(cJSON_IsObject(record), "log is not a valid JSON object");
  const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(
    record, "@timestamp"
  );
  support_assert_true(cJSON_IsString(timestamp), "missing JSON timestamp");
  char timestamp_with_space[32];
  snprintf(
    timestamp_with_space, sizeof(timestamp_with_space), "%s ",
    timestamp->valuestring
  );
  support_assert_true(
    strlen(timestamp->valuestring) == 24 &&
      has_timestamp_shape(timestamp_with_space),
    "JSON timestamp is not RFC 3339 UTC"
  );
  const char *keys[] = {"@timestamp", "levelStr", "component", "message"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    support_assert_true(
      cJSON_IsString(cJSON_GetObjectItemCaseSensitive(record, keys[i])),
      "missing JSON log field"
    );
  }
  const bool has_errno = cJSON_HasObjectItem(record, "errno");
  support_assert_true(
    cJSON_GetArraySize(record) == (has_errno ? 5 : 4),
    "unexpected JSON log fields"
  );
  return record;
}

static void assert_json_string(
  const cJSON *object, const char *key, const char *expected
) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  support_assert_true(cJSON_IsString(value), "missing JSON string field");
  support_assert_string_equal(value->valuestring, expected, key);
}

static cJSON *parse_next_json_record(char **cursor) {
  char *next = strchr(*cursor, '\n');
  if (!next) {
    support_fail("unterminated JSON record");
  }
  next++;
  const char saved = *next;
  *next = '\0';
  cJSON *record = parse_json_record(*cursor);
  *next = saved;
  *cursor = next;
  return record;
}

static void init_json_logger(void) {
  support_set_environment("pg_status__log_format", "json");
  pg_status_log_init();
}

static void test_text_format_environment(void) {
  // Arrange
  const char *formats[] = {"text", ""};
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    support_set_environment("pg_status__log_format", formats[i]);

    // Act & Assert
    test_message_format();
  }
}

static void emit_json_messages(void) {
  static const char *component = "monitor\n\"\\\t";
  static const char *message = "replica µ 😀\n\r\t\b\f\001\037\177\"\\";
  pg_status_log_set_level(PG_STATUS_LOG_DEBUG);
  pg_status_log(PG_STATUS_LOG_DEBUG, component, "%s", message);
  pg_status_log(PG_STATUS_LOG_INFO, nullptr, "info");
  pg_status_log(PG_STATUS_LOG_WARNING, "monitor", "warning");
  pg_status_log(PG_STATUS_LOG_ERROR, "monitor", "error");
  pg_status_log_set_level(PG_STATUS_LOG_ERROR);
  pg_status_log(PG_STATUS_LOG_INFO, "monitor", "filtered out");
  pg_status_log_system_error(
    PG_STATUS_LOG_ERROR, "monitor", ENOENT, "open failed"
  );
  pg_status_log(PG_STATUS_LOG_FATAL, "monitor", "fatal severity");
  pg_status_log_flush();
}

static void test_json_format(void) {
  // Arrange
  init_json_logger();

  // Act
  char *output = support_capture_standard_error(emit_json_messages);
  pg_status_log_shutdown();

  // Assert
  static const char *levels[] = {"DEBUG", "INFO",  "WARNING",
                                 "ERROR", "ERROR", "ERROR"};
  size_t index = 0;
  char *line = output;
  while (*line) {
    cJSON *record = parse_next_json_record(&line);
    support_assert_true(
      index < sizeof(levels) / sizeof(levels[0]), "unexpected JSON record"
    );
    assert_json_string(record, "levelStr", levels[index]);
    if (index == 0) {
      assert_json_string(record, "component", "monitor\n\"\\\t");
      assert_json_string(
        record, "message", "replica µ 😀\n\r\t\b\f\001\037\177\"\\"
      );
    } else if (index == 1) {
      assert_json_string(record, "component", "unknown");
    } else if (index == 4) {
      const cJSON *error = cJSON_GetObjectItemCaseSensitive(record, "errno");
      support_assert_true(
        cJSON_IsNumber(error) && error->valueint == ENOENT,
        "missing numeric errno"
      );
      const cJSON *message = cJSON_GetObjectItemCaseSensitive(
        record, "message"
      );
      support_assert_true(cJSON_IsString(message), "missing error message");
      support_assert_contains(
        message->valuestring, strerror(ENOENT), "missing system error detail"
      );
    } else if (index == 5) {
      support_assert_true(
        !cJSON_HasObjectItem(record, "errno"), "errno leaked between records"
      );
    }
    cJSON_Delete(record);
    index++;
  }
  support_assert_true(
    index == sizeof(levels) / sizeof(levels[0]), "missing JSON records"
  );

  // Cleanup
  free(output);
}

static void emit_escaped_long_message(void) {
  char component[5000];
  char message[3000];
  memset(component, '\001', sizeof(component) - 1);
  component[sizeof(component) - 1] = '\0';
  memset(message, '\002', sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
  pg_status_log(PG_STATUS_LOG_WARNING, component, "%s", message);
  pg_status_log_flush();
}

static void test_json_truncation(void) {
  // Arrange
  init_json_logger();
  const support_action_t emitters[] = {
    emit_long_message, emit_escaped_long_message
  };
  for (size_t i = 0; i < sizeof(emitters) / sizeof(emitters[0]); i++) {
    // Act
    char *output = support_capture_standard_error(emitters[i]);

    // Assert
    cJSON *record = parse_json_record(output);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(record, "message");
    support_assert_true(cJSON_IsString(message), "missing truncated message");
    support_assert_contains(
      message->valuestring, "...[truncated]", "missing JSON truncation marker"
    );
    if (i == 1) {
      const cJSON *component = cJSON_GetObjectItemCaseSensitive(
        record, "component"
      );
      support_assert_true(
        cJSON_IsString(component), "missing truncated component"
      );
      support_assert_contains(
        component->valuestring, "...[truncated]",
        "missing component truncation marker"
      );
    }

    // Cleanup
    cJSON_Delete(record);
    free(output);
  }

  // Cleanup
  pg_status_log_shutdown();
}

static void emit_invalid_utf8(void) {
  pg_status_log(
    PG_STATUS_LOG_INFO, "\xff",
    "valid é 😀; invalid \xc0\xaf \xed\xa0\x80 \xf4\x90\x80\x80 \xe2"
  );
  pg_status_log_flush();
}

static void test_json_invalid_utf8(void) {
  // Arrange
  init_json_logger();

  // Act
  char *output = support_capture_standard_error(emit_invalid_utf8);

  // Assert
  cJSON *record = parse_json_record(output);
  assert_json_string(record, "component", "�");
  assert_json_string(record, "message", "valid é 😀; invalid �� ��� ���� �");

  // Cleanup
  cJSON_Delete(record);
  free(output);
  pg_status_log_shutdown();
}

static PGStatusLogEntry json_test_entry(
  const char *component, const char *message
) {
  return (PGStatusLogEntry){
    .timestamp = "2026-09-08T12:34:56.123Z",
    .level = PG_STATUS_LOG_INFO,
    .level_name = "INFO",
    .component = component,
    .message = message,
  };
}

static cJSON *format_json_test_entry(const PGStatusLogEntry *entry) {
  char line[4096];
  const size_t length = pg_status_format_json(line, sizeof(line), entry);
  support_assert_true(
    length > 0 && length == strlen(line), "invalid JSON record length"
  );
  return parse_json_record(line);
}

static void test_json_escape_roundtrip(void) {
  // Arrange
  char ascii[128];
  for (size_t i = 0; i < sizeof(ascii) - 1; i++) {
    ascii[i] = (char)(i + 1);
  }
  ascii[sizeof(ascii) - 1] = '\0';
  const PGStatusLogEntry entry = json_test_entry(ascii, ascii);

  // Act
  cJSON *record = format_json_test_entry(&entry);

  // Assert
  assert_json_string(record, "component", ascii);
  assert_json_string(record, "message", ascii);

  // Cleanup
  cJSON_Delete(record);
}

static void test_json_unicode_boundaries(void) {
  // Arrange
  static const struct {
    const char *input;
    const char *expected;
  } cases[] = {
    // Scalar boundaries: U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF,
    // U+10000 and U+10FFFF must survive serialization unchanged.
    {"\xc2\x80", "\xc2\x80"},
    {"\xdf\xbf", "\xdf\xbf"},
    {"\xe0\xa0\x80", "\xe0\xa0\x80"},
    {"\xed\x9f\xbf", "\xed\x9f\xbf"},
    {"\xee\x80\x80", "\xee\x80\x80"},
    {"\xef\xbf\xbf", "\xef\xbf\xbf"},
    {"\xf0\x90\x80\x80", "\xf0\x90\x80\x80"},
    {"\xf4\x8f\xbf\xbf", "\xf4\x8f\xbf\xbf"},
    {"\xe0\x9f\xbf", "���"},       // Overlong encoding
    {"\xed\xa0\x80", "���"},       // Surrogate
    {"\xf0\x8f\xbf\xbf", "����"},  // Overlong encoding
    {"\xf4\x90\x80\x80", "����"},  // Above U+10FFFF
    {"\xf5\x80\x80\x80", "����"},  // Invalid leading byte
    {"\xc2", "�"},                 // Incomplete sequences
    {"\xe0\xa0", "��"},
    {"\xf0\x90\x80", "���"},
    {"\xe2"
     "A",
     "�A"},  // Preserve the ASCII following a broken sequence
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const PGStatusLogEntry entry = json_test_entry(
      cases[i].input, cases[i].input
    );

    // Act
    cJSON *record = format_json_test_entry(&entry);

    // Assert
    assert_json_string(record, "component", cases[i].expected);
    assert_json_string(record, "message", cases[i].expected);

    // Cleanup
    cJSON_Delete(record);
  }
}

static void test_json_errno_boundaries(void) {
  // Arrange
  const int errors[] = {0, INT_MIN, INT_MAX};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
    PGStatusLogEntry entry = json_test_entry("monitor", "system error");
    entry.error_number = &errors[i];

    // Act
    cJSON *record = format_json_test_entry(&entry);

    // Assert
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(record, "errno");
    support_assert_true(
      cJSON_IsNumber(error) && error->valuedouble == (double)errors[i],
      "JSON errno changed during serialization"
    );

    // Cleanup
    cJSON_Delete(record);
  }
}

static void test_json_exact_capacity(void) {
  // Arrange
  const PGStatusLogEntry entry = json_test_entry("monitor", "ready");
  char expected[256];
  const size_t length = pg_status_format_json(
    expected, sizeof(expected), &entry
  );
  if (length == 0 || length >= sizeof(expected) - 1) {
    support_fail("invalid reference JSON length");
  }
  char line[256];
  memset(line, 'x', sizeof(line));

  // Act & Assert
  support_assert_true(
    pg_status_format_json(line, length + 1, &entry) == length,
    "exactly fitting JSON was not serialized"
  );
  support_assert_string_equal(line, expected, "exactly fitting JSON changed");
  support_assert_true(
    line[length + 1] == 'x', "write past exact buffer capacity"
  );

  // Arrange
  memset(line, 'x', sizeof(line));

  // Act & Assert
  support_assert_true(
    pg_status_format_json(line, length, &entry) == 0,
    "JSON fit without space for its terminator"
  );
  support_assert_true(
    line[0] == '\0' && line[length] == 'x',
    "incomplete JSON or write past buffer capacity"
  );
}

static void assert_json_truncated(const cJSON *object, const char *key) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  support_assert_true(cJSON_IsString(value), "missing truncated JSON field");
  const char *marker = strstr(value->valuestring, "...[truncated]");
  support_assert_true(
    marker && strcmp(marker, "...[truncated]") == 0,
    "expected exactly one trailing truncation marker"
  );
}

static void test_json_fitting_values_preserved(void) {
  // Arrange
  char component[2801];
  memset(component, 'c', sizeof(component) - 1);
  component[sizeof(component) - 1] = '\0';
  char message[1701];
  memset(message, '\n', sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
  const PGStatusLogEntry entries[] = {
    json_test_entry(component, "small message"),
    json_test_entry("monitor", message),
  };
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    // Act
    cJSON *record = format_json_test_entry(&entries[i]);

    // Assert
    assert_json_string(record, "message", entries[i].message);
    assert_json_string(record, "component", entries[i].component);

    // Cleanup
    cJSON_Delete(record);
  }
}

static void test_json_retry_truncation(void) {
  // Arrange
  char component[2001];
  memset(component, '\001', sizeof(component) - 1);
  component[sizeof(component) - 1] = '\0';
  char message[1801];
  memset(message, '\002', sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
  const PGStatusLogEntry entries[] = {
    json_test_entry("monitor", message),
    json_test_entry(component, "small"),
    json_test_entry(component, message),
  };
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    // Act
    cJSON *record = format_json_test_entry(&entries[i]);

    // Assert
    if (i == 0) {
      assert_json_truncated(record, "message");
      assert_json_string(record, "component", "monitor");
    } else if (i == 1) {
      assert_json_string(record, "message", "small");
      assert_json_truncated(record, "component");
    } else {
      assert_json_string(record, "message", "...[truncated]");
      assert_json_truncated(record, "component");
    }

    // Cleanup
    cJSON_Delete(record);
  }
}

static void test_json_utf8_truncation(void) {
  // Arrange
  static const char character[] = "😀";
  char value[7201];
  for (size_t i = 0; i < sizeof(value) - 1; i += sizeof(character) - 1) {
    memcpy(value + i, character, sizeof(character) - 1);
  }
  value[sizeof(value) - 1] = '\0';
  const PGStatusLogEntry entries[] = {
    json_test_entry("monitor", value),
    json_test_entry(value, "small"),
  };
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    // Act
    cJSON *record = format_json_test_entry(&entries[i]);

    // Assert
    const char *key = i == 0 ? "message" : "component";
    assert_json_truncated(record, key);
    const cJSON *truncated = cJSON_GetObjectItemCaseSensitive(record, key);
    const size_t prefix_length = strlen(truncated->valuestring) -
                                 strlen("...[truncated]");
    support_assert_true(
      prefix_length % (sizeof(character) - 1) == 0, "UTF-8 character was split"
    );
    support_assert_true(
      memcmp(truncated->valuestring, value, prefix_length) == 0,
      "UTF-8 prefix changed"
    );

    // Cleanup
    cJSON_Delete(record);
  }
}

static void test_json_small_output_buffer(void) {
  // Arrange
  char value[3001];
  memset(value, '\001', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  const PGStatusLogEntry entry = json_test_entry(value, value);
  for (size_t capacity = 0; capacity <= 512; capacity++) {
    // Arrange
    unsigned char storage[514];
    memset(storage, 0xa5, sizeof(storage));
    char *line = (char *)storage + 1;

    // Act
    const size_t length = pg_status_format_json(line, capacity, &entry);

    // Assert
    support_assert_true(
      storage[0] == 0xa5 && storage[capacity + 1] == 0xa5,
      "JSON formatter overran its output buffer"
    );
    if (length == 0) {
      support_assert_true(capacity < 256, "JSON did not fit after truncation");
      support_assert_true(
        capacity == 0 || line[0] == '\0',
        "incomplete JSON was left in the output buffer"
      );
    } else {
      support_assert_true(
        length < capacity && length == strlen(line),
        "invalid bounded JSON length"
      );
      cJSON *record = parse_json_record(line);
      cJSON_Delete(record);
    }
  }
}

static void emit_filtered_messages(void) {
  pg_status_log(PG_STATUS_LOG_DEBUG, "filter", "debug message");
  pg_status_log(PG_STATUS_LOG_INFO, "filter", "info message");
  pg_status_log_flush();
}

static void test_level_filtering(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_filtered_messages);

  // Assert
  support_assert_not_contains(
    output, "debug message", "disabled log level was emitted"
  );
  support_assert_contains(output, "info message", "enabled log level missing");

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void test_init_from_environment(void) {
  // Arrange
  support_set_environment("pg_status__log_level", "debug");

  // Act
  pg_status_log_init();

  // Assert
  support_assert_true(
    pg_status_log_get_level() == PG_STATUS_LOG_DEBUG,
    "pg_status__log_level was not applied"
  );

  // Cleanup
  support_clear_environment("pg_status__log_level");
  pg_status_log_shutdown();
}

static void test_invalid_level(void) {
  // Arrange
  support_set_environment("pg_status__log_level", "verbose");

  // Act
  pg_status_log_init();
}

static void test_invalid_format(void) {
  // Arrange
  support_set_environment("pg_status__log_format", "yaml");

  // Act
  pg_status_log_init();
}

static void emit_system_error(void) {
  pg_status_log_system_error(
    PG_STATUS_LOG_ERROR, "test", ENOENT, "open failed path=%s", "/missing"
  );
  pg_status_log_flush();
}

static void test_system_error(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);
  char expected_error[256];
  const int length = snprintf(
    expected_error, sizeof(expected_error), "%s (errno=%d)\n", strerror(ENOENT),
    ENOENT
  );
  support_assert_true(
    length > 0 && (size_t)length < sizeof(expected_error),
    "system error expected output is too large"
  );

  // Act
  char *output = support_capture_standard_error(emit_system_error);

  // Assert
  support_assert_contains(
    output, " ERROR test: open failed path=/missing:", "system error message"
  );
  support_assert_contains(output, expected_error, "system error number");

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static int errno_after_log;

static void emit_while_preserving_errno(void) {
  errno = EDOM;
  pg_status_log(PG_STATUS_LOG_INFO, "test", "preserve errno");
  errno_after_log = errno;
  pg_status_log_flush();
}

static void test_errno_preserved(void) {
  // Arrange
  errno_after_log = 0;
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_while_preserving_errno);

  // Assert
  support_assert_true(errno_after_log == EDOM, "logger changed errno");

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void emit_multiline_message(void) {
  pg_status_log(
    PG_STATUS_LOG_INFO, "test\ncomponent",
    "first\tsecond"
    "\x1b"
    "third\x7f"
    "fourth"
  );
  pg_status_log_flush();
}

static void test_multiline_sanitized(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_multiline_message);

  // Assert
  support_assert_contains(
    output, " INFO test component: first second third fourth\n",
    "log control-character sanitization"
  );
  const char *first_newline = strchr(output, '\n');
  support_assert_true(
    first_newline && first_newline[1] == '\0',
    "log message spans multiple lines"
  );

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void emit_long_component(void) {
  char component[5000];
  memset(component, 'c', sizeof(component) - 1);
  component[sizeof(component) - 1] = '\0';
  pg_status_log(PG_STATUS_LOG_INFO, component, "message");
  pg_status_log_flush();
}

static void test_long_component_truncated(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_long_component);

  // Assert
  support_assert_contains(output, "...[truncated]\n", "line truncation marker");
  const char *first_newline = strchr(output, '\n');
  support_assert_true(
    first_newline && first_newline[1] == '\0', "truncated log spans lines"
  );

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void emit_long_message(void) {
  char message[3000];
  memset(message, 'x', sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
  pg_status_log(PG_STATUS_LOG_INFO, "test", "%s", message);
  pg_status_log_flush();
}

static void test_long_message_truncated(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_long_message);

  // Assert
  support_assert_contains(output, "...[truncated]\n", "truncation marker");

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

enum {
  WORKER_COUNT = 4,
  MESSAGES_PER_WORKER = 25,
  CONCURRENT_ROUND_COUNT = 4,
};

static void *log_worker(void *argument) {
  const size_t worker = *(const size_t *)argument;
  for (size_t i = 0; i < MESSAGES_PER_WORKER; i++) {
    pg_status_log(
      PG_STATUS_LOG_INFO, "concurrency", "worker=%zu message=%zu", worker, i
    );
  }
  return nullptr;
}

static void emit_concurrent_messages(void) {
  for (size_t round = 0; round < CONCURRENT_ROUND_COUNT; round++) {
    pthread_t threads[WORKER_COUNT];
    size_t workers[WORKER_COUNT];
    for (size_t i = 0; i < WORKER_COUNT; i++) {
      workers[i] = (round * WORKER_COUNT) + i;
      const int created = pthread_create(
        &threads[i], nullptr, log_worker, &workers[i]
      );
      if (created != 0) {
        support_fail("failed to create logger test thread");
      }
    }
    for (size_t i = 0; i < WORKER_COUNT; i++) {
      if (pthread_join(threads[i], nullptr) != 0) {
        support_fail("failed to join logger test thread");
      }
    }
    pg_status_log_flush();
  }
  // Wait for the final dropped-message summary before restoring stderr.
  pg_status_log_shutdown();
}

typedef struct {
  bool seen[CONCURRENT_ROUND_COUNT * WORKER_COUNT][MESSAGES_PER_WORKER];
  size_t written;
  size_t dropped;
} ConcurrentRecords;

static size_t read_concurrent_number(const char **cursor, const char *prefix) {
  const size_t prefix_length = strlen(prefix);
  if (strncmp(*cursor, prefix, prefix_length) != 0) {
    support_fail("corrupted concurrent message prefix");
  }
  const char *number = *cursor + prefix_length;
  if (!isdigit((unsigned char)*number)) {
    support_fail("missing concurrent message number");
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = strtoul(number, &end, 10);
  if (errno != 0 || end == number) {
    support_fail("invalid concurrent message number");
  }
  *cursor = end;
  return (size_t)parsed;
}

static void count_concurrent_message(
  ConcurrentRecords *records, const char *message
) {
  const size_t worker = read_concurrent_number(&message, "worker=");
  const size_t index = read_concurrent_number(&message, " message=");
  support_assert_true(*message == '\0', "corrupted concurrent message");
  if (
    worker >= (size_t)CONCURRENT_ROUND_COUNT * WORKER_COUNT ||
    index >= MESSAGES_PER_WORKER
  ) {
    support_fail("unexpected concurrent message identity");
  }
  support_assert_true(!records->seen[worker][index], "duplicate log message");
  records->seen[worker][index] = true;
  records->written++;
}

static void count_concurrent_drops(
  ConcurrentRecords *records, const char *message
) {
  const size_t dropped = read_concurrent_number(
    &message, "messages dropped count="
  );
  support_assert_string_equal(
    message, " reason=backpressure", "corrupted dropped-message summary"
  );
  support_assert_true(
    dropped > 0 && dropped <= (size_t)CONCURRENT_ROUND_COUNT * WORKER_COUNT *
                                MESSAGES_PER_WORKER,
    "invalid dropped-message count"
  );
  records->dropped += dropped;
}

static void assert_concurrent_accounting(const ConcurrentRecords *records) {
  support_assert_true(records->written > 0, "no concurrent messages written");
  support_assert_true(
    records->written + records->dropped ==
      (size_t)CONCURRENT_ROUND_COUNT * WORKER_COUNT * MESSAGES_PER_WORKER,
    "concurrent messages lost without an accurate summary"
  );
}

static void test_concurrent_messages(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_concurrent_messages);

  // Assert
  ConcurrentRecords records = {0};
  char *save_pointer = nullptr;
  char *line = strtok_r(output, "\n", &save_pointer);
  while (line) {
    support_assert_true(has_timestamp_shape(line), "concurrent timestamp");
    static const char prefix[] = "INFO concurrency: ";
    static const char drop_prefix[] = "WARNING logger: ";
    const char *record = line + 25;
    if (strncmp(record, prefix, sizeof(prefix) - 1) == 0) {
      count_concurrent_message(&records, record + sizeof(prefix) - 1);
    } else {
      support_assert_true(
        strncmp(record, drop_prefix, sizeof(drop_prefix) - 1) == 0,
        "interleaved concurrent log line"
      );
      count_concurrent_drops(&records, record + sizeof(drop_prefix) - 1);
    }
    line = strtok_r(nullptr, "\n", &save_pointer);
  }
  assert_concurrent_accounting(&records);

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void test_json_concurrent_messages(void) {
  // Arrange
  init_json_logger();

  // Act
  char *output = support_capture_standard_error(emit_concurrent_messages);
  pg_status_log_shutdown();

  // Assert
  char *cursor = output;
  ConcurrentRecords records = {0};
  while (*cursor) {
    cJSON *record = parse_next_json_record(&cursor);
    const char *component =
      cJSON_GetObjectItemCaseSensitive(record, "component")->valuestring;
    const char *message =
      cJSON_GetObjectItemCaseSensitive(record, "message")->valuestring;
    if (strcmp(component, "concurrency") == 0) {
      assert_json_string(record, "levelStr", "INFO");
      count_concurrent_message(&records, message);
    } else {
      assert_json_string(record, "component", "logger");
      assert_json_string(record, "levelStr", "WARNING");
      count_concurrent_drops(&records, message);
    }
    cJSON_Delete(record);
  }
  assert_concurrent_accounting(&records);

  // Cleanup
  free(output);
}

enum { ORDERED_MESSAGE_COUNT = 16 };

static void emit_ordered_messages(void) {
  for (size_t i = 0; i < ORDERED_MESSAGE_COUNT; i++) {
    pg_status_log(PG_STATUS_LOG_INFO, "order", "sequence=%zu", i);
  }
  pg_status_log_flush();
}

static void test_ordered_messages(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_ordered_messages);

  // Assert
  const char *position = output;
  for (size_t i = 0; i < ORDERED_MESSAGE_COUNT; i++) {
    char expected[64];
    const int length = snprintf(
      expected, sizeof(expected), " INFO order: sequence=%zu\n", i
    );
    support_assert_true(
      length > 0 && (size_t)length < sizeof(expected),
      "ordered log expectation is too large"
    );
    const char *match = strstr(position, expected);
    if (match == nullptr) {
      support_fail("asynchronous log order");
    }
    position = match + (size_t)length;
  }

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static void emit_message_and_shutdown(void) {
  pg_status_log(PG_STATUS_LOG_INFO, "shutdown", "last queued message");
  pg_status_log_shutdown();
}

static void test_shutdown_flushes(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_message_and_shutdown);

  // Assert
  support_assert_contains(
    output, " INFO shutdown: last queued message\n",
    "shutdown did not flush queued log"
  );

  // Cleanup
  free(output);
}

static void test_json_shutdown_flushes(void) {
  // Arrange
  init_json_logger();

  // Act
  char *output = support_capture_standard_error(emit_message_and_shutdown);

  // Assert
  cJSON *record = parse_json_record(output);
  assert_json_string(record, "component", "shutdown");
  assert_json_string(record, "message", "last queued message");

  // Cleanup
  cJSON_Delete(record);
  free(output);
}

static atomic_bool lifecycle_workers_should_stop;
static atomic_uint_fast64_t lifecycle_operation_count;

static void pause_lifecycle_thread(void) {
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000};
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
}

static void *run_lifecycle_log_worker(void *argument) {
  const size_t worker = *(const size_t *)argument;
  size_t message = 0;
  while (!atomic_load_explicit(
    &lifecycle_workers_should_stop, memory_order_relaxed
  )) {
    pg_status_log(
      PG_STATUS_LOG_INFO, "lifecycle", "worker=%zu message=%zu", worker, message
    );
    message++;
    atomic_fetch_add_explicit(
      &lifecycle_operation_count, 1, memory_order_relaxed
    );
    pause_lifecycle_thread();
  }
  return nullptr;
}

static void *run_lifecycle_flush_worker(void *argument) {
  (void)argument;
  while (!atomic_load_explicit(
    &lifecycle_workers_should_stop, memory_order_relaxed
  )) {
    pg_status_log_flush();
    atomic_fetch_add_explicit(
      &lifecycle_operation_count, 1, memory_order_relaxed
    );
    pause_lifecycle_thread();
  }
  return nullptr;
}

static void test_concurrent_lifecycle(void) {
  enum {
    lifecycle_worker_count = 3,
    lifecycle_round_count = 8,
    operations_per_round = 100,
  };

  // Arrange
  const int saved_stderr = save_standard_error();
  const int null_descriptor = open("/dev/null", O_WRONLY);
  if (null_descriptor < 0) {
    support_fail("failed to open /dev/null");
  }
  redirect_standard_error(null_descriptor);
  close(null_descriptor);
  atomic_store_explicit(
    &lifecycle_workers_should_stop, false, memory_order_relaxed
  );
  atomic_store_explicit(&lifecycle_operation_count, 0, memory_order_relaxed);

  pthread_t log_threads[lifecycle_worker_count];
  size_t workers[lifecycle_worker_count];
  for (size_t i = 0; i < lifecycle_worker_count; i++) {
    workers[i] = i;
    const int created = pthread_create(
      &log_threads[i], nullptr, run_lifecycle_log_worker, &workers[i]
    );
    support_assert_true(created == 0, "failed to create lifecycle log worker");
  }
  pthread_t flush_thread;
  const int flush_worker_created = pthread_create(
    &flush_thread, nullptr, run_lifecycle_flush_worker, nullptr
  );
  support_assert_true(
    flush_worker_created == 0, "failed to create lifecycle flush worker"
  );

  // Act
  for (size_t round = 0; round < lifecycle_round_count; round++) {
    pg_status_log_init();
    const uint_fast64_t target =
      atomic_load_explicit(&lifecycle_operation_count, memory_order_relaxed) +
      operations_per_round;
    while (
      atomic_load_explicit(&lifecycle_operation_count, memory_order_relaxed) <
      target) {
      pause_lifecycle_thread();
    }
    pg_status_log_shutdown();
  }

  // Cleanup
  atomic_store_explicit(
    &lifecycle_workers_should_stop, true, memory_order_relaxed
  );
  for (size_t i = 0; i < lifecycle_worker_count; i++) {
    support_assert_true(
      pthread_join(log_threads[i], nullptr) == 0,
      "failed to join lifecycle log worker"
    );
  }
  support_assert_true(
    pthread_join(flush_thread, nullptr) == 0,
    "failed to join lifecycle flush worker"
  );
  pg_status_log_shutdown();
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_true(
    atomic_load_explicit(&lifecycle_operation_count, memory_order_relaxed) >=
      (uint_fast64_t)lifecycle_round_count * operations_per_round,
    "logger lifecycle workers did not make progress"
  );
}

static void test_json_concurrent_lifecycle(void) {
  // Arrange
  init_json_logger();
  pg_status_log_shutdown();

  // Act & Assert
  test_concurrent_lifecycle();
}

static void test_closed_output(void) {
  // Arrange
  const int saved_stderr = save_standard_error();
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    support_fail("failed to create output pipe");
  }
  close(output_pipe[0]);
  redirect_standard_error(output_pipe[1]);
  close(output_pipe[1]);

  // Act
  pg_status_log_init();
  pg_status_log(PG_STATUS_LOG_INFO, "output", "closed pipe");
  pg_status_log_flush();
  pg_status_log_shutdown();

  // Cleanup
  restore_standard_error(saved_stderr);

  // Assert
  // Reaching this point proves that SIGPIPE did not terminate the process and
  // that flush and shutdown completed after the write failure.
}

static void test_missing_standard_error(void) {
  // Arrange
  const int saved_stderr = save_standard_error();
  close(STDERR_FILENO);

  // Act
  pg_status_log_init();
  pg_status_log(PG_STATUS_LOG_INFO, "output", "missing stderr");
  pg_status_log_flush();
  pg_status_log_shutdown();

  // Cleanup
  restore_standard_error(saved_stderr);

  // Assert
  // Reaching this point proves that logger initialization supplied a safe
  // /dev/null fallback instead of reusing descriptor 2 for its wake pipe.
}

static void handle_test_sigpipe(const int signal_number) {
  (void)signal_number;
}

static void test_output_configuration_restored(void) {
  // Arrange
  const int saved_stderr = save_standard_error();
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    support_fail("failed to create output pipe");
  }
  redirect_standard_error(output_pipe[1]);
  close(output_pipe[1]);

  struct sigaction test_sigpipe_action = {
    .sa_handler = handle_test_sigpipe,
  };
  if (sigemptyset(&test_sigpipe_action.sa_mask) != 0) {
    support_fail("failed to initialize test SIGPIPE handler");
  }
  struct sigaction original_sigpipe_action;
  if (sigaction(SIGPIPE, &test_sigpipe_action, &original_sigpipe_action) != 0) {
    support_fail("failed to install test SIGPIPE handler");
  }
  const int original_flags = fcntl(STDERR_FILENO, F_GETFL);
  if (original_flags < 0) {
    support_fail("failed to read original stderr flags");
  }

  // Act
  pg_status_log_init();
  const int configured_flags = fcntl(STDERR_FILENO, F_GETFL);
  struct sigaction configured_sigpipe_action;
  if (sigaction(SIGPIPE, nullptr, &configured_sigpipe_action) != 0) {
    support_fail("failed to read configured SIGPIPE handler");
  }
  pg_status_log_shutdown();
  const int restored_flags = fcntl(STDERR_FILENO, F_GETFL);
  struct sigaction restored_sigpipe_action;
  if (sigaction(SIGPIPE, nullptr, &restored_sigpipe_action) != 0) {
    support_fail("failed to read restored SIGPIPE handler");
  }

  // Cleanup
  if (sigaction(SIGPIPE, &original_sigpipe_action, nullptr) != 0) {
    support_fail("failed to restore original SIGPIPE handler");
  }
  close(output_pipe[0]);
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_true(configured_flags >= 0, "configured stderr flags missing");
  support_assert_true(
    (configured_flags & O_NONBLOCK) != 0,
    "logger did not configure non-blocking stderr"
  );
  support_assert_true(
    configured_sigpipe_action.sa_handler == SIG_IGN,
    "logger did not ignore SIGPIPE"
  );
  support_assert_true(
    restored_flags == original_flags, "logger did not restore stderr flags"
  );
  support_assert_true(
    restored_sigpipe_action.sa_handler == handle_test_sigpipe,
    "logger did not restore the SIGPIPE handler"
  );
}

static void fill_standard_error_pipe(void) {
  char block[4096];
  memset(block, 'x', sizeof(block));
  for (;;) {
    const ssize_t written = write(STDERR_FILENO, block, sizeof(block));
    if (written > 0) {
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    support_fail("failed to fill stderr pipe");
  }
}

static void drain_pipe(const int descriptor) {
  char buffer[4096];
  for (;;) {
    const ssize_t bytes_read = read(descriptor, buffer, sizeof(buffer));
    if (bytes_read > 0) {
      continue;
    }
    if (bytes_read < 0 && errno == EINTR) {
      continue;
    }
    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    support_fail("failed to drain stderr pipe");
  }
}

static size_t read_pipe(
  const int descriptor, char *buffer, const size_t capacity
) {
  size_t used = 0;
  while (used + 1 < capacity) {
    const ssize_t bytes_read = read(
      descriptor, buffer + used, capacity - used - 1
    );
    if (bytes_read > 0) {
      used += (size_t)bytes_read;
      continue;
    }
    if (bytes_read < 0 && errno == EINTR) {
      continue;
    }
    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    support_fail("failed to read stderr pipe");
  }
  buffer[used] = '\0';
  return used;
}

static bool wait_for_pipe_data(const int descriptor) {
  struct pollfd output_poll = {
    .fd = descriptor,
    .events = POLLIN,
    .revents = 0,
  };
  int result;
  do {
    result = poll(&output_poll, 1, 2000);
  } while (result < 0 && errno == EINTR);
  return result > 0 && (output_poll.revents & POLLIN) != 0;
}

static bool standard_error_is_writable(void) {
  struct pollfd output_poll = {
    .fd = STDERR_FILENO,
    .events = POLLOUT,
    .revents = 0,
  };
  int result;
  do {
    result = poll(&output_poll, 1, 0);
  } while (result < 0 && errno == EINTR);
  support_assert_true(result >= 0, "failed to poll stderr writability");
  return result > 0 && (output_poll.revents & POLLOUT) != 0;
}

static void make_standard_error_barely_writable(const int reader) {
  char buffer[128];
  while (!standard_error_is_writable()) {
    const ssize_t bytes_read = read(reader, buffer, sizeof(buffer));
    if (bytes_read > 0) {
      continue;
    }
    if (bytes_read < 0 && errno == EINTR) {
      continue;
    }
    support_fail("failed to make stderr barely writable");
  }
}

static void test_output_backpressure_recovery(void) {
  // Arrange
  const int saved_stderr = save_standard_error();
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    support_fail("failed to create output pipe");
  }
  const int read_flags = fcntl(output_pipe[0], F_GETFL);
  if (read_flags < 0) {
    support_fail("failed to read output pipe flags");
  }
  if (fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
    support_fail("failed to make output pipe non-blocking");
  }
  redirect_standard_error(output_pipe[1]);
  close(output_pipe[1]);
  pg_status_log_init();
  fill_standard_error_pipe();

  // Act
  pg_status_log(PG_STATUS_LOG_INFO, "output", "message to drop");
  pg_status_log_flush();
  drain_pipe(output_pipe[0]);
  pg_status_log(PG_STATUS_LOG_INFO, "output", "message after recovery");
  pg_status_log_flush();
  char output[8192];
  const size_t output_length = read_pipe(
    output_pipe[0], output, sizeof(output)
  );

  // Cleanup
  pg_status_log_shutdown();
  close(output_pipe[0]);
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_true(output_length > 0, "logger did not resume output");
  support_assert_contains(
    output, " INFO output: message after recovery\n",
    "message after output recovery"
  );
  support_assert_contains(
    output, " WARNING logger: messages dropped count=1 reason=backpressure\n",
    "dropped-message summary after output recovery"
  );
}

static void check_output_backpressure_periodic_recovery(const bool json) {
  // Arrange
  const int saved_stderr = save_standard_error();
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    support_fail("failed to create output pipe");
  }
  const int read_flags = fcntl(output_pipe[0], F_GETFL);
  if (read_flags < 0) {
    support_fail("failed to read output pipe flags");
  }
  if (fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
    support_fail("failed to make output pipe non-blocking");
  }
  redirect_standard_error(output_pipe[1]);
  close(output_pipe[1]);
  pg_status_log_init();
  fill_standard_error_pipe();

  // Act
  pg_status_log(PG_STATUS_LOG_INFO, "output", "message to drop");
  pg_status_log_flush();
  drain_pipe(output_pipe[0]);
  const bool recovery_ready = wait_for_pipe_data(output_pipe[0]);
  char output[8192];
  const size_t output_length = read_pipe(
    output_pipe[0], output, sizeof(output)
  );

  // Cleanup
  pg_status_log_shutdown();
  close(output_pipe[0]);
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_true(recovery_ready, "logger output recovery timed out");
  support_assert_true(output_length > 0, "logger did not retry output");
  if (json) {
    cJSON *record = parse_json_record(output);
    assert_json_string(record, "levelStr", "WARNING");
    assert_json_string(
      record, "message", "messages dropped count=1 reason=backpressure"
    );
    assert_json_string(record, "component", "logger");
    cJSON_Delete(record);
  } else {
    support_assert_contains(
      output, " WARNING logger: messages dropped count=1 reason=backpressure\n",
      "periodic dropped-message summary"
    );
  }
}

static void test_output_backpressure_periodic_recovery(void) {
  // Act & Assert
  check_output_backpressure_periodic_recovery(false);
}

static void test_json_backpressure_recovery(void) {
  // Arrange
  support_set_environment("pg_status__log_format", "json");

  // Act & Assert
  check_output_backpressure_periodic_recovery(true);
}

static void check_output_partial_write_recovery(const bool json) {
  if (json) {
    support_set_environment("pg_status__log_format", "json");
  }
  // Arrange
  const int saved_stderr = save_standard_error();
  int output_socket[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, output_socket) != 0) {
    support_fail("failed to create output socket");
  }
  const int send_buffer_size = 1024;
  if (
    setsockopt(
      output_socket[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
      sizeof(send_buffer_size)
    ) != 0
  ) {
    support_fail("failed to configure output socket buffer");
  }
  const int read_flags = fcntl(output_socket[1], F_GETFL);
  if (read_flags < 0) {
    support_fail("failed to read output socket flags");
  }
  if (fcntl(output_socket[1], F_SETFL, read_flags | O_NONBLOCK) != 0) {
    support_fail("failed to make output socket non-blocking");
  }
  redirect_standard_error(output_socket[0]);
  close(output_socket[0]);
  pg_status_log_init();
  fill_standard_error_pipe();
  make_standard_error_barely_writable(output_socket[1]);

  char component[5000];
  memset(component, 'p', sizeof(component) - 1);
  static const char component_prefix[] = "partial-record";
  memcpy(component, component_prefix, sizeof(component_prefix) - 1);
  component[sizeof(component) - 1] = '\0';

  // Act
  pg_status_log(PG_STATUS_LOG_INFO, component, "message");
  pg_status_log_flush();
  // Until the retained tail is complete, no newer record may be started.
  pg_status_log(PG_STATUS_LOG_INFO, "output", "must-not-interleave");
  pg_status_log_flush();
  char output[73728];
  size_t used = read_pipe(output_socket[1], output, sizeof(output));
  char *record = output + strspn(output, "x");  // Discard socket-filling bytes.
  const bool partial_write = strchr(record, '\n') == nullptr;
  while (strchr(record, '\n') == nullptr) {
    support_assert_true(
      wait_for_pipe_data(output_socket[1]), "partial record retry timed out"
    );
    if (used + 1 >= sizeof(output)) {
      support_fail("partial record exceeded capture buffer");
    }
    used += read_pipe(output_socket[1], output + used, sizeof(output) - used);
  }
  pg_status_log(PG_STATUS_LOG_INFO, "output", "message after recovery");
  pg_status_log_flush();
  if (used + 1 >= sizeof(output)) {
    support_fail("recovery exceeded capture buffer");
  }
  (void)read_pipe(output_socket[1], output + used, sizeof(output) - used);

  // Cleanup
  pg_status_log_shutdown();
  close(output_socket[1]);
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_contains(
    record, "message after recovery", "output did not recover"
  );
  if (partial_write) {
    support_assert_not_contains(
      record, "must-not-interleave", "new record interleaved with retained tail"
    );
  }
  if (json) {
    char *cursor = record;
    cJSON *first = parse_next_json_record(&cursor);
    assert_json_string(first, "message", "message");
    assert_json_truncated(first, "component");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(first, "component");
    support_assert_contains(
      name->valuestring, "partial-record", "JSON prefix was lost"
    );
    cJSON_Delete(first);
    while (*cursor) {
      cJSON *next = parse_next_json_record(&cursor);
      cJSON_Delete(next);
    }
  } else {
    const char *newline = strchr(record, '\n');
    if (!newline) {
      support_fail("missing completed text record");
    }
    support_assert_true(
      (size_t)(newline - record) == 4094, "text record tail was lost"
    );
    support_assert_contains(
      record, " INFO partial-record", "text prefix was lost"
    );
    static const char suffix[] = "...[truncated]";
    support_assert_true(
      memcmp(newline - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0,
      "text tail was not restored"
    );
  }
}

static void test_output_partial_write_recovery(void) {
  // Act & Assert
  check_output_partial_write_recovery(false);
}

static void test_json_partial_write_recovery(void) {
  // Act & Assert
  check_output_partial_write_recovery(true);
}

static atomic_bool fatal_shutdown_started;

static void *run_fatal_shutdown_worker(void *argument) {
  (void)argument;
  atomic_store_explicit(&fatal_shutdown_started, true, memory_order_release);
  pg_status_log_shutdown();
  return nullptr;
}

static void run_fatal_shutdown_child(void) {
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    support_fail("failed to create fatal output pipe");
  }
  close(output_pipe[0]);
  redirect_standard_error(output_pipe[1]);
  close(output_pipe[1]);

  struct sigaction default_sigpipe_action = {
    .sa_handler = SIG_DFL,
  };
  if (
    sigemptyset(&default_sigpipe_action.sa_mask) != 0 ||
    sigaction(SIGPIPE, &default_sigpipe_action, nullptr) != 0
  ) {
    support_fail("failed to restore default SIGPIPE action");
  }

  pg_status_log_init();
  atomic_store_explicit(&fatal_shutdown_started, false, memory_order_relaxed);
  pthread_t shutdown_thread;
  const int created = pthread_create(
    &shutdown_thread, nullptr, run_fatal_shutdown_worker, nullptr
  );
  support_assert_true(created == 0, "failed to create shutdown worker");
  support_assert_true(
    pthread_detach(shutdown_thread) == 0, "failed to detach shutdown worker"
  );
  while (!atomic_load_explicit(&fatal_shutdown_started, memory_order_acquire)) {
  }

  struct timespec delay = {
    .tv_sec = 0,
    .tv_nsec = 1000000,
  };
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
  pg_status_log_fatal("fatal", "concurrent shutdown");
}

static void test_fatal_shutdown_race(void) {
  enum { round_count = 8 };

  // Arrange & Act
  for (size_t round = 0; round < round_count; round++) {
    const pid_t child = fork();
    if (child < 0) {
      support_fail("failed to fork fatal logger test");
    }
    if (child == 0) {
      execl(
        logger_test_program, logger_test_program, "fatal_shutdown_child",
        (char *)nullptr
      );
      _exit(127);
    }

    int status;
    pid_t waited;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    // Assert
    support_assert_true(
      waited == child, "failed to wait for fatal logger test"
    );
    support_assert_true(
      !WIFSIGNALED(status) || WTERMSIG(status) != SIGPIPE,
      "fatal logger was terminated by SIGPIPE"
    );
    support_assert_true(
      WIFEXITED(status), "fatal logger was terminated by an unexpected signal"
    );
    support_assert_true(
      WEXITSTATUS(status) == EXIT_FAILURE, "unexpected fatal logger exit status"
    );
  }
}

static void test_fatal(void) {
  // Arrange & Act
  pg_status_log_fatal("test", "fatal message value=%d", 42);
}

static void test_fatal_flushes_queue(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);
  pg_status_log(PG_STATUS_LOG_INFO, "fatal", "queued before fatal");

  // Act
  pg_status_log_fatal("fatal", "fatal after queued message");
}

static void test_invalid_enum_log(void) {
  // Arrange & Act
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  pg_status_log((PGStatusLogLevel)100, "test", "invalid level");
}

static void test_invalid_enum_set(void) {
  // Arrange & Act
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  pg_status_log_set_level((PGStatusLogLevel)100);
}

static const char *json_failure_case;

static void run_json_failure_child(void) {
  const pid_t child = fork();
  support_assert_true(child >= 0, "failed to fork JSON failure test");
  if (child == 0) {
    execl(
      logger_test_program, logger_test_program, json_failure_case,
      (char *)nullptr
    );
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  support_assert_true(waited == child, "failed to wait for JSON failure test");
  const bool expected_abort = strcmp(
                                json_failure_case, "json_invalid_enum_child"
                              ) == 0;
  support_assert_true(
    expected_abort ? WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT
                   : WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE,
    "unexpected JSON failure exit status"
  );
}

static void test_json_failure_paths(void) {
  // Arrange
  const char *cases[] = {
    "json_fatal_child", "json_invalid_level_child", "json_invalid_enum_child"
  };
  const char *messages[] = {
    "fatal after queued message", "invalid pg_status__log_level",
    "invalid log level value=100"
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    json_failure_case = cases[i];

    // Act
    char *output = support_capture_standard_error(run_json_failure_child);

    // Assert
    char *cursor = output;
    if (i == 0) {
      cJSON *queued = parse_next_json_record(&cursor);
      assert_json_string(queued, "message", "queued before fatal");
      cJSON_Delete(queued);
    }
    cJSON *record = parse_json_record(cursor);
    assert_json_string(record, "levelStr", "ERROR");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(record, "message");
    support_assert_true(cJSON_IsString(message), "missing fatal JSON message");
    support_assert_contains(
      message->valuestring, messages[i], "wrong fatal JSON message"
    );

    // Cleanup
    cJSON_Delete(record);
    free(output);
  }
}

static void json_fatal_child(void) {
  support_set_environment("pg_status__log_format", "json");
  test_fatal_flushes_queue();
}

static void json_invalid_level_child(void) {
  support_set_environment("pg_status__log_format", "json");
  test_invalid_level();
}

static void json_invalid_enum_child(void) {
  init_json_logger();
  test_invalid_enum_log();
}

static const struct {
  const char *name;
  logger_test_function_t function;
} test_cases[] = {
  {"message_format", test_message_format},
  {"text_format_environment", test_text_format_environment},
  {"json_format", test_json_format},
  {"json_escape_roundtrip", test_json_escape_roundtrip},
  {"json_unicode_boundaries", test_json_unicode_boundaries},
  {"json_errno_boundaries", test_json_errno_boundaries},
  {"json_exact_capacity", test_json_exact_capacity},
  {"json_shutdown_flushes", test_json_shutdown_flushes},
  {"json_concurrent_lifecycle", test_json_concurrent_lifecycle},
  {"json_truncation", test_json_truncation},
  {"json_fitting_values_preserved", test_json_fitting_values_preserved},
  {"json_retry_truncation", test_json_retry_truncation},
  {"json_utf8_truncation", test_json_utf8_truncation},
  {"json_small_output_buffer", test_json_small_output_buffer},
  {"json_invalid_utf8", test_json_invalid_utf8},
  {"json_concurrent_messages", test_json_concurrent_messages},
  {"json_backpressure_recovery", test_json_backpressure_recovery},
  {"json_partial_write_recovery", test_json_partial_write_recovery},
  {"json_failure_paths", test_json_failure_paths},
  {"json_fatal_child", json_fatal_child},
  {"json_invalid_level_child", json_invalid_level_child},
  {"json_invalid_enum_child", json_invalid_enum_child},
  {"invalid_format", test_invalid_format},
  {"level_filtering", test_level_filtering},
  {"init_from_environment", test_init_from_environment},
  {"invalid_level", test_invalid_level},
  {"system_error", test_system_error},
  {"errno_preserved", test_errno_preserved},
  {"multiline_sanitized", test_multiline_sanitized},
  {"long_message_truncated", test_long_message_truncated},
  {"long_component_truncated", test_long_component_truncated},
  {"concurrent_messages", test_concurrent_messages},
  {"ordered_messages", test_ordered_messages},
  {"shutdown_flushes", test_shutdown_flushes},
  {"concurrent_lifecycle", test_concurrent_lifecycle},
  {"closed_output", test_closed_output},
  {"missing_standard_error", test_missing_standard_error},
  {"output_configuration_restored", test_output_configuration_restored},
  {"output_backpressure_recovery", test_output_backpressure_recovery},
  {"output_backpressure_periodic_recovery",
   test_output_backpressure_periodic_recovery},
  {"output_partial_write_recovery", test_output_partial_write_recovery},
  {"fatal_shutdown_race", test_fatal_shutdown_race},
  {"fatal_shutdown_child", run_fatal_shutdown_child},
  {"fatal", test_fatal},
  {"fatal_flushes_queue", test_fatal_flushes_queue},
  {"invalid_enum_log", test_invalid_enum_log},
  {"invalid_enum_set", test_invalid_enum_set},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }

  logger_test_program = argv[0];
  support_clear_environment("pg_status__log_format");

  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("logger_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }

  support_fail("unknown test case name");
}
