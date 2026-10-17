/** Connection pacing tests: real scheduler and checks, scripted libpq. */

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

#include "common_support.h"
#include "connection.h"

// Every libpq call on these objects is scripted below, and any other one
// fails the test, so the test may complete the opaque type with its own
// placeholder.
struct pg_conn {
  char unused;
};

static uint64_t clock_ms;
static uint64_t start_cost_ms;
static uint64_t connect_poll_cost_ms;
static uint64_t wake_latency_ms;
static PGconn fake_connections[MAX_HOSTS];
static ConnStatusType connection_status[MAX_HOSTS];
static PostgresPollingStatusType connect_poll_results[MAX_HOSTS];
static bool start_fails[MAX_HOSTS];
static bool start_connects[MAX_HOSTS];
static bool tcp_refused[MAX_HOSTS];
static bool reconnects[MAX_HOSTS];
static bool used_password[MAX_HOSTS];
static bool needs_password[MAX_HOSTS];
static bool ssl_in_use[MAX_HOSTS];
static uint64_t connect_times[256];
static size_t connect_host_indexes[256];
static unsigned int connect_count;
static unsigned int connects[MAX_HOSTS];
static unsigned int closes[MAX_HOSTS];
static unsigned int tcp_polls[MAX_HOSTS];
static unsigned int connect_polls[MAX_HOSTS];

static size_t connection_index(const PGconn *conn) {
  return (size_t)(conn - fake_connections);
}

static PGconn *scripted_connection_start(const MonitorHost *host) {
  const size_t i = (size_t)(host - monitor_host_list);
  connects[i]++;
  support_assert_true(
    connect_count < sizeof(connect_times) / sizeof(connect_times[0]),
    "connection log capacity"
  );
  connect_host_indexes[connect_count] = i;
  connect_times[connect_count++] = clock_ms;
  // Like libpq resolving a host name, the start may block the whole loop.
  clock_ms += start_cost_ms;
  // Unless a test says otherwise, the TCP connection is still in progress.
  connection_status[i] = CONNECTION_STARTED;
  if (start_fails[i]) {
    connection_status[i] = CONNECTION_BAD;
  } else if (start_connects[i]) {
    connection_status[i] = CONNECTION_MADE;
  }
  return &fake_connections[i];
}

static ConnStatusType scripted_status(const PGconn *conn) {
  return connection_status[connection_index(conn)];
}

static PostgresPollingStatusType scripted_connect_poll(PGconn *conn) {
  const size_t i = connection_index(conn);
  if (connection_status[i] == CONNECTION_STARTED) {
    // Like libpq, the step after TCP connects only reads the TCP result.
    tcp_polls[i]++;
    if (tcp_refused[i]) {
      connection_status[i] = CONNECTION_BAD;
      return PGRES_POLLING_FAILED;
    }
    connection_status[i] = CONNECTION_MADE;
    return PGRES_POLLING_WRITING;
  }
  connect_polls[i]++;
  clock_ms += connect_poll_cost_ms;
  const PostgresPollingStatusType result = connect_poll_results[i];
  if (result == PGRES_POLLING_OK) {
    connection_status[i] = CONNECTION_OK;
  } else if (result == PGRES_POLLING_FAILED && reconnects[i]) {
    // libpq drops the connection and connects again, for example to retry
    // another encryption method; it forgets the TLS and password state.
    reconnects[i] = false;
    ssl_in_use[i] = false;
    used_password[i] = false;
    connection_status[i] = CONNECTION_STARTED;
    return PGRES_POLLING_WRITING;
  } else if (result == PGRES_POLLING_FAILED) {
    connection_status[i] = CONNECTION_BAD;
  } else if (result == PGRES_POLLING_READING) {
    connection_status[i] = CONNECTION_SSL_STARTUP;
  }
  return result;
}

static int scripted_used_password(const PGconn *conn) {
  return used_password[connection_index(conn)];
}

static int scripted_needs_password(const PGconn *conn) {
  return needs_password[connection_index(conn)];
}

static int scripted_ssl_in_use(PGconn *conn) {
  return ssl_in_use[connection_index(conn)];
}

static int scripted_set_nonblocking(PGconn *conn, int arg) {
  (void)conn;
  (void)arg;
  return 0;
}

static void scripted_finish(PGconn *conn) {
  closes[connection_index(conn)]++;
}

static int scripted_send_query(PGconn *conn, const char *query) {
  (void)conn;
  (void)query;
  return 1;
}

static int scripted_flush(PGconn *conn) {
  (void)conn;
  return 0;
}

static int scripted_consume_input(PGconn *conn) {
  (void)conn;
  return 1;
}

// The response never arrives on its own; tests finish checks explicitly.
static int scripted_is_busy(PGconn *conn) {
  (void)conn;
  return 1;
}

// Checks never get past PQisBusy here, so a result would be unscripted.
static PGresult *scripted_get_result(PGconn *conn) {
  (void)conn;
  support_fail("unscripted PQgetResult on a placeholder connection");
}

static int scripted_socket(const PGconn *conn) {
  (void)conn;
  return 7;
}

static char *scripted_error_message(const PGconn *conn) {
  (void)conn;
  static char message[] = "scripted";
  return message;
}

#define pg_connection_start scripted_connection_start
#define PQstatus scripted_status
#define PQconnectPoll scripted_connect_poll
#define PQconnectionUsedPassword scripted_used_password
#define PQconnectionNeedsPassword scripted_needs_password
#define PQsslInUse scripted_ssl_in_use
#define PQsetnonblocking scripted_set_nonblocking
#define PQfinish scripted_finish
#define PQsendQuery scripted_send_query
#define PQflush scripted_flush
#define PQconsumeInput scripted_consume_input
#define PQisBusy scripted_is_busy
#define PQgetResult scripted_get_result
#define PQsocket scripted_socket
#define PQerrorMessage scripted_error_message
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/check_utils.c"
#undef pg_connection_start
#undef PQstatus
#undef PQconnectPoll
#undef PQconnectionUsedPassword
#undef PQconnectionNeedsPassword
#undef PQsslInUse
#undef PQsetnonblocking
#undef PQfinish
#undef PQsendQuery
#undef PQflush
#undef PQconsumeInput
#undef PQisBusy
#undef PQgetResult
#undef PQsocket
#undef PQerrorMessage

static uint64_t scripted_monotonic_ms(void) {
  return clock_ms;
}

static int scripted_poll(struct pollfd *pfds, nfds_t count, int timeout_ms) {
  (void)pfds;
  (void)count;
  clock_ms += (uint64_t)timeout_ms;
  if (timeout_ms > 0) {
    // A loop throttled by its CPU quota wakes up late.
    clock_ms += wake_latency_ms;
  }
  return 0;
}

#define monotonic_ms scripted_monotonic_ms
#define poll scripted_poll
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/pg_monitor.c"
#undef monotonic_ms
#undef poll

/** Resets the hosts, the scheduler, and every scripted libpq setting. */
static void configure_hosts(const unsigned int count) {
  host_count = count;
  clock_ms = 10000;
  start_cost_ms = 0;
  connect_poll_cost_ms = 0;
  wake_latency_ms = 0;
  parameters.sleep_ms = 1000;
  parameters.query_timeout_ms = 1000;
  parameters.max_fails = 3;
  parameters.conn_max_age_ms = 300000;
  next_connect_at_ms = 0;
  next_session_setup_at_ms = 0;
  memset(connection_status, 0, sizeof(connection_status));
  memset(connect_poll_results, 0, sizeof(connect_poll_results));
  memset(start_fails, 0, sizeof(start_fails));
  memset(start_connects, 0, sizeof(start_connects));
  memset(tcp_refused, 0, sizeof(tcp_refused));
  memset(reconnects, 0, sizeof(reconnects));
  memset(used_password, 0, sizeof(used_password));
  memset(needs_password, 0, sizeof(needs_password));
  memset(ssl_in_use, 0, sizeof(ssl_in_use));
  memset(connects, 0, sizeof(connects));
  memset(closes, 0, sizeof(closes));
  memset(tcp_polls, 0, sizeof(tcp_polls));
  memset(connect_polls, 0, sizeof(connect_polls));
  connect_count = 0;
  for (unsigned int i = 0; i < count; i++) {
    monitor_host_list[i] = (MonitorHost){.host = "test", .pollfd_slot = -1};
  }
  save_master_index(-1);
}

static size_t host_index(const MonitorHost *host) {
  return (size_t)(host - monitor_host_list);
}

/** Completes the current check of `host` the way a polled response would. */
static void finish_check(MonitorHost *host) {
  if (host->poll_state == HOST_POLL_CONNECTING) {
    host->connected_at_ms = clock_ms;
  }
  connection_status[host_index(host)] = CONNECTION_OK;
  host->iter_data_ready = true;
  host->iter_new_status = (MonitorStatus){.alive = true};
  finish_iteration(host, true, clock_ms);
}

/** Gives every host an open connection created at `connected_at_ms`. */
static void connect_hosts(const uint64_t connected_at_ms) {
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    host->conn = &fake_connections[i];
    host->connected_at_ms = connected_at_ms;
    host->next_poll_at_ms = clock_ms;
    connection_status[i] = CONNECTION_OK;
  }
}

/**
 * Makes host `i` an alive master with a working connection whose next check
 * never becomes due, so that a master is known throughout the test.
 */
static void keep_master(const unsigned int i) {
  MonitorHost *host = &monitor_host_list[i];
  host->conn = &fake_connections[i];
  connection_status[i] = CONNECTION_OK;
  host->next_poll_at_ms = UINT64_MAX;
  publish_monitor_snapshot(host, (MonitorSnapshot){.status = master_status()});
  recompute_master_index();
}

/** Due time of a check given a fresh snapshot of the connection turn. */
static uint64_t due_at(const MonitorHost *host) {
  const ConnectionTurn turn = connection_turn();
  return host_poll_due_at_ms(host, &turn);
}

static int wait_timeout(void) {
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  return timeout_ms;
}

static void run_until(const uint64_t end_ms) {
  for (int i = 0; i < 6000 && clock_ms < end_ms; i++) {
    (void)pump_one_iteration();
  }
}

/** Marks the connection of host `i` as past its TLS handshake. */
static void script_handshake(const unsigned int i) {
  ssl_in_use[i] = true;
}

/** Delivers the TCP connect result of host `i`, as a writable socket does. */
static void tcp_connected(const unsigned int i) {
  advance_host_poll(&monitor_host_list[i], clock_ms);
}

/**
 * Completes the TCP connection of host `i` and starts its session setup,
 * unless another setup holds the turn, in which case the host queues.
 */
static void begin_session_setup(const unsigned int i) {
  tcp_connected(i);
  connect_poll_results[i] = PGRES_POLLING_READING;
  advance_host_poll(&monitor_host_list[i], clock_ms);
}

static void test_startup_connections_are_spaced(void) {
  // Arrange
  configure_hosts(4);

  // Act & Assert
  for (unsigned int i = 0; i < 4; i++) {
    start_and_timeout_hosts();
    support_assert_true(connect_count == i + 1, "one connection per slot");
    support_assert_true(
      connect_times[i] == 10000 + ((uint64_t)i * 250),
      "connections spread across the period"
    );
    support_assert_true(
      !all_hosts_have_polled(), "a delayed first check delays readiness"
    );
    if (i < 3) {
      support_assert_true(wait_timeout() == 250, "wake for the next slot");
      clock_ms += 250;
    }
  }
  for (unsigned int i = 0; i < 4; i++) {
    finish_check(&monitor_host_list[i]);
  }
  support_assert_true(all_hosts_have_polled(), "every host has polled");
}

static void test_connect_interval(void) {
  // Arrange
  configure_hosts(4);

  // Act & Assert
  support_assert_true(connect_interval_ms() == 250, "period split by hosts");
  host_count = 10;
  support_assert_true(connect_interval_ms() == 100, "many hosts fit a period");
  host_count = 2;
  parameters.sleep_ms = 60000;
  support_assert_true(
    connect_interval_ms() == 250, "a long period does not delay startup"
  );
  host_count = 3;
  parameters.sleep_ms = 2;
  support_assert_true(connect_interval_ms() == 0, "tiny period disables it");
}

static void test_slow_lookup_keeps_spacing(void) {
  // Arrange
  configure_hosts(2);
  start_cost_ms = 300;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(clock_ms == 10300, "first start blocks on lookup");
  support_assert_true(connect_count == 1, "second host waits");
  support_assert_true(
    due_at(&monitor_host_list[1]) == 10550,
    "spacing counts from the end of the lookup"
  );
  support_assert_true(wait_timeout() == 250, "wake for the next turn");

  // Act
  start_cost_ms = 0;
  clock_ms = 10550;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connect_count == 2 && connect_times[1] == 10550, "second host connects"
  );
}

static void test_connection_turn_snapshot(void) {
  // Arrange: a healthy host, and two hosts without a connection.
  configure_hosts(3);
  connect_hosts(clock_ms);
  monitor_host_list[1].conn = nullptr;
  monitor_host_list[1].next_poll_at_ms = 10000;
  monitor_host_list[2].conn = nullptr;
  monitor_host_list[2].next_poll_at_ms = 9000;

  // Act & Assert
  ConnectionTurn turn = connection_turn();
  support_assert_true(
    turn.first_waiting == &monitor_host_list[2], "the host due first waits"
  );
  support_assert_true(
    turn.session_setup_due_at_ms == 0 && turn.connect_at_ms == 0, "turn is free"
  );
  support_assert_true(
    host_poll_due_at_ms(&monitor_host_list[0], &turn) == 10000,
    "healthy host keeps its schedule"
  );
  support_assert_true(
    host_poll_due_at_ms(&monitor_host_list[1], &turn) == UINT64_MAX,
    "the other waiting host yields"
  );
  support_assert_true(
    host_poll_due_at_ms(&monitor_host_list[2], &turn) == 9000,
    "the first waiting host is due at its schedule"
  );

  // Arrange: equal due times, a setup in progress, and a spaced start.
  monitor_host_list[2].next_poll_at_ms = 10000;
  monitor_host_list[0].poll_state = HOST_POLL_CONNECTING;
  monitor_host_list[0].session_setup_hold_until_ms = 12000;
  next_connect_at_ms = 12500;

  // Act & Assert
  turn = connection_turn();
  support_assert_true(
    turn.first_waiting == &monitor_host_list[1], "ties go in list order"
  );
  support_assert_true(
    turn.session_setup_due_at_ms == 12000 && turn.connect_at_ms == 12500,
    "holds and spacing are both in the snapshot"
  );
  support_assert_true(
    host_poll_due_at_ms(&monitor_host_list[1], &turn) == 12500,
    "the first waiting host waits for the turn"
  );

  // Arrange: the first waiting host is no longer idle.
  monitor_host_list[1].poll_state = HOST_POLL_CONNECTING;

  // Act & Assert
  turn = connection_turn();
  support_assert_true(
    turn.first_waiting == &monitor_host_list[2],
    "only idle hosts wait for a turn"
  );
}

static void test_failed_start_takes_no_turn(void) {
  // Arrange
  configure_hosts(2);
  start_fails[0] = true;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connects[0] == 1 && closes[0] == 1 &&
      monitor_host_list[0].failed_connections == 1,
    "failed start counts as a failed check"
  );
  support_assert_true(
    connects[1] == 1 && connect_times[1] == 10000,
    "next host connects without waiting"
  );
}

static void test_ready_tcp_setups_take_turns(void) {
  // Arrange: two hosts reconnect at once, alongside a healthy connection.
  configure_hosts(3);
  parameters.sleep_ms = 1;
  connect_hosts(clock_ms);
  monitor_host_list[0].conn = nullptr;
  monitor_host_list[1].conn = nullptr;
  script_handshake(0);
  start_and_timeout_hosts();

  // Act: both TCP connections complete in the same wake-up.
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  pfds[1].revents = POLLOUT;
  pfds[2].revents = POLLOUT;
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    tcp_polls[0] == 1 && tcp_polls[1] == 1, "both TCP results are read"
  );
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_CONNECTING &&
      monitor_host_list[1].poll_state == HOST_POLL_SESSION_WAIT,
    "the second ready socket waits for the setup turn"
  );
  support_assert_true(
    build_poll_fd(clock_ms, pfds, &timeout_ms) == 3 &&
      monitor_host_list[1].pollfd_slot == -1 && timeout_ms == 1000,
    "queued writable socket is omitted instead of spinning"
  );

  // Act: the first host sends its startup packet.
  connect_poll_results[0] = PGRES_POLLING_READING;
  pfds[1].revents = POLLOUT;
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    connect_polls[0] == 1 && connect_polls[1] == 0,
    "only one ready TCP socket enters TLS"
  );

  // Act: the first setup completes; the healthy host keeps polling.
  clock_ms = 10100;
  connect_poll_results[0] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[0], clock_ms);
  finish_check(&monitor_host_list[2]);
  clock_ms = 10199;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connect_polls[1] == 0, "queued setup keeps the pause");
  support_assert_true(
    monitor_host_list[2].poll_state == HOST_POLL_QUERY_READ,
    "healthy host continues polling while another setup waits"
  );
  support_assert_true(wait_timeout() == 1, "wake at the end of the pause");

  // Act
  clock_ms = 10200;
  connect_poll_results[1] = PGRES_POLLING_READING;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connect_polls[1] == 1, "queued setup resumes on time");
  support_assert_true(
    monitor_host_list[1].iter_deadline_ms == 11200,
    "waiting for the setup turn does not consume the poll timeout"
  );
}

static void test_queued_setups_resume_in_queue_order(void) {
  // Arrange: four hosts reconnect at once. The first takes the setup turn;
  // the other TCP connections complete in reverse list order and queue.
  configure_hosts(4);
  parameters.sleep_ms = 1;
  for (unsigned int i = 0; i < 4; i++) {
    script_handshake(i);
    connect_poll_results[i] = PGRES_POLLING_READING;
  }
  start_and_timeout_hosts();
  begin_session_setup(0);
  for (unsigned int i = 3; i > 0; i--) {
    clock_ms++;
    tcp_connected(i);
  }
  support_assert_true(
    connection_turn().first_session_waiter == &monitor_host_list[3],
    "the socket that became ready first leads the queue"
  );

  // Act & Assert: each setup takes 100 ms and is followed by the pause.
  const unsigned int order[] = {0, 3, 2, 1};
  for (unsigned int k = 1; k < 4; k++) {
    MonitorHost *previous = &monitor_host_list[order[k - 1]];
    clock_ms = 10000 + ((uint64_t)k * 200) - 100;
    connect_poll_results[order[k - 1]] = PGRES_POLLING_OK;
    advance_host_poll(previous, clock_ms);
    clock_ms += 99;
    start_and_timeout_hosts();
    support_assert_true(
      connect_polls[order[k]] == 0, "the next setup keeps the pause"
    );
    clock_ms++;
    start_and_timeout_hosts();
    support_assert_true(
      connect_polls[order[k]] == 1 &&
        monitor_host_list[order[k]].session_setup_hold_until_ms ==
          clock_ms + 1000,
      "the socket that became ready first takes the turn"
    );
    for (unsigned int later = k + 1; later < 4; later++) {
      support_assert_true(
        connect_polls[order[later]] == 0 &&
          monitor_host_list[order[later]].poll_state == HOST_POLL_SESSION_WAIT,
        "sockets that became ready later keep waiting"
      );
    }
  }
}

static void test_ready_socket_queues_behind_waiting_one(void) {
  // Arrange: the first host holds the setup turn, the second host's socket
  // waits for it.
  configure_hosts(3);
  parameters.sleep_ms = 1;
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);
  clock_ms = 10010;
  tcp_connected(1);
  clock_ms = 10100;
  connect_poll_results[0] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Act: the third host's TCP connection completes in the wake-up that ends
  // the pause after the first setup.
  clock_ms = 10200;
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  pfds[monitor_host_list[2].pollfd_slot].revents = POLLOUT;
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    tcp_polls[2] == 1 &&
      monitor_host_list[2].poll_state == HOST_POLL_SESSION_WAIT,
    "a socket that becomes ready queues behind the one already waiting"
  );

  // Act
  connect_poll_results[1] = PGRES_POLLING_READING;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connect_polls[1] == 1 && connect_polls[2] == 0,
    "the socket that waited longer goes first"
  );
}

static void test_new_connection_waits_behind_ready_socket(void) {
  // Arrange: the second host holds the setup turn, the third host's socket
  // waits for it. The first host's check is due later, and libpq will
  // return from its start with TCP already connected.
  configure_hosts(3);
  parameters.sleep_ms = 1;
  monitor_host_list[0].next_poll_at_ms = 10150;
  start_connects[0] = true;
  script_handshake(1);
  start_and_timeout_hosts();
  begin_session_setup(1);
  clock_ms = 10010;
  tcp_connected(2);
  clock_ms = 10100;
  connect_poll_results[1] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[1], clock_ms);
  clock_ms = 10200;

  // Act & Assert
  support_assert_true(
    connects[0] == 0 && due_at(&monitor_host_list[0]) == UINT64_MAX,
    "no connection starts while a ready socket waits for its turn"
  );
  connect_poll_results[2] = PGRES_POLLING_READING;
  start_and_timeout_hosts();
  support_assert_true(
    connect_polls[2] == 1 && connects[0] == 0, "the waiting socket goes first"
  );
  support_assert_true(
    due_at(&monitor_host_list[0]) == 11200,
    "the new connection waits for the resumed setup"
  );
}

static void test_setup_hold_starts_after_dns(void) {
  // Arrange: the first start blocks on a lookup for longer than the hold.
  configure_hosts(2);
  start_cost_ms = 1500;
  start_and_timeout_hosts();
  start_cost_ms = 0;
  begin_session_setup(0);

  // Act
  clock_ms = 11750;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connects[1] == 0, "DNS does not consume the setup hold");
  support_assert_true(
    due_at(&monitor_host_list[1]) == 12500,
    "setup holds the turn for a second after DNS returns"
  );
}

static void test_queued_setup_survives_hung_predecessor(void) {
  // Arrange: the first host hangs after its TLS handshake.
  configure_hosts(2);
  parameters.sleep_ms = 1;
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);
  begin_session_setup(1);

  // Act
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].failed_connections == 1 &&
      monitor_host_list[1].failed_connections == 0 && connect_polls[1] == 0,
    "only the hung setup times out, the queued socket keeps waiting"
  );
  support_assert_true(wait_timeout() == 100, "timeout is followed by a pause");

  // Act
  clock_ms = 11100;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connect_polls[1] == 1 && monitor_host_list[1].iter_deadline_ms == 12100,
    "queued setup gets its full timeout after the predecessor fails"
  );
}

static void test_hung_setup_without_handshake_adds_no_pause(void) {
  // Arrange: the first host's server never answers the startup packet.
  configure_hosts(2);
  parameters.sleep_ms = 1;
  start_and_timeout_hosts();
  begin_session_setup(0);
  begin_session_setup(1);

  // Act
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].failed_connections == 1 && connect_polls[1] == 1,
    "queued setup resumes as soon as the hung one times out"
  );
  support_assert_true(
    next_session_setup_at_ms == 0 &&
      monitor_host_list[1].iter_deadline_ms == 12000,
    "a setup that spent no CPU adds no pause"
  );
}

static void test_queued_setup_proceeds_after_hold(void) {
  // Arrange
  configure_hosts(2);
  parameters.sleep_ms = 1;
  parameters.query_timeout_ms = 60000;
  start_and_timeout_hosts();
  begin_session_setup(0);
  begin_session_setup(1);
  support_assert_true(wait_timeout() == 1000, "wake when the hold expires");

  // Act
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connect_polls[1] == 1 && monitor_host_list[0].failed_connections == 0,
    "queued setup proceeds before the hung predecessor's long timeout"
  );
}

static void test_stall_preserves_setup_hold_and_wait(void) {
  // Arrange
  configure_hosts(2);
  parameters.sleep_ms = 1;
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);
  begin_session_setup(1);
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  pfds[1].revents = POLLIN;

  // Act: a libpq step blocks (for example, a GSSAPI name lookup) while
  // another socket is queued. The active hold must survive that stall, and
  // the queued socket must exclude the same time only once.
  clock_ms = 10100;
  connect_poll_cost_ms = 1500;
  process_poll_result(pfds);

  // Assert
  support_assert_true(
    clock_ms == 11600 && wait_timeout() == 900,
    "blocking libpq step consumes neither the setup hold nor the timeout"
  );
  support_assert_true(
    monitor_host_list[1].poll_state == HOST_POLL_SESSION_WAIT,
    "other setup remains queued after a long stall"
  );

  // Act
  connect_poll_cost_ms = 0;
  connect_poll_results[0] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[0], clock_ms);
  clock_ms = 11700;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connect_polls[1] == 1 && monitor_host_list[1].iter_deadline_ms == 12700,
    "queued time includes the stall exactly once"
  );
}

static void test_immediate_setup_success_keeps_pause(void) {
  // Arrange
  configure_hosts(2);
  parameters.sleep_ms = 1;
  script_handshake(0);
  start_and_timeout_hosts();

  // Act: the first setup completes in one libpq step.
  tcp_connected(0);
  connect_poll_results[0] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[0], clock_ms);
  begin_session_setup(1);

  // Assert
  support_assert_true(
    connect_polls[1] == 0 && wait_timeout() == 100,
    "a setup completed in one libpq call still leaves a pause"
  );
}

static void test_session_setup_holds_turn(void) {
  // Arrange: the first host's TCP connection is up, authentication is not.
  configure_hosts(2);
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);
  clock_ms = 10600;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connects[1] == 0, "no second setup at the same time");
  support_assert_true(
    due_at(&monitor_host_list[1]) == 11000,
    "setup holds the turn for at most a second"
  );
  support_assert_true(
    wait_timeout() == 400, "wait for the setup socket or its deadline"
  );

  // Act
  clock_ms = 10700;
  connect_poll_results[0] = PGRES_POLLING_OK;
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Assert
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_QUERY_READ,
    "connected host sends its query"
  );
  support_assert_true(
    due_at(&monitor_host_list[1]) == 10800,
    "next connection pauses after the setup"
  );

  // Act
  clock_ms = 10799;
  start_and_timeout_hosts();
  support_assert_true(connects[1] == 0, "pause is kept");
  clock_ms = 10800;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connects[1] == 1, "second host connects after pause");
}

static void test_pause_follows_session_setup_only(void) {
  // Arrange
  configure_hosts(2);
  start_and_timeout_hosts();
  clock_ms = 10200;
  tcp_refused[0] = true;

  // Act: refused while waiting for TCP
  tcp_connected(0);

  // Assert
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_IDLE &&
      next_connect_at_ms == 10250,
    "a refused connection fails at once and adds no pause"
  );

  // Arrange
  clock_ms = 10250;
  start_and_timeout_hosts();
  begin_session_setup(1);
  used_password[1] = true;
  clock_ms = 10600;
  connect_poll_results[1] = PGRES_POLLING_FAILED;

  // Act: rejected after authentication
  advance_host_poll(&monitor_host_list[1], clock_ms);

  // Assert
  support_assert_true(
    connection_turn_at_ms() == 10700, "a rejected setup is followed by a pause"
  );
  support_assert_true(
    monitor_host_list[0].handshake_failures == 0 &&
      monitor_host_list[1].handshake_failures == 1,
    "only the failure after authentication is costly"
  );
}

static void test_refused_tcp_fails_during_setup_hold(void) {
  // Arrange: one host is in TLS, the other's port is closed.
  configure_hosts(2);
  parameters.sleep_ms = 1;
  start_and_timeout_hosts();
  begin_session_setup(0);
  tcp_refused[1] = true;

  // Act
  clock_ms = 10050;
  tcp_connected(1);

  // Assert
  support_assert_true(
    monitor_host_list[1].poll_state == HOST_POLL_IDLE &&
      monitor_host_list[1].failed_connections == 1,
    "a refused connection is reported while another host holds the turn"
  );
  support_assert_true(
    tcp_polls[1] == 1 && connect_polls[1] == 0 &&
      monitor_host_list[1].handshake_failures == 0,
    "reading the TCP result costs no setup step"
  );
}

static void test_reconnect_releases_hold(void) {
  // Arrange: the server completes TLS, then libpq drops the connection and
  // connects again.
  configure_hosts(2);
  start_and_timeout_hosts();
  begin_session_setup(0);
  script_handshake(0);
  advance_host_poll(&monitor_host_list[0], clock_ms);
  reconnects[0] = true;
  connect_poll_results[0] = PGRES_POLLING_FAILED;

  // Act: libpq waits for TCP again
  clock_ms = 10300;
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Assert
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_CONNECTING &&
      monitor_host_list[0].session_setup_hold_until_ms == 0,
    "waiting for TCP again releases the hold"
  );
  support_assert_true(
    due_at(&monitor_host_list[1]) == 10400,
    "the dropped handshake is followed by a pause"
  );

  // Act
  clock_ms = 10400;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connects[1] == 1, "the next host connects after it");
}

static void test_connect_completing_at_start_holds_turn(void) {
  // Arrange: libpq returns from the start with TCP already connected.
  configure_hosts(2);
  start_connects[0] = true;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].session_setup_hold_until_ms == 11000,
    "session setup holds the turn from the start"
  );
  support_assert_true(
    connects[1] == 0 && due_at(&monitor_host_list[1]) == 11000,
    "the next host waits for the hold"
  );
}

static void test_hung_setup_does_not_jump_the_queue(void) {
  // Arrange: the first host's server accepts TCP, finishes TLS, then hangs.
  configure_hosts(2);
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);

  // Act
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_IDLE && connects[1] == 0,
    "hung setup times out"
  );
  support_assert_true(
    due_at(&monitor_host_list[1]) == 11100, "waiting host goes after the pause"
  );
  support_assert_true(
    due_at(&monitor_host_list[0]) == UINT64_MAX,
    "the host due later goes after it"
  );

  // Act
  clock_ms = 11100;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connects[0] == 1 && connects[1] == 1, "the waiting host gets the turn"
  );
  support_assert_true(
    due_at(&monitor_host_list[0]) == 11350,
    "the timed out host takes the next turn"
  );
}

static void test_hung_setup_holds_turn_briefly(void) {
  // Arrange: the server accepts TCP and then never answers.
  configure_hosts(2);
  parameters.query_timeout_ms = 60000;
  start_and_timeout_hosts();
  begin_session_setup(0);

  // Act
  clock_ms = 10999;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(connects[1] == 0, "setup holds the turn");
  support_assert_true(wait_timeout() == 1, "wake when the hold ends");

  // Act
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    connects[1] == 1 && monitor_host_list[0].poll_state == HOST_POLL_CONNECTING,
    "next host connects while the hung setup continues"
  );
}

static void test_late_wakeups_do_not_add_up(void) {
  // Arrange: four unreachable hosts and a loop that wakes up 50 ms late.
  configure_hosts(4);
  parameters.max_fails = 1000;
  parameters.query_timeout_ms = 500;
  wake_latency_ms = 50;

  // Act
  run_until(30000);

  // Assert: each host is late only by its own wake-up, not by the others'.
  support_assert_true(connect_count > 70, "hosts keep retrying");
  for (unsigned int i = 1; i < connect_count; i++) {
    for (unsigned int j = i; j-- > 0;) {
      if (connect_host_indexes[j] == connect_host_indexes[i]) {
        support_assert_true(
          connect_times[i] - connect_times[j] <= 1000 + wake_latency_ms,
          "late wake-ups do not lengthen the period"
        );
        break;
      }
    }
  }
}

static void test_aged_connection_replaced_when_turn_free(void) {
  // Arrange
  configure_hosts(2);
  clock_ms = 400000;
  connect_hosts(0);

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 1 && connects[0] == 1, "first aged connection is replaced"
  );
  support_assert_true(
    closes[1] == 0 && connects[1] == 0 &&
      monitor_host_list[1].poll_state == HOST_POLL_QUERY_READ,
    "second check runs on its aged connection while the turn is busy"
  );

  // Act
  finish_check(&monitor_host_list[0]);
  finish_check(&monitor_host_list[1]);

  // Assert
  support_assert_true(
    closes[1] == 0, "finishing a check does not close an aged connection"
  );

  // Act
  clock_ms += 1000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 1 && connects[0] == 1, "replacement is reused"
  );
  support_assert_true(
    closes[1] == 1 && connects[1] == 1, "next check replaces it"
  );
}

static void test_broken_connection_goes_before_recycle(void) {
  // Arrange
  configure_hosts(2);
  clock_ms = 400000;
  connect_hosts(0);
  connection_status[1] = CONNECTION_BAD;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[1] == 1 && connects[1] == 1, "broken connection takes the turn"
  );
  support_assert_true(
    closes[0] == 0 && connects[0] == 0 &&
      monitor_host_list[0].poll_state == HOST_POLL_QUERY_READ,
    "aged connection serves the check meanwhile"
  );

  // Act
  finish_check(&monitor_host_list[0]);
  finish_check(&monitor_host_list[1]);
  clock_ms = 401000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 1 && connects[0] == 1, "next check replaces it"
  );
}

static void test_aged_connection_keeps_serving_until_turn_free(void) {
  // Arrange: another connection has just started when the age is reached.
  configure_hosts(1);
  clock_ms = 400000;
  connect_hosts(0);
  next_connect_at_ms = 400100;

  // Act
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 0 && monitor_host_list[0].poll_state == HOST_POLL_QUERY_READ,
    "busy turn postpones the replacement, not the check"
  );

  // Act: the turn is busy again when the next check is due.
  finish_check(&monitor_host_list[0]);
  next_connect_at_ms = 401200;
  clock_ms = 401000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 0 && monitor_host_list[0].poll_state == HOST_POLL_QUERY_READ,
    "the check runs on the aged connection again"
  );

  // Act
  finish_check(&monitor_host_list[0]);
  clock_ms = 402000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    closes[0] == 1 && connects[0] == 1,
    "aged connection is replaced once the turn is free"
  );
}

static void test_failed_hosts_reconnect_spaced(void) {
  // Arrange: four hosts start together, then stop answering. They stay
  // possibly dead, so the longer period of dead hosts does not apply.
  configure_hosts(4);
  parameters.max_fails = 1000;
  for (int i = 0; i < 6000; i++) {
    (void)pump_one_iteration();
    if (clock_ms >= 30000) {
      break;
    }
  }

  // Assert
  support_assert_true(connect_count > 70, "hosts keep retrying");
  for (unsigned int i = 1; i < connect_count; i++) {
    support_assert_true(
      connect_times[i] - connect_times[i - 1] >= 250,
      "reconnects never start in the same slot"
    );
  }
  for (unsigned int i = 0; i < 4; i++) {
    support_assert_true(
      connects[i] >= 19, "every host still retries about once per period"
    );
    support_assert_true(
      atomic_get_status(&monitor_host_list[i]).possible_dead,
      "failed hosts are possibly dead"
    );
  }
}

static void test_dead_hosts_retry_less_often(void) {
  // Arrange: every attempt times out while the scripted libpq reports a
  // used password, so each failure counts as one after authentication. The
  // fifth host is a working master.
  configure_hosts(5);
  keep_master(4);
  for (unsigned int i = 0; i < 4; i++) {
    used_password[i] = true;
  }

  // Act: 40 seconds without answers
  run_until(50000);

  // Assert: attempts at 0, 1, 2, then 4, 8, 16, 24 and 32 s after the start
  for (unsigned int i = 0; i < 4; i++) {
    support_assert_true(connects[i] == 8, "dead host period grows to 8 s");
    support_assert_true(
      !atomic_get_status(&monitor_host_list[i]).alive, "failed hosts are dead"
    );
  }
  support_assert_true(get_master_index() == 4, "the master stays known");
  for (unsigned int i = 1; i < connect_count; i++) {
    support_assert_true(
      connect_times[i] - connect_times[i - 1] >= 200, "reconnects stay spaced"
    );
  }
}

static void test_dead_hosts_without_master_keep_period(void) {
  // Arrange: as above, but no host is a master, so any of them may be the
  // one that brings it back.
  configure_hosts(4);
  for (unsigned int i = 0; i < 4; i++) {
    used_password[i] = true;
  }

  // Act: 40 seconds without answers
  run_until(50000);

  // Assert
  for (unsigned int i = 0; i < 4; i++) {
    support_assert_true(
      connects[i] == 40 && monitor_host_list[i].handshake_failures >= 3,
      "costly failures keep the normal period without a master"
    );
  }
}

static void test_lost_master_cancels_backoff(void) {
  // Arrange: while another host is the master, a dead host fails after
  // authentication for the fifth time in a row, inside a libpq step that
  // blocks the loop for 300 ms.
  configure_hosts(2);
  keep_master(1);
  MonitorHost *host = &monitor_host_list[0];
  host->failed_connections = 4;
  host->handshake_failures = 4;
  used_password[0] = true;
  start_and_timeout_hosts();
  begin_session_setup(0);
  struct pollfd pfds[MAX_HOSTS + 1];
  int timeout_ms;
  (void)build_poll_fd(clock_ms, pfds, &timeout_ms);
  pfds[host->pollfd_slot].revents = POLLIN;
  connect_poll_results[0] = PGRES_POLLING_FAILED;
  connect_poll_cost_ms = 300;
  process_poll_result(pfds);
  support_assert_true(
    host->failed_connections == 5 && host->next_poll_at_ms == 18300,
    "the dead host waits 8 s, plus the blocked time, while a master is known"
  );

  // Act: the master fails
  publish_monitor_snapshot(
    &monitor_host_list[1], (MonitorSnapshot){.status = dead_status()}
  );
  recompute_master_index();

  // Assert
  support_assert_true(
    host->next_poll_at_ms == 11300 && due_at(host) == 11300,
    "without a master the normal period applies at once, blocked time kept"
  );
}

static void test_unreachable_dead_hosts_keep_period(void) {
  // Arrange: no server answers, so no attempt reaches authentication.
  configure_hosts(4);

  // Act: 40 seconds without answers
  run_until(50000);

  // Assert
  for (unsigned int i = 0; i < 4; i++) {
    support_assert_true(connects[i] == 40, "dead host is checked each period");
    support_assert_true(
      !atomic_get_status(&monitor_host_list[i]).alive, "failed hosts are dead"
    );
  }
}

static uint64_t dead_host_period(
  const unsigned int failures, const uint16_t handshakes
) {
  const MonitorHost host = {
    .failed_connections = failures, .handshake_failures = handshakes
  };
  return poll_period_ms(&host);
}

static void test_dead_host_period(void) {
  // Arrange
  configure_hosts(1);
  save_master_index(0);

  // Act & Assert
  support_assert_true(dead_host_period(2, 2) == 1000, "possibly dead");
  support_assert_true(dead_host_period(3, 3) == 2000, "doubles once dead");
  support_assert_true(dead_host_period(4, 4) == 4000, "doubles again");
  support_assert_true(dead_host_period(5, 5) == 8000, "reaches 8 s");
  support_assert_true(dead_host_period(50, 50) == 8000, "stays at 8 s");
  support_assert_true(dead_host_period(50, 0) == 1000, "cheap failure resets");
  support_assert_true(dead_host_period(50, 1) == 2000, "then doubles again");
  parameters.sleep_ms = 3000;
  support_assert_true(dead_host_period(3, 3) == 6000, "long period doubles");
  support_assert_true(dead_host_period(4, 4) == 8000, "capped at 8 s");
  parameters.sleep_ms = 10000;
  support_assert_true(
    dead_host_period(50, 50) == 10000, "period above the cap is kept"
  );
  parameters.sleep_ms = 100;
  support_assert_true(dead_host_period(8, 8) == 6400, "short period doubles");
  support_assert_true(dead_host_period(9, 9) == 8000, "up to 8 s");
  save_master_index(-1);
  support_assert_true(
    dead_host_period(9, 9) == 100, "no growth while no master is known"
  );
}

static void test_reused_connection_failure_is_cheap(void) {
  // Arrange: an authenticated TLS connection from an earlier check, and the
  // first failure already makes the host dead while a master is known.
  configure_hosts(2);
  parameters.max_fails = 1;
  connect_hosts(clock_ms);
  keep_master(1);
  used_password[0] = true;
  script_handshake(0);
  start_and_timeout_hosts();
  support_assert_true(
    monitor_host_list[0].poll_state == HOST_POLL_QUERY_READ,
    "check reuses the connection"
  );

  // Act: the query times out
  clock_ms = 11000;
  start_and_timeout_hosts();

  // Assert
  support_assert_true(
    monitor_host_list[0].failed_connections == 1 &&
      monitor_host_list[0].handshake_failures == 0,
    "a failure on a reused connection cost no handshake"
  );
  support_assert_true(
    monitor_host_list[0].next_poll_at_ms == 11000,
    "the period does not grow before a costly reconnect"
  );
}

static void test_missing_password_is_cheap(void) {
  // Arrange: libpq has no password for the server's request. pg-status
  // always passes one, so this covers only the guard in did_handshake().
  configure_hosts(1);
  used_password[0] = true;
  needs_password[0] = true;
  start_and_timeout_hosts();
  begin_session_setup(0);
  clock_ms = 10300;
  connect_poll_results[0] = PGRES_POLLING_FAILED;

  // Act
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Assert
  support_assert_true(
    monitor_host_list[0].failed_connections == 1 &&
      monitor_host_list[0].handshake_failures == 0,
    "giving up without a password costs no handshake"
  );
  support_assert_true(connection_turn_at_ms() == 10250, "and adds no pause");
}

static void test_tls_only_rejection_is_costly(void) {
  // Arrange: the server completes TLS and rejects without asking a password.
  configure_hosts(1);
  script_handshake(0);
  start_and_timeout_hosts();
  begin_session_setup(0);
  clock_ms = 10300;
  connect_poll_results[0] = PGRES_POLLING_FAILED;

  // Act
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Assert
  support_assert_true(
    monitor_host_list[0].handshake_failures == 1,
    "a TLS handshake counts as a costly failure"
  );
  support_assert_true(
    connection_turn_at_ms() == 10400, "and is followed by a pause"
  );
}

static void test_tls_handshake_failure_is_costly(void) {
  // Arrange: the server accepts TLS, and the handshake then fails. libpq
  // drops its TLS state together with the failure.
  configure_hosts(1);
  start_and_timeout_hosts();
  begin_session_setup(0);
  script_handshake(0);
  advance_host_poll(&monitor_host_list[0], clock_ms);
  ssl_in_use[0] = false;
  clock_ms = 10300;
  connect_poll_results[0] = PGRES_POLLING_FAILED;

  // Act
  advance_host_poll(&monitor_host_list[0], clock_ms);

  // Assert
  support_assert_true(
    monitor_host_list[0].handshake_failures == 1,
    "a handshake that fails midway counts as a costly failure"
  );
  support_assert_true(
    connection_turn_at_ms() == 10400, "and is followed by a pause"
  );
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  const struct {
    const char *name;
    support_action_t run;
  } cases[] = {
    {"startup_connections_are_spaced", test_startup_connections_are_spaced},
    {"connect_interval", test_connect_interval},
    {"slow_lookup_keeps_spacing", test_slow_lookup_keeps_spacing},
    {"connection_turn_snapshot", test_connection_turn_snapshot},
    {"failed_start_takes_no_turn", test_failed_start_takes_no_turn},
    {"ready_tcp_setups_take_turns", test_ready_tcp_setups_take_turns},
    {"queued_setups_resume_in_queue_order",
     test_queued_setups_resume_in_queue_order},
    {"ready_socket_queues_behind_waiting_one",
     test_ready_socket_queues_behind_waiting_one},
    {"new_connection_waits_behind_ready_socket",
     test_new_connection_waits_behind_ready_socket},
    {"setup_hold_starts_after_dns", test_setup_hold_starts_after_dns},
    {"queued_setup_survives_hung_predecessor",
     test_queued_setup_survives_hung_predecessor},
    {"hung_setup_without_handshake_adds_no_pause",
     test_hung_setup_without_handshake_adds_no_pause},
    {"queued_setup_proceeds_after_hold", test_queued_setup_proceeds_after_hold},
    {"stall_preserves_setup_hold_and_wait",
     test_stall_preserves_setup_hold_and_wait},
    {"immediate_setup_success_keeps_pause",
     test_immediate_setup_success_keeps_pause},
    {"session_setup_holds_turn", test_session_setup_holds_turn},
    {"pause_follows_session_setup_only", test_pause_follows_session_setup_only},
    {"refused_tcp_fails_during_setup_hold",
     test_refused_tcp_fails_during_setup_hold},
    {"reconnect_releases_hold", test_reconnect_releases_hold},
    {"connect_completing_at_start_holds_turn",
     test_connect_completing_at_start_holds_turn},
    {"hung_setup_does_not_jump_the_queue",
     test_hung_setup_does_not_jump_the_queue},
    {"hung_setup_holds_turn_briefly", test_hung_setup_holds_turn_briefly},
    {"late_wakeups_do_not_add_up", test_late_wakeups_do_not_add_up},
    {"aged_connection_replaced_when_turn_free",
     test_aged_connection_replaced_when_turn_free},
    {"broken_connection_goes_before_recycle",
     test_broken_connection_goes_before_recycle},
    {"aged_connection_keeps_serving_until_turn_free",
     test_aged_connection_keeps_serving_until_turn_free},
    {"failed_hosts_reconnect_spaced", test_failed_hosts_reconnect_spaced},
    {"dead_hosts_retry_less_often", test_dead_hosts_retry_less_often},
    {"dead_hosts_without_master_keep_period",
     test_dead_hosts_without_master_keep_period},
    {"lost_master_cancels_backoff", test_lost_master_cancels_backoff},
    {"unreachable_dead_hosts_keep_period",
     test_unreachable_dead_hosts_keep_period},
    {"dead_host_period", test_dead_host_period},
    {"reused_connection_failure_is_cheap",
     test_reused_connection_failure_is_cheap},
    {"missing_password_is_cheap", test_missing_password_is_cheap},
    {"tls_only_rejection_is_costly", test_tls_only_rejection_is_costly},
    {"tls_handshake_failure_is_costly", test_tls_handshake_failure_is_costly},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      cases[i].run();
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
