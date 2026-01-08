#include "pg_monitor.h"
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
atomic_bool pg_monitor_ready = false;

/**
 * A pointer to the head of the linked list of hosts
 */
MonitorHost *monitor_host_head = nullptr;

/**
 * Just a master host to find it asap
 */
_Atomic (char *) master_host = nullptr;

/**
 * A pointer to the last host returned in the round-robin algorithm
 */
_Atomic (MonitorHost *) round_robin_cursor = nullptr;

/**
 * pg-monitor parameters. The default parameters are set here.
 */
MonitorParameters parameters = {
    .user = "postgres",
    .password = "postgres",
    .database = "postgres",
    .hosts_delimiter = ",",
    .hosts = nullptr,
    .port = "5432",
    .connect_timeout = "2",
    .sleep = 5,
    .max_fails = 3,
    .sync_max_lag_ms = 1000,
    .sync_max_lag_bytes = 1000000,  // 1 mb
};


/**
 * A sign that all data have been initialized and pg_status is ready
 * to provide information about hosts.
 */
bool is_pg_monitor_ready(void) {
    return atomic_load_explicit(
        &pg_monitor_ready, memory_order_acquire
    );
}

/**
 * Returns a pointer to the head of the linked list of hosts.
 */
MonitorHost *get_monitor_host_head(void) {
    return monitor_host_head;
}

/**
 * Overrides default parameters if they are set in environment variables.
 */
void get_values_from_env(void) {
    replace_from_env("pg_status__pg_user", &parameters.user);
    replace_from_env("pg_status__pg_database", &parameters.database);
    replace_from_env("pg_status__pg_password", &parameters.password);
    replace_from_env("pg_status__delimiter", &parameters.hosts_delimiter);
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
    if (parameters.hosts == nullptr) {
        raise_error("pg_status__hosts not set");
    }
}

/**
 * Returns hosts sequentially. When no more hosts are available,
 * it returns nullptr.
 */
char *next_host(char *hosts) {
    static char *save_ptr = nullptr;
    static char *host = nullptr;

    if (host == nullptr) {
        host = strtok_r(hosts, parameters.hosts_delimiter, &save_ptr);
    }
    else {
        host = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);
    }
    return host;
}

/**
 * Returns ports sequentially. When no more ports are available,
 * it continues returning the last one.
 * This allows a single port to be used for all hosts.
 */
char *next_port(char *ports) {
    static char *last_port = nullptr;
    static char *save_ptr = nullptr;
    static char *port = nullptr;

    if (port == nullptr) {
        port = strtok_r(ports, parameters.hosts_delimiter, &save_ptr);
    }
    else {
        port = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);
    }

    if (port != nullptr) {
        last_port = port;
    }
    return last_port;
}

/**
 * Returns the string to connect to pg
 */
char *get_connection_string(char *host, char *port) {
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
MonitorStatus *init_monitor_status(void) {
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
MonitorHost *init_monitor_host(char *host, char *port) {
    MonitorHost *monitor_host = calloc(1, sizeof(MonitorHost));
    if (!monitor_host) {
        raise_error("Failed to allocate MonitorHost");
    }
    monitor_host -> host = strdup(host);
    monitor_host -> connection_str = get_connection_string(host, port);
    monitor_host -> next = nullptr;
    monitor_host -> failed_connections = 0;

    atomic_store_explicit(
        &monitor_host -> status,
        init_monitor_status(),
        memory_order_release
    );

    atomic_store_explicit(
        &monitor_host -> not_actual_status,
        init_monitor_status(),
        memory_order_release
    );

    return monitor_host;
}

/**
 * Initializes MonitorHost linked list to its initial value.
 */
void init_monitor_host_linked_list(void) {
    char *hosts = strdup(parameters.hosts);
    char *host = next_host(hosts);

    char *ports = strdup(parameters.port);
    char *port = next_port(ports);

    monitor_host_head = init_monitor_host(host, port);
    MonitorHost *cursor = monitor_host_head;
    host = next_host(hosts);
    port = next_port(ports);
    unsigned int cnt = 1;

    while (host) {
        if (cnt == MAX_HOSTS) {
            raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);
        }

        cursor -> next = init_monitor_host(host, port);
        cursor = cursor -> next;

        host = next_host(hosts);
        port = next_port(ports);
        cnt++;
    }

    free(hosts);
    free(ports);
}


char *get_master_host(void) {
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
MonitorHost *find_host_by_name(const char *host) {
    MonitorHost *cursor = monitor_host_head;

    while (cursor) {
        if (is_equal_strings(cursor -> host, host)) {
            return cursor;
        }
        cursor = cursor -> next;
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
MonitorHost *get_next_host_in_circle(MonitorHost *cursor) {
    if (!cursor || !cursor -> next) {
        return get_monitor_host_head();
    }
    return cursor -> next;
}

/**
 * Moves the cursor of the round-robin algorithm and returns the host from
 * which to start the crawl
 */
MonitorHost *get_next_host_round_robin(void) {
    MonitorHost *cursor = atomic_load_explicit(
        &round_robin_cursor, memory_order_acquire
    );
    cursor = get_next_host_in_circle(cursor);
    atomic_store_explicit(&round_robin_cursor, cursor, memory_order_release);
    return cursor;
}

/**
 * A function for searching for a host that matches certain conditions using the round-robin algorithm.
 * @param handler A function that determines whether the specified host has been found
 * @param master_if_not_found Determines whether to return the master if the desired host is not found by handler
 * @return Host name corresponding to conditions
 */
char *find_host_round_robin(
    const condition_handler handler, const bool master_if_not_found
) {
    MonitorHost *cursor = get_next_host_round_robin();
    const MonitorHost *start = cursor;

    MonitorStatus status = atomic_get_status(cursor);

    while (!handler(&status)) {
        cursor = get_next_host_in_circle(cursor);
        status = atomic_get_status(cursor);

        if (cursor == start) {
            if (master_if_not_found) {
                return get_master_host();
            }
            return nullptr;
        }

    }

    return cursor -> host;
}

/**
 * One iteration of host checking
 */
void check_hosts(void) {
    MonitorHost *cursor = monitor_host_head;

    char *master = nullptr;

    while (cursor) {
        check_host_streaming_replication(cursor, parameters.max_fails);

        if (!master) {
            MonitorStatus status = atomic_get_status(cursor);
            if (is_master(&status)) {
                master = cursor -> host;
            }
        }

        cursor = cursor -> next;
    }
    save_master_host(master);
    (void)fflush(stdout);
}

/**
 * The main monitoring thread, which runs continuously and periodically
 * does host checks
 */
void *pg_monitor_thread(void *arg) {
    get_values_from_env();
    init_monitor_host_linked_list();

    check_hosts();
    atomic_store_explicit(&pg_monitor_ready, true, memory_order_release);

    struct timespec ts;
    pthread_mutex_lock(&monitor_mutex);
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
    const int started = pthread_create(
        &monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0) {
        raise_error("Failed to start pg_monitor");
    }

    while (!is_pg_monitor_ready()) {
        sleep(1);
    }
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
