#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

#include <pthread.h>

/**
 * The maximum number of hosts monitored by pg-status
 */
# define MAX_HOSTS 10

/**
 * Starts a host monitoring thread
 */
pthread_t start_pg_monitor();

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void);

/**
 * List of all monitoring parameters
 */
typedef struct MonitorParameters {
    //pg user
    char *user;

    // pg password
    char *password;

    // pg database name
    char *database;

    // delimiter. For example: ,
    char *hosts_delimiter;

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
 * Host status. Separated into a structure for atomic access.
 */
typedef struct MonitorStatus {
    unsigned long long delay_ms;
    unsigned long long delay_bytes;
    bool is_master;
    bool alive;
} MonitorStatus;


/**
 *  Host parameters, including a double buffer (status and not_actual_status)
 *  that is atomically replaced during the next iteration of
 *  host status checking.
 *  Hosts form a linked list.
 */
typedef struct MonitorHost {
    char *host;
    char *connection_str;
    struct MonitorHost *next;
    _Atomic(MonitorStatus *) status;
    _Atomic(MonitorStatus *) not_actual_status;
    unsigned int failed_connections;
} MonitorHost;


/**
 * Returns a pointer to the head of the linked list of hosts.
 */
MonitorHost *get_monitor_host_head(void);


/**
 * Atomically returns a pointer to the host status
 */
MonitorStatus *atomic_get_status(const MonitorHost *host);


/**
 * Returns a random replica using the round-robin algorithm.
 * If there are no live replicas, it returns the master.
 */
char *round_robin_replica(void);

/**
 * Describes the interface of the function for searching hosts
 */
typedef bool (*condition_handler)(const MonitorStatus *);

/**
 * A function for searching for a host that matches certain conditions
 * @param handler A function that determines whether the specified host has been found
 * @param master_if_not_found Determines whether to return the master if the desired host is not found by handler
 * @return Host name corresponding to conditions
 */
char *find_host(
    const condition_handler handler, const bool master_if_not_found
);

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


/**
 * Updates the host status
 */
void check_host_streaming_replication(
    MonitorHost *host, const unsigned int max_fails
);

#endif //PG_STATUS_PG_MONITOR_H
