#include "pg_monitor.h"

#include <assert.h>

#include "utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


/**
 * Parameters for stopping a thread
 */
static pthread_mutex_t monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  monitor_cond  = PTHREAD_COND_INITIALIZER;
static bool monitor_running = true;
static pthread_t monitor_tid;

/**
 * A sign that all data have been initialized and pg_status is ready
 * to provide information about hosts.
 */
static bool pg_monitor_ready = false;

/**
 * The actual number of hosts
 */
uint8_t host_count = 0;

/**
 * Array of monitoring hosts
 */
MonitorHost monitor_host_list[MAX_HOSTS] = {0};

/**
 * Just a master host to find it asap
 */
static _Atomic (char *) master_host = nullptr;

/**
 * A pointer to the last host returned in the round-robin algorithm
 */
static atomic_uint_least8_t round_robin_cursor = 0;

/**
 * pg-monitor parameters. The default parameters are set here.
 */
MonitorParameters parameters = {
    .user = "postgres",
    .password = "postgres",
    .database = "postgres",
    .hosts = nullptr,
    .port = "5432",
    .connect_timeout = "2",
    .sleep = 5,
    .max_fails = 3,
    .sync_max_lag_ms = 1000,
    .sync_max_lag_bytes = 1000000,  // 1 mb
};

/**
 * Overrides default parameters if they are set in environment variables.
 */
static void get_values_from_env(void) {
    replace_from_env("pg_status__pg_user", &parameters.user);
    replace_from_env("pg_status__pg_database", &parameters.database);
    replace_from_env("pg_status__pg_password", &parameters.password);
    replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);
    replace_from_env("pg_status__pg_port", &parameters.port);
    replace_from_env_uint("pg_status__sleep", &parameters.sleep);
    replace_from_env_uint("pg_status__max_fails", &parameters.max_fails);
    replace_from_env_ull(
        "pg_status__sync_max_lag_ms", &parameters.sync_max_lag_ms
    );
    replace_from_env_ull(
        "pg_status__sync_max_lag_bytes", &parameters.sync_max_lag_bytes
    );

    replace_from_env("pg_status__hosts", &parameters.hosts);
    if (!parameters.hosts || !*parameters.hosts) {
        raise_error("pg_status__hosts not set");
    }
}

/**
 * Returns hosts sequentially. When no more hosts are available,
 * it returns nullptr.
 */
static char *next_host(char *hosts) {
    static char *save_ptr = nullptr;
    static char *host = nullptr;

    if (host == nullptr) {
        host = strtok_r(hosts, ",", &save_ptr);
    }
    else {
        host = strtok_r(nullptr, ",", &save_ptr);
    }
    return host;
}

/**
 * Returns ports sequentially. When no more ports are available,
 * it continues returning the last one.
 * This allows a single port to be used for all hosts.
 */
static char *next_port(char *ports) {
    static char *last_port = nullptr;
    static char *save_ptr = nullptr;
    static char *port = nullptr;

    if (port == nullptr) {
        port = strtok_r(ports, ",", &save_ptr);
    }
    else {
        port = strtok_r(nullptr, ",", &save_ptr);
    }

    if (port != nullptr) {
        last_port = port;
    }
    return last_port;
}

/**
 * Returns the string to connect to pg
 */
static char *get_connection_string(char *host, char *port) {
    return format_string(
        "user=%s password=%s host=%s port=%s "
        "dbname=%s connect_timeout=%s",
        parameters.user, parameters.password, host, port,
        parameters.database, parameters.connect_timeout
    );
}

/**
 * Initializes MonitorStatus to its initial value.
 */
static MonitorStatus *init_monitor_status(void) {
    MonitorStatus *status = malloc(sizeof(MonitorStatus));
    if (!status) {
        raise_error("Failed to allocate MonitorStatus");
    }
    status -> delay_ms = 0;
    status -> delay_bytes = 0;
    status -> master = false;
    status -> alive = false;
    return status;
}

/**
 * Initializes MonitorHost to its initial value.
 */
static MonitorHost init_monitor_host(char *host, char *port) {
    MonitorHost monitor_host;
    monitor_host.host = strdup(host);
    monitor_host.connection_str = get_connection_string(host, port);
    monitor_host.failed_connections = 0;

    atomic_store_explicit(
        &monitor_host.status,
        init_monitor_status(),
        memory_order_release
    );

    atomic_store_explicit(
        &monitor_host.not_actual_status,
        init_monitor_status(),
        memory_order_release
    );

    return monitor_host;
}

/**
 * Initializes MonitorHost linked list to its initial value.
 */
static void init_monitor_host_list(void) {
    char *hosts = strdup(parameters.hosts);
    char *host = next_host(hosts);

    char *ports = strdup(parameters.port);
    char *port = next_port(ports);

    while (host) {
        if (host_count == MAX_HOSTS) {
            raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);
        }
        monitor_host_list[host_count] = init_monitor_host(host, port);

        host = next_host(hosts);
        port = next_port(ports);
        host_count++;
    }

    free(hosts);
    free(ports);
}


const char *get_master_host(void) {
    return atomic_load_explicit(
        &master_host, memory_order_acquire
    );
}

void save_master_host(char *host) {
    atomic_store_explicit(
        &master_host, host, memory_order_release
    );
}

/**
 * Atomically returns MonitorStatus.
 *
 * To avoid a very unlikely UB when the reader gets stuck with a pointer that
 * the second iteration of the loop is already underway and the structure is
 * starting to update, the reader is working with a copy on the stack.
 */
MonitorStatus atomic_get_status(const MonitorHost *host) {
    const MonitorStatus *ptr = atomic_load_explicit(
        &host -> status, memory_order_acquire
    );
    return *ptr;
}

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host) {
    for (uint8_t i = 0; i < host_count; i++) {
        MonitorHost *item = &monitor_host_list[i];
        if (is_equal_strings(item -> host, host)) {
            return item;
        }
    }
    return nullptr;
}

/**
 * condition_handler that searches for a live master
 */
bool is_master(const MonitorStatus *status) {
    return status -> alive && status -> master;
}

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(const MonitorStatus *status) {
    return status -> alive && !status -> master;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(const MonitorStatus *status) {
    return (
        is_alive_replica(status) &&
        status -> delay_ms <= parameters.sync_max_lag_ms
    );
}

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(const MonitorStatus *status) {
    return (
        is_alive_replica(status) &&
        status -> delay_bytes <= parameters.sync_max_lag_bytes
    );
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(const MonitorStatus *status) {
    return (
        is_sync_replica_by_time(status) ||
        is_sync_replica_by_bytes(status)
    );
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(const MonitorStatus *status) {
    return (
        is_sync_replica_by_time(status) &&
        is_sync_replica_by_bytes(status)
    );
}

/**
 * It takes the next host from the list, and if it's over,
 * it starts from the beginning.
 */
static uint8_t get_next_cursor_in_circle(const uint8_t cursor) {
    assert(cursor <= host_count);
    return (cursor + 1) % host_count;
}

/**
 * Moves the cursor of the round-robin algorithm and returns the host from
 * which to start the crawl
 */
static uint8_t get_next_cursor_round_robin(void) {
    return atomic_fetch_add_explicit(
        &round_robin_cursor, 1, memory_order_relaxed
    ) % host_count;
}

/**
 * A function for searching for a host that matches certain conditions using the round-robin algorithm.
 * @param handler A function that determines whether the specified host has been found
 * @param master_if_not_found Determines whether to return the master if the desired host is not found by handler
 * @return Host name corresponding to conditions
 */
const char *find_host_round_robin(
    const condition_handler handler, const bool master_if_not_found
) {
    uint8_t cursor = get_next_cursor_round_robin();
    const uint8_t start_cursor = cursor;

    const MonitorHost *mon_host = &monitor_host_list[cursor];
    MonitorStatus status = atomic_get_status(mon_host);

    while (!handler(&status)) {
        cursor = get_next_cursor_in_circle(cursor);
        mon_host = &monitor_host_list[cursor];
        status = atomic_get_status(mon_host);

        if (cursor == start_cursor) {
            if (master_if_not_found) {
                return get_master_host();
            }
            return nullptr;
        }

    }

    return mon_host -> host;
}

/**
 * One iteration of host checking
 */
static void check_hosts(void) {
    char *master = nullptr;
    for (uint8_t i = 0; i < host_count; i++) {
        MonitorHost *item = &monitor_host_list[i];
        check_host_streaming_replication(item, parameters.max_fails);

        if (!master) {
            MonitorStatus status = atomic_get_status(item);
            if (is_master(&status)) {
                master = item -> host;
            }
        }

    }
    save_master_host(master);
    (void)fflush(stdout);
}

/**
 * The main monitoring thread, which runs continuously and periodically
 * does host checks
 */
static void *pg_monitor_thread(void *arg) {
    get_values_from_env();
    init_monitor_host_list();

    check_hosts();

    struct timespec ts;
    pthread_mutex_lock(&monitor_mutex);
    pg_monitor_ready = true;
    pthread_cond_broadcast(&monitor_cond);

    while (monitor_running) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += parameters.sleep;

        pthread_cond_timedwait(&monitor_cond, &monitor_mutex, &ts);
        if (!monitor_running) {
            break;
        };

        check_hosts();
    }
    pthread_mutex_unlock(&monitor_mutex);
    return nullptr;
}

/**
 * Starts a host monitoring thread
 */
pthread_t start_pg_monitor() {
    pthread_mutex_lock(&monitor_mutex);
    const int started = pthread_create(
        &monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0) {
        pthread_mutex_unlock(&monitor_mutex);
        raise_error("Failed to start pg_monitor");
    }

    while (!pg_monitor_ready) {
        pthread_cond_wait(&monitor_cond, &monitor_mutex);
    }
    pthread_mutex_unlock(&monitor_mutex);
    printf("pg_monitor started\n");
    return monitor_tid;
}

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void) {
    pthread_mutex_lock(&monitor_mutex);
    monitor_running = false;
    pthread_cond_signal(&monitor_cond);
    pthread_mutex_unlock(&monitor_mutex);

    pthread_join(monitor_tid, nullptr);
    printf("pg_monitor stopped\n");
}
