/** Poll-result parsing and permission fallback using controlled libpq I/O. */

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_support.h"

// A placeholder connection marks a host whose connection works; only the
// libpq calls scripted below are made on it.
struct pg_conn {
  char unused;
};

static PGconn working_connection;

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

static ConnStatusType scripted_status(const PGconn *conn) {
  support_assert_true(conn == &working_connection, "status of a placeholder");
  return CONNECTION_OK;
}

static void scripted_finish(PGconn *conn) {
  support_assert_true(conn == &working_connection, "close a placeholder");
}

#define PQconsumeInput scripted_consume_input
#define PQisBusy scripted_is_busy
#define PQgetResult scripted_get_result
#define PQresultErrorField scripted_error_field
#define PQsendQuery scripted_send_query
#define PQflush scripted_flush
#define PQstatus scripted_status
#define PQfinish scripted_finish

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
#undef PQstatus
#undef PQfinish

/** Columns of the polling query, in order. */
enum { POLL_COLUMNS = 10 };

static PGresult *make_poll_result(const char *const values[POLL_COLUMNS]) {
  PGresult *res = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  support_assert_true(res != nullptr, "allocate poll result");
  char column_name[] = "value";
  PGresAttDesc attributes[POLL_COLUMNS] = {0};
  for (int i = 0; i < POLL_COLUMNS; i++) {
    attributes[i].name = column_name;
    attributes[i].typid = 25;
    attributes[i].typlen = -1;
  }
  support_assert_true(
    PQsetResultAttrs(res, POLL_COLUMNS, attributes) == 1, "set columns"
  );
  for (int i = 0; i < POLL_COLUMNS; i++) {
    char *value = values[i] ? copy_string(values[i]) : nullptr;
    const int length = value ? (int)strlen(value) : -1;
    const int set = PQsetvalue(res, 0, i, value, length);
    free(value);
    support_assert_true(set == 1, "set poll value");
  }
  return res;
}

static void publish_result_at(
  MonitorHost *host, const char *const values[POLL_COLUMNS],
  const uint64_t started_at_ms, const uint64_t now_ms
) {
  PGresult *res = make_poll_result(values);
  host->iter_started_at_ms = started_at_ms;
  host->iter_data_ready = parse_result(host, res, now_ms);
  support_assert_true(host->iter_data_ready, "parse poll result");
  finish_iteration(host, true, now_ms);
  PQclear(res);
}

static void publish_result(
  MonitorHost *host, const char *const values[POLL_COLUMNS]
) {
  publish_result_at(host, values, host->iter_started_at_ms, 1000);
}

static void configure_hosts(const char *master_position) {
  host_count = 2;
  monitor_host_list[0] = (MonitorHost){.host = "master"};
  monitor_host_list[1] = (MonitorHost){.host = "replica"};
  parameters.dc_locality_enabled = false;
  parameters.geo_locality_enabled = false;
  parameters.max_fails = 3;
  reference_master_host = nullptr;
  save_reference_master_timeline(0);
  save_master_index(-1);
  if (master_position) {
    const char *values[POLL_COLUMNS] = {"f", master_position, nullptr, nullptr,
                                        "0", nullptr,         "f"};
    publish_result(&monitor_host_list[0], values);
    save_master_index(0);
  }
}

static MonitorSnapshot replica_result(
  const char *received, const char *replayed, const char *latest_end
) {
  const char *values[POLL_COLUMNS] = {"t",    nullptr,    received, replayed,
                                      "9000", latest_end, nullptr};
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

  // Arrange: no master has been seen, and the WAL receiver runs.
  configure_hosts(nullptr);
  const char *streaming[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/1000", "9000", nullptr, nullptr, nullptr, "t",
  };

  // Act
  publish_result(&monitor_host_list[1], streaming);
  snap = atomic_get_snapshot(&monitor_host_list[1]);

  // Assert
  support_assert_true(
    snap.status.alive && snap.lag_bytes == 8192, "NULL receiver without master"
  );

  // Act: the receiver has stopped.
  snap = replica_result("0/3000", "0/1000", nullptr);

  // Assert
  support_assert_true(
    snap.status.alive && snap.lag_bytes == UINT64_MAX,
    "no upstream position to compare with"
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
  const char *values[POLL_COLUMNS] = {"f", "0/3000", "0/1000", "0/1000",
                                      "0", "0/5000", "f"};

  // Act
  publish_result(&monitor_host_list[0], values);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(&monitor_host_list[0]);
  support_assert_true(snap.status.master && snap.status.alive, "master role");
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 0, "master has zero lag"
  );
  support_assert_true(
    snap.lsn == 0x3000 && monitor_host_list[0].master_wal_lsn == 0x3000,
    "master keeps its own WAL position"
  );
}

/** Checks host 0 as the selected master; the check takes 5 ms. */
static void master_check(const char *position, const uint64_t started_at_ms) {
  const char *values[POLL_COLUMNS] = {"f", position, nullptr, nullptr,
                                      "0", nullptr,  "f"};
  publish_result_at(
    &monitor_host_list[0], values, started_at_ms, started_at_ms + 5
  );
  save_master_index(0);
}

/**
 * Checks host 1 as a replica that has replayed all WAL it knows about;
 * the check takes 5 ms.
 */
static MonitorSnapshot replica_check(
  const char *replayed, const char *commit_age_ms, const uint64_t started_at_ms
) {
  const char *values[POLL_COLUMNS] = {
    "t", nullptr, replayed, replayed, commit_age_ms, replayed, nullptr
  };
  publish_result_at(
    &monitor_host_list[1], values, started_at_ms, started_at_ms + 5
  );
  return atomic_get_snapshot(&monitor_host_list[1]);
}

static void test_idle_master_bounds_time_lag(void) {
  // Arrange: the last commit was a minute ago, and since then the master has
  // written no WAL that the replica lacks.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);

  // Act
  MonitorSnapshot snap = replica_check("0/3000", "60000", 10300);

  // Assert
  support_assert_true(snap.lag_bytes == 0, "replica has replayed all WAL");
  support_assert_true(
    snap.lag_ms == 305, "time lag is bounded by the start of the master check"
  );
  const LagThresholds thresholds = {.max_lag_ms = 1000, .max_lag_bytes = 0};
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[1],
    "caught-up replica is time-synchronous"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &thresholds, "test") ==
      &monitor_host_list[1],
    "caught-up replica is synchronous by time and bytes"
  );

  // Act: the master has not been checked since.
  snap = replica_check("0/3000", "63000", 13000);

  // Assert
  support_assert_true(
    snap.lag_ms == 3005, "the bound ages with the master check"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[0],
    "an old master check does not make the replica time-synchronous"
  );
}

static void test_commit_time_lag_below_bound(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 10000);

  // Act
  const MonitorSnapshot snap = replica_check("0/3000", "20", 10300);

  // Assert
  support_assert_true(snap.lag_ms == 20, "the bound never raises the lag");
}

static void test_lagging_replica_keeps_commit_time_lag(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/5000", 10000);

  // Act
  const MonitorSnapshot snap = replica_check("0/3000", "60000", 10300);

  // Assert
  support_assert_true(snap.lag_bytes == 8192, "replica lacks master WAL");
  support_assert_true(snap.lag_ms == 60000, "no bound while WAL is missing");
  const LagThresholds thresholds = {.max_lag_ms = 1000};
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[0],
    "lagging replica falls back to the master"
  );
}

static void test_unusable_master_check_keeps_commit_time_lag(void) {
  // Arrange: the last check of the master failed.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);
  finish_iteration(&monitor_host_list[0], false, 11005);

  // Act
  MonitorSnapshot snap = replica_check("0/3000", "60000", 11300);

  // Assert
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 60000,
    "no bound after a failed master check"
  );

  // Arrange: no master is selected.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);
  save_master_index(-1);

  // Act
  snap = replica_check("0/3000", "60000", 10300);

  // Assert
  support_assert_true(snap.lag_ms == 60000, "no bound without a master");

  // Arrange: a master that is not selected was checked later.
  configure_hosts(nullptr);
  host_count = 3;
  monitor_host_list[2] = (MonitorHost){.host = "other-master"};
  master_check("0/3000", 10000);
  const char *other[POLL_COLUMNS] = {"f", "0/9000", nullptr, nullptr,
                                     "0", nullptr,  "f"};
  publish_result_at(&monitor_host_list[2], other, 10100, 10105);

  // Act
  snap = replica_check("0/3000", "60000", 10300);

  // Assert
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 305,
    "only the selected master's check counts"
  );

  // Arrange: the selected master is now a replica itself.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);
  const char *demoted[POLL_COLUMNS] = {"t",     nullptr,  "0/3000", "0/3000",
                                       "60000", "0/3000", nullptr};

  // Act
  publish_result_at(&monitor_host_list[0], demoted, 11000, 11005);

  // Assert
  snap = atomic_get_snapshot(&monitor_host_list[0]);
  support_assert_true(
    !snap.status.master && snap.lag_ms == 60000,
    "a demoted master is not bounded by its own old position"
  );
}

static void run_idle_master_checks(void) {
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);
  configure_hosts(nullptr);
  const char *commit_ages_ms[] = {"1300", "2300", "3300", "4300", "5300"};
  for (size_t i = 0; i < sizeof(commit_ages_ms) / sizeof(commit_ages_ms[0]);
       i++) {
    const uint64_t cycle_ms = 10000 + ((uint64_t)i * 1000);
    master_check("0/3000", cycle_ms);
    const MonitorSnapshot snap = replica_check(
      "0/3000", commit_ages_ms[i], cycle_ms + 900
    );
    support_assert_true(snap.lag_ms == 905, "bounded on every check");
  }
  pg_status_log_shutdown();
}

static void test_idle_master_keeps_time_sync(void) {
  // Act
  char *logs = support_capture_standard_error(run_idle_master_checks);

  // Assert
  support_assert_contains(
    logs, "host state changed host=replica state=alive role=replica",
    "replica checks are logged"
  );
  support_assert_not_contains(
    logs, "dimension=time",
    "caught-up replica of an idle master stays time-synchronous"
  );

  // Cleanup
  free(logs);
}

static void test_unwritten_master_wal_keeps_commit_time_lag(void) {
  // Arrange: an asynchronous commit is visible on the master, but its WAL is
  // not written yet, so it is not covered by the master's WAL position.
  configure_hosts(nullptr);
  const char *unwritten[POLL_COLUMNS] = {"f", "0/3000", nullptr, nullptr,
                                         "0", nullptr,  "t"};
  publish_result_at(&monitor_host_list[0], unwritten, 10000, 10005);
  save_master_index(0);

  // Act
  MonitorSnapshot snap = replica_check("0/3000", "60000", 10300);

  // Assert
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 60000,
    "no bound while the master has unwritten WAL"
  );

  // Act: the WAL is written by the next master check.
  master_check("0/3000", 11000);
  snap = replica_check("0/3000", "61000", 11300);

  // Assert
  support_assert_true(snap.lag_ms == 305, "the bound applies again");
}

/** Starts a check of the master that is still in progress. */
static void start_master_check(const uint64_t started_at_ms) {
  MonitorHost *master = &monitor_host_list[0];
  reset_iter_state(master, started_at_ms);
  poll_state_query_read(master);
}

/** Starts a check of the replica that is still in progress. */
static MonitorHost *start_replica_check(const uint64_t started_at_ms) {
  MonitorHost *replica = &monitor_host_list[1];
  reset_iter_state(replica, started_at_ms);
  poll_state_query_read(replica);
  return replica;
}

/** Delivers the result of the check of `host` in progress at `now_ms`. */
static void deliver_result(
  MonitorHost *host, const char *const values[POLL_COLUMNS],
  const uint64_t now_ms
) {
  memset(&poll_script, 0, sizeof(poll_script));
  poll_script.send_ok = 1;
  poll_script.result = make_poll_result(values);
  read_step(host, now_ms);
}

static const char *const CAUGHT_UP_REPLICA[POLL_COLUMNS] = {
  "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr
};

static void test_replica_waits_for_master_check(void) {
  // Arrange: the replica's result arrives before the result of the master's
  // check that started at the same time.
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);

  // Act
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT &&
      !atomic_get_snapshot(replica).status.alive,
    "the replica waits for the master's check before publishing"
  );

  // Act
  const char *master[POLL_COLUMNS] = {"f", "0/3000", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  deliver_result(&monitor_host_list[0], master, 10004);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && replica->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead,
    "the master's result ends the replica's check"
  );
  support_assert_true(
    snap.lag_bytes == 0 && snap.lag_ms == 2,
    "bounded by the master check in progress, not by the previous one"
  );
  const LagThresholds thresholds = {.max_lag_ms = 50};
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") == replica,
    "a caught-up replica meets a threshold below the polling period"
  );
}

static void test_master_check_with_new_wal_ends_wait(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Act: the master has written WAL that the replica lacks.
  const char *master[POLL_COLUMNS] = {"f", "0/5000", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  deliver_result(&monitor_host_list[0], master, 10004);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && snap.lag_bytes == 8192,
    "the byte lag is measured against the new master position"
  );
  support_assert_true(
    snap.lag_ms == 1002, "the master's previous check still bounds the time lag"
  );
}

static void test_failed_master_check_ends_wait(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Act
  timeout_host_poll(&monitor_host_list[0], 11000);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && replica->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead,
    "the replica's check succeeds"
  );
  support_assert_true(
    snap.lag_ms == 1002,
    "the master's previous check still bounds the time lag after a failure"
  );
}

static void test_master_wait_ends_at_deadline(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Act
  finish_master_waits(10999);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT, "wait until the deadline"
  );

  // Act
  finish_master_waits(11000);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && replica->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead,
    "the deadline ends the wait without a failure"
  );
  support_assert_true(
    snap.lag_ms == 1002, "bounded by the master's previous check"
  );
}

static void test_replica_waits_for_due_master_check(void) {
  // Arrange: the master's next check is due at 10000, but it needs a new
  // connection, which waits for a connection turn.
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  MonitorHost *master = &monitor_host_list[0];
  MonitorHost *replica = start_replica_check(9500);

  // Act
  deliver_result(replica, CAUGHT_UP_REPLICA, 10000);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 1000,
    "no wait for a check that needs a new connection"
  );

  // Arrange: the master's connection works.
  master->conn = &working_connection;
  replica = start_replica_check(9500);

  // Act: the replica's result arrives just before the check is due.
  deliver_result(replica, CAUGHT_UP_REPLICA, 9999);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 999,
    "no wait before the master's check is due"
  );

  // Act: the result arrives once the check is due but has not started.
  replica = start_replica_check(10500);
  master->next_poll_at_ms = 11000;
  deliver_result(replica, CAUGHT_UP_REPLICA, 11000);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT, "wait for the due check"
  );

  // Act: the check starts late, as the loop wakes up after it is due.
  start_master_check(11001);
  finish_master_waits(11001);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT, "keep waiting while it runs"
  );

  // Act
  const char *values[POLL_COLUMNS] = {"f", "0/3000", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  deliver_result(master, values, 11003);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 0,
    "a master check that started later shows the replica was up to date"
  );
}

static void test_master_check_that_does_not_start_ends_wait(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  MonitorHost *master = &monitor_host_list[0];
  master->conn = &working_connection;
  MonitorHost *replica = start_replica_check(9500);
  deliver_result(replica, CAUGHT_UP_REPLICA, 10000);

  // Act: the due check fails as it starts.
  reset_iter_state(master, 10001);
  finish_iteration(master, false, 10001);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 1000,
    "bounded by the master's previous check"
  );
}

static void test_replica_waits_only_when_master_check_helps(void) {
  // Arrange: the replica has WAL left to replay.
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  const char *behind[POLL_COLUMNS] = {"t",     nullptr, "0/4000", "0/3000",
                                      "60000", nullptr, nullptr};

  // Act
  deliver_result(replica, behind, 10002);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 60000,
    "a replica with WAL to replay does not wait"
  );

  // Arrange: the master's check has ended, and its next one is not due.
  master_check("0/3000", 10000);
  monitor_host_list[0].conn = &working_connection;
  replica = start_replica_check(10300);

  // Act
  deliver_result(replica, CAUGHT_UP_REPLICA, 10302);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 302,
    "a replica does not wait while the master is idle"
  );
}

/** A replica left on the WAL history that a failover abandoned. */
static const char *const DIVERGED_REPLICA[POLL_COLUMNS] = {
  "t", nullptr, "0/9000", "0/9000", "60000", nullptr, nullptr
};

static void test_replica_ahead_of_master_keeps_commit_time_lag(void) {
  // Arrange: the master was promoted behind the replica, which has kept
  // the WAL of the old master that the new one never had.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);

  // Act
  const MonitorSnapshot snap = replica_check("0/9000", "60000", 10300);

  // Assert
  support_assert_true(
    snap.lag_ms == 60000, "no bound for a position past the master's"
  );
  const LagThresholds thresholds = {.max_lag_ms = 1000};
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[0],
    "a replica ahead of the master is not time-synchronous"
  );
}

static void test_replica_ahead_waits_for_next_master_check(void) {
  // Arrange: after its check, the master wrote WAL without commits, such as
  // a running-transactions record, and the replica has replayed it.
  configure_hosts(nullptr);
  master_check("0/3000", 10000);
  MonitorHost *master = &monitor_host_list[0];
  master->conn = &working_connection;
  MonitorHost *replica = start_replica_check(10500);
  const char *ahead[POLL_COLUMNS] = {"t",     nullptr,  "0/3100", "0/3100",
                                     "60000", "0/3100", nullptr};

  // Act
  deliver_result(replica, ahead, 10600);
  finish_master_waits(10800);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT,
    "wait for the master's next check"
  );

  // Act
  start_master_check(11000);
  const char *values[POLL_COLUMNS] = {"f", "0/3100", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  deliver_result(master, values, 11004);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && snap.lag_bytes == 0 &&
      snap.lag_ms == 0,
    "the next check shows that the replica was up to date"
  );
}

static void test_later_master_check_keeps_byte_lag(void) {
  // Arrange: a long transaction writes WAL without commits. The master's
  // last check found WAL not written yet, and the replica has since
  // replayed all WAL it has received.
  configure_hosts(nullptr);
  const char *unwritten[POLL_COLUMNS] = {"f", "0/3000", nullptr, nullptr,
                                         "0", nullptr,  "t"};
  publish_result_at(&monitor_host_list[0], unwritten, 10000, 10005);
  save_master_index(0);
  MonitorHost *master = &monitor_host_list[0];
  master->conn = &working_connection;
  MonitorHost *replica = start_replica_check(10500);
  const char *caught_up[POLL_COLUMNS] = {"t",     nullptr,  "0/3100", "0/3100",
                                         "60000", "0/3100", nullptr};
  deliver_result(replica, caught_up, 10600);

  // Act: by the master's next check, the transaction has written 2 MiB.
  start_master_check(11000);
  const char *values[POLL_COLUMNS] = {"f", "0/203100", nullptr, nullptr,
                                      "0", nullptr,    "t"};
  deliver_result(master, values, 11004);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && snap.lag_bytes == 0,
    "WAL written after the replica's result is not its byte lag"
  );
  support_assert_true(snap.lag_ms == 60000, "no bound while WAL is written");
  const LagThresholds thresholds = {.max_lag_bytes = 1000000};
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &thresholds, "test") == replica,
    "the replica stays byte-synchronous"
  );
}

static void test_diverged_replica_stops_waiting(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 10000);
  MonitorHost *master = &monitor_host_list[0];
  master->conn = &working_connection;
  MonitorHost *replica = start_replica_check(10500);
  deliver_result(replica, DIVERGED_REPLICA, 10600);

  // Act: the master's next check is still behind the replica, which no
  // replica of this master can be.
  start_master_check(11000);
  const char *values[POLL_COLUMNS] = {"f", "0/3100", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  deliver_result(master, values, 11004);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE && snap.lag_ms == 60000,
    "a replica ahead of a later master position keeps its commit age"
  );
  const LagThresholds thresholds = {.max_lag_ms = 1000};
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") == master,
    "the diverged replica is not time-synchronous"
  );
}

static void test_replica_does_not_wait_for_unhealthy_master(void) {
  // Arrange: the master's last check failed, and it is reconnecting.
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  finish_iteration(&monitor_host_list[0], false, 9500);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);

  // Act
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 60000,
    "a master whose last check failed is not waited for"
  );
}

static void test_master_wait_ends_before_next_check(void) {
  // Arrange: the deadline is past the replica's next check, and the
  // master's check hangs.
  parameters.query_timeout_ms = 5000;
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  deliver_result(replica, CAUGHT_UP_REPLICA, 10002);

  // Act
  finish_master_waits(10999);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_MASTER_WAIT, "wait while it may help"
  );

  // Act
  finish_master_waits(11000);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(replica);
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      replica->next_poll_at_ms == 11000 && snap.status.alive,
    "the wait ends when the replica's next check is due"
  );
  support_assert_true(
    snap.lag_ms == 1002, "bounded by the master's previous check"
  );

  // Cleanup
  parameters.query_timeout_ms = 1000;
}

static void test_replica_with_recent_commit_does_not_wait(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check("0/3000", 9000);
  start_master_check(10000);
  MonitorHost *replica = start_replica_check(10000);
  const char *recent[POLL_COLUMNS] = {"t", nullptr,  "0/3000", "0/3000",
                                      "1", "0/3000", nullptr};

  // Act
  deliver_result(replica, recent, 10050);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE &&
      atomic_get_snapshot(replica).lag_ms == 1,
    "no wait when the master's check cannot beat the commit age"
  );
}

static void test_selected_master_is_byte_lag_reference(void) {
  // Arrange: host 0 is the selected master; host 1 is another master,
  // checked later.
  configure_hosts(nullptr);
  host_count = 3;
  monitor_host_list[1] = (MonitorHost){.host = "other-master"};
  monitor_host_list[2] = (MonitorHost){.host = "replica"};
  MonitorHost *replica = &monitor_host_list[2];
  const char *ahead[POLL_COLUMNS] = {"f", "0/6340748", nullptr, nullptr,
                                     "0", nullptr,     "f"};
  const char *caught_up[POLL_COLUMNS] = {
    "t", nullptr, "0/6000060", "0/6000060", "100", "0/6000060", nullptr,
  };
  master_check("0/6000060", 10000);
  publish_result_at(&monitor_host_list[1], ahead, 10100, 10105);

  // Act
  publish_result_at(replica, caught_up, 10300, 10305);

  // Assert
  const LagThresholds thresholds = {.max_lag_bytes = 1048576};
  support_assert_true(
    atomic_get_snapshot(replica).lag_bytes == 0,
    "a master that is not selected is no reference"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &thresholds, "test") == replica &&
      find_most_sync_replica_by_bytes(&thresholds, "test") == replica,
    "the caught-up replica is byte-synchronous"
  );

  // Arrange: the selected master is ahead, the other one behind, and they
  // are checked in the other order.
  configure_hosts(nullptr);
  host_count = 3;
  monitor_host_list[1] = (MonitorHost){.host = "other-master"};
  monitor_host_list[2] = (MonitorHost){.host = "replica"};
  const char *behind[POLL_COLUMNS] = {"f", "0/1000000", nullptr, nullptr,
                                      "0", nullptr,     "f"};
  const char *lagging[POLL_COLUMNS] = {
    "t",     nullptr, "0/6000000", "0/6000000", "100",
    nullptr, nullptr, nullptr,     "t",
  };
  publish_result_at(&monitor_host_list[1], behind, 9900, 9905);
  master_check("0/9000000", 10000);
  publish_result_at(&monitor_host_list[1], behind, 10100, 10105);

  // Act
  publish_result_at(replica, lagging, 10300, 10305);

  // Assert
  support_assert_true(
    atomic_get_snapshot(replica).lag_bytes == 0x3000000,
    "the byte lag counts from the selected master"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &thresholds, "test") ==
      &monitor_host_list[0],
    "the lagging replica falls back to the master"
  );
}

static void test_byte_lag_unknown_without_reference(void) {
  // Arrange: no master has been seen, and the WAL receiver does not run.
  configure_hosts(nullptr);
  MonitorHost *replica = &monitor_host_list[1];

  // Act
  MonitorSnapshot snap = replica_result(nullptr, "0/4303740", nullptr);

  // Assert
  const LagThresholds any = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX
  };
  const LagThresholds largest = {.max_lag_bytes = UINT64_MAX - 1};
  support_assert_true(snap.lag_bytes == UINT64_MAX, "the byte lag is unknown");
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &largest, "test") == nullptr,
    "an unknown byte lag meets no threshold"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &any, "test") == replica,
    "it is still returned without a limit"
  );
  support_assert_true(
    find_most_sync_replica_by_bytes(&any, "test") == replica,
    "without a limit, the most synchronous replica may have an unknown lag"
  );

  // Act: the replica's next check fails.
  finish_iteration(replica, false, 2000);

  // Assert
  support_assert_true(
    atomic_get_snapshot(replica).status.possible_dead &&
      find_most_sync_replica_by_bytes(&any, "test") == replica,
    "so may a possibly dead one"
  );

  // Act: as after the receiver stopped, it has replayed all it received.
  snap = replica_result("0/4303740", "0/4303740", nullptr);

  // Assert
  support_assert_true(
    snap.lag_bytes == UINT64_MAX, "the received position is no reference"
  );

  // Arrange: another replica streams.
  host_count = 3;
  monitor_host_list[2] = (MonitorHost){.host = "streaming"};
  const char *streaming[POLL_COLUMNS] = {
    "t",         nullptr, "0/4400000", "0/4300000", "100",
    "0/4400000", nullptr, nullptr,     "t",
  };
  publish_result(&monitor_host_list[2], streaming);

  // Assert
  support_assert_true(
    atomic_get_snapshot(&monitor_host_list[2]).lag_bytes == 0x100000 &&
      find_most_sync_replica_by_bytes(&largest, "test") ==
        &monitor_host_list[2],
    "a replica with a known byte lag ranks first"
  );

  // Act: the master is seen.
  master_check("0/5000000", 20000);
  snap = replica_result(nullptr, "0/4303740", nullptr);

  // Assert
  support_assert_true(
    snap.lag_bytes == 0x5000000 - 0x4303740, "the master is the reference"
  );
}

static void test_unknown_commit_age(void) {
  // Arrange: the replica has replayed no commit since it started, and it
  // lags behind the master.
  configure_hosts(nullptr);
  MonitorHost *replica = &monitor_host_list[1];
  master_check("0/5000", 10000);
  const char *lagging[POLL_COLUMNS] = {"t",     nullptr,  "0/3000", "0/3000",
                                       nullptr, "0/3000", nullptr};

  // Act
  publish_result_at(replica, lagging, 10300, 10305);

  // Assert
  MonitorSnapshot snap = atomic_get_snapshot(replica);
  const LagThresholds thresholds = {.max_lag_ms = 1000};
  const LagThresholds any = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX
  };
  support_assert_true(
    snap.lag_ms == UINT64_MAX && snap.lag_bytes == 0x2000,
    "the time lag is unknown, not zero"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[0],
    "the lagging replica is not time-synchronous"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &any, "test") == replica,
    "it is still returned without a limit"
  );

  // Act: the replica has caught up with the master.
  const char *caught_up[POLL_COLUMNS] = {"t",     nullptr,  "0/5000", "0/5000",
                                         nullptr, "0/5000", nullptr};
  publish_result_at(replica, caught_up, 10400, 10405);

  // Assert
  snap = atomic_get_snapshot(replica);
  support_assert_true(
    snap.lag_ms == 405 &&
      find_replica(is_sync_replica_by_time, &thresholds, "test") == replica,
    "the master's check bounds the time lag"
  );

  // Act: no master is selected.
  save_master_index(-1);
  publish_result_at(replica, caught_up, 10500, 10505);

  // Assert
  support_assert_true(
    atomic_get_snapshot(replica).lag_ms == UINT64_MAX, "nothing bounds it"
  );
}

/** Checks host 0 as the selected master on timeline 2. */
static void master_check_on_timeline_2(void) {
  const char *values[POLL_COLUMNS] = {"f",     "0/3000", nullptr, nullptr,
                                      nullptr, nullptr,  "f",     "2"};
  publish_result_at(&monitor_host_list[0], values, 10000, 10005);
  save_master_index(0);
}

static void test_replica_on_other_timeline(void) {
  // Arrange: a failover left the replica on timeline 1, ahead of the new
  // master on timeline 2.
  configure_hosts(nullptr);
  master_check_on_timeline_2();
  MonitorHost *replica = &monitor_host_list[1];
  MonitorHost *master = &monitor_host_list[0];
  const char *abandoned[POLL_COLUMNS] = {
    "t", nullptr, "0/9000", "0/9000", "60000", nullptr, nullptr, "1", "f",
  };

  // Act
  publish_result_at(replica, abandoned, 10300, 10305);

  // Assert
  MonitorSnapshot snap = atomic_get_snapshot(replica);
  const LagThresholds written = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX, .min_lsn = 0x3000
  };
  const LagThresholds bytes = {.max_lag_ms = 1000, .max_lag_bytes = 1048576};
  const LagThresholds any = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX
  };
  support_assert_true(
    snap.status.alive && snap.status.timeline_mismatch &&
      snap.lag_bytes == UINT64_MAX && snap.lag_ms == 60000,
    "the byte lag is unknown on another timeline"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
        master &&
      find_replica(is_sync_replica_by_bytes, &bytes, "test") == master &&
      find_replica(is_sync_replica_by_time_or_bytes, &bytes, "test") ==
        master &&
      find_most_sync_replica_by_bytes(&bytes, "test") == master,
    "the replica meets neither min_lsn nor a byte threshold"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &any, "test") == replica,
    "it is still returned without a limit"
  );

  // Act: the same position as the master's, by coincidence.
  const char *coincident[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr, "1", "t",
  };
  publish_result_at(replica, coincident, 10400, 10405);

  // Assert
  snap = atomic_get_snapshot(replica);
  support_assert_true(
    snap.status.timeline_mismatch && snap.lag_ms == 60000,
    "an equal position on another timeline does not bound the time lag"
  );

  // Act: the replica follows the master's timeline.
  const char *following[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr, "2", "t",
  };
  publish_result_at(replica, following, 10500, 10505);

  // Assert
  snap = atomic_get_snapshot(replica);
  support_assert_true(
    !snap.status.timeline_mismatch && snap.lag_bytes == 0 &&
      snap.lag_ms == 505 &&
      find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
        replica,
    "on the master's timeline, the replica meets min_lsn"
  );

  // Act: a newer timeline than the selected master's.
  const char *newer[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr, "3", "t",
  };
  publish_result_at(replica, newer, 10600, 10605);

  // Assert
  support_assert_true(
    atomic_get_snapshot(replica).status.timeline_mismatch,
    "a replica of another master is on another history too"
  );

  // Act: the timeline is unknown, as without the control file functions.
  const char *unknown[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr,
  };
  publish_result_at(replica, unknown, 10700, 10705);

  // Assert
  snap = atomic_get_snapshot(replica);
  support_assert_true(
    !snap.status.timeline_mismatch && snap.lag_bytes == 0,
    "an unknown timeline is not taken for another one"
  );
}

static void test_replica_on_other_timeline_does_not_wait(void) {
  // Arrange
  configure_hosts(nullptr);
  master_check_on_timeline_2();
  start_master_check(11000);
  MonitorHost *replica = start_replica_check(11000);
  const char *coincident[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr, "1", "t",
  };

  // Act
  deliver_result(replica, coincident, 11002);

  // Assert
  support_assert_true(
    replica->poll_state == HOST_POLL_IDLE,
    "no master check can bound a replica on another timeline"
  );
}

/** Checks `host` as a master on `timeline`; the check takes 5 ms. */
static void master_check_on_timeline(
  MonitorHost *host, const char *position, const char *timeline,
  const uint64_t started_at_ms
) {
  const char *values[POLL_COLUMNS] = {"f",     position, nullptr, nullptr,
                                      nullptr, nullptr,  "f",     timeline};
  publish_result_at(host, values, started_at_ms, started_at_ms + 5);
}

/** A replica of the master on timeline 1 that has replayed all its WAL. */
static const char *const CAUGHT_UP_ON_TIMELINE_1[POLL_COLUMNS] = {
  "t",         nullptr, "0/3000000", "0/3000000", "60000",
  "0/3000000", nullptr, "1",         "t",
};

/**
 * Hosts before a failover: host 0 is the selected master on timeline 1,
 * host 1 a replica that has replayed all of its WAL, host 2 a replica that
 * lags and will be promoted, and host 3 a replica whose timeline is unknown.
 */
static void configure_failover_hosts(void) {
  configure_hosts(nullptr);
  host_count = 4;
  monitor_host_list[2] = (MonitorHost){.host = "promoted"};
  monitor_host_list[3] = (MonitorHost){.host = "unknown-timeline"};
  master_check_on_timeline(&monitor_host_list[0], "0/3000000", "1", 10000);
  save_master_index(0);
  publish_result_at(
    &monitor_host_list[1], CAUGHT_UP_ON_TIMELINE_1, 10300, 10305
  );
  const char *unknown[POLL_COLUMNS] = {
    "t", nullptr, "0/1000000", "0/1000000", "60000", "0/1000000", nullptr,
  };
  publish_result_at(&monitor_host_list[3], unknown, 10300, 10305);
}

/** Host 2 is promoted to timeline 2, behind host 1, and selected. */
static void promote_lagging_replica(void) {
  master_check_on_timeline(&monitor_host_list[2], "0/2000000", "2", 11000);
  save_master_index(2);
}

static void test_failover_marks_replica_at_once(void) {
  // Arrange
  configure_failover_hosts();
  MonitorHost *abandoned = &monitor_host_list[1];
  MonitorHost *promoted = &monitor_host_list[2];
  MonitorSnapshot snap = atomic_get_snapshot(abandoned);
  support_assert_true(
    !snap.status.timeline_mismatch && snap.lag_bytes == 0 && snap.lag_ms == 305,
    "before the failover, the replica is synchronous"
  );

  // Act: the promoted host is about to be selected, before the replica's
  // next check.
  master_check_on_timeline(promoted, "0/2000000", "2", 11000);
  mark_replicas_on_other_timelines(promoted);

  // Assert
  support_assert_true(
    get_master_index() == 0 &&
      atomic_get_snapshot(abandoned).status.timeline_mismatch,
    "the replica is marked before the new master is selected"
  );

  // Act
  save_master_index(2);

  // Assert
  snap = atomic_get_snapshot(abandoned);
  const LagThresholds written = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0x2000000,
  };
  const LagThresholds sync = {.max_lag_ms = 1000, .max_lag_bytes = 1048576};
  const LagThresholds any = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX
  };
  support_assert_true(
    snap.status.alive && snap.status.timeline_mismatch &&
      snap.lag_bytes == UINT64_MAX && snap.lag_ms == 60000 &&
      snap.lsn == 0x3000000,
    "the replica on the abandoned timeline is marked at once"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
        promoted &&
      find_replica(is_sync_replica_by_bytes, &sync, "test") == promoted &&
      find_replica(is_sync_replica_by_time, &sync, "test") == promoted &&
      find_replica(is_sync_replica_by_time_or_bytes, &sync, "test") ==
        promoted &&
      find_most_sync_replica_by_bytes(&sync, "test") == promoted,
    "it meets neither min_lsn nor a lag threshold"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &any, "test") != promoted,
    "it is still returned without a limit"
  );
  support_assert_true(
    !atomic_get_snapshot(&monitor_host_list[3]).status.timeline_mismatch,
    "a replica of unknown timeline is not marked"
  );

  // Act: marking again publishes nothing.
  const uint64_t seq = atomic_load(&abandoned->seq);
  mark_replicas_on_other_timelines(&monitor_host_list[2]);

  // Assert
  support_assert_true(
    atomic_load(&abandoned->seq) == seq, "a marked replica is not republished"
  );

  // Act: the replica's next check finds it on the new master's timeline.
  const char *following[POLL_COLUMNS] = {
    "t",         nullptr, "0/2000000", "0/2000000", "60000",
    "0/2000000", nullptr, "2",         "t",
  };
  publish_result_at(abandoned, following, 12000, 12005);

  // Assert
  snap = atomic_get_snapshot(abandoned);
  support_assert_true(
    !snap.status.timeline_mismatch && snap.lag_bytes == 0 &&
      snap.lag_ms == 1005,
    "the next check ends the mark"
  );
}

static void test_failover_during_replica_check(void) {
  // Arrange: the replica's result is read while the old master is
  // selected, and its check ends after the failover.
  configure_failover_hosts();
  MonitorHost *abandoned = &monitor_host_list[1];
  PGresult *res = make_poll_result(CAUGHT_UP_ON_TIMELINE_1);
  abandoned->iter_started_at_ms = 10900;
  abandoned->iter_data_ready = parse_result(abandoned, res, 10905);
  PQclear(res);
  promote_lagging_replica();

  // Act
  finish_iteration(abandoned, true, 11010);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(abandoned);
  support_assert_true(
    snap.status.timeline_mismatch && snap.lag_bytes == UINT64_MAX &&
      snap.lag_ms == 60000,
    "the master selected by the end of the check counts"
  );
}

static void test_client_timeline(void) {
  // Arrange: host 2 has been promoted to timeline 2 but is not selected
  // yet; host 0, the old master on timeline 1, still is. A client that
  // found the new master elsewhere wrote on it.
  configure_failover_hosts();
  MonitorHost *old_master = &monitor_host_list[0];
  MonitorHost *abandoned = &monitor_host_list[1];
  MonitorHost *follower = &monitor_host_list[3];
  master_check_on_timeline(&monitor_host_list[2], "0/2000100", "2", 11000);
  const LagThresholds lsn_only = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0x2000100,
  };
  LagThresholds written = lsn_only;
  written.timeline = 2;

  // Act & Assert: the replica left on timeline 1 is past the write.
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &lsn_only, "test") ==
      abandoned,
    "without the client's timeline, its position looks sufficient"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
      old_master,
    "with it, the replica is not on the write's timeline"
  );

  // Act: a replica that follows the new master has replayed the write.
  const char *following[POLL_COLUMNS] = {
    "t",         nullptr, "0/2000100", "0/2000100", "60000",
    "0/2000100", nullptr, "2",         "t",
  };
  publish_result_at(follower, following, 11300, 11305);

  // Assert
  support_assert_true(
    atomic_get_snapshot(follower).status.timeline_mismatch &&
      atomic_get_snapshot(follower).timeline == 2,
    "the follower differs from the master pg-status still selects"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
      follower,
    "the client's timeline vouches for its position"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &lsn_only, "test") ==
      abandoned,
    "without it, only the selected master's timeline counts"
  );

  // Act: the timeline alone, without min_lsn.
  const LagThresholds timeline_only = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = UINT64_MAX, .timeline = 1
  };

  // Assert
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &timeline_only, "test") ==
      abandoned,
    "the timeline filters replicas by itself"
  );

  // Act: the follower's timeline becomes unknown, then newer.
  const char *unknown[POLL_COLUMNS] = {
    "t", nullptr, "0/2000100", "0/2000100", "60000", "0/2000100", nullptr,
  };
  publish_result_at(follower, unknown, 12300, 12305);
  const bool unknown_matches = find_replica(
                                 is_sync_replica_by_time_and_bytes, &written,
                                 "test"
                               ) == follower;
  const char *newer[POLL_COLUMNS] = {
    "t",         nullptr, "0/2000100", "0/2000100", "60000",
    "0/2000100", nullptr, "3",         "t",
  };
  publish_result_at(follower, newer, 13300, 13305);

  // Assert
  support_assert_true(
    !unknown_matches, "an unknown timeline does not match the client's"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
      old_master,
    "nor does a newer one: the write may not be on its history"
  );
}

static void test_client_timeline_before_failover(void) {
  // Arrange: the client wrote on timeline 1; since then, host 2 has been
  // promoted to timeline 2 and selected, and host 1 stays on timeline 1.
  configure_failover_hosts();
  MonitorHost *abandoned = &monitor_host_list[1];
  MonitorHost *promoted = &monitor_host_list[2];
  master_check_on_timeline(promoted, "0/2000000", "2", 11000);
  mark_replicas_on_other_timelines(promoted);
  save_master_index(2);
  const LagThresholds written = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0x1000000,
    .timeline = 1,
  };

  // Act
  const MonitorHost *selected = find_replica(
    is_sync_replica_by_time_and_bytes, &written, "test"
  );

  // Assert
  support_assert_true(
    atomic_get_snapshot(abandoned).timeline == 1 && selected == promoted,
    "a replica left on the write's timeline after a failover is not used"
  );
}

/** Fails checks of `host`, one per second from `at_ms`, until it is dead. */
static void fail_until_dead(MonitorHost *host, const uint64_t at_ms) {
  for (unsigned int i = 0; i < failure_limit(); i++) {
    host->iter_started_at_ms = at_ms + ((uint64_t)i * 1000);
    finish_iteration(host, false, host->iter_started_at_ms + 5);
  }
}

static void test_lost_master_stays_timeline_reference(void) {
  // Arrange: the old master on timeline 1 is dead. Host 2, promoted to
  // timeline 2 behind host 1, is selected and lost before any replica check
  // ends.
  configure_failover_hosts();
  MonitorHost *abandoned = &monitor_host_list[1];
  MonitorHost *promoted = &monitor_host_list[2];
  fail_until_dead(&monitor_host_list[0], 10500);
  mark_replicas_on_other_timelines(nullptr);
  save_master_index(-1);
  master_check_on_timeline(promoted, "0/2000000", "2", 13000);
  mark_replicas_on_other_timelines(promoted);
  save_master_index(2);
  fail_until_dead(promoted, 14000);
  mark_replicas_on_other_timelines(nullptr);
  save_master_index(-1);

  // Act: the replica's next check finds it still on timeline 1.
  publish_result_at(abandoned, CAUGHT_UP_ON_TIMELINE_1, 17000, 17005);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(abandoned);
  const LagThresholds written = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0x2000100,
  };
  support_assert_true(
    snap.status.timeline_mismatch && snap.lag_bytes == UINT64_MAX,
    "the lost master, not the one before it, stays the reference"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time_and_bytes, &written, "test") ==
      nullptr,
    "the replica left on the abandoned history does not meet a write on it"
  );
  const LagThresholds caught_up = {
    .max_lag_ms = UINT64_MAX, .max_lag_bytes = 0
  };
  support_assert_true(
    find_replica(is_sync_replica_by_bytes, &caught_up, "test") == nullptr,
    "nor does it look caught up by bytes"
  );
}

static void test_client_timeline_after_master_lost(void) {
  // Arrange: the client wrote on timeline 1. Since then, host 2 has been
  // promoted to timeline 2 and selected, host 1 stays on timeline 1, and
  // host 2 has been lost, so that no master is selected.
  configure_failover_hosts();
  MonitorHost *promoted = &monitor_host_list[2];
  fail_until_dead(&monitor_host_list[0], 10500);
  mark_replicas_on_other_timelines(nullptr);
  save_master_index(-1);
  master_check_on_timeline(promoted, "0/2000000", "2", 13000);
  mark_replicas_on_other_timelines(promoted);
  save_master_index(2);
  fail_until_dead(promoted, 14000);
  mark_replicas_on_other_timelines(nullptr);
  save_master_index(-1);
  const LagThresholds written = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0x1000000,
    .timeline = 1,
  };

  // Act
  const MonitorHost *selected = find_replica(
    is_sync_replica_by_time_and_bytes, &written, "test"
  );

  // Assert
  support_assert_true(
    atomic_get_snapshot(&monitor_host_list[1]).timeline == 1 &&
      selected == nullptr,
    "the write's timeline vouches for no replica after the master on a "
    "later one is lost"
  );
}

static void run_failover_logging(void) {
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);
  configure_failover_hosts();
  promote_lagging_replica();
  mark_replicas_on_other_timelines(&monitor_host_list[2]);
  pg_status_log_shutdown();
}

static void test_failover_logging(void) {
  // Act
  char *logs = support_capture_standard_error(run_failover_logging);

  // Assert
  support_assert_contains(
    logs,
    "replica timeline differs from master host=replica timeline=1 "
    "master_timeline=2",
    "the mark is logged"
  );
  support_assert_contains(
    logs, "dimension=time synchronous=false lag_ms=60000 threshold_ms=1000",
    "the loss of time synchronization is logged"
  );

  // Cleanup
  free(logs);
}

static void run_timeline_logging(void) {
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_INFO);
  configure_hosts(nullptr);
  master_check_on_timeline_2();
  const char *abandoned[POLL_COLUMNS] = {
    "t", nullptr, "0/9000", "0/9000", "60000", nullptr, nullptr, "1", "f",
  };
  const char *following[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "60000", "0/3000", nullptr, "2", "t",
  };
  publish_result_at(&monitor_host_list[1], abandoned, 10300, 10305);
  publish_result_at(&monitor_host_list[1], abandoned, 11300, 11305);
  publish_result_at(&monitor_host_list[1], following, 12300, 12305);
  pg_status_log_shutdown();
}

static void test_timeline_logging(void) {
  // Act
  char *logs = support_capture_standard_error(run_timeline_logging);

  // Assert
  support_assert_contains(
    logs,
    "replica timeline differs from master host=replica timeline=1 "
    "master_timeline=2",
    "the mismatch is logged"
  );
  support_assert_contains(
    logs, "lag_bytes=unknown", "an unknown lag is logged as such"
  );
  support_assert_contains(
    logs, "replica timeline no longer differs from master host=replica",
    "the recovery is logged"
  );

  // Cleanup
  free(logs);
}

/** Epoch time, in ms, of the master's clock at monotonic time 0. */
static const uint64_t EPOCH_AT_ZERO_MS = UINT64_C(1700000000000);

/**
 * Checks `host` with its query sent at `sent_at_ms` and the result read
 * 4 ms later, its clock `clock_shift_ms` ahead of the master's.
 */
static MonitorSnapshot check_with_clock(
  MonitorHost *host, const char *const values[POLL_COLUMNS],
  const uint64_t sent_at_ms, const int64_t clock_shift_ms
) {
  char server_now[32];
  (void)snprintf(
    server_now, sizeof(server_now), "%lld",
    (long long)(EPOCH_AT_ZERO_MS + sent_at_ms + 2) + (long long)clock_shift_ms
  );
  const char *row[POLL_COLUMNS];
  for (int i = 0; i < POLL_COLUMNS - 1; i++) {
    row[i] = values[i];
  }
  row[POLL_COLUMNS - 1] = server_now;
  host->iter_query_sent_at_ms = sent_at_ms;
  publish_result_at(host, row, sent_at_ms, sent_at_ms + 4);
  return atomic_get_snapshot(host);
}

static void test_replica_clock_behind_master(void) {
  // Arrange: the replica lags, so that no master check bounds its time lag.
  configure_hosts(nullptr);
  MonitorHost *replica = &monitor_host_list[1];
  const char *master[POLL_COLUMNS] = {"f",     "0/5000", nullptr, nullptr,
                                      nullptr, nullptr,  "f"};
  (void)check_with_clock(&monitor_host_list[0], master, 10000, 0);
  save_master_index(0);
  const char *behind_age[POLL_COLUMNS] = {"t",   nullptr,  "0/3000", "0/3000",
                                          "500", "0/3000", nullptr};

  // Act: the replica's clock is 2 s behind, so the real age is 2500 ms.
  MonitorSnapshot snap = check_with_clock(replica, behind_age, 10300, -2000);

  // Assert: 8 ms is the error of measuring both clocks.
  const LagThresholds thresholds = {.max_lag_ms = 1000};
  support_assert_true(
    snap.lag_ms == 2492, "the commit age is not understated by the skew"
  );
  support_assert_true(
    find_replica(is_sync_replica_by_time, &thresholds, "test") ==
      &monitor_host_list[0],
    "the replica is not time-synchronous"
  );

  // Act: the commit is newer than the difference of the clocks.
  const char *negative_age[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/3000", "-1500", "0/3000", nullptr
  };
  snap = check_with_clock(replica, negative_age, 10400, -2000);

  // Assert
  support_assert_true(snap.lag_ms == 492, "a negative age is corrected too");

  // Act: the replica's clock is ahead, which overstates the age.
  const char *ahead_age[POLL_COLUMNS] = {"t",    nullptr,  "0/3000", "0/3000",
                                         "2500", "0/3000", nullptr};
  snap = check_with_clock(replica, ahead_age, 10500, 2000);

  // Assert
  support_assert_true(snap.lag_ms == 2500, "an overstated age is left alone");

  // Act: the difference is within the error of measuring it.
  snap = check_with_clock(replica, behind_age, 10600, -5);

  // Assert
  support_assert_true(snap.lag_ms == 500, "no correction within the error");

  // Act: the master's clock has not been measured.
  publish_result_at(&monitor_host_list[0], master, 11000, 11005);
  snap = check_with_clock(replica, behind_age, 11300, -2000);

  // Assert
  support_assert_true(snap.lag_ms == 500, "no correction without the master");
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
    host->query_fallback == QUERY_FULL && !host->iter_retry_with_query_fallback,
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
    host->iter_retry_with_query_fallback && poll_script.queries_sent == 0 &&
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

  // Arrange: the control file shows the replica's timeline.
  const char *values[POLL_COLUMNS] = {
    "t", nullptr, "0/3000", "0/2000", "1234", nullptr, nullptr, "1",
  };
  poll_script.result = make_poll_result(values);

  // Act
  advance_host_poll(host, 1030);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE && host->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead && !snap.status.master &&
      snap.lag_bytes == 12288 && snap.lag_ms == 1234 && snap.lsn == 0x2000 &&
      snap.timeline == 1,
    "successful fallback publishes fresh replica measurements"
  );

  // Act & Assert
  reset_iter_state(host, 1100);
  support_assert_true(send_query(host, 1100), "send next poll");
  support_assert_true(
    poll_script.last_query == streaming_replication_query_without_wal_receiver,
    "next poll on this connection skips the receiver"
  );
  poll_script.drained = true;
  support_assert_true(
    send_query(&monitor_host_list[0], 1100), "send another host poll"
  );
  support_assert_true(
    poll_script.last_query == streaming_replication_query,
    "permission fallback is isolated to the affected host"
  );
  close_conn(host);
  poll_script.drained = true;
  support_assert_true(send_query(host, 1100), "send after reconnect");
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
  support_assert_not_contains(
    logs, "control file", "the control file functions are still used"
  );

  // Cleanup
  free(logs);
}

static void test_permission_fallback_master(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();

  // Act
  read_step(host, 1010);
  const char *values[POLL_COLUMNS] = {"f", "0/9000", nullptr, nullptr,
                                      "0", nullptr,  "f"};
  poll_script.result = make_poll_result(values);
  advance_host_poll(host, 1020);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->failed_connections == 0 && snap.status.alive && snap.status.master &&
      !snap.status.possible_dead && snap.lag_bytes == 0 && snap.lag_ms == 0 &&
      snap.lsn == 0x9000 && host->master_wal_lsn == 0x9000,
    "successful fallback publishes master role and WAL position"
  );
}

static void test_permission_fallback_expires(void) {
  // Arrange: the fallback starts at 1010 ms on a connection that stays open.
  MonitorHost *host = configure_permission_poll();
  read_step(host, 1010);
  const char *values[POLL_COLUMNS] = {"t",    nullptr, "0/3000", "0/2000",
                                      "1234", nullptr, nullptr};
  poll_script.result = make_poll_result(values);
  advance_host_poll(host, 1020);
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE && host->failed_connections == 0,
    "fallback poll succeeds"
  );

  // Act & Assert
  poll_script.drained = true;
  support_assert_true(send_query(host, 301009), "send poll before expiry");
  support_assert_true(
    poll_script.last_query == streaming_replication_query_without_wal_receiver,
    "receiver statistics stay skipped for five minutes"
  );
  poll_script.drained = true;
  support_assert_true(send_query(host, 301010), "send poll after expiry");
  support_assert_true(
    poll_script.last_query == streaming_replication_query,
    "receiver statistics are retried without a reconnect"
  );
}

static void run_permission_fallback_control_file(void) {
  // Arrange: the query without the WAL receiver statistics is denied too.
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_WARNING);
  MonitorHost *host = configure_permission_poll();
  read_step(host, 1010);
  queue_error("42501");

  // Act
  advance_host_poll(host, 1020);

  // Assert
  support_assert_true(
    host->poll_state == HOST_POLL_QUERY_SEND &&
      host->iter_deadline_ms == 2000 && poll_script.queries_sent == 2 &&
      poll_script.last_query ==
        streaming_replication_query_without_wal_receiver_and_control_file &&
      host->failed_connections == 0,
    "the next retry also leaves out the control file functions"
  );

  // Arrange
  const char *values[POLL_COLUMNS] = {"t",    nullptr, "0/3000", "0/2000",
                                      "1234", nullptr, nullptr};
  poll_script.result = make_poll_result(values);

  // Act
  advance_host_poll(host, 1030);

  // Assert
  const MonitorSnapshot snap = atomic_get_snapshot(host);
  support_assert_true(
    host->poll_state == HOST_POLL_IDLE && host->failed_connections == 0 &&
      snap.status.alive && !snap.status.possible_dead && snap.lsn == 0x2000 &&
      snap.timeline == 0,
    "the check succeeds without the replica's timeline"
  );

  // Act & Assert
  reset_iter_state(host, 1100);
  support_assert_true(send_query(host, 301019), "send poll before expiry");
  support_assert_true(
    poll_script.last_query ==
      streaming_replication_query_without_wal_receiver_and_control_file,
    "the last fallback stays for five minutes from when it began"
  );
  poll_script.drained = true;
  support_assert_true(send_query(host, 301020), "send poll after expiry");
  support_assert_true(
    poll_script.last_query == streaming_replication_query,
    "the full query is tried again after five minutes"
  );

  // Cleanup
  pg_status_log_shutdown();
}

static void test_permission_fallback_control_file(void) {
  // Act
  char *logs = support_capture_standard_error(
    run_permission_fallback_control_file
  );

  // Assert
  const char *first = strstr(
    logs, "retrying poll without WAL receiver statistics for 5 minutes"
  );
  support_assert_true(first != nullptr, "the first fallback is logged");
  support_assert_contains(
    first,
    "retrying poll without WAL receiver statistics and control file "
    "functions for 5 minutes",
    "the second fallback is logged after it"
  );
  support_assert_contains(
    logs,
    "EXECUTE on pg_catalog.pg_control_checkpoint() and "
    "pg_catalog.pg_control_recovery()",
    "control file privilege hint"
  );
  support_assert_contains(
    logs, "unless the request passes timeline",
    "the lost protection is explained"
  );

  // Cleanup
  free(logs);
}

static void test_permission_fallback_failure(void) {
  // Arrange
  MonitorHost *host = configure_permission_poll();

  // Act: every variant of the query is denied.
  read_step(host, 1010);
  queue_error("42501");
  advance_host_poll(host, 1020);
  queue_error("42501");
  advance_host_poll(host, 1030);

  // Assert
  support_assert_true(
    poll_script.queries_sent == 2, "retry once with each fallback query"
  );
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
    {"idle_master_bounds_time_lag", test_idle_master_bounds_time_lag},
    {"commit_time_lag_below_bound", test_commit_time_lag_below_bound},
    {"lagging_replica_keeps_commit_time_lag",
     test_lagging_replica_keeps_commit_time_lag},
    {"unusable_master_check_keeps_commit_time_lag",
     test_unusable_master_check_keeps_commit_time_lag},
    {"idle_master_keeps_time_sync", test_idle_master_keeps_time_sync},
    {"unwritten_master_wal_keeps_commit_time_lag",
     test_unwritten_master_wal_keeps_commit_time_lag},
    {"replica_waits_for_master_check", test_replica_waits_for_master_check},
    {"master_check_with_new_wal_ends_wait",
     test_master_check_with_new_wal_ends_wait},
    {"failed_master_check_ends_wait", test_failed_master_check_ends_wait},
    {"master_wait_ends_at_deadline", test_master_wait_ends_at_deadline},
    {"replica_waits_only_when_master_check_helps",
     test_replica_waits_only_when_master_check_helps},
    {"replica_waits_for_due_master_check",
     test_replica_waits_for_due_master_check},
    {"master_check_that_does_not_start_ends_wait",
     test_master_check_that_does_not_start_ends_wait},
    {"replica_ahead_of_master_keeps_commit_time_lag",
     test_replica_ahead_of_master_keeps_commit_time_lag},
    {"replica_ahead_waits_for_next_master_check",
     test_replica_ahead_waits_for_next_master_check},
    {"selected_master_is_byte_lag_reference",
     test_selected_master_is_byte_lag_reference},
    {"byte_lag_unknown_without_reference",
     test_byte_lag_unknown_without_reference},
    {"unknown_commit_age", test_unknown_commit_age},
    {"replica_on_other_timeline", test_replica_on_other_timeline},
    {"replica_on_other_timeline_does_not_wait",
     test_replica_on_other_timeline_does_not_wait},
    {"failover_marks_replica_at_once", test_failover_marks_replica_at_once},
    {"failover_during_replica_check", test_failover_during_replica_check},
    {"failover_logging", test_failover_logging},
    {"client_timeline", test_client_timeline},
    {"client_timeline_before_failover", test_client_timeline_before_failover},
    {"lost_master_stays_timeline_reference",
     test_lost_master_stays_timeline_reference},
    {"client_timeline_after_master_lost",
     test_client_timeline_after_master_lost},
    {"timeline_logging", test_timeline_logging},
    {"replica_clock_behind_master", test_replica_clock_behind_master},
    {"later_master_check_keeps_byte_lag",
     test_later_master_check_keeps_byte_lag},
    {"diverged_replica_stops_waiting", test_diverged_replica_stops_waiting},
    {"replica_does_not_wait_for_unhealthy_master",
     test_replica_does_not_wait_for_unhealthy_master},
    {"master_wait_ends_before_next_check",
     test_master_wait_ends_before_next_check},
    {"replica_with_recent_commit_does_not_wait",
     test_replica_with_recent_commit_does_not_wait},
    {"permission_fallback_replica", test_permission_fallback_replica},
    {"permission_fallback_master", test_permission_fallback_master},
    {"permission_fallback_expires", test_permission_fallback_expires},
    {"permission_fallback_control_file", test_permission_fallback_control_file},
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
