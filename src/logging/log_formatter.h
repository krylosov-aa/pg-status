#ifndef PG_STATUS_LOG_FORMATTER_H
#define PG_STATUS_LOG_FORMATTER_H

#include <stddef.h>

#include "logger.h"

/* Internal interface; entry strings remain valid for the formatter call. */
typedef struct {
  const char *timestamp;
  PGStatusLogLevel level;
  const char *level_name;
  const char *component;
  const char *message;
  const int *error_number;
} PGStatusLogEntry;

/* Return the byte count including the final newline, excluding the NUL. */
typedef size_t (*PGStatusLogFormatter)(
  char *line, size_t capacity, const PGStatusLogEntry *entry
);

size_t pg_status_format_text(
  char *line, size_t capacity, const PGStatusLogEntry *entry
);
size_t pg_status_format_json(
  char *line, size_t capacity, const PGStatusLogEntry *entry
);

#endif  // PG_STATUS_LOG_FORMATTER_H
