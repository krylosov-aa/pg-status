/** Single-line text format. */

#include <stdio.h>
#include <string.h>

#include "log_formatter.h"

static void sanitize_log_line(char *line, const size_t length) {
  for (size_t i = 0; i + 1 < length; i++) {
    const unsigned char character = (unsigned char)line[i];
    if (character < 0x20 || character == 0x7f) {
      line[i] = ' ';
    }
  }
}

static size_t mark_line_truncated(char *line, const size_t capacity) {
  static const char marker[] = "...[truncated]";
  const size_t marker_length = sizeof(marker) - 1;
  if (capacity <= marker_length + 1) {
    return 0;
  }
  const size_t marker_position = capacity - marker_length - 2;
  memcpy(line + marker_position, marker, marker_length);
  line[capacity - 2] = '\n';
  line[capacity - 1] = '\0';
  return capacity - 1;
}

size_t pg_status_format_text(
  char *line, const size_t capacity, const PGStatusLogEntry *entry
) {
  const int line_length = snprintf(
    line, capacity, "%s %s %s: %s\n", entry->timestamp, entry->level_name,
    entry->component, entry->message
  );
  if (line_length < 0) {
    return 0;
  }
  if ((size_t)line_length >= capacity) {
    const size_t length = mark_line_truncated(line, capacity);
    sanitize_log_line(line, length);
    return length;
  }
  const size_t length = (size_t)line_length;
  sanitize_log_line(line, length);
  return length;
}
