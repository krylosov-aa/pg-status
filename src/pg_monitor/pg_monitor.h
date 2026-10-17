#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

#include <stdint.h>

// Parameters

/**
 * The maximum number of hosts monitored by pg-status
 */
enum { MAX_HOSTS = 64 };

/**
 * List of all monitoring parameters
 */
typedef struct {
  const char *user;

  const char *password;

  const char *database;

  // pg hosts, comma-separated.
  const char *hosts;

  // DC of each pg host, comma-separated and positional to `hosts`.
  const char *hosts_dc;

  // Geo of each pg host, comma-separated and positional to `hosts`.
  const char *hosts_geo;

  // Effective DC of this pg-status instance.
  const char *current_dc;

  // Name of an environment variable containing the current DC.
  const char *current_dc_env;

  // Whether any DC locality environment parameter was present.
  bool dc_locality_configured;

  // Whether complete DC locality data was validated during startup.
  bool dc_locality_enabled;

  // Effective geo of this pg-status instance.
  const char *current_geo;

  // Name of an environment variable containing the current geo.
  const char *current_geo_env;

  // Whether any geo locality environment parameter was present.
  bool geo_locality_configured;

  // Whether complete geo locality data was validated during startup.
  bool geo_locality_enabled;

  // pg port. You can specify multiple ports, comma-separated.
  const char *port;

  // Maximum lag in ms for a time-synchronous replica (inclusive).
  uint64_t sync_max_lag_ms;

  // Maximum lag in bytes for a byte-synchronous replica (inclusive).
  uint64_t sync_max_lag_bytes;

  // Target period between starts of checks of the same host, in ms (> 0).
  // While a master is known, lengthened up to 8 s for dead hosts whose
  // connection attempts fail after a TLS handshake or password
  // authentication.
  int sleep_ms;

  // After this number of failed checks in a row, the host is considered dead.
  unsigned int max_fails;

  // Maximum age of a reused PGconn in ms. An older connection is replaced
  // by the first check that finds the connection turn free and no host with
  // a missing or broken connection due before it; until then it keeps
  // serving checks.
  uint64_t conn_max_age_ms;

  // Hard deadline for a single poll iteration (connect + send + read) in ms.
  // On expiry the connection is closed and failed_connections is incremented.
  // Time the loop is blocked inside libpq (host name lookups) is excluded.
  // A ready TCP socket's wait for a session setup turn is also excluded.
  uint64_t query_timeout_ms;

  // Minimum interval in ms between ERROR records about continuing poll
  // failures of the same host; failures in between are logged at DEBUG.
  // 0 logs every failure at ERROR.
  uint64_t poll_error_log_interval_ms;
} MonitorParameters;

/**
 * pg-monitor parameters. Default values are defined in parameters.c.
 */
extern MonitorParameters parameters;

/**
 * Overrides default parameters if they are set in environment variables.
 */
void set_parameters_from_env(void);

// Start/Stop monitoring

/**
 * Starts a host monitoring thread without waiting for its initial polls.
 */
void start_pg_monitor(void);

/**
 * True after every host has completed its first poll (success or failure).
 * An acquire load publishes initialized parameters and hosts to API readers.
 */
bool is_pg_monitor_ready(void);

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void);

// Host list

/**
 * The actual number of hosts
 */
extern unsigned int host_count;

/**
 * Atomically gets the current master position in the array
 */
int get_master_index(void);

/**
 * Host status flags. Small enough to be loaded atomically as a whole.
 */
typedef struct {
  bool master : 1;
  bool alive : 1;
  bool possible_dead : 1;
} MonitorStatus;

/**
 * Forward declaration of libpq opaque connection type.
 */
struct pg_conn;

/**
 * Phase of the per-host async poll state machine.
 */
typedef enum {
  HOST_POLL_IDLE = 0,      // nothing in flight
  HOST_POLL_CONNECTING,    // PQconnectStart issued
  HOST_POLL_SESSION_WAIT,  // TCP socket ready; waiting for a TLS/auth turn
  HOST_POLL_QUERY_SEND,    // PQsendQuery issued
  HOST_POLL_QUERY_READ,    // reading the result
} HostPollState;

/**
 * Host configuration and observed state.
 *
 * The polling thread is the sole writer. Readers use atomic_get_snapshot()
 * to read {status, lag_ms, lag_bytes, lsn} from the same poll, both for host
 * selection and JSON responses. Single-field accessors do not provide this
 * consistency across fields.
 *
 * lsn is pg_last_wal_replay_lsn() on a replica and pg_current_wal_lsn() on a
 * master, as of the last successful poll. Failed polls preserve the lags
 * and LSN; the HTTP API hides these measurements once the host is dead.
 *
 * The writer sets seq to odd before updating the snapshot and to even after
 * publication. Readers retry if seq is odd or changes during their read.
 */
typedef struct {
  const char *host;                 // immutable after init
  const char *port;                 // immutable after init
  const char *dc;                   // immutable after init, or NULL
  const char *geo;                  // immutable after init, or NULL
  _Atomic uint64_t seq;             // seqlock: odd = writing, even = stable
  _Atomic uint64_t lag_ms;          // protected by seq
  _Atomic uint64_t lag_bytes;       // protected by seq
  _Atomic uint64_t lsn;             // protected by seq
  unsigned int failed_connections;  // writer-private
  _Atomic MonitorStatus status;     // protected by seq
  // writer-private: failed checks in a row whose connection attempt did a
  // TLS handshake or password authentication, saturating
  uint16_t handshake_failures;

  // writer-private async-poll state
  // monotonic time at which the permission fallback below expires
  uint64_t wal_receiver_retry_at_ms;
  struct pg_conn *conn;         // reused PGconn, or NULL when disconnected
  HostPollState poll_state;     // current phase of the state machine
  short poll_events;            // events to wait for on PQsocket(conn)
  bool iter_opened_connection;  // the current check opened a new connection
  bool iter_tls_started;        // TLS was seen in progress during this check
  uint64_t next_poll_at_ms;     // monotonic deadline to start next iteration
  uint64_t backoff_ms;          // part of next_poll_at_ms added for a dead host
  uint64_t iter_started_at_ms;  // monotonic start of current/last iteration
  uint64_t iter_deadline_ms;    // monotonic deadline for current iteration
  uint64_t session_setup_hold_until_ms;       // 0 until TLS/auth starts
  uint64_t session_setup_wait_started_at_ms;  // SESSION_WAIT queue order
  uint64_t connected_at_ms;       // monotonic time of last successful connect
  uint64_t next_error_log_at_ms;  // per-host error limit; reset on poll success
  int pollfd_slot;                // transient index into the main-loop pollfd[]
  bool wal_receiver_disabled;     // permission fallback, reset on disconnect
  bool iter_retry_without_wal_receiver;  // drain results before retrying

  // Staging area for the current iteration. Populated once the result row
  // is parsed; published to {status, lag_ms, lag_bytes, lsn} on success.
  bool iter_data_ready;
  MonitorStatus iter_new_status;
  uint64_t iter_new_lag_ms;
  uint64_t iter_new_lag_bytes;
  uint64_t iter_new_lsn;
} MonitorHost;

/**
 * Consistent snapshot of a host's state
 */
typedef struct {
  MonitorStatus status;
  uint64_t lag_ms;
  uint64_t lag_bytes;
  uint64_t lsn;
} MonitorSnapshot;

/**
 * Array of monitoring hosts
 */
extern MonitorHost monitor_host_list[MAX_HOSTS];

/**
 * Initializes the MonitorHost array to its initial values.
 */
void init_monitor_host_list(void);

/**
 * Atomically saves the current master index in the host array.
 */
void save_master_index(int i);

// Lookup utils

/**
 * Returns the current master object, or NULL when no master is known.
 */
const MonitorHost *get_master_monitor_host(void);

/**
 * Atomically returns MonitorStatus
 */
MonitorStatus atomic_get_status(const MonitorHost *host);

/**
 * Atomically returns the host's replication lag in milliseconds
 */
uint64_t atomic_get_lag_ms(const MonitorHost *host);

/**
 * Atomically returns the host's replication lag in bytes
 */
uint64_t atomic_get_lag_bytes(const MonitorHost *host);

/**
 * Returns a consistent snapshot of {status, lag_ms, lag_bytes, lsn} via the
 * seqlock on host->seq. May spin briefly if the writer is mid-update,
 * but never blocks the writer.
 */
MonitorSnapshot atomic_get_snapshot(const MonitorHost *host);

/**
 * Atomically publishes a consistent snapshot for a host.
 * There must be only one writer for a given host.
 */
void publish_monitor_snapshot(MonitorHost *host, MonitorSnapshot snapshot);

/**
 * Tests a host against a consistent snapshot and caller-supplied context.
 */
typedef bool (*condition_handler)(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * Searches for a replica host that matches the given condition. Within each
 * health class, prefers the current DC, then the current geo, then uses the
 * round-robin order within the best available group. The shared cursor
 * advances to the selected replica; concurrent selections retry on conflict.
 * Prefers a fully alive match; falls back to a
 * `possible_dead` match if no alive replica satisfies the handler. If no
 * replica matches at all, returns the current master as a fallback, or nullptr
 * if there is no master either.
 * @param handler A function that determines whether the specified host
 * matches
 * @param ctx Opaque context forwarded to the handler
 * @param log_context The context that will be visible in the logs
 * @return Host object matching the condition, or the master as a fallback, or
 * nullptr if no host is available
 */
const MonitorHost *find_replica(
  condition_handler handler, const void *ctx, const char *log_context
);

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host);

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * Per-request lag thresholds. Passed as ctx to is_sync_replica_by_*
 * handlers so each request can override the global sync thresholds.
 *
 * `min_lsn` is an optional strict freshness filter: when non-zero, a
 * candidate replica must have replayed at least up to this LSN to match.
 * A value of 0 means no LSN constraint (the field is unused). This is
 * the read-your-writes primitive — clients pass the master's LSN from
 * just after their write.
 */
typedef struct {
  uint64_t max_lag_ms;
  uint64_t max_lag_bytes;
  uint64_t min_lsn;
} LagThresholds;

/**
 * Searches for the most byte-synchronous replica whose lag still satisfies
 * the byte threshold. Ties are broken by host order. Prefers a
 * fully alive match; falls back to a `possible_dead` match only if no
 * alive replica satisfies the thresholds. If no replica matches, returns
 * the current master, or nullptr if there is no master.
 * @param thresholds Lag thresholds the replica must satisfy
 * @param log_context The context that will be visible in the logs
 * @return Host object of the most byte-synchronous replica, or the master
 * as a fallback, or nullptr if no host is available
 */
const MonitorHost *find_most_sync_replica_by_bytes(
  const LagThresholds *thresholds, const char *log_context
);

// Host checking utils

/**
 * Snapshot of the connection turn. New connections take turns: one starts
 * only when no other has been negotiating TLS or authenticating for less
 * than 1 s of unblocked loop time, 100 ms after the end of the previous
 * setup that reached a TLS handshake or password authentication, and
 * min(sleep_ms / host_count, 250 ms) after the previous one started. Hosts
 * whose connection is missing or broken take turns in the order their
 * checks became due. Ready TCP sockets begin TLS or authentication in the
 * order they became ready, before any new connection starts.
 */
typedef struct {
  // earliest time a ready TCP connection may begin TLS or authentication
  uint64_t session_setup_due_at_ms;
  // earliest time a new connection may start; UINT64_MAX while a ready TCP
  // socket waits for its turn
  uint64_t connect_at_ms;
  // IDLE host whose connection is missing or broken and whose check became
  // due first, or NULL
  const MonitorHost *first_waiting;
  // SESSION_WAIT host whose TCP connection became ready first, or NULL
  MonitorHost *first_session_waiter;
} ConnectionTurn;

/**
 * Takes the snapshot in one pass over the hosts. It is valid until a host
 * starts a check, resumes a queued session setup, times out, or the loop
 * is blocked inside libpq; the polling loop takes it once per pass and
 * again after each of those, so scheduling every host costs constant time.
 */
ConnectionTurn connection_turn(void);

/**
 * Returns the monotonic time at which the next check of an IDLE host is due
 * given the current `turn`. A host with a working connection is due at its
 * scheduled time, even if the connection is aged. A host whose connection
 * is missing or broken is due once its scheduled time has come and the
 * turn is free; while another host goes first, the due time is unknown and
 * UINT64_MAX is returned.
 */
uint64_t host_poll_due_at_ms(
  const MonitorHost *host, const ConnectionTurn *turn
);

/**
 * Moves the next connection turn later by `stall_ms`, the time the loop was
 * blocked inside libpq, so that the spacing between connection starts counts
 * from when the blocking call returned. Also shifts session setup holds
 * and the pause after setup. A free turn stays free.
 */
void postpone_connection_turn(uint64_t stall_ms);

/**
 * Removes the longer period of dead hosts from their next checks, so that
 * each is checked once per sleep_ms again. Called while no master is known:
 * any dead host may be the one that brings it back.
 */
void cancel_dead_host_backoff(void);

/**
 * Kicks off a new poll iteration for `host`: opens the connection if needed
 * (or reuses the existing one, recycling it if older than conn_max_age_ms
 * and a connection turn is free), fires PQsendQuery, and sets the
 * iteration deadline. After the call the host is in CONNECTING or QUERY_SEND
 * state and ready to be added to a poll() fd set.
 */
void start_host_poll(MonitorHost *host, uint64_t now_ms);

/**
 * Drives the per-host state machine in response to `revents` from
 * poll(), or when a SESSION_WAIT host's turn becomes due. On completion
 * (success or any error), the host returns to IDLE and
 * its shared state (`status`, `lag_ms`, `lag_bytes`, `lsn`) is updated via the
 * seqlock. The next check is due at the previous start plus `sleep_ms`
 * (while a master is known, doubled per failure after a TLS handshake or
 * password authentication for a dead host, up to 8 s), or immediately if
 * that time has already passed.
 * Checks never overlap.
 */
void advance_host_poll(MonitorHost *host, uint64_t now_ms);

/**
 * Aborts the current iteration after iter_deadline_ms has passed. Closes
 * the connection, bumps failed_connections, and returns the host to IDLE.
 */
void timeout_host_poll(MonitorHost *host, uint64_t now_ms);

/**
 * Returns the file descriptor of the underlying libpq connection, or -1
 * if the host has no open connection. Used by the main loop to build
 * the pollfd[] array without including libpq-fe.h.
 */
int host_socket(const MonitorHost *host);

/**
 * Closes every open PGconn in monitor_host_list and resets each host to
 * IDLE. Called from the writer thread before it exits.
 */
void close_all_host_connections(void);

#endif  // PG_STATUS_PG_MONITOR_H
