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

  // pg hosts, separated by hosts_delimiter.
  char *hosts;

  // pg Port. You can specify multiple ports, separated by hosts_delimiter
  char *port;

  // Time to attempt connection to host
  char *connect_timeout;

  // The lag in ms below which a replica is considered synchronous
  unsigned long long sync_max_lag_ms;

  // The lag in bytes below which a replica is considered synchronous
  unsigned long long sync_max_lag_bytes;

  // Time between checks in ms
  int sleep_ms;

  // After this number of falls, the host is considered dead.
  unsigned int max_fails;
} MonitorParameters;

/**
 * pg-monitor parameters. The default parameters are set here.
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
 * Host status. Separated into a structure for atomic access.
 *
 * This way, concurrent threads can read the current status lock-free,
 * and the writer can perform lock-free writes.
 *
 * But the downside of this solution is the inconsistency: if the writer
 * has started updating the hosts but hasn't finished updating all of
 * them, some hosts may have the new data while others still have the old data.
 */
typedef struct {
  bool sync_by_time : 1;
  bool sync_by_bytes : 1;
  bool master : 1;
  bool alive : 1;
  bool possible_dead : 1;
} MonitorStatus;

/**
 *  Host parameters
 */
typedef struct {
  char *host;
  char *connection_str;
  unsigned int failed_connections;
  _Atomic MonitorStatus status;
  _Atomic uint64_t lag_ms;
  _Atomic uint64_t lag_bytes;
} MonitorHost;

/**
 * Array of monitoring hosts
 */
extern MonitorHost monitor_host_list[MAX_HOSTS];

/**
 * Initializes MonitorHost linked list to its initial value.
 */
void init_monitor_host_list(void);

/**
 * Atomically saves the current master's host
 */
void save_master_index(int i);

// ------------------------ Lookup utils ------------------------

/**
 * Atomic acquisition of the current master
 */
const char *get_master_host(void);

/**
 * Atomically returns a pointer to the host status
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
 * Describes the interface of the function for searching hosts
 */
typedef bool (*condition_handler)(MonitorStatus);

/**
 * A function for searching for a host that matches certain conditions using the
 * round-robin algorithm.
 * @param handler A function that determines whether the specified host has been
 * found
 * @param log_context The context that will be visible in the logs
 * @return Host name corresponding to conditions
 */
const char *find_replica_round_robin(
  condition_handler handler, const char *log_context
);

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host);

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(MonitorStatus status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(MonitorStatus status);

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(MonitorStatus status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(MonitorStatus status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(MonitorStatus status);

// ------------------------ Host checking utils ------------------------

/**
 * Updates the host status
 */
void check_host_streaming_replication(
  MonitorHost *host, unsigned int max_fails
);

#endif  // PG_STATUS_PG_MONITOR_H
