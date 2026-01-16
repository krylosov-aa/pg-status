/**
 * Utilities for initializing and writing an array of monitoring hosts
 */

#include "pg_monitor.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>


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
_Atomic (char *) master_host = nullptr;


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
void init_monitor_host_list(void) {
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

/**
 * Atomically saves the current master's host
 */
void save_master_host(char *host) {
    atomic_store_explicit(
        &master_host, host, memory_order_release
    );
}
