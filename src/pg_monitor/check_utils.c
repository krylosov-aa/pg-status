/**
 * Utilities for checking the status of a host
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
 * Creates a connection to pg
 */
static PGconn *db_connect(const char *connection_str) {
  PGconn *conn = PQconnectdb(connection_str);
  if (PQstatus(conn) != CONNECTION_OK) {
    printf_error("connect error: %s \n ", PQerrorMessage(conn));
    PQfinish(conn);
    return nullptr;
  }

  return conn;
}

/**
 * Checks that the pg answer is valid. Fails with an error if it's not.
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
 * Converts pg lsn to bytes
 */
static unsigned long long parse_lsn(const char *lsn) {
  if (!lsn) {
    return 0;
  }

  char *slash = strchr(lsn, '/');
  if (!slash) {
    return 0;
  }

  errno = 0;
  const unsigned long hi = strtoul(lsn, NULL, 16);
  if (errno != 0) {
    return 0;
  }

  errno = 0;
  const unsigned long lo = strtoul(slash + 1, NULL, 16);
  if (errno != 0) {
    return 0;
  }

  return (unsigned long long)hi << 32 | lo;
}

/**
 * Selects the maximum lsn
 */
static unsigned long long max_lsn(
  const unsigned long long a, const unsigned long long b
) {
  return a > b ? a : b;
}

/**
 * Logs changes to stdout if there have been changes in the lag in time
 */
static void log_time_lag_changes(
  const MonitorHost *host, const MonitorStatus new_status,
  const MonitorStatus status
) {
  const bool is_sync = is_sync_replica_by_time(new_status);
  if (is_sync != is_sync_replica_by_time(status)) {
    if (is_sync) {
      printf("%s: synchronous in time\n", host->host);
    } else {
      printf("%s: out of sync in time\n", host->host);
    }
  }
}

/**
 * Logs changes to stdout if there have been changes in the lag in bytes
 */
static void log_bytes_lag_changes(
  const MonitorHost *host, const MonitorStatus new_status,
  const MonitorStatus status
) {
  const bool is_sync = is_sync_replica_by_bytes(new_status);
  if (is_sync != is_sync_replica_by_bytes(status)) {
    if (is_sync) {
      printf("%s: synchronous in bytes\n", host->host);
    } else {
      printf("%s: out of sync in bytes\n", host->host);
    }
  }
}

/**
 * Logs changes to stdout if there have been changes in the status
 */
static void log_changes(
  const MonitorHost *host, const MonitorStatus new_status,
  const MonitorStatus status
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
    log_time_lag_changes(host, new_status, status);
    log_bytes_lag_changes(host, new_status, status);
    return;
  }

  if (new_status.master != status.master) {
    if (new_status.master) {
      printf("%s: master\n", host->host);
      return;
    }
    printf("%s: replica\n", host->host);
    log_time_lag_changes(host, new_status, status);
    log_bytes_lag_changes(host, new_status, status);
  }
}

static MonitorStatus dead_status(void) {
  return (MonitorStatus){.alive = false,
                         .master = false,
                         .sync_by_time = false,
                         .sync_by_bytes = false,
                         .possible_dead = true};
}

static MonitorStatus master_status(void) {
  return (MonitorStatus){.alive = true,
                         .master = true,
                         .sync_by_time = true,
                         .sync_by_bytes = true,
                         .possible_dead = false};
}

static MonitorStatus replica_status(
  const unsigned long long lag_ms, const unsigned long long lag_bytes
) {
  return (MonitorStatus){.alive = true,
                         .master = false,
                         .sync_by_time = lag_ms <= parameters.sync_max_lag_ms,
                         .sync_by_bytes = lag_bytes <=
                                          parameters.sync_max_lag_bytes,
                         .possible_dead = false};
}

/**
 * Updates the host status
 *
 * This way, concurrent threads can read the current status lock-free,
 * and the writer can perform lock-free writes.
 *
 * But the downside of this solution is the inconsistency: if the writer
 * has started updating the hosts but hasn't finished updating all of
 * them, some hosts may have the new data while others still have the old data.
 *
 * A replica’s lsn lag is defined as the difference between its own lsn and
 * the greater of the lsn received by the replica or the lsn on the master.
 * Therefore, even if a replica does not receive a new lsn, a measurable
 * lag can still occur.
 */
void check_host_streaming_replication(
  MonitorHost *host, const unsigned int max_fails
) {
  static unsigned long long master_lsn = 0;
  const MonitorStatus status = atomic_load_explicit(
    &host->status, memory_order_relaxed
  );
  MonitorStatus new_status = status;

  PGconn *conn = db_connect(host->connection_str);

  PGresult *q_res = nullptr;
  if (conn) {
    q_res = execute_sql(conn, streaming_replication_query);
  }

  unsigned long long new_lag_ms = 0;
  unsigned long long new_lag_bytes = 0;

  if (!q_res) {
    host->failed_connections++;
    new_status.possible_dead = true;
    if (host->failed_connections >= max_fails) {
      new_status = dead_status();
    }
  } else {
    host->failed_connections = 0;
    new_status.possible_dead = false;

    const bool is_replica = is_t(PQgetvalue(q_res, 0, 0));
    if (is_replica) {
      new_lag_ms = str_to_ull(PQgetvalue(q_res, 0, 4));

      const unsigned long long replica_received_lsn = parse_lsn(
        PQgetvalue(q_res, 0, 2)
      );
      const unsigned long long replica_lsn = parse_lsn(PQgetvalue(q_res, 0, 3));
      new_lag_bytes = max_lsn(master_lsn, replica_received_lsn) - replica_lsn;
      new_status = replica_status(new_lag_ms, new_lag_bytes);
    } else {
      new_status = master_status();
      master_lsn = parse_lsn(PQgetvalue(q_res, 0, 1));
    }
  }

  atomic_store_explicit(&host->lag_ms, new_lag_ms, memory_order_relaxed);
  atomic_store_explicit(&host->lag_bytes, new_lag_bytes, memory_order_relaxed);
  atomic_store_explicit(&host->status, new_status, memory_order_relaxed);
  log_changes(host, new_status, status);

  if (q_res) {
    PQclear(q_res);
  }

  if (conn) {
    PQfinish(conn);
  }
}
