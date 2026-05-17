/**
 * Monitoring of postgresql hosts
 */

#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

#include <pthread.h>
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
pthread_t start_pg_monitor();

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
 *  Host parameters.
 *
 *  Concurrency model: one writer (the poll thread) and many readers
 *  (HTTP handlers). The fields {status, lag_ms, lag_bytes} together
 *  describe a host and must be read as a consistent snapshot — otherwise
 *  routing endpoints could see, e.g., the old (alive) status with the
 *  new (zeroed) lags and return a dead host as a "sync replica".
 *
 *  A seqlock protects the snapshot:
 *  - Writer increments `seq` to odd, writes the three fields, then
 *    increments `seq` to even. Each step is lock-free; the writer
 *    never waits.
 *  - Readers must call atomic_get_snapshot() — never read status / lag_ms
 *    / lag_bytes directly during routing decisions. Direct atomic_get_*
 *    accessors remain available for places that only need one field
 *    (e.g. /hosts and /status JSON rendering).
 */
typedef struct {
  char *host;                       // immutable after init
  char *connection_str;             // immutable after init
  _Atomic uint64_t seq;             // seqlock: odd = writing, even = stable
  _Atomic uint64_t lag_ms;          // protected by seq
  _Atomic uint64_t lag_bytes;       // protected by seq
  unsigned int failed_connections;  // writer-private
  _Atomic MonitorStatus status;     // protected by seq
} MonitorHost;

/**
 * Consistent snapshot of a host's state
 */
typedef struct {
  MonitorStatus status;
  uint64_t lag_ms;
  uint64_t lag_bytes;
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
 */
typedef struct {
  uint64_t max_lag_ms;
  uint64_t max_lag_bytes;
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
 * Updates the host status
 */
void check_host_streaming_replication(
  MonitorHost *host, unsigned int max_fails
);

#endif  // PG_STATUS_PG_MONITOR_H
