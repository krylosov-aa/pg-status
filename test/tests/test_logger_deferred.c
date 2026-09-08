/** Deterministically stall the formatter to exercise queue ownership/admission.
 * These formatter doubles are linked with the real logger, without changing
 * production code. The normal logger tests exercise the real TXT/JSON output.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common_support.h"
#include "log_formatter.h"

static pthread_t caller;
static bool use_json;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;
static bool formatter_entered;
static bool formatter_released;
static bool snapshot_seen;
static char snapshot_message[256];
static char snapshot_timestamp[32];
static int snapshot_errno;

static void lock_gate(void) {
  support_assert_true(pthread_mutex_lock(&gate_mutex) == 0, "lock test gate");
}

static void unlock_gate(void) {
  support_assert_true(
    pthread_mutex_unlock(&gate_mutex) == 0, "unlock test gate"
  );
}

static void wait_gate(void) {
  support_assert_true(
    pthread_cond_wait(&gate_condition, &gate_mutex) == 0, "wait on test gate"
  );
}

static void notify_gate(void) {
  support_assert_true(
    pthread_cond_broadcast(&gate_condition) == 0, "notify test gate"
  );
}

static size_t stalled_formatter(
  char *line, const size_t capacity, const PGStatusLogEntry *entry,
  const bool json
) {
  support_assert_true(
    !pthread_equal(caller, pthread_self()),
    "formatting ran in the caller thread"
  );
  support_assert_true(json == use_json, "wrong deferred formatter");
  lock_gate();
  if (strcmp(entry->component, "hold") == 0) {
    formatter_entered = true;
    notify_gate();
    while (!formatter_released) {
      wait_gate();
    }
  }
  if (strcmp(entry->component, "snapshot") == 0) {
    snapshot_seen = true;
    snprintf(snapshot_message, sizeof(snapshot_message), "%s", entry->message);
    snprintf(
      snapshot_timestamp, sizeof(snapshot_timestamp), "%s", entry->timestamp
    );
    snapshot_errno = entry->error_number ? *entry->error_number : -1;
  }
  unlock_gate();
  support_assert_true(capacity >= 2, "formatter buffer too small");
  memcpy(line, "\n", 2);
  return 1;
}

size_t pg_status_format_text(
  char *line, const size_t capacity, const PGStatusLogEntry *entry
) {
  return stalled_formatter(line, capacity, entry, false);
}

size_t pg_status_format_json(
  char *line, const size_t capacity, const PGStatusLogEntry *entry
) {
  return stalled_formatter(line, capacity, entry, true);
}

static void timestamp_now(char *buffer, const size_t capacity) {
  struct timespec now;
  struct tm utc;
  support_assert_true(clock_gettime(CLOCK_REALTIME, &now) == 0, "read time");
  support_assert_true(gmtime_r(&now.tv_sec, &utc) != nullptr, "convert time");
  char seconds[24];
  support_assert_true(
    strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc) > 0,
    "format test time"
  );
  snprintf(
    buffer, capacity, "%s.%03uZ", seconds,
    (unsigned int)(now.tv_nsec / 1000000L)
  );
}

static void assert_stopped_call_skips_formatting(void) {
  int touched = -1;
  pg_status_log(PG_STATUS_LOG_ERROR, "stopped", "%n", &touched);
  support_assert_true(touched == -1, "stopped logger formatted a message");
}

static void queue_while_formatter_is_stalled(void) {
  pg_status_log(PG_STATUS_LOG_INFO, "hold", "stall the worker");
  lock_gate();
  while (!formatter_entered) {
    wait_gate();
  }
  unlock_gate();

  char before[32];
  char after[32];
  char component[] = "snapshot";
  char message[] = "original";
  timestamp_now(before, sizeof(before));
  errno = EDOM;
  pg_status_log_system_error(
    PG_STATUS_LOG_ERROR, component, EACCES, "value=%s", message
  );
  support_assert_true(errno == EDOM, "enqueue changed caller errno");
  timestamp_now(after, sizeof(after));
  memset(component, 'x', sizeof(component) - 1);
  memset(message, 'x', sizeof(message) - 1);

  // %n tells us whether vsnprintf ran, even when its output is discarded.
  bool full = false;
  for (size_t i = 0; i < 1024; i++) {
    int touched = -1;
    pg_status_log(PG_STATUS_LOG_INFO, "fill", "%n", &touched);
    if (touched == -1) {
      full = true;
      break;
    }
  }
  support_assert_true(full, "queue did not reject messages before formatting");
  int touched = -1;
  pg_status_log(PG_STATUS_LOG_ERROR, "reserved", "%n", &touched);
  support_assert_true(touched == 0, "error reserve was not available");
  for (size_t i = 0; i < 1024; i++) {
    pg_status_log(PG_STATUS_LOG_ERROR, "fill", "fill error reserve");
  }
  for (size_t i = 0; i < 1024; i++) {
    touched = -1;
    pg_status_log(PG_STATUS_LOG_ERROR, "full", "%n", &touched);
    support_assert_true(touched == -1, "full queue still formatted a message");
  }

  // Ensure formatting happens later than the event's capture interval.
  const struct timespec delay = {.tv_nsec = 20000000};
  (void)nanosleep(&delay, nullptr);
  lock_gate();
  formatter_released = true;
  notify_gate();
  unlock_gate();
  pg_status_log_flush();

  lock_gate();
  support_assert_true(snapshot_seen, "queued component was not copied");
  support_assert_contains(
    snapshot_message, "value=original: ", "queued message changed"
  );
  support_assert_contains(
    snapshot_message, strerror(EACCES), "missing deferred error text"
  );
  support_assert_true(snapshot_errno == EACCES, "queued errno changed");
  support_assert_true(
    strcmp(snapshot_timestamp, before) >= 0 &&
      strcmp(snapshot_timestamp, after) <= 0,
    "timestamp reflects output time instead of event time"
  );
  unlock_gate();
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected text or json");
  }
  use_json = strcmp(argv[1], "json") == 0;
  caller = pthread_self();
  support_clear_environment("pg_status__log_level");
  support_set_environment("pg_status__log_format", argv[1]);
  assert_stopped_call_skips_formatting();
  pg_status_log_init();
  queue_while_formatter_is_stalled();
  pg_status_log_shutdown();
  assert_stopped_call_skips_formatting();
  return 0;
}
