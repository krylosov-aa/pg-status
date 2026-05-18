/**
 * Utilities for initializing and writing an array of monitoring hosts
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "pg_monitor.h"
#include "utils.h"

/**
 * The actual number of hosts
 */
unsigned int host_count = 0;

/**
 * Array of monitoring hosts
 */
MonitorHost monitor_host_list[MAX_HOSTS] = {0};

/**
 * Just a master host index in the array to find it asap
 */
atomic_int master_index = -1;

/**
 * Atomically gets the current master position in the array
 */
int get_master_index(void) {
  return atomic_load_explicit(&master_index, memory_order_relaxed);
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
  } else {
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
  } else {
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
    parameters.user, parameters.password, host, port, parameters.database,
    parameters.connect_timeout
  );
}

/**
 * Initializes MonitorHost to its initial value.
 */
static void init_monitor_host(
  MonitorHost *monitor_host, char *host, char *port
) {
  monitor_host->host = copy_string(host);
  monitor_host->connection_str = get_connection_string(host, port);
  monitor_host->failed_connections = 0;
  monitor_host->conn = nullptr;
  monitor_host->checks_since_reconnect = 0;

  atomic_store_explicit(&monitor_host->seq, 0, memory_order_relaxed);
  atomic_store_explicit(
    &monitor_host->status,
    (MonitorStatus){.alive = false, .master = false, .possible_dead = false},
    memory_order_relaxed
  );
  atomic_store_explicit(&monitor_host->lag_ms, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lag_bytes, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lsn, 0, memory_order_relaxed);
}

/**
 * Initializes the MonitorHost array to its initial values.
 */
void init_monitor_host_list(void) {
  char *hosts = copy_string(parameters.hosts);
  char *host = next_host(hosts);

  char *ports = copy_string(parameters.port);
  char *port = next_port(ports);

  while (host) {
    if (host_count == MAX_HOSTS) {
      raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);
    }
    init_monitor_host(&monitor_host_list[host_count], host, port);

    host = next_host(hosts);
    port = next_port(ports);
    host_count++;
  }
  if (host_count == 0) {
    raise_error("host count must be greater then 0");
  }
  free(hosts);
  free(ports);
}

/**
 * Atomically saves the current master's host
 */
void save_master_index(const int i) {
  atomic_store_explicit(&master_index, i, memory_order_relaxed);
}
