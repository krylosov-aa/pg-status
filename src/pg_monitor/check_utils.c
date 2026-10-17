/**
 * Utilities for checking the status of a host
 */

#include <libpq-fe.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>

#include "connection.h"
#include "logger.h"
#include "pg_monitor.h"
#include "streaming_replication_query.h"
#include "utils.h"

/**
 * Converts pg true to bool true
 */
static bool is_t(const char *pg_val) {
  return is_equal_strings(pg_val, "t");
}

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
 * Selects the later of two monotonic times
 */
static uint64_t max_ms(const uint64_t a, const uint64_t b) {
  return a > b ? a : b;
}

/**
 * Logs changes in the lag-in-time classification.
 */
static void log_time_lag_changes(
  const MonitorHost *host, const uint64_t new_lag_ms, const uint64_t lag_ms
) {
  if (new_lag_ms > parameters.sync_max_lag_ms) {
    if (lag_ms <= parameters.sync_max_lag_ms) {
      pg_status_log(
        PG_STATUS_LOG_INFO, "monitor",
        "replica synchronization changed host=%s dimension=time "
        "synchronous=false lag_ms=%llu threshold_ms=%llu",
        host->host, (unsigned long long)new_lag_ms,
        (unsigned long long)parameters.sync_max_lag_ms
      );
    }
  } else {
    if (lag_ms > parameters.sync_max_lag_ms) {
      pg_status_log(
        PG_STATUS_LOG_INFO, "monitor",
        "replica synchronization changed host=%s dimension=time "
        "synchronous=true lag_ms=%llu threshold_ms=%llu",
        host->host, (unsigned long long)new_lag_ms,
        (unsigned long long)parameters.sync_max_lag_ms
      );
    }
  }
}

/**
 * Logs changes in the lag-in-bytes classification.
 */
static void log_bytes_lag_changes(
  const MonitorHost *host, const uint64_t new_lag_bytes,
  const uint64_t lag_bytes
) {
  if (new_lag_bytes > parameters.sync_max_lag_bytes) {
    if (lag_bytes <= parameters.sync_max_lag_bytes) {
      pg_status_log(
        PG_STATUS_LOG_INFO, "monitor",
        "replica synchronization changed host=%s dimension=bytes "
        "synchronous=false lag_bytes=%llu threshold_bytes=%llu",
        host->host, (unsigned long long)new_lag_bytes,
        (unsigned long long)parameters.sync_max_lag_bytes
      );
    }
  } else {
    if (lag_bytes > parameters.sync_max_lag_bytes) {
      pg_status_log(
        PG_STATUS_LOG_INFO, "monitor",
        "replica synchronization changed host=%s dimension=bytes "
        "synchronous=true lag_bytes=%llu threshold_bytes=%llu",
        host->host, (unsigned long long)new_lag_bytes,
        (unsigned long long)parameters.sync_max_lag_bytes
      );
    }
  }
}

/**
 * Logs role, availability, and synchronization transitions.
 */
static void log_changes(
  const MonitorHost *host, const MonitorStatus new_status,
  const MonitorStatus status, const uint64_t new_lag_ms, const uint64_t lag_ms,
  const uint64_t new_lag_bytes, const uint64_t lag_bytes,
  const bool reached_dead_threshold
) {
  if (reached_dead_threshold) {
    pg_status_log(
      PG_STATUS_LOG_WARNING, "monitor", "host state changed host=%s state=dead",
      host->host
    );
    return;
  }

  if (new_status.alive != status.alive) {
    if (!new_status.alive) {
      pg_status_log(
        PG_STATUS_LOG_WARNING, "monitor",
        "host state changed host=%s state=dead", host->host
      );
      return;
    }
    pg_status_log(
      PG_STATUS_LOG_INFO, "monitor",
      "host state changed host=%s state=alive role=%s", host->host,
      new_status.master ? "master" : "replica"
    );
    if (!new_status.master) {
      log_time_lag_changes(host, new_lag_ms, lag_ms);
      log_bytes_lag_changes(host, new_lag_bytes, lag_bytes);
    }
    return;
  }

  bool recovered = false;
  if (new_status.possible_dead != status.possible_dead) {
    if (new_status.possible_dead) {
      pg_status_log(
        PG_STATUS_LOG_WARNING, "monitor",
        "host state changed host=%s state=possible_dead", host->host
      );
      return;
    }
    pg_status_log(
      PG_STATUS_LOG_INFO, "monitor",
      "host state changed host=%s state=alive role=%s", host->host,
      new_status.master ? "master" : "replica"
    );
    recovered = true;
  }

  if (!recovered && new_status.master != status.master) {
    pg_status_log(
      PG_STATUS_LOG_INFO, "monitor", "host role changed host=%s role=%s",
      host->host, new_status.master ? "master" : "replica"
    );
  }

  if (new_status.alive && !new_status.master) {
    log_time_lag_changes(host, new_lag_ms, lag_ms);
    log_bytes_lag_changes(host, new_lag_bytes, lag_bytes);
  }
}

/** Share one error-log deadline across all failures of this host. */
static PGStatusLogLevel poll_error_level(
  MonitorHost *host, const uint64_t now_ms
) {
  if (now_ms < host->next_error_log_at_ms) {
    return PG_STATUS_LOG_DEBUG;
  }
  // Saturate so that a huge interval cannot wrap the deadline into the past.
  const uint64_t interval_ms = parameters.poll_error_log_interval_ms;
  host->next_error_log_at_ms = interval_ms > UINT64_MAX - now_ms
                                 ? UINT64_MAX
                                 : now_ms + interval_ms;
  return PG_STATUS_LOG_ERROR;
}

static void log_postgres_error(
  MonitorHost *host, const char *operation, const uint64_t now_ms
) {
  const char *error = host->conn ? PQerrorMessage(host->conn) : "out of memory";
  pg_status_log(
    poll_error_level(host, now_ms), "monitor",
    "PostgreSQL operation failed operation=%s host=%s error=%s", operation,
    host->host, error
  );
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
    .alive = true,
    .master = false,
    .possible_dead = false,
  };
}

/**
 * How long a host is polled without WAL receiver statistics after a
 * permission error before they are tried again on the same connection, so
 * granted privileges take effect without waiting for a reconnect.
 */
enum { WAL_RECEIVER_RETRY_MS = 300000 };

static bool send_query(MonitorHost *host, const uint64_t now_ms) {
  if (host->wal_receiver_disabled && now_ms >= host->wal_receiver_retry_at_ms) {
    host->wal_receiver_disabled = false;
  }
  const char *query = host->wal_receiver_disabled
                        ? streaming_replication_query_without_wal_receiver
                        : streaming_replication_query;
  if (PQsendQuery(host->conn, query) == 0) {
    log_postgres_error(host, "send_query", now_ms);
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

/** Number of consecutive failed checks after which a host is dead. */
static unsigned int failure_limit(void) {
  return parameters.max_fails == 0 ? 1 : parameters.max_fails;
}

/**
 * Longest period between checks of a dead host, unless sleep_ms is longer.
 * Bounds how late pg-status notices that the host has recovered.
 */
enum { MAX_DEAD_HOST_PERIOD_MS = 8000 };

/**
 * Period between the starts of checks of `host`. Every check of a dead host
 * is a new connection attempt. When the attempt gets as far as a TLS
 * handshake or password authentication before failing, it costs as much CPU
 * as a successful setup, so while a master is known the period doubles with
 * each such failure, starting with the one that makes the host dead. Without
 * a master, any dead host may be the one that brings it back, so each keeps
 * the normal period. Failures before the handshake cost little and keep the
 * normal period too, so recovery after a network outage is noticed promptly.
 */
static uint64_t poll_period_ms(const MonitorHost *host) {
  const unsigned int limit = failure_limit();
  uint64_t period_ms = (uint64_t)parameters.sleep_ms;
  if (host->failed_connections < limit || get_master_index() == -1) {
    return period_ms;
  }
  const unsigned int dead_failures = host->failed_connections - limit + 1;
  const unsigned int handshakes = host->handshake_failures;
  const unsigned int doublings = dead_failures < handshakes ? dead_failures
                                                            : handshakes;
  for (unsigned int i = 0; i < doublings && period_ms < MAX_DEAD_HOST_PERIOD_MS;
       i++) {
    period_ms = period_ms * 2 < MAX_DEAD_HOST_PERIOD_MS
                  ? period_ms * 2
                  : MAX_DEAD_HOST_PERIOD_MS;
  }
  return period_ms;
}

static void poll_state_idle(MonitorHost *host, const uint64_t now_ms) {
  host->poll_state = HOST_POLL_IDLE;
  host->poll_events = 0;
  host->iter_deadline_ms = 0;
  host->iter_data_ready = false;
  host->iter_retry_without_wal_receiver = false;
  const uint64_t normal_at_ms = max_ms(
    host->iter_started_at_ms + (uint64_t)parameters.sleep_ms, now_ms
  );
  host->next_poll_at_ms = max_ms(
    host->iter_started_at_ms + poll_period_ms(host), now_ms
  );
  host->backoff_ms = host->next_poll_at_ms - normal_at_ms;
}

void cancel_dead_host_backoff(void) {
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    // A host in a check gets its next time when the check ends.
    if (host->poll_state == HOST_POLL_IDLE) {
      host->next_poll_at_ms -= host->backoff_ms;
      host->backoff_ms = 0;
    }
  }
}

/**
 * Last observed master LSN. Used to compute the byte lag of replicas
 * whose iteration completes in a different order than the master's.
 * Lives in writer-only memory; with a single writer thread, no
 * synchronization is needed.
 */
static uint64_t master_lsn = 0;

/**
 * Earliest monotonic time at which the next new connection may start.
 * TLS and SCRAM make connection setup the costliest part of polling; under
 * a small CPU quota, setting up several connections at once can use up the
 * quota shared with the HTTP thread. Lives in writer-only memory.
 */
static uint64_t next_connect_at_ms = 0;

/** Pause between session setups, independent of the TCP start spacing. */
static uint64_t next_session_setup_at_ms = 0;

/**
 * Upper bound of the spacing between connection starts, so that a long
 * polling period does not delay startup and recovery more than needed.
 */
enum { MAX_CONNECT_INTERVAL_MS = 250 };

/**
 * Pause after the end of a setup that reached a TLS handshake or password
 * authentication, successful or not, before the next one may start. One
 * default CFS quota period leaves quota for the HTTP thread between setups,
 * even when a throttled setup outlasts the spacing of connection starts or
 * that spacing is short because of many hosts. A setup that reached
 * neither, for example one that timed out before the server answered, spent
 * no noticeable CPU and adds no pause.
 */
enum { CONNECT_SETUP_PAUSE_MS = 100 };

/**
 * Longest time one connection setup keeps others from starting, not
 * counting time the loop is blocked inside libpq. A throttled setup under a
 * 0.1 vCPU quota takes about half of it; a longer one most likely waits for
 * a server that hangs, which costs no CPU, and must not keep other hosts
 * from connecting until query_timeout_ms.
 */
enum { MAX_SESSION_SETUP_HOLD_MS = 1000 };

/**
 * Spaces connection starts so that when every host needs a new connection
 * and setup is fast, each still gets one per polling period.
 */
static uint64_t connect_interval_ms(void) {
  const uint64_t interval_ms = (uint64_t)parameters.sleep_ms / host_count;
  return interval_ms < MAX_CONNECT_INTERVAL_MS ? interval_ms
                                               : MAX_CONNECT_INTERVAL_MS;
}

/**
 * True while `host` negotiates TLS or authenticates. Waiting for the TCP
 * connection costs no CPU and does not count, so unreachable hosts do not
 * hold up connections to the others.
 */
static bool is_setting_up_session(const MonitorHost *host) {
  return host->poll_state == HOST_POLL_CONNECTING &&
         host->session_setup_hold_until_ms != 0;
}

/**
 * True when the connection attempt of the current check has done the
 * CPU-intensive part of a session setup: a TLS handshake or password
 * authentication. libpq keeps both facts after a failure past the
 * handshake, but drops its TLS state when the handshake itself fails or
 * when it reconnects to retry another encryption method, so TLS is also
 * remembered once seen in progress. The PQconnectionNeedsPassword check is
 * only a guard: pg-status always passes a non-empty password, and a request
 * that libpq could not answer would cost nothing.
 */
static bool did_handshake(const MonitorHost *host) {
  if (host->iter_tls_started) {
    return true;
  }
  if (host->conn == nullptr) {
    return false;
  }
  return PQsslInUse(host->conn) != 0 ||
         (PQconnectionUsedPassword(host->conn) != 0 &&
          PQconnectionNeedsPassword(host->conn) == 0);
}

/**
 * True when the next check of `host` must open a connection because the
 * current one is missing or broken. Replacing an aged connection is not a
 * need: it keeps serving checks until a turn is free.
 */
static bool needs_new_connection(const MonitorHost *host) {
  return host->conn == nullptr || PQstatus(host->conn) != CONNECTION_OK;
}

ConnectionTurn connection_turn(void) {
  uint64_t held_until_ms = 0;
  const MonitorHost *first_waiting = nullptr;
  MonitorHost *first_session_waiter = nullptr;
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    if (is_setting_up_session(host)) {
      // A setup that ends sooner frees the turn at once.
      held_until_ms = max_ms(held_until_ms, host->session_setup_hold_until_ms);
    }
    // Sockets that became ready at the same time go in list order.
    if (
      host->poll_state == HOST_POLL_SESSION_WAIT &&
      (first_session_waiter == nullptr ||
       host->session_setup_wait_started_at_ms <
         first_session_waiter->session_setup_wait_started_at_ms)
    ) {
      first_session_waiter = host;
    }
    if (host->poll_state != HOST_POLL_IDLE || !needs_new_connection(host)) {
      continue;
    }
    // Hosts due at the same time go in list order. Without an order, a host
    // whose server hangs during setup could take every turn as soon as it
    // frees up.
    if (
      first_waiting == nullptr ||
      host->next_poll_at_ms < first_waiting->next_poll_at_ms
    ) {
      first_waiting = host;
    }
  }
  const uint64_t setup_due_at_ms = max_ms(
    held_until_ms, next_session_setup_at_ms
  );
  return (ConnectionTurn){
    .session_setup_due_at_ms = setup_due_at_ms,
    // A ready socket goes before connections that have not started yet:
    // one that started now could finish TCP first and take its turn.
    .connect_at_ms = first_session_waiter != nullptr
                       ? UINT64_MAX
                       : max_ms(setup_due_at_ms, next_connect_at_ms),
    .first_waiting = first_waiting,
    .first_session_waiter = first_session_waiter,
  };
}

/** Earliest time at which a new connection may start. */
static uint64_t connection_turn_at_ms(void) {
  return connection_turn().connect_at_ms;
}

uint64_t host_poll_due_at_ms(
  const MonitorHost *host, const ConnectionTurn *turn
) {
  if (!needs_new_connection(host)) {
    return host->next_poll_at_ms;
  }
  if (turn->first_waiting != host) {
    // Another host goes first. Its start wakes the loop, and only then is
    // this host's due time known.
    return UINT64_MAX;
  }
  return max_ms(turn->connect_at_ms, host->next_poll_at_ms);
}

void postpone_connection_turn(const uint64_t stall_ms) {
  // A turn already free stays free: it moves no later than the current time.
  next_connect_at_ms += stall_ms;
  next_session_setup_at_ms += stall_ms;
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    if (is_setting_up_session(host)) {
      host->session_setup_hold_until_ms += stall_ms;
    }
  }
}

/**
 * Takes the connection turn for `host`, which starts connecting at `now_ms`.
 * The next turn counts from when this one was due rather than from now, so
 * late wake-ups of the loop do not add up across hosts. The due time is
 * computed here rather than taken from host_poll_due_at_ms(), which ignores
 * the turn for a host whose working connection is being replaced.
 */
static void take_connection_turn(
  const MonitorHost *host, const uint64_t now_ms
) {
  const uint64_t interval_ms = connect_interval_ms();
  const uint64_t due_at_ms = max_ms(
    connection_turn_at_ms(), host->next_poll_at_ms
  );
  const bool on_time = due_at_ms <= now_ms && now_ms - due_at_ms < interval_ms;
  next_connect_at_ms = (on_time ? due_at_ms : now_ms) + interval_ms;
}

/**
 * Called when a host leaves the CONNECTING phase or returns to waiting for
 * TCP. After a TLS handshake or password authentication, both new
 * connections and already-ready TCP sockets wait for the pause.
 */
static void end_session_setup(MonitorHost *host, const uint64_t now_ms) {
  if (is_setting_up_session(host) && did_handshake(host)) {
    next_session_setup_at_ms = max_ms(
      next_session_setup_at_ms, now_ms + CONNECT_SETUP_PAUSE_MS
    );
  }
  host->session_setup_hold_until_ms = 0;
}

/**
 * Parks a host whose TCP connection is ready while another host owns the
 * session setup turn. The socket stays out of poll() until the turn is free
 * to avoid a writable spin; the wait is excluded from the query timeout when
 * the host resumes.
 */
static void wait_for_session_setup_turn(
  MonitorHost *host, const uint64_t now_ms
) {
  host->poll_state = HOST_POLL_SESSION_WAIT;
  host->poll_events = 0;
  host->session_setup_wait_started_at_ms = now_ms;
}

/**
 * Closes the underlying PGconn if open and resets its bookkeeping.
 */
static void close_conn(MonitorHost *host) {
  if (host->conn) {
    PQfinish(host->conn);
    host->conn = nullptr;
  }
  host->connected_at_ms = 0;
  host->session_setup_hold_until_ms = 0;
  host->session_setup_wait_started_at_ms = 0;
  host->wal_receiver_disabled = false;
  host->iter_retry_without_wal_receiver = false;
}

/**
 * Closes a broken PGconn, or an aged one when it can be replaced now: the
 * connection turn is free and no host with a broken or missing connection
 * is due at the same time or earlier. Otherwise the aged connection serves
 * this check, and the replacement is tried again at the next check; a check
 * is never delayed because of the age of a working connection. Planned
 * replacements of different hosts never coincide, as each takes a turn.
 */
static void close_conn_if_need(MonitorHost *host, const uint64_t now_ms) {
  if (host->conn == nullptr) {
    return;
  }
  if (needs_new_connection(host)) {
    // The check was started only once a connection turn was free.
    close_conn(host);
    return;
  }
  const bool too_old = (now_ms - host->connected_at_ms) >
                       parameters.conn_max_age_ms;
  if (!too_old) {
    return;
  }
  const ConnectionTurn turn = connection_turn();
  const bool another_goes_first = turn.first_waiting != nullptr &&
                                  turn.first_waiting->next_poll_at_ms <=
                                    host->next_poll_at_ms;
  if (now_ms >= turn.connect_at_ms && !another_goes_first) {
    close_conn(host);
  }
}

static bool validate_sql_result(
  MonitorHost *host, const PGresult *res, const uint64_t now_ms
) {
  const ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
    log_postgres_error(host, "validate_result", now_ms);
    return false;
  }
  if (PQntuples(res) != 1) {
    pg_status_log(
      poll_error_level(host, now_ms), "monitor",
      "unexpected PostgreSQL result row count host=%s rows=%d", host->host,
      PQntuples(res)
    );
    return false;
  }
  return true;
}

static void parse_replica_result(MonitorHost *host, const PGresult *res) {
  const uint64_t replica_received_lsn = parse_lsn(PQgetvalue(res, 0, 2));
  const uint64_t replica_lsn = parse_lsn(PQgetvalue(res, 0, 3));
  const uint64_t receiver_latest_end_lsn = parse_lsn(PQgetvalue(res, 0, 5));
  const uint64_t known_end_lsn = max_lsn(
    master_lsn, max_lsn(replica_received_lsn, receiver_latest_end_lsn)
  );
  host->iter_new_lag_ms = str_to_ull(PQgetvalue(res, 0, 4));
  host->iter_new_lag_bytes = known_end_lsn > replica_lsn
                               ? known_end_lsn - replica_lsn
                               : 0;
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
static bool parse_result(
  MonitorHost *host, const PGresult *res, const uint64_t now_ms
) {
  const bool result_ok = validate_sql_result(host, res, now_ms);
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
 * preserves the last successful measurements, and closes the connection
 * so the next iteration will reconnect.
 * Always returns the host to IDLE and schedules the next iteration
 * relative to the iteration's start, without overlapping checks or catch-up.
 */
static void finish_iteration(
  MonitorHost *host, const bool success, const uint64_t now_ms
) {
  const MonitorSnapshot old_snapshot = atomic_get_snapshot(host);
  const MonitorStatus old_status = old_snapshot.status;
  const uint64_t old_lag_ms = old_snapshot.lag_ms;
  const uint64_t old_lag_bytes = old_snapshot.lag_bytes;

  MonitorStatus new_status;
  uint64_t new_lag_ms;
  uint64_t new_lag_bytes;
  uint64_t new_lsn;
  bool reached_dead_threshold = false;

  if (success && host->iter_data_ready) {
    host->failed_connections = 0;
    host->handshake_failures = 0;
    host->next_error_log_at_ms = 0;
    new_status = host->iter_new_status;
    new_lag_ms = host->iter_new_lag_ms;
    new_lag_bytes = host->iter_new_lag_bytes;
    new_lsn = host->iter_new_lsn;
  } else {
    host->failed_connections++;
    // Only a connection opened by this check can have cost a handshake; a
    // query failing on a reused connection costs nothing to notice.
    if (host->iter_opened_connection && did_handshake(host)) {
      if (host->handshake_failures < UINT16_MAX) {
        host->handshake_failures++;
      }
    } else {
      host->handshake_failures = 0;
    }
    new_status = old_status;
    new_status.possible_dead = true;
    const unsigned int limit = failure_limit();
    if (host->failed_connections >= limit) {
      new_status = dead_status();
      reached_dead_threshold = host->failed_connections == limit;
    }
    new_lag_ms = old_lag_ms;
    new_lag_bytes = old_lag_bytes;
    new_lsn = old_snapshot.lsn;
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
    old_lag_bytes, reached_dead_threshold
  );

  poll_state_idle(host, now_ms);
}

static bool consume_input(MonitorHost *host, const uint64_t now_ms) {
  if (PQconsumeInput(host->conn) == 0) {
    log_postgres_error(host, "consume_input", now_ms);
    return false;
  }
  return true;
}

static bool retry_without_wal_receiver(
  MonitorHost *host, const PGresult *res, const uint64_t now_ms
) {
  if (
    host->wal_receiver_disabled || PQresultStatus(res) != PGRES_FATAL_ERROR ||
    !is_equal_strings(PQresultErrorField(res, PG_DIAG_SQLSTATE), "42501")
  ) {
    return false;
  }

  // SQLSTATE identifies insufficient_privilege without depending on the
  // server's message language. If another required object is denied, the
  // query without the receiver will still fail and count as a failed poll.
  host->wal_receiver_disabled = true;
  host->wal_receiver_retry_at_ms = now_ms + WAL_RECEIVER_RETRY_MS;
  host->iter_retry_without_wal_receiver = true;
  pg_status_log(
    PG_STATUS_LOG_WARNING, "monitor",
    "PostgreSQL permission denied host=%s sqlstate=42501; "
    "retrying poll without WAL receiver statistics for 5 minutes or until "
    "reconnect; "
    "for more complete byte lag measurements, check SELECT on "
    "pg_catalog.pg_stat_wal_receiver and EXECUTE on "
    "pg_catalog.pg_stat_get_wal_receiver(); pg_read_all_stats privileges "
    "are also needed to expose latest_end_lsn",
    host->host
  );
  return true;
}

/**
 * Drives the QUERY_READ phase: consume any pending input, drain ready
 * results, finish if the end-of-results NULL is observed, otherwise
 * suspend waiting on POLLIN.
 */
static void read_step(MonitorHost *host, const uint64_t now_ms) {
  const bool consumed = consume_input(host, now_ms);
  if (!consumed) {
    finish_iteration(host, false, now_ms);
    return;
  }

  while (PQisBusy(host->conn) == 0) {
    PGresult *res = PQgetResult(host->conn);
    if (res == nullptr) {
      if (host->iter_retry_without_wal_receiver) {
        // libpq requires draining the previous query through NULL before
        // sending another one. Keep the original iteration deadline.
        host->iter_retry_without_wal_receiver = false;
        if (!send_query(host, now_ms)) {
          finish_iteration(host, false, now_ms);
          return;
        }
        poll_state_query_send(host);
        return;
      }
      finish_iteration(host, host->iter_data_ready, now_ms);
      return;
    }
    if (
      !host->iter_data_ready && !host->iter_retry_without_wal_receiver &&
      !retry_without_wal_receiver(host, res, now_ms)
    ) {
      host->iter_data_ready = parse_result(host, res, now_ms);
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
      log_postgres_error(host, "flush", now_ms);
      finish_iteration(host, false, now_ms);
      return;
  }
}

static bool pq_set_non_blocking(MonitorHost *host, const uint64_t now_ms) {
  if (PQsetnonblocking(host->conn, 1) != 0) {
    log_postgres_error(host, "set_non_blocking", now_ms);
    return false;
  }
  return true;
}

/**
 * Drives the CONNECTING phase via PQconnectPoll. On success, sets up
 * the connection for non-blocking sends and kicks off the query.
 */
static void advance_connecting(MonitorHost *host, const uint64_t now_ms) {
  // While waiting for TCP, this step only reads the connect result, which
  // costs no CPU and reports a refused connection at once. A host past that
  // point holds the session setup turn.
  const PostgresPollingStatusType st = PQconnectPoll(host->conn);
  if (PQsslInUse(host->conn) != 0) {
    host->iter_tls_started = true;
  }
  if (PQstatus(host->conn) == CONNECTION_STARTED) {
    // Still, or again, waiting for TCP: libpq dropped the connection and is
    // connecting again, to another address or with another encryption
    // method. No session setup is in progress.
    end_session_setup(host, now_ms);
  } else if (!is_setting_up_session(host) && st != PGRES_POLLING_FAILED) {
    const ConnectionTurn turn = connection_turn();
    if (
      now_ms < turn.session_setup_due_at_ms ||
      turn.first_session_waiter != nullptr
    ) {
      // TCP has just connected; the startup packet waits for the turn,
      // behind sockets that became ready earlier.
      wait_for_session_setup_turn(host, now_ms);
      return;
    }
    host->session_setup_hold_until_ms = now_ms + MAX_SESSION_SETUP_HOLD_MS;
  }
  switch (st) {
    case PGRES_POLLING_READING:
      host->poll_events = POLLIN;
      return;
    case PGRES_POLLING_WRITING:
      host->poll_events = POLLOUT;
      return;
    case PGRES_POLLING_OK:
      end_session_setup(host, now_ms);
      host->connected_at_ms = now_ms;

      const bool set = pq_set_non_blocking(host, now_ms);
      if (!set) {
        finish_iteration(host, false, now_ms);
        return;
      }

      const bool query_sent = send_query(host, now_ms);
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
      end_session_setup(host, now_ms);
      log_postgres_error(host, "connect", now_ms);
      finish_iteration(host, false, now_ms);
      return;
  }
}

/**
 * Lets a host waiting in SESSION_WAIT begin TLS or authentication once the
 * turn is free and no socket has been waiting longer. The host takes the
 * turn before its libpq step; otherwise the sockets still waiting would
 * send it back to the queue.
 */
static void resume_session_setup(MonitorHost *host, const uint64_t now_ms) {
  const ConnectionTurn turn = connection_turn();
  if (
    now_ms < turn.session_setup_due_at_ms || turn.first_session_waiter != host
  ) {
    return;
  }
  // This delay belongs to our scheduler, not to PostgreSQL's response.
  host->iter_deadline_ms += now_ms - host->session_setup_wait_started_at_ms;
  host->session_setup_wait_started_at_ms = 0;
  poll_state_connecting(host);
  host->session_setup_hold_until_ms = now_ms + MAX_SESSION_SETUP_HOLD_MS;
  advance_connecting(host, now_ms);
}

static bool start_connect(MonitorHost *host, const uint64_t now_ms) {
  host->conn = pg_connection_start(host);
  if (host->conn == nullptr || PQstatus(host->conn) == CONNECTION_BAD) {
    // A start that fails at once costs no handshake and takes no turn.
    log_postgres_error(host, "connect_start", now_ms);
    return false;
  }
  take_connection_turn(host, now_ms);
  if (PQstatus(host->conn) != CONNECTION_STARTED) {
    // libpq normally returns while TCP is still connecting. Should it
    // already be past that, session setup has begun and holds the turn.
    host->session_setup_hold_until_ms = now_ms + MAX_SESSION_SETUP_HOLD_MS;
  }
  return true;
}

static void reset_iter_state(MonitorHost *host, const uint64_t now_ms) {
  host->iter_started_at_ms = now_ms;
  host->iter_opened_connection = false;
  host->iter_tls_started = false;
  host->iter_data_ready = false;
  host->iter_retry_without_wal_receiver = false;
  host->iter_new_status = (MonitorStatus){
    .alive = false, .master = false, .possible_dead = true
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
    host->iter_opened_connection = true;
    const bool started = start_connect(host, now_ms);
    if (started) {
      poll_state_connecting(host);
    } else {
      finish_iteration(host, false, now_ms);
    }
    return;
  }

  const bool query_sent = send_query(host, now_ms);
  if (!query_sent) {
    finish_iteration(host, false, now_ms);
    return;
  }
  poll_state_query_send(host);
  flush_step(host, now_ms);
}

void advance_host_poll(MonitorHost *host, const uint64_t now_ms) {
  switch (host->poll_state) {
    case HOST_POLL_SESSION_WAIT:
      resume_session_setup(host, now_ms);
      return;
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
  pg_status_log(
    poll_error_level(host, now_ms), "monitor",
    "PostgreSQL poll timed out host=%s timeout_ms=%llu", host->host,
    (unsigned long long)parameters.query_timeout_ms
  );
  end_session_setup(host, now_ms);
  // The failed iteration closes the connection after checking whether the
  // attempt cost a handshake.
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
