/** Poll-result parsing and permission fallback using controlled libpq I/O. */

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

#include "common_support.h"

static struct {
  PGresult *result;
  const char *sqlstate;
  const char *last_query;
  unsigned int queries_sent;
  bool busy;
  bool pause_after_result;
  bool drained;
  int send_ok;
} poll_script;

static int scripted_consume_input(PGconn *conn) {
  (void)conn;
  return 1;
}

static int scripted_is_busy(PGconn *conn) {
  (void)conn;
  return poll_script.busy;
}

static PGresult *scripted_get_result(PGconn *conn) {
  (void)conn;
  PGresult *res = poll_script.result;
  poll_script.result = nullptr;
  if (res) {
    poll_script.busy = poll_script.pause_after_result;
  } else {
    poll_script.drained = true;
  }
  return res;
}

static char *scripted_error_field(const PGresult *res, int field) {
  (void)res;
  support_assert_true(field == PG_DIAG_SQLSTATE, "inspect SQLSTATE");
  // libpq's return type is mutable; keep the synthetic field mutable too.
  static char sqlstate[6];
  if (!poll_script.sqlstate) {
    return nullptr;
  }
  memcpy(sqlstate, poll_script.sqlstate, sizeof(sqlstate));
  return sqlstate;
}

static int scripted_send_query(PGconn *conn, const char *query) {
  (void)conn;
  support_assert_true(poll_script.drained, "drain through NULL before retry");
  poll_script.last_query = query;
  poll_script.queries_sent++;
  poll_script.drained = false;
  return poll_script.send_ok;
}

static int scripted_flush(PGconn *conn) {
  (void)conn;
  return 0;
}

#define PQconsumeInput scripted_consume_input
#define PQisBusy scripted_is_busy
#define PQgetResult scripted_get_result
#define PQresultErrorField scripted_error_field
#define PQsendQuery scripted_send_query
#define PQflush scripted_flush

// Exercise the private parser and publication path without exposing a
// production API solely for tests. Other monitor objects come from pg_monitor.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/check_utils.c"

#undef PQconsumeInput
#undef PQisBusy
#undef PQgetResult
#undef PQresultErrorField
#undef PQsendQuery
#undef PQflush

static PGresult *make_poll_result(const char *const values[6]) {
  PGresult *res = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  support_assert_true(res != nullptr, "allocate poll result");
  char column_name[] = "value";
  PGresAttDesc attributes[6] = {0};
  for (int i = 0; i < 6; i++) {
    attributes[i].name = column_name;
    attributes[i].typid = 25;
    attributes[i].typlen = -1;
  }
  support_assert_true(PQsetResultAttrs(res, 6, attributes) == 1, "set columns");
  for (int i = 0; i < 6; i++) {
    char *value = values[i] ? copy_string(values[i]) : nullptr;
    const int length = value ? (int)strlen(value) : -1;
    const int set = PQsetvalue(res, 0, i, value, length);
    free(value);
    support_assert_true(set == 1, "set poll value");
  }
  return res;
}

static void publish_result(MonitorHost *host, const char *const values[6]) {
  PGresult *res = make_poll_result(values);
  host->iter_data_ready = parse_result(host, res);
  support_assert_true(host->iter_data_ready, "parse poll result");
  finish_iteration(host, true, 1000);
  PQclear(res);
}

static void configure_hosts(const char *master_position) {
  host_count = 2;
  monitor_host_list[0] = (MonitorHost){.host = "master"};
  monitor_host_list[1] = (MonitorHost){.host = "replica"};
  parameters.dc_locality_enabled = false;
  parameters.geo_locality_enabled = false;
  parameters.max_fails = 3;
  master_lsn = 0;
  save_master_index(-1);
  if (master_position) {
    const char *values[] = {"f", master_position, nullptr, nullptr,
                            "0", nullptr};
    publish_result(&monitor_host_list[0], values);
    save_master_index(0);
  }
}

static MonitorSnapshot replica_result(
  const char *received, const char *replayed, const char *latest_end
) {
  const char *values[] = {"t", nullptr, received, replayed, "9000", latest_end};
  publish_result(&monitor_host_list[1], values);
  return atomic_get_snapshot(&monitor_host_list[1]);
}

static void test_latest_end_lsn(void) {
  // Arrange
  configure_hosts("0/1000");

  // Act
  const MonitorSnapshot snap = replica_result("0/1000", "0/1000", "0/3000");

  // Assert
  support_assert_true(snap.status.alive && !snap.status.master, "live replica");
  support_assert_true(snap.lag_bytes == 8192, "include WAL not yet received");
  support_assert_true(snap.lag_ms == 9000, "preserve time lag");
  support_assert_true(snap.lsn == 0x1000, "LSN remains the replay position");

  const LagThresholds thresholds = {.max_lag_ms = 1000, .max_lag_bytes = 0};

  // Assert
  support_assert_true(
    find_replica(is_sync_replica_by_time_or_bytes, &thresholds, "test") ==
      &monitor_host_list[0],
    "unreceived WAL must prevent a false zero-byte sync match"
  );
  const LagThresholds minimum = {.min_lsn = 0x3000};

  // Assert
  support_assert_true(
    !is_alive_replica(snap, &monitor_host_list[1], &minimum),
    "advertised LSN must not satisfy a replay LSN constraint"
  );

  // Act
  finish_iteration(&monitor_host_list[1], false, 2000);

  // Assert
  const MonitorSnapshot failed = atomic_get_snapshot(&monitor_host_list[1]);
  support_assert_true(
    failed.status.possible_dead && failed.lag_bytes == 8192 &&
      failed.lsn == 0x1000,
    "failed poll preserves lag including the advertised WAL"
  );
}

static void test_latest_end_lsn_without_master(void) {
  // Arrange
  configure_hosts(nullptr);

  // Act
  const MonitorSnapshot snap = replica_result("0/1000", "0/1000", "0/3000");

  // Assert
  support_assert_true(
    snap.lag_bytes == 8192, "receiver works without master poll"
  );
  const LagThresholds thresholds = {.max_lag_bytes = 0};

  // Assert
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &thresholds, "test") == nullptr,
    "no byte-sync candidate while advertised WAL is missing"
  );
}

static void test_older_latest_end_lsn(void) {
  // Arrange
  configure_hosts("0/5000");

  // Act
  MonitorSnapshot snap = replica_result("0/3000", "0/1000", "0/2000");

  // Assert
  support_assert_true(
    snap.lag_bytes == 16384, "master position still contributes"
  );

  // Arrange
  configure_hosts("0/2000");

  // Act
  snap = replica_result("0/5000", "0/1000", "0/3000");

  // Assert
  support_assert_true(
    snap.lag_bytes == 16384, "received position still contributes"
  );
}

static void test_missing_latest_end_lsn(void) {
  // Arrange
  // An absent receiver or statistics hidden from this user yields SQL NULL.
  configure_hosts("0/5000");

  // Act
  MonitorSnapshot snap = replica_result("0/3000", "0/1000", nullptr);

  // Assert
  support_assert_true(
    snap.status.alive && snap.lag_bytes == 16384, "NULL receiver with master"
  );

  // Arrange
  configure_hosts(nullptr);

  // Act
  snap = replica_result("0/3000", "0/1000", nullptr);

  // Assert
  support_assert_true(
    snap.status.alive && snap.lag_bytes == 8192, "NULL receiver without master"
  );
}

static void test_replay_ahead_of_observations(void) {
  // Arrange
  configure_hosts("0/1000");

  // Act
  MonitorSnapshot snap = replica_result(nullptr, "0/2000", nullptr);

  // Assert
  support_assert_true(
    snap.lag_bytes == 0, "missing receive LSN must not underflow"
  );

  // Act
  snap = replica_result("0/1000", "0/3000", "0/2000");

  // Assert
  support_assert_true(
    snap.lag_bytes == 0, "older WAL observations must not underflow"
  );
}

static void test_master_ignores_latest_end_lsn(void) {
  // Arrange
  configure_hosts(nullptr);
  const char *values[] = {"f", "0/3000", "0/1000", "0/1000", "0", "0/5000"};

  // Act
  publish_result(&monitor_host_list[0], values);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(&monitor_host_list[0]);
  support_assert_true(snap.status.master && snap.status.alive, "master role");
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 0, "master has zero lag"
  );
  support_assert_true(
    snap.lsn == 0x3000 && master_lsn == 0x3000,
    "master keeps its own WAL position"
  );
}

static void queue_error(const char *sqlstate) {
  poll_script.sqlstate = sqlstate;
  poll_script.result = PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR);
  support_assert_true(poll_script.result != nullptr, "allocate SQL error");
}

static MonitorHost *configure_permission_poll(void) {
  configure_hosts("0/5000");
  (void)replica_result("0/1000", "0/1000", nullptr);
  MonitorHost *host = &monitor_host_list[1];
  reset_iter_state(host, 1000);
  host->iter_deadline_ms = 2000;
  poll_state_query_read(host);
  memset(&poll_script, 0, sizeof(poll_script));
  poll_script.send_ok = 1;
  queue_error("42501");
  return host;
}

static void assert_failed_poll(const MonitorHost *host) {
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE && host->failed_connections == 1,
    "failed poll finishes once"
  );
  support_assert_true(
    snap.status.alive && snap.status.possible_dead && snap.lsn == 0x1000 &&
      snap.lag_ms == 9000 && snap.lag_bytes == 16384,
    "real failure preserves previous measurements"
  );
  support_assert_true(
    !host->wal_receiver_disabled && !host->iter_retry_without_wal_receiver,
    "closing the connection resets fallback state"
  );
}

static void run_permission_fallback_replica(void) {
  // Arrange
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_WARNING);
  MonitorHost *host = configure_permission_poll();
  poll_script.pause_after_result = true;

  // Act
  read_step(host, 1010);

  // Assert
  support_assert_true(
    host->iter_retry_without_wal_receiver && poll_script.queries_sent == 0 &&
      host->poll_state == HOST_POLL_QUERY_READ,
    "wait for end-of-results before retrying"
  );
  support_assert_true(
    host->failed_connections == 0 &&
      !atomic_get_snapshot(host).status.possible_dead,
    "permission error alone does not change host health"
  );

  // Act
  poll_script.busy = false;
  poll_script.pause_after_result = false;
  read_step(host, 1020);

  // Assert
  support_assert_true(
    host->poll_state == HOST_POLL_QUERY_SEND &&
      host->iter_deadline_ms == 2000 && poll_script.queries_sent == 1 &&
      poll_script.last_query ==
        streaming_replication_query_without_wal_receiver,
    "retry without receiver keeps the original deadline"
  );

  // Arrange
  const char *values[] = {"t", nullptr, "0/3000", "0/2000", "1234", nullptr};
  poll_script.result = make_poll_result(values);

  // Act
  advance_host_poll(host, 1030);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE && host->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead && !snap.status.master &&
      snap.lag_bytes == 12288 && snap.lag_ms == 1234 && snap.lsn == 0x2000,
    "successful fallback publishes fresh replica measurements"
  );

  // Act & Assert
  reset_iter_state(host, 1100);
  support_assert_true(send_query(host), "send next poll");
  support_assert_true(
    poll_script.last_query == streaming_replication_query_without_wal_receiver,
    "next poll on this connection skips the receiver"
  );
  poll_script.drained = true;
  support_assert_true(
    send_query(&monitor_host_list[0]), "send another host poll"
  );
  support_assert_true(
    poll_script.last_query == streaming_replication_query,
    "permission fallback is isolated to the affected host"
  );
  close_conn(host);
  poll_script.drained = true;
  support_assert_true(send_query(host), "send after reconnect");
  support_assert_true(
    poll_script.last_query == streaming_replication_query,
    "a new connection retries receiver access"
  );

  // Cleanup
  pg_status_log_shutdown();
}

static void test_permission_fallback_replica(void) {
  // Act
  char *logs = support_capture_standard_error(run_permission_fallback_replica);

  // Assert
  const char *warning =
    "WARNING monitor: PostgreSQL permission denied host=replica";
  support_assert_contains(logs, warning, "permission fallback emits a warning");
  support_assert_not_contains(
    strstr(logs, warning) + 1, warning,
    "do not repeat warnings for cached fallback"
  );
  support_assert_contains(
    logs, "SELECT on pg_catalog.pg_stat_wal_receiver", "view privilege hint"
  );
  support_assert_contains(
    logs, "EXECUTE on pg_catalog.pg_stat_get_wal_receiver()",
    "function privilege hint"
  );
  support_assert_contains(
    logs, "pg_read_all_stats", "statistics visibility hint"
  );

  // Cleanup
  free(logs);
}

static void test_permission_fallback_master(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();

  // Act
  read_step(host, 1010);
  const char *values[] = {"f", "0/9000", nullptr, nullptr, "0", nullptr};
  poll_script.result = make_poll_result(values);
  advance_host_poll(host, 1020);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->failed_connections == 0 && snap.status.alive && snap.status.master &&
      !snap.status.possible_dead && snap.lag_bytes == 0 && snap.lag_ms == 0 &&
      snap.lsn == 0x9000 && master_lsn == 0x9000,
    "successful fallback publishes master role and WAL position"
  );
}

static void test_permission_fallback_failure(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();

  // Act
  read_step(host, 1010);
  queue_error("42501");
  advance_host_poll(host, 1020);

  // Assert
  support_assert_true(poll_script.queries_sent == 1, "retry at most once");
  assert_failed_poll(host);
}

static void test_permission_fallback_unrelated_error(void) {
  // Arrange
  const char *sqlstates[] = {"XX000", nullptr};
  for (size_t i = 0; i < sizeof(sqlstates) / sizeof(sqlstates[0]); i++) {
    // Arrange
    MonitorHost *host = configure_permission_poll();
    poll_script.sqlstate = sqlstates[i];

    // Act
    read_step(host, 1010);

    // Assert
    support_assert_true(
      poll_script.queries_sent == 0, "do not retry other errors"
    );
    assert_failed_poll(host);
  }
}

static void test_permission_fallback_timeout(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();

  // Act
  read_step(host, 1010);
  timeout_host_poll(host, 2000);

  // Assert
  assert_failed_poll(host);
}

static void test_permission_fallback_send_failure(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();
  poll_script.send_ok = 0;

  // Act
  read_step(host, 1010);

  // Assert
  assert_failed_poll(host);
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  const struct {
    const char *name;
    support_action_t run;
  } cases[] = {
    {"latest_end_lsn", test_latest_end_lsn},
    {"latest_end_lsn_without_master", test_latest_end_lsn_without_master},
    {"older_latest_end_lsn", test_older_latest_end_lsn},
    {"missing_latest_end_lsn", test_missing_latest_end_lsn},
    {"replay_ahead_of_observations", test_replay_ahead_of_observations},
    {"master_ignores_latest_end_lsn", test_master_ignores_latest_end_lsn},
    {"permission_fallback_replica", test_permission_fallback_replica},
    {"permission_fallback_master", test_permission_fallback_master},
    {"permission_fallback_failure", test_permission_fallback_failure},
    {"permission_fallback_unrelated_error",
     test_permission_fallback_unrelated_error},
    {"permission_fallback_timeout", test_permission_fallback_timeout},
    {"permission_fallback_send_failure", test_permission_fallback_send_failure},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      cases[i].run();
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
