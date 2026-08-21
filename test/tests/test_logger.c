/** Test scenarios for process logging. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common_support.h"
#include "logger.h"

typedef void (*logger_test_function_t)(void);

static const char *logger_test_program;

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
  static constexpr size_t digit_positions[] = {
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

static constexpr size_t WORKER_COUNT = 4;
static constexpr size_t MESSAGES_PER_WORKER = 25;
static constexpr size_t CONCURRENT_ROUND_COUNT = 4;

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
}

static void test_concurrent_messages(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Act
  char *output = support_capture_standard_error(emit_concurrent_messages);

  // Assert
  size_t line_count = 0;
  char *save_pointer = nullptr;
  char *line = strtok_r(output, "\n", &save_pointer);
  while (line) {
    support_assert_true(has_timestamp_shape(line), "concurrent timestamp");
    support_assert_contains(
      line, " INFO concurrency: worker=", "interleaved concurrent log line"
    );
    line_count++;
    line = strtok_r(nullptr, "\n", &save_pointer);
  }
  support_assert_true(
    line_count == CONCURRENT_ROUND_COUNT * WORKER_COUNT * MESSAGES_PER_WORKER,
    "concurrent log line count"
  );

  // Cleanup
  free(output);
  pg_status_log_shutdown();
}

static constexpr size_t ORDERED_MESSAGE_COUNT = 16;

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
    position = strstr(position, expected);
    support_assert_true(position != nullptr, "asynchronous log order");
    position += (size_t)length;
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

static atomic_bool lifecycle_workers_should_stop;
static atomic_uint_fast64_t lifecycle_operation_count;

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
  }
  return nullptr;
}

static void test_concurrent_lifecycle(void) {
  static constexpr size_t lifecycle_worker_count = 3;
  static constexpr size_t lifecycle_round_count = 8;
  static constexpr uint_fast64_t operations_per_round = 100;

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
    }
    pg_status_log_shutdown();
  }
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
      lifecycle_round_count * operations_per_round,
    "logger lifecycle workers did not make progress"
  );
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

static void wait_for_pipe_data(const int descriptor) {
  struct pollfd output_poll = {
    .fd = descriptor,
    .events = POLLIN,
    .revents = 0,
  };
  int result;
  do {
    result = poll(&output_poll, 1, 2000);
  } while (result < 0 && errno == EINTR);
  support_assert_true(result > 0, "logger output recovery timed out");
  support_assert_true(
    (output_poll.revents & POLLIN) != 0,
    "logger output pipe did not become readable"
  );
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

static void test_output_backpressure_periodic_recovery(void) {
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
  wait_for_pipe_data(output_pipe[0]);
  char output[8192];
  const size_t output_length = read_pipe(
    output_pipe[0], output, sizeof(output)
  );
  pg_status_log_shutdown();
  close(output_pipe[0]);
  restore_standard_error(saved_stderr);

  // Assert
  support_assert_true(output_length > 0, "logger did not retry output");
  support_assert_contains(
    output, " WARNING logger: messages dropped count=1 reason=backpressure\n",
    "periodic dropped-message summary"
  );
}

static void test_output_partial_write_recovery(void) {
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
  static constexpr char component_prefix[] = "partial-record";
  memcpy(component, component_prefix, sizeof(component_prefix) - 1);
  component[sizeof(component) - 1] = '\0';

  // Act
  pg_status_log(PG_STATUS_LOG_INFO, component, "message");
  pg_status_log_flush();
  char partial_output[65536];
  const size_t partial_length = read_pipe(
    output_socket[1], partial_output, sizeof(partial_output)
  );
  wait_for_pipe_data(output_socket[1]);
  char recovered_output[8192];
  const size_t recovered_length = read_pipe(
    output_socket[1], recovered_output, sizeof(recovered_output)
  );
  pg_status_log_shutdown();
  close(output_socket[1]);
  restore_standard_error(saved_stderr);

  // Assert
  const char *partial_record = strstr(partial_output, " INFO partial-record");
  support_assert_true(
    partial_record != nullptr, "partial record was not written"
  );
  support_assert_true(partial_length > 0, "partial output is empty");
  support_assert_true(
    partial_output[partial_length - 1] != '\n',
    "test did not produce a partial write"
  );
  support_assert_true(recovered_length > 0, "partial line was not recovered");
  support_assert_true(
    recovered_output[0] == '\n', "partial line was not terminated on recovery"
  );
  support_assert_contains(
    recovered_output,
    " WARNING logger: messages dropped count=1 reason=backpressure\n",
    "partial-write dropped-message summary"
  );
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
  static constexpr size_t round_count = 8;

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

static const struct {
  const char *name;
  logger_test_function_t function;
} test_cases[] = {
  {"message_format", test_message_format},
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

  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("logger_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }

  support_fail("unknown test case name");
}
