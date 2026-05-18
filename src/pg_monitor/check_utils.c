/**
 * Utilities for checking the status of a host
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "pg_monitor.h"
#include "utils.h"

#ifdef __APPLE__
#include <libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif
#include <stdio.h>

/**
 * Converts pg true to bool true
 */
static bool is_t(const char *pg_val) {
  return is_equal_strings(pg_val, "t");
}

/**
 * Maximum lifetime of a persistent connection, in milliseconds.
 * After this it is dropped so upstream proxies (e.g. haproxy with
 * "disable server") get a chance to re-route the next session.
 */
constexpr unsigned int RECONNECT_AFTER_MS = 30000;

/**
 * Returns a usable libpq connection for the host, reusing host->conn
 * when possible. A broken connection is reset; if reset fails or the
 * persistent connection has lived longer than RECONNECT_AFTER_MS, it
 * is closed and host->conn is cleared so the next call reconnects.
 */
static PGconn *db_connect(MonitorHost *host) {
  const unsigned int ms = parameters.sleep_ms > 0
    ? (unsigned int)parameters.sleep_ms : RECONNECT_AFTER_MS;
  const unsigned int every = ms >= RECONNECT_AFTER_MS
    ? 1 : RECONNECT_AFTER_MS / ms;
  if (host->conn && host->checks_since_reconnect >= every) {
    PQfinish(host->conn);
    host->conn = nullptr;
    host->checks_since_reconnect = 0;
  }

  if (host->conn) {
    if (PQstatus(host->conn) == CONNECTION_OK) {
      return host->conn;
    }
    PQreset(host->conn);
    if (PQstatus(host->conn) == CONNECTION_OK) {
      return host->conn;
    }
    printf_error("connect error: %s \n ", PQerrorMessage(host->conn));
    PQfinish(host->conn);
    host->conn = nullptr;
    return nullptr;
  }

  PGconn *conn = PQconnectdb(host->connection_str);
  if (PQstatus(conn) != CONNECTION_OK) {
    printf_error("connect error: %s \n ", PQerrorMessage(conn));
    PQfinish(conn);
    return nullptr;
  }

  host->conn = conn;
  return conn;
}

/**
 * Closes all persistent libpq connections held by the host list.
 */
void close_host_connections(void) {
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *item = &monitor_host_list[i];
    if (item->conn) {
      PQfinish(item->conn);
      item->conn = nullptr;
    }
  }
}

/**
 * Checks that the pg answer is valid. Returns 0 on success, 1 on error
 * (and prints the pg error message to stderr).
 */
static int check_exec_result(const PGconn *conn, const PGresult *result) {
  const ExecStatusType resStatus = PQresultStatus(result);
  if (resStatus != PGRES_TUPLES_OK && resStatus != PGRES_COMMAND_OK) {
    printf_error("execute sql error: %s \n ", PQerrorMessage(conn));

    return 1;
  }
  return 0;
}
/**
 * Executes sql query with verification that there is a correct pg answer
 */
static PGresult *execute_sql(PGconn *conn, const char *query) {
  PGresult *res = PQexec(conn, query);
  const int check_result = check_exec_result(conn, res);
  if (check_result == 1) {
    PQclear(res);
    return nullptr;
  }
  return res;
}

/**
 * sql query to get host status
 */
static const char streaming_replication_query[] = {
#embed "streaming_replication_query.sql"
  , '\0'
};

/**
 * Converts pg lsn to bytes; malformed/missing input yields 0.
 */
static uint64_t parse_lsn(const char *lsn) {
  uint64_t value = 0;
  (void)try_parse_lsn(lsn, &value);
  return value;
}

/**
 * Selects the maximum lsn
 */
static uint64_t max_lsn(const uint64_t a, const uint64_t b) {
  return a > b ? a : b;
}

/**
 * Logs changes to stdout if there have been changes in the lag in time
 */
static void log_time_lag_changes(
  const MonitorHost *host, const uint64_t new_lag_ms, const uint64_t lag_ms
) {
  if (new_lag_ms > parameters.sync_max_lag_ms) {
    if (lag_ms <= parameters.sync_max_lag_ms) {
      printf("%s: out of sync in time\n", host->host);
    }
  } else {
    if (lag_ms > parameters.sync_max_lag_ms) {
      printf("%s: synchronous in time\n", host->host);
    }
  }
}

/**
 * Logs changes to stdout if there have been changes in the lag in bytes
 */
static void log_bytes_lag_changes(
  const MonitorHost *host, const uint64_t new_lag_bytes,
  const uint64_t lag_bytes
) {
  if (new_lag_bytes > parameters.sync_max_lag_bytes) {
    if (lag_bytes <= parameters.sync_max_lag_bytes) {
      printf("%s: out of sync in bytes\n", host->host);
    }
  } else {
    if (lag_bytes > parameters.sync_max_lag_bytes) {
      printf("%s: synchronous in bytes\n", host->host);
    }
  }
}

/**
 * Logs changes to stdout if there have been changes in the status
 */
static void log_changes(
  const MonitorHost *host, const MonitorStatus new_status,
  const MonitorStatus status, const uint64_t new_lag_ms, const uint64_t lag_ms,
  const uint64_t new_lag_bytes, const uint64_t lag_bytes
) {
  if (new_status.possible_dead != status.possible_dead &&
      new_status.possible_dead) {
    printf("%s: possible dead\n", host->host);
    return;
  }

  if (new_status.alive != status.alive) {
    if (!new_status.alive) {
      printf("%s: dead\n", host->host);
      return;
    }
    if (new_status.master) {
      printf("%s: master\n", host->host);
      return;
    }
    printf("%s: replica\n", host->host);
    log_time_lag_changes(host, new_lag_ms, lag_ms);
    log_bytes_lag_changes(host, new_lag_bytes, lag_bytes);
    return;
  }

  if (new_status.master != status.master) {
    if (new_status.master) {
      printf("%s: master\n", host->host);
      return;
    }
    printf("%s: replica\n", host->host);
    log_time_lag_changes(host, new_lag_ms, lag_ms);
    log_bytes_lag_changes(host, new_lag_bytes, lag_bytes);
  }
}

static MonitorStatus dead_status(void) {
  return (MonitorStatus){
    .alive = false, .master = false, .possible_dead = true
  };
}

static MonitorStatus master_status(void) {
  return (MonitorStatus){.alive = true, .master = true, .possible_dead = false};
}

static MonitorStatus replica_status() {
  return (MonitorStatus){
    .alive = true, .master = false, .possible_dead = false
  };
}

/**
 * Updates the host status, lag_ms and lag_bytes as a consistent snapshot
 * via the seqlock on host->seq:
 *  - Bump seq to odd (writer in progress).
 *  - Store new lag_ms, lag_bytes, status.
 *  - Bump seq back to even (stable).
 * Readers (atomic_get_snapshot) loop until they see two equal, even seq
 * values around their reads, so they never observe a torn snapshot.
 * Writer never waits on readers; readers never block the writer.
 *
 * Inconsistency between hosts is still permitted by design: while the
 * writer is partway through monitor_host_list[], some hosts have the
 * new state and others still have the old one.
 *
 * A replica's lsn lag is defined as the difference between its own lsn and
 * the greater of the lsn received by the replica or the lsn on the master.
 * Therefore, even if a replica does not receive a new lsn, a measurable
 * lag can still occur.
 */
void check_host_streaming_replication(
  MonitorHost *host, const unsigned int max_fails
) {
  static uint64_t master_lsn = 0;
  const uint64_t lag_ms = atomic_get_lag_ms(host);
  const uint64_t lag_bytes = atomic_get_lag_bytes(host);
  const MonitorStatus status = atomic_load_explicit(
    &host->status, memory_order_relaxed
  );
  MonitorStatus new_status = status;

  PGconn *conn = db_connect(host);

  PGresult *q_res = nullptr;
  if (conn) {
    q_res = execute_sql(conn, streaming_replication_query);
  }

  uint64_t new_lag_ms = 0;
  uint64_t new_lag_bytes = 0;
  uint64_t new_lsn = 0;

  if (!q_res) {
    host->failed_connections++;
    new_status.possible_dead = true;
    if (host->failed_connections >= max_fails) {
      new_status = dead_status();
    }
    // A failed query may leave the connection in a bad state. Drop it so
    // the next iteration reconnects from scratch.
    if (host->conn) {
      PQfinish(host->conn);
      host->conn = nullptr;
      host->checks_since_reconnect = 0;
    }
  } else {
    host->failed_connections = 0;
    new_status.possible_dead = false;
    host->checks_since_reconnect++;

    const bool is_replica = is_t(PQgetvalue(q_res, 0, 0));
    if (is_replica) {
      new_lag_ms = str_to_ull(PQgetvalue(q_res, 0, 4));

      const uint64_t replica_received_lsn = parse_lsn(PQgetvalue(q_res, 0, 2));
      const uint64_t replica_lsn = parse_lsn(PQgetvalue(q_res, 0, 3));
      new_lag_bytes = max_lsn(master_lsn, replica_received_lsn) - replica_lsn;
      new_lsn = replica_lsn;
      new_status = replica_status();
    } else {
      new_status = master_status();
      master_lsn = parse_lsn(PQgetvalue(q_res, 0, 1));
      new_lsn = master_lsn;
    }
  }

  atomic_fetch_add_explicit(&host->seq, 1, memory_order_release);
  atomic_store_explicit(&host->lag_ms, new_lag_ms, memory_order_relaxed);
  atomic_store_explicit(&host->lag_bytes, new_lag_bytes, memory_order_relaxed);
  atomic_store_explicit(&host->lsn, new_lsn, memory_order_relaxed);
  atomic_store_explicit(&host->status, new_status, memory_order_relaxed);
  atomic_fetch_add_explicit(&host->seq, 1, memory_order_release);

  log_changes(
    host, new_status, status, new_lag_ms, lag_ms, new_lag_bytes, lag_bytes
  );

  if (q_res) {
    PQclear(q_res);
  }
}
