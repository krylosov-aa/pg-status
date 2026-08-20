/**
 * Utilities for checking the status of a host
 */

#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "pg_monitor.h"
#include "utils.h"

#ifdef __APPLE__
#include <libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif

/**
 * Converts pg true to bool true
 */
static bool is_t(const char *pg_val) {
  return is_equal_strings(pg_val, "t");
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
  if (
    new_status.possible_dead != status.possible_dead && new_status.possible_dead
  ) {
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

static bool send_query(const MonitorHost *host) {
  if (PQsendQuery(host->conn, streaming_replication_query) == 0) {
    printf_error(
      "send query failed for %s: %s", host->host, PQerrorMessage(host->conn)
    );
    return false;
  }
  return true;
}

static void poll_state_connecting(MonitorHost *host) {
  host->poll_state = HOST_POLL_CONNECTING;
  host->poll_events = POLLOUT;
}

static void poll_state_query_send(MonitorHost *host) {
  host->poll_state = HOST_POLL_QUERY_SEND;
  host->poll_events = POLLOUT;
}

static void poll_state_query_read(MonitorHost *host) {
  host->poll_state = HOST_POLL_QUERY_READ;
  host->poll_events = POLLIN;
}

static void poll_state_idle(MonitorHost *host, const uint64_t now_ms) {
  host->poll_state = HOST_POLL_IDLE;
  host->poll_events = 0;
  host->iter_deadline_ms = 0;
  host->iter_data_ready = false;
  host->next_poll_at_ms = now_ms + (uint64_t)parameters.sleep_ms;
}

/**
 * Last observed master LSN. Used to compute the byte lag of replicas
 * whose iteration completes in a different order than the master's.
 * Lives in writer-only memory; with a single writer thread, no
 * synchronization is needed.
 */
static uint64_t master_lsn = 0;

/**
 * Closes the underlying PGconn if open and resets its bookkeeping.
 */
static void close_conn(MonitorHost *host) {
  if (host->conn) {
    PQfinish(host->conn);
    host->conn = nullptr;
  }
  host->connected_at_ms = 0;
}

/**
 * Closes the underlying PGconn if open, and it's time for refresh
 */
static void close_conn_if_need(MonitorHost *host, const uint64_t now_ms) {
  if (host->conn) {
    const bool too_old = (now_ms - host->connected_at_ms) >
                         parameters.conn_max_age_ms;
    if (too_old || PQstatus(host->conn) != CONNECTION_OK) {
      close_conn(host);
    }
  }
}

static bool validate_sql_result(const MonitorHost *host, const PGresult *res) {
  const ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
    printf_error("execute sql error: %s", PQerrorMessage(host->conn));
    return false;
  }
  if (PQntuples(res) != 1) {
    printf_error(
      "unexpected result rows from %s: %d", host->host, PQntuples(res)
    );
    return false;
  }
  return true;
}

static void parse_replica_result(MonitorHost *host, const PGresult *res) {
  const uint64_t replica_received_lsn = parse_lsn(PQgetvalue(res, 0, 2));
  const uint64_t replica_lsn = parse_lsn(PQgetvalue(res, 0, 3));
  host->iter_new_lag_ms = str_to_ull(PQgetvalue(res, 0, 4));
  host->iter_new_lag_bytes = max_lsn(master_lsn, replica_received_lsn) -
                             replica_lsn;
  host->iter_new_lsn = replica_lsn;
  host->iter_new_status = replica_status();
}

static void parse_master_result(MonitorHost *host, const PGresult *res) {
  master_lsn = parse_lsn(PQgetvalue(res, 0, 1));
  host->iter_new_status = master_status();
  host->iter_new_lsn = master_lsn;
  host->iter_new_lag_ms = 0;
  host->iter_new_lag_bytes = 0;
}

/**
 * Parses the streaming-replication row into the host's staging fields.
 * Returns true on success, false on any error.
 */
static bool parse_result(MonitorHost *host, const PGresult *res) {
  const bool result_ok = validate_sql_result(host, res);
  if (!result_ok) {
    return false;
  }

  const bool is_replica = is_t(PQgetvalue(res, 0, 0));
  if (is_replica) {
    parse_replica_result(host, res);
  } else {
    parse_master_result(host, res);
  }
  return true;
}

/**
 * Closes out one poll iteration. On success, publishes the staged data;
 * on failure, bumps failed_connections, marks possible_dead (or dead),
 * and closes the connection so the next iteration will reconnect.
 * Always returns the host to IDLE and schedules the next iteration
 * `sleep_ms` into the future.
 */
static void finish_iteration(
  MonitorHost *host, const bool success, const uint64_t now_ms
) {
  const MonitorStatus old_status = atomic_get_status(host);
  const uint64_t old_lag_ms = atomic_get_lag_ms(host);
  const uint64_t old_lag_bytes = atomic_get_lag_bytes(host);

  MonitorStatus new_status;
  uint64_t new_lag_ms;
  uint64_t new_lag_bytes;
  uint64_t new_lsn;

  if (success && host->iter_data_ready) {
    host->failed_connections = 0;
    new_status = host->iter_new_status;
    new_lag_ms = host->iter_new_lag_ms;
    new_lag_bytes = host->iter_new_lag_bytes;
    new_lsn = host->iter_new_lsn;
  } else {
    host->failed_connections++;
    new_status = old_status;
    new_status.possible_dead = true;
    if (host->failed_connections >= parameters.max_fails) {
      new_status = dead_status();
    }
    new_lag_ms = 0;
    new_lag_bytes = 0;
    new_lsn = 0;
    close_conn(host);
  }

  publish_monitor_snapshot(
    host, (MonitorSnapshot){
            .status = new_status,
            .lag_ms = new_lag_ms,
            .lag_bytes = new_lag_bytes,
            .lsn = new_lsn,
          }
  );
  log_changes(
    host, new_status, old_status, new_lag_ms, old_lag_ms, new_lag_bytes,
    old_lag_bytes
  );

  if (
    success && host->conn &&
    (now_ms - host->connected_at_ms) > parameters.conn_max_age_ms
  ) {
    close_conn(host);
  }

  poll_state_idle(host, now_ms);
}

static bool consume_input(const MonitorHost *host) {
  if (PQconsumeInput(host->conn) == 0) {
    printf_error(
      "consume input failed for %s: %s", host->host, PQerrorMessage(host->conn)
    );
    return false;
  }
  return true;
}

/**
 * Drives the QUERY_READ phase: consume any pending input, drain ready
 * results, finish if the end-of-results NULL is observed, otherwise
 * suspend waiting on POLLIN.
 */
static void read_step(MonitorHost *host, const uint64_t now_ms) {
  const bool consumed = consume_input(host);
  if (!consumed) {
    finish_iteration(host, false, now_ms);
    return;
  }

  while (PQisBusy(host->conn) == 0) {
    PGresult *res = PQgetResult(host->conn);
    if (res == nullptr) {
      finish_iteration(host, host->iter_data_ready, now_ms);
      return;
    }
    if (!host->iter_data_ready) {
      host->iter_data_ready = parse_result(host, res);
    }
    PQclear(res);
  }
  host->poll_events = POLLIN;
}

typedef enum {
  FLUSH_ERROR = -1,
  FLUSH_FULL = 0,  // Everything has been sent
  FLUSH_PART = 1,  // Only a part has been sent
} FlushState;

/**
 * Drives the QUERY_SEND phase: flush any pending request bytes.
 * On full flush, optimistically tries read_step in the same wake.
 */
static void flush_step(MonitorHost *host, const uint64_t now_ms) {
  const FlushState flush_state = (FlushState)PQflush(host->conn);
  switch (flush_state) {
    case FLUSH_FULL:
      poll_state_query_read(host);
      read_step(host, now_ms);
      return;
    case FLUSH_PART:
      host->poll_events = POLLOUT;
      return;
    case FLUSH_ERROR:
      printf_error(
        "flush failed for %s: %s", host->host, PQerrorMessage(host->conn)
      );
      finish_iteration(host, false, now_ms);
      return;
  }
}

static bool pq_set_non_blocking(MonitorHost *host) {
  if (PQsetnonblocking(host->conn, 1) != 0) {
    printf_error(
      "set non-blocking failed for %s: %s", host->host,
      PQerrorMessage(host->conn)
    );
    return false;
  }
  return true;
}

/**
 * Drives the CONNECTING phase via PQconnectPoll. On success, sets up
 * the connection for non-blocking sends and kicks off the query.
 */
static void advance_connecting(MonitorHost *host, const uint64_t now_ms) {
  const PostgresPollingStatusType st = PQconnectPoll(host->conn);
  switch (st) {
    case PGRES_POLLING_READING:
      host->poll_events = POLLIN;
      return;
    case PGRES_POLLING_WRITING:
      host->poll_events = POLLOUT;
      return;
    case PGRES_POLLING_OK:
      host->connected_at_ms = now_ms;

      const bool set = pq_set_non_blocking(host);
      if (!set) {
        finish_iteration(host, false, now_ms);
        return;
      }

      const bool query_sent = send_query(host);
      if (!query_sent) {
        finish_iteration(host, false, now_ms);
        return;
      }

      poll_state_query_send(host);
      flush_step(host, now_ms);
      return;
    case PGRES_POLLING_ACTIVE:
    case PGRES_POLLING_FAILED:
    default:
      printf_error("connect error: %s", PQerrorMessage(host->conn));
      finish_iteration(host, false, now_ms);
      return;
  }
}

static bool start_connect(MonitorHost *host) {
  host->conn = PQconnectStart(host->connection_str);
  if (host->conn == nullptr || PQstatus(host->conn) == CONNECTION_BAD) {
    printf_error(
      "connect start failed for %s: %s", host->host,
      host->conn ? PQerrorMessage(host->conn) : "out of memory"
    );
    return false;
  }
  return true;
}

static void reset_iter_state(MonitorHost *host, const uint64_t now_ms) {
  host->iter_data_ready = false;
  host->iter_new_status = (MonitorStatus){
    .alive = false, .master = false, .possible_dead = false
  };
  host->iter_new_lag_ms = 0;
  host->iter_new_lag_bytes = 0;
  host->iter_new_lsn = 0;
  host->iter_deadline_ms = now_ms + parameters.query_timeout_ms;
}

void start_host_poll(MonitorHost *host, const uint64_t now_ms) {
  reset_iter_state(host, now_ms);
  close_conn_if_need(host, now_ms);

  if (host->conn == nullptr) {
    const bool started = start_connect(host);
    if (started) {
      poll_state_connecting(host);
    } else {
      finish_iteration(host, false, now_ms);
    }
    return;
  }

  const bool query_sent = send_query(host);
  if (!query_sent) {
    finish_iteration(host, false, now_ms);
    return;
  }
  poll_state_query_send(host);
  flush_step(host, now_ms);
}

void advance_host_poll(MonitorHost *host, const uint64_t now_ms) {
  switch (host->poll_state) {
    case HOST_POLL_CONNECTING:
      advance_connecting(host, now_ms);
      return;
    case HOST_POLL_QUERY_SEND:
      flush_step(host, now_ms);
      return;
    case HOST_POLL_QUERY_READ:
      read_step(host, now_ms);
      return;
    case HOST_POLL_IDLE:
      return;
  }
}

void timeout_host_poll(MonitorHost *host, const uint64_t now_ms) {
  printf_error(
    "host %s: poll iteration timed out after %llums", host->host,
    (unsigned long long)parameters.query_timeout_ms
  );
  close_conn(host);
  finish_iteration(host, false, now_ms);
}

int host_socket(const MonitorHost *host) {
  if (host->conn == nullptr) {
    return -1;
  }
  return PQsocket(host->conn);
}

void close_all_host_connections(void) {
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    close_conn(host);
    host->poll_state = HOST_POLL_IDLE;
    host->poll_events = 0;
    host->iter_data_ready = false;
  }
}
