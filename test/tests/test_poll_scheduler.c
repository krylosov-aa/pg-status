/** Deterministic scheduling tests: real lifecycle, controlled clock and I/O. */

#include <stdlib.h>
#include <string.h>

#include "common_support.h"

// Keep scheduling and publication real; replace only the clock, poll(), and
// the transport steps so these tests do not depend on network or wall time.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/check_utils.c"

static uint64_t clock_ms;
static uint64_t start_cost_ms;
static bool start_fails;
static uint64_t advance_cost_ms;
static unsigned int starts[MAX_HOSTS];
static unsigned int advances;
static unsigned int timeouts;
static int socket_fd = 7;
static int last_poll_timeout;

static uint64_t scripted_monotonic_ms(void) {
  return clock_ms;
}

static void scripted_start(MonitorHost *host, const uint64_t now_ms) {
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE, "no overlapping polls"
  );
  starts[(size_t)(host - monitor_host_list)]++;
  reset_iter_state(host, now_ms);
  // Like libpq resolving a host name, the start blocks the whole loop.
  clock_ms += start_cost_ms;
  if (start_fails) {
    finish_iteration(host, false, now_ms);
  } else {
    poll_state_query_read(host);
  }
}

static void finish_success(MonitorHost *host) {
  host->iter_data_ready = true;
  host->iter_new_status = (MonitorStatus){.alive = true};
  finish_iteration(host, true, clock_ms);
}

static void scripted_advance(MonitorHost *host, const uint64_t now_ms) {
  (void)now_ms;
  advances++;
  finish_success(host);
  clock_ms += advance_cost_ms;
}

static void scripted_timeout(MonitorHost *host, const uint64_t now_ms) {
  timeouts++;
  timeout_host_poll(host, now_ms);
}

static int scripted_socket(const MonitorHost *host) {
  (void)host;
  return socket_fd;
}

static int scripted_poll(struct pollfd *pfds, nfds_t count, int timeout_ms) {
  (void)pfds;
  (void)count;
  support_assert_true(timeout_ms >= 0, "finite wait for configured hosts");
  last_poll_timeout = timeout_ms;
  clock_ms += (uint64_t)timeout_ms;
  return 0;
}

#define monotonic_ms scripted_monotonic_ms
#define start_host_poll scripted_start
#define advance_host_poll scripted_advance
#define timeout_host_poll scripted_timeout
#define host_socket scripted_socket
#define poll scripted_poll
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/pg_monitor.c"
#undef monotonic_ms
#undef start_host_poll
#undef advance_host_poll
#undef timeout_host_poll
#undef host_socket
#undef poll

static void configure_hosts(unsigned int count) {
  host_count = count;
  clock_ms = 10000;
  parameters.sleep_ms = 1000;
  parameters.query_timeout_ms = 1000;
  parameters.max_fails = 3;
  parameters.poll_error_log_interval_ms = 60000;
  for (unsigned int i = 0; i < count; i++) {
    monitor_host_list[i] = (MonitorHost){.host = "test", .pollfd_slot = -1};
    publish_monitor_snapshot(
      &monitor_host_list[i], (MonitorSnapshot){.status = {.alive = true}}
    );
  }
}

static int prepare_poll(struct pollfd *pfds) {
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  return timeout_ms;
}

static void test_period(void) {
  // Arrange
  configure_hosts(1);
  MonitorHost *host = &monitor_host_list[0];
  const uint64_t durations[] = {20, 200, 900};

  // Act & Assert
  for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
    const uint64_t expected_start = 10000 + (i * 1000);
    clock_ms = expected_start;
    start_and_timeout_hosts();
    support_assert_true(
      host->iter_started_at_ms == expected_start, "stable start period"
    );
    clock_ms += durations[i];
    finish_success(host);
    struct pollfd pfds[MAX_HOSTS + 1];
    support_assert_true(
      prepare_poll(pfds) == (int)(1000 - durations[i]),
      "wait only the remaining period"
    );
    clock_ms = expected_start + 999;
    start_and_timeout_hosts();
    support_assert_true(
      starts[0] == i + 1, "do not poll before the period elapses"
    );
  }
}

static void test_overrun(void) {
  // Arrange
  configure_hosts(1);
  parameters.query_timeout_ms = 4000;
  MonitorHost *host = &monitor_host_list[0];

  // Act & Assert
  start_and_timeout_hosts();
  clock_ms = 13500;
  finish_success(host);
  struct pollfd pfds[MAX_HOSTS + 1];
  support_assert_true(
    prepare_poll(pfds) == 0, "overrun starts next poll immediately"
  );
  start_and_timeout_hosts();
  support_assert_true(starts[0] == 2, "only one new poll after missed periods");
  clock_ms = 13550;
  finish_success(host);
  support_assert_true(
    host->next_poll_at_ms == 14500, "anchor to actual new start"
  );
  start_and_timeout_hosts();
  support_assert_true(
    starts[0] == 2, "no catch-up burst after a fast response"
  );
}

static void test_failure_period(void) {
  // Arrange
  configure_hosts(1);
  parameters.query_timeout_ms = 300;
  MonitorHost *host = &monitor_host_list[0];

  // Act & Assert
  for (unsigned int i = 0; i < 3; i++) {
    clock_ms = 10000 + ((uint64_t)i * 1000);
    start_and_timeout_hosts();
    clock_ms += 300;
    start_and_timeout_hosts();
    support_assert_true(
      host->next_poll_at_ms == 11000 + ((uint64_t)i * 1000),
      "timeout uses start period"
    );
    support_assert_true(
      host->failed_connections == i + 1, "one failure per check"
    );
  }
  support_assert_true(
    !atomic_get_status(host).alive, "dead after three timeouts"
  );
}

static void test_default_failure_timing(void) {
  // Arrange
  configure_hosts(1);

  // Act & Assert
  for (unsigned int i = 0; i < 3; i++) {
    (void)pump_one_iteration();
    support_assert_true(last_poll_timeout == 1000, "one timeout per second");
  }
  support_assert_true(
    clock_ms == 13000 && starts[0] == 3, "no extra sleep after timeout"
  );
  support_assert_true(
    !atomic_get_status(&monitor_host_list[0]).alive,
    "three failed checks mark dead"
  );
}

static void test_independent_hosts_and_warmup(void) {
  // Arrange
  configure_hosts(2);
  parameters.query_timeout_ms = 3000;

  // Act & Assert
  start_and_timeout_hosts();
  support_assert_true(
    !all_hosts_have_polled(), "starting checks is not warmup completion"
  );
  clock_ms = 10100;
  finish_success(&monitor_host_list[1]);
  support_assert_true(
    !all_hosts_have_polled(), "wait for the slow host's first result"
  );
  clock_ms = 11000;
  start_and_timeout_hosts();
  support_assert_true(
    starts[0] == 1 && starts[1] == 2,
    "fast host continues while slow host waits"
  );
  clock_ms = 11100;
  finish_success(&monitor_host_list[1]);
  clock_ms = 13000;
  start_and_timeout_hosts();
  support_assert_true(
    all_hosts_have_polled(), "a first timeout also completes warmup"
  );
}

static void test_deadline_before_event(void) {
  // Arrange
  configure_hosts(1);
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];
  (void)prepare_poll(pfds);
  pfds[1].revents = POLLIN;
  clock_ms = 11000;

  // Act
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    timeouts == 1 && advances == 0, "deadline wins even when socket is ready"
  );
  support_assert_true(
    monitor_host_list[0].failed_connections == 1,
    "late response is a failed check"
  );
}

/** Starts host 0 at 10000, then host 1 at 10500 with a blocking start. */
static void stall_while_host_in_flight(const uint64_t stall_ms) {
  configure_hosts(2);
  monitor_host_list[1].next_poll_at_ms = 10500;
  start_and_timeout_hosts();
  clock_ms = 10500;
  start_cost_ms = stall_ms;
  start_and_timeout_hosts();
}

static void test_stall_does_not_fail_ready_host(void) {
  // Arrange: host 1's start blocks the loop past host 0's deadline
  stall_while_host_in_flight(1000);
  struct pollfd pfds[MAX_HOSTS + 1];
  (void)prepare_poll(pfds);
  pfds[1].revents = POLLIN;

  // Act
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    advances == 1 && timeouts == 0, "ready response is read after a stall"
  );
  support_assert_true(
    monitor_host_list[0].failed_connections == 0,
    "loop stall is not a failure of a host whose response is ready"
  );
}

static void test_stall_is_not_charged(void) {
  // Arrange: host 0 waits for more I/O, e.g. the next handshake step
  stall_while_host_in_flight(1000);
  struct pollfd pfds[MAX_HOSTS + 1];

  // Act & Assert
  support_assert_true(
    prepare_poll(pfds) == 500, "in-flight check keeps its unblocked time"
  );
  process_poll_result(pfds);
  support_assert_true(timeouts == 0, "loop stall does not time out checks");
  clock_ms = 12000;
  process_poll_result(pfds);
  support_assert_true(
    timeouts == 1, "deadline still bounds the unblocked time"
  );
}

static void test_own_stall_is_not_charged(void) {
  // Arrange
  configure_hosts(1);
  start_cost_ms = 1500;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].iter_started_at_ms == 10000,
    "period stays anchored to the start"
  );
  struct pollfd pfds[MAX_HOSTS + 1];
  support_assert_true(
    prepare_poll(pfds) == 1000, "own lookup time is not charged"
  );
}

static void test_failed_start_after_stall(void) {
  // Arrange: the start blocks for 5 s, then fails (e.g. DNS timeout)
  configure_hosts(1);
  start_cost_ms = 5000;
  start_fails = true;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].failed_connections == 1, "failed start is counted"
  );
  struct pollfd pfds[MAX_HOSTS + 1];
  support_assert_true(
    prepare_poll(pfds) == 1000,
    "retry a full period after the blocked start, not back-to-back"
  );
}

static void test_advance_stall_is_not_charged(void) {
  // Arrange: processing host 0's event blocks the loop (GSSAPI lookup)
  configure_hosts(2);
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];
  (void)prepare_poll(pfds);
  pfds[1].revents = POLLIN;
  advance_cost_ms = 1500;

  // Act
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    advances == 1 && timeouts == 0, "processing stall does not time out checks"
  );
  support_assert_true(
    prepare_poll(pfds) == 1000, "in-flight check keeps its unblocked time"
  );
}

static void test_fresh_clock(void) {
  // Arrange
  configure_hosts(2);
  start_cost_ms = 100;

  // Act
  (void)pump_one_iteration();

  // Assert
  support_assert_true(
    monitor_host_list[1].iter_started_at_ms == 10100,
    "each host gets its actual start time"
  );
  support_assert_true(
    last_poll_timeout == 1000, "recompute remaining time after starting hosts"
  );
  support_assert_true(timeouts == 2, "start time is charged to neither check");
}

static void test_missing_socket(void) {
  // Arrange
  configure_hosts(1);

  // Act & Assert
  start_and_timeout_hosts();
  socket_fd = -1;
  struct pollfd pfds[MAX_HOSTS + 1];
  support_assert_true(
    prepare_poll(pfds) == 0, "missing socket does not delay failure"
  );
  process_poll_result(pfds);
  support_assert_true(
    timeouts == 1, "fail a missing socket without waiting for another tick"
  );
}

static void test_wait_for_deadline(void) {
  // Arrange
  configure_hosts(1);
  parameters.query_timeout_ms = 5000;
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];

  // Act & Assert
  support_assert_true(
    prepare_poll(pfds) == 5000, "in-flight checks do not wake every sleep_ms"
  );
}

/**
 * Makes host 0 the selected master with a check in progress until a late
 * deadline, and host 1 a replica whose result is read and waits for it.
 */
static MonitorHost *wait_for_master(void) {
  configure_hosts(2);
  MonitorHost *master = &monitor_host_list[0];
  publish_monitor_snapshot(
    master, (MonitorSnapshot){.status = master_status()}
  );
  save_master_index(0);
  reset_iter_state(master, clock_ms);
  master->iter_deadline_ms = clock_ms + 5000;
  poll_state_query_read(master);
  MonitorHost *replica = &monitor_host_list[1];
  reset_iter_state(replica, clock_ms);
  replica->iter_data_ready = true;
  replica->iter_new_status = replica_status();
  // The last commit is old, so the master's check may bound the time lag.
  replica->iter_commit_age_ms = 60000;
  replica->iter_observed_at_ms = clock_ms;
  replica->poll_state = HOST_POLL_MASTER_WAIT;
  return replica;
}

static void test_master_wait_ends_at_deadline(void) {
  // Arrange
  MonitorHost *replica = wait_for_master();
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;

  // Act
  const int count = build_poll_fd(clock_ms, pfds, &timeout_ms);

  // Assert
  support_assert_true(
    count == 2 && replica->pollfd_slot == -1,
    "a waiting replica's socket stays out of poll()"
  );
  support_assert_true(timeout_ms == 1000, "wake at the replica's deadline");

  // Act: the loop sleeps until the deadline and then handles its sockets.
  (void)pump_one_iteration();

  // Assert
  support_assert_true(
    clock_ms == 11000 && replica->poll_state == HOST_POLL_MASTER_WAIT &&
      timeouts == 0,
    "socket handling does not time out a waiting replica"
  );

  // Act
  (void)pump_one_iteration();

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && timeouts == 0 &&
      replica->failed_connections == 0 && snap.status.alive &&
      !snap.status.possible_dead,
    "the deadline ends the replica's check without a failure"
  );
}

static void test_master_wait_ends_before_next_check(void) {
  // Arrange: the replica's deadline is past its next check.
  MonitorHost *replica = wait_for_master();
  replica->iter_deadline_ms = clock_ms + 5000;
  struct pollfd pfds[MAX_HOSTS + 1];

  // Act
  const int timeout_ms = prepare_poll(pfds);
  (void)pump_one_iteration();
  (void)pump_one_iteration();

  // Assert
  support_assert_true(
    timeout_ms == 1000, "wake when the replica's next check is due"
  );
  support_assert_true(
    clock_ms == 11000 && replica->poll_state == HOST_POLL_IDLE &&
      replica->next_poll_at_ms == 11000 && timeouts == 0,
    "a hanging master check does not delay the replica's next check"
  );
}

static void test_master_change_ends_wait(void) {
  // Arrange
  MonitorHost *replica = wait_for_master();

  // Act
  save_master_index(-1);
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && timeouts == 0 &&
      atomic_get_snapshot(replica).status.alive,
    "without the selected master, the replica stops waiting"
  );
}

static void fail_with_result(
  MonitorHost *host, const ExecStatusType status, const uint64_t now_ms
) {
  PGresult *res = PQmakeEmptyPGresult(nullptr, status);
  support_assert_true(res != nullptr, "allocate failed poll result");
  support_assert_true(
    !validate_sql_result(host, res, now_ms), "reject failed poll result"
  );
  PQclear(res);
  finish_iteration(host, false, now_ms);
}

static void run_failure_logging(const PGStatusLogLevel level) {
  configure_hosts(2);
  MonitorHost *host = &monitor_host_list[0];
  host->host = "pg-a";
  monitor_host_list[1].host = "pg-b";
  pg_status_log_init();
  pg_status_log_set_level(level);

  timeout_host_poll(host, 0);
  fail_with_result(host, PGRES_FATAL_ERROR, 1);
  fail_with_result(host, PGRES_TUPLES_OK, 59999);
  fail_with_result(host, PGRES_FATAL_ERROR, 60000);
  timeout_host_poll(host, 60001);
  timeout_host_poll(host, 119999);
  fail_with_result(host, PGRES_TUPLES_OK, 120000);
  timeout_host_poll(&monitor_host_list[1], 120001);
  timeout_host_poll(host, 180000);

  pg_status_log_shutdown();
}

static void run_failure_logging_info(void) {
  run_failure_logging(PG_STATUS_LOG_INFO);
}

static void run_failure_logging_debug(void) {
  run_failure_logging(PG_STATUS_LOG_DEBUG);
}

static void assert_log_count(
  const char *logs, const char *message, const size_t expected
) {
  size_t count = 0;
  const size_t length = strlen(message);
  for (const char *match = strstr(logs, message); match;
       match = strstr(match + length, message)) {
    count++;
  }
  support_assert_true(count == expected, message);
}

static void assert_failure_error_logs(const char *logs) {
  assert_log_count(logs, " ERROR monitor: ", 5);
  assert_log_count(
    logs, "ERROR monitor: PostgreSQL poll timed out host=pg-a", 2
  );
  assert_log_count(
    logs,
    "ERROR monitor: PostgreSQL operation failed operation=validate_result "
    "host=pg-a",
    1
  );
  assert_log_count(
    logs, "ERROR monitor: unexpected PostgreSQL result row count host=pg-a", 1
  );
  assert_log_count(
    logs, "ERROR monitor: PostgreSQL poll timed out host=pg-b", 1
  );
  assert_log_count(logs, "host state changed host=pg-a state=dead", 1);
}

static void test_failure_logging(void) {
  support_set_environment("pg_status__log_format", "text");
  char *logs = support_capture_standard_error(run_failure_logging_info);
  assert_failure_error_logs(logs);
  support_assert_not_contains(logs, " DEBUG ", "quiet between reminders");
  free(logs);
}

static void test_failure_logging_debug(void) {
  support_set_environment("pg_status__log_format", "text");
  char *logs = support_capture_standard_error(run_failure_logging_debug);
  assert_failure_error_logs(logs);
  assert_log_count(logs, " DEBUG monitor: ", 4);
  assert_log_count(
    logs, "DEBUG monitor: PostgreSQL poll timed out host=pg-a", 2
  );
  assert_log_count(
    logs,
    "DEBUG monitor: PostgreSQL operation failed operation=validate_result "
    "host=pg-a",
    1
  );
  assert_log_count(
    logs, "DEBUG monitor: unexpected PostgreSQL result row count host=pg-a", 1
  );
  free(logs);
}

static void run_recovery_logging(void) {
  configure_hosts(1);
  MonitorHost *host = &monitor_host_list[0];
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  // Recover before max_fails, then immediately start a new failure episode.
  timeout_host_poll(host, 1000);
  clock_ms = 1001;
  finish_success(host);
  finish_success(host);
  timeout_host_poll(host, 1002);

  // No result is not recovery, even if the caller passes success=true.
  finish_iteration(host, true, 1003);
  timeout_host_poll(host, 1004);
  clock_ms = 1005;
  finish_success(host);
  timeout_host_poll(host, 1006);

  // A host that has never been alive must also report its first failure.
  configure_hosts(1);
  publish_monitor_snapshot(
    host, (MonitorSnapshot){.status = {.possible_dead = true}}
  );
  timeout_host_poll(host, 2000);
  clock_ms = 2001;
  finish_success(host);
  pg_status_log_shutdown();
}

static void test_recovery_logging(void) {
  support_set_environment("pg_status__log_format", "text");
  char *logs = support_capture_standard_error(run_recovery_logging);
  assert_log_count(logs, "ERROR monitor: PostgreSQL poll timed out", 4);
  assert_log_count(
    logs, "INFO monitor: host state changed host=test state=alive", 3
  );
  assert_log_count(logs, "host state changed host=test state=dead", 1);
  free(logs);
}

static void run_failure_logging_interval(void) {
  configure_hosts(3);
  monitor_host_list[0].host = "custom";
  monitor_host_list[1].host = "every";
  monitor_host_list[2].host = "first";
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);

  parameters.poll_error_log_interval_ms = 5000;
  timeout_host_poll(&monitor_host_list[0], 1000);
  timeout_host_poll(&monitor_host_list[0], 5999);
  timeout_host_poll(&monitor_host_list[0], 6000);

  parameters.poll_error_log_interval_ms = 0;
  timeout_host_poll(&monitor_host_list[1], 1000);
  timeout_host_poll(&monitor_host_list[1], 1000);
  timeout_host_poll(&monitor_host_list[1], 1001);

  // The deadline saturates instead of wrapping into the past.
  parameters.poll_error_log_interval_ms = UINT64_MAX;
  timeout_host_poll(&monitor_host_list[2], 1000);
  timeout_host_poll(&monitor_host_list[2], 2000);

  pg_status_log_shutdown();
}

static void test_failure_logging_interval(void) {
  support_set_environment("pg_status__log_format", "text");
  char *logs = support_capture_standard_error(run_failure_logging_interval);
  assert_log_count(logs, " ERROR monitor: ", 6);
  assert_log_count(
    logs, "ERROR monitor: PostgreSQL poll timed out host=custom", 2
  );
  assert_log_count(
    logs, "ERROR monitor: PostgreSQL poll timed out host=every", 3
  );
  assert_log_count(
    logs, "ERROR monitor: PostgreSQL poll timed out host=first", 1
  );
  free(logs);
}

static void configure_environment(void) {
  support_set_environment("pg_status__hosts", "127.0.0.1");
  support_clear_environment("pg_status__sleep");
  support_clear_environment("pg_status__sleep_ms");
  support_clear_environment("pg_status__query_timeout_ms");
  support_clear_environment("pg_status__max_fails");
  support_clear_environment("pg_status__poll_error_log_interval_ms");
}

static void test_defaults(void) {
  // Arrange
  configure_environment();

  // Act
  set_parameters_from_env();

  // Assert
  support_assert_true(
    parameters.sleep_ms == 1000 && parameters.query_timeout_ms == 1000 &&
      parameters.max_fails == 3,
    "default polling parameters"
  );
  support_assert_true(
    parameters.poll_error_log_interval_ms == 60000,
    "default poll error log interval"
  );
}

static void test_custom_parameters(void) {
  // Arrange
  configure_environment();
  support_set_environment("pg_status__sleep_ms", "200");
  support_set_environment("pg_status__query_timeout_ms", "3000");
  support_set_environment("pg_status__max_fails", "5");
  support_set_environment("pg_status__poll_error_log_interval_ms", "0");

  // Act
  set_parameters_from_env();

  // Assert
  support_assert_true(
    parameters.sleep_ms == 200 && parameters.query_timeout_ms == 3000 &&
      parameters.max_fails == 5,
    "timeout may exceed the polling period"
  );
  support_assert_true(
    parameters.poll_error_log_interval_ms == 0,
    "zero poll error log interval is accepted"
  );
}

static void invalid_parameter(const char *name, const char *value) {
  // Arrange
  configure_environment();
  support_set_environment(name, value);
  pg_status_log_init();

  // Act
  set_parameters_from_env();

  // Cleanup
  pg_status_log_shutdown();
}

static void test_zero_period(void) {
  // Act
  invalid_parameter("pg_status__sleep_ms", "0");
}

static void test_zero_timeout(void) {
  // Act
  invalid_parameter("pg_status__query_timeout_ms", "0");
}

static void test_overflow_timeout(void) {
  // Act
  invalid_parameter("pg_status__query_timeout_ms", "18446744073709551615");
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  const struct {
    const char *name;
    support_action_t run;
  } cases[] = {
    {"period", test_period},
    {"overrun", test_overrun},
    {"failure_period", test_failure_period},
    {"default_failure_timing", test_default_failure_timing},
    {"independent_hosts_and_warmup", test_independent_hosts_and_warmup},
    {"deadline_before_event", test_deadline_before_event},
    {"stall_does_not_fail_ready_host", test_stall_does_not_fail_ready_host},
    {"stall_is_not_charged", test_stall_is_not_charged},
    {"own_stall_is_not_charged", test_own_stall_is_not_charged},
    {"failed_start_after_stall", test_failed_start_after_stall},
    {"advance_stall_is_not_charged", test_advance_stall_is_not_charged},
    {"fresh_clock", test_fresh_clock},
    {"missing_socket", test_missing_socket},
    {"wait_for_deadline", test_wait_for_deadline},
    {"master_wait_ends_at_deadline", test_master_wait_ends_at_deadline},
    {"master_wait_ends_before_next_check",
     test_master_wait_ends_before_next_check},
    {"master_change_ends_wait", test_master_change_ends_wait},
    {"failure_logging", test_failure_logging},
    {"failure_logging_debug", test_failure_logging_debug},
    {"recovery_logging", test_recovery_logging},
    {"failure_logging_interval", test_failure_logging_interval},
    {"defaults", test_defaults},
    {"custom_parameters", test_custom_parameters},
    {"zero_period", test_zero_period},
    {"zero_timeout", test_zero_timeout},
    {"overflow_timeout", test_overflow_timeout},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      cases[i].run();
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
