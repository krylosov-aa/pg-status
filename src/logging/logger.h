#ifndef PG_STATUS_LOGGER_H
#define PG_STATUS_LOGGER_H

typedef enum {
  PG_STATUS_LOG_DEBUG,
  PG_STATUS_LOG_INFO,
  PG_STATUS_LOG_WARNING,
  PG_STATUS_LOG_ERROR,
  PG_STATUS_LOG_FATAL,
} PGStatusLogLevel;

/**
 * Configures logging from pg_status__log_level (default: info) and
 * pg_status__log_format (text or json, default: text), then starts the
 * asynchronous writer. Invalid values terminate the process.
 */
void pg_status_log_init(void);

/**
 * Waits until all messages queued before this call have been processed by the
 * writer. A message can be dropped if the output is unavailable; a partially
 * written record can remain pending. This is not a delivery guarantee.
 * The duration can depend on an operating-system write already in progress.
 */
void pg_status_log_flush(void);

/** Processes the queue and stops the writer; unavailable output may lose logs.
 */
void pg_status_log_shutdown(void);

void pg_status_log_set_level(PGStatusLogLevel level);
PGStatusLogLevel pg_status_log_get_level(void);

/**
 * Captures the message and owned component before returning. Timestamp text,
 * error descriptions, and TXT/JSON records are formatted by the writer.
 * Ordinary calls never wait for output, queue space, or a logger mutex.
 * Admission attempts are bounded; overloaded or stopped loggers drop new
 * messages. vsnprintf still runs in the caller and can take time for large
 * arguments.
 */
[[gnu::format(printf, 3, 4)]]
void pg_status_log(
  PGStatusLogLevel level, const char *component, const char *format, ...
);

[[gnu::format(printf, 4, 5)]]
void pg_status_log_system_error(
  PGStatusLogLevel level, const char *component, int error_number,
  const char *format, ...
);

[[noreturn, gnu::format(printf, 2, 3)]]
void pg_status_log_fatal(const char *component, const char *format, ...);

[[noreturn, gnu::format(printf, 3, 4)]]
void pg_status_log_system_fatal(
  const char *component, int error_number, const char *format, ...
);

#endif  // PG_STATUS_LOGGER_H
