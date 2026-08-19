/**
 * Monitoring of postgresql hosts
 */

#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

#include <stdint.h>

// ------------------------ Parameters ------------------------

/**
 * The maximum number of hosts monitored by pg-status
 */
constexpr unsigned int MAX_HOSTS = 10;

/**
 * List of all monitoring parameters
 */
typedef struct {
  // pg user
  char *user;

  // pg password
  char *password;

  // pg database name
  char *database;

  // pg hosts, comma-separated.
  char *hosts;

  // pg port. You can specify multiple ports, comma-separated.
  char *port;

  // Time to attempt connection to host
  char *connect_timeout;

  // The lag in ms below which a replica is considered synchronous
  uint64_t sync_max_lag_ms;

  // The lag in bytes below which a replica is considered synchronous
  uint64_t sync_max_lag_bytes;

  // Time between checks in ms
  int sleep_ms;

  // After this number of failed checks in a row, the host is considered dead.
  unsigned int max_fails;

  // Maximum age of a reused PGconn in ms. When the current connection is
  // older than this, it is closed at the end of the current iteration and
  // the next iteration reconnects.
  uint64_t conn_max_age_ms;

  // Hard deadline for a single poll iteration (connect + send + read) in ms.
  // On expiry the connection is closed and failed_connections is incremented.
  uint64_t query_timeout_ms;
} MonitorParameters;

/**
 * pg-monitor parameters. Default values are defined in parameters.c.
 */
extern MonitorParameters parameters;

/**
 * Overrides default parameters if they are set in environment variables.
 */
void set_parameters_from_env(void);

// ------------------------ Start/Stop monitoring ------------------------

/**
 * Starts a host monitoring thread
 */
void start_pg_monitor(void);

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void);

// ------------------------ Host list ------------------------

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
  HOST_POLL_IDLE = 0,    // nothing in flight
  HOST_POLL_CONNECTING,  // PQconnectStart issued
  HOST_POLL_QUERY_SEND,  // PQsendQuery issued
  HOST_POLL_QUERY_READ,  // reading the result
} HostPollState;

/**
 *  Host parameters.
 *
 *  Concurrency model: one writer (the poll thread) and many readers
 *  (HTTP handlers). The fields {status, lag_ms, lag_bytes, lsn}
 *  together describe a host and must be read as a consistent snapshot —
 *  otherwise routing endpoints could see, e.g., the old (alive) status
 *  with the new (zeroed) lags and return a dead host as a "sync replica".
 *
 *  `lsn` is the latest WAL position known to this host as of the
 *  last successful poll: `pg_last_wal_replay_lsn()` on a replica,
 *  `pg_current_wal_lsn()` on a master.
 *
 *  A seqlock protects the snapshot:
 *  - Writer increments `seq` to odd, writes the four fields, then
 *    increments `seq` to even. Each step is lock-free; the writer
 *    never waits.
 *  - Readers must call atomic_get_snapshot() — never read status / lag_ms
 *    / lag_bytes / lsn directly during routing decisions. Direct
 *    atomic_get_* accessors remain available for places that only need
 *    one field (e.g. /hosts and /status JSON rendering).
 */
typedef struct {
  char *host;                       // immutable after init
  char *connection_str;             // immutable after init
  _Atomic uint64_t seq;             // seqlock: odd = writing, even = stable
  _Atomic uint64_t lag_ms;          // protected by seq
  _Atomic uint64_t lag_bytes;       // protected by seq
  _Atomic uint64_t lsn;             // protected by seq
  unsigned int failed_connections;  // writer-private
  _Atomic MonitorStatus status;     // protected by seq

  // ---- writer-private async-poll state ----
  struct pg_conn *conn;       // reused PGconn, or NULL when disconnected
  HostPollState poll_state;   // current phase of the state machine
  short poll_events;          // events to wait for on PQsocket(conn)
  uint64_t next_poll_at_ms;   // monotonic deadline to start next iteration
  uint64_t iter_deadline_ms;  // monotonic deadline for current iteration
  uint64_t connected_at_ms;   // monotonic time of last successful connect
  int pollfd_slot;            // transient index into the main-loop pollfd[]

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

// ------------------------ Lookup utils ------------------------

/**
 * Atomic acquisition of the current master
 */
const char *get_master_host(void);

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
 * Returns a consistent snapshot of {status, lag_ms, lag_bytes} via the
 * seqlock on host->seq. May spin briefly if the writer is mid-update,
 * but never blocks the writer.
 */
MonitorSnapshot atomic_get_snapshot(const MonitorHost *host);

/**
 * Describes the interface of the function for searching hosts.
 * Receives a consistent MonitorSnapshot (status + lags from the same
 * poll), the host itself (for host->host name access), and an opaque
 * caller-supplied context pointer.
 */
typedef bool (*condition_handler)(
  MonitorSnapshot snap, const MonitorHost *host, const void *ctx
);

/**
 * Searches for a replica host that matches the given condition using the
 * round-robin algorithm. Prefers a fully alive match; falls back to a
 * `possible_dead` match if no alive replica satisfies the handler. If no
 * replica matches at all, returns the current master as a fallback,
 * or nullptr if there is no master either.
 * @param handler A function that determines whether the specified host
 * matches
 * @param ctx Opaque context forwarded to the handler
 * @param log_context The context that will be visible in the logs
 * @return Host name matching the condition, or the master as a fallback, or
 * nullptr if no host is available
 */
const char *find_replica_round_robin(
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
 * @return Host name of the most byte-synchronous replica, or the master
 * as a fallback, or nullptr if no host is available
 */
const char *find_most_sync_replica_by_bytes(
  const LagThresholds *thresholds, const char *log_context
);

// ------------------------ Host checking utils ------------------------

/**
 * Kicks off a new poll iteration for `host`: opens the connection if needed
 * (or reuses the existing one, recycling it if older than conn_max_age_ms),
 * fires PQsendQuery, and sets the iteration deadline. After the call the
 * host is in CONNECTING or QUERY_SEND state and ready to be added to a
 * poll() fd set.
 */
void start_host_poll(MonitorHost *host, uint64_t now_ms);

/**
 * Drives the per-host state machine in response to `revents` from
 * poll(). On completion (success or any error), the host returns to IDLE and
 * its shared state (`status`, `lag_ms`, `lag_bytes`, `lsn`) is updated via the
 * seqlock; `next_poll_at_ms` is scheduled `sleep_ms` into the future.
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
 * IDLE. Called from the writer thread before it exits, so that valgrind
 * sees a clean shutdown.
 */
void close_all_host_connections(void);

#endif  // PG_STATUS_PG_MONITOR_H
