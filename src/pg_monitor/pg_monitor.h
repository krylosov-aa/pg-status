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
# define MAX_HOSTS 10

/**
 * List of all monitoring parameters
 */
typedef struct {
    //pg user
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

    // Time between checks
    unsigned int sleep;

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
extern uint8_t host_count;

/**
 * Just a master host to find it asap
 */
extern _Atomic (char *) master_host;

/**
 * Host status. Separated into a structure for atomic access.
 */
typedef struct {
    unsigned long long delay_ms;
    unsigned long long delay_bytes;
    bool master;
    bool alive;
} MonitorStatus;

/**
 *  Host parameters, including a double buffer (status and not_actual_status)
 *  that is atomically replaced during the next iteration of
 *  host status checking.
 *  Hosts form a linked list.
 */
typedef struct {
    char *host;
    char *connection_str;
    _Atomic(MonitorStatus *) status;
    _Atomic(MonitorStatus *) not_actual_status;
    unsigned int failed_connections;
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
void save_master_host(char *host);


// ------------------------ Lookup utils ------------------------


/**
 * Atomically returns a pointer to the host status
 */
MonitorStatus atomic_get_status(const MonitorHost *host);

/**
 * Describes the interface of the function for searching hosts
 */
typedef bool (*condition_handler)(const MonitorStatus *);

/**
 * A function for searching for a host that matches certain conditions using the round-robin algorithm.
 * @param handler A function that determines whether the specified host has been found
 * @param master_if_not_found Determines whether to return the master if the desired host is not found by handler
 * @return Host name corresponding to conditions
 */
const char *find_host_round_robin(
    condition_handler handler, bool master_if_not_found
);

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host);

/**
 * condition_handler that searches for a live master
 */
bool is_master(const MonitorStatus *status);

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(const MonitorStatus *status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(const MonitorStatus *status);

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(const MonitorStatus *status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(const MonitorStatus *status);

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(const MonitorStatus *status);


// ------------------------ Host checking utils ------------------------


/**
 * Updates the host status
 */
void check_host_streaming_replication(
    MonitorHost *host, unsigned int max_fails
);

/**
 * Atomic acquisition of the current master
 */
const char *get_master_host(void);

#endif //PG_STATUS_PG_MONITOR_H
