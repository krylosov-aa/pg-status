/** Fixed-schema JSON logs, written directly into the caller's buffer. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "log_formatter.h"

static const char TRUNCATION_MARKER[] = "...[truncated]";

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool failed;
} JsonWriter;

/* A view of the original string; shortening never modifies the input. */
typedef struct {
  const char *data;
  size_t length;
  bool truncated;
} JsonText;

static JsonText full_text(const char *text) {
  return (JsonText){.data = text, .length = strlen(text)};
}

static void append_bytes(
  JsonWriter *out, const char *data, const size_t length
) {
  if (out->failed) {
    return;
  }
  /* Keep one byte for the NUL terminator. */
  if (length >= out->capacity - out->length) {
    out->failed = true;
    return;
  }
  memcpy(out->data + out->length, data, length);
  out->length += length;
  out->data[out->length] = '\0';
}

static void append_literal(JsonWriter *out, const char *text) {
  append_bytes(out, text, strlen(text));
}

static bool is_utf8_continuation(const unsigned char byte) {
  /* Continuation bytes have the binary prefix 10xxxxxx. */
  return byte >= 0x80 && byte <= 0xbf;
}

typedef struct {
  unsigned char first_min;
  unsigned char first_max;
  unsigned char second_min;
  unsigned char second_max;
  size_t length;
} Utf8Pattern;

/* Legal UTF-8 byte ranges, including the boundaries of Unicode scalar values.
 * The second-byte restrictions reject overlong encodings and surrogates. */
static const Utf8Pattern UTF8_PATTERNS[] = {
  {0xc2, 0xdf, 0x80, 0xbf, 2},  // U+0080..U+07FF
  {0xe0, 0xe0, 0xa0, 0xbf, 3},  // U+0800..U+0FFF
  {0xe1, 0xec, 0x80, 0xbf, 3},  // U+1000..U+CFFF
  {0xed, 0xed, 0x80, 0x9f, 3},  // U+D000..U+D7FF (before surrogates)
  {0xee, 0xef, 0x80, 0xbf, 3},  // U+E000..U+FFFF (after surrogates)
  {0xf0, 0xf0, 0x90, 0xbf, 4},  // U+10000..U+3FFFF
  {0xf1, 0xf3, 0x80, 0xbf, 4},  // U+40000..U+FFFFF
  {0xf4, 0xf4, 0x80, 0x8f, 4},  // U+100000..U+10FFFF
};

static size_t utf8_character_length(
  const unsigned char *text, const size_t available
) {
  for (size_t i = 0; i < sizeof(UTF8_PATTERNS) / sizeof(UTF8_PATTERNS[0]);
       i++) {
    const Utf8Pattern pattern = UTF8_PATTERNS[i];
    if (text[0] < pattern.first_min || text[0] > pattern.first_max) {
      continue;
    }
    if (
      available < pattern.length || text[1] < pattern.second_min ||
      text[1] > pattern.second_max
    ) {
      return 0;
    }
    for (size_t j = 2; j < pattern.length; j++) {
      if (!is_utf8_continuation(text[j])) {
        return 0;
      }
    }
    return pattern.length;
  }
  return 0;
}

static void append_ascii(JsonWriter *out, const unsigned char byte) {
  static const char hex[] = "0123456789abcdef";
  static const char short_escapes[32] = {
    ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't',
  };
  if (byte == '"' || byte == '\\') {
    const char escaped[] = {'\\', (char)byte};
    append_bytes(out, escaped, sizeof(escaped));
  } else if (byte < 0x20 && short_escapes[byte]) {
    const char escaped[] = {'\\', short_escapes[byte]};
    append_bytes(out, escaped, sizeof(escaped));
  } else if (byte < 0x20) {
    const char escaped[] = {'\\',           'u',           '0', '0',
                            hex[byte >> 4], hex[byte & 15]};
    append_bytes(out, escaped, sizeof(escaped));
  } else {
    const char character = (char)byte;
    append_bytes(out, &character, 1);
  }
}

static size_t append_character(
  JsonWriter *out, const unsigned char *text, const size_t available
) {
  if (*text < 0x80) {
    append_ascii(out, *text);
    return 1;
  }
  const size_t length = utf8_character_length(text, available);
  if (length == 0) {
    append_literal(out, "\xef\xbf\xbd");  // U+FFFD: replace one invalid byte
    return 1;
  }
  append_bytes(out, (const char *)text, length);
  return length;
}

static void append_string(JsonWriter *out, const JsonText text) {
  append_literal(out, "\"");
  size_t position = 0;
  while (position < text.length && !out->failed) {
    position += append_character(
      out, (const unsigned char *)text.data + position, text.length - position
    );
  }
  if (text.truncated) {
    append_literal(out, TRUNCATION_MARKER);
  }
  append_literal(out, "\"");
}

/* Field names below are fixed ASCII constants, not caller-provided strings. */
static void append_field_name(JsonWriter *out, const char *name) {
  if (out->length > 1) {
    append_literal(out, ",");
  }
  append_literal(out, "\"");
  append_literal(out, name);
  append_literal(out, "\":");
}

static void append_string_field(
  JsonWriter *out, const char *name, const JsonText text
) {
  append_field_name(out, name);
  append_string(out, text);
}

static void append_errno(JsonWriter *out, const int error_number) {
  char number[32];
  const int length = snprintf(number, sizeof(number), "%d", error_number);
  if (length < 0 || (size_t)length >= sizeof(number)) {
    out->failed = true;
    return;
  }
  append_field_name(out, "errno");
  append_bytes(out, number, (size_t)length);
}

static void append_record(
  JsonWriter *out, const PGStatusLogEntry *entry, const JsonText message,
  const JsonText component
) {
  const char *level = entry->level == PG_STATUS_LOG_FATAL ? "ERROR"
                                                          : entry->level_name;
  append_literal(out, "{");
  append_string_field(out, "@timestamp", full_text(entry->timestamp));
  append_string_field(out, "levelStr", full_text(level));
  append_string_field(out, "component", component);
  append_string_field(out, "message", message);
  if (entry->error_number) {
    append_errno(out, *entry->error_number);
  }
  append_literal(out, "}\n");
}

/* Every successful call reduces the output value, including its marker. */
static bool shorten_text(JsonText *text) {
  const size_t marker_length = sizeof(TRUNCATION_MARKER) - 1;
  if (text->truncated) {
    if (text->length == 0) {
      return false;
    }
    text->length /= 2;
  } else {
    if (text->length <= marker_length) {
      return false;
    }
    text->length = (text->length - marker_length) / 2;
  }
  while (text->length > 0 &&
         is_utf8_continuation((unsigned char)text->data[text->length])) {
    text->length--;
  }
  text->truncated = true;
  return true;
}

size_t pg_status_format_json(
  char *line, const size_t capacity, const PGStatusLogEntry *entry
) {
  if (capacity == 0) {
    return 0;
  }
  JsonText message = full_text(entry->message);
  JsonText component = full_text(entry->component);
  for (;;) {
    JsonWriter out = {.data = line, .capacity = capacity};
    append_record(&out, entry, message, component);
    if (!out.failed) {
      return out.length;
    }
    if (!shorten_text(&message) && !shorten_text(&component)) {
      line[0] = '\0';
      return 0;  // Never publish an incomplete JSON record.
    }
  }
}
