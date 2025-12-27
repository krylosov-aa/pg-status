#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


atomic_uint pg_monitor_running = 1;

MonitorHost *monitor_host_head = nullptr;

_Atomic (MonitorHost *) last_random_replica = nullptr;

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

void get_values_from_env(void) {
    replace_from_env("pg_status__pg_user", &parameters.user);
    replace_from_env("pg_status__pg_database", &parameters.database);
    replace_from_env("pg_status__pg_password", &parameters.password);
    replace_from_env("pg_status__delimiter", &parameters.hosts_delimiter);
    replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);
    replace_from_env("pg_status__port", &parameters.port);
    replace_from_env_uint("pg_status__sleep", &parameters.sleep);
    replace_from_env_uint("pg_status__max_fails", &parameters.max_fails);
    replace_from_env_ull(
        "pg_status__sync_max_lag_ms", &parameters.sync_max_lag_ms
    );
    replace_from_env_ull(
        "pg_status__sync_max_lag_bytes", &parameters.sync_max_lag_bytes
    );

    replace_from_env("pg_status__hosts", &parameters.hosts);
    if (parameters.hosts == nullptr)
        raise_error("pg_status__hosts not set");
}

char *next_host(char *hosts) {
    static char *save_ptr = nullptr;
    static char *host = nullptr;

    if (host == nullptr)
        host = strtok_r(hosts, parameters.hosts_delimiter, &save_ptr);
    else
        host = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);
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

    if (port == nullptr)
        port = strtok_r(ports, parameters.hosts_delimiter, &save_ptr);
    else
        port = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);

    if (port != nullptr)
        last_port = port;
    return last_port;
}

char *get_connection_string(char *host, char *port) {
    return format_string(
        "user=%s password=%s host=%s port=%s "
        "dbname=%s connect_timeout=%s",
        parameters.user, parameters.password, host, port,
        parameters.database, parameters.connect_timeout
    );
}

MonitorStatus *init_monitor_status(void) {
    MonitorStatus *status = malloc(sizeof(MonitorStatus));
    status -> delay_ms = 0;
    status -> delay_bytes = 0;
    status -> is_master = false;
    status -> alive = false;
    return status;
}

MonitorHost *init_monitor_host(char *host, char *port) {
    MonitorHost *monitor_host = calloc(1, sizeof(MonitorHost));
    monitor_host -> host = copy_string(host);
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

void init_monitor_host_linked_list(void) {
    char *hosts = copy_string(parameters.hosts);
    char *host = next_host(hosts);

    char *ports = copy_string(parameters.port);
    char *port = next_port(ports);

    monitor_host_head = init_monitor_host(host, port);
    MonitorHost *cursor = monitor_host_head;
    host = next_host(hosts);
    port = next_port(ports);
    unsigned int cnt = 1;

    while (host) {
        if (cnt == MAX_HOSTS)
            raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);

        cursor -> next = init_monitor_host(host, port);
        cursor = cursor -> next;

        host = next_host(hosts);
        port = next_port(ports);
        cnt++;
    }

    free(hosts);
    free(ports);
}

MonitorStatus *atomic_get_status(const MonitorHost *host) {
    return atomic_load_explicit(
        &host -> status, memory_order_acquire
    );
}

void check_hosts(void) {
    MonitorHost *cursor = monitor_host_head;

    while (cursor) {
        check_host_streaming_replication(cursor, parameters.max_fails);
        cursor = cursor -> next;
    }
    printf("\n");
}

char *get_master_host(void) {
    MonitorHost *cursor = monitor_host_head;
    while (cursor) {
        if (atomic_get_status(cursor) -> is_master)
            return cursor -> host;
        cursor = cursor -> next;
    }
    return "null";
}

MonitorHost *get_monitor_host_head(void) {
    return monitor_host_head;
}

bool is_alive_replica(const MonitorHost *host) {
    const MonitorStatus *status = atomic_get_status(host);
    return status -> alive && !status -> is_master;
}

char *round_robin_replica(void) {
    MonitorHost *cursor = atomic_load_explicit(
        &last_random_replica, memory_order_acquire
    );
    if (!cursor || !cursor -> next)
        cursor = get_monitor_host_head();
    else
        cursor = cursor -> next;

    const MonitorHost *start = cursor;
    while (!is_alive_replica(cursor)) {
        cursor = cursor -> next;
        if (!cursor)
            cursor = get_monitor_host_head();

        if (cursor == start)
            return get_master_host();
    }
    atomic_store_explicit(&last_random_replica, cursor, memory_order_release);

    return cursor -> host;
}

/**
 * Searches for a synchronous replica.
 * If there isn't one, it returns the master if he's alive.
 */
char *sync_host_by_time(void) {
    const MonitorHost *cursor = monitor_host_head;
    while (cursor) {
        const MonitorStatus *status = atomic_get_status(cursor);
        if (
            status -> alive &&
            !status -> is_master &&
            status -> delay_ms <= parameters.sync_max_lag_ms
        )
            return cursor -> host;
        cursor = cursor -> next;
    }
    return get_master_host();
}

/**
 * Searches for a synchronous replica.
 * If there isn't one, it returns the master if he's alive.
 */
char *sync_host_by_bytes(void) {
    const MonitorHost *cursor = monitor_host_head;
    while (cursor) {
        const MonitorStatus *status = atomic_get_status(cursor);
        if (
            status -> alive &&
            !status -> is_master &&
            status -> delay_bytes <= parameters.sync_max_lag_bytes
        )
            return cursor -> host;
        cursor = cursor -> next;
    }
    return get_master_host();
}

void stop_pg_monitor(void) {
    atomic_store(&pg_monitor_running, true);
    printf("pg_monitor stopped\n");
}

void *pg_monitor_thread(void *arg) {
    (void)arg;

    get_values_from_env();
    init_monitor_host_linked_list();

    while (atomic_load(&pg_monitor_running)) {
        check_hosts();
        sleep(parameters.sleep);
    }
    return nullptr;
}

pthread_t start_pg_monitor() {
    pthread_t pg_monitor_tid;
    const int started = pthread_create(
        &pg_monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0)
        raise_error("Failed to start pg_monitor");

    printf("pg_monitor started\n");
    return pg_monitor_tid;
}
