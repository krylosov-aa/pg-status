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
  poll_state_query_read(host);
  clock_ms += start_cost_ms;
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
  configure_hosts(1);
  MonitorHost *host = &monitor_host_list[0];
  const uint64_t durations[] = {20, 200, 900};
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
  configure_hosts(1);
  parameters.query_timeout_ms = 4000;
  MonitorHost *host = &monitor_host_list[0];
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
  configure_hosts(1);
  parameters.query_timeout_ms = 300;
  MonitorHost *host = &monitor_host_list[0];
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
  configure_hosts(1);
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
  configure_hosts(2);
  parameters.query_timeout_ms = 3000;
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
  configure_hosts(1);
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];
  (void)prepare_poll(pfds);
  pfds[1].revents = POLLIN;
  clock_ms = 11000;
  process_poll_result(pfds);
  support_assert_true(
    timeouts == 1 && advances == 0, "deadline wins even when socket is ready"
  );
  support_assert_true(
    monitor_host_list[0].failed_connections == 1,
    "late response is a failed check"
  );
}

static void test_fresh_clock(void) {
  configure_hosts(2);
  start_cost_ms = 100;
  (void)pump_one_iteration();
  support_assert_true(
    monitor_host_list[1].iter_started_at_ms == 10100,
    "each host gets its actual start time"
  );
  support_assert_true(
    last_poll_timeout == 800, "recompute remaining time after starting hosts"
  );
  support_assert_true(
    timeouts == 1, "later-started host keeps its remaining time"
  );

  clock_ms = 11050;
  start_cost_ms = 0;
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];
  (void)prepare_poll(pfds);
  pfds[1].revents = POLLIN;
  pfds[2].revents = POLLIN;
  advance_cost_ms = 100;
  process_poll_result(pfds);
  support_assert_true(
    advances == 1 && timeouts == 2,
    "refresh time between processing host events"
  );
}

static void test_missing_socket(void) {
  configure_hosts(1);
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
  configure_hosts(1);
  parameters.query_timeout_ms = 5000;
  start_and_timeout_hosts();
  struct pollfd pfds[MAX_HOSTS + 1];
  support_assert_true(
    prepare_poll(pfds) == 5000, "in-flight checks do not wake every sleep_ms"
  );
}

static void configure_environment(void) {
  support_set_environment("pg_status__hosts", "127.0.0.1");
  support_clear_environment("pg_status__sleep");
  support_clear_environment("pg_status__sleep_ms");
  support_clear_environment("pg_status__query_timeout_ms");
  support_clear_environment("pg_status__max_fails");
}

static void test_defaults(void) {
  configure_environment();
  set_parameters_from_env();
  support_assert_true(
    parameters.sleep_ms == 1000 && parameters.query_timeout_ms == 1000 &&
      parameters.max_fails == 3,
    "default polling parameters"
  );
}

static void test_custom_parameters(void) {
  configure_environment();
  support_set_environment("pg_status__sleep_ms", "200");
  support_set_environment("pg_status__query_timeout_ms", "3000");
  support_set_environment("pg_status__max_fails", "5");
  set_parameters_from_env();
  support_assert_true(
    parameters.sleep_ms == 200 && parameters.query_timeout_ms == 3000 &&
      parameters.max_fails == 5,
    "timeout may exceed the polling period"
  );
}

static void invalid_parameter(const char *name, const char *value) {
  configure_environment();
  support_set_environment(name, value);
  pg_status_log_init();
  set_parameters_from_env();
  pg_status_log_shutdown();
}

static void test_zero_period(void) {
  invalid_parameter("pg_status__sleep_ms", "0");
}

static void test_zero_timeout(void) {
  invalid_parameter("pg_status__query_timeout_ms", "0");
}

static void test_overflow_timeout(void) {
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
    {"fresh_clock", test_fresh_clock},
    {"missing_socket", test_missing_socket},
    {"wait_for_deadline", test_wait_for_deadline},
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
