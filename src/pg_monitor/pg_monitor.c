#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


MonitorStatus *monitor_status = nullptr;

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

void set_bool_atomic(atomic_bool *ptr, const bool val) {
    atomic_store_explicit(ptr, val, memory_order_release);
}

bool get_bool_atomic(const atomic_bool *ptr) {
    return atomic_load_explicit(ptr, memory_order_acquire);
}

char *get_master_host(void) {
    MonitorStatus *cursor = monitor_status;
    while (cursor) {
        if (get_bool_atomic(&cursor -> is_master))
            return cursor -> host;
        cursor = cursor -> next;
    }
    return "null";
}

MonitorStatus *get_monitor_status(void) {
    return monitor_status;
}

bool is_alive_replica(const MonitorStatus *host) {
    return (
        get_bool_atomic(&host -> alive) &&
        !get_bool_atomic(&host -> is_master)
    );
}

_Atomic (MonitorStatus *) last_random_replica = nullptr;

char *round_robin_replica(void) {
    MonitorStatus *cursor = atomic_load_explicit(
        &last_random_replica, memory_order_acquire
    );
    if (!cursor || !cursor -> next)
        cursor = get_monitor_status();
    else
        cursor = cursor -> next;

    unsigned int i = 0;
    while (!is_alive_replica(cursor)) {
        cursor = cursor -> next;
        if (!cursor)
            cursor = get_monitor_status();
        i++;
        if (i == MAX_HOSTS)
            break;
    }
    atomic_store_explicit(&last_random_replica, cursor, memory_order_release);

    return i < MAX_HOSTS ? cursor -> host: "null";
}

void init_monitor_status(void) {
    get_values_from_env();

    char *hosts = copy_string(parameters.hosts);
    char *host = next_host(hosts);

    char *ports = copy_string(parameters.port);
    char *port = next_port(ports);

    monitor_status = calloc(1, sizeof(MonitorStatus));
    MonitorStatus *cursor = monitor_status;
    unsigned int cnt = 0;

    while (host != nullptr) {
        if (cnt == MAX_HOSTS)
            raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);
        if (cnt > 0) {
            cursor -> next = calloc(1, sizeof(MonitorStatus));
            cursor = cursor -> next;
        }

        cursor -> host = copy_string(host);
        cursor -> connection_str = get_connection_string(host, port);
        cursor -> failed_connections = 0;
        set_bool_atomic(&cursor -> is_master, false);
        set_bool_atomic(&cursor -> alive, false);

        host = next_host(hosts);
        port = next_port(ports);
        cnt++;
    }

    free(hosts);
    free(ports);
}

void check_hosts(void) {
    MonitorStatus *cursor = monitor_status;

    while (cursor) {
        bool is_replica = true;
        const int exit_val = is_host_in_recovery(
            cursor -> connection_str,
            &is_replica
        );

        if (exit_val == 0) {
            cursor -> failed_connections = 0;
            set_bool_atomic(&cursor -> alive, true);

            if (is_replica) {
                printf("%s: replica\n", cursor -> host);
                set_bool_atomic(&cursor -> is_master, false);
            }
            else {
                printf("%s: master\n", cursor -> host);
                set_bool_atomic(&cursor -> is_master, true);
            }
        }
        else {
            printf("%s: dead\n", cursor -> host);
            if (cursor -> failed_connections >= parameters.max_fails) {
                set_bool_atomic(&cursor -> alive, false);
                set_bool_atomic(&cursor -> is_master, false);
            }
            else
                cursor -> failed_connections++;
        }

        cursor = cursor -> next;
    }
    printf("\n");
}


atomic_uint pg_monitor_running = 1;

void stop_pg_monitor(void) {
    atomic_store(&pg_monitor_running, true);
    printf("pg_monitor stopped\n");
}

void *pg_monitor_thread(void *arg) {
    (void)arg;

    init_monitor_status();

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
