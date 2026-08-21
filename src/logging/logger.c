/** Bounded asynchronous process logger. */

#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
  LOG_MESSAGE_CAPACITY = 2048,
  LOG_LINE_CAPACITY = 4096,
  LOG_TIMESTAMP_CAPACITY = 32,
  LOG_QUEUE_CAPACITY = 256,
  LOG_ERROR_RESERVE = 16,
  LOG_WRITE_BATCH_SIZE = 64,
  LOG_OUTPUT_WAIT_TIMEOUT_MS = 10,
  LOG_OUTPUT_RETRY_INTERVAL_MS = 250,
};
static const char TRUNCATION_MARKER[] = "...[truncated]";

typedef struct {
  _Atomic size_t sequence;
  size_t length;
  char line[LOG_LINE_CAPACITY];
} LogQueueSlot;

typedef struct {
  size_t length;
  char line[LOG_LINE_CAPACITY];
} LogRecord;

typedef enum {
  LOGGER_STOPPED,
  LOGGER_RUNNING,
  LOGGER_STOPPING,
} LoggerState;

typedef enum {
  LOG_WRITE_COMPLETE,
  LOG_WRITE_FAILED,
  LOG_WRITE_PARTIAL,
} LogWriteResult;

static atomic_int minimum_level = PG_STATUS_LOG_INFO;
static atomic_int logger_state = LOGGER_STOPPED;
static atomic_bool stop_requested;
static atomic_size_t enqueue_position;
static atomic_size_t dequeue_position;
static atomic_size_t completed_position;
static atomic_uint_fast64_t dropped_log_count;
static atomic_size_t active_operation_count;
static atomic_bool output_backpressured;
static bool output_line_incomplete;

static LogQueueSlot log_queue[LOG_QUEUE_CAPACITY];
static pthread_t logger_thread;
static int wake_pipe[2] = {-1, -1};

static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lifecycle_condition = PTHREAD_COND_INITIALIZER;
static pthread_cond_t flush_condition = PTHREAD_COND_INITIALIZER;

static int stderr_status_flags;
static struct sigaction previous_sigpipe_action;

static LogWriteResult write_all(const char *buffer, size_t length);

[[noreturn]] static void internal_logger_error(
  const char *message, const int error_number
) {
  char output[192];
  const int length = snprintf(
    output, sizeof(output), "pg-status logger internal error: %s (error=%d)\n",
    message, error_number
  );
  if (length > 0) {
    const size_t bytes_to_write = (size_t)length < sizeof(output)
                                    ? (size_t)length
                                    : sizeof(output) - 1;
    (void)write_all(output, bytes_to_write);
  }
  abort();
}

[[noreturn]] static void invalid_log_level(const PGStatusLogLevel level) {
  char message[96];
  const int length = snprintf(
    message, sizeof(message),
    "pg-status logger internal error: invalid log level value=%d\n", (int)level
  );
  if (length > 0) {
    const size_t bytes_to_write = (size_t)length < sizeof(message)
                                    ? (size_t)length
                                    : sizeof(message) - 1;
    (void)write_all(message, bytes_to_write);
  }
  abort();
}

static void validate_log_level(const PGStatusLogLevel level) {
  switch (level) {
    case PG_STATUS_LOG_DEBUG:
    case PG_STATUS_LOG_INFO:
    case PG_STATUS_LOG_WARNING:
    case PG_STATUS_LOG_ERROR:
    case PG_STATUS_LOG_FATAL:
      return;
  }
  invalid_log_level(level);
}

static const char *level_name(const PGStatusLogLevel level) {
  switch (level) {
    case PG_STATUS_LOG_DEBUG:
      return "DEBUG";
    case PG_STATUS_LOG_INFO:
      return "INFO";
    case PG_STATUS_LOG_WARNING:
      return "WARNING";
    case PG_STATUS_LOG_ERROR:
      return "ERROR";
    case PG_STATUS_LOG_FATAL:
      return "FATAL";
  }
  invalid_log_level(level);
}

static bool should_log(const PGStatusLogLevel level) {
  return level >= (PGStatusLogLevel)atomic_load_explicit(
                    &minimum_level, memory_order_relaxed
                  );
}

static void format_timestamp(char *buffer, const size_t capacity) {
  struct timespec now;
  struct tm utc;
  if (
    clock_gettime(CLOCK_REALTIME, &now) != 0 ||
    gmtime_r(&now.tv_sec, &utc) == nullptr
  ) {
    (void)snprintf(buffer, capacity, "unknown-time");
    return;
  }

  char seconds[24];
  if (strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
    (void)snprintf(buffer, capacity, "unknown-time");
    return;
  }
  if (now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
    (void)snprintf(buffer, capacity, "unknown-time");
    return;
  }
  const unsigned int milliseconds = (unsigned int)(now.tv_nsec / 1000000L);
  (void)snprintf(buffer, capacity, "%s.%03uZ", seconds, milliseconds);
}

static void mark_truncated(char *buffer, const size_t capacity) {
  const size_t marker_length = sizeof(TRUNCATION_MARKER) - 1;
  if (capacity <= marker_length) {
    return;
  }
  const size_t marker_position = capacity - marker_length - 1;
  memcpy(buffer + marker_position, TRUNCATION_MARKER, marker_length + 1);
}

static void sanitize_log_line(char *line, const size_t length) {
  for (size_t i = 0; i + 1 < length; i++) {
    const unsigned char character = (unsigned char)line[i];
    if (character < 0x20 || character == 0x7f) {
      line[i] = ' ';
    }
  }
}

static size_t mark_line_truncated(char *line, const size_t capacity) {
  const size_t marker_length = sizeof(TRUNCATION_MARKER) - 1;
  if (capacity <= marker_length + 1) {
    return 0;
  }
  const size_t marker_position = capacity - marker_length - 2;
  memcpy(line + marker_position, TRUNCATION_MARKER, marker_length);
  line[capacity - 2] = '\n';
  line[capacity - 1] = '\0';
  return capacity - 1;
}

static void get_error_text(
  const int error_number, char *buffer, const size_t capacity
) {
  if (strerror_r(error_number, buffer, capacity) != 0) {
    (void)snprintf(buffer, capacity, "Unknown error");
  }
}

static bool wait_until_stderr_is_writable(const int timeout_ms) {
  struct pollfd output_poll = {
    .fd = STDERR_FILENO,
    .events = POLLOUT,
    .revents = 0,
  };
  const int result = poll(&output_poll, 1, timeout_ms);
  return result > 0 && (output_poll.revents & POLLOUT) != 0;
}

static LogWriteResult write_all(const char *buffer, const size_t length) {
  if (
    atomic_load_explicit(&output_backpressured, memory_order_relaxed) &&
    !wait_until_stderr_is_writable(0)
  ) {
    return LOG_WRITE_FAILED;
  }

  atomic_store_explicit(&output_backpressured, false, memory_order_relaxed);
  size_t written = 0;
  while (written < length) {
    const ssize_t result = write(
      STDERR_FILENO, buffer + written, length - written
    );
    if (result > 0) {
      written += (size_t)result;
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (
      result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
      wait_until_stderr_is_writable(LOG_OUTPUT_WAIT_TIMEOUT_MS)
    ) {
      continue;
    }
    atomic_store_explicit(&output_backpressured, true, memory_order_relaxed);
    return written > 0 ? LOG_WRITE_PARTIAL : LOG_WRITE_FAILED;
  }
  return LOG_WRITE_COMPLETE;
}

static bool write_record_locked(const char *line, const size_t length) {
  if (output_line_incomplete) {
    if (write_all("\n", 1) != LOG_WRITE_COMPLETE) {
      return false;
    }
    output_line_incomplete = false;
  }

  const LogWriteResult result = write_all(line, length);
  if (result == LOG_WRITE_PARTIAL) {
    output_line_incomplete = true;
  }
  return result == LOG_WRITE_COMPLETE;
}

static bool write_record(const char *line, const size_t length) {
  const int locked = pthread_mutex_lock(&output_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock output mutex", locked);
  }
  const bool written = write_record_locked(line, length);
  const int unlocked = pthread_mutex_unlock(&output_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock output mutex", unlocked);
  }
  return written;
}

[[gnu::format(printf, 6, 0)]] static size_t format_log_line(
  char *line, const size_t line_capacity, const PGStatusLogLevel level,
  const char *component, const int *error_number, const char *format,
  va_list args
) {
  char message[LOG_MESSAGE_CAPACITY];
  const int message_length = vsnprintf(message, sizeof(message), format, args);
  if (message_length < 0) {
    snprintf(message, sizeof(message), "Unable to format log message");
  } else if ((size_t)message_length >= sizeof(message)) {
    mark_truncated(message, sizeof(message));
  }

  if (error_number) {
    char error_text[256];
    get_error_text(*error_number, error_text, sizeof(error_text));
    const size_t used = strlen(message);
    const int suffix_length = snprintf(
      message + used, sizeof(message) - used, ": %s (errno=%d)", error_text,
      *error_number
    );
    if (suffix_length < 0 || (size_t)suffix_length >= sizeof(message) - used) {
      mark_truncated(message, sizeof(message));
    }
  }
  char timestamp[LOG_TIMESTAMP_CAPACITY];
  format_timestamp(timestamp, sizeof(timestamp));

  const char *safe_component = component ? component : "unknown";
  const int line_length = snprintf(
    line, line_capacity, "%s %s %s: %s\n", timestamp, level_name(level),
    safe_component, message
  );
  if (line_length < 0) {
    return 0;
  }
  if ((size_t)line_length >= line_capacity) {
    const size_t length = mark_line_truncated(line, line_capacity);
    sanitize_log_line(line, length);
    return length;
  }
  const size_t length = (size_t)line_length;
  sanitize_log_line(line, length);
  return length;
}

static bool queue_position_is_available(
  const PGStatusLogLevel level, const size_t position
) {
  if (level >= PG_STATUS_LOG_ERROR) {
    return true;
  }
  const size_t dequeued = atomic_load_explicit(
    &dequeue_position, memory_order_acquire
  );
  return position - dequeued < LOG_QUEUE_CAPACITY - LOG_ERROR_RESERVE;
}

static bool try_enqueue_record(
  const PGStatusLogLevel level, const char *line, const size_t length
) {
  size_t position = atomic_load_explicit(
    &enqueue_position, memory_order_relaxed
  );
  LogQueueSlot *slot;
  for (;;) {
    if (!queue_position_is_available(level, position)) {
      return false;
    }
    slot = &log_queue[position % LOG_QUEUE_CAPACITY];
    const size_t sequence = atomic_load_explicit(
      &slot->sequence, memory_order_acquire
    );
    const intptr_t difference = (intptr_t)sequence - (intptr_t)position;
    if (difference == 0) {
      if (
        atomic_compare_exchange_weak_explicit(
          &enqueue_position, &position, position + 1, memory_order_relaxed,
          memory_order_relaxed
        )
      ) {
        break;
      }
      continue;
    }
    if (difference < 0) {
      return false;
    }
    position = atomic_load_explicit(&enqueue_position, memory_order_relaxed);
  }

  memcpy(slot->line, line, length);
  slot->length = length;
  atomic_store_explicit(&slot->sequence, position + 1, memory_order_release);
  return true;
}

static bool try_dequeue_record(LogRecord *record, size_t *position_after) {
  const size_t position = atomic_load_explicit(
    &dequeue_position, memory_order_relaxed
  );
  LogQueueSlot *slot = &log_queue[position % LOG_QUEUE_CAPACITY];
  const size_t sequence = atomic_load_explicit(
    &slot->sequence, memory_order_acquire
  );
  const intptr_t difference = (intptr_t)sequence - (intptr_t)(position + 1);
  if (difference != 0) {
    return false;
  }

  record->length = slot->length;
  memcpy(record->line, slot->line, record->length);
  atomic_store_explicit(
    &slot->sequence, position + LOG_QUEUE_CAPACITY, memory_order_release
  );
  *position_after = position + 1;
  atomic_store_explicit(
    &dequeue_position, *position_after, memory_order_release
  );
  return true;
}

static void wake_logger(void) {
  const char byte = 1;
  ssize_t result;
  do {
    result = write(wake_pipe[1], &byte, 1);
  } while (result < 0 && errno == EINTR);

  if (
    result == 1 || (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
  ) {
    return;
  }
  const int error_number = result < 0 ? errno : EIO;
  internal_logger_error("failed to wake logging thread", error_number);
}

static void end_logger_operation(void) {
  const size_t previous_count = atomic_fetch_sub_explicit(
    &active_operation_count, 1, memory_order_seq_cst
  );
  if (previous_count == 0) {
    internal_logger_error("logger operation count underflow", EINVAL);
  }
  if (previous_count != 1) {
    return;
  }
  if (
    (LoggerState)atomic_load_explicit(&logger_state, memory_order_seq_cst) ==
    LOGGER_RUNNING
  ) {
    return;
  }

  const int locked = pthread_mutex_lock(&lifecycle_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock lifecycle mutex", locked);
  }
  const int notified = pthread_cond_broadcast(&lifecycle_condition);
  const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (notified != 0) {
    internal_logger_error("failed to notify lifecycle waiters", notified);
  }
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }
}

/*
 * Sequential consistency closes the registration race with shutdown: an
 * operation either observes RUNNING after incrementing the count, or observes
 * STOPPING and withdraws before touching the wake pipe.
 */
static LoggerState begin_logger_operation(void) {
  const LoggerState initial_state = (LoggerState)atomic_load_explicit(
    &logger_state, memory_order_seq_cst
  );
  if (initial_state != LOGGER_RUNNING) {
    return initial_state;
  }

  atomic_fetch_add_explicit(&active_operation_count, 1, memory_order_seq_cst);
  const LoggerState registered_state = (LoggerState)atomic_load_explicit(
    &logger_state, memory_order_seq_cst
  );
  if (registered_state == LOGGER_RUNNING) {
    return LOGGER_RUNNING;
  }

  end_logger_operation();
  return registered_state;
}

static void dispatch_record(
  const PGStatusLogLevel level, const char *line, const size_t length
) {
  for (;;) {
    const LoggerState state = begin_logger_operation();
    if (state == LOGGER_RUNNING) {
      if (!try_enqueue_record(level, line, length)) {
        atomic_fetch_add_explicit(&dropped_log_count, 1, memory_order_relaxed);
      }
      wake_logger();
      end_logger_operation();
      return;
    }
    if (state == LOGGER_STOPPING) {
      return;
    }

    const int locked = pthread_mutex_lock(&lifecycle_mutex);
    if (locked != 0) {
      internal_logger_error("failed to lock lifecycle mutex", locked);
    }
    const LoggerState locked_state = (LoggerState)atomic_load_explicit(
      &logger_state, memory_order_seq_cst
    );
    if (locked_state == LOGGER_STOPPED) {
      (void)write_record(line, length);
    }
    const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
    if (unlocked != 0) {
      internal_logger_error("failed to unlock lifecycle mutex", unlocked);
    }
    if (locked_state != LOGGER_RUNNING) {
      return;
    }
  }
}

[[gnu::format(printf, 4, 0)]] static void log_message(
  const PGStatusLogLevel level, const char *component, const int *error_number,
  const char *format, va_list args
) {
  validate_log_level(level);
  if (!should_log(level)) {
    return;
  }

  char line[LOG_LINE_CAPACITY];
  const size_t length = format_log_line(
    line, sizeof(line), level, component, error_number, format, args
  );
  if (length > 0) {
    dispatch_record(level, line, length);
  }
}

static void notify_flush_waiters(void) {
  /*
   * Always synchronize with flush_mutex before broadcasting. Checking for
   * waiters without the mutex creates a lost-wakeup race with a flusher that
   * has checked completed_position but has not entered pthread_cond_wait yet.
   */
  const int locked = pthread_mutex_lock(&flush_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock flush mutex", locked);
  }
  const int notified = pthread_cond_broadcast(&flush_condition);
  const int unlocked = pthread_mutex_unlock(&flush_mutex);
  if (notified != 0) {
    internal_logger_error("failed to notify log flush waiters", notified);
  }
  if (unlocked != 0) {
    internal_logger_error("failed to unlock flush mutex", unlocked);
  }
}

static void write_dropped_log_summary(void) {
  const uint_fast64_t dropped = atomic_exchange_explicit(
    &dropped_log_count, 0, memory_order_acq_rel
  );
  if (dropped == 0) {
    return;
  }

  char line[LOG_LINE_CAPACITY];
  char message[128];
  const int message_length = snprintf(
    message, sizeof(message), "messages dropped count=%llu reason=backpressure",
    (unsigned long long)dropped
  );
  if (message_length <= 0 || (size_t)message_length >= sizeof(message)) {
    internal_logger_error("failed to format dropped-message summary", EIO);
  }

  char timestamp[LOG_TIMESTAMP_CAPACITY];
  format_timestamp(timestamp, sizeof(timestamp));
  const int line_length = snprintf(
    line, sizeof(line), "%s WARNING logger: %s\n", timestamp, message
  );
  if (line_length <= 0 || (size_t)line_length >= sizeof(line)) {
    internal_logger_error("failed to format dropped-message log", EIO);
  }
  if (!write_record(line, (size_t)line_length)) {
    atomic_fetch_add_explicit(
      &dropped_log_count, dropped, memory_order_relaxed
    );
  }
}

static void drain_wake_pipe(void) {
  char bytes[128];
  for (;;) {
    const ssize_t result = read(wake_pipe[0], bytes, sizeof(bytes));
    if (result > 0) {
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (result == 0) {
      return;
    }
    internal_logger_error("failed to read logging wake pipe", errno);
  }
}

static void wait_for_log_records(void) {
  struct pollfd wake_poll = {
    .fd = wake_pipe[0],
    .events = POLLIN,
    .revents = 0,
  };
  int result;
  const int timeout = atomic_load_explicit(
                        &dropped_log_count, memory_order_relaxed
                      ) > 0
                        ? LOG_OUTPUT_RETRY_INTERVAL_MS
                        : -1;
  do {
    result = poll(&wake_poll, 1, timeout);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    internal_logger_error("failed to poll logging wake pipe", errno);
  }
  drain_wake_pipe();
}

static bool all_enqueued_records_completed(void) {
  const size_t enqueued = atomic_load_explicit(
    &enqueue_position, memory_order_acquire
  );
  const size_t completed = atomic_load_explicit(
    &completed_position, memory_order_acquire
  );
  return completed == enqueued;
}

static void *run_logger(void *argument) {
  (void)argument;
  LogRecord record;

  for (;;) {
    size_t processed = 0;
    size_t position_after = 0;
    while (processed < LOG_WRITE_BATCH_SIZE &&
           try_dequeue_record(&record, &position_after)) {
      if (!write_record(record.line, record.length)) {
        atomic_fetch_add_explicit(&dropped_log_count, 1, memory_order_relaxed);
      }
      atomic_store_explicit(
        &completed_position, position_after, memory_order_release
      );
      processed++;
    }

    write_dropped_log_summary();
    if (processed > 0) {
      notify_flush_waiters();
    }

    if (
      atomic_load_explicit(&stop_requested, memory_order_acquire) &&
      all_enqueued_records_completed()
    ) {
      write_dropped_log_summary();
      notify_flush_waiters();
      return nullptr;
    }
    if (processed == LOG_WRITE_BATCH_SIZE) {
      continue;
    }
    wait_for_log_records();
  }
}

static int install_ignored_sigpipe(struct sigaction *previous_action) {
  struct sigaction ignored_sigpipe_action = {
    .sa_handler = SIG_IGN,
  };
  if (sigemptyset(&ignored_sigpipe_action.sa_mask) != 0) {
    return errno;
  }
  if (sigaction(SIGPIPE, &ignored_sigpipe_action, previous_action) != 0) {
    return errno;
  }
  return 0;
}

static void configure_stderr(void) {
  const int locked = pthread_mutex_lock(&output_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock output mutex", locked);
  }

  const char *failure_message = nullptr;
  int error_number = install_ignored_sigpipe(&previous_sigpipe_action);
  if (error_number != 0) {
    failure_message = "failed to ignore SIGPIPE while logging";
  }

  if (!failure_message) {
    stderr_status_flags = fcntl(STDERR_FILENO, F_GETFL);
  }
  if (!failure_message && stderr_status_flags < 0 && errno == EBADF) {
    const int null_descriptor = open("/dev/null", O_WRONLY);
    if (null_descriptor < 0) {
      error_number = errno;
      failure_message = "failed to open /dev/null for stderr";
    } else if (
      null_descriptor != STDERR_FILENO &&
      dup2(null_descriptor, STDERR_FILENO) < 0
    ) {
      error_number = errno;
      close(null_descriptor);
      failure_message = "failed to restore a valid stderr descriptor";
    } else {
      if (null_descriptor != STDERR_FILENO) {
        close(null_descriptor);
      }
      stderr_status_flags = fcntl(STDERR_FILENO, F_GETFL);
    }
  }
  if (
    !failure_message &&
    (stderr_status_flags < 0 ||
     fcntl(STDERR_FILENO, F_SETFL, stderr_status_flags | O_NONBLOCK) != 0)
  ) {
    error_number = errno;
    failure_message = "failed to configure non-blocking stderr";
  }
  atomic_store_explicit(&output_backpressured, false, memory_order_relaxed);
  output_line_incomplete = false;

  const int unlocked = pthread_mutex_unlock(&output_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock output mutex", unlocked);
  }
  if (failure_message) {
    pg_status_log_system_fatal("logger", error_number, "%s", failure_message);
  }
}

static void restore_stderr_configuration(void) {
  const int locked = pthread_mutex_lock(&output_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock output mutex", locked);
  }

  const char *failure_message = nullptr;
  int error_number = 0;
  if (fcntl(STDERR_FILENO, F_SETFL, stderr_status_flags) != 0) {
    error_number = errno;
    failure_message = "failed to restore stderr flags";
  }
  if (sigaction(SIGPIPE, &previous_sigpipe_action, nullptr) != 0) {
    if (!failure_message) {
      error_number = errno;
      failure_message = "failed to restore SIGPIPE handler";
    }
  }
  atomic_store_explicit(&output_backpressured, false, memory_order_relaxed);
  output_line_incomplete = false;

  const int unlocked = pthread_mutex_unlock(&output_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock output mutex", unlocked);
  }
  if (failure_message) {
    internal_logger_error(failure_message, error_number);
  }
}

static void prepare_fatal_output_locked(void) {
  (void)install_ignored_sigpipe(nullptr);
  const int status_flags = fcntl(STDERR_FILENO, F_GETFL);
  if (status_flags >= 0) {
    (void)fcntl(STDERR_FILENO, F_SETFL, status_flags | O_NONBLOCK);
  }
  atomic_store_explicit(&output_backpressured, false, memory_order_relaxed);
}

static void write_fatal_record(const char *line, const size_t length) {
  const int locked = pthread_mutex_lock(&output_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock output mutex", locked);
  }
  prepare_fatal_output_locked();
  (void)write_record_locked(line, length);
  const int unlocked = pthread_mutex_unlock(&output_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock output mutex", unlocked);
  }
}

static void configure_descriptor(const int descriptor) {
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  if (
    descriptor_flags < 0 ||
    fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0
  ) {
    const int error_number = errno;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    pg_status_log_system_fatal(
      "logger", error_number, "failed to configure logging wake pipe"
    );
  }

  const int status_flags = fcntl(descriptor, F_GETFL);
  if (
    status_flags < 0 ||
    fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0
  ) {
    const int error_number = errno;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    pg_status_log_system_fatal(
      "logger", error_number, "failed to configure logging wake pipe"
    );
  }
}

static PGStatusLogLevel configured_log_level(void) {
  const char *configured_level = getenv("pg_status__log_level");
  if (
    !configured_level || *configured_level == '\0' ||
    strcmp(configured_level, "info") == 0
  ) {
    return PG_STATUS_LOG_INFO;
  }
  if (strcmp(configured_level, "debug") == 0) {
    return PG_STATUS_LOG_DEBUG;
  }
  if (
    strcmp(configured_level, "warning") == 0 ||
    strcmp(configured_level, "warn") == 0
  ) {
    return PG_STATUS_LOG_WARNING;
  }
  if (strcmp(configured_level, "error") == 0) {
    return PG_STATUS_LOG_ERROR;
  }
  if (strcmp(configured_level, "fatal") == 0) {
    return PG_STATUS_LOG_FATAL;
  }
  pg_status_log_fatal(
    "config",
    "invalid pg_status__log_level='%s'; expected debug, info, warning, error, "
    "or fatal",
    configured_level
  );
}

void pg_status_log_init(void) {
  pg_status_log_set_level(configured_log_level());

  const int locked = pthread_mutex_lock(&lifecycle_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock lifecycle mutex", locked);
  }

  LoggerState state = (LoggerState)atomic_load_explicit(
    &logger_state, memory_order_seq_cst
  );
  while (state == LOGGER_STOPPING) {
    const int waited = pthread_cond_wait(
      &lifecycle_condition, &lifecycle_mutex
    );
    if (waited != 0) {
      internal_logger_error("failed to wait for logger shutdown", waited);
    }
    state = (LoggerState)atomic_load_explicit(
      &logger_state, memory_order_seq_cst
    );
  }

  if (state == LOGGER_RUNNING) {
    const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
    if (unlocked != 0) {
      internal_logger_error("failed to unlock lifecycle mutex", unlocked);
    }
    return;
  }

  atomic_store_explicit(&enqueue_position, 0, memory_order_relaxed);
  atomic_store_explicit(&dequeue_position, 0, memory_order_relaxed);
  atomic_store_explicit(&completed_position, 0, memory_order_relaxed);
  atomic_store_explicit(&dropped_log_count, 0, memory_order_relaxed);
  atomic_store_explicit(&stop_requested, false, memory_order_relaxed);
  for (size_t i = 0; i < LOG_QUEUE_CAPACITY; i++) {
    atomic_store_explicit(&log_queue[i].sequence, i, memory_order_relaxed);
  }

  configure_stderr();
  if (pipe(wake_pipe) != 0) {
    pg_status_log_system_fatal(
      "logger", errno, "failed to create logging wake pipe"
    );
  }
  configure_descriptor(wake_pipe[0]);
  configure_descriptor(wake_pipe[1]);

  const int started = pthread_create(
    &logger_thread, nullptr, run_logger, nullptr
  );
  if (started != 0) {
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    pg_status_log_system_fatal(
      "logger", started, "failed to start logging thread"
    );
  }
  atomic_store_explicit(&logger_state, LOGGER_RUNNING, memory_order_seq_cst);

  const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }
}

void pg_status_log_flush(void) {
  if (begin_logger_operation() != LOGGER_RUNNING) {
    return;
  }

  const size_t target = atomic_load_explicit(
    &enqueue_position, memory_order_acquire
  );

  const int locked = pthread_mutex_lock(&flush_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock flush mutex", locked);
  }
  wake_logger();
  while (atomic_load_explicit(&completed_position, memory_order_acquire) <
         target) {
    const int waited = pthread_cond_wait(&flush_condition, &flush_mutex);
    if (waited != 0) {
      internal_logger_error("failed to wait for log flush", waited);
    }
  }
  const int unlocked = pthread_mutex_unlock(&flush_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock flush mutex", unlocked);
  }
  end_logger_operation();
}

void pg_status_log_shutdown(void) {
  int locked = pthread_mutex_lock(&lifecycle_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock lifecycle mutex", locked);
  }

  LoggerState state = (LoggerState)atomic_load_explicit(
    &logger_state, memory_order_seq_cst
  );
  while (state == LOGGER_STOPPING) {
    const int waited = pthread_cond_wait(
      &lifecycle_condition, &lifecycle_mutex
    );
    if (waited != 0) {
      internal_logger_error("failed to wait for logger shutdown", waited);
    }
    state = (LoggerState)atomic_load_explicit(
      &logger_state, memory_order_seq_cst
    );
  }
  if (state == LOGGER_STOPPED) {
    const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
    if (unlocked != 0) {
      internal_logger_error("failed to unlock lifecycle mutex", unlocked);
    }
    return;
  }

  atomic_store_explicit(&logger_state, LOGGER_STOPPING, memory_order_seq_cst);
  while (atomic_load_explicit(&active_operation_count, memory_order_seq_cst) !=
         0) {
    const int waited = pthread_cond_wait(
      &lifecycle_condition, &lifecycle_mutex
    );
    if (waited != 0) {
      internal_logger_error("failed to wait for active log operations", waited);
    }
  }
  atomic_store_explicit(&stop_requested, true, memory_order_release);
  wake_logger();

  int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }

  const int joined = pthread_join(logger_thread, nullptr);
  if (joined != 0) {
    internal_logger_error("failed to join logging thread", joined);
  }

  locked = pthread_mutex_lock(&lifecycle_mutex);
  if (locked != 0) {
    internal_logger_error("failed to lock lifecycle mutex", locked);
  }
  close(wake_pipe[0]);
  close(wake_pipe[1]);
  wake_pipe[0] = -1;
  wake_pipe[1] = -1;
  restore_stderr_configuration();
  atomic_store_explicit(&logger_state, LOGGER_STOPPED, memory_order_seq_cst);
  const int notified = pthread_cond_broadcast(&lifecycle_condition);

  unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (notified != 0) {
    internal_logger_error("failed to notify lifecycle waiters", notified);
  }
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }
}

void pg_status_log_set_level(const PGStatusLogLevel level) {
  validate_log_level(level);
  atomic_store_explicit(&minimum_level, level, memory_order_relaxed);
}

PGStatusLogLevel pg_status_log_get_level(void) {
  const PGStatusLogLevel level = (PGStatusLogLevel)atomic_load_explicit(
    &minimum_level, memory_order_relaxed
  );
  validate_log_level(level);
  return level;
}

void pg_status_log(
  const PGStatusLogLevel level, const char *component, const char *format, ...
) {
  const int saved_errno = errno;
  va_list args;
  va_start(args, format);
  log_message(level, component, nullptr, format, args);
  va_end(args);
  errno = saved_errno;
}

void pg_status_log_system_error(
  const PGStatusLogLevel level, const char *component, const int error_number,
  const char *format, ...
) {
  const int saved_errno = errno;
  va_list args;
  va_start(args, format);
  log_message(level, component, &error_number, format, args);
  va_end(args);
  errno = saved_errno;
}

[[noreturn, gnu::format(printf, 3, 0)]] static void fatal_log_message(
  const char *component, const int *error_number, const char *format,
  va_list args
) {
  char line[LOG_LINE_CAPACITY];
  const size_t length = format_log_line(
    line, sizeof(line), PG_STATUS_LOG_FATAL, component, error_number, format,
    args
  );
  pg_status_log_flush();
  if (length > 0) {
    write_fatal_record(line, length);
  }
  exit(EXIT_FAILURE);
}

void pg_status_log_fatal(const char *component, const char *format, ...) {
  va_list args;
  va_start(args, format);
  fatal_log_message(component, nullptr, format, args);
}

void pg_status_log_system_fatal(
  const char *component, const int error_number, const char *format, ...
) {
  va_list args;
  va_start(args, format);
  fatal_log_message(component, &error_number, format, args);
}
