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

#include "log_formatter.h"

enum {
  LOG_MESSAGE_CAPACITY = 2048,
  LOG_LINE_CAPACITY = 4096,
  LOG_COMPONENT_CAPACITY = 4096,
  LOG_ENQUEUE_ATTEMPTS = 16,
  LOG_TIMESTAMP_CAPACITY = 32,
  LOG_QUEUE_CAPACITY = 256,
  LOG_ERROR_RESERVE = 16,
  LOG_WRITE_BATCH_SIZE = 64,
  LOG_OUTPUT_WAIT_TIMEOUT_MS = 10,
  LOG_OUTPUT_RETRY_INTERVAL_MS = 250,
};
static const char TRUNCATION_MARKER[] = "...[truncated]";

/* Queue entries own their strings; no caller pointers survive publication. */
typedef struct {
  struct timespec occurred_at;
  PGStatusLogFormatter formatter;
  PGStatusLogLevel level;
  int error_number;
  bool has_error;
  bool message_truncated;
  bool component_truncated;
  char message[LOG_MESSAGE_CAPACITY];
  char component[LOG_COMPONENT_CAPACITY];
} LogEvent;

typedef struct {
  _Atomic size_t sequence;
  LogEvent event;
} LogQueueSlot;

typedef struct {
  size_t length;
  size_t written;
  char line[LOG_LINE_CAPACITY];
} PendingOutput;

typedef enum {
  LOGGER_STOPPED,
  LOGGER_RUNNING,
  LOGGER_STOPPING,
} LoggerState;

/* Low bit closes admission; every active operation contributes two. */
enum { OPERATIONS_CLOSED = 1, OPERATION_REFERENCE = 2 };

static _Atomic(PGStatusLogFormatter) record_formatter = pg_status_format_text;
static atomic_int minimum_level = PG_STATUS_LOG_INFO;
static atomic_int logger_state = LOGGER_STOPPED;
static atomic_bool stop_requested;
static atomic_size_t enqueue_position;
static atomic_size_t dequeue_position;
static atomic_size_t completed_position;
static atomic_uint_fast64_t dropped_log_count;
static atomic_size_t operation_gate = OPERATIONS_CLOSED;
static atomic_bool wake_pending;
static atomic_bool output_backpressured;
static atomic_bool output_pending;
/* Protected by output_mutex, including fatal output and lifecycle changes. */
static PendingOutput pending_output;

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

static bool write_remaining(const char *buffer, size_t length, size_t *written);
static size_t format_record(
  char *line, size_t capacity, PGStatusLogLevel level, const char *component,
  const char *message, const int *error_number
);

[[noreturn]] static void internal_logger_error(
  const char *message, const int error_number
) {
  char output[LOG_LINE_CAPACITY];
  char detail[192];
  (void)snprintf(
    detail, sizeof(detail), "pg-status logger internal error: %s (error=%d)",
    message, error_number
  );
  const size_t length = format_record(
    output, sizeof(output), PG_STATUS_LOG_FATAL, "logger", detail, &error_number
  );
  if (length > 0) {
    size_t written = 0;
    (void)write_remaining(output, length, &written);
  }
  abort();
}

[[noreturn]] static void invalid_log_level(const PGStatusLogLevel level) {
  char message[96];
  const int length = snprintf(
    message, sizeof(message),
    "pg-status logger internal error: invalid log level value=%d", (int)level
  );
  if (length > 0) {
    char output[LOG_LINE_CAPACITY];
    const size_t output_length = format_record(
      output, sizeof(output), PG_STATUS_LOG_FATAL, "logger", message, nullptr
    );
    size_t written = 0;
    (void)write_remaining(output, output_length, &written);
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
  /* Public logging calls validate the level before formatting. Keep this
   * conversion independent of diagnostics used by that validation. */
  return "UNKNOWN";
}

static bool should_log(const PGStatusLogLevel level) {
  return level >= (PGStatusLogLevel)atomic_load_explicit(
                    &minimum_level, memory_order_relaxed
                  );
}

static struct timespec capture_time(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    now = (struct timespec){.tv_nsec = -1};
  }
  return now;
}

static void format_timestamp(
  char *buffer, const size_t capacity, const struct timespec *now
) {
  struct tm utc;
  if (
    now->tv_nsec < 0 || now->tv_nsec >= 1000000000L ||
    gmtime_r(&now->tv_sec, &utc) == nullptr
  ) {
    (void)snprintf(buffer, capacity, "unknown-time");
    return;
  }

  char seconds[24];
  if (strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
    (void)snprintf(buffer, capacity, "unknown-time");
    return;
  }
  const unsigned int milliseconds = (unsigned int)(now->tv_nsec / 1000000L);
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

static void get_error_text(
  const int error_number, char *buffer, const size_t capacity
) {
  if (strerror_r(error_number, buffer, capacity) != 0) {
    (void)snprintf(buffer, capacity, "Unknown error");
  }
}

static size_t format_record_at(
  char *line, const size_t capacity, const PGStatusLogLevel level,
  const char *component, const char *message, const int *error_number,
  const struct timespec *occurred_at, const PGStatusLogFormatter formatter
) {
  char timestamp[LOG_TIMESTAMP_CAPACITY];
  format_timestamp(timestamp, sizeof(timestamp), occurred_at);
  const PGStatusLogEntry entry = {
    .timestamp = timestamp,
    .level = level,
    .level_name = level_name(level),
    .component = component ? component : "unknown",
    .message = message,
    .error_number = error_number,
  };
  return formatter(line, capacity, &entry);
}

static size_t format_record(
  char *line, const size_t capacity, const PGStatusLogLevel level,
  const char *component, const char *message, const int *error_number
) {
  const struct timespec now = capture_time();
  const PGStatusLogFormatter formatter = atomic_load_explicit(
    &record_formatter, memory_order_relaxed
  );
  return format_record_at(
    line, capacity, level, component, message, error_number, &now, formatter
  );
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

static bool write_remaining(
  const char *buffer, const size_t length, size_t *written
) {
  if (
    atomic_load_explicit(&output_backpressured, memory_order_relaxed) &&
    !wait_until_stderr_is_writable(0)
  ) {
    return false;
  }
  atomic_store_explicit(&output_backpressured, false, memory_order_relaxed);
  while (*written < length) {
    const ssize_t result = write(
      STDERR_FILENO, buffer + *written, length - *written
    );
    if (result > 0) {
      *written += (size_t)result;
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
    return false;
  }
  return true;
}

static bool finish_pending_output_locked(void) {
  if (pending_output.length == 0) {
    return true;
  }
  if (!write_remaining(
        pending_output.line, pending_output.length, &pending_output.written
      )) {
    return false;
  }
  pending_output.length = 0;
  pending_output.written = 0;
  atomic_store_explicit(&output_pending, false, memory_order_relaxed);
  return true;
}

/* True means either written completely or retained for a later retry. */
static bool write_record_locked(const char *line, const size_t length) {
  if (!finish_pending_output_locked()) {
    return false;
  }
  size_t written = 0;
  if (write_remaining(line, length, &written)) {
    return true;
  }
  if (written == 0) {
    return false;
  }
  memcpy(pending_output.line, line, length);
  pending_output.length = length;
  pending_output.written = written;
  atomic_store_explicit(&output_pending, true, memory_order_relaxed);
  return true;
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

[[gnu::format(printf, 5, 0)]] static void capture_event(
  LogEvent *event, const PGStatusLogLevel level, const char *component,
  const int *error_number, const char *format, va_list args
) {
  event->occurred_at = capture_time();
  event->formatter = atomic_load_explicit(
    &record_formatter, memory_order_relaxed
  );
  event->level = level;
  event->has_error = error_number != nullptr;
  event->error_number = error_number ? *error_number : 0;

  const char *name = component ? component : "unknown";
  const size_t length = strnlen(name, sizeof(event->component));
  event->component_truncated = length >= sizeof(event->component);
  const size_t copied = event->component_truncated
                          ? sizeof(event->component) - 1
                          : length;
  memcpy(event->component, name, copied);
  event->component[copied] = '\0';

  const int message_length = vsnprintf(
    event->message, sizeof(event->message), format, args
  );
  event->message_truncated = message_length >= (int)sizeof(event->message);
  if (message_length < 0) {
    static const char failure[] = "Unable to format log message";
    memcpy(event->message, failure, sizeof(failure));
  }
}

static size_t format_event(LogEvent *event, char *line, const size_t capacity) {
  if (event->component_truncated) {
    mark_truncated(event->component, sizeof(event->component));
  }
  if (event->message_truncated) {
    mark_truncated(event->message, sizeof(event->message));
  }
  if (event->has_error) {
    char error_text[256];
    get_error_text(event->error_number, error_text, sizeof(error_text));
    const size_t used = strlen(event->message);
    const int length = snprintf(
      event->message + used, sizeof(event->message) - used, ": %s (errno=%d)",
      error_text, event->error_number
    );
    if (length < 0 || (size_t)length >= sizeof(event->message) - used) {
      mark_truncated(event->message, sizeof(event->message));
    }
  }
  return format_record_at(
    line, capacity, event->level, event->component, event->message,
    event->has_error ? &event->error_number : nullptr, &event->occurred_at,
    event->formatter
  );
}

/* Reserve before vsnprintf so full queues cost no message formatting.
 * A reserved slot is always published before its operation reference is freed.
 */
static LogQueueSlot *try_reserve_record(
  const PGStatusLogLevel level, size_t *reserved_position
) {
  size_t position = atomic_load_explicit(
    &enqueue_position, memory_order_relaxed
  );
  const size_t limit = level >= PG_STATUS_LOG_ERROR
                         ? LOG_QUEUE_CAPACITY
                         : LOG_QUEUE_CAPACITY - LOG_ERROR_RESERVE;
  for (size_t attempt = 0; attempt < LOG_ENQUEUE_ATTEMPTS; attempt++) {
    const size_t dequeued = atomic_load_explicit(
      &dequeue_position, memory_order_acquire
    );
    if (position - dequeued >= limit) {
      return nullptr;
    }
    LogQueueSlot *slot = &log_queue[position % LOG_QUEUE_CAPACITY];
    if (
      atomic_load_explicit(&slot->sequence, memory_order_acquire) == position
    ) {
      const size_t candidate = position;
      if (
        atomic_compare_exchange_weak_explicit(
          &enqueue_position, &position, position + 1, memory_order_relaxed,
          memory_order_relaxed
        )
      ) {
        *reserved_position = candidate;
        return slot;
      }
    } else {
      position = atomic_load_explicit(&enqueue_position, memory_order_relaxed);
    }
  }
  return nullptr;
}

static LogQueueSlot *next_ready_record(size_t *position) {
  *position = atomic_load_explicit(&dequeue_position, memory_order_relaxed);
  LogQueueSlot *slot = &log_queue[*position % LOG_QUEUE_CAPACITY];
  if (
    atomic_load_explicit(&slot->sequence, memory_order_acquire) != *position + 1
  ) {
    return nullptr;
  }
  return slot;
}

static void complete_record(LogQueueSlot *slot, const size_t position) {
  atomic_store_explicit(
    &slot->sequence, position + LOG_QUEUE_CAPACITY, memory_order_release
  );
  atomic_store_explicit(&dequeue_position, position + 1, memory_order_release);
  atomic_store_explicit(
    &completed_position, position + 1, memory_order_release
  );
}

static void wake_logger(void) {
  if (atomic_exchange_explicit(&wake_pending, true, memory_order_acq_rel)) {
    return;
  }
  const char byte = 1;
  /* One non-blocking attempt. The worker also polls periodically, so EINTR
   * cannot lose a notification forever or force the caller into a retry loop.
   */
  const ssize_t written = write(wake_pipe[1], &byte, 1);
  (void)written;
}

static void end_logger_operation(void) {
  atomic_fetch_sub_explicit(
    &operation_gate, OPERATION_REFERENCE, memory_order_release
  );
}

/* Closing and reference acquisition share one atomic, so shutdown cannot miss
 * an entering caller. The caller never needs the lifecycle mutex. */
static bool begin_logger_operation(void) {
  size_t gate = atomic_load_explicit(&operation_gate, memory_order_acquire);
  for (size_t attempt = 0; attempt < LOG_ENQUEUE_ATTEMPTS; attempt++) {
    if (
      (gate & OPERATIONS_CLOSED) != 0 || gate > SIZE_MAX - OPERATION_REFERENCE
    ) {
      return false;
    }
    if (
      atomic_compare_exchange_weak_explicit(
        &operation_gate, &gate, gate + OPERATION_REFERENCE,
        memory_order_acquire, memory_order_relaxed
      )
    ) {
      return true;
    }
  }
  return false;
}

[[gnu::format(printf, 4, 0)]] static void log_message(
  const PGStatusLogLevel level, const char *component, const int *error_number,
  const char *format, va_list args
) {
  validate_log_level(level);
  if (!should_log(level)) {
    return;
  }
  if (!begin_logger_operation()) {
    if (
      (atomic_load_explicit(&operation_gate, memory_order_acquire) &
       OPERATIONS_CLOSED) == 0
    ) {
      // The periodic worker poll reports contention without a wake-pipe
      // reference.
      atomic_fetch_add_explicit(&dropped_log_count, 1, memory_order_relaxed);
    }
    return;
  }

  size_t position = 0;
  LogQueueSlot *slot = try_reserve_record(level, &position);
  if (slot) {
    capture_event(&slot->event, level, component, error_number, format, args);
    atomic_store_explicit(&slot->sequence, position + 1, memory_order_release);
  } else {
    atomic_fetch_add_explicit(&dropped_log_count, 1, memory_order_relaxed);
  }
  wake_logger();
  end_logger_operation();
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

  const size_t line_length = format_record(
    line, sizeof(line), PG_STATUS_LOG_WARNING, "logger", message, nullptr
  );
  if (line_length == 0) {
    internal_logger_error("failed to format dropped-message log", EIO);
  }
  if (!write_record(line, line_length)) {
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
  drain_wake_pipe();
  /* Publish-before-notify plus this acquire exchange/recheck prevents a
   * coalesced wakeup from being lost just as the worker goes to sleep. */
  (void)atomic_exchange_explicit(&wake_pending, false, memory_order_acq_rel);
  size_t position = 0;
  if (
    next_ready_record(&position) ||
    atomic_load_explicit(&stop_requested, memory_order_acquire)
  ) {
    return;
  }
  struct pollfd wake_poll = {.fd = wake_pipe[0], .events = POLLIN};
  const int result = poll(&wake_poll, 1, LOG_OUTPUT_RETRY_INTERVAL_MS);
  if (result < 0 && errno != EINTR) {
    internal_logger_error("failed to poll logging wake pipe", errno);
  }
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
  char line[LOG_LINE_CAPACITY];

  for (;;) {
    size_t processed = 0;
    size_t position = 0;
    LogQueueSlot *slot;
    while (processed < LOG_WRITE_BATCH_SIZE &&
           (slot = next_ready_record(&position)) != nullptr) {
      const size_t length = format_event(&slot->event, line, sizeof(line));
      if (length == 0 || !write_record(line, length)) {
        atomic_fetch_add_explicit(&dropped_log_count, 1, memory_order_relaxed);
      }
      complete_record(slot, position);
      processed++;
    }

    /* An empty write only retries the previous record; it emits no new bytes.
     */
    if (atomic_load_explicit(&output_pending, memory_order_relaxed)) {
      (void)write_record("", 0);
    }
    write_dropped_log_summary();
    if (processed > 0) {
      notify_flush_waiters();
    }
    if (
      atomic_load_explicit(&stop_requested, memory_order_acquire) &&
      all_enqueued_records_completed()
    ) {
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

static PGStatusLogFormatter configured_log_formatter(void) {
  const char *value = getenv("pg_status__log_format");
  if (!value || *value == '\0' || strcmp(value, "text") == 0) {
    return pg_status_format_text;
  }
  if (strcmp(value, "json") == 0) {
    return pg_status_format_json;
  }
  pg_status_log_fatal(
    "config", "invalid pg_status__log_format='%s'; expected text or json", value
  );
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
  atomic_store_explicit(
    &record_formatter, configured_log_formatter(), memory_order_relaxed
  );
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
  atomic_store_explicit(&wake_pending, false, memory_order_relaxed);
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
  atomic_store_explicit(&operation_gate, 0, memory_order_release);

  const int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }
}

void pg_status_log_flush(void) {
  /* Explicit flush may retry admission; ordinary log calls never do. */
  while (!begin_logger_operation()) {
    if (
      (atomic_load_explicit(&operation_gate, memory_order_acquire) &
       OPERATIONS_CLOSED) != 0
    ) {
      return;
    }
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

  atomic_fetch_or_explicit(
    &operation_gate, OPERATIONS_CLOSED, memory_order_acq_rel
  );
  atomic_store_explicit(&logger_state, LOGGER_STOPPING, memory_order_seq_cst);
  int unlocked = pthread_mutex_unlock(&lifecycle_mutex);
  if (unlocked != 0) {
    internal_logger_error("failed to unlock lifecycle mutex", unlocked);
  }

  /* Only shutdown waits. Producers release their references without locking
   * or signalling a condition variable. */
  while (atomic_load_explicit(&operation_gate, memory_order_acquire) !=
         OPERATIONS_CLOSED) {
    const struct timespec delay = {.tv_nsec = 100000};
    (void)nanosleep(&delay, nullptr);
  }
  atomic_store_explicit(&stop_requested, true, memory_order_release);
  wake_logger();

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
  LogEvent event;
  capture_event(
    &event, PG_STATUS_LOG_FATAL, component, error_number, format, args
  );
  char line[LOG_LINE_CAPACITY];
  const size_t length = format_event(&event, line, sizeof(line));
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
