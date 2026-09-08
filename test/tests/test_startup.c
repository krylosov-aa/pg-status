/** Startup readiness with real monitor polls held by local TCP listeners. */

#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common_support.h"
#include "http_test.h"
#include "pg_monitor.h"
#include "pg_status_api.h"
#include "pg_status_version.h"
#include "utils.h"

static const struct {
  const char *path;
  unsigned int ready_status;
} monitor_routes[] = {
  {"/ready", 200},
  {"/master", 404},
  {"/replica", 404},
  {"/hosts", 200},
  {"/status?host=127.0.0.1", 200},
  {"/sync_by_time", 404},
  {"/sync_by_bytes", 404},
  {"/sync_by_time_or_bytes", 404},
  {"/sync_by_time_and_bytes", 404},
  {"/most_sync_by_bytes", 404},
  {"/status", 400},
  {"/replica?lag_ms=invalid", 400},
  {"/sync_by_bytes?min_lsn=invalid", 400},
};

static void pause_briefly(void) {
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
  (void)nanosleep(&delay, nullptr);
}

static int create_listener(uint16_t *port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    support_fail("create PostgreSQL listener");
  }
  struct sockaddr_in address = {
    .sin_family = AF_INET,
    .sin_port = 0,
    .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
  };
  support_assert_true(
    bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0,
    "bind PostgreSQL listener"
  );
  support_assert_true(listen(fd, 1) == 0, "listen for PostgreSQL connection");
  socklen_t length = sizeof(address);
  support_assert_true(
    getsockname(fd, (struct sockaddr *)&address, &length) == 0,
    "get PostgreSQL listener port"
  );
  *port = ntohs(address.sin_port);
  return fd;
}

static int accept_monitor(const int listener) {
  struct pollfd pfd = {.fd = listener, .events = POLLIN};
  support_assert_true(poll(&pfd, 1, 2000) == 1, "monitor did not connect");
  const int fd = accept(listener, nullptr, nullptr);
  if (fd < 0) {
    support_fail("accept monitor connection");
  }
  return fd;
}

static void configure_monitor(const char *ports) {
  support_set_environment("pg_status__hosts", "127.0.0.1,127.0.0.1");
  support_set_environment("pg_status__pg_port", ports);
  support_set_environment("pg_status__sleep_ms", "60000");
  support_set_environment("pg_status__query_timeout_ms", "60000");
  support_set_environment("PGSSLMODE", "disable");
  support_set_environment("PGGSSENCMODE", "disable");
}

static void assert_probes(const uint16_t port) {
  const char *headers[] = {nullptr, "Accept: application/json\r\n"};
  for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
    TestHTTPResponse response = http_test_get(port, "/live", headers[i]);
    http_test_assert_status(&response, 200);
    http_test_assert_body(&response, "OK");
    http_test_assert_contains(
      &response, "Content-Type: text/plain; charset=utf-8"
    );
    http_test_response_free(&response);

    response = http_test_get(port, "/version", headers[i]);
    http_test_assert_status(&response, 200);
    http_test_assert_body(&response, PG_STATUS_VERSION);
    http_test_assert_contains(
      &response, "Content-Type: text/plain; charset=utf-8"
    );
    http_test_response_free(&response);
  }
}

static void assert_monitor_routes(const uint16_t port, const bool ready) {
  const char *headers[] = {nullptr, "Accept: application/json\r\n"};
  for (size_t i = 0; i < sizeof(monitor_routes) / sizeof(monitor_routes[0]);
       i++) {
    for (size_t j = 0; j < sizeof(headers) / sizeof(headers[0]); j++) {
      TestHTTPResponse response = http_test_get(
        port, monitor_routes[i].path, headers[j]
      );
      http_test_assert_status(
        &response, ready ? monitor_routes[i].ready_status : 503
      );
      if (!ready) {
        http_test_assert_body(
          &response, "{\"error_text\": \"pg_monitor is not ready\"}"
        );
        http_test_assert_contains(&response, "Content-Type: application/json");
      } else if (strcmp(monitor_routes[i].path, "/ready") == 0) {
        http_test_assert_body(&response, "OK");
        http_test_assert_contains(
          &response, "Content-Type: text/plain; charset=utf-8"
        );
      }
      http_test_response_free(&response);
    }
  }
}

static void test_startup(const bool stop_during_warmup) {
  uint16_t first_port;
  uint16_t second_port;
  const int first_listener = create_listener(&first_port);
  const int second_listener = create_listener(&second_port);
  char ports[32];
  (void)snprintf(
    ports, sizeof(ports), "%u,%u", (unsigned int)first_port,
    (unsigned int)second_port
  );
  configure_monitor(ports);

  support_assert_true(!is_pg_monitor_ready(), "monitor initially ready");
  HTTPServer *server = start_pg_status_api("127.0.0.1", 0);
  const uint16_t api_port = http_server_port(server);
  // The API must be usable even before monitor parameters/hosts are
  // initialized.
  assert_probes(api_port);
  assert_monitor_routes(api_port, false);

  const uint64_t started_at = monotonic_ms();
  start_pg_monitor();
  support_assert_true(
    monotonic_ms() - started_at < 2000, "monitor startup blocked on polls"
  );
  const int first_connection = accept_monitor(first_listener);
  const int second_connection = accept_monitor(second_listener);
  assert_probes(api_port);
  assert_monitor_routes(api_port, false);

  if (!stop_during_warmup) {
    close(first_connection);
    // Wait for the first host to publish its failed poll. The second remains
    // blocked, so a partial topology must not make the API ready.
    const uint64_t first_deadline = monotonic_ms() + 2000;
    while (
      atomic_load_explicit(&monitor_host_list[0].seq, memory_order_acquire) ==
        0 &&
      monotonic_ms() < first_deadline) {
      pause_briefly();
    }
    support_assert_true(
      atomic_load_explicit(&monitor_host_list[0].seq, memory_order_acquire) > 0,
      "first host did not publish its poll"
    );
    assert_monitor_routes(api_port, false);

    close(second_connection);
    const uint64_t ready_deadline = monotonic_ms() + 2000;
    while (!is_pg_monitor_ready() && monotonic_ms() < ready_deadline) {
      pause_briefly();
    }
    support_assert_true(is_pg_monitor_ready(), "monitor did not become ready");
    // Completed failures still make the monitor ready, with normal 404s for
    // host selection and 200s for host status.
    assert_probes(api_port);
    assert_monitor_routes(api_port, true);
  }

  const uint64_t stopped_at = monotonic_ms();
  stop_pg_monitor();
  support_assert_true(
    monotonic_ms() - stopped_at < 2000, "monitor shutdown blocked on polls"
  );
  support_assert_true(!is_pg_monitor_ready(), "stopped monitor is ready");
  assert_probes(api_port);
  assert_monitor_routes(api_port, false);
  stop_http_server(server);
  if (stop_during_warmup) {
    close(first_connection);
    close(second_connection);
  }
  close(first_listener);
  close(second_listener);
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  if (strcmp(argv[1], "readiness") == 0) {
    test_startup(false);
  } else if (strcmp(argv[1], "shutdown_during_warmup") == 0) {
    test_startup(true);
  } else {
    support_fail("unknown test case name");
  }
  return EXIT_SUCCESS;
}
